# The desktop harness: what the laptop can prove before a phone run

Phone time is scarce; the desktop has a fast CPU and plenty of RAM. This document says exactly what
the host can decide, what it only *looks* like it decides, and what must wait for hardware. Run
`tools/hostcheck` (see the bottom) before every phone run.

The premise: most of our bugs are Vulkan-semantics bugs (MRT, load/store ops, formats, sampling, WSI,
DXVK feature negotiation), not GPU-hardware bugs. Inform the premise, not the other way round.

## Inventory: what runs today

Verified on this host on 2026-11-19 (see `tools/hostcheck --inventory` for the live matrix).

| Capability | Command | Really exercises | Silently skips / does not prove |
|---|---|---|---|
| Offline gate | `vk/check.sh` (builds first) or `vk/check.sh --no-build` | the front end built for x86-64, on the fake backend for v10/v11/v13: instance/device/procaddr, format props, descriptor lowering, command-stream goldens (R-VA/R-GEOM/R-HEAP/R-SET/R-CS/R-FLAGS), shader cache cold==warm, sanitizer and leak variants, `vulkaninfo --summary` and full | nothing renders: the fake backend accepts and discards every command stream. Any test that checks pixels reports "skipped" |
| Native x86-64 ICD | `MALI_TARGET=native vk/build.sh` → `vk/out/native/libvulkan_mali.so` | the whole Vulkan layer above the kernel: object lifetimes, entrypoint tables, pipeline/descriptor lowering, the CSF **word** building for v11 **and** v13 | the kernel uAPI, cache maintenance, real tiling. `MALI_BACKEND=fake` never touches kbase |
| DXVK/Steam negotiation | `MALI_BACKEND=fake MALI_FAKE_GPU=v13 out/native/t_steam_caps --direct out/native/libvulkan_mali.so` | the exact `vkCreateDevice` extension/feature set DXVK 2.7 and the Steam client ask for; unresolved extension commands; foreign-queue barriers | DXVK's own code paths: no d3d11 render happens. Vendor-specific extension *behaviour*, only presence |
| dEQP extension block | `out/native/t_dxvk_caps --direct … --expect-missing …` | the feature bits the DXVK gap list depends on | anything below the API |
| X11/WSI | `vk/tests/xwsi-host.sh vk/out/native v10` (`vk/check.sh` runs it) | a real `VK_KHR_xcb_surface` swapchain: create/acquire/clear; present x3 (`t_vk_xcb`), and `glxinfo -B` through zink (kopper needs a working xcb surface + swapchain) | the real compositor, scanout, dmabuf import/export. Runs under `gamescope --backend headless`; skips itself (exit 0, "skipped") without gamescope/Xwayland or `t_vk_xcb` |
| Host dEQP-VK | `vk/check.sh --deqp-host` (opt-in, ~15 min) | ~200 dEQP-VK cases against the fake backend through the real loader | functional pixel results: dEQP runs but every draw is a no-op, so only Crash/Timeout/NotSupported are gated |
| Guest aarch64 build | `vk/cross/build-cross.sh glibc` | the 32-bit-ABI / bionic-libc differences of the same sources | nothing is executed on the host: there is no aarch64 user-mode runner wired here (`qemu-aarch64-static` is installed but no script uses it, and the sysroot's `ld.so` is not set up). Arch-specific *words* are already checked natively by the goldens: they are data, not code |
| Soft-rendered frames | `vk/tests/soft-frame.sh` (`vk/check.sh` runs it; `--bless` re-records) | fixed-function state and attachment metadata of a recorded frame end to end: clear/render-area coverage, row stride, formats, viewport/scissor, culling, depth test/write (D32F, D24X8, D16), blending, write masks, MRT (2-4 attachments), colour LOAD, two-half renders. Pixels are compared tile by tile with `vk/tests/golden/soft-frame.txt`; the map is byte-identical across v10/v11/v13 | shader *code*: CPU programs stand in for the pipeline's shaders. Not vkmark/DXVK |

Not available on this host today: `vkmark`, DXVK/d3d11 binaries, `Xvfb`, a wired `qemu-aarch64` runner.

## What the software rasteriser does not model

Say exactly which of these a passing `soft-frame.sh` does **not** prove:

* **Multiple render targets and colour `loadOp LOAD` are modelled** (the rasteriser; not the hardware path).
  `struct mali_frame` carries 1-4 colour attachments (`ncolour`, `colour`, `colour_x[]`), each with its own address, row
  stride, format and flags: CLEAR / LOAD (`clear = false`, memory kept) / DONT_CARE (`dont_care`, modelled as clear to 0)
  and storeOp DONT_CARE (`no_store`, the pass renders to a copy and memory is left alone). A CPU program drives them
  through `fs_mrt`. All RTs share attachment 0's blend state and write mask (no `independentBlend`). The hardware frame
  builder rejects `ncolour > 1` (`maxColorAttachments = 1`), and real passes lower colour LOAD to a preload *shader* draw
  that the CPU model cannot run, so MRT/LOAD goldens use hand-built frame records, not `vkCmdBeginRenderPass`. Cases:
  `mrt` (two passes: RT0+RT1, then RT1 LOAD and RT0 DONT_CARE load/store, each attachment diffed), `mrt4`
  (four formats/strides, one LOAD), `load-partial`, and `halves` (below).
* **The "rows 64-127" class is guarded.** `halves` renders one image as two render areas, rows 0..63 then 64..127, from
  zeroed memory; a skipped second half reads back black and fails both the asserts and the golden. Mutation check: making
  the rasteriser skip any render area with `y >= 64` fails `halves` ("row 64 is black: the second (rows 64..127) half was
  never written"). This proves the *frame record* describes both halves, not that AFBC compression writes them.
* **Compression and tiling are not modelled.** Attachments are linear; AFBC headers/bodies, 16x16 tiles, tile-buffer
  flush order and cache behaviour are hardware-only.
* **Shader code.** The rasteriser executes a CPU program registered per pipeline (`mali_soft_bind_program`), not the app's
  SPIR-V. It proves vertex fetch, clipping-less triangle setup (triangles reaching behind the eye are dropped, not
  near-clipped), interpolation, and the whole fixed-function output path; it says nothing about the compiled Valhall
  shader, descriptor tables or texture sampling.
* **Sampling, MSAA, stencil, compute, indirect draws, non-triangle topologies.** Each makes `mali_soft_frame` return
  `-ENOTSUP` (the submit fails loudly, it never draws nothing silently). `t_soft_frame` covers single-sample colour +
  depth, triangle lists, direct draws.

## What the host cannot prove, ever

* **The kernel and the hardware**: kbase uAPI version quirks, COHERENT_LOCAL flags, dma-buf import,
  tilers, MSAA sample patterns, cache maintenance, power/thermal. Any of these can and did break on
  the phone while the host stayed green.
* **Performance**: the fake backend retires instantly. Numbers from the host mean nothing.
* **App behaviour**: DXVK and Proton apps need real rendering; a no-op backend only proves the
  negotiation, not the frame.

## Rules of thumb

1. A failing host gate is always a real bug. Fix it before touching a phone.
2. A passing host gate never removes the need for one phone smoke run per release; it removes the
   *debugging* phone runs.
3. If a bug reproduces in the soft-rendered frame (wrong stride, wrong load op, MRT rejected, wrong
   clear colour, blend factor), fix it on the host and only verify once on the phone.
4. If a bug needs a shader to execute, or a tile to be written by the hardware, go straight to the
   phone with the smallest repro; do not wait for the host.

## tools/hostcheck

```sh
tools/hostcheck              # build + offline gate + soft frame diffs  (~60 s warm)
tools/hostcheck --no-build   # skip the build
tools/hostcheck --x11        # additionally the gamescope X11/WSI test
tools/hostcheck --inventory  # what this host has (tools, X server, dEQP, vkmark, DXVK)
```

It prints one line per stage and a final `PASS`/`FAIL`. Use it as the first gate before any phone
run; `vk/check.sh` remains the authoritative gate for the driver itself, `hostcheck` is the wrap-up
(same checks, plus the soft-rendered frame diffs and an inventory of what the host actually has).
