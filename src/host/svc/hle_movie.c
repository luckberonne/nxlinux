// The SDK movie library's decoder (subsdk0: movie::Decoder over NVIDIA's hardware decoder) cannot run here: the host has no nvdrv, no nvdec and no mm:u.
// Left alone, its decoder thread dereferences a null session when the device opens fail, and a title that waits for frames waits forever (Mario Kart 8 Deluxe's
// menus play content:/UI/cmn/movie/MovieMenu.mp4 behind them). The library's extractor is software (Android's MPEG4 / WebM extractors) and is kept.
//
// Takeover: the movie::Decoder methods (each a forwarder to its DecoderImpl at +8) are the host's. The constructor leaves the impl pointer null, so the
// library's own destructors only free the object. The host keeps a decoder per object: input buffers in guest memory, FFmpeg (when built with it) decodes
// what the title sends, and the frames wait in a ready list for AcquireOutputBufferIndex, as MediaCodec does. Without FFmpeg the frames are black.
// The four events of movie::DecoderEvents (input available, output available, format changed, error) are signalled through the guest's nn::os::SignalEvent.
#include "hle.h"
#include "cpu.h"
#include "kernel.h"
#include "guest_mem.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef NX_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#endif

extern uint64_t module_find(const char *);

enum { ST_OK = 0, ST_BAD_ARG = 0x100b, ST_NO_IMPL = 0x100c, ST_NO_BUFFER = 0x1011 };   // movie::Status values the library returns
enum { FMT_ARGB = 2, FMT_NV12 = 4 };                                                    // movie::DecoderOutputFormat values it converts to
enum { NIN = 4, NOUT = 8, IN_CAP = 2u << 20 };
enum { EV_INPUT, EV_OUTPUT, EV_FORMAT, EV_ERROR };

// movie::Buffer: +0 offset, +4 size, +0x10 owns, +0x18 capacity, +0x28 base
typedef struct { int32_t offset, size, i32; uint32_t u32; uint8_t owns, pad[7]; uint64_t capacity; int64_t i64; uint64_t base; } mbuf;

typedef struct dec {
    uint64_t self;                     // the guest movie::Decoder
    int type, fmt, mode;
    uint64_t events;                   // movie::DecoderEvents*: four nn::os::EventType*
    int w, h, started;
    uint64_t in_mem[NIN]; int in_free[NIN], nin_free;
    struct { uint64_t mem; size_t cap, size; int64_t pts; uint32_t flags; int busy; } out[NOUT];
    int ready[NOUT], nready;
    int fmt_sent, held;
#ifdef NX_HAVE_FFMPEG
    AVCodecContext *av; AVPacket *pkt; AVFrame *frm; struct SwsContext *sws;
#endif
    struct dec *next;
} dec;

static dec *g_decs;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int mlog(void) { static int l = -1; if (l < 0) l = getenv("NX_MOVIE_LOG") != NULL; return l; }
#define MLOG(...) do { if (mlog()) LOG("[movie] " __VA_ARGS__); } while (0)

static dec *find(uint64_t self) { for (dec *d = g_decs; d; d = d->next) if (d->self == self) return d; return NULL; }

