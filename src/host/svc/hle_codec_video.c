// nn::codec's hardware video decoder (Vp9, Av1, H264, Vp8) for the SDKs that boot nn::gs (Switch 2 titles). The SDK's implementation drives the decoder through the graphics supervisor's user session
// (nn::gsv::GetUserSession), which is null here (hle_gs.c takes the boot over), so the first call locked a null critical section. The movie library (subsdk0: movie::VideoDecoder -> movie::detail::*)
// calls these functions; the host decodes with ffmpeg instead, so an Ocarina of Time title movie (Event/Movie/Dm_Title_0005.webm, which the title waits for) plays.
//
// What the library does with it, as far as the calls and the code around them show:
//  - a VideoDecoderImagePool is a set of decoded images (one per output buffer); InitializeDecoderImagePool makes it, CalculateDecoderImagePoolBuffer sizes the memory the library gives it
//    (the host keeps the images itself, so the size is nominal). The pool's width and height are in the VideoDecoderImagePreference MakeDecodeImagePreference built: +8 holds (width - 1) | (height - 1) << 16.
//  - Initialize<Codec>DecoderBy{Remote,Transfer}Memory makes the decoder; RegisterDecoderBuffer(decoder, buffer index, pool, image index) lends it an output buffer; Start/Stop/Flush/Finalize.
//  - Output buffers go through the decoder in a queue. RegisterDecoderBuffer and ReleaseDecoderOutputBuffer put a buffer in it; AcquireDecoderOutputBuffer (which the library calls right after
//    either, and never waits in) takes the next one in order and gives a fence, a sync point handle that is reached when the decoder has written a picture into that buffer. The library keeps the
//    acquired buffers in order and polls the first one's fence (nn::syncpt TryWaitSyncPoint, a request on a sync point event for its thread); once it is reached, GetDecoderOutputBufferProperties
//    gives the picture's size and timestamp, the library reads the image with ReadDecoderOutputImage{Nv12,Rgba} (or imports it as a native image, GetDecoderImagePoolNativeImageHandle, which is
//    logged where it happens) and gives the buffer back with ReleaseDecoderOutputBuffer.
//  - SubmitDecoderInputBuffer(&consumed, decoder, data, size, timestamp, last) feeds one compressed frame; each decoded frame goes into the oldest acquired buffer and reaches its fence (hle_gs.c
//    keeps the fences and the sync point events).
//  - GetDecoderSubmissionReadyEvent is a SystemEvent the library waits on before a Submit: signalled while the decoder has a free buffer and no frame waiting for one.
// NX_CODEC_LOG=1 logs every call. Without ffmpeg the images are black.
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

void dm_guest_sleep_ns(int64_t ns);

static int clog(void) { static int l = -1; if (l < 0) l = getenv("NX_CODEC_LOG") != NULL; return l; }
#define CLOG(...) do { if (clog()) LOG("[codec] " __VA_ARGS__); } while (0)

enum { CODEC_VP9, CODEC_AV1, CODEC_H264, CODEC_VP8 };
#define MAXBUF 64
#define MAXPEND 16

// hle_gs.c: the sync point fences and events
uint64_t syncpt_fence_new(void);
void syncpt_fence_signal(uint64_t id);

typedef struct { uint8_t *nv12; int w, h; } image;               // a decoded picture, NV12
typedef struct vpool { uint64_t obj; int w, h; uint32_t n; image *img; struct vpool *next; } vpool;

enum { B_NONE, B_QUEUED, B_ACQUIRED, B_DECODED };                // a buffer: unregistered; in the output queue; acquired, waiting for a picture; holding one

typedef struct vdec {
    uint64_t obj; int codec, w, h;
    struct { uint64_t pool; uint32_t pidx; int state; uint64_t fence; int64_t ts; int last; } buf[MAXBUF];
    uint32_t outq[MAXBUF]; int noutq;                             // queued buffers, in order
    uint32_t acq[MAXBUF]; int nacq;                               // acquired buffers still waiting for a picture, in order
    uint64_t event;                                               // the submission-ready SystemEventType (guest)
    int started, closed;
    int eof;                                                      // the title sent its last packet: the final picture is flagged last, which is how the movie player learns the stream is over
#ifdef NX_HAVE_FFMPEG
    AVCodecContext *av; AVPacket *pkt; AVFrame *frm; struct SwsContext *sws;
    AVFrame *pend[MAXPEND]; int npend;                            // decoded frames that found no acquired buffer
#endif
    struct vdec *next;
} vdec;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static vdec *g_decs; static vpool *g_pools;

