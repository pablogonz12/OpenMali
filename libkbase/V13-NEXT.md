# V13-NEXT: draw path on Mali-G925 (arch 13), offline port

Status: written and compile-checked (native + aarch64, `-Wall -Wextra -Werror -pedantic`) offline. NEVER run on hardware.
Poco baseline (runs/20261116-poco-libkbase): `t_clear` and `t_submit` pass; every draw test faults at the first `RUN_IDVS`
(CS_INVALID_INSTRUCTION: opcode 6 no longer exists on v13).

Build: `./build-v13.sh [native|aarch64|all]` -> `out/t_triangle_v13-{native,aarch64}`. Run: `t_triangle_v13 [--dump] [--only 1]`.

## New files (all additive; v11 files untouched)
`include/kbase_cs_v13.h`, `include/kbase_draw_v13.h`, `src/draw_v13.c`, `tests/t_triangle_v13.c`, `tests/shaders_v13.h`, `build-v13.sh`.
Shared edits (v11 output verified byte-identical): `driver/compiler-spike/gen-shaders.sh` (args `[out.h] [arch]`), `gen_header.py` (env `GEN_ARCH` comment).
Mesa paths below are under `driver/third_party/mesa/src/panfrost/`; `v13.xml` = `genxml/v13.xml`, `cmd_draw` = `vulkan/csf/panvk_vX_cmd_draw.c`.

## Field table (V = verified in genxml/PanVK source, G = guess, no hardware evidence)
| Item | Value | Source | |
|---|---|---|---|
| RUN_IDVS2 opcode | 12; flags_override [31:0]=0, malloc_enable bit 33, vertex_shading_mode [36:35]=0 Early, draw_id [47:40]=0, stage [51:48]=0 | v13.xml:825-833, :554 | V |
| Progress Increment bit 32 | not set | PanVK does not set it | V |
| IDVS register map | SRT 0/2, FAU 8/10, SPD 16/18, TSD 24/26, 32..37 counts, TILER_CTX 40, SCISSOR 42, VIEWPORT_HIGH 44, VIEWPORT_LOW 46, INDEX_BUFFER 48, INDEX_BUFFER_SIZE 50, VARY_SIZE 51, ZSD 52, BLEND 54, TILER_FLAGS 56, DCD0/1/2 57/58/59, LINE_WIDTH 60, TILER_FLAGS2 61, OQ 64 | v13.xml:1451 | V |
| VERTEX_SPD only | no secondary/varying shader: `VS_SECONDARY_ENABLE 0` | shaders_v13.h (spike) | V |
| TSD r24 and r26 | both = Local Storage descriptor | cmd_draw:772-776 | V |
| Viewport r44:45 / r46:47 | {minx,miny,maxx,maxy} u16, {min depth, max depth} f32 | v13.xml:2272-2279, cmd_draw:869-948 | V |
| Scissor r42:43 | still set on v13 | cmd_draw prepare_vp | V |
| flags_override | 0 (non-indexed) | cmd_draw:3085-3190 | V |
| Tiler flags r56 | Triangles 8 \| bit16 \| bit17 \| FIFO basic 1<<22; no secondary bit 18, no point-size bits | cmd_draw:2791-2830 | V flags / G bit positions carried from v11 |
| Tiler ctx word 2 | hmask [12:0], first provoking bit 18, **Effective Tile Size [23:20] log2 (new)** | v13.xml:2633-2637 | field V, value 10 = G |
| Tiler ctx other | FB w/h-1 word3, layers word4, heap w6, geom buf w8 | v13.xml:2638-2644 | V |
| SPD | word0 type 8 / stage<<4 / frag coverage bit 8 / regalloc [31:30]; word1 preload [15:0], **Max Warps [31:16] (new)**; v11 warp-limit bits removed | v13.xml:2090-2102 | Max Warps=0 is G (`--max-warps N`) |
| ZSD | word4 = cull 22 \| clamp 23 \| fixed-fn depth 25 \| write 27 \| func 29; **bit 21 removed** | v13.xml:2678-2705 | V |
| DCD0 | unchanged from v11 (scissor-to-bbox 23, cull zero area 26, front CCW 16, pk/zs 2-5) | v13.xml:2712-2736 | V |
| DCD1 | sample mask \| RT mask (12 bits) << 16 | v13.xml:2738 | V |
| DCD2 | read \| write<<12 \| no-sh-stencil 24 \| no-sh-depth 25; HSR bits 26..31 = 0 by default | v13.xml:2753-2763 | V |
| HSR (opt-in `--hsr`) | FBD word0: filter b13, hier b14, interleave b12 only if ZS fits (z_tile_buf_budget unknown -> 0), prepass b11=0; DCD2 can_cull/can_be_culled b26/27 = 1 | pan_desc.c:1573-1587, cmd_draw:2600-2650 | bits V, values G |
| FS varying attribute | as v11 (type 6 vertex packet, table 61, offset 1024+16, stride 32) but **buffer_index = 1** | cmd_draw:504 | V (varying test only) |
| FBD | `kb_fb_pack` unchanged: Effective Tile Size 10 in word 11[12:9], tiler ptr w14:15; passes in `t_clear` | fb.c | colour V (hardware), tiler/HSR in a draw untested |
| Blend, Buffer, Resource, FAU ptr, tiler heap desc | unchanged | | G (not diffed against hardware) |
| Shading mode | Early; Defer Mode default per genxml not touched | | G |
| VS binary | `shaders_v13.h`: VS_FAU_COUNT 13, reserved 8, imm words 8..12, VARY_SIZE 32; position/varyings stored via hardware buffers (LEA_BUF_IMM table 13), no driver binding | spike | G |

