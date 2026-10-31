// Presentation on Vulkan, the plain way: the texture the game presents is read back into host memory as RGBA8 (waiting for the GPU to finish it), and the frame goes to a sink.
// Two sinks. Memory keeps the last frame, for tests and for running without a display. SDL (NX_PRESENT=sdl, when built with SDL3) shows it in a window from its own thread, which also
// pumps the window's events; closing the window sets a flag the host can poll. A swapchain that blits on the GPU is the faster way and replaces this when frames get big;
// the readback of 1080p at 60 Hz is about 500 MB/s, which an iGPU with shared memory carries.
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_vk.h"
#include "nvn_vk_priv.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef NX_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static uint8_t *g_frame; static uint32_t g_fw, g_fh; static size_t g_fcap;      // the latest frame, RGBA8, tightly packed
static uint64_t g_ticket, g_shown, g_pub;                                      // tickets handed out / frames fully handled by the sink / frames whose pixels are in g_frame
static double g_last_time;
static int g_sink_sdl, g_sink_init, g_quit;
static _Atomic uint64_t g_count;

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

// ---- the SDL sink: a thread that owns the window
#ifdef NX_HAVE_SDL3
// Controller: the keyboard and the first gamepad become player 1's Pro Controller (the hid service in src/host/svc/hid_input.c publishes it).
// Keys: arrows D-pad, WASD left stick, K/Return A, J/Backspace B, I X, U Y, Q L, E R, 1 ZL, 3 ZR, P plus, O minus. A gamepad keeps its physical layout (east = A).
void hid_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) __attribute__((weak));
static int16_t axis_of(SDL_Gamepad *g, SDL_GamepadAxis a) { int v = SDL_GetGamepadAxis(g, a); return (int16_t)(v < -32767 ? -32767 : v); }
static void input_poll(SDL_Gamepad **pad) {
    if (!hid_input_set) return;
    const bool *k = SDL_GetKeyboardState(NULL);
    static const struct { SDL_Scancode sc; int bit; } kmap[] = {
        {SDL_SCANCODE_K, 0}, {SDL_SCANCODE_RETURN, 0}, {SDL_SCANCODE_J, 1}, {SDL_SCANCODE_BACKSPACE, 1}, {SDL_SCANCODE_I, 2}, {SDL_SCANCODE_U, 3},
        {SDL_SCANCODE_Q, 6}, {SDL_SCANCODE_E, 7}, {SDL_SCANCODE_1, 8}, {SDL_SCANCODE_3, 9}, {SDL_SCANCODE_P, 10}, {SDL_SCANCODE_O, 11},
        {SDL_SCANCODE_LEFT, 12}, {SDL_SCANCODE_UP, 13}, {SDL_SCANCODE_RIGHT, 14}, {SDL_SCANCODE_DOWN, 15}};
    uint64_t b = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    for (unsigned i = 0; i < sizeof kmap / sizeof *kmap; i++) if (k[kmap[i].sc]) b |= 1ull << kmap[i].bit;
    lx = (k[SDL_SCANCODE_D] - k[SDL_SCANCODE_A]) * 32767; ly = (k[SDL_SCANCODE_W] - k[SDL_SCANCODE_S]) * 32767;
    SDL_Gamepad *g = *pad;
    if (g) {
        static const struct { SDL_GamepadButton gb; int bit; } gmap[] = {
            {SDL_GAMEPAD_BUTTON_EAST, 0}, {SDL_GAMEPAD_BUTTON_SOUTH, 1}, {SDL_GAMEPAD_BUTTON_NORTH, 2}, {SDL_GAMEPAD_BUTTON_WEST, 3},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, 4}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 5}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 6}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 7},
            {SDL_GAMEPAD_BUTTON_START, 10}, {SDL_GAMEPAD_BUTTON_BACK, 11}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 12}, {SDL_GAMEPAD_BUTTON_DPAD_UP, 13},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 14}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 15}};
        for (unsigned i = 0; i < sizeof gmap / sizeof *gmap; i++) if (SDL_GetGamepadButton(g, gmap[i].gb)) b |= 1ull << gmap[i].bit;
        if (axis_of(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) b |= 1ull << 8;
        if (axis_of(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) b |= 1ull << 9;
        int32_t gx = axis_of(g, SDL_GAMEPAD_AXIS_LEFTX), gy = -axis_of(g, SDL_GAMEPAD_AXIS_LEFTY);
        if (gx * gx + gy * gy > 6000 * 6000) { lx = gx; ly = gy; }
        rx = axis_of(g, SDL_GAMEPAD_AXIS_RIGHTX); ry = -axis_of(g, SDL_GAMEPAD_AXIS_RIGHTY);
    }
    hid_input_set(b, lx, ly, rx, ry);
}
static void *sdl_thread(void *arg) {
    (void)arg;
    SDL_Window *win = NULL; SDL_Renderer *rr = NULL; SDL_Texture *tx = NULL; uint32_t tw = 0, th = 0; uint64_t seen = 0;
    SDL_Gamepad *pad = NULL;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "[dm] [RED] [nvn] SDL video could not start (%s): frames go to memory only\n", SDL_GetError()); goto fail; }
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_shown + 0 >= g_pub && !g_quit) {                 // nothing new: wake up now and then to keep the window alive
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 16 * 1000 * 1000; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&g_cv, &g_mu, &ts)) break;
        }
        uint64_t upto = g_pub; uint32_t w = g_fw, h = g_fh; uint8_t *copy = NULL;
        if (upto > seen && g_frame && w && h) { copy = malloc((size_t)w * h * 4); memcpy(copy, g_frame, (size_t)w * h * 4); }
        pthread_mutex_unlock(&g_mu);
        if (copy) {
            if (!win) {
                win = SDL_CreateWindow("nxlinux", (int)w, (int)h, SDL_WINDOW_RESIZABLE);
                rr = win ? SDL_CreateRenderer(win, NULL) : NULL;
                if (!rr) { fprintf(stderr, "[dm] [RED] [nvn] no SDL window or renderer (%s): frames go to memory only\n", SDL_GetError()); free(copy); goto fail; }
                SDL_SetRenderVSync(rr, 1);
            }
            if (!tx || tw != w || th != h) { if (tx) SDL_DestroyTexture(tx); tx = SDL_CreateTexture(rr, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h); tw = w; th = h;
                if (tx) SDL_SetTextureBlendMode(tx, SDL_BLENDMODE_NONE); }       // the game's alpha is not a transparency of the window: with the default blending the translucent menu bars were drawn over black and looked grey
            if (tx) { SDL_UpdateTexture(tx, NULL, copy, (int)w * 4); SDL_RenderClear(rr); SDL_RenderTexture(rr, tx, NULL, NULL); SDL_RenderPresent(rr); }
            free(copy);
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) __atomic_store_n(&g_quit, 1, __ATOMIC_RELEASE);
            else if (ev.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(ev.gdevice.which);
            else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED && pad && SDL_GetGamepadID(pad) == ev.gdevice.which) { SDL_CloseGamepad(pad); pad = NULL; }
        }
        input_poll(&pad);
        pthread_mutex_lock(&g_mu); if (upto > seen) seen = upto; g_shown = seen; pthread_cond_broadcast(&g_cv); int quit = g_quit; pthread_mutex_unlock(&g_mu);
        if (quit) break;
    }
    if (tx) SDL_DestroyTexture(tx);
    if (rr) SDL_DestroyRenderer(rr);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    return NULL;