static void signal_ev(dec *d, int which) {
    if (!d->events) return;
    uint64_t ev = ((uint64_t *)(uintptr_t)d->events)[which];
    static uint64_t sig;
    if (!sig) sig = module_find("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    cpu_t *c = kernel_cpu();
    if (!ev || !sig || !c) return;
    uint64_t a[1] = {ev};
    cpu_call64(c, sig, a, 1);
}

// Guest memory that lives as long as the process: buffers are kept in a pool and handed again to the next decoder
static struct { uint64_t mem; size_t size; int used; } g_pool[64]; static int g_npool;
static uint64_t pool_take(size_t size) {
    for (int i = 0; i < g_npool; i++) if (!g_pool[i].used && g_pool[i].size >= size) { g_pool[i].used = 1; return g_pool[i].mem; }
    if (g_npool == 64) return 0;
    uint64_t m = guest_alloc(size, 0x1000);
    if (m) { g_pool[g_npool].mem = m; g_pool[g_npool].size = size; g_pool[g_npool].used = 1; g_npool++; }
    return m;
}
static void pool_give(uint64_t mem) { for (int i = 0; i < g_npool; i++) if (g_pool[i].mem == mem) g_pool[i].used = 0; }

// A frame's size in the output format
static size_t frame_bytes(const dec *d) { return d->fmt == FMT_ARGB ? (size_t)d->w * d->h * 4 : (size_t)d->w * d->h * 3 / 2; }

static int out_slot(dec *d) {
    for (int i = 0; i < NOUT; i++) if (!d->out[i].busy) {
        size_t need = frame_bytes(d);
        if (d->out[i].cap < need) { d->out[i].mem = pool_take(need); d->out[i].cap = d->out[i].mem ? need : 0; }
        if (!d->out[i].mem) return -1;
        d->out[i].busy = 1; return i;
    }
    return -1;
}

static void push_frame(dec *d, const uint8_t *const *planes, const int *stride, int src_fmt, int64_t pts) {
    int i = out_slot(d);
    if (i < 0) { MLOG("decoder %#llx: no free output slot, a frame is dropped", (unsigned long long)d->self); return; }
    uint8_t *dst = (uint8_t *)(uintptr_t)d->out[i].mem;
    size_t n = frame_bytes(d);
    if (!planes) {                                                   // black: zero RGBA, or Y 16 with neutral chroma
        if (d->fmt == FMT_ARGB) memset(dst, 0, n);
        else { memset(dst, 16, (size_t)d->w * d->h); memset(dst + (size_t)d->w * d->h, 128, n - (size_t)d->w * d->h); }
    }
#ifdef NX_HAVE_FFMPEG
    else {
        enum AVPixelFormat want = d->fmt == FMT_ARGB ? AV_PIX_FMT_RGBA : AV_PIX_FMT_NV12;
        d->sws = sws_getCachedContext(d->sws, d->w, d->h, (enum AVPixelFormat)src_fmt, d->w, d->h, want, SWS_BILINEAR, NULL, NULL, NULL);
        uint8_t *dp[4] = {dst, dst + (size_t)d->w * d->h, NULL, NULL}; int ds[4] = {d->fmt == FMT_ARGB ? d->w * 4 : d->w, d->w, 0, 0};
        if (d->sws) sws_scale(d->sws, planes, stride, 0, d->h, dp, ds);
    }
#else
    (void)stride; (void)src_fmt;
#endif
    d->out[i].size = n; d->out[i].pts = pts; d->out[i].flags = 0;
    d->ready[d->nready++] = i;
}

// MediaData lookups through the guest's own movie::MediaData (strings and results in a scratch block of guest memory)
static uint64_t scratch(void) { static uint64_t s; if (!s) s = guest_alloc(0x1000, 0x1000); return s; }
static int md_int(uint64_t md, const char *key, int *out) {
    static uint64_t fn; if (!fn) fn = module_find("_ZNK5movie9MediaData9FindInt32EPKcPi");
    cpu_t *c = kernel_cpu(); uint64_t s = scratch();
    if (!fn || !c || !s || !md) return 0;
    strcpy((char *)(uintptr_t)(s + 0x100), key); *(int32_t *)(uintptr_t)s = 0;
    uint64_t a[3] = {md, s + 0x100, s};
    uint64_t r = cpu_call64(c, fn, a, 3);
    *out = *(int32_t *)(uintptr_t)s;
    return (r & 0xff) != 0;
}
static int md_buffer(uint64_t md, const char *key, const uint8_t **data, int *size) {
    static uint64_t fn; if (!fn) fn = module_find("_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE");
    cpu_t *c = kernel_cpu(); uint64_t s = scratch();
    if (!fn || !c || !s || !md) return 0;
    strcpy((char *)(uintptr_t)(s + 0x100), key); *(uint64_t *)(uintptr_t)s = 0;
    uint64_t a[3] = {md, s + 0x100, s};
    uint64_t r = cpu_call64(c, fn, a, 3);
    const mbuf *b = (const mbuf *)(uintptr_t)*(uint64_t *)(uintptr_t)s;
    if (!(r & 0xff) || !b || !b->base) return 0;
    *data = (const uint8_t *)(uintptr_t)(b->base + (uint64_t)b->offset); *size = b->size;
    return 1;
}
static void md_set_int(uint64_t md, const char *key, int v) {
    static uint64_t fn; if (!fn) fn = module_find("_ZN5movie9MediaData8SetInt32EPKci");
    cpu_t *c = kernel_cpu(); uint64_t s = scratch();
    if (!fn || !c || !s || !md) return;
    strcpy((char *)(uintptr_t)(s + 0x100), key);
    uint64_t a[3] = {md, s + 0x100, (uint64_t)(uint32_t)v};
    cpu_call64(c, fn, a, 3);
}
static void md_set_rect(uint64_t md, const char *key, int l, int t, int r, int b) {
    static uint64_t fn; if (!fn) fn = module_find("_ZN5movie9MediaData7SetRectEPKciiii");
    cpu_t *c = kernel_cpu(); uint64_t s = scratch();
    if (!fn || !c || !s || !md) return;
    strcpy((char *)(uintptr_t)(s + 0x100), key);
    uint64_t a[6] = {md, s + 0x100, (uint64_t)(uint32_t)l, (uint64_t)(uint32_t)t, (uint64_t)(uint32_t)r, (uint64_t)(uint32_t)b};
    cpu_call64(c, fn, a, 6);
}
static void fill_format(const dec *d, uint64_t md) {
    md_set_int(md, "width", d->w); md_set_int(md, "height", d->h);
    md_set_int(md, "stride", d->fmt == FMT_ARGB ? d->w * 4 : d->w); md_set_int(md, "slice-height", d->h);
    md_set_int(md, "color-format", d->fmt);
    md_set_rect(md, "crop", 0, 0, d->w - 1, d->h - 1);
}

#ifdef NX_HAVE_FFMPEG
static void av_feed(dec *d, const uint8_t *p, int n, int64_t pts) {
    if (!d->av || n <= 0) return;
    d->pkt->data = (uint8_t *)p; d->pkt->size = n; d->pkt->pts = pts;
    int e = avcodec_send_packet(d->av, d->pkt);
    if (e < 0) { MLOG("decoder %#llx: FFmpeg refused %d bytes (error %d, starts %02x %02x %02x %02x %02x)", (unsigned long long)d->self, n, e, p[0], p[1], p[2], p[3], n > 4 ? p[4] : 0); return; }
    while (avcodec_receive_frame(d->av, d->frm) == 0) {
        if (!d->w || !d->h) { d->w = d->frm->width; d->h = d->frm->height; }
        push_frame(d, (const uint8_t *const *)d->frm->data, d->frm->linesize, d->frm->format, d->frm->pts);
    }
}
#endif

// ---- the takeover
static uint64_t vtable(void) { static uint64_t v; if (!v) v = module_find("_ZTVN5movie7DecoderE"); return v ? v + 0x10 : 0; }

static void construct(uint64_t self, int type, int fmt, int mode) {
    uint64_t *o = (uint64_t *)(uintptr_t)self;
    o[0] = vtable(); o[1] = 0;
    pthread_mutex_lock(&g_mu);
    dec *d = find(self);
    if (d) {                           // a decoder made again at the same address: the old one was destroyed by the library's destructor
        for (int i = 0; i < NIN; i++) if (d->in_mem[i]) pool_give(d->in_mem[i]);
        for (int i = 0; i < NOUT; i++) if (d->out[i].mem) pool_give(d->out[i].mem);
#ifdef NX_HAVE_FFMPEG
        if (d->av) avcodec_free_context(&d->av);
        if (d->pkt) av_packet_free(&d->pkt);
        if (d->frm) av_frame_free(&d->frm);
        if (d->sws) sws_freeContext(d->sws);
#endif
        dec *next = d->next; memset(d, 0, sizeof *d); d->next = next;
    } else { d = calloc(1, sizeof *d); d->next = g_decs; g_decs = d; }
    d->self = self; d->type = type; d->fmt = fmt; d->mode = mode;
    pthread_mutex_unlock(&g_mu);
    static int said; if (!said++) LOG("[RED] movie: the hardware decoder is replaced by the host's (%s)",
#ifdef NX_HAVE_FFMPEG
        "FFmpeg"
#else
        "black frames: built without FFmpeg"
#endif
    );
    MLOG("Decoder %#llx type %d output format %d mode %d", (unsigned long long)self, type, fmt, mode);
}
HLE_FORCE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE", "movie decoder: the host decodes (no hardware decoder)", void, h_dec_ctor, uint64_t self, int type, int fmt, int mode) { construct(self, type, fmt, mode); }
HLE_FORCE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "movie decoder: the host decodes (no hardware decoder)", void, h_dec_ctor2, uint64_t self, int type, int fmt, int mode, int x) { (void)x; construct(self, type, fmt, mode); }

