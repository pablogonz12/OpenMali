# V13 next, round 2 (Poco, Mali-G925 arch 13.8)

Goal: flat triangle 8192 px, polylist_hi16 0x4, state_w0 0x1. Result: NOT reached. 7 hardware runs (14 total), all identical:
fence signals ~3 ms, no fault, clear colour 0x80402010 lands, 0 covered px, hi16 0, w0 0.

## Options added (t_triangle_v13)
- `--zsext`: 320 B FBD, CRC extension words 32..47, RT at 48..63, w12 |= 0xd0200000, ptr tag 3, 4 KB zeroed CRC buffer.
- `--cs-tctx`: tiler context built by the CS (3x STORE_MULTIPLE) from a private subqueue-ctx block (heap desc +0x30, geom buf +0x38).
- `--sb-prologue`: 5x SET_STATE_IMM32, NEXT_SB_ENTRY, LOGIC_OP32, SET_STATE before HEAP_OPERATION.
- `--heap-panvk` (1 MiB chunk, init 10, max 400), `--panvk-desc` (sampler 0x02a99901, z scale 1/offset 0).
- `--all-panvk` = all of the above (+ fbd w0 0x7000, dcd2 0x0c000000). Its CS matches PanVK draw instructions 4..27 word for word.
- JSON `triangle_flat.tctx_after` dumps the tiler context after the job.

## Runs (runs/20261116-poco-libkbase/v13-runs2/)
1-4 `--all-panvk` variants, 5 `--zsext`, 6 `--zsext --sb-prologue`, 7 `--zsext --cs-tctx`. All 0 px.

## Evidence
tctx_after: w2 0xc40000, w3 0xff00ff, heap/geom pointers intact, and the HW wrote the polygon-list head (w0/w1 = 0x6000101e80,
inside the heap chunk). Tiler runs but bins no polygon, so w1 hi16 = 0.

## Remaining ideas (6 runs left of cap, none used here)
1. VS side: PanVK VS FAU count 5 qwords (imm 1, c7000000, 47000000, 4) vs ours 7 with 0x10; VARY_SIZE 32 vs 16; reserved-6 FAU.
2. Shader BO VA zone: PanVK exec BO at 0x8000001000 (SPD VS), ours is a normal GPU_EX alloc; compare MEM_ALLOC_EX flags
   (0x380f/0x280f/0x1817/0x8200f) with src/bo.c.
3. Untested combos: `--cs-tctx --sb-prologue` without zsext, `--tf-or 0x40000` / `--vary 16` over `--all-panvk`, `--hsr`.
4. `--replay DIR`: PanVK descriptors/CS from poco-trace.jsonl.mem kicks 4-6 relocated; shader binaries are not in the trace
   (exec BO not CPU-mapped), so only with our shaders.
5. Then `--swap-X` bisect (one PanVK blob at a time).

## After the flat triangle passes (not started)
Bisect to the necessary toggle set and make it default; then port varying (--only 2), uniform, depth, texture.

## Replay of PanVK kick 4 (Poco, Mali-G925, uAPI 1.30)

Correction: `panvk-trace/poco-trace.jsonl.mem` DOES hold the exec shader BO (CPU 0x5fffe1a000, GPU VA 0x800000001000, flags 0x1817, 16 KiB).
`tools-replay/mkreplay.py` builds `runs/20261116-poco-libkbase/replay-kick4/`; `tests/replay_v13.h` adds `t_triangle_v13 --replay DIR [--swap-shaders] [--swap-spd] [--no-sync] [--plain-flags] [--exec-flags F]`.
Raw MEM_ALLOC_EX with the captured flags, MEM_SYNC after CPU writes, qword and MOVE32 relocation, both rings on two queues (vertex group 0, fragment group 1), our own tiler heap.
Success = AFBC header histogram (RT is AFBC, no linear pixel count).

| run | config | result |
|---|---|---|
| 1 | plain | wrong RT region checked, nothing seen |
| 2 | plain | no fault, clear only |
| 3 | plain | PASS: 256 headers = 216 clear 0x80402010, 24 solid ff996633, 16 edge (~8192 px) |
| 4 | --swap-shaders | PASS, identical to 3 |
| 5 | swap-shaders + RP_BREAK (our FS zeroed) | triangle gone (208 clear, 1 distinct) -> proves swap is live |
| 6 | swap-shaders + swap-spd | PASS, 16 distinct, covered 916 |

