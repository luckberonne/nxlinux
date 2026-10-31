// The draw path of the Vulkan backend: the state the replay accumulates, the pipelines built from it, the render pass, and the draw calls.
#pragma once
#include "nvn_obj.h"

// Render-target and scissor state, shared with the clears (nvn_replay.c): the textures in a record are snapshots made at record time, so only serials are kept
typedef struct { int n; uint64_t cser[8]; view_t cview[8]; uint8_t has_cview[8]; uint64_t dser; view_t dview; uint8_t has_dview; } nvn_rt_t;
extern nvn_rt_t g_rt;
extern struct nvn_scissor_state { int has; int r[4]; } g_sc;       // x, y, w, h once the game set a scissor

void nvn_draw_pass_end(void);                    // closes the render pass, if one is open (everything but a draw needs it closed)
int nvn_draw_pass_active(void);                 // 1 while a render pass is open: no barrier can be recorded then
void nvn_draw_new_submission(void);              // a new command buffer: nothing is bound in it yet
int nvn_draw_state_op(uint32_t op, const void *payload);    // state records the draw reads; 1 if consumed
void nvn_draw(const p_draw *d);
void nvn_dispatch(const p_dispatch *d);        // compute: runs the compute program bound, outside any render pass
void nvn_draw_idx(const p_draw_idx *d);

uint64_t nvn_draw_count(void);                   // draws recorded into command buffers
uint64_t nvn_draw_skipped(void);                 // draws that could not be made (no program, an unsupported primitive...), each announced once in red
uint64_t nvn_pipeline_count(void);               // distinct pipelines built
int nvn_prog_keys(const prog_t *p, uint8_t out[6][8]);      // hle_nvn.c
