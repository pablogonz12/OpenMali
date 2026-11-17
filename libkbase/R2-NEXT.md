# R2-NEXT: uniforms (FAU) and depth test (written 2026-11-16, OFFLINE: nothing here has run on hardware)

Tests: `out/t_uniform-aarch64`, `out/t_depth-aarch64` (build: `./build.sh`; shaders: `../compiler-spike/gen-shaders.sh`; harness: `tests/rung2.h`).
Paths below are relative to `driver/third_party/mesa/src/panfrost` (`v11.xml` = `genxml/v11.xml`).

## 1. What was added and where it comes from
| Item (our code) | Value | Derived from |
|---|---|---|
| spike: `nir_lower_explicit_io(push_const, 32bit_offset)` + `push_const_addr_format` | API push constants become `load_push_constant` | `vulkan/panvk_vX_shader.c:884` |
| spike: `SPIKE_FAU_RESERVED`, `SPIKE_PUSH_BASE` | VS reserved 16 words, user push block at byte 32; FS reserved 8, base 0 | `panvk_vX_shader.c:1037` (`fau.reserved = total*2`); byte->FAU word mapping `compiler/bifrost/bifrost_compile.c:886-915` (`word = base>>2`, assert `<= reserved`); **base 32 is our layout choice (guess)**: words 0..5 are viewport sysvals from spike's `lower_sysvals` |
| FAU table (`kb_draw_fau_*`) | VS 16 words: viewport (sx sy sz ox oy oz) at 0..5, push {scale.xy, offset.xy} at 8..11, z at 12; FS 8 words: colour at 0..3 | compiled uniform indices seen in the disasm (VS: `u4`=words 8,9 scale, `u5`=10,11 offset, `u6`=12 z, `u0..u2`=viewport) |
| FAU pointer regs r8:9 / r12:13 | `fau_va | (words/2)<<56`, written with two MOVE32 | existing `kb_draw_fau_ptr`; PanVK FAU count in 64-bit entries (PANVK-SEQUENCE.md item 11) |
| ZSD: Depth function bits 4:29, Depth write enable 4:27 | LESS = 1, write 1 | `v11.xml:2393,2395`, enum Func `v11.xml:77`; PanVK `csf/panvk_vX_cmd_draw.c:2323-2325`. Other bits unchanged from the working `kb_draw_zsd_off` (4:21 sep. dependency, 4:22 depth cull, 4:23 clamp [0,1], 4:25 source) = `panvk_vX_cmd_draw.c:2310-2318` for PAN_ARCH 11 |
| DCD0 for the depth draws | pixel kill 0, ZS update 0 (force early) | `lib/pan_earlyzs.c` analyze(): no discard/side effects, zs_always_passes false -> `best_early_mode` = Force Early; v11 forces early update. Differs from t_triangle (pk 2 = weak early) because the depth test does not always pass |
| FBD Z fields | 12:16 Z Internal Format (D16 0, D24 1, D32 2); 12:18 Z Write Enable; 12:21 Has ZS CRC Extension; word 13 Z Clear (float bits) | `v11.xml:1986,1987,1990,1995`; `lib/pan_fb.c:932-950` |
| ZS/CRC extension (64 B at FBD+128, RT moves to +192) | word 0: write format (D32 = 14, D24X8 = 4, D16 = 1) bits 3:0, block format Linear 2 bits 7:4, bit 31 Clean Tile Write; w8:9 base; w10 row stride (27 b) + surface stride hi (31:27); w11 surface stride | `v11.xml:2008-2019` (ZS Target), ZS Format enum `v11.xml:1887`, `lib/pan_desc.c:368-384` (linear), `lib/pan_fb.c:479-545` (clean tile write) |
| FBD pointer bit 1 | `kb_fb_pointer_ex(.., zs_ext=1)` = FBD | 1 | 2 | `lib/pan_fb.c:985` (`zs_crc_extension_present`); PANVK-SEQUENCE.md item 12 (`FBD|3`) |
| Depth clamp regs r44/r45 | still 0 / 1.0 (VS z viewport: scale 1, offset 0, i.e. Vulkan [0,1]) | unchanged; z written to depth buffer = push `z` |