fail:
    pthread_mutex_lock(&g_mu); g_sink_sdl = 0; g_shown = UINT64_MAX; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    return NULL;
}
#endif

static void sink_init(void) {
    if (g_sink_init) return;
    g_sink_init = 1;
    const char *p = getenv("NX_PRESENT");
    if (p && !strcmp(p, "sdl")) {
#ifdef NX_HAVE_SDL3
        pthread_t t; g_sink_sdl = 1;
        if (pthread_create(&t, NULL, sdl_thread, NULL)) g_sink_sdl = 0; else pthread_detach(t);
#else
        fprintf(stderr, "[dm] [RED] [nvn] NX_PRESENT=sdl but this build has no SDL3: frames go to memory only\n");
#endif
    }
}

// ---- the NVN layer's entry points
static inline uint64_t present_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static _Atomic uint64_t g_pt_read, g_pt_swap, g_pt_wait, g_pt_n;   // where a present spends its time (reported every 300 presents: NX_PRESENT_STATS=1)
// The presentation that does not make the game wait for the GPU: the copy of the window texture is submitted without waiting and a thread of its own picks the pixels up once the GPU has
// finished the frame, puts them in g_frame and tells the sink. The game's wait for the display is relaxed to the frame before (one frame of latency, as nxnative's present snapshots), so
// the frame limit stays. On for the window sink (and with NX_PRESENT_ASYNC for the memory sink); off for NX_PRESENT_SYNC and for the frame dumps, which need the pixels at once.
typedef struct { uint64_t tk; mtl_rb_t rb; int swap; } pjob_t;
static pjob_t g_jq[3]; static int g_jn, g_jbusy, g_worker_started, g_async_on;
static pthread_cond_t g_jcv = PTHREAD_COND_INITIALIZER;                         // the job queue changed
static int use_async(void) {
    static int v = -1; if (v >= 0) return v;
    v = (getenv("NX_PRESENT_SYNC") || getenv("NX_FRAME_DUMP")) ? 0 : (g_sink_sdl || getenv("NX_PRESENT_ASYNC")) ? 1 : 0;
    return v;
}
static void *present_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_jn == 0) pthread_cond_wait(&g_jcv, &g_mu);
        pjob_t j = g_jq[0]; g_jbusy = 1;
        pthread_mutex_unlock(&g_mu);
        const uint8_t *src = mtl_read_async_wait(&j.rb);                       // the GPU is done with the frame
        pthread_mutex_lock(&g_mu);
        size_t n = (size_t)j.rb.w * j.rb.h;
        if (src && n * 4 > g_fcap) { free(g_frame); g_frame = malloc(n * 4); g_fcap = n * 4; }
        if (src && g_frame) {
            if (j.swap) { uint8_t *d = g_frame; for (size_t i = 0; i < n; i++) { d[i * 4] = src[i * 4 + 2]; d[i * 4 + 1] = src[i * 4 + 1]; d[i * 4 + 2] = src[i * 4]; d[i * 4 + 3] = src[i * 4 + 3]; } }
            else memcpy(g_frame, src, n * 4);
            g_fw = j.rb.w; g_fh = j.rb.h;
        }
        if (j.tk > g_pub) g_pub = j.tk;
        if (!g_sink_sdl) g_shown = g_pub;
        for (int i = 1; i < g_jn; i++) g_jq[i - 1] = g_jq[i];
        g_jn--; g_jbusy = 0;
        pthread_cond_broadcast(&g_cv); pthread_cond_broadcast(&g_jcv);
        pthread_mutex_unlock(&g_mu);
        mtl_read_async_done(&j.rb);
    }
    return NULL;
}
static void drain_jobs(void) {                                                  // before anything that publishes a frame itself: the order of the frames must hold
    pthread_mutex_lock(&g_mu);
    while (g_jn || g_jbusy) pthread_cond_wait(&g_jcv, &g_mu);
    pthread_mutex_unlock(&g_mu);
}

uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    static int bad_fmt;
    (void)period_s;
    pthread_mutex_lock(&g_mu); sink_init(); pthread_mutex_unlock(&g_mu);
    int fmt = (int)t->d.format;
    int swap = fmt == NVN_FORMAT_BGRA8 || fmt == NVN_FORMAT_BGRA8_SRGB;
    if (!(swap || fmt == NVN_FORMAT_RGBA8 || fmt == NVN_FORMAT_RGBA8_SRGB)) {
        if (!__atomic_exchange_n(&bad_fmt, 1, __ATOMIC_RELAXED)) fprintf(stderr, "[dm] [RED] [nvn] present: the window texture has NVN format %#x; only RGBA8 and BGRA8 can be shown\n", fmt);
        pthread_mutex_lock(&g_mu); uint64_t tk = ++g_ticket; if (tk > g_pub) g_pub = tk; if (!g_sink_sdl) g_shown = g_ticket; pthread_mutex_unlock(&g_mu);
        return tk;
    }
    if (crop && !crop[2] && !crop[3]) crop = NULL;                    // an unset crop (all zeros) is the whole texture
    int x = crop ? crop[0] : 0, y = crop ? crop[1] : 0, w = crop ? crop[2] : (int)t->d.width, h = crop ? crop[3] : (int)t->d.height;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x + w > (int)t->d.width) w = (int)t->d.width - x;
    if (y + h > (int)t->d.height) h = (int)t->d.height - y;
    uint64_t tk;
    if (w <= 0 || h <= 0) { pthread_mutex_lock(&g_mu); tk = ++g_ticket; if (tk > g_pub) g_pub = tk; if (!g_sink_sdl) g_shown = g_ticket; pthread_mutex_unlock(&g_mu); return tk; }
    NVNcopyRegion r = {x, y, 0, w, h, 1};
    if (use_async()) {                                                          // the copy is submitted and the frame is published by the worker once the GPU is done
        pthread_mutex_lock(&g_mu); while (g_jn == 3) pthread_cond_wait(&g_jcv, &g_mu); pthread_mutex_unlock(&g_mu);
        mtl_rb_t rb;
        if (mtl_read_async_begin(t, &r, &rb) == 0) {
            pthread_mutex_lock(&g_mu);
            tk = ++g_ticket; g_last_time = now_s(); g_async_on = 1;
            g_jq[g_jn].tk = tk; g_jq[g_jn].rb = rb; g_jq[g_jn].swap = swap; g_jn++;
            if (!g_worker_started) { g_worker_started = 1; pthread_t th; if (pthread_create(&th, NULL, present_worker, NULL) == 0) pthread_detach(th); }
            atomic_fetch_add(&g_count, 1);
            pthread_cond_broadcast(&g_jcv);
            pthread_mutex_unlock(&g_mu);
            return tk;
        }
    }
    drain_jobs();
    uint8_t *px = malloc((size_t)w * h * 4);
    uint64_t pt0 = present_ns();
    mtl_read_texels(t, 0, 0, &r, px, (size_t)w * 4, (size_t)w * h * 4);
    uint64_t pt1 = present_ns();
    if (swap) for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t b = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = b; }
    atomic_fetch_add(&g_pt_read, pt1 - pt0); atomic_fetch_add(&g_pt_swap, present_ns() - pt1);
    {   // NX_FRAME_DUMP=<prefix>: frames 100, 400, 1000 and 2500 go to <prefix>.<n>.ppm, with a count of the pixels that are not black
        static int dump = -1; static unsigned nth; if (dump < 0) dump = getenv("NX_FRAME_DUMP") != NULL;
        nth++;
        if (dump && nth == 1000) { extern void nvn_vk_debug_texstats(void); nvn_vk_debug_texstats(); }
        if (dump && getenv("NX_TEX_RAW")) { extern void nvn_vk_debug_raw(unsigned); nvn_vk_debug_raw(nth); }
        static int every = -1; if (every < 0) every = getenv("NX_FRAME_EVERY") ? atoi(getenv("NX_FRAME_EVERY")) : 0;   // NX_FRAME_EVERY=<n>: also every n-th frame
        if (dump && (nth == 100 || nth == 400 || nth == 1000 || nth == 2500 || (nth > 2500 && nth % 1500 == 0) || (every > 0 && nth % (unsigned)every == 0))) {
            size_t lit = 0, translucent = 0; for (size_t i = 0; i < (size_t)w * h; i++) { if (px[i * 4] | px[i * 4 + 1] | px[i * 4 + 2]) lit++; if (px[i * 4 + 3] != 255) translucent++; }
            char fn[600]; snprintf(fn, sizeof fn, "%s.%u.ppm", getenv("NX_FRAME_DUMP"), nth);
            FILE *o = fopen(fn, "wb"); if (o) { fprintf(o, "P6\n%d %d\n255\n", w, h); for (size_t i = 0; i < (size_t)w * h; i++) fwrite(px + i * 4, 1, 3, o); fclose(o); }
            { extern long nvn_vk_debug_lit(uint64_t, size_t *, const char *); extern uint64_t nvn_draw_main_target(void); size_t tot = 0; uint64_t ms = nvn_draw_main_target(); char pn[640]; snprintf(pn, sizeof pn, "%s.%u.offscreen.ppm", getenv("NX_FRAME_DUMP"), nth); long l = ms ? nvn_vk_debug_lit(ms, &tot, pn) : -1;
              fprintf(stderr, "[dm] [nvn] offscreen target serial %llu: %ld of %zu bytes are not zero\n", (unsigned long long)ms, l, tot); }
            fprintf(stderr, "[dm] [nvn] frame %u written to %s: %zu of %zu pixels are not black, %zu with alpha below 255 (texture serial %llu)\n", nth, fn, lit, (size_t)w * h, translucent, (unsigned long long)t->serial);
        }
    }
    {   // NX_DRAW_TRACE=<n> with NX_FRAME_DUMP: every target the traced frame drew into goes to <prefix>.<n>.rt<serial>.ppm (RGBA8 and RGBA16F), with its format and how much of it is not zero
        static long tf = -2; if (tf == -2) tf = getenv("NX_DRAW_TRACE") ? atol(getenv("NX_DRAW_TRACE")) : -1;
        if (tf >= 0 && getenv("NX_FRAME_DUMP") && (long)atomic_load(&g_count) == tf) {
            extern int nvn_draw_trace_targets(uint64_t *, int); extern int nvn_vk_debug_info(uint64_t, unsigned *, unsigned *, unsigned *, unsigned *, unsigned *); extern long nvn_vk_debug_lit(uint64_t, size_t *, const char *);
            uint64_t ser[600]; int ns = nvn_draw_trace_targets(ser, 600);
            for (int i = 0; i < ns; i++) { unsigned f = 0, tw = 0, th = 0, lv = 0, dd = 0; if (!nvn_vk_debug_info(ser[i], &f, &tw, &th, &lv, &dd)) { fprintf(stderr, "[dm] [nvn] trace target serial %llu: not in the table\n", (unsigned long long)ser[i]); continue; }
                char fn[640]; snprintf(fn, sizeof fn, "%s.%ld.rt%llu.ppm", getenv("NX_FRAME_DUMP"), tf + 1, (unsigned long long)ser[i]); size_t tot = 0; long lit = nvn_vk_debug_lit(ser[i], &tot, fn);
                { extern int nvn_vk_debug_mem(uint64_t, uint64_t *, uint64_t *); uint64_t mm = 0, ms = 0; nvn_vk_debug_mem(ser[i], &mm, &ms); fprintf(stderr, "[dm] [nvn] trace target serial %llu: %ux%u format %#x levels %u%s, %ld of %zu bytes not zero, storage %#llx +%#llx\n", (unsigned long long)ser[i], tw, th, f, lv, dd ? " (finalized)" : "", lit, tot, (unsigned long long)mm, (unsigned long long)ms); } }
        }
    }
    pthread_mutex_lock(&g_mu);
    sink_init();
    if ((size_t)w * h * 4 > g_fcap) { free(g_frame); g_frame = malloc((size_t)w * h * 4); g_fcap = (size_t)w * h * 4; }
    memcpy(g_frame, px, (size_t)w * h * 4); g_fw = (uint32_t)w; g_fh = (uint32_t)h;
    tk = ++g_ticket; if (tk > g_pub) g_pub = tk; g_last_time = now_s();
    if (!g_sink_sdl) g_shown = g_ticket;
    atomic_fetch_add(&g_count, 1);
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    free(px);
    return tk;
}

