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

Remaining before a phone test: (a) a way to read an AFBC render target back: a render-only image cannot be sampled or copied, so either stage 6
(AFBC texture plane descriptor for SAMPLED) or a debug env that dumps the BO via CPU map and a host AFBC decoder; (b) layout transitions /
barriers (cache flush, no decompress needed for same-format use); (c) a test app rendering to an offscreen AFBC image, with a linear reference
compared after decode; (d) v13 X8 first, farm hold, temp < 43 C, watch kbase faults; (e) verify v13 RT bit positions (kb_fb_pack TODO) and that zeroed headers are a valid start.
