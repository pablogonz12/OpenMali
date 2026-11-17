# GPU-side gap, ours vs vendor (MaliBridge) on Poco X8 Pro Max (G925 MC11), 2026-11-17

Baseline (BENCH-poco-20261117.md): gpu_ms ours/bridge: cube 0.84/0.17, drawcalls 1.95/1.04, overdraw 2.06/0.84, alu 10.83/5.77, tex 2.24/0.69, compute 1.74/0.85.
Note: "bridge" is the vendor driver; PanVK is the structural oracle (third_party/mesa panvk sources).

## Findings from reading our CS/descriptors vs PanVK (offline)
1. Compute: we never set `COMPUTE_SIZE_WORKGROUP.allow_merging_workgroups` (bit 31; PanVK sets it via `valhall_can_merge_workgroups`) and always used
   task increment 1 on axis X (PanVK: `calculate_task_axis_and_increment`, ~threads_per_task = max_threads/max_tasks_per_core). Both implemented
   (commit "compute allow_merging_workgroups + panvk-style task axis"); env A/B knobs `MALI_NO_MERGE=1`, `MALI_TASK_THREADS=<n>` (default max_threads/4).
2. Every graphics frame and compute job starts with FLUSH_CACHE2(L2 clean+inv, LSC clean+inv, other inv) + WAIT (mali_cs_emit.c `MALI_HF`, default 0x332;
   compute head always (3,3,2)); PanVK does no full cache clean/invalidate between jobs. CMF measurement: MALI_HF=0 MALI_NOTF=1 saved 0.35 ms on cube.
   gpu_ms (timestamps around the CS) includes these.
3. Render targets/depth are linear, no AFBC (CMF: gpu_ms scales with pixels). WSI accepts LINEAR modifier only (mali_wsi_modifier).
4. Fragment path: FBD always Clean Tile Write, colour always stored; tile size/hierarchy: hmask 0 on v13, tile_log2 12 (libkbase default).

## Experiments (Poco, mdbench compute, ours, mailbox, 8-10 s, 1 run; noise ~+-0.1 ms; temp 44-46 C)
| config | gpu_ms |
|---|---|
| NO_MERGE=1 TASK_THREADS=1 (= old behaviour) | 1.76 |
| merge on, inc default (512 thr) | 1.75 |
| merge off, inc default | 1.34 |
| merge on, inc 1 | 1.39 |
(non-additive: needs repeats, see below)
