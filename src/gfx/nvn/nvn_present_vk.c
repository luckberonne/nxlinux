#include <signal.h>
#include <sys/stat.h>
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
#include <unistd.h>
#ifdef NX_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
// Three frame buffers (RGBA8, tightly packed), whose roles move by swapping indices under g_mu, so that no 8 MB copy runs while g_mu is held (the game's present takes g_mu: a copy
// under it stalled the game's thread for a millisecond or two every frame, twice):
//   g_ib[F]  front: where a frame is published (the async worker swaps its finished back buffer in; the synchronous path copies into it under the lock)
//   g_ib[W]  the async worker's back buffer, written outside the lock
//   g_ib[D]  the window's: the SDL thread takes the newest frame by swapping it with the front, then uploads it outside the lock
// g_last is the one that holds the newest complete frame (the front or the window's); the worker's buffer is never it, so readers under the lock see a whole frame.
typedef struct { uint8_t *p; size_t cap; uint32_t w, h; } fbuf_t;
static fbuf_t g_fb[3]; enum { FB_F, FB_W, FB_D };
static int g_ib[3] = {0, 1, 2}, g_last = -1;      // g_ib[role] = buffer index; g_last = buffer index of the newest frame, -1 before the first
static void fb_fit(fbuf_t *b, size_t n) { if (n > b->cap) { free(b->p); b->p = malloc(n); b->cap = b->p ? n : 0; } }
static uint64_t g_ticket, g_shown, g_pub;                                      // tickets handed out / frames fully handled by the sink / frames whose pixels are in a frame buffer (g_fb)
static double g_last_time;
static int g_sink_sdl, g_sink_init, g_quit;
static _Atomic uint64_t g_count;
static _Atomic int g_shot_signal;
static void shot_signal(int sig) { (void)sig; atomic_store(&g_shot_signal, 1); }

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

