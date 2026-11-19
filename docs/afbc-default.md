# AFBC by default: decision, and the case that has to pass first

Question: AFBC is implemented behind `MALI_AFBC=1` and nothing is advertised. Is there a subset that can be turned
on by default now, with a per-allocation fallback to linear for everything else? **No. Not yet.** This file records
what is implemented, what has run on hardware, the one thing that blocks a default-on subset (render-pass `loadOp
LOAD`), and the measurement that decides it.

## What exists

| Piece | State | Hardware |
|---|---|---|
| Layout maths, `mali_afbc.h` (one plane, 2D, single sample, mips + layers, 64-bit checked) | done, `t_afbc` | n/a |
| Eligibility `mali_afbc_image_eligible` (OPTIMAL, 2D, 1 sample, colour table, >=16x16, no STORAGE/HOST_TRANSFER/TRANSFER_*/INPUT_ATTACHMENT, no mutable/cube/disjoint/alias/sparse) | done, `t_afbc`/`t_image` | n/a |
| Sizing wired into `mali_image.c` (`max(linear, AFBC)`, linear planes untouched) | done, flag on only | n/a |
| RT words (v10/v11/v13 packing, compression modes, reverse issue order) + FBD patch | done, `t_render_afbc` | render OK on X8 (v13) and v11 with the v11 `kb_fb_pack` layout |
| Colour attachment end to end (clear via RT clear colour, store, bind-time header zeroing) | done | yes: X8, all 64 superblock headers rewritten, guards intact |
| Sampled reads through the AFBC texture Plane descriptor (v10/v11 64-bit pointer, v12/v13 48-bit) | done, `t_image` + view rejects | yes on v11 (14336 clear / 2048 red / 0 other). X8 needed the genxml 0-based superblock enum (PR #216) and that fix has only been proven on v11 |
| `loadOp LOAD` on an AFBC attachment | **not implemented**: refused (`-EINVAL`, the frame is reported unsupported) | n/a |
| TRANSFER_SRC/DST, copies, blits, `vkCmdClearColorImage`, INPUT_ATTACHMENT, storage, host transfer | **linear only** (rejected at image creation) | n/a |
| MSAA (2x/4x), resolves, depth/stencil, 3D, cube, mutable-format views, sparse/alias | **linear only** | n/a |
| Mips and array layers | layout + descriptors host-tested | **never sampled or rendered on a phone** (t_afbc_hw uses 1x1) |
| WSI export/import, `EXT_image_drm_format_modifier` | not implemented, nothing advertised | n/a |
| Render -> sample in one queue submit | no layout/cache handling; the test uses a fence between submits | unverified |

## Why a default-on subset is not shippable today

The blocked axis is the *load op*, and it is not a property of the allocation, so it cannot be decided at
`vkCreateImage` time:

- This driver preserves colour contents for `loadOp LOAD` with a preload draw that reads the image as a **linear
  storage buffer** (`mali_cmd.c`, `mali_preload_src`), because `kb_fb_pack` has no hardware tile-preload knob.
  On an AFBC image the same read would decode headers and compressed body as rows of pixels.
- So `mali_att_request_afbc()` refuses `LOAD`/`NONE` and the command buffer fails with
  `VK_ERROR_FEATURE_NOT_PRESENT`. With `MALI_AFBC=1` that is an opt-in experiment; as a default it is a broken app.
- Every allocation that is a plausible 120 fps win is one an app may legally load: a UI composite target, a
  G-buffer, a DXVK render target (CLEAR on first use, LOAD afterwards). Excluding images that "might be loaded"
  leaves only `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` images, where `LOAD` is invalid, and that is
  effectively MSAA-only in practice - which AFBC rejects anyway. That subset has no measurable benefit, so
  shipping it would be a pretend optimisation.

Everything else in the default-on list is already decided and works: allocations fall back to linear at creation
(eligibility), unsupported *views* fail with `-EINVAL`, and unsupported *passes* fail the command buffer instead of
reading compressed memory as linear rows. `t_afbc_hw --case load` now pins that last contract on the host
(`check.sh`, fake backend) so a future change cannot silently turn the refusal into corruption.

## What default-on needs, in order

1. **AFBC preload**: implement `loadOp LOAD` for an AFBC attachment as a draw that reads the image through the
   AFBC texture plane (the sampled path above, already working) instead of a storage buffer. This is the whole
   blocker; the rest is bookkeeping.
2. **One-queue render -> sample**: a barrier/cache flush between rendering and sampling the same AFBC image (today
   the test needs a separate submit and a fence).
3. **v13 sample re-run** with the superblock-enum fix (PR #216) on the X8, plus mips and array layers (2x2 view,
   non-zero base level) in `t_afbc_hw`.
4. Then, and only then, the default-on rule: apply AFBC at image creation for the eligibility class above, and
   keep the per-allocation fallback to linear at creation for everything not covered. No env var.
   Things that could silently break once it is on for real apps, in rough order of danger: `LOAD` (above), a
   `vkCmdClearColorImage`/copy on an image whose usage we failed to exclude, a view whose format changes the
   compression mode (sRGB vs UNORM is the same mode; width changes are not), a resolve or an input attachment,
   and any pass that samples and renders to the same AFBC image.

## Expected benefit, and how to prove it

AFBC cuts texture and render-target bandwidth by roughly 2-4x for UI and flat-shaded colour content and saves
nothing for noise. For a 720p RGBA8 target that is a few MB per frame each way; against the measured floor in
`vk/BENCH-cmf-20261117-v3.md` (GPU time scaling linearly with pixels, ~1.3 ms at 720p) the plausible win is a
double-digit percentage of the *fill-bound* part for a UI composite and near zero for an ALU-bound frame. It does
not shrink allocations (the AFBC size is the upper bound).

To prove it on device, extend `t_afbc_hw` (it already renders, samples and dumps headers):

- `--case load`: reference must pass today; AFBC must be refused today and must produce the same pixel counts as
  the reference once (1) lands. That single line is the gate.
- a `--bench N` loop that renders a frame with flat colour + a triangle N times into the AFBC image and into a
  linear one, timing with `CLOCK_MONOTONIC` around the submits, so the ratio is measured on the same GPU, same
  submit count. Compare against `bench/` gpu_ms for the real workloads (CMF, Steam UI) as the end-to-end check.

Phone runs go through `farm hold` (< 43 C), as before. Nothing here is enabled by default until that has run.