HLE_FORCE("_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", "movie decoder configure", int, h_dec_configure, uint64_t self, uint64_t md) {
    pthread_mutex_lock(&g_mu); dec *d = find(self); pthread_mutex_unlock(&g_mu);
    if (!d) return ST_NO_IMPL;
    int w = 0, h = 0; md_int(md, "width", &w); md_int(md, "height", &h);
    d->w = w; d->h = h;
    for (int i = 0; i < NIN; i++) { if (!d->in_mem[i]) d->in_mem[i] = pool_take(IN_CAP); d->in_free[i] = i; }
    d->nin_free = NIN;
#ifdef NX_HAVE_FFMPEG
    enum AVCodecID id = d->type == 1 ? AV_CODEC_ID_H264 : d->type == 2 ? AV_CODEC_ID_VP8 : d->type == 3 ? AV_CODEC_ID_VP9 : d->type == 4 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_NONE;
    const AVCodec *codec = id != AV_CODEC_ID_NONE ? avcodec_find_decoder(id) : NULL;
    if (codec) {
        d->av = avcodec_alloc_context3(codec); d->av->thread_count = 1;
        d->pkt = av_packet_alloc(); d->frm = av_frame_alloc();
        if (avcodec_open2(d->av, codec, NULL) < 0) avcodec_free_context(&d->av);
        const uint8_t *csd; int n;                                   // H.264 parameter sets, as the extractor's track configuration carries them
        for (int k = 0; k < 2 && d->av; k++) {
            char key[8] = "csd-0"; key[4] = (char)('0' + k);
            if (md_buffer(md, key, &csd, &n)) { MLOG("Configure %#llx: %s %d bytes (%02x %02x %02x %02x %02x)", (unsigned long long)self, key, n, csd[0], csd[1], csd[2], csd[3], n > 4 ? csd[4] : 0); av_feed(d, csd, n, 0); }
            else MLOG("Configure %#llx: no %s", (unsigned long long)self, key);
        }
        if (d->av && md_buffer(md, "avcc", &csd, &n) && n > 7) {     // an AVCDecoderConfigurationRecord: its SPS and PPS go in with start codes, as the samples come
            MLOG("Configure %#llx: avcc %d bytes", (unsigned long long)self, n);
            static const uint8_t sc[4] = {0, 0, 0, 1};
            uint8_t *ps = malloc((size_t)n * 2 + 64); int k = 0, p = 5;
            for (int set = 0; set < 2 && p < n; set++) {
                int cnt = set == 0 ? (csd[p] & 31) : csd[p]; p++;
                for (int i = 0; i < cnt && p + 2 <= n; i++) {
                    int len = csd[p] << 8 | csd[p + 1]; p += 2;
                    if (p + len > n) break;
                    memcpy(ps + k, sc, 4); memcpy(ps + k + 4, csd + p, (size_t)len); k += 4 + len; p += len;
                }
            }
            if (k) av_feed(d, ps, k, 0);
            free(ps);
        }
    }
#endif
    MLOG("Configure %#llx: %dx%d", (unsigned long long)self, w, h);
    return ST_OK;
}
HLE_FORCE("_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", "movie decoder events", int, h_dec_events, uint64_t self, uint64_t ev) {
    dec *d = find(self); if (!d) return ST_NO_IMPL;
    d->events = ev; MLOG("RegisterClientEvents %#llx: %#llx", (unsigned long long)self, (unsigned long long)ev);
    return ST_OK;
}
HLE_FORCE("_ZN5movie7Decoder5StartEv", "movie decoder start", int, h_dec_start, uint64_t self) {
    dec *d = find(self); if (!d) return ST_NO_IMPL;
    d->started = 1; MLOG("Start %#llx", (unsigned long long)self);
    signal_ev(d, EV_INPUT);
    return ST_OK;
}
static int reset(uint64_t self, const char *what) {
    dec *d = find(self); if (!d) return ST_NO_IMPL;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < NIN; i++) d->in_free[i] = i;
    d->nin_free = NIN; d->nready = 0;
    for (int i = 0; i < NOUT; i++) d->out[i].busy = 0;
