# Desktop GPU: what it is good for, what it cannot do

The host has an **AMD Radeon RX 9070 XT (RDNA4, `RADV GFX1201`)** on Mesa/Vulkan 1.4.363. This page records what we
measured and built around it, and — just as important — what it must not be used to claim.

The one asset a desktop GPU gives us here is a **second, independent implementation that produces pixels**. Everything
below follows from that, and stops where a Mali-specific behaviour begins.

## Verdict on the four options

| Option | Verdict |
|---|---|
| **1. A "desktop GPU" backend that executes our command stream** | **Not worth it as a driver test, and it is not one.** Our pipeline lowering is the product under test: `mali_compile.c` turns the app's SPIR-V into Valhall/Bifrost ISA, `mali_descriptor_lower.c` packs descriptors into Mali format, `mali_cs_emit.c`/`mali_queue.c` build and submit the command stream, and the tiler/AFBC are hardware. A backend that hands the *same SPIR-V* to RADV bypasses all of that, so it would test the `vk_*` front-end glue only — the part the fake backend plus `tests/golden/` command-stream diffs and the host dEQP run already cover offline, with no GPU. Calling it a "desktop GPU driver backend" would be misleading. |
| **2. Desktop GPU as a reference renderer** | **The useful one, and what this page is about.** One scene, one client, two implementations: our ICD on the phone, RADV on the desktop. Diff the frames. Catches attachment-store/readback bugs, format/clear/blend mistakes, descriptor and pipeline state errors — the class where a phone frame is simply wrong and nobody has a known-good answer. |
| **3. WSI/pacing stress against a real compositor** | Small value. The `xwsi-host.sh` path already puts our swapchain in front of a real X server with the fake backend; the desktop GPU adds nothing there (it is not our present path), and pacing at 120 Hz on a desktop says little about a phone compositor under a Steam guest. |
| **4. DXVK/vkd3d conformance through a translation backend** | Blocked by the same wall as option 1: those tests need our driver to *execute*, and no translation layer can execute our ISA. DXVK on RADV tests RADV. |

## What the reference path validates

`vk/tests/ref_scene.h` defines one deterministic scene: geometry, a generated 256×256 texture, draw order with real
depth (`D32_SFLOAT`), alpha blending, a second pass with `loadOp=LOAD`, and one clear colour. `vk/tests/t_vk_ref.c` is a
plain Vulkan client that renders it and writes a PPM plus per-block digests. The **same binary** runs on both sides, so
the only variable is the driver.

It validates, on the real phone path:

- **attachment writes and the readback path**: the colour image is `VK_IMAGE_TILING_OPTIMAL` (AFBC on Mali) and is read
  back with `vkCmdCopyImageToBuffer`. This is deliberate: the existing phone pixel tests (`t_vk_draw`) render into
  `VK_IMAGE_TILING_LINEAR` + host-visible memory, which never exercises AFBC. A store or decode that loses or shifts a
  64-row block shows up here as one row of the block map;
- **`loadOp=LOAD` round-trips** of colour and depth (second pass), i.e. store→load of the same allocation;
- **depth test/write order**, blending, clear values, and the format metadata behind them;
- **pipeline/descriptor/image-view/command-buffer state** as the driver interprets it: if our lowering binds the wrong
  sampler, stride, or attachment, the pixels differ;
- **agreement with an independent implementation**, which is what makes a failure actionable: the report says *which*
  block and *which* 64-row band, so "rows 64–127 are wrong" is one line.

## What it cannot do

- **Execute our compiled shader ISA.** RADV runs SPIR-V. Nothing here says anything about `mali_compile.c`'s output.
- **Reproduce tiler behaviour**, tile lists, or AFBC's hardware layout. It sees AFBC only through its *symptoms* (a
  wrong frame on the phone); it cannot tell you the layout is wrong.
- **Touch kbase.** No ioctls, queues, heaps, CQS words or fences are involved on the desktop side.
- **Measure performance.** RADV is not a model of a Mali GPU, and the scene is not a benchmark.
- **Validate our WSI/present path**, which runs in a Steam guest against a compositor.

It is also not a substitute for the CPU rasteriser over the recorded frame (`MALI_SOFT=1`, `docs/host-harness.md` when it
lands): that one shares our frame record and can check fixed-function state without a phone, but it stands in for
shaders. The desktop reference is the opposite trade: real shading, no driver internals.

## Running it