// Fences reached while g_mu was held are signalled once it is released: signalling runs guest code, which may hand the core to a thread that wants g_mu
static __thread uint64_t t_sig[MAXBUF + MAXPEND]; static __thread int t_nsig;
static void flush_signals(void) { while (t_nsig > 0) syncpt_fence_signal(t_sig[--t_nsig]); }

static vdec *dec_find(uint64_t obj) { for (vdec *d = g_decs; d; d = d->next) if (d->obj == obj) return d; return NULL; }
static vpool *pool_find(uint64_t obj) { for (vpool *p = g_pools; p; p = p->next) if (p->obj == obj) return p; return NULL; }

static int npending(const vdec *d) {
#ifdef NX_HAVE_FFMPEG
    return d->npend;
#else
    (void)d; return 0;
#endif
}

// The submission-ready event: signalled when the library may submit again (a buffer waits for a picture, and no picture waits for a buffer)
static void update_ready(vdec *d) {
    int ready = d->event && d->nacq > 0 && npending(d) == 0 && !d->closed;
    if (ready) { uint64_t ev = d->event; pthread_mutex_unlock(&g_mu); hle_signal_system_event((void *)(uintptr_t)ev); pthread_mutex_lock(&g_mu); }
}

static image *slot_of(vdec *d, int b) {
    vpool *p = pool_find(d->buf[b].pool);
    if (!p || d->buf[b].pidx >= p->n) return NULL;
    return &p->img[d->buf[b].pidx];
}

static void store_black(image *im, int w, int h) {
    free(im->nv12); im->nv12 = malloc((size_t)w * h * 3 / 2); im->w = w; im->h = h;
    if (im->nv12) { memset(im->nv12, 16, (size_t)w * h); memset(im->nv12 + (size_t)w * h, 128, (size_t)w * h / 2); }
}

#ifdef NX_HAVE_FFMPEG
static void store_frame(vdec *d, image *im, const AVFrame *f) {
    int w = f->width, h = f->height;
    free(im->nv12); im->nv12 = malloc((size_t)w * h * 3 / 2); im->w = w; im->h = h;
    if (!im->nv12) return;
    d->sws = sws_getCachedContext(d->sws, w, h, (enum AVPixelFormat)f->format, w, h, AV_PIX_FMT_NV12, SWS_BILINEAR, NULL, NULL, NULL);
    if (!d->sws) { store_black(im, w, h); return; }
    uint8_t *dp[4] = {im->nv12, im->nv12 + (size_t)w * h, NULL, NULL}; int ds[4] = {w, w, 0, 0};
    sws_scale(d->sws, (const uint8_t *const *)f->data, f->linesize, 0, h, dp, ds);
}
#endif

// A decoded picture (or a black one) goes into the oldest acquired buffer, whose fence is then reached; the caller holds g_mu. Returns the buffer, -1 when none is waiting.
static int place(vdec *d, const void *frame, int64_t ts, int last) {
    if (d->nacq == 0) return -1;
    int b = (int)d->acq[0]; memmove(d->acq, d->acq + 1, (size_t)(d->nacq - 1) * sizeof d->acq[0]); d->nacq--;
    image *im = slot_of(d, b);
    if (im) {
#ifdef NX_HAVE_FFMPEG
        if (frame) store_frame(d, im, (const AVFrame *)frame); else store_black(im, d->w, d->h);
#else
        (void)frame; store_black(im, d->w, d->h);
#endif
    }
    d->buf[b].state = B_DECODED; d->buf[b].ts = ts; d->buf[b].last = last;
    if (t_nsig < (int)(sizeof t_sig / sizeof t_sig[0])) t_sig[t_nsig++] = d->buf[b].fence;
    CLOG("picture in buffer %d (pool %#llx image %u) ts %lld ns%s, fence %llu reached", b, (unsigned long long)d->buf[b].pool, d->buf[b].pidx, (long long)ts, last ? " (last)" : "", (unsigned long long)d->buf[b].fence);
    return b;
}