Conclusions
- Library layer (alloc, flags, queues, kick, heap) works on uAPI 1.30. Replay draws.
- Our VS/FS binaries, SPD words (identical to PanVK's), FAU layout, VARY_SIZE 32 and tiler flags 0x430008 all draw inside PanVK's CS/descriptors. They are NOT the culprit.
- Remaining suspects: our descriptors (RSD/FBD/draw/tiler/scissor/depth) and our CS ordering. `--swap-desc` was not implemented (run cap); next step is to put our descriptors into PanVK's CS one at a time.

Flags / VA comparison
- PanVK exec BO: 0x1817 (CPU_RD|WR, GPU_RD|EX, COHERENT_LOCAL 0x800, CACHED_CPU 0x1000), VA in exec zone 0x800000001000, mmap offset = GPU VA, then MEM_SYNC. Ours (src/bo.c): 0x17, no 0x800/0x1000.
- PanVK descriptors 0x380f, RT/arena 0x280f, sync 0x8200f; aliased 1 MiB zero BO via MEM_ALIAS (nr 21), referenced at subqueue ctx +0x20.
- PanVK FAU reserved 6 words, FS FAU holds blend words 4,5; VARY_SIZE 0x10, tiler flags 0x470008 (bit 18 secondary shader). Ours: 8 reserved, 32, 0x430008.

## SOLVED: flat triangle passes (Poco, 5 hardware runs of cap 8)

Root cause: descriptor/arena BO allocation flags. PanVK allocates every GPU BO with COHERENT_LOCAL (0x800) (descriptors 0x380f, arena 0x280f);
we allocated 0x200f (SAME_VA only). Without 0x800 the tiler bins nothing (hi16 0, 0 px). Descriptor CONTENTS were not the problem.

| run | config | result |
|---|---|---|
| 1 | `--all-panvk --exec-flags 0x1817` (exec BO 0x1817 at VA 0x800000001000 + MEM_SYNC ok) | 0 px: candidate (A) exec flags ruled out |
| 2 | + `--bo-flags 0x1800` (COHERENT_LOCAL + CACHED_CPU on arena/geo/hbuf + MEM_SYNC) | HANG: fence never signals, CPU_QUEUE_DUMP, tctx_after all 0. CACHED_CPU needs more (device->CPU invalidate, ring/sync handling) |
| 3 | `--all-panvk --exec-flags 0x1817 --bo-flags 0x800` | tiler works (hi16 0x4, w0 0x1) but 0 px: PanVK's zsext/AFBC-style FBD is wrong for our linear RT |
| 4 | `--bo-flags 0x800` only | PASS: 8192 px (0.0 %), hi16 0x4, w0 0x1, 0 bad inside/outside |
| 5 | no flags (new default) | PASS, same numbers |

Offline diff (no hardware) of PanVK kick 4 vs our dumps: blend, depth/stencil, SPDs, VS/FS FAU (modulo our shader layout), VS/FS driver sets, resource tables,
heap desc, subqueue ctx (+0x30 heap desc, +0x38 geom buf|16) are word-identical. Only differences: vertex buffer entry sizes (0x60 vs 0x30) and FBD RT words
49/50/58/59 (PanVK RT is AFBC, ours linear) plus pointers. So `--swap-desc` was not needed and was NOT implemented.

Changes
- `src/bo.c`: opt-in global `uint64_t kb_bo_extra_flags` OR'd into plain GPU_RD/WR allocations (not GPU_EX, not CSF_EVENT). Default 0: v11 unchanged (check.sh OK).
- `t_triangle_v13`: sets it to 0x800 by default (`--bo-flags F` overrides, `--bo-flags 0` = old behaviour); `--exec-flags F` (shader BO raw flags + MEM_SYNC).
  When non-zero, MEM_SYNC of arena/geo/hbuf runs before submit.
- Needed set: only 0x800. Plain options (no --all-panvk, no zsext/cs-tctx/sb-prologue/heap-panvk) suffice; those toggles were red herrings.

Next: port varying (`--only 2`), uniform, depth, texture to v13 with the 0x800 default; investigate CACHED_CPU (run 2 hang) only if wanted for parity;
consider making 0x800 the default for arch>=13 in kb_bo_alloc (src/bo.c) once the v13 library path is wired.

## Ladder port (varying, uniform, depth): 14 of 14 hardware runs used

| rung | result | notes |
|---|---|---|
| flat triangle | PASS | `--only 1` |
| varying triangle | PASS | 8192 px, interpolation error 0. Needs (a) FS compiled against the VS varying layout (LD_VAR_BUF_IMM, `SPIKE_LAYOUT_OUT/IN` in `compiler-spike/spike.c`, set by `gen-shaders.sh` for arch 13 only; no FS varying attribute descriptor, `--var-attr` restores it) and (b) tiler flags bit 18 (`secondary_shader`) set: now the default (`--tf-or 0` = off). Without bit 18 the varying draw gave 0 px 6/6 times; with it 3/3 cold + 1000/1000 loop. |
| uniform scene 1 (scale/offset/colour push constants) | PASS | 65,152 px checked, 0 bad; `t_uniform_v13` (default scene 1) |
| uniform scene 2 (two draws, one frame) | FAIL | draw 1 renders, draw 2 missing (area -50 %). Not fixed by CS_WAIT between draws, pixel-kill 2 (`--var 1/2/3` via `--sweep`). |
| depth (D32F / D24X8) | FAIL | 0 px, zs buffer = clear value, also with depth func ALWAYS (`--var 4`) and pk 2: points at the ZS-attached FBD / ZS path, not the compare. Opt-in: `qdirect poco t_depth_v13`. |
| texture | NOT PORTED | no hardware runs left; `shaders_v13.h` already has fs_tex (handle immediate 0x08000801). |

Loop stress: `t_triangle_v13 --loop 1000` (flat + varying per iteration): 1000/1000 pass, 1.5 / 2.1 / 4.0 ms min/mean/max. `--loop-reset-ctx` NOT run (cap).
IDVS deferred mode (`--idvs-mode 1`) faults the queue: do not use.

Next: capture PanVK (vk) doing a 2-draw frame and a depth frame on the Poco and diff against `t_uniform_v13 --only 2` / `t_depth_v13` (same `rung2_v13.h` CS); then port texture
from `t_texture.c` on the same harness; then `--loop 1000` and `--loop 1000 --loop-reset-ctx` on each passing test.

## PanVK capture + diff round (10 hardware runs this round, cap 14; 4 unused)

PanVK was run in the guest under `ioctl_trace.so` with `runs/20261116-poco-libkbase/panvk-trace2/vkdraw2.c` (modes `two`, `depth`, `tex`; traces `{two,depth,tex}.jsonl[.mem]`).
PanVK renders all three (two draws, depth with overlap, 8x8 texture). Decoders: `tools-replay/cs2.py`, `tools-replay/cmp3.py depth|two`.

Findings (PanVK vs ours):
- RUN_IDVS2 word is identical (`0x0c00000200000000`); between the two draws only FAU registers change. So the CS is not the draw-2 bug.
- Tiler flags `0x430008`, DCD0/DCD1, ZSD, blend, TSD, SPDs identical. PanVK depth DCD2 = `0x1f001000` (bits 24..28), no-depth `0x0f001000`; ours `0x03001000`.
- FBD w0 = `0x7000` (HSR bits 12..14); w12 = `0xf0260000` (bits 31:28); depth target is AFBC-tiled (block format 0xd), ours linear (block 2, valid per pan_desc.c).
- Opt-in `--var 256` applies the HSR set (w0 + DCD2 26/27/28): renders identically to baseline, fixes neither problem.
- `--var 16` ORs w12 31:28: **faults the queue** (also alone, with or without DCD2). Do not set.
- `--var 128` (no ZS attachment at all): depth still 0 px. So the ZS target is not the cause; colour of a depth-scene draw is missing even with func ALWAYS. pk 2 makes the ZS clear correct (zs_bad 7692 = drawn pixels only), colour still 0.
- uniform `--only 2` unchanged (2048/4096 px, draw 1 only).

Not done: texture port, `--loop 1000` / `--loop-reset-ctx` on new tests, `qdirect poco all` additions. Next ideas: diff the PanVK depth VS/FS shader and vertex setup against ours (our depth scene may differ in
more than the FBD), and look at per-draw tiler/geometry buffer state for draw 2 (PanVK allocates vertex-time buffers from the tiler heap per draw).

## SOLVED: two draws, depth, texture (round 4, 12 hardware runs of cap 18)

Root cause of BOTH failures (uniform draw 2 missing, depth 0 px): a test-harness bug in `tests/rung2_v13.h`, not the driver path.
`vs_uni` has `VS_UNI_FAU_COUNT 17` words (FAU reserved 16 + one promoted immediate at word 16 = 0x00000001), i.e. 9 qwords = 72 B, but the VS FAU block
was allocated with 64 B and the FS FAU block followed directly. Word 16 of the VS FAU was therefore word 0 of the FS FAU = the draw colour's red channel
(float bits). The VS only works when that word is odd:

| draw | red | word 16 seen by the VS | result before the fix |
|---|---|---|---|
| uniform scene 1 | 0.8 | 0x3f4ccccd | drawn |
| two-draw, draw 1 | 0.2 | 0x3e4ccccd | drawn |
| two-draw, draw 2 | 1.0 | 0x3f800000 | missing |
| depth near / far | 1.0 / 0.0 | 0x3f800000 / 0 | both missing (0 px) |

Found offline: the fake-backend dump of `t_depth_v13 --dump` showed `vs_fau` with 16 words and `fs_fau` at +0x40, while the CS gives the hardware a count of 9 qwords.
Fix: `R2_FAU_BYTES(words32)` sizes each FAU block from the qword count passed in `kb_draw_fau_ptr` (64 B aligned). No CS, descriptor, flag or FBD change.
Nothing from the earlier hypothesis list was involved (no state reset between RUN_IDVS2 calls, no ZS alloc flags, no HSR/FBD bits); `--var` toggles stay opt-in experiments.
`--replay`/`--swap-X` for the two-draw and depth traces were not built: not needed.
`check.sh` only rebuilt when `tests/<t>.c` changed, so edits to `tests/*.h` ran a stale fake binary; it now depends on `tests/*.h` too.

Texture: `tests/t_texture_v13.c` (port of t_texture.c: v13 register map, one VS SPD, FS linked to the VS varying layout, FS driver set = one zero sampler,
FS FAU words 4,5 = blend conversion, handle immediate at word 8). PanVK's Texture / Plane / Sampler words in `panvk-trace2/tex.jsonl` are identical to our v11 packers,
so no new packer was needed. Passed on the first hardware run.

| test (Poco, qdirect) | single | `--loop 1000` | `--loop 1000 --loop-reset-ctx` | us min / mean / max (loop) |
|---|---|---|---|---|
| t_triangle_v13 (flat + varying) | PASS | 1000/1000 | 1000/1000 | 850 / 2146 / 4172 |
| t_uniform_v13 (scene 1 + two draws, 4096 px, 0 bad) | PASS | 1000/1000 | 1000/1000 | 1500 / 2408 / 5003 |
| t_depth_v13 (D32F, D24X8, clear value; colour + ZS readback 0 bad) | PASS | 1000/1000 | 1000/1000 | 2579 / 3490 / 5888 |
| t_texture_v13 (65,536 px, 0 bad, max channel error 0) | PASS | 1000/1000 | 1000/1000 | 425 / 1062 / 2822 |

`qdirect poco all` = t_submit + the four tests above: 5/5. `t_uniform_v13` now runs all scenes by default. quick.py accepts a clean `loop_summary` as the verdict
(`--loop-reset-ctx` prints no `summary` record, which was reported as FAIL although 1000/1000 passed).

Open (not blocking): tiler flags bit 18 + VARY_SIZE 8 are still set for the varying-less uni shaders (PanVK uses 0x430008 / 0; ours renders correctly as is);
`kb_fb_pack` still prints the "v11 descriptors on arch 13: UNVERIFIED" warning although the v13 path is now validated; CACHED_CPU (0x1000) BOs still hang.