Not ported: `kb_cs_add_imm64` (v13: ARITH_IMM64 op 17 + sub-opcode ADD_IMM64 bits 39:32), not used by the test. Scratch regs moved to r80+ because r64:65 = OQ.

## Hardware test order (Poco; run each, stop at first failure, keep the jsonl)
1. `./t_clear` (regression: FBD path still passes).
2. `./t_triangle_v13 --dump --only 1` default (HSR off, tile log2 10). Expect `triangle_flat ok:true`, flat colour (51,102,153,255).
3. If CS fault: `--tile-log2 6` (then 5, 7, 8); tiler ctx tile size is the main guess.
4. If hang/no output but no fault: `--max-warps 1`, then `--hsr`.
5. Intermittents: `--arena-rewind`, `--no-flush-wait`, `--settle-us N` (INTERMITTENT.md) only after a first pass; then `--inner N` style loop via loop.h.
6. Then varying (`--only 2`, needs vertex varying buffer; untested) and uniform/depth/texture ports (shaders exist, tests not ported).

## If the first draw still fails: capture with PanVK
Run a trivial `vkdraw` (one triangle, no depth, RGBA8 256x256) on the Poco under `driver/tools/ioctl_trace.c`
(LD_PRELOAD). Capture: (a) the CS buffer dump of the draw (compare register order and values vs `--dump` `cs_dump`);
(b) Tiler Context words 0..15 (Effective Tile Size, hierarchy mask, geometry buffer size); (c) FBD words 0..16 (HSR bits,
word 11/12, tiler pointer); (d) both SPDs (word1 Max Warps, word0 flags); (e) ZSD, DCD0/1/2 register values; (f) the fault
notification text from our run (`drain_events` prints `text`/exception; first run did not capture it).
Diff each against the descriptors we dump; the first mismatching guess row above is the suspect.

## Poco hardware results (2026-11-16, 12 runs, runs/20261116-poco-libkbase/v13-runs/, PanVK reference: runs/20261116-poco-libkbase/panvk-trace/)
- **Fault decode fixed** (test only): `kb_fence_wait` read and discarded the fatal group notification (kb_event_read inside the wait), so `drain_events` found nothing. t_triangle_v13 now uses `wait_log()` which prints every notification. Struct layout is fine on 1.30: it decoded `group 0 FATAL: TRANSLATION_FAULT (0x7002c2, sideband 0x5fe428d040)` when FBD w12 bit 21 (ZS/CRC ext) was set without the extension.
- **RUN_IDVS2 (op 12) executes with NO fault.** Fence signals in 0.7-2.2 ms, clear colour 0x80402010 lands, triangle covers 0 px (every run, 11 variants). Not yet 8192.
- PanVK vs ours (decoded from the Poco trace, vertex chunk 0x5fff35e000): IDVS register values/opcode identical (RUN_IDVS2 0x0c00000200000000, r42..r47, r33..r37, DCD0 0x0481000b, DCD1 0x1ffff, ZSD, blend ptr|1, SPD words, driver sets, blend desc 0xa00/0xf0122122/0x19/0xed000, tiler heap desc 0x29/size/chunk/+64/end all match).
- Differences found (applied via options): **Effective Tile Size 12 (64x64) in BOTH tiler ctx w2[23:20] and FBD w11[12:9], colour alloc w11[31:24]=0x10 (we had 10/0x04), hmask 0 (we 0xfe)** (now defaults); tiler flags r56 = 0x470008 (bit 18 Secondary Shader; `--tf-or 0x40000`); VARY_SIZE r51 = 0x10 (`--vary 16`); FBD w0 0x7000 (`--fbd0-or`), DCD2 0x0f001000 (`--dcd2-or 0x0c000000`); FBD w12 0xf0210000 + FBD pointer tag 3 + 64-byte ZS/CRC extension at FBD words 32..47, RT descriptor at word 48 (ours: RT at 32, tag 1); **FS FAU words 4,5 = RT conversion descriptor (0x19, 0x000ed000)** (now default), VS FAU z scale 1.0/offset 0 and reserved words 6 (ours 8, own compiler layout). Subqueue ctx [+0x30..] gives heap=0x5ffffa4000 (tiler heap desc BO), geom buf 0x5ffffa5010: same scheme as ours.
- None of these produced pixels (run list: default; tile 6/hsr/max-warps 1/reqzero+nofpk with the old FBD/tiler tile mismatch; tile 12+hmask 0; all-PanVK flags (fault from CRC bit w/o ext); tf+vary; +fbd0/dcd2 HSR; +FS FAU blend words).
- Not yet tried: build the ZS/CRC extension (tag 3, RT at word 48, CRC enable) exactly like PanVK; compare our VS/FS binaries with PanVK's (exec BO is not CPU-mapped so the trace has no code; dump it with a kbase MEM read or compile PanVK's vkdraw shaders with the same offline compiler and diff); PanVK-style CS-built tiler context (LOAD_MULTIPLE from subqueue ctx + STORE_MULTIPLE); the scoreboard prologue (r66/r67 STORE to subqueue ctx +0x70, NEXT_SB_ENTRY) before RUN_IDVS2; initial chunks 10/max 400 for the heap.
- Options added: `--tf-or --vary --fbd0-or --fbd12-or --dcd2-or --dcd1`. Varying triangle (--only 2) not attempted.