#ifdef NX_HAVE_FFMPEG
static void drain_pending(vdec *d) {
    while (d->npend > 0 && d->nacq > 0) {
        AVFrame *f = d->pend[0];
        place(d, f, f->pts, d->eof && d->npend == 1);
        av_frame_free(&f);
        memmove(d->pend, d->pend + 1, (size_t)(d->npend - 1) * sizeof d->pend[0]); d->npend--;
    }
}
static void receive_all(vdec *d) {
    while (avcodec_receive_frame(d->av, d->frm) == 0) {
        int64_t ts = d->frm->pts == AV_NOPTS_VALUE ? 0 : d->frm->pts;
        if (d->nacq > 0 && d->npend == 0) place(d, d->frm, ts, 0);
        else if (d->npend < MAXPEND) { AVFrame *c = av_frame_clone(d->frm); if (c) { c->pts = ts; d->pend[d->npend++] = c; } }
        av_frame_unref(d->frm);
    }
}
#endif

// ---- the decoder ----
static Result cv_init(int codec, uint64_t obj, int w, int h) {
    pthread_mutex_lock(&g_mu);
    vdec *d = dec_find(obj);
    if (!d) { d = calloc(1, sizeof *d); d->obj = obj; d->next = g_decs; g_decs = d; }
    d->codec = codec; d->w = w > 0 ? w : 1; d->h = h > 0 ? h : 1; d->started = d->closed = d->eof = 0; d->noutq = d->nacq = 0;
    memset(d->buf, 0, sizeof d->buf);
    if (!d->event) { d->event = guest_alloc(0x40, 8); if (d->event) hle_bind_system_event((void *)(uintptr_t)d->event, 1); }
#ifdef NX_HAVE_FFMPEG
    if (d->av) avcodec_free_context(&d->av);
    enum AVCodecID id = codec == CODEC_VP9 ? AV_CODEC_ID_VP9 : codec == CODEC_AV1 ? AV_CODEC_ID_AV1 : codec == CODEC_H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_VP8;
    const AVCodec *c = avcodec_find_decoder(id);
    if (c) { d->av = avcodec_alloc_context3(c); d->av->thread_count = 4; if (avcodec_open2(d->av, c, NULL) < 0) avcodec_free_context(&d->av); }
    if (!d->pkt) d->pkt = av_packet_alloc();
    if (!d->frm) d->frm = av_frame_alloc();
    d->npend = 0;
    CLOG("decoder %#llx: codec %d %dx%d, ffmpeg %s", (unsigned long long)obj, codec, d->w, d->h, d->av ? "open" : "unavailable");
#else
    CLOG("decoder %#llx: codec %d %dx%d, no ffmpeg (black frames)", (unsigned long long)obj, codec, d->w, d->h);
#endif
    pthread_mutex_unlock(&g_mu);
    return RES_OK;
}
static void enqueue(vdec *d, uint32_t idx) { d->buf[idx].state = B_QUEUED; d->outq[d->noutq++] = idx; }
static Result cv_register(uint64_t obj, uint32_t idx, uint64_t pool, uint32_t pidx) {
    pthread_mutex_lock(&g_mu);
    vdec *d = dec_find(obj);
    if (d && idx < MAXBUF) { d->buf[idx].pool = pool; d->buf[idx].pidx = pidx; enqueue(d, idx); }
    CLOG("register buffer %u -> pool %#llx image %u", idx, (unsigned long long)pool, pidx);
    pthread_mutex_unlock(&g_mu);
    return RES_OK;
}
static Result cv_start(uint64_t obj) { pthread_mutex_lock(&g_mu); vdec *d = dec_find(obj); if (d) { d->started = 1; update_ready(d); } CLOG("start"); pthread_mutex_unlock(&g_mu); return RES_OK; }
static Result cv_stop(uint64_t obj) { pthread_mutex_lock(&g_mu); vdec *d = dec_find(obj); if (d) d->started = 0; CLOG("stop"); pthread_mutex_unlock(&g_mu); return RES_OK; }
static Result cv_flush(uint64_t obj) {
    pthread_mutex_lock(&g_mu);
    vdec *d = dec_find(obj);
    if (d) {
#ifdef NX_HAVE_FFMPEG
        if (d->av) avcodec_flush_buffers(d->av);
        while (d->npend > 0) av_frame_free(&d->pend[--d->npend]);
#endif
        // what is waiting for a picture gets none: the library takes those buffers back through Release, and their fences are reached so that it can
        for (int i = 0; i < d->nacq; i++) { int b = (int)d->acq[i]; d->buf[b].state = B_DECODED; d->buf[b].ts = 0; d->buf[b].last = 0; if (t_nsig < (int)(sizeof t_sig / sizeof t_sig[0])) t_sig[t_nsig++] = d->buf[b].fence; }
        d->nacq = 0;
        update_ready(d);
    }
    CLOG("flush");
    pthread_mutex_unlock(&g_mu);
    flush_signals();
    return RES_OK;
}
static void cv_finalize(uint64_t obj) { pthread_mutex_lock(&g_mu); vdec *d = dec_find(obj); if (d) { d->closed = 1; d->started = 0; } CLOG("finalize"); pthread_mutex_unlock(&g_mu); }
static uint64_t cv_ready_event(uint64_t obj) { pthread_mutex_lock(&g_mu); vdec *d = dec_find(obj); uint64_t e = d ? d->event : 0; CLOG("submission-ready event of %#llx: %#llx", (unsigned long long)obj, (unsigned long long)e); pthread_mutex_unlock(&g_mu); return e; }

