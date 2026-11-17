# t_triangle: next steps after r17 (0 / 8192 px covered)

Offline diff of our r17 run (`runs/20261115-cmf-libkbase/triangle-r17.jsonl`) against PanVK's working capture
(`runs/20261115-cmf-libkbase/panvk-trace/draw-flat.{jsonl,mem}`), Mali-G615, arch 11.8. Nothing here has run on hardware yet.

## What r17 tells us

- Fence signals, no fault, clear colour is correct everywhere: the fragment path, FBD and RT writeback work.
- Our tiler context after the run: w0:1 = `0x0000_0060_00201a80`, private state (w24..31) all zero, completed top/bottom zero.
- PanVK's tiler context after its run: w0:1 = `0x0004_0060_00101a80`, private state w24 = 1.
- Both polygon-list pointers sit 0x580 below the top of the first chunk, so our tiler ran and allocated the same structure,
  but it recorded nothing. **The triangle is lost before binning** (vertex fetch, position shading, clip or cull), not in the
  fragment shader or blend.

`t_triangle` now prints `tiler_polylist_hi16` and `tiler_state_w0` in every result line. PanVK values with one binned triangle
are `0x4` and `0x1`; r17 had `0x0` and `0x0`. (What these two fields mean is inferred from this one comparison only.)

## Fixed in the encoders (r18 defaults)

Ranked by how likely each one explains zero binned primitives. Each has a `--revert` bit to undo it alone.

| # | Fix | Ours (r17) | PanVK | Revert bit | Where |
|---|-----|-----------|-------|-----------|-------|
| 1 | Attribute pixel format: low 12 bits are the v7+ "RGB Component Order" enum (RGBA = 0), not the v6 swizzle 0x688 | attr w0 `0x2fda2115` | `0x2fc00115` | 1 | `KB_PIXFMT_*` in `include/kbase_draw.h` |
| 2 | IDVS TSD_0 (r24:25) never set; PanVK points it at a Local Storage descriptor (w1 = 0x1f, rest 0) | r24 = 0 | `MOVE48 r24 = LS desc` | 2 | `kb_draw_local_storage_none`, `KB_IDVS_TSD` |
| 3 | Blend w0 lacks "round to FB precision"; w3 conversion format was the pipe format with the v6 swizzle | `0x200`, `0xbb688` | `0xa00`, `0xed000` (RGBA8_TB) | 4 | `kb_draw_blend_opaque_rgba8` |
| 4 | DCD0: front face CCW (bit 16) missing, ZS update weak-early instead of force-early | `0x0480002b` | `0x0481000b` | 8 | `kb_dcd_opts.front_ccw`, default `--zs 0` |
| 5 | Varying SPD warp limit Half instead of Full | `0x80000138` | `0x80000038` | 16 | `kb_draw_spd_vs_varying` |

Notes on the ranking:

1. This is the only difference in the whole VS driver set (attributes 0/1, fillers, both Buffer descriptors are otherwise
   bit-identical). 0x688 is not a value of the v11 component-order enum, so the position fetch is undefined. Garbage or zero
   positions give a clipped or zero-area triangle, which matches "tiler ran, binned nothing". It also affected the FS varying
   attribute and the blend conversion format.
2. Certain difference, uncertain effect: r24 = 0 raised no fault in r17, so the hardware may not dereference it when TLS size
   is 0. Cheap and what PanVK does.
3. Cannot explain zero binned primitives (fragment side), but the old w3 was an invalid format and would likely have broken
   colour output once fix 1 landed.
4. Both cull bits (17, 18) are clear on both sides, so CCW only changes front/back classification. Unlikely to matter here.
5. Only affects the varying shader. Unlikely to matter for `triangle_flat`.

## Checked and not a bug

- **r48 varying size 0x20 vs PanVK 0x10.** Different shaders: PanVK's VS FAU has the clamp immediates at words 6/7 and no
  offset immediate; ours (compiler-spike, `pan_build_varying_layout_compact`) has them at 8/9 plus word 10 = 0x10, the offset
  of the colour varying inside a 32-byte packet. Size 32, VS store at +16 and the FS attribute offset 16 are self-consistent.
- **r58:59 "tiler ctx" in PanVK's fragment chunk** is a scratch register for the LOAD_MULTIPLE before FINISH_FRAGMENT. Our r74
  does the same job.
- **FBD pointer tag 1 vs 3.** PanVK has a ZS/CRC extension block, we do not. Tag 1 is consistent with our FBD.
- **Tiler context built on the CPU** instead of by STORE_MULTIPLE: the resulting words (w2, w3, heap, geometry buffer) match.
- Matching bit-for-bit: RUN_IDVS flags, r32..38, r42/43, r44/45, r50, r52, r56, r58, SRT/FAU pointers, position and fragment
  SPD, ZSD, vertex Buffer descriptors, tiler heap descriptor layout.

## Remaining hypotheses if r18 still covers 0 px

1. **Our shader binary or FAU layout is wrong for this draw** (shader bytes were not in the PanVK capture, so not diffed).
   Viewport: ours scale/offset z = 0.5/0.5, PanVK 1.0/0.0. Both land inside the [0, 1] depth clamp, but swapping to PanVK's
   values is a one-line test (`vsfau[2] = 1.0f; vsfau[5] = 0.0f`).
2. **Tiler context must be written by the CS** (PanVK zeroes it and fills it with three STORE_MULTIPLEs, masks 0xffff @0,
   0xfc03 @64, 0xfc03 @96, right before RUN_IDVS). Would matter only if the firmware latches it at a point our CPU write misses.
