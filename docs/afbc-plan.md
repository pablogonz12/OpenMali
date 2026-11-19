# AFBC plan (colour first)

Status: flag `MALI_AFBC=1` exists, default off, nothing advertised, no driver behaviour change. Hardware gain unproven
(0.1-0.3 ms on cube; the swapchain stays LINEAR, so only app-internal targets benefit). Research notes: `/tmp/oc/afbc.md`.
Hours are focused lane-hours including tests. Total about 55-60 h; colour-only milestone (stages 0-4 + minimal 6) about 30 h.

| # | Stage | h | Test |
|---|-------|---|------|
| 0 | Layout maths + flag (`mali_afbc.h`, PR #169). Add a Mesa `pan_afbc.h` cross-check over ~200 random (fmt, mod, w, h) | 1 | `t_afbc` host (hand-derived values, overflow cases); cross-check compares sizes to Mesa |
| 1 | Eligibility `mali_afbc_image_eligible` (this PR): optimal, 2D, 1 sample, no STORAGE/HOST_TRANSFER/DISJOINT/ALIAS/SPARSE, colour table, >=16x16 | 2 | `t_afbc`: one case per reject reason, format hit/miss table |
| 2 | Image layout and alloc: host part DONE (`mali_afbc_image_layout`: mips + layers, #stacked PR); sizing wired in `mali_image.c` (`mali_image_afbc_apply`, flag on only; size/alignment = max(linear, AFBC), linear planes untouched, no modifier exposed); remaining: real AFBC subresource layout and bind. Default off | 4 | `t_image` golden sizes with flag on and off; flag off must be byte-identical |
| 3 | RT descriptor packing (v10/v11/v13), compression-mode table, `reverse_issue_order` when tile < superblock. Host part DONE (`mali_afbc_rt_words`/`mali_afbc_rt_patch`, hand-derived goldens from v13.xml); v10/v11 packing not cross-checked | 5 | golden descriptor bytes vs genxml on both fake GPUs |
| 4 | Colour attachment end to end: clear via RT clear colour (not a linear memset), LOAD preload with AFBC plane descriptor, store. First phone run | 8 | fake-backend command-stream goldens; phone: solid + noise RT read back through graphics blit, byte compare, 100-frame loop |
| 5 | Sampled reads: AFBC plane descriptor in descriptor/sampler code; view format must keep the same AFBC mode | 6 | descriptor goldens; phone sample test |
| 6 | Copies, blits, clears, resolves on AFBC images via graphics (compute cannot touch AFBC) | 12-16 | CTS copy/blit/clear subsets; dominant risk |
| 7 | Depth/stencil AFBC: ZS descriptor, 16x16 only, separate-layouts question | 8 | ZS descriptor goldens; phone depth test |
| 8 | Hardening: CTS image and dynamic-rendering subsets, DXVK games, hang soak. WSI modifiers are separate and compositor-dependent | 8 | farm CTS, A/B gpu_ms and image diff with MALI_AFBC=1 vs 0 |

Rules: nothing is switched on by default before stage 4 passes on hardware; phone runs go through the farm hold and stay
below 43 C; a bad descriptor means a GPU fault, so descriptor stages need golden tests first.

## Progress and what is left before a phone test

Spent so far (agent hours, approximate): stage 0 helpers 1, stage 1 eligibility 1.5, stage 2 layout + sizing wiring 2.5, stage 3 host
fields + env call-site test 2. About 7 of the 55-60 h; all host-only, flag default off, nothing advertised.

Needed before the first hardware test (smallest useful: solid-colour RT written as AFBC, read back):
1. Emit the AFBC RT: call `mali_afbc_rt_words` from `mali_render.c` when the attachment image has `lay.afbc`, using `rt_gpu_va + level offset`
   as header; the frame emission must patch the packed libkbase RT (or libkbase grows an AFBC RT packer) and use the real tile size.
2. Images must actually be bound as AFBC: the colour plane layout (`mali_image_layout_init`) still describes linear rows, so views,
   clears and copies would misread the memory. Either restrict the first test to an image only rendered to and read back via a graphics blit,
   or finish stage 5/6.
3. Fast clear/preload: clear through the RT clear colour, no linear memset on `afbc` images; LOAD preload needs the AFBC plane descriptor.
4. Header init policy decided (zero headers once at layout transition from UNDEFINED, debug aid `mali_afbc_init_bytes`).
5. Readback path: a graphics pass sampling the AFBC image (needs the AFBC texture plane descriptor, stage 5) or decode on host from headers.
6. Phone: v13 (X8) first, farm hold, temp < 43 C; compare solid and noise RT against a linear reference; watch dmesg/kbase for faults.

### Stage 4 (RT emit, host part) DONE

`struct mali_att` gained `afbc` + `afbc_rt`; `mali_render_pack_fbd` patches the packed RT (v13 layout, ZS-ext aware) only when `afbc` is set, so
the default FBD is byte-identical (`t_render_afbc`). `mali_render_att_afbc` builds the words for one level/layer (header VA = image VA + layer
stride + level offset, tile px from `1u << tile_log2`). `mali_afbc_image_header_init` zeroes all headers (debug aid; PanVK does no init).
Not yet done: nothing in `mali_cmd.c` calls `mali_render_att_afbc` (the render pass still sets linear va/stride), and `BindImageMemory2` does not
call the header init (needs a CPU mapping of the BO, or a GPU-side clear). ~2 h; total about 9 of 55-60 h. Items 1 (call site) and 4 (init
call site) of the phone-test list above are now only wiring.

### Stage 5 (command-buffer + bind wiring) DONE, still MALI_AFBC=1 only

- `mali_pipeline.c res_image_view` marks colour targets of AFBC images; `mali_cmd.c` requests AFBC on the frame colour attachment
  (`mali_att_request_afbc`); `mali_render_pack_fbd` resolves it with the real tile size (v13: `1u << tile_log2`, v11: 256 px). Bad level/layer fails the frame.
- `BindImageMemory2` calls `mali_image_afbc_bind_init` (zero headers, bounds-checked against the BO).
- Sampling / transfer policy (chosen: **ineligible**, no decompress fallback): an image is AFBC only when `usage` has none of SAMPLED,
  TRANSFER_SRC, TRANSFER_DST, INPUT_ATTACHMENT (plus STORAGE/HOST_TRANSFER from stage 1). So no descriptor, copy, blit, clear-image or
  compute path ever sees an AFBC image. LOAD from an AFBC attachment and MSAA are refused as `unsupported`. Note this excludes WSI swapchain images
  (they carry TRANSFER/sampled usage), so on a phone only offscreen render-only images can be AFBC.
- Not host-covered: the one-line hooks in `mali_pipeline.c`/`mali_cmd.c`/`BindImageMemory2` (need the Mesa build); the pure parts are tested.
- Hours: ~2.5 this stage, about 11.5 of 55-60 total.

### First phone test (prepared, NOT run; phones were busy with another lane)

`vk/tests/t_afbc_hw.c` (built by `vk/build.sh` as `out/a64-bionic/t_afbc_hw`; check.sh runs it on the fake backend in both modes). It renders a
128x128 RGBA8 attachment twice (clear; clear + red triangle). With `MALI_AFBC=1` the image is OPTIMAL + COLOR_ATTACHMENT only, so it is AFBC.
Nothing can decode that yet, so PASS means what the host can see without a decoder: fence signalled (no hang/DEVICE_LOST), every superblock header
the driver zeroed at bind was rewritten by the GPU, 4 KiB of 0xA5 guard bytes after the image are intact, and the draw case's headers differ from the
clear case's. Headers are printed as hex for the stage-6 decoder. Without `MALI_AFBC` it is a linear reference run that compares real pixels
(proves the test and the default path). On the fake backend (host) it proves `BindImageMemory2` zeroed the headers; mutation-checked (no-op'ing
`mali_image_afbc_bind_init` fails `afbc_hw bind v13` and `t_image`).

Run it on the X8 (v13) first, one phone, under a hold; stop above 43 C; the script also prints the last kernel mali/kbase/fault lines. Reference runs first:
a failing reference stops the script (device/driver problem, not AFBC).

```
cd <afbc2 worktree> && ~/.local/bin/mali-slot -- vk/build.sh        # builds vk/out/a64-bionic/{libvulkan_mali.so,t_afbc_hw}
farm hold <x8-device> --max 20 -- vk/tests/afbc_hw_run.sh           # exit 0 = both runs PASS
```

`<x8-device>`: see `farm status`. Run the reference half first; a failing reference means the device/driver is unusable, not an AFBC bug. Exit 124 =
hold ended while running (kill, check `qrestore` output). Expected on success: `{"afbc_hw":"PASS","mode":"afbc"}`.

### Stage 7: sampling an AFBC image through the AFBC texture Plane descriptor, DONE on the host, still MALI_AFBC=1 only

Route (chosen in the previous PR): sample it through the `AFBC Plane` descriptor (v13.xml:1994; Mesa `pan_texture.c emit_afbc_plane` / `get_afbc_plane_props`),
so the hardware decompresses while sampling. A host decoder (more code, proves nothing about what the GPU samples) and compute decompress (compute cannot read AFBC) were rejected.

Code: `mali_afbc_tex_plane_words` (`mali_afbc.h`, pure) and the hook in `mali_view_emit` (`mali_image.c`): when `lay->afbc`, the 32-byte payload entry of each
(layer, level) is the AFBC plane instead of the Generic plane (header VA = image + `level_offset[lev]` + layer * `array_stride`; size = level size; row stride in
BYTES = `header_row_stride`; header slice size = `header_size`; slice stride 0; extent of the mip), and the texture descriptor sets the interleave bit like Mesa
(`texel_interleave = modifier != LINEAR`). Words (genxml, all classes): w0 = Plane(10) | AFBC plane type(6)<<4 | superblock size<<8 | YTR<<16 | split<<17 | tiled header<<19 |
prefetch<<20 (Mesa always sets it) | compression mode<<26; w1 = size lo; w7 = (w-1) | (h-1)<<16.

GPU classes (decided by `arch`, from `v10.xml`, `v11.xml`, `v12.xml`, `v13.xml`; the AFBC Plane struct of v10 == v11 and v12 == v13):

| class | arch | pointer | size | row stride | slice stride | status |
|---|---|---|---|---|---|---|
| v10, v11 (G-series 5th gen early) | 10, 11 | 64-bit, words 2-3 | 32 bit (w1); a larger level is refused (-1) | 32 bit, word 4 | 32 bit, word 6 | supported |
| v12, v13 | 12, 13 | 48-bit: word 2 + word 3 bits 0..15 | 48 bit: w1 + `Size hi` in word 3 bits 16..31 | 27 bit, word 4 bits 0..26 (larger: -1) | word 6 + `slice stride hi` word 4 bits 27..31 (always 0 for 2D) | supported |
| arch 7..9 (Bifrost, Valhall v9) and unknown >13 | | plane encoding differs / unverified | | | | NOT supported: SAMPLED images stay linear, render-only AFBC unchanged |

Eligibility (`mali_afbc_image_eligible`, `sample_ok`): SAMPLED is allowed on an AFBC image only for arch 10..13 and an AFBC colour format (same table as render). TRANSFER_SRC,
TRANSFER_DST and INPUT_ATTACHMENT stay linear (no decompress/copy path). Also kept linear when SAMPLED: CUBE_COMPATIBLE and MUTABLE_FORMAT images (cube views and re-interpreting views
are not covered), storage / host-transfer as before. Views of an AFBC image must be 2D or 2D_ARRAY, not storage, and have the image's compression mode (sRGB vs UNORM of the same
format is the same mode and fine); otherwise `mali_view_emit` fails with -EINVAL. Default (flag off) path and the golden are unchanged.

Tests: `t_afbc` has hand-derived words per class (v13 with a >4 GiB size and 48-bit pointer, v12, v10/v11 with a 64-bit pointer, 27-bit stride overflow, modifier bits, R5G6B5 mode, unsupported
archs/formats) and the eligibility matrix; `t_image` checks the emitted payload of a 2 layer x 2 level view (layer-major, hand-derived header addresses/sizes/strides) plus the
interleave bit, view rejects, and that a linear image still gets Generic planes. 12 mutants (arch class, size hi, compression mode shift, prefetch, header size, cube flag,
interleave, layer stride, level offset, mode check, eligibility arch window) are all caught. `t_afbc_hw` gets `--case sample` (default: runs after clear and draw): the SAMPLED AFBC image
is rendered, then drawn 1:1 through the tex shaders into a linear target; every target pixel must be the clear colour or the triangle red and about 2048 red pixels must be present. Reference mode runs
the same pass on a linear image. On the fake backend only the bind-time part is checked (and that the SAMPLED image really is AFBC-sized).

Remaining before a hardware run (honest list; the descriptor semantics can only be checked on hardware, golden tests catch bit positions, not meaning):
1. v13 RT bit positions: `kb_fb_pack` TODO(v13) still emits v11 descriptors on v13, so the first phone run may fault in the render pass (the stage-5 test shows that first).
2. Render -> sample barrier: no layout-transition/cache flush is emitted for an AFBC image; the test uses a separate submit with the fence in between. Needs the barrier work for in-queue use.
3. Zeroed headers: the AFBC "cleared" state is believed to be all-zero headers for a never-written image (PanVK does no init); unverified, and a sampled-before-rendered image depends on it.
4. Copies/blits, TRANSFER_*, INPUT_ATTACHMENT, MSAA, LOAD ops, depth/stencil, 3D, cube, mutable views, storage: all still linear/refused.
5. Hardening: modifier exposure (nothing advertised), kb_fb_pack for lower archs, mip/array sampling on hardware, BW numbers.
6. Run the phone test (`farm hold`, X8 first), see above; `afbc_hw_run.sh` now also pushes the tex shaders.

Hours: ~3.5 this PR (descriptor words, view wiring, eligibility, tests, mutation checks, sample pass, doc); about 16.5 of 55-60 total.