static Result cv_submit(uint64_t *consumed, uint64_t obj, const void *data, uint64_t size, int64_t ts, int last) {
    pthread_mutex_lock(&g_mu);
    vdec *d = dec_find(obj);
    if (consumed) *consumed = size;
    if (!d) { pthread_mutex_unlock(&g_mu); return RES_OK; }
    CLOG("submit %llu bytes ts %lld ns last %d (%d acquired buffers, %d pending pictures)", (unsigned long long)size, (long long)ts, last, d->nacq, npending(d));
#ifdef NX_HAVE_FFMPEG
    if (d->av) {
        if (size) {
            av_packet_unref(d->pkt);
            if (av_new_packet(d->pkt, (int)size) == 0) { memcpy(d->pkt->data, data, size); d->pkt->pts = ts; d->pkt->dts = ts; }
            pthread_mutex_unlock(&g_mu); kernel_core_release();
            int r = avcodec_send_packet(d->av, d->pkt);
            kernel_core_acquire(); pthread_mutex_lock(&g_mu);
            if (r < 0) CLOG("avcodec_send_packet failed (%d)", r);
        } else if (last) avcodec_send_packet(d->av, NULL);
        if (last && !size) d->eof = 1;
        receive_all(d);
        drain_pending(d);
        if (last) for (int i = 0; i < MAXBUF; i++) if (d->buf[i].state == B_DECODED && d->buf[i].ts == ts) d->buf[i].last = 1;
    } else
#endif
    {
        if (size) place(d, NULL, ts, last);
    }
    update_ready(d);
    pthread_mutex_unlock(&g_mu);
    flush_signals();
    return RES_OK;
}