Both sides run the same binary; only the ICD differs. Build once:

    cd vk && ./build.sh              # out/native/t_vk_ref

Desktop reference (RADV, no display needed — the scene is offscreen):

    VK_LOADER_LAYERS_DISABLE=~all~ env -u DISPLAY \
        out/native/t_vk_ref --spv-dir shader --out /tmp/ref.ppm --json /tmp/ref.json

or `tests/refdesktop.sh /tmp/ref` which does the same and keeps the two files together. A full 512×512 frame with five
draws takes **~373 µs** on the RX 9070 XT, so regenerating the reference is cheap enough to do on every change to the
scene.

Phone (the binary is the aarch64 build of the same client, from `cross/build-cross.sh` + the `t_vk_ref` line in
`build.sh`):

    adb push out/a64-glibc/t_vk_ref /data/local/tmp/
    adb shell 'cd /data/local/tmp && MALI_BACKEND=kbase ./t_vk_ref \
        --direct ./libvulkan_mali.so --spv-dir shader --out phone.ppm --json phone.json'

Offscreen, so no compositor or Steam session is needed. On the farm, submit the JSON line: the per-64-row-block digests
are in it, so a run can be compared without copying the image back.

Compare:

    tests/refdiff.py phone.ppm ref.ppm              # human report + block map, exit 1 on mismatch
    tests/refdiff.py phone.ppm ref.ppm --json       # one JSON object
    tests/refdiff.py phone.ppm ref.ppm --diff d.ppm # amplified difference image

## Tolerances, and why they are not zero

RADV and Mali are different implementations of the same spec. Where the spec allows a choice, the pixels may differ:

| Source | Expected size | Handled by |
|---|---|---|
| texture filtering (bilinear subtexel weights, 8-bit precision) | ±1–2 per channel on texel boundaries | `--tol` |
| blending fixed-point rounding, alpha | ±1–2 | `--tol` |
| cleartext / format conversion rounding | ±1 | `--tol` |
| triangle coverage and interpolation | **exactly equal** (Vulkan fixes the fill rule); a difference here is a driver bug on one side | block budget |
| AFBC/tiling store bugs | whole 64-row bands, hundreds of pixels | block map |

Defaults: `--tol 4` per channel, a block fails above 0.5 % bad pixels (20 of 4096, so a 1-pixel silhouette edge is not a
failure but a 1-pixel-wide diagonal across a block is), and the frame fails above 0.25 % bad pixels. Pass explicit values
in a report; a comparison without a stated tolerance is not a result.

A mismatch means **the two implementations disagree**, which is a question, not automatically a Mali bug: RADV can be the
odd one out. Adjudicate with the spec and, for a suspect block, with a second phone or the CPU rasteriser.

## Notes and pitfalls

- **Do not point the client at RADV and call it a driver test.** With `--direct` absent the client uses the loader, so
  `VK_ICD_FILENAMES` decides which driver runs. `refdesktop.sh` makes the intent explicit; the JSON's `device` field
  records what actually rendered (`AMD Radeon RX 9070 XT (RADV GFX1201)` vs `Mali-G925 MC11 (mali-own, fake backend)`).
- **Layer noise.** This machine has implicit layers (MangoHud, Steam overlay, LSFG, gamescope WSI) and one of them
  (`liblsfg-vk-layer.so`) fails to load under `vulkaninfo`. `refdesktop.sh` disables all layers; a result produced with
  layers enabled is not comparable.
- **The fake backend renders nothing.** `MALI_BACKEND=fake` runs the whole API path and reports `skipped`; that is the
  `check.sh` case, not a reference comparison.
- **`--nearest`** exists to remove filtering from the comparison when isolating a non-filtering bug; the reference image
  must then be regenerated with the same flag.
- Keep scenes small and deterministic. A scene that samples a transcendental per fragment will wobble by ±1 and teach
  nothing; `ref_scene.h` avoids that on purpose.

## Files

| File | What |
|---|---|
| `vk/tests/ref_scene.h` | the scene: geometry, texture, draws, clears, per-block digests |
| `vk/shader/ref.vert`, `ref.frag` (`.spv` committed) | the shaders, compiled by `glslangValidator`/`spirv-val` |
| `vk/tests/t_vk_ref.c` | the client, one binary for phone and desktop |
| `vk/tests/refdiff.py` | block comparator (`--selftest` runs in `check.sh`) |
| `vk/tests/refdesktop.sh` | the RADV reference run, with layers off |