**Guessed / unverified (no Mesa line):** VS with no varying has `secondary_enable = 0`, so prim flags bit 18 = 0 and the VARY SPD is still set (pointing at vs+256, unused); PanVK always writes VARY_SPD (`panvk_vX_cmd_draw.c:2186`) and sets `secondary_shader = secondary_enable && fs` (`:2806`), but we have never run a VS without the secondary shader. `VARY_SIZE` = compiler `generic_size_B` = 8. D24X8 byte layout (which 24 bits hold depth) is not documented; the test tries both. Clean Tile Write for ZS = 1 is our choice (PanVK uses `ct.zs`). Z Clear stored as float even for D24/D16.

## 2. Hardware test order (one step per run, stop at first failure; `--dump` to capture descriptors)
1. Regression: `t_clear`, `t_triangle` (both must still pass, unchanged: FBD without ZS is byte-identical, 192 B).
2. `t_uniform --only 1` (VS+FS uniforms, no depth, single draw). If the VS-without-secondary path hangs, that is the guess above: compare `cs_dump` r18/prim flags.
3. `t_uniform --only 2` (two draws, two FAU tables, one tiling/fragment pass).
4. `t_depth --only 1 --zsnostore` (depth test inside the tile, no write-back: colour only, isolates ZSD + DCD + FBD Z fields from the ZS target).
5. `t_depth --only 1` (adds ZS extension write-back of D32F).
6. `t_depth --only 3` (clear depth 0.5), then `--only 2` (D24X8).
7. Experiments if 4 fails: `--pk 2 --zs 0`, `--pk 3 --zs 3` (late), `--func 7` (Always: far triangle must then win the overlap: proves the depth function reaches the hardware; expectation follows --func).

## 3. Expected pass values (256x256, clear colour 0x80402010, viewport 128 + 128*ndc)
`t_uniform` scene 1: triangle (128,64) (192,64) (160,128), area 2048 px, colour R204 G102 B51 A255 = 0xff3366cc, tolerance +-1/channel, area within 2 %.
Scene 2: left (32,96) (96,96) (64,160) = 0xffff9933 (R51 G153 B255); right (160,96) (224,96) (192,160) = 0xff0080ff (R255 G~128 B0).
`t_depth`: near red 0xff0000ff (z 0.25) vertices (57.6,76.8) (160,76.8) (108.8,179.2); far green 0xff00ff00 (z 0.75) vertices (96,76.8) (198.4,76.8) (147.2,179.2) (both approximate in px; the test computes them exactly).
- scenes 1, 2: overlap = red (class bit pattern 0b11 has `class_bad` 0), far-only region green, depth buffer: near 0.25, far-only 0.75, elsewhere 1.0 (D32F within 2e-3; D24X8 reported `zs_decode_mode` 0 = low 24 bits, 1 = high 24 bits).
- scene 3 (clear 0.5): far-only region shows the clear colour 0x80402010, overlap red, depth there 0.25, far-only/elsewhere 0.5.
- Failure signatures: overlap green -> depth test not applied (ZSD/DCD/FBD Z); depth buffer unchanged 0xa5a5a5a5 -> ZS target not written (ext pointer bit, Z Write Enable, block format); everything clear -> hang/fault path (see `fault_state`).

## 4. Hardware results (CMF, 2026-11-16, 1 run each, qdrv, default opts)
All PASS first try, no encoder changes: t_clear (11 rec), t_triangle (7), t_uniform (scene 1: 2048 px, area err 0%, bad 0; scene 2: 4096 px, bad 0), t_depth (D32F scene 1, D24X8 scene 2, clear 0.5 scene 3: covered == analytic, bad 0, zs_bad 0, class_bad 0; D24X8 decodes as zs_decode_mode 0 = low 24 bits). Guesses confirmed: VS without secondary shader, FAU base 32, Clean Tile Write, force-early (pk 0, zs 0). Raw: runs/20261116-cmf-libkbase/t_*-run1.jsonl. 4 of 10 hardware runs used.
(Part B zero-copy result: see README.md, "Part B".)