// Takes the next queued buffer in order and gives its fence: reached when the decoder has written a picture into it (the library never waits in this call)
static Result cv_acquire(uint32_t *idx, uint64_t *handle, uint64_t obj) {
    static int once; if (!once++) LOG("codec: the video decoder is the host's (ffmpeg)");
    pthread_mutex_lock(&g_mu);
    vdec *d = dec_find(obj);
    if (!d || d->noutq == 0) { CLOG("acquire: no buffer in the output queue"); pthread_mutex_unlock(&g_mu); return RES_FAIL; }
    uint32_t b = d->outq[0]; memmove(d->outq, d->outq + 1, (size_t)(d->noutq - 1) * sizeof d->outq[0]); d->noutq--;
    d->buf[b].state = B_ACQUIRED; d->buf[b].fence = syncpt_fence_new(); d->acq[d->nacq++] = b;
    if (idx) *idx = b;
    if (handle) *handle = d->buf[b].fence;
    CLOG("acquire -> buffer %u, fence %llu", b, (unsigned long long)d->buf[b].fence);
#ifdef NX_HAVE_FFMPEG
    drain_pending(d);
#endif
    update_ready(d);
    pthread_mutex_unlock(&g_mu);
    flush_signals();
    return RES_OK;
}
static Result cv_release(uint64_t obj, uint32_t idx) {
    pthread_mutex_lock(&g_mu);
    vdec *d = dec_find(obj);
    if (d && idx < MAXBUF) enqueue(d, idx);
    CLOG("release buffer %u", idx);
    pthread_mutex_unlock(&g_mu);
    return RES_OK;
}
static Result cv_props(uint16_t *a, uint16_t *b, uint16_t *c, uint16_t *e, int64_t *ts, uint8_t *flag, uint64_t obj, uint32_t idx) {
    pthread_mutex_lock(&g_mu);
    vdec *d = dec_find(obj);
    int w = d ? d->w : 0, h = d ? d->h : 0; int64_t t = 0; int last = 0;
    if (d && idx < MAXBUF) { image *im = slot_of(d, (int)idx); if (im && im->w) { w = im->w; h = im->h; } t = d->buf[idx].ts; last = d->buf[idx].last; }
    pthread_mutex_unlock(&g_mu);
    if (a) *a = (uint16_t)w; if (b) *b = (uint16_t)h; if (c) *c = (uint16_t)w; if (e) *e = (uint16_t)h;
    if (ts) *ts = t; if (flag) *flag = (uint8_t)last;
    CLOG("properties of buffer %u: %dx%d ts %lld last %d", idx, w, h, (long long)t, last);
    return RES_OK;
}

// ---- the image pool ----
static int pref_w(const void *pref) { return pref ? (int)((*(const uint64_t *)((const uint8_t *)pref + 8)) & 0xffff) + 1 : 0; }
static int pref_h(const void *pref) { return pref ? (int)(((*(const uint64_t *)((const uint8_t *)pref + 8)) >> 16) & 0xffff) + 1 : 0; }

HLE_FORCE("_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", "image pool size: the host keeps the decoded images itself",
          Result, h_cv_PoolCalc, uint64_t *size, uint64_t *align, const void *pref, uint32_t count) {
    uint64_t one = ((uint64_t)pref_w(pref) * pref_h(pref) * 3 / 2 + 0xffff) & ~0xffffull;
    if (size) *size = one * (count ? count : 1); if (align) *align = 0x1000;
    CLOG("pool size for %dx%d x%u: %#llx", pref_w(pref), pref_h(pref), count, (unsigned long long)(one * count));
    return RES_OK;
}
HLE_FORCE("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj", "image pool: the images are the host's",
          Result, h_cv_PoolInit, uint64_t pool, void *mem, uint64_t size, const void *pref, uint32_t count) {
    (void)mem; (void)size;
    pthread_mutex_lock(&g_mu);
    vpool *p = pool_find(pool);
    if (!p) { p = calloc(1, sizeof *p); p->obj = pool; p->next = g_pools; g_pools = p; }
    if (p->img) { for (uint32_t i = 0; i < p->n; i++) free(p->img[i].nv12); free(p->img); }
    p->w = pref_w(pref); p->h = pref_h(pref); p->n = count; p->img = calloc(count ? count : 1, sizeof *p->img);
    CLOG("pool %#llx: %dx%d, %u images", (unsigned long long)pool, p->w, p->h, count);
    pthread_mutex_unlock(&g_mu);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", "image pool teardown", void, h_cv_PoolFin, uint64_t pool) {
    pthread_mutex_lock(&g_mu);
    for (vpool **pp = &g_pools; *pp; pp = &(*pp)->next) if ((*pp)->obj == pool) {
        vpool *p = *pp; *pp = p->next;
        for (uint32_t i = 0; i < p->n; i++) free(p->img[i].nv12);
        free(p->img); free(p); break;
    }
    CLOG("pool %#llx finalized", (unsigned long long)pool);
    pthread_mutex_unlock(&g_mu);
}
// The title takes the image as a native image (a texture imported without a copy) instead of reading it: not supported, said where it is asked
HLE_FORCE("_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", "native image of a decoded picture: not available on the host",
          uint64_t, h_cv_PoolNative, uint64_t pool, uint32_t idx) {
    LOG("[RED] codec: GetDecoderImagePoolNativeImageHandle(pool %#llx, image %u): the title imports decoded images as textures, which the host does not do", (unsigned long long)pool, idx);
    return 0;
}