3. **Single queue vs PanVK's separate vertex and fragment queues**, with HEAP_SET and REQ_RESOURCE in the ring prologue rather
   than the chunk.
4. **r48 / varying layout** after all: test with a VS that has no varying (`pos.vert`) and r48 = 0.

## Hardware test plan

Build: `./build.sh aarch64`, binary `out/t_triangle-aarch64`.

**Test 1 (first, single run):** `t_triangle --only 1 --dump`

- Expected: `triangle_flat` ok, `covered_pixels` 8192 (within 2 %), inside samples `0xff996633`-like flat colour
  (R 51, G 102, B 153, A 255), outside samples = clear value, `tiler_polylist_hi16` = `0x4`, `tiler_state_w0` = `0x1`.
- If `tiler_state_w0` = 1 but coverage is still 0: binning now works and the problem moved to the fragment side. Go to test 3.
- If `tiler_state_w0` = 0: binning still fails. Go to "remaining hypotheses" 1, then 2.

**Test 2 (attribution, only if test 1 passes):** `t_triangle --only 1 --revert 1`

- Expected: back to 0 px with `tiler_state_w0` = 0, proving the pixel format was the cause. If it still passes, run
  `--revert 2`, then `--revert 8`, to find which fix mattered.

**Test 3 (only if binned but not shaded):** `--revert 4`, then `--nofpk`, then `--pk 0`. One at a time, compare coverage.

**Test 4 (after flat passes):** `t_triangle --only 2` for the interpolated triangle. A failure here with flat passing points
at r48 / the varying attribute (packet stride, offset 16) or the varying SPD.

## Hardware result 20261116 (CMF, supersedes "nothing has run")
Test 1 PASS (8192 px, w0=0x1, hi16=0x4, flat 0xff996633). Test 2 `--revert 1`: 0 px, w0=0x0 -> fix 1 (attribute pixel format) is the cause; other fixes not needed to bisect.
Test 4 PASS: interpolated max err 0/255, ~31 ms submit->signal. Tests 3 and the fallback list were not needed.

## 2026-11-16: "triangle_varying fails after triangle_flat" NOT reproduced (CMF <serial>)

Offline: both draws share one ctx, queue, tiler heap (`kb_tiler_heap_create` once), geo BO, arena (reset to 0, so every descriptor
lands on the same VAs) and sync BO. Draw 2 re-writes the heap descriptor (`kb_draw_tiler_heap_desc`, first_chunk_va) and the tiler
context on the CPU. Only the render target and the CS are new. Untested latent hazards if the failure returns: the CPU rewrite
of the heap descriptor at the same VA after the firmware advanced it, and arena reuse while the GPU may still hold cached lines.
Hardware (9 runs, full runs `t_triangle` x5, t_clear/uniform/depth/texture x1): ALL PASS, both draws 8192 px, polylist 0x4, state 0x1.
No cause established and no code changed; the earlier failure may have come from a stale binary or a transient state.
Raw: runs/20261116-cmf-libkbase/triangle-full-rerun{1..5}.jsonl, t_{clear,uniform,depth,texture}-rerun.jsonl.
If it recurs: bisect with a fresh heap per draw, then a per-draw geo BO, then a non-reset arena.

## 2026-11-16 stress loops (CMF <serial>): failure REPRODUCED, rate ~0.4 %/iteration
New harness `tests/loop.h`: `--loop N`, `--loop-reset-ctx`, `--loop-max-fail K` (default 3), `--dump-on-fail` (failures always dump). t_triangle also got bisect knobs
`--fresh-heap`, `--fresh-geo`, `--arena-adv` (do not reset the arena to 0 per draw; wraps at 32 KB). Raw: runs/20261116-cmf-libkbase/loop-*.jsonl.
Failure signature (any draw of an iteration >= 1, flat or varying, 3 of 5 in the flat draw): fence signals normally (~0.7 ms), no queue fault, no notification,
0 px, `tiler_polylist_hi16`=0, `tiler_state_w0`=0, target = clear value. i.e. the tiler/IDVS did not bin, same as the pre-fix attribute-format bug, but descriptors are byte-identical to passing iterations.

| run (t_triangle, 2 draws/iter) | iters | fail |
|---|---|---|
| default, max-fail 3 | 244 | 3 (iters 13, 15, 243) |
| default | 300 | 2 (148, ...) |
| default | 600 | 0 |
| `--fresh-heap` | 300 | 2 |
| `--fresh-geo` | 300 | 7 |
| `--arena-adv` | 300 + 600 | 0 + 0 |
| `--loop-reset-ctx` | 300 | 0 |
t_uniform 200, t_depth 200, t_texture 200: 0 failures each.
Default total 5 failures / 1144 iterations; arena-adv 0 / 900 (p ~ 0.02 if same rate). Failing runs have slower mean submit->signal (2.4-3.3 ms vs 1.2-1.5 ms in clean runs), so it correlates with GPU/system load or clock state.
Hypothesis (not proven): the draw reuses the same arena VAs (descriptors, tiler ctx, FBD, CS) every draw; with a stale GPU/firmware cache or a CPU write still in flight the old tiler context/state is seen. Fresh heap/geo do not help; non-reused arena does.
Next: make `kb_arena` advance by default (or add a cache-maintenance step/barrier before submit), then re-run `--loop 600` x3; also try `--arena-adv` under load (Megabonk running).
