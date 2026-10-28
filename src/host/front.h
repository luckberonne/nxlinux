// Host front end. On Linux a window (SDL3) provides the surface the graphics backend presents to; none yet.
#pragma once
#include <stdint.h>
#include <stdlib.h>
// Run the guest: with a window the guest runs on its own thread and the main thread belongs to the windowing system; without a window it runs directly on the current thread.
void front_run(void (*guest)(void));
// The highest refresh rate of the window's screen in Hz, 0 = not known. Any thread
int front_screen_max_fps(void);
// Text input box for the software keyboard: confirm returns 1 and writes UTF-8 into out; cancel 0; no window -1.
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap);
// Touchscreen: whenever the set of touch points changes the whole set is pushed once; n = 0 = all lifted. Normalized coordinates [0,1), origin at top left.
typedef struct { uint32_t id; float x, y; } dm_touch;
void dm_touch_set(const dm_touch *t, int n);
// Save not written (disk full etc.): called once per refused save transaction. The default only logs.
void front_save_failed(int err);