void mtl_present_wait(uint64_t ticket) {
    uint64_t t0 = present_ns();
    pthread_mutex_lock(&g_mu);
    uint64_t slack = g_async_on ? 1 : 0;                                       // asynchronous frames: the frame before this one being shown is enough
    while (g_shown + slack < ticket && g_sink_sdl) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_pt_wait, present_ns() - t0);
    static int on = -1; if (on < 0) on = getenv("NX_PRESENT_STATS") != NULL;
    uint64_t n = atomic_fetch_add(&g_pt_n, 1) + 1;
    if (on && n % 300 == 0) fprintf(stderr, "[dm] [nvn] present per frame over the last 300: GPU read-back %.2f ms, BGRA->RGBA %.2f ms, wait for the window %.2f ms\n",
        (double)atomic_exchange(&g_pt_read, 0) / 300e6, (double)atomic_exchange(&g_pt_swap, 0) / 300e6, (double)atomic_exchange(&g_pt_wait, 0) / 300e6);
}
void mtl_present_flush(void) {
    pthread_mutex_lock(&g_mu);
    while (g_pub < g_ticket) pthread_cond_wait(&g_cv, &g_mu);                  // every frame handed out has its pixels in place
    uint64_t t = g_ticket;
    while (g_shown < t && g_sink_sdl) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
}
int mtl_has_window(void) { return g_sink_sdl; }
int mtl_display_hz(void) { return 60; }
double mtl_presented_time(uint64_t n) { pthread_mutex_lock(&g_mu); double t = n == g_ticket ? g_last_time : 0.0; pthread_mutex_unlock(&g_mu); return t; }

// ---- for tests and for the host
uint64_t nvn_present_count(void) { return atomic_load(&g_count); }
int nvn_present_quit_requested(void) { return __atomic_load_n(&g_quit, __ATOMIC_ACQUIRE); }
int nvn_present_last(uint32_t *w, uint32_t *h, uint8_t *rgba, size_t cap) {
    pthread_mutex_lock(&g_mu);
    int ok = g_frame && g_fw && g_fh;
    if (ok) { if (w) *w = g_fw; if (h) *h = g_fh; if (rgba) memcpy(rgba, g_frame, (size_t)g_fw * g_fh * 4 < cap ? (size_t)g_fw * g_fh * 4 : cap); }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
