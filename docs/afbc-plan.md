# AFBC plan (colour first)

Status: flag `MALI_AFBC=1` exists, default off, nothing advertised, no driver behaviour change. Hardware gain unproven
(0.1-0.3 ms on cube; the swapchain stays LINEAR, so only app-internal targets benefit). Research notes: `/tmp/oc/afbc.md`.
Hours are focused lane-hours including tests. Total about 55-60 h; colour-only milestone (stages 0-4 + minimal 6) about 30 h.

| # | Stage | h | Test |
|---|-------|---|------|
| 0 | Layout maths + flag (`mali_afbc.h`, PR #169). Add a Mesa `pan_afbc.h` cross-check over ~200 random (fmt, mod, w, h) | 1 | `t_afbc` host (hand-derived values, overflow cases); cross-check compares sizes to Mesa |
| 1 | Eligibility `mali_afbc_image_eligible` (this PR): optimal, 2D, 1 sample, no STORAGE/HOST_TRANSFER/DISJOINT/ALIAS/SPARSE, colour table, >=16x16 | 2 | `t_afbc`: one case per reject reason, format hit/miss table |
| 2 | Image layout and alloc: host part DONE (`mali_afbc_image_layout`: mips + layers, #stacked PR); remaining wiring in `mali_image.c`, memory requirements, subresource layout. Default off | 4 | `t_image` golden sizes with flag on and off; flag off must be byte-identical |
| 3 | RT descriptor packing (v10/v11/v13), compression-mode table, `reverse_issue_order` when tile < superblock | 5 | golden descriptor bytes vs genxml on both fake GPUs |
| 4 | Colour attachment end to end: clear via RT clear colour (not a linear memset), LOAD preload with AFBC plane descriptor, store. First phone run | 8 | fake-backend command-stream goldens; phone: solid + noise RT read back through graphics blit, byte compare, 100-frame loop |
| 5 | Sampled reads: AFBC plane descriptor in descriptor/sampler code; view format must keep the same AFBC mode | 6 | descriptor goldens; phone sample test |
| 6 | Copies, blits, clears, resolves on AFBC images via graphics (compute cannot touch AFBC) | 12-16 | CTS copy/blit/clear subsets; dominant risk |
| 7 | Depth/stencil AFBC: ZS descriptor, 16x16 only, separate-layouts question | 8 | ZS descriptor goldens; phone depth test |
| 8 | Hardening: CTS image and dynamic-rendering subsets, DXVK games, hang soak. WSI modifiers are separate and compositor-dependent | 8 | farm CTS, A/B gpu_ms and image diff with MALI_AFBC=1 vs 0 |

Rules: nothing is switched on by default before stage 4 passes on hardware; phone runs go through the farm hold and stay
below 43 C; a bad descriptor means a GPU fault, so descriptor stages need golden tests first.