// ---- the SDL sink: a thread that owns the window
#ifdef NX_HAVE_SDL3
// Controller: the keyboard and the gamepads (up to 8, one player each) become Pro Controllers (the hid service in src/host/svc/hid_input.c publishes it).
// Keys: arrows D-pad, WASD left stick, K/Return A, J/Backspace B, I X, U Y, Q L, E R, 1 ZL, 3 ZR, P plus, O minus. A gamepad's A is the button labelled A (south on Xbox, east on Nintendo layouts).
void hid_input_set_player(int p, int connected, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) __attribute__((weak));
#define NX_PADS 8
static int16_t axis_of(SDL_Gamepad *g, SDL_GamepadAxis a) { int v = SDL_GetGamepadAxis(g, a); return (int16_t)(v < -32767 ? -32767 : v); }
static void input_poll(SDL_Gamepad **pads) {
    if (!hid_input_set_player) return;
    const bool *k = SDL_GetKeyboardState(NULL);
    static const struct { SDL_Scancode sc; int bit; } kmap[] = {
        {SDL_SCANCODE_K, 0}, {SDL_SCANCODE_RETURN, 0}, {SDL_SCANCODE_J, 1}, {SDL_SCANCODE_BACKSPACE, 1}, {SDL_SCANCODE_I, 2}, {SDL_SCANCODE_U, 3},
        {SDL_SCANCODE_Q, 6}, {SDL_SCANCODE_E, 7}, {SDL_SCANCODE_1, 8}, {SDL_SCANCODE_3, 9}, {SDL_SCANCODE_P, 10}, {SDL_SCANCODE_O, 11},
        {SDL_SCANCODE_LEFT, 12}, {SDL_SCANCODE_UP, 13}, {SDL_SCANCODE_RIGHT, 14}, {SDL_SCANCODE_DOWN, 15}};
    // A gamepad takes the lowest free player number the first time it is used (a button, a stick or a trigger moved), not when it is plugged in: a receiver such as the
    // Twin USB adapter shows two devices for its two ports even with one pad on it, and an empty port must not become a player. The keyboard is always player 1.
    static int slot_of[NX_PADS]; static int init;
    if (!init) { for (int i = 0; i < NX_PADS; i++) slot_of[i] = -1; init = 1; }
    uint64_t pb[NX_PADS] = {0}; int32_t ps[NX_PADS][4] = {{0}}; int conn[NX_PADS] = {1};
    for (unsigned i = 0; i < sizeof kmap / sizeof *kmap; i++) if (k[kmap[i].sc]) pb[0] |= 1ull << kmap[i].bit;
    ps[0][0] = (k[SDL_SCANCODE_D] - k[SDL_SCANCODE_A]) * 32767; ps[0][1] = (k[SDL_SCANCODE_W] - k[SDL_SCANCODE_S]) * 32767;
    for (int d = 0; d < NX_PADS; d++) {
        SDL_Gamepad *g = pads[d];
        if (!g) { slot_of[d] = -1; continue; }
        uint64_t b = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
        // The face buttons follow their labels, not their positions: a pad whose south button is labelled A (Xbox, generic) confirms with south; one whose east button is labelled A (Nintendo layout) confirms with east
        SDL_GamepadButton fa = SDL_GAMEPAD_BUTTON_SOUTH, fb = SDL_GAMEPAD_BUTTON_EAST, fx = SDL_GAMEPAD_BUTTON_WEST, fy = SDL_GAMEPAD_BUTTON_NORTH;
        if (SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A) { fa = SDL_GAMEPAD_BUTTON_EAST; fb = SDL_GAMEPAD_BUTTON_SOUTH; fx = SDL_GAMEPAD_BUTTON_NORTH; fy = SDL_GAMEPAD_BUTTON_WEST; }
        const struct { SDL_GamepadButton gb; int bit; } gmap[] = {
            {fa, 0}, {fb, 1}, {fx, 2}, {fy, 3},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, 4}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 5}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 6}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 7},
            {SDL_GAMEPAD_BUTTON_START, 10}, {SDL_GAMEPAD_BUTTON_BACK, 11}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 12}, {SDL_GAMEPAD_BUTTON_DPAD_UP, 13},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 14}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 15}};
        // Driving layout (the default): the left shoulder and trigger throw the item (the console's L and ZL, the game's item buttons), the right ones hop and drift (R). NX_PAD_LAYOUT=pro keeps the physical Pro Controller layout (RT = ZR instead of R).
        static int kart = -1; if (kart < 0) { const char *l = getenv("NX_PAD_LAYOUT"); kart = !(l && !strcmp(l, "pro")); }
        for (unsigned i = 0; i < sizeof gmap / sizeof *gmap; i++) if (SDL_GetGamepadButton(g, gmap[i].gb)) b |= 1ull << gmap[i].bit;
        if (axis_of(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) b |= 1ull << 8;                   // LT = ZL, throws the item
        if (axis_of(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) b |= 1ull << (kart ? 7 : 9);    // RT hops and drifts (R); ZR with the physical layout
        int32_t gx = axis_of(g, SDL_GAMEPAD_AXIS_LEFTX), gy = -axis_of(g, SDL_GAMEPAD_AXIS_LEFTY);
        if (gx * gx + gy * gy > 6000 * 6000) { lx = gx; ly = gy; }
        int32_t sx = axis_of(g, SDL_GAMEPAD_AXIS_RIGHTX), sy = -axis_of(g, SDL_GAMEPAD_AXIS_RIGHTY);
        if (sx * sx + sy * sy > 6000 * 6000) { rx = sx; ry = sy; }      // the same dead zone as the left stick: a worn pad does not drift the camera
        if (slot_of[d] < 0 && (b || lx || ly || rx || ry)) {
            int used[NX_PADS] = {0}; for (int e = 0; e < NX_PADS; e++) if (slot_of[e] >= 0) used[slot_of[e]] = 1;
            for (int s = 0; s < NX_PADS; s++) if (!used[s]) { slot_of[d] = s; fprintf(stderr, "[dm] [hid] '%s' joined as player %d\n", SDL_GetGamepadName(g), s + 1); break; }
        }
        int s = slot_of[d]; if (s < 0) continue;
        conn[s] = 1; pb[s] |= b;
        if (lx || ly) { ps[s][0] = lx; ps[s][1] = ly; }
        ps[s][2] = rx; ps[s][3] = ry;
    }
    for (int s = 0; s < NX_PADS; s++) hid_input_set_player(s, conn[s], pb[s], ps[s][0], ps[s][1], ps[s][2], ps[s][3]);
}
static void *sdl_thread(void *arg) {
    (void)arg;
    SDL_Window *win = NULL; SDL_Renderer *rr = NULL; SDL_Texture *tx = NULL; uint32_t tw = 0, th = 0; uint64_t seen = 0;
    SDL_Gamepad *pads[NX_PADS] = {0}; long shot_target = -1; unsigned shot_n = 0; int shot_now = 0;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "[dm] [RED] [nvn] SDL video could not start (%s): frames go to memory only\n", SDL_GetError()); goto fail; }
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_shown + 0 >= g_pub && !g_quit) {                 // nothing new: wake up now and then to keep the window alive
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 16 * 1000 * 1000; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&g_cv, &g_mu, &ts)) break;
        }
        uint64_t upto = g_pub; uint32_t w = 0, h = 0; const uint8_t *copy = NULL;
        if (upto > seen && g_last == g_ib[FB_F] && g_fb[g_last].p && g_fb[g_last].w && g_fb[g_last].h) { int t = g_ib[FB_F]; g_ib[FB_F] = g_ib[FB_D]; g_ib[FB_D] = t; g_last = t; }      // the newest frame becomes the window's: no copy
        if (upto > seen && g_last == g_ib[FB_D]) { copy = g_fb[g_last].p; w = g_fb[g_last].w; h = g_fb[g_last].h; }
        pthread_mutex_unlock(&g_mu);
        if (copy && shot_target >= 0 && (long)atomic_load(&g_count) > shot_target) {      // the traced frame has been presented: this copy is it (or a later, equal one on a still screen)
            const char *dir = getenv("NX_SHOT_DIR"); char fn[700]; snprintf(fn, sizeof fn, "%s", dir ? dir : "nx-shots"); mkdir(fn, 0755);
            snprintf(fn, sizeof fn, "%s/shot%u_frame%ld.ppm", dir ? dir : "nx-shots", shot_n, shot_target);
            FILE *o = fopen(fn, "wb"); if (o) { fprintf(o, "P6\n%u %u\n255\n", w, h); for (size_t i = 0; i < (size_t)w * h; i++) fwrite(copy + i * 4, 1, 3, o); fclose(o); fprintf(stderr, "[dm] [nvn] shot: frame written to %s\n", fn); }
            shot_target = -1;
        }
        if (copy) {
            if (!win) {
                win = SDL_CreateWindow("nxlinux", (int)w, (int)h, SDL_WINDOW_RESIZABLE);
                rr = win ? SDL_CreateRenderer(win, NULL) : NULL;
                if (!rr) { fprintf(stderr, "[dm] [RED] [nvn] no SDL window or renderer (%s): frames go to memory only\n", SDL_GetError()); goto fail; }
                { const char *v = getenv("NX_SDL_VSYNC"); SDL_SetRenderVSync(rr, v && *v == '0' ? 0 : 1); }      // NX_SDL_VSYNC=0: present without waiting for the display
            }
            if (!tx || tw != w || th != h) { if (tx) SDL_DestroyTexture(tx); tx = SDL_CreateTexture(rr, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h); tw = w; th = h;
                if (tx) SDL_SetTextureBlendMode(tx, SDL_BLENDMODE_NONE); }       // the game's alpha is not a transparency of the window: with the default blending the translucent menu bars were drawn over black and looked grey
            if (tx) { SDL_UpdateTexture(tx, NULL, copy, (int)w * 4); SDL_RenderClear(rr); SDL_RenderTexture(rr, tx, NULL, NULL);
                static unsigned nshown; const char *rb = getenv("NX_SDL_READBACK");           // NX_SDL_READBACK=<prefix>: every NX_SDL_READBACK_EVERY (default 100) shown frame, what the renderer holds for the window, to <prefix>.<n>.ppm
                static unsigned every_rb; if (!every_rb) every_rb = getenv("NX_SDL_READBACK_EVERY") ? (unsigned)atoi(getenv("NX_SDL_READBACK_EVERY")) : 100;
                if (rb && ++nshown % every_rb == 0) {
                    SDL_Surface *s0 = SDL_RenderReadPixels(rr, NULL), *s1 = s0 ? SDL_ConvertSurface(s0, SDL_PIXELFORMAT_RGB24) : NULL;
                    if (s1) { char fn[600]; snprintf(fn, sizeof fn, "%s.%u.ppm", rb, nshown); FILE *o = fopen(fn, "wb");
                        if (o) { fprintf(o, "P6\n%d %d\n255\n", s1->w, s1->h); for (int y = 0; y < s1->h; y++) fwrite((uint8_t *)s1->pixels + (size_t)y * s1->pitch, 1, (size_t)s1->w * 3, o); fclose(o); } }
                    if (s1) SDL_DestroySurface(s1); if (s0) SDL_DestroySurface(s0);
                }
                SDL_RenderPresent(rr); }
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) __atomic_store_n(&g_quit, 1, __ATOMIC_RELEASE);
            else if (ev.type == SDL_EVENT_KEY_DOWN && (ev.key.key == SDLK_F12 || ev.key.key == SDLK_F9) && !ev.key.repeat) shot_now = 1;
            else if (0) {      // F12: keep the frame shown (a PPM in NX_SHOT_DIR, default nx-shots) and trace the draws of that frame to the log
                extern long nvn_trace_request(void); shot_target = nvn_trace_request(); shot_n++; fprintf(stderr, "[dm] [nvn] F12: shot %u will be frame %ld, its draws are traced to this log\n", shot_n, shot_target); }
            else if (ev.type == SDL_EVENT_GAMEPAD_ADDED) { static int maxp = -1; if (maxp < 0) { const char *m = getenv("NX_MAX_PADS"); maxp = m ? atoi(m) : NX_PADS; }      // NX_MAX_PADS=<n>: only the first n gamepads count
                for (int i = 0; i < NX_PADS && i < maxp; i++) if (!pads[i]) { pads[i] = SDL_OpenGamepad(ev.gdevice.which); if (pads[i]) fprintf(stderr, "[dm] [hid] gamepad '%s' is device %d\n", SDL_GetGamepadName(pads[i]), i + 1); break; } }      // the n-th pad plugged in is player n; a freed slot is taken by the next one
            else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) { for (int i = 0; i < NX_PADS; i++) if (pads[i] && SDL_GetGamepadID(pads[i]) == ev.gdevice.which) { SDL_CloseGamepad(pads[i]); pads[i] = NULL; } }
        }
        {   // a shot is asked for by F9 / F12, by SIGUSR1, or every NX_SHOT_EVERY seconds (at most NX_SHOT_MAX, default 80, in all)
            static double every = -2, last; static unsigned maxn; if (every < -1) { const char *e = getenv("NX_SHOT_EVERY"); every = e ? atof(e) : 0; const char *m = getenv("NX_SHOT_MAX"); maxn = m ? (unsigned)atoi(m) : 80; signal(SIGUSR1, shot_signal); }
            double nowt = now_s(); if (every > 0 && nowt - last >= every) { last = nowt; shot_now = 1; }
            if (atomic_exchange(&g_shot_signal, 0)) shot_now = 1;
            if (shot_now && shot_target < 0 && shot_n < maxn) { extern long nvn_trace_request(void); shot_target = nvn_trace_request(); shot_n++; fprintf(stderr, "[dm] [nvn] shot %u will be frame %ld, its draws are traced to this log\n", shot_n, shot_target); }
            shot_now = 0;
        }
        input_poll(pads);
        pthread_mutex_lock(&g_mu); if (upto > seen) seen = upto; g_shown = seen; pthread_cond_broadcast(&g_cv); int quit = g_quit; pthread_mutex_unlock(&g_mu);
        if (quit) break;
    }
    if (tx) SDL_DestroyTexture(tx);
    if (rr) SDL_DestroyRenderer(rr);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    // Closing the window ends the process. It used to end only the window: the game ran on unseen, a core or more each, and every launch from the desktop left one more behind
    // (four after a day, slowing the one being played). Nothing is lost: save data goes to its files with pwrite as the game writes it (ipc_fs.c), not through a buffer.
    fprintf(stderr, "[dm] the window was closed: exiting\n"); fflush(NULL);
    _exit(0);
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
// finished the frame, puts them in its back buffer, swaps it in as the front (g_fb) and tells the sink. The game's wait for the display is relaxed to the frame before (one frame of latency, as nxnative's present snapshots), so
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
        size_t n = (size_t)j.rb.w * j.rb.h;
        pthread_mutex_lock(&g_mu); fbuf_t *b = &g_fb[g_ib[FB_W]]; pthread_mutex_unlock(&g_mu);      // only this thread moves the back buffer's role, so it stays this one
        if (src) fb_fit(b, n * 4);
        int got = src && b->p;
        if (got) {                                                             // outside the lock
            if (j.swap) { uint8_t *d = b->p; for (size_t i = 0; i < n; i++) { d[i * 4] = src[i * 4 + 2]; d[i * 4 + 1] = src[i * 4 + 1]; d[i * 4 + 2] = src[i * 4]; d[i * 4 + 3] = src[i * 4 + 3]; } }
            else memcpy(b->p, src, n * 4);
            b->w = j.rb.w; b->h = j.rb.h;
        }
        pthread_mutex_lock(&g_mu);
        if (got) { int t = g_ib[FB_F]; g_ib[FB_F] = g_ib[FB_W]; g_ib[FB_W] = t; g_last = g_ib[FB_F]; }
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
        extern long nvn_trace_target(void); long tf = nvn_trace_target();
        if (tf >= 0 && getenv("NX_FRAME_DUMP") && (long)atomic_load(&g_count) == tf) {
            extern int nvn_draw_trace_targets(uint64_t *, int); extern int nvn_vk_debug_info(uint64_t, unsigned *, unsigned *, unsigned *, unsigned *, unsigned *); extern long nvn_vk_debug_lit(uint64_t, size_t *, const char *);
            {   // the textures the traced draws sampled (not only the ones they drew into): the GPU's decoded view of each, <prefix>.<n+1>.tex<serial>.ppm
                extern int nvn_trace_request_count(void); if (nvn_trace_request_count() <= 1) { extern int nvn_draw_trace_sampled(uint64_t *, int); extern int nvn_vk_debug_gpu_ppm(uint64_t, const char *); uint64_t ss[400]; int nsm = nvn_draw_trace_sampled(ss, 400);
                for (int i = 0; i < nsm; i++) if (ss[i]) { char fn[640]; snprintf(fn, sizeof fn, "%s.%ld.tex%llu.ppm", getenv("NX_FRAME_DUMP"), tf + 1, (unsigned long long)ss[i]); nvn_vk_debug_gpu_ppm(ss[i], fn); } } }
            uint64_t ser[600]; int ns = nvn_draw_trace_targets(ser, 600);
            for (int i = 0; i < ns; i++) { unsigned f = 0, tw = 0, th = 0, lv = 0, dd = 0; if (!nvn_vk_debug_info(ser[i], &f, &tw, &th, &lv, &dd)) { fprintf(stderr, "[dm] [nvn] trace target serial %llu: not in the table\n", (unsigned long long)ser[i]); continue; }
                char fn[640]; snprintf(fn, sizeof fn, "%s.%ld.rt%llu.ppm", getenv("NX_FRAME_DUMP"), tf + 1, (unsigned long long)ser[i]); size_t tot = 0; long lit = nvn_vk_debug_lit(ser[i], &tot, fn);
                { extern int nvn_vk_debug_mem(uint64_t, uint64_t *, uint64_t *); uint64_t mm = 0, ms = 0; nvn_vk_debug_mem(ser[i], &mm, &ms); fprintf(stderr, "[dm] [nvn] trace target serial %llu: %ux%u format %#x levels %u%s, %ld of %zu bytes not zero, storage %#llx +%#llx\n", (unsigned long long)ser[i], tw, th, f, lv, dd ? " (finalized)" : "", lit, tot, (unsigned long long)mm, (unsigned long long)ms); } }
        }
    }
    pthread_mutex_lock(&g_mu);
    sink_init();
    { fbuf_t *b = &g_fb[g_ib[FB_F]]; fb_fit(b, (size_t)w * h * 4);
      if (b->p) { memcpy(b->p, px, (size_t)w * h * 4); b->w = (uint32_t)w; b->h = (uint32_t)h; g_last = g_ib[FB_F]; } }
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
    const fbuf_t *b = g_last >= 0 ? &g_fb[g_last] : NULL;
    int ok = b && b->p && b->w && b->h;
    if (ok) { if (w) *w = b->w; if (h) *h = b->h; if (rgba) memcpy(rgba, b->p, (size_t)b->w * b->h * 4 < cap ? (size_t)b->w * b->h * 4 : cap); }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
