// Controller state the hid service publishes in its shared memory block, fed by whoever reads the devices (the SDL window).
#pragma once
#include <stdint.h>

// Buttons as nn::hid::NpadButton bits (A = bit 0, B = 1, X = 2, Y = 3, StickL = 4, StickR = 5, L = 6, R = 7, ZL = 8, ZR = 9, Plus = 10, Minus = 11, Left/Up/Right/Down = 12..15).
// Sticks are -32767..32767, up and right positive. Safe to call from any thread, before or after the guest maps the block.
void hid_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);

void hid_start(void);              // begins refilling the block once the guest maps it
#include "../ipc.h"
ipc_shmem_t *hid_block(void);