// Reads from the picture: x, y, w, h select the part wanted; dst gets rows of `stride` bytes
static uint64_t guest_stack_arg(int n) { cpu_t *c = kernel_cpu(); return c ? ((uint64_t *)(uintptr_t)cpu_get_sp(c))[n] : 0; }
static const image *pool_image(uint64_t pool, uint32_t idx) { vpool *p = pool_find(pool); return p && idx < p->n && p->img[idx].nv12 ? &p->img[idx] : NULL; }

HLE_FORCE("_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", "read a decoded picture as NV12",
          Result, h_cv_ReadNv12, uint64_t pool, uint32_t idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *ydst, uint64_t ystride) {
    // after the rectangle, each plane is (destination, row stride, size): the luma plane's stride is the eighth argument, its size the first on the stack
    uint64_t ysize = guest_stack_arg(0); uint8_t *uvdst = (uint8_t *)(uintptr_t)guest_stack_arg(1); uint64_t uvstride = guest_stack_arg(2), uvsize = guest_stack_arg(3);
    x &= 0xffff; y &= 0xffff; w &= 0xffff; h &= 0xffff;
    pthread_mutex_lock(&g_mu);
    const image *im = pool_image(pool, idx);
    CLOG("read NV12 pool %#llx image %u rect %u,%u %ux%u -> y %p (%llu, stride %llu) uv %p (%llu, stride %llu)%s", (unsigned long long)pool, idx, x, y, w, h, (void *)ydst,
         (unsigned long long)ysize, (unsigned long long)ystride, (void *)uvdst, (unsigned long long)uvsize, (unsigned long long)uvstride, im ? "" : " [no picture: black]");
    for (uint32_t r = 0; r < h; r++) {
        uint8_t *d = ydst + (uint64_t)r * ystride;
        if (im && y + r < (uint32_t)im->h) { uint32_t n = x < (uint32_t)im->w ? (uint32_t)im->w - x : 0; if (n > w) n = w; memcpy(d, im->nv12 + (size_t)(y + r) * im->w + x, n); if (n < w) memset(d + n, 16, w - n); }
        else memset(d, 16, w);
    }
    if (uvdst) for (uint32_t r = 0; r < (h + 1) / 2; r++) {
        uint8_t *d = uvdst + (uint64_t)r * uvstride; uint32_t row = (y + r * 2) / 2;
        if (im && row < (uint32_t)im->h / 2) { uint32_t n = x < (uint32_t)im->w ? (uint32_t)im->w - x : 0; if (n > w) n = w; memcpy(d, im->nv12 + (size_t)im->w * im->h + (size_t)row * im->w + (x & ~1u), n); if (n < w) memset(d + n, 128, w - n); }
        else memset(d, 128, w);
    }
    { static int lm = -1; static int nl; if (lm < 0) lm = getenv("NX_CODEC_LUMA") ? (atoi(getenv("NX_CODEC_LUMA")) > 1 ? atoi(getenv("NX_CODEC_LUMA")) : 400) : 0; if (lm && nl < lm && ydst && w && h) { nl++; uint64_t sum = 0, k = 0; for (uint32_t r = 0; r < h; r += 8) for (uint32_t c = 0; c < w; c += 8) { sum += ydst[(uint64_t)r * ystride + c]; k++; }
        extern uint64_t nvn_present_count(void); fprintf(stderr, "[dm] [codec] read #%d at present %llu: luma mean %.1f%s\n", nl, (unsigned long long)nvn_present_count(), k ? (double)sum / k : 0.0, im ? "" : " (black)"); } }
    { static int dn = -2; static int cnt; if (dn == -2) dn = getenv("NX_CODEC_DUMP") ? atoi(getenv("NX_CODEC_DUMP")) : -1;       // NX_CODEC_DUMP=<n>: the luma plane of the n-th picture read goes to <NX_FRAME_DUMP or /tmp>.codec_y.pgm, with its range
      if (dn >= 0 && cnt++ == dn && ydst && w && h) { unsigned mn = 255, mx = 0; uint64_t sum = 0; for (uint32_t r = 0; r < h; r++) for (uint32_t c = 0; c < w; c++) { unsigned v = ydst[(uint64_t)r * ystride + c]; if (v < mn) mn = v; if (v > mx) mx = v; sum += v; }
        char fn[600]; snprintf(fn, sizeof fn, "%s.codec_y.pgm", getenv("NX_FRAME_DUMP") ? getenv("NX_FRAME_DUMP") : "/tmp/nx"); FILE *o = fopen(fn, "wb"); if (o) { fprintf(o, "P5\n%u %u\n255\n", w, h); for (uint32_t r = 0; r < h; r++) fwrite(ydst + (uint64_t)r * ystride, 1, w, o); fclose(o); }
        fprintf(stderr, "[dm] [codec] picture %d (image %u of the pool, %s) luma %u..%u mean %.1f -> %s\n", dn, idx, im ? "decoded" : "black", mn, mx, (double)sum / ((double)w * h), fn); } }
    pthread_mutex_unlock(&g_mu);
    return RES_OK;
}