#ifdef NX_HAVE_FFMPEG
    if (d->av) avcodec_flush_buffers(d->av);
#endif
    pthread_mutex_unlock(&g_mu);
    MLOG("%s %#llx", what, (unsigned long long)self);
    return ST_OK;
}
HLE_FORCE("_ZN5movie7Decoder4StopEv", "movie decoder stop", int, h_dec_stop, uint64_t self) { dec *d = find(self); if (d) d->started = 0; return reset(self, "Stop"); }
HLE_FORCE("_ZN5movie7Decoder5ResetEv", "movie decoder reset", int, h_dec_reset, uint64_t self) { return reset(self, "Reset"); }
HLE_FORCE("_ZN5movie7Decoder5FlushEv", "movie decoder flush", int, h_dec_flush, uint64_t self) { int r = reset(self, "Flush"); dec *d = find(self); if (d) signal_ev(d, EV_INPUT); return r; }

HLE_FORCE("_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", "movie decoder input buffer", int, h_dec_acq_in, uint64_t self, int32_t *index, int32_t *left) {
    dec *d = find(self); if (!d) return ST_NO_IMPL;
    if (!index || !left) return ST_BAD_ARG;
    pthread_mutex_lock(&g_mu);
    *index = -1; *left = 0;
    int r = ST_NO_BUFFER;
    int busy = 0; for (int i = 0; i < NOUT; i++) busy += d->out[i].busy;
    if (busy >= NOUT - 2) d->held = 1;                                // the output side is full: input waits, as a hardware decoder's would (ReleaseOutputBufferIndex signals again)
    else if (d->nin_free) { *index = d->in_free[0]; memmove(d->in_free, d->in_free + 1, (size_t)--d->nin_free * sizeof *d->in_free); *left = d->nin_free; r = ST_OK; }
    pthread_mutex_unlock(&g_mu);
    return r;
}
HLE_FORCE("_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE", "movie decoder input buffer", int, h_dec_get_in, uint64_t self, int index, mbuf *b) {
    dec *d = find(self); if (!d) return ST_NO_IMPL;
    if (index < 0 || index >= NIN || !b || !d->in_mem[index]) return ST_BAD_ARG;
    b->base = d->in_mem[index]; b->capacity = IN_CAP; b->owns = 0; b->offset = 0; b->size = 0;
    return ST_OK;
}
HLE_FORCE("_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", "movie decoder decode", int, h_dec_send, uint64_t self, int index, int offset, int size, int64_t pts, uint32_t flags) {
    dec *d = find(self); if (!d) return ST_NO_IMPL;
    if (index < 0 || index >= NIN) return ST_BAD_ARG;
    pthread_mutex_lock(&g_mu);
    int before = d->nready;
#ifdef NX_HAVE_FFMPEG
    if (d->av && size > 0 && offset >= 0 && (size_t)offset + (size_t)size <= IN_CAP) av_feed(d, (const uint8_t *)(uintptr_t)d->in_mem[index] + offset, size, pts);
    else if (!d->av && size > 0 && d->w && d->h) push_frame(d, NULL, NULL, 0, pts);
    if (d->av && (flags & 4)) { avcodec_send_packet(d->av, NULL); while (avcodec_receive_frame(d->av, d->frm) == 0) push_frame(d, (const uint8_t *const *)d->frm->data, d->frm->linesize, d->frm->format, d->frm->pts); }
#else
    if (size > 0 && d->w && d->h) push_frame(d, NULL, NULL, 0, pts);
#endif
    if (flags & 4) {                                                 // end of stream: an empty frame carries the flag out
        int i = d->nready < NOUT ? out_slot(d) : -1;
        if (i >= 0) { d->out[i].size = 0; d->out[i].pts = pts; d->out[i].flags = flags; d->ready[d->nready++] = i; }
    }
    d->in_free[d->nin_free++] = index;
    int send_fmt = d->nready > before && !d->fmt_sent && d->w;
    if (send_fmt) d->fmt_sent = 1;
    int got = d->nready > before;
    pthread_mutex_unlock(&g_mu);
    static unsigned n; if (n++ < 8) MLOG("SendInputBufferForDecode %#llx index %d offset %d size %d pts %lld flags %#x: %d frames ready", (unsigned long long)self, index, offset, size, (long long)pts, flags, d->nready);
    if (send_fmt) signal_ev(d, EV_FORMAT);
    if (got) signal_ev(d, EV_OUTPUT);
    signal_ev(d, EV_INPUT);
    return ST_OK;
}
HLE_FORCE("_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_", "movie decoder output buffer", int, h_dec_acq_out, uint64_t self, int32_t *index, int64_t *pts, uint32_t *flags, int32_t *left) {
    dec *d = find(self); if (!d) return ST_NO_IMPL;
    if (!index || !pts || !flags || !left) return ST_BAD_ARG;
    pthread_mutex_lock(&g_mu);
    *index = -1; *pts = 0; *flags = 0; *left = 0;
    int r = ST_NO_BUFFER;
    if (d->nready) { int i = d->ready[0]; memmove(d->ready, d->ready + 1, (size_t)--d->nready * sizeof *d->ready); *index = i; *pts = d->out[i].pts; *flags = d->out[i].flags; *left = d->nready; r = ST_OK; }
    pthread_mutex_unlock(&g_mu);
    static unsigned k; if (k++ < 12) MLOG("AcquireOutputBufferIndex %#llx: status %#x index %d pts %lld", (unsigned long long)self, r, *index, (long long)*pts);
    return r;
}
static int get_out(uint64_t self, int index, mbuf *b) {
    dec *d = find(self); if (!d) return ST_NO_IMPL;
    if (index < 0 || index >= NOUT || !b || !d->out[index].busy) return ST_BAD_ARG;
    size_t n = d->out[index].size;
    if (b->base && b->capacity >= n) {                               // the client's own memory: the frame is copied into it
        if (n) memcpy((void *)(uintptr_t)b->base, (const void *)(uintptr_t)d->out[index].mem, n);
    } else { b->base = d->out[index].mem; b->capacity = d->out[index].cap; b->owns = 0; }
    b->offset = 0; b->size = (int32_t)n;
    static unsigned k; if (k++ < 4) MLOG("GetOutputBuffer %#llx index %d: %zu bytes (client memory %s)", (unsigned long long)self, index, n, b->base == d->out[index].mem ? "no" : "yes");
    return ST_OK;
}
HLE_FORCE("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", "movie decoder output buffer", int, h_dec_get_out, uint64_t self, int index, mbuf *b) { return get_out(self, index, b); }
HLE_FORCE("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", "movie decoder output buffer", int, h_dec_get_out2, uint64_t self, int index, mbuf *b, uint64_t md) {
    int r = get_out(self, index, b); dec *d = find(self);
    if (!r && d && md) fill_format(d, md);
    return r;
}
HLE_FORCE("_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", "movie decoder release", int, h_dec_release, uint64_t self, int index) {
    dec *d = find(self); if (!d) return ST_NO_IMPL;
    if (index < 0 || index >= NOUT) return ST_BAD_ARG;
    pthread_mutex_lock(&g_mu); d->out[index].busy = 0; int held = d->held; d->held = 0; pthread_mutex_unlock(&g_mu);
    if (held) signal_ev(d, EV_INPUT);
    static unsigned k; if (k++ < 12) MLOG("ReleaseOutputBufferIndex %#llx index %d", (unsigned long long)self, index);
    return ST_OK;
}
HLE_FORCE("_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE", "movie decoder format", int, h_dec_format, uint64_t self, uint64_t md) {
    dec *d = find(self); if (!d) return ST_NO_IMPL;
    fill_format(d, md); MLOG("GetOutputFormat %#llx: %dx%d format %d", (unsigned long long)self, d->w, d->h, d->fmt);
    return ST_OK;
}
HLE_FORCE("_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE", "movie decoder format", int, h_dec_bformat, uint64_t self, int index, uint64_t md) {
    dec *d = find(self); if (!d) return ST_NO_IMPL;
    static unsigned k; if (k++ < 6) MLOG("GetOutputBufferFormat %#llx index %d", (unsigned long long)self, index);
    fill_format(d, md);
    return ST_OK;
}
HLE_FORCE("_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_", "movie decoder size", int, h_dec_bsize, uint64_t self, int32_t *a, int32_t *b) {
    dec *d = find(self); if (!d) return ST_NO_IMPL;
    if (a) *a = (int32_t)frame_bytes(d);
    if (b) *b = NOUT;
    MLOG("GetOutputBufferSize %#llx: %zu", (unsigned long long)self, frame_bytes(d));
    return ST_OK;
}
HLE_FORCE("_ZN5movie7Decoder12GetLastErrorEv", "movie decoder error", int, h_dec_error, uint64_t self) { (void)self; return ST_OK; }
