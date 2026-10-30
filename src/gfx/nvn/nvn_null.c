// What the Vulkan backend (nvn_vk.c) does not do yet, behind the Metal-layer interface that hle_nvn.c calls (nvn_mtl.h): presentation,
// pacing, statistics, and the replay of recorded commands are accepted and do nothing. Each is replaced by a real one as it is written.
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include <stdatomic.h>

// Memory pools, submissions and fences are in nvn_vk.c; textures in nvn_tex_vk.c. On the null GPU there are no images: a texture the
// game writes through the CPU is read back by de-swizzling its storage only, and a GPU-owned one reads as zero.

// ---- presentation is in nvn_present_vk.c; there is no display link, so the game paces itself against its own clock
int mtl_fps_target(void) { return 60; }              // hle_nvn.c divides by it
int mtl_link_active(void) { return 0; }
int mtl_link_rate(void) { return 60; }
int mtl_link_stalled(void) { return 0; }
int mtl_link_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { (void)n; (void)target; (void)lead_ms; (void)ticks; (void)gap_ms; return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 1; }
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) { (void)ticks; (void)timeout_ns; return 1; }
int mtl_snap_on(void) { return 0; }
uint64_t mtl_snap_acquire_wait(void) { return 0; }

// ---- DM_NVN_CAPTURE hooks (nvn_capture.inc is part of the Metal file in nxnative)
void nvn_cap_submit(int n, const uint64_t *handles) { (void)n; (void)handles; }
void nvn_cap_present(const tex_t *t, const int crop[4]) { (void)t; (void)crop; }

// ---- Everything else: statistics and tuning read by hle_nvn.c's reports, and the object notifications a real backend acts on
void mtl_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) { (void)f; (void)gpu_ms; (void)frame_ms; }
void mtl_alloc_stats(uint64_t *wired, uint64_t *gpu, int *n) { if (wired) *wired = 0; if (gpu) *gpu = 0; if (n) *n = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { if (enc) *enc = 0; if (reuse) *reuse = 0; }
void mtl_barrier_stats(uint64_t *skip, uint64_t *full) { if (skip) *skip = 0; if (full) *full = 0; }
uint64_t mtl_blit_splits(void) { return 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes) { if (load) *load = 0; if (store) *store = 0; if (full_rw_passes) *full_rw_passes = 0; }
uint64_t mtl_copy_cut_count(void) { return 0; }
unsigned mtl_cpu_tex_count(void) { return 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { if (lib_ns) *lib_ns = 0; if (pso_ns) *pso_ns = 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { if (lib) *lib = 0; if (pso) *pso = 0; }
void mtl_cshoist_stats(uint64_t *moved, uint64_t *depth, uint64_t stops[8]) { if (moved) *moved = 0; if (depth) *depth = 0; if (stops) for (int i = 0; i < 8; i++) stops[i] = 0; }
uint64_t mtl_cut_count(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
void mtl_geohoist_stats(uint64_t *draws, uint64_t *runs, uint64_t stops[6]) { if (draws) *draws = 0; if (runs) *runs = 0; if (stops) for (int i = 0; i < 6; i++) stops[i] = 0; }
void mtl_geomesh_stats(uint64_t *mesh, uint64_t *compute) { if (mesh) *mesh = 0; if (compute) *compute = 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0.0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { if (t0) *t0 = 0; if (t1) *t1 = 0; return 0; }
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { if (draws) *draws = 0; if (made) *made = 0; if (none) *none = 0; }
void mtl_lc_stats(uint64_t *taken, uint64_t *landed) { if (taken) *taken = 0; if (landed) *landed = 0; }
void mtl_mem_stat(unsigned long long frame, unsigned long long mb) { (void)frame; (void)mb; }
uint64_t mtl_pkg_edge_skips(void) { return 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { if (uploads) *uploads = 0; if (reuploads) *reuploads = 0; }
uint32_t mtl_present_qos(void) { return 0; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_pso_line(char *out, size_t n) { if (n) out[0] = 0; }
void mtl_queue_activate(uint64_t serial) { (void)serial; }
void mtl_queue_forget(uint64_t serial) { (void)serial; }
void mtl_reg_image(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_res_stats(char *out, size_t n) { if (n) out[0] = 0; }
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_sm_stats(uint64_t *passes, uint64_t *layers, uint64_t *waits) { if (passes) *passes = 0; if (layers) *layers = 0; if (waits) *waits = 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { if (passes) *passes = 0; if (draws) *draws = 0; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
void mtl_ubd_stats(uint64_t *deferred, uint64_t *cut) { if (deferred) *deferred = 0; if (cut) *cut = 0; }
void mtl_ubp_stats(uint64_t *partial, uint64_t *cached, uint64_t *refused) { if (partial) *partial = 0; if (cached) *cached = 0; if (refused) *refused = 0; }
void mtl_ubp_why(uint64_t why[5]) { for (int i = 0; i < 5; i++) why[i] = 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { if (call) *call = 0; if (skip) *skip = 0; }
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes) { if (resolve) *resolve = 0; if (reuse) *reuse = 0; if (set) *set = 0; if (skip) *skip = 0; if (copies) *copies = 0; if (copy_bytes) *copy_bytes = 0; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }

// ---- the NVN function table's glue
// nxnative's native-code thunks (nvn_thunks.S) are replaced by svc stubs in the guest window: a thunk is created the first time its
// name is looked up, once nvn_init_slots has filled the slot table.
void nvn_unimpl_entry(void) {}
void nvn_prof_entry(void) {}
void nvn_big_entry(void) {}