static Result read_rgba(uint64_t pool, uint32_t idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *dst, uint64_t size, uint64_t stride, int hdr) {
    x &= 0xffff; y &= 0xffff; w &= 0xffff; h &= 0xffff;
    pthread_mutex_lock(&g_mu);
    const image *im = pool_image(pool, idx);
    CLOG("read %s pool %#llx image %u rect %u,%u %ux%u -> %p (%llu, stride %llu)%s", hdr ? "RGBA-HDR" : "RGBA", (unsigned long long)pool, idx, x, y, w, h, (void *)dst, (unsigned long long)size, (unsigned long long)stride, im ? "" : " [no picture: black]");
    for (uint32_t r = 0; r < h; r++) {
        uint8_t *d = dst + (uint64_t)r * stride;
        for (uint32_t c = 0; c < w; c++) {
            int Y = 16, U = 128, V = 128;
            if (im && x + c < (uint32_t)im->w && y + r < (uint32_t)im->h) {
                Y = im->nv12[(size_t)(y + r) * im->w + x + c];
                const uint8_t *uv = im->nv12 + (size_t)im->w * im->h + (size_t)((y + r) / 2) * im->w + ((x + c) & ~1u); U = uv[0]; V = uv[1];
            }
            int cy = Y - 16, cu = U - 128, cv = V - 128;
            int R = (298 * cy + 409 * cv + 128) >> 8, G = (298 * cy - 100 * cu - 208 * cv + 128) >> 8, B = (298 * cy + 516 * cu + 128) >> 8;
            R = R < 0 ? 0 : R > 255 ? 255 : R; G = G < 0 ? 0 : G > 255 ? 255 : G; B = B < 0 ? 0 : B > 255 ? 255 : B;
            if (!hdr) { d[c * 4] = (uint8_t)R; d[c * 4 + 1] = (uint8_t)G; d[c * 4 + 2] = (uint8_t)B; d[c * 4 + 3] = 255; }
            else { uint16_t *p = (uint16_t *)(d + c * 8); p[0] = (uint16_t)(R * 257); p[1] = (uint16_t)(G * 257); p[2] = (uint16_t)(B * 257); p[3] = 0xffff; }
        }
    }
    pthread_mutex_unlock(&g_mu);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", "read a decoded picture as RGBA8", Result, h_cv_ReadRgba,
          uint64_t pool, uint32_t idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *dst, uint64_t stride) { return read_rgba(pool, idx, x, y, w, h, dst, guest_stack_arg(0), stride, 0); }
HLE_FORCE("_ZN2nn5codec29ReadDecoderOutputImageRgbaHdrEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", "read a decoded picture as 16-bit RGBA", Result, h_cv_ReadRgbaHdr,
          uint64_t pool, uint32_t idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *dst, uint64_t stride) { return read_rgba(pool, idx, x, y, w, h, dst, guest_stack_arg(0), stride, 1); }

// ---- the four codecs share one ABI: the functions below are generated from the SDK's symbol names (hle_codec_video.inc) ----
#include "hle_codec_video.inc"
