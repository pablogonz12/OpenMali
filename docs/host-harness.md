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
| DXVK/Steam negotiation | `MALI_BACKEND=fake MALI_FAKE_GPU=v13 vk/out/native/t_steam_caps --direct vk/out/native/libvulkan_mali.so` | the exact `vkCreateDevice` extension/feature set DXVK 2.7 and the Steam client ask for; unresolved extension commands; foreign-queue barriers | DXVK's own code paths: no d3d11 render happens. Vendor-specific extension *behaviour*, only presence |
| dEQP extension block | `MALI_BACKEND=fake MALI_FAKE_GPU=v11 vk/out/native/t_dxvk_caps --direct vk/out/native/libvulkan_mali.so --expect-missing 5` (the count is `DXVK_MISSING` in `vk/check.sh`; the same run is done for v13) | the feature bits the DXVK gap list depends on | anything below the API |
| X11/WSI | `vk/tests/xwsi-host.sh vk/out/native v10` (`vk/check.sh` runs it) | a real `VK_KHR_xcb_surface` swapchain: create/acquire/clear; present x3 (`t_vk_xcb`), and `glxinfo -B` through zink (kopper needs a working xcb surface + swapchain) | the real compositor, scanout, dmabuf import/export. Runs under `gamescope --backend headless`; skips itself (exit 0, "skipped") without gamescope/Xwayland or `t_vk_xcb` |
| Host dEQP-VK | `vk/check.sh --deqp-host` (opt-in, ~15 min) | ~200 dEQP-VK cases against the fake backend through the real loader | functional pixel results: dEQP runs but every draw is a no-op, so only Crash/Timeout/NotSupported are gated |
| Guest aarch64 build | `vk/cross/build-cross.sh glibc` | the 32-bit-ABI / bionic-libc differences of the same sources | nothing is executed on the host: there is no aarch64 user-mode runner wired here (`qemu-aarch64-static` is installed but no script uses it, and the sysroot's `ld.so` is not set up). Arch-specific *words* are already checked natively by the goldens: they are data, not code |
| Soft-rendered frames | `vk/tests/soft-frame.sh` (`vk/check.sh` runs it; `--bless` re-records) | fixed-function state and attachment metadata of a recorded frame end to end: clear/render-area coverage, row stride, formats, viewport/scissor, culling, depth test/write (D32F, D24X8, D16), blending, write masks. Pixels are compared tile by tile with `vk/tests/golden/soft-frame.txt`; the map is byte-identical across v10/v11/v13 | shader *code*: CPU programs stand in for the pipeline's shaders. Not vkmark/DXVK |

Not available on this host today: `vkmark`, DXVK/d3d11 binaries, `Xvfb`, a wired `qemu-aarch64` runner.

## What the software rasteriser does not model

Say exactly which of these a passing `soft-frame.sh` does **not** prove:

* **MRT in one pass.** `struct mali_frame` carries a single colour attachment (`colour`, one `mali_att`), because the front
  end advertises `maxColorAttachments = 1` today. A two-RT pipeline is rejected at pipeline creation (covered by
  `t_dxvk_caps`/`t_steam_caps`), and the rasteriser has nothing to write the second RT through. Multi-RT *frames* are
  covered as two separate passes/attachments, never as one subpass.
* **Colour `loadOp LOAD`.** `mali_cmd.c` rejects a colour attachment with `loadOp LOAD` (not representable in the FBD), so
  no Vulkan client can reach it. The rasteriser does implement LOAD (`clear = false` keeps memory), and
  `case_render_area` exercises it directly on a hand-built frame record: a CLEAR limited to rows 64..127 must leave rows
  0..63 and 128..255 untouched. That is the second-block-row class ("rows 64-127 read zero"); it is unit-tested on the
  frame record, not through `vkCmdBeginRenderPass`.
* **Shader code.** The rasteriser executes a CPU program registered per pipeline (`mali_soft_bind_program`), not the app's
  SPIR-V. It proves vertex fetch, clipping-less triangle setup (triangles reaching behind the eye are dropped, not
  near-clipped), interpolation, and the whole fixed-function output path; it says nothing about the compiled Valhall
  shader, descriptor tables or texture sampling.
* **MSAA, stencil, compute, indirect draws, non-triangle topologies, indexed primitive restart.** Each makes
  `mali_soft_frame` return false, which `mali_frame_build` turns into `-ENOTSUP` (the submit fails loudly, it never draws
  nothing silently). `t_soft_frame` covers single-sample colour + depth, triangle lists, direct draws. Restart is
  rejected rather than split, because a restart index is a primitive boundary, not a vertex to fetch.
* **Texture sampling.** Not sampled *or* rejected: the check is for a registered CPU program (`mali_soft_bind_program`),
  not for what the shader does, and the CPU program receives interpolated varyings and push constants only, so it has no
  way to read a texture. A pipeline whose shader samples can therefore pass with a flat or constant colour where the GPU
  would show a texture. The same holds for any shader feature the CPU stand-in does not implement.
* **Shared-edge sample ownership.** Every sample centre inside a triangle, edge samples included, is covered by it, so two
  triangles that share an edge both cover the samples on that edge (no top-left tie-break). With blending that diagonal
  is blended twice and differs from the rest of the quad by up to one blend step; without blending the result is
  identical. A result that depends on exactly-once coverage of a shared edge is not proven by this rasteriser.
* **A rejected frame may already have cleared its attachments.** The colour and depth clears happen before the per-draw
  validation (program registered, topology, indirect origin, attribute format), so when `mali_soft_frame` returns false
  the attachments may hold the clear colour. The submit fails with `VK_ERROR_FEATURE_NOT_PRESENT` and no command stream
  is executed, so the frame never "succeeds" with a partial image, but a caller must not read the attachment after a
  rejected submit. Render-into-staging would be needed to leave the resources untouched.

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
