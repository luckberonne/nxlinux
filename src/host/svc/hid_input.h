// Controller state the hid service publishes in its shared memory block, fed by whoever reads the devices (the SDL window).
#pragma once
#include <stdint.h>

// Buttons as nn::hid::NpadButton bits (A = bit 0, B = 1, X = 2, Y = 3, StickL = 4, StickR = 5, L = 6, R = 7, ZL = 8, ZR = 9, Plus = 10, Minus = 11, Left/Up/Right/Down = 12..15).
// Sticks are -32767..32767, up and right positive. Safe to call from any thread, before or after the guest maps the block.
void hid_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);

// Player p (0..7): one Pro Controller each; p > 0 shows as connected only while `connected` is nonzero. hid_input_set is player 0.
void hid_input_set_player(int p, int connected, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);

// The controller applet (svc/hle_hid.c): how many players are connected now, and keep players 1..n connected (still, no pad yet) until a pad takes them.
int hid_connected_count(void);
void hid_reserve_players(int n);
// AcquireNpadStyleSetUpdateEventHandle: the event of player p (0..7), signaled when that player connects or disconnects.
void hid_set_style_event(int p, uint32_t handle);

void hid_start(void);              // begins refilling the block once the guest maps it
#include "../ipc.h"
ipc_shmem_t *hid_block(void);
