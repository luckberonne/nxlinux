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
static uint64_t g_ticket, g_shown;                                              // tickets handed out / frames fully handled by the sink
static double g_last_time;
static int g_sink_sdl, g_sink_init, g_quit;
static _Atomic uint64_t g_count;

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

// ---- the SDL sink: a thread that owns the window
#ifdef NX_HAVE_SDL3
static void *sdl_thread(void *arg) {
    (void)arg;
    SDL_Window *win = NULL; SDL_Renderer *rr = NULL; SDL_Texture *tx = NULL; uint32_t tw = 0, th = 0; uint64_t seen = 0;
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "[dm] [RED] [nvn] SDL video could not start (%s): frames go to memory only\n", SDL_GetError()); goto fail; }
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_shown + 0 >= g_ticket && !g_quit) {                 // nothing new: wake up now and then to keep the window alive
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 16 * 1000 * 1000; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&g_cv, &g_mu, &ts)) break;
        }
        uint64_t upto = g_ticket; uint32_t w = g_fw, h = g_fh; uint8_t *copy = NULL;
        if (upto > seen && g_frame && w && h) { copy = malloc((size_t)w * h * 4); memcpy(copy, g_frame, (size_t)w * h * 4); }
        pthread_mutex_unlock(&g_mu);
        if (copy) {
            if (!win) {
                win = SDL_CreateWindow("nxlinux", (int)w, (int)h, SDL_WINDOW_RESIZABLE);
                rr = win ? SDL_CreateRenderer(win, NULL) : NULL;
                if (!rr) { fprintf(stderr, "[dm] [RED] [nvn] no SDL window or renderer (%s): frames go to memory only\n", SDL_GetError()); free(copy); goto fail; }
                SDL_SetRenderVSync(rr, 1);
            }
            if (!tx || tw != w || th != h) { if (tx) SDL_DestroyTexture(tx); tx = SDL_CreateTexture(rr, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h); tw = w; th = h; }
            if (tx) { SDL_UpdateTexture(tx, NULL, copy, (int)w * 4); SDL_RenderClear(rr); SDL_RenderTexture(rr, tx, NULL, NULL); SDL_RenderPresent(rr); }
            free(copy);
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) __atomic_store_n(&g_quit, 1, __ATOMIC_RELEASE);
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
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    static int bad_fmt;
    (void)period_s;
    int fmt = (int)t->d.format;
    int swap = fmt == NVN_FORMAT_BGRA8 || fmt == NVN_FORMAT_BGRA8_SRGB;
    if (!(swap || fmt == NVN_FORMAT_RGBA8 || fmt == NVN_FORMAT_RGBA8_SRGB)) {
        if (!__atomic_exchange_n(&bad_fmt, 1, __ATOMIC_RELAXED)) fprintf(stderr, "[dm] [RED] [nvn] present: the window texture has NVN format %#x; only RGBA8 and BGRA8 can be shown\n", fmt);
        pthread_mutex_lock(&g_mu); uint64_t tk = ++g_ticket; if (!g_sink_sdl) g_shown = g_ticket; pthread_mutex_unlock(&g_mu);
        return tk;
    }
    if (crop && !crop[2] && !crop[3]) crop = NULL;                    // an unset crop (all zeros) is the whole texture
    int x = crop ? crop[0] : 0, y = crop ? crop[1] : 0, w = crop ? crop[2] : (int)t->d.width, h = crop ? crop[3] : (int)t->d.height;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x + w > (int)t->d.width) w = (int)t->d.width - x;
    if (y + h > (int)t->d.height) h = (int)t->d.height - y;
    uint64_t tk;
    if (w <= 0 || h <= 0) { pthread_mutex_lock(&g_mu); tk = ++g_ticket; if (!g_sink_sdl) g_shown = g_ticket; pthread_mutex_unlock(&g_mu); return tk; }
    uint8_t *px = malloc((size_t)w * h * 4);
    NVNcopyRegion r = {x, y, 0, w, h, 1};
    mtl_read_texels(t, 0, 0, &r, px, (size_t)w * 4, (size_t)w * h * 4);
    if (swap) for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t b = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = b; }
    pthread_mutex_lock(&g_mu);
    sink_init();
    if ((size_t)w * h * 4 > g_fcap) { free(g_frame); g_frame = malloc((size_t)w * h * 4); g_fcap = (size_t)w * h * 4; }
    memcpy(g_frame, px, (size_t)w * h * 4); g_fw = (uint32_t)w; g_fh = (uint32_t)h;
    tk = ++g_ticket; g_last_time = now_s();
    if (!g_sink_sdl) g_shown = g_ticket;
    atomic_fetch_add(&g_count, 1);
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    free(px);
    return tk;
}

void mtl_present_wait(uint64_t ticket) {
    pthread_mutex_lock(&g_mu);
    while (g_shown < ticket && g_sink_sdl) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
}
void mtl_present_flush(void) { pthread_mutex_lock(&g_mu); uint64_t t = g_ticket; pthread_mutex_unlock(&g_mu); mtl_present_wait(t); }
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
