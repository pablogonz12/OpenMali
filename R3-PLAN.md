# R3 plan: Vulkan front end (2026-11-16)

Goal (ARCHITECTURE-proposal.md): **vkcube + dEQP-VK api/pipeline/renderpass ≥ 95 % on v11 (CMF G615) and v13 (Poco G925)**.
State: M1 done and verified offline (`vk/check.sh`). M5 done offline (`vk/tests/build-t_image.sh`, see "M5 status"): formats, layout, images, views, buffers, samplers, descriptor bytes = libkbase golden; wired into the driver (2026-11-16, see "M5/M7 wiring"). Everything from M2 on needs the phones for its pass criterion. M6 core done offline (`vk/tests/build-t_compile.sh`): `mali_compile()` + disk cache, R2 shaders byte-identical v11+v13; open: vk_pipeline_cache wiring, blend, dEQP corpus (M3), M7 offline part done (`vk/tests/build-t_desc.sh`): set layouts/pools/sets/updates/templates/dynamic buffers, pipeline-layout flattening, NIR descriptor lowering hooked into `mali_compile(layout)`; wired (see "M5/M7 wiring"); open: push descriptors, inline uniform blocks, dEQP numbers (M3).

## 1. Decisions

| Topic | Decision | Why |
|---|---|---|
| Runtime | Mesa 26.3 `vk_*` runtime, unmodified, linked statically | Done in M1; loader glue, dispatch, object base, enumeration all come from it. |
| Pipelines | No own `VkPipeline`. Implement `vk_device_shader_ops` (`compile`, `cmd_bind_shaders`, `cmd_set_dynamic_graphics_state`); the runtime's `vk_pipeline.c` gives graphics/compute pipelines, pipeline cache, and later GPL and shader objects | Removes a whole object family; all graphics state arrives as `vk_dynamic_graphics_state`, one emission path. |
| Render pass | Implement only `CmdBeginRendering`/`CmdEndRendering`; `vk_render_pass.c` maps VkRenderPass/Framebuffer onto them | Half the entry points, one FBD path. |
| Clears, copies, blits, resolves | `vk_meta_*` from the runtime, driven by our draw and compute paths | ~4k lines we don't write; costs correctness of our own draw path first (hence the order M9 → M12). |
| Descriptor packing | Mesa genxml (`v11.xml`, `v13.xml`) + `genxml/cs_builder.h` (MIT) | Stated in the architecture. libkbase's hand packers stay as test fixtures and byte-level goldens. |
| Texture/format/blend helpers | Link `src/panfrost/lib` unmodified: `pan_format`, `pan_layout`/`pan_image`/`pan_texture`, `pan_blend`, `pan_samples`, `pan_earlyzs`, `pan_fb`/`pan_desc`, `pan_shader.h` | Not PanVK code; per-arch archives (`libpan-arch-v11/v13.a`) already build. `pan_kmod` is pulled in as types only. |
| Kernel interface | libkbase only, behind `mali_backend.h` | No DRM node on these phones; libkbase is verified on both. |
| Sync | Own binary `vk_sync_type` on sync_file (KCPU fence), timelines by `vk_sync_timeline` emulation, runtime submit thread | `vk_drm_syncobj` needs a DRM device. The emulation forces the threaded submit mode the architecture wants anyway. |
| Compiler | Mesa `bi_*` backend in-process behind `mali_compile()`; Kraid is a later A/B behind the same call | The spike proved the pipeline; Kraid needs panfrost-rust + bindgen. |
| Build | Mesa by meson (three build dirs), our code by `vk/build.sh` reading flags and the library list from the build dir | See section 4. |
| PanVK | Oracle only: run it, compare output and CS dumps. No file from `src/panfrost/vulkan` is copied | Project policy, not a licence need (PanVK is MIT too). `check.sh` asserts no `panvk` symbol. |

## 2. v11/v13 abstraction

Three layers, the same scheme Mesa's own per-arch libraries use:

1. **Arch-independent, compiled once**: instance, physical device, memory, sync, queue, buffers, images (layout through
   `pan_image`), descriptor set bookkeeping, shader cache, WSI. Prefix `mali_`.
2. **Per-arch, compiled twice** with `-DPAN_ARCH=11` and `-DPAN_ARCH=13`: everything that packs a descriptor or emits a CS
   instruction (`mali_cmd_*.c`, `mali_fb.c`, `mali_draw.c`, `mali_dispatch.c`, `mali_desc_pack.c`, `mali_queue_cs.c`).
   Names go through `GENX()` → `mali_v11_*`/`mali_v13_*`. Vulkan entry points land in the per-arch device dispatch tables that
   M1 already generates (weak symbols, fallback to `vk_common_*`).
3. **`struct mali_arch_ops`**: one const table per arch for internal calls from layer 1 into layer 2 (pack texture, pack sampler,
   emit queue init, emit FBD). Chosen once at device create from `arch_major`.

Differences stay inside layer 2 as `#if PAN_ARCH >= 13` (RUN_IDVS2, FBD and tiler layout). Budget: under 60 such sites; if
it grows past that the affected file is split into `_v11.c`/`_v13.c`. uAPI differences (1.21 vs 1.30) never reach the front
end: they stay in libkbase's version table.

## 3. Milestones

Each milestone runs on v11 first, then v13. "qdirect" = bionic binary by `scripts/quick/qdirect`, "guest" = glibc binary in
the guest through the Khronos loader (`qdrv`). dEQP pass rate = Pass / (Pass + Fail + Crash + Timeout), NotSupported
excluded, on the case list frozen in M3.

| # | Days | Content | Pass criterion |
|---|---|---|---|
| M1 ✔ | 1 | ICD on `vk_*` runtime: instance, physical device, props/features/limits, queue family, heap/types, device create; fake backend | Offline: `vk/check.sh` (loader and direct dlopen on fake v11/v13, 3 exports, no panvk symbols, full `vulkaninfo`). **Done.** |
| M2 ✔ | 2 | Mesa cross builds (aarch64 glibc, aarch64 bionic); `mali_backend_kbase.c` over libkbase; real probe (gpu_id, cores, texture features, uAPI); device create opens a kbase context; `vkGetInstanceProcAddr` export in the bionic flavour | qdirect `t_icd` on both phones reports G615 MC2 / G925 MC11; guest `vulkaninfo --summary` the same; 1000× create/destroy device with stable fd count. Offline: `check.sh` unchanged and also green on libkbase's `backend_fake`. |
| M3 (harness ✔, baseline partial) | 2 | dEQP-VK harness (section 5): build `deqp-vk`, `scripts/quick/qdeqp` runner, frozen case lists, oracle baseline | Oracle (PanVK-kbase) baseline for the R3 list recorded per phone in `driver/vk/deqp/baseline-{v11,v13}.txt`; a killed run resumes without rerunning finished cases; two runs of a 5k sample give identical results. |
| M4 | 3 | `VkDeviceMemory` on `kb_bo` (type 0 uncached coherent, type 1 cached + Flush/Invalidate), map/unmap, dma-buf import/export via dma-heap + MEM_IMPORT; `mali_sync`; `driver_submit` with an empty CS; fences, semaphores (binary, timeline), `QueueWaitIdle` | dEQP-VK `api.device_init.*`, `memory.allocation.*`, `memory.mapping.*`, `synchronization.basic.{fence,semaphore,timeline_semaphore}.*` ≥ 95 %; own `t_sync`: 10k empty submits, 0 lost fences, both phones. |
| M5 | 3 | Buffers, images (1D/2D/3D/cube/array, mips, linear and tiled-u), views, samplers, format properties from `pan_format`; texture and sampler descriptor packing | dEQP-VK `api.info.format_properties.*`, `api.info.image_format_properties.*`, `api.object_management.single.*` for these objects, `memory.requirements.*`, `memory.binding.*` ≥ 95 %. Offline: packed texture descriptor byte-equal to libkbase's golden for the R2 texture test. |
| M6 (core ✔) | 3 | `mali_compile()`: SPIR-V → NIR → lowering (I/O, varying layout, sysval and push constant → FAU, blend) → `bi_*` → binary + info; `vk_device_shader_ops.compile`; `vk_pipeline_cache` + on-disk `disk_cache` keyed by SPIR-V hash, lowering key, gpu_id, Mesa tag | Offline: 300 SPIR-V modules taken from the dEQP shader cache file of an M3 run (`--deqp-shadercache-filename`) all compile for v11 and v13, second run is 100 % cache hits and >20× faster; R2 compiled triangle renders identically with the binary coming from `mali_compile()`. |
| M7 (offline ✔) | 2 | Descriptor set layouts, pools, sets, updates, templates, push descriptors off; pipeline layout; own NIR pass mapping (set, binding, index) to Valhall resource tables; dynamic buffers | Offline: unit test of table layout and of the lowered NIR for all 11 descriptor types; dEQP-VK `api.descriptor_pool.*`, `api.descriptor_set.*`, `api.object_management.single.descriptor_set*` ≥ 95 %. |
| M8 | 3 | Command pool/buffer on `vk_command_buffer`; CS chunk allocator; `CmdBeginRendering`/`EndRendering` → tiler context + FBD + fragment job; loadOp CLEAR/DONT_CARE, storeOp; queue submit of recorded CS | Own `t_clear` through Vulkan on both phones: color + depth clear of a linear image read back through mapped memory, 1000 frames, 0 mismatches; dEQP-VK `api.command_buffers.{pool,allocate,reset,record_*}` subset without events ≥ 95 %. |
| M9 | 3 | Graphics draw path on v11: vertex input, RUN_IDVS, index buffers, viewport/scissor, raster, depth/stencil, blend via `pan_blend`, all from `vk_dynamic_graphics_state`; direct and indexed draws, instancing | v11: dEQP-VK `draw.renderpass.basic_draw.*`, `pipeline.monolithic.{vertex_input,input_assembly,depth,stencil,blend}.*` ≥ 90 %; triangle image equals the oracle's. |
| M10 | 2 | Same on v13: RUN_IDVS2, v13 FBD and tiler differences | v13: the M8 and M9 groups within 2 points of v11. |
| M11 | 2 | (status: compute works on CMF, see M11 status) Compute pipelines, `CmdDispatch`/indirect, storage buffers and images; indirect draws | dEQP-VK `compute.pipeline.basic.*` ≥ 95 %, `draw.renderpass.indirect_draw.*` ≥ 90 %, both phones. |
| M12 | 3 | `vk_meta`: `CmdClear*`, copy buffer/image both ways, blit, resolve; loadOp LOAD (preload); MSAA; MRT; `vk_render_pass` wiring | dEQP-VK `api.copy_and_blit.core.*`, `api.image_clearing.core.*` ≥ 95 %; `renderpass.suballocation.{simple,formats,attachment}.*` ≥ 90 %. |
| M13 | 2 | Barriers (render pass split on feedback, cache maintenance for type 1, layouts are no-ops), events (CS sync ops), occlusion and timestamp queries, secondary command buffers | dEQP-VK `api.command_buffers.*` ≥ 95 %, `query_pool.occlusion_query.*` ≥ 90 %, `synchronization.op.single_queue.event.*` ≥ 90 %. |
| M14 | 3 | WSI: `wsi_device_init`, `VK_KHR_wayland_surface` + `VK_KHR_swapchain` through `wsi_common`; own image allocation callback (dma-heap, imported into kbase); acquire/present fences per PRESENTATION-path.md | Guest, both phones: `vkcube` 10 min at refresh ±5 %, 0 CPU copies per frame (counted like the R2 present test), clean swapchain recreate on resize; dEQP-VK `wsi.wayland.swapchain.{create,render}.*` ≥ 90 %. |
| M15 | 3 | Burn-down 1: `api.*`, `renderpass.*`, `renderpass2.*`, `dynamic_rendering.*`; raise limits and features to what passes | Both phones ≥ 95 % on these groups; 0 cases that need a reboot. |
| M16 | 3 | Burn-down 2: `pipeline.monolithic.*`; full gate run; 2 h soak (vkcube + looping dEQP sample) | **R3 gate**: full list ≥ 95 % on v11 and v13, flake rate < 0.1 % (cases differing between two full runs), soak without device loss, gap to oracle reported per group. |

Sum: 40 agent-days sequential against a budget of 30 (6 agent-weeks). M3 and M6/M7 (offline parts) do not depend on
M2/M4/M5, so two agents in parallel for the first two weeks bring it to about 6 calendar weeks. With one agent R3 is 8 weeks.
The estimate is the honest one; cutting to 30 means dropping M11 and M13 below the gate's needs, which the dEQP `api` group
does not allow (it uses events and copies).

## 4. Build system

- **Mesa**: meson, unmodified source, one build dir per target under `third_party/`: `build-vk` (x86-64, offline tests and
  the host tools), `build-vk-a64-glibc` (cross file around `droidtest/tools/cc-aarch64-glibc` and its Ubuntu arm64 sysroot),
  `build-vk-a64-bionic` (NDK r27d cross file, API 29, `-Dplatforms=`, `-Dandroid-libbacktrace=disabled`). The cross
  builds use `-Dmesa-clc=system -Dprecomp-compiler=system` with `mesa_clc` and `vtn_bindgen2` taken from `build-vk` on PATH.
  Only the static libraries are built (the ninja targets `build.sh` checks for); the PanVK `.so` target is the convenient
  way to get them all.
- **Our code**: `vk/build.sh`, `MESA_BUILD=<dir> MALI_TARGET=<name>`. It takes the compile line, system libs and compiler from
  the Mesa build dir, so the three targets differ only in the build dir. Per-arch files are compiled twice.
- **Why not meson for our code**: adding our directory to Mesa's `meson.build` breaks "unmodified"; using Mesa as a meson
  subproject does not work, since its internal static libraries are not exported as dependencies. The cost is one list of
  archive names in `build.sh` to revisit on each Mesa bump.
- **Bionic specifics to settle in M2**: the native `.so` today has NEEDED `libdrm`, `libudev`, `libdisplay-info`,
  `SPIRV-Tools`, `expat`, `zlib`, `zstd` (measured with `readelf -d`; `libelf` is on PanVK's link line but `--as-needed`
  drops it). Of these the NDK has only `zlib`. Expected result after configuring without WSI display and with
  `-Dxmlconfig=disabled -Dzstd=disabled`: `zlib` plus Mesa's bundled `drm-uapi` headers. Anything left is built statically
  from Mesa's wraps. If more than 2 Mesa patches are needed for bionic, the bionic flavour is dropped and all
  testing goes through the guest.
- **Outputs**: `libvulkan_mali.so` (+ `mali_icd.json`) for the guest loader; the bionic flavour also exports
  `vkGetInstanceProcAddr` so tests and dEQP can load it without a loader.

## 5. dEQP-VK on the phone

*Status 2026-11-16: built (vulkan-cts-1.3.10.0, clang cross, 25 min), `qdeqp` works, lists frozen, Poco PanVK slice baseline recorded; see `vk/deqp/README.md` (open: 137 crash artefacts, CMF baseline, determinism run).*

- **Binary**: VK-GL-CTS tag matching Vulkan 1.3 mustpass (`vulkan-cts-1.3.10.x`), `deqp-vk` only, built for aarch64 glibc with the
  guest toolchain (`DEQP_TARGET=default`, no X11). One binary then serves the oracle and us through the same loader, in the
  environment games run in. Needs network once for `external/fetch_sources.py`; about 1.5 h of build on 4 cores.
- **Bionic variant** (`DEQP_TARGET=android`, `DEQP_ANDROID_EXE=ON`, run with `--deqp-vk-library-path=<our .so>`): built only if
  the guest run projects more than 12 h per phone for the full list, since proot taxes every ioctl.
- **Case list**: from `external/vulkancts/mustpass/main/vk-default/`: `api.txt`, `pipeline/monolithic.txt`, `renderpass.txt`,
  `renderpass2.txt`, `dynamic-rendering/*.txt`. Frozen as `vk/deqp/r3-list.txt` in M3 together with the feature and extension
  set R3 declares, so that NotSupported cannot be used to shrink the denominator later. A fixed 1/10 sample is the daily list.
- **Runner** `scripts/quick/qdeqp` (host Python, adb): pushes chunks of 500 cases as `--deqp-caselist-file`, 60 s per-case
  watchdog, parses the QPA log; on a crash or hang the case after the last finished one is recorded as Crash and the chunk
  restarts behind it. Results append to a TSV, so a run resumes after adb drops or a reboot. Output: per-group table and a
  diff against the oracle baseline. `--deqp-log-images=disable --deqp-log-shader-sources=disable`, results on `/data/local/tmp`.
- **Thermals**: the runner pauses above the battery temperature used by `qpreflight`; pass/fail must not depend on it.

## 6. Reuse map

| Area | Reuse unmodified (Mesa, MIT) | Write ourselves |
|---|---|---|
| ICD, instance, physical device, device | `vk_instance`, `vk_physical_device`, `vk_device`, entry point generators | Probe, properties, features, limits |
| Memory | `vk_device_memory` base | BO types, cache maintenance, dma-heap import/export (on libkbase) |
| Sync, queue | `vk_sync`, `vk_sync_timeline`, `vk_fence`, `vk_semaphore`, `vk_queue` (submit thread) | `mali_sync` on sync_file/KCPU, `driver_submit` |
| Buffers, images, samplers | `vk_buffer`, `vk_image`, `vk_image_view`, `vk_sampler`, `vk_format`; `pan_format`, `pan_image`, `pan_texture` | Bind, view and sampler descriptor glue |
| Descriptors | `vk_descriptor_set_layout`, `vk_pipeline_layout`, `vk_descriptor_update_template` | Pools, sets, table layout, NIR descriptor lowering |
| Shaders, pipelines | `vk_shader`, `vk_pipeline`, `vk_pipeline_cache`, `vk_graphics_state`, `disk_cache`; spirv_to_nir, NIR, `bi_*` backend | `mali_compile()`, I/O and sysval lowering, varying layout, shader descriptor packing |
| Render pass | `vk_render_pass`; `pan_fb`/`pan_desc` | Begin/EndRendering, tiler context, fragment job |
| Command stream | `cs_builder.h`, genxml packers | All CS emission: draws, dispatch, queue init, sync ops |
| Meta | `vk_meta_*` | The hooks it needs |
| WSI | `wsi_common`, `wsi_common_wayland` | Image allocation, fence import/export, gamescope → Android leg |
| Kernel | none | libkbase (exists) |

Estimated own code for R3: 12–15k lines of C.

## 7. Licensing

- Our code stays MIT, SPDX header in every file.
- Mesa is MIT with a few permissively licensed parts we link (blake3: CC0/Apache-2.0, xxhash: BSD-2, SPIR-V headers:
  Khronos MIT-style). Static linking means binary releases must carry those notices: `THIRD-PARTY-NOTICES` generated from
  Mesa's `docs/license.rst` plus the Mesa tag and commit, shipped next to the `.so`.
- LLVM is used only by the host build tool `mesa_clc`, not linked into the ICD.
- `libelf` (LGPL/GPL) is on PanVK's link line, which `build.sh` borrows, but our `.so` does not end up depending on it
  (`--as-needed`). M2 adds a `check.sh` assertion that it never appears in NEEDED. SPIRV-Tools is Apache-2.0 (notice
  needed if bundled).
- No GPL source, no ARM DDK source, no blob disassembly enters the tree. kbase uAPI knowledge stays in libkbase, written from
  headers and observed behaviour as before.

## 8. Risks and kill criteria

| # | Risk | Signal | Action |
|---|---|---|---|
| K1 | Effort: plan is 40 days against 30 | More than 26 days spent when M9 passes (plan 22) | Stop, re-plan with the owner; the cheapest lever is lifting the no-copy policy for PanVK's descriptor lowering and draw emission. |
| K2 | v13 lags (RUN_IDVS2, untested features) | M10 not passed after 2 + 3 days | Ship R3 numbers for v11, keep v13 as a named blocker with oracle CS traces; do not start M14 on v13. |
| K3 | Intermittent empty draw returns under dEQP load (INTERMITTENT.md) | Flake rate > 0.1 % after M9 | Stop feature work, 2 days root cause; if open, submit through the ioctl kick only and re-measure. |
| K4 | Mesa runtime does not fit without patches | More than 20 local Mesa patches, or any patch changing runtime semantics | Reassess against building inside a Mesa fork. |
| K5 | Gate missed | After M16: < 95 % and more than 2 points below the oracle on the same list | 3 more days if the top 3 failure causes cover the gap; otherwise architecture kill 4 (move our lower layers into the fork). ≥ oracle − 2 but < 95: accept with the gap list. |
| K6 | dEQP too slow in the guest | Full list projects > 12 h per phone | Build the bionic `deqp-vk`; if still > 24 h, gate on the frozen 1/10 sample plus one full run per week. |
| K7 | Compiler bugs in the `bi_*` backend | More than 1 % of failures traced to code generation that the oracle passes | Build Kraid (panfrost-rust) and A/B behind `MALI_COMPILER`; not before. |
| K8 | Implicit sync for present | `DMA_BUF_IOCTL_IMPORT/EXPORT_SYNC_FILE` missing on either kernel (still unmeasured) | CPU-wait the render fence before attach in M14; record the latency cost, revisit in R6. |
| K9 | GPU faults that need a reboot stall unattended runs | More than 1 per 10k cases | Fault decode from libkbase into the runner log and a context reset path before continuing the burn-down. |
| K10 | Bionic build drags in dependencies | More than 2 Mesa patches | Drop the bionic flavour, test through the guest only. |

## M2 status (2026-11-16): done

- Cross builds: `vk/cross/build-cross.sh` (glibc: sysroot cross file, `-Dllvm=disabled`, host `mesa_clc`/`vtn_bindgen2`/`panfrost_compile`
  from `build-vk`; bionic: NDK r27d API 29). **Bionic needed 0 Mesa patches** (K10 not triggered): libdrm via Mesa's own wrap
  (`-Dallow-fallback-for=libdrm`, its objects linked in), three NDK gaps (`cutils/native_handle.h`, `pthread_cancel`, `sync_wait`)
  filled by the force-included `vk/cross/shim/` (out of tree). `.so` NEEDED: bionic `libm libz liblog libdl libc` (libc++ static),
  glibc `libdrm libz libzstd libstdc++ libm libc`. No libelf (check.sh asserts it). The bionic `.so` exports the 3 `vk_icd*` symbols,
  `vkGetInstanceProcAddr` is reachable through `vk_icdGetInstanceProcAddr` (direct mode of `t_icd` uses exactly that).
- Backend: `src/mali_backend_kbase.c` (libkbase ctx/bo/queue/sync/heap compiled in): probe = `kb_open`, `kb_props` (gpu_id, arch,
  popcount(shader_present) cores), uAPI version, heap = RAM/2, name "Mali-G615/G925 MCn"; probe closes its fd. Device create =
  `kb_open` + one 4 KiB CS queue (group), destroy = `kb_queue_destroy` + `kb_close`. Texture-feature probing is not done yet (M3).
- Hardware (8 runs of 10): bionic direct via `adb shell` and guest (direct dlopen + system loader 1.4.341 + vulkaninfo 1.4.341
  aarch64 built from Vulkan-Tools, run with its own loader) on both phones. CMF: `Mali-G615 MC2 (mali-own)`, deviceID 0xb8a3, 3711 MiB
  heap; Poco: `Mali-G925 MC11 (mali-own)`, deviceID 0xd830, 5634 MiB. Device + queue create/destroy pass; fd count back to the
  start value (6 = 6 = 6), no `/dev/mali*` fd left; libkbase prints no leak warning. Runner for the guest: `vk/tests/m2guest.sh` via `qdrv`.
- Known gaps: guest has no `vulkaninfo`/dev loader; the guest run shows `mali_fds_live:0` because proot hides the `/dev/mali0`
  readlink, so the leak check there rests on the total fd count. Mesa shader compile (`mali_compile*`) is not linked yet.

## M4 status (2026-11-16): offline done, CMF hardware PASS (Poco and dEQP pending)

Files: `vk/src/mali_memory.c`, `mali_sync.c`, `mali_queue.c/.h`, `tests/t_queue.c`; backend seam extended (BO alloc/import/export/cache, submit_signal, fence_wait, device_lost) in `mali_backend{,_kbase,_fake}.c`.
Wiring edits: `mali_private.h`, `mali_device.c`, `mali_physical_device.c`, `mali_instance.c`, `build.sh`, `check.sh`.

- Offline (`vk/check.sh` and `libkbase/check.sh`, fake backend): all pass.
- **CS_CONFIG_FAULT root cause: a completely full ring.** With `CS_INSERT - CS_EXTRACT == ring size` the firmware raises CS_CONFIG_FAULT
  (status 0x40, sideband 0) the moment it looks at the queue. `kb_queue_submit` accepted that state (`>` instead of `>=`) and the backend's
  `wait_ring_space` let the 64th 64-byte submission in. An unwaited burst fills the 4096-byte ring while the GPU is still powering up
  (failing dump: extract 0, insert 4096, CQS counter 0, no CS executed), which is why it showed "after a few CSs" and never with waits.
  Not the cause (all pass with one slot kept free): head flush, flush-id register, trailing WAIT, kick ioctl vs doorbell, GPU-cached ring.
  Fix: `libkbase/src/queue.c` returns -EAGAIN unless one 64-byte unit stays free (-EINVAL for a CS >= ring); `wait_ring_space(k, bytes)`
  waits for `pending + bytes + 64 <= size`. Rule for M8: ring capacity is size - 64.
- CPU-cached (type 1) BOs came back from the kbase pool with old contents (also after `dc civac`, so real data, not stale lines).
  `kbase_bo_alloc` now zeroes them and cleans the cache (`memset` + `kb_bo_cache_flush_inv`); `t_queue mem` requires zero for both types,
  including a re-allocation of dirtied pages.
- CMF results (3 adb sessions, logs `runs/20261116-cmf-m4/run{1,2,3}-*.log`): `idle` 300 and `--burst 10000` (64- and 128-byte CSs), `submits` 10000
  (0 lost fences, mean 745 us, max 2.5 ms), `mem`, `dmabuf`, `binary` 200, `timeline` 500 (639 us per ping-pong), `syncfile`,
  `fault` with `MALI_FAULT_INJECT=1` and `=50` (DEVICE_LOST at the injected submit, DEVICE_LOST afterwards), full run: all PASS, fds 6 = 6.
  Soak: 10 x (burst 20000, full run with 3000 submits, 128-byte burst 5000) = 30/30 processes, 36 s.
- Gaps: Poco not run (reserved for dEQP); dEQP groups pending; waits are CPU-side only; no sparse; the libkbase fake backend does not model
  the full-ring fault (offline tests cannot catch a regression); the ring is 4096 bytes (63 queued submissions, then the submit thread polls).
- Env: `MALI_FAULT_INJECT=n`, `MALI_FAKE_SLOW_US`, `MALI_BO_COHERENT_LOCAL=1`, `MALI_SUBMIT_DBG` (0x80 = dump queue state and ring on a failed
  submit, 0x20 = 128-byte submissions). Test: `t_queue --burst N` sets the unwaited submit count of `idle`.

## M5 status (2026-11-16): offline done (descriptor bytes = libkbase golden on v11 and v13), not wired, no hardware

- Files (all new, none of the M4-owned files touched): `vk/src/mali_format.{c,h}` (format table from the linked `pan_pipe_format_v11/v13` and
  `pan_blendable_formats_v11/v13`; format/image-format properties; entrypoints `mali_GetPhysicalDevice{Format,ImageFormat,SparseImageFormat}Properties2`),
  `mali_image.{c,h}` (layout, plane/texture packers, images, views), `mali_buffer.c` (buffers, texel-buffer views, `mali_res_ops`), `mali_sampler.c`,
  `tests/t_image.c` + `tests/build-t_image.sh`.
- Verified offline in 0.00 s on v11 and v13: 8x8 RGBA8 texture and plane words byte-equal to `kb_draw_texture_2d_rgba8` and `kb_draw_plane_linear`; RGBA8 `hw`
  187<<12, RT tilebuffer 237<<12, writeback 19; RGBA32F attribute word0 `0x2fc00115` (v10+ encoding, equals `kb_draw_attr_1d`); D16/D24X8/D32 ZS codes 1/4/14 and 0/1/2;
  sampler equals `kb_draw_sampler_nearest_clamp` except bits 21 (round to nearest even) and 26 (clamp integer array index), the genxml defaults libkbase omits; texel buffer
  descriptor; layout sizes (rows 64 B aligned, mips, arrays, 3D, 4x MSAA, tiled, D32S8 two planes); every advertised format has a texture/RT/storage/vertex mapping and a valid layout.
- Layout: linear everywhere by default (64 B row/slice/image alignment). u-interleaved 16x16 exists, enabled with `MALI_IMAGE_TILED=1`, not hardware-verified. Buffers align 256.
  D+S formats are two planes (depth, then S8). Limits: 2D 4096 (13 mips), 3D 256, 256 layers, samples 1 and 4.
- Wiring edits for the M4/M2 owner (not done here):
  1. `build.sh`: add `mali_format.c mali_image.c mali_buffer.c mali_sampler.c` (they need `mali_private.h`/`mali_queue.h`, `libpan-format-v11/v13.a`, `libmesa_util.a` for `util_format_description`).
  2. Entrypoint table: `mali_GetPhysicalDeviceFormatProperties2`, `...ImageFormatProperties2`, `...SparseImageFormatProperties2` (physical device); device: `mali_CreateBuffer/DestroyBuffer`,
     `GetDeviceBufferMemoryRequirements`, `BindBufferMemory2`, `CreateBufferView/DestroyBufferView`, `CreateImage/DestroyImage`, `GetImageMemoryRequirements2`,
     `GetDeviceImageMemoryRequirements`, `BindImageMemory2`, `GetImageSubresourceLayout2` (also under the `KHR` name), `GetDeviceImageSubresourceLayout`, `GetImageSparseMemoryRequirements2`,
     `CreateImageView/DestroyImageView`, `CreateSampler/DestroySampler`. The 1.0 variants come from `vk_common_*`.
  3. Per device at init: `mali_desc_set_res_ops(&mali_res_ops)`; `mali_image_set_mem_ops(...)` and `mali_desc_set_mem_ops(...)` with a BO sub-allocator (default is malloc, va = host pointer).
  4. `mali_physical_device.c` limits/features must stay equal to the `MALI_MAX_*` constants in `mali_format.h`.
- Not done: ASTC/YUV/ETC1 (ASTC needs the ASTC plane descriptor); sparse; tiled layout and MSAA on hardware; S8 as a separate plane in the FBD; storage-image descriptor variant;
  writeback/internal formats for non-blendable RTs (pure int: only the `hw` word); cube-array `array_size` semantics; DRM modifiers (M14); linear vs optimal feature split is the same today;
  `arch` is 11 or 13 only (other `pan_*_vN` tables are not linked). dEQP numbers need the phone.

## M5/M7 wiring (2026-11-16): done offline, no hardware

- `build.sh` compiles in `mali_format/image/buffer/sampler/descriptor/descriptor_lower/pipeline_layout/compile/resources.c` and links the pan_format +
  panfrost compiler static libs (`PANLIBS`; no PanVK object); also builds `out/<target>/t_resources` (static link of the driver objects, white-box).
- New `src/mali_resources.c`: `mali_resources_init()` (called from `mali_CreateDevice`) installs `mali_res_ops` (descriptor resolvers -> real image view/sampler/buffer/buffer view objects)
  and the BO-backed `mali_desc_mem_ops`/image-view payload pool (one M4 backend BO per allocation, 64 B header holds the `mali_bo`; sub-allocation later), plus the device
  entrypoints `mali_{Create,Destroy,Reset}DescriptorPool`, `AllocateDescriptorSets`, `FreeDescriptorSets`, `UpdateDescriptorSets`, `UpdateDescriptorSetWithTemplate`,
  `CreateDescriptorSetLayout`, `GetDescriptorSetLayoutSupport`. Template create/destroy, `DestroyDescriptorSetLayout` and `vkCreatePipelineLayout` are the runtime's `vk_common_*`.
- `mali_physical_device.c`: M1 placeholders for format/image-format/sparse properties removed (mali_format.c provides them); device extension `VK_KHR_descriptor_update_template`
  enabled (the device apiVersion is still 1.0, templates are core only from 1.1). `vulkaninfo --show-formats` lists 325 format entries.
- Tests: `check.sh` runs `t_resources` on v11 and v13 (buffer+image+view+sampler+texel view, memory, binds, layout/pool/set, UpdateDescriptorSets and template; packed bytes compared
  with the packers; flatten of the pipeline layout). t_icd, t_queue, t_image, t_desc, t_compile unchanged and passing. glibc and bionic cross builds build libvulkan_mali.so + t_resources.
- Not done: freed pool BOs are not shared (one BO per pool/view), CmdBindDescriptorSets (M8), pipeline creation with `mali_compile(layout)` (M6/M8), dEQP numbers, hardware.

## M8 status (2026-11-16): offline done (CS stream = libkbase t_uniform on v11 and v13), not wired, no hardware

- New files (`vk/src`): `mali_cs_emit.{h,c}` (Mesa-free core: frame -> descriptors + CS, uses libkbase `kb_draw_*`, `kb_v13_*`, `kb_fb_pack`, `kb_cs_*`, `kb13_*`), `mali_render.c` (FBD v11/v13),
  `mali_cmd.{h,c}` (`vk_command_buffer` ops, Begin/End, bind/push/draw/rendering/clear entrypoints, `mali_cmd_submit`). Recording only builds a CPU-side `mali_frame`; descriptors and CS are built at submit from a rolling arena (INTERMITTENT.md).
- Test: `vk/tests/build-t_cmd.sh` records the t_uniform scenes through the Vulkan entrypoints on the fake backend, diffs the dump against libkbase `t_uniform`/`t_uniform_v13` field by field (only GPU VAs allowlisted), runs `decode_stream.py` static rules. Passes, 1.7 s run.
- Wiring needed in existing files (not done):
  1. `dev->vk.command_buffer_ops = &mali_cmd_ops` in `mali_device.c`.
  2. `mali_queue_submit` calls `mali_cmd_submit` per command buffer (M4 still sends an empty CS).
  3. Install `mali_cmd_set_hw` (alloc = `kb_arena_alloc`, job_begin = `kb_arena_begin`, submit = `kb_queue_submit`, env: heap, geo BO, sync BO) and `mali_cmd_set_res` (pipeline/image view/image/shader resolvers; M5/M6 own those objects).
  4. `build.sh`: add `mali_cs_emit.c mali_render.c mali_cmd.c` and libkbase `draw.c draw_v13.c fb.c`.
  5. v13 submit hook must do the CPU->device MEM_SYNC ioctl on arena/geo/vertex BOs; v13 BOs need `kb_bo_extra_flags = 0x800`.
  6. `vk_device_shader_ops::cmd_bind_shaders = mali_cmd_bind_shaders` when M6 lands.
- Deviations: arch is a runtime switch (`mali_env.arch`), not a per-arch compile. `mali_backend_ops` has no submit-CS op, no tiler heap, no bigger ring; `mali_kdev` is private to `mali_backend_kbase.c`. The CS ring is 4 KiB (512 instrs): large frames do not fit until the ring grows.
- Unsupported (command buffer gets `VK_ERROR_FEATURE_NOT_PRESENT`): indexed draws; colour loadOp LOAD; more than one colour attachment; depth-only passes; partial or post-draw `vkCmdClearAttachments` (a full-area clear before the first draw folds into loadOp CLEAR); depth-only `vkCmdClearDepthStencilImage`; formats other than RGBA8 colour and D32F/D24X8/D16; instance-rate attributes; non-opaque blend; dynamic descriptor offsets. `vkCmdClearColorImage` is RGBA8 only. firstVertex != 0 is UNVERIFIED.
- Not verified on hardware: everything above; v13 geometry-less (clear-only) frames in particular. No CALL encoder in libkbase.

## 9. Hardware need

M1 is the only milestone with an offline pass criterion. **M2 is the first that needs the phones** (real probe and device
create on both). M3 needs them too and is independent of M2. Offline work that can run ahead while phones are busy: the
cross builds of M2, the dEQP build of M3, all of M6, the layout and lowering unit tests of M7.

## M9 status (2026-11-16): triangle, draws, indexed draws, texture, UBO, descriptor update, push-constant FS, scissor, viewport, clearatt PASS on the CMF (x1000); M11 compute below

Files: `vk/src/mali_pipeline.c` (graphics pipelines, shader modules via `mali_compile`, VS+FS paired with varyings, `mali_cmd_res` resolvers, no-op
`vkCmdPipelineBarrier2`), `mali_queue.c` (`struct mali_hwctx`: 1 MiB arena, geometry/sync BOs, tiler heap; `mali_queue_submit` -> `mali_cmd_submit`),
backend ops `submit_cs`, `wait_counter`, `heap_create/destroy` + BO kinds `MALI_BO_EVENT`/`MALI_BO_EXEC` (kbase, fake), `mali_device.c`
(`command_buffer_ops=&mali_cmd_ops`, `mali_hw_init/finish`), `build.sh` (M8 sources + libkbase `draw.c`, `draw_v13.c`, `fb.c`), `check.sh`,
`tests/t_vk_draw.c`, `tests/build-t_cmd.sh` (now links `mali_pipeline`).

- Arch is picked at runtime (`arch_major >= 12` -> v13: hmask 0, tf_or 0x40000, tile_log2 12; else v11 hmask 0xfe). v13 is NOT run on hardware; MEM_SYNC
  over tracked BOs runs only with `MALI_BO_COHERENT_LOCAL=1`.
- Frames over the 4 KiB ring: the backend splits the CS into 256-instruction pieces, each waits for ring space (registers persist, no chaining).
- Every frame is waited to completion before the next is built (event BO counter), so the rolling arena cannot be overwritten in flight (INTERMITTENT.md).
  Serial and slow (~2.1 ms per triangle frame), safe.
- Bug found on hardware: `struct vk_graphics_pipeline_state` must be zero-initialised before `vk_graphics_pipeline_state_fill` (garbage `ms`, `ia` and a failing
  Mesa assertion in debug/bionic builds; the NDEBUG native build hid it). Triangle topology is read from `pInputAssemblyState`.
- Limits: triangle lists only, no compute pipelines (`FEATURE_NOT_PRESENT`), RGBA8 colour, no indexed draws, no colour loadOp LOAD, no depth-only passes,
  static pipeline viewport/scissor not applied (use dynamic), process-wide one-device hw seam, device apiVersion 1.0 so use render passes.
- Offline: `vk/check.sh` (vk_draw v11/v13, loops 200/1000 with reset), `tests/build-t_cmd.sh`, `libkbase/check.sh`: all pass. Fake backend renders nothing, so pixel checks are skipped there.
- CMF (Mali-G615, 5 adb sessions, logs `runs/20261116-cmf-m9/`): triangle 8192 px (0.000% error, vertex colours and centroid ok), two draws 16384 px,
  depth LESS 12288 px with the second draw rejected in the overlap (centre pixel = first triangle), 1000 rotated frames x2 (with and without `--loop-reset`),
  soak 20/20 processes x 200 frames. No faults.
- Not done: texture quad, uniform buffer, descriptor-set client cases (needs a VS with a vec2 uv output; `desc.frag` mismatches `rot.vert`), Poco/v13 hardware.
- Env: `MALI_PIPE_DBG=1` prints unsupported pipeline state, `MALI_FRAME_TIMEOUT_MS` (default 10 s).

### M9 rest (2026-11-16, CMF, logs `runs/20261116-cmf-m9rest/`)
Indexed draws (idx16, idx32) and the cases scissor, viewport, clearatt, ring, pushfs, ubo, descupd, texture pass offline and on the CMF (loops up to 1000, with and without loop-reset).

## M11 status (2026-11-16): compute works on the CMF (Mali-G615 MC2, v11); logs `runs/20261116-cmf-m11/`
- Done: compute pipelines, bind, push constants, `vkCmdDispatch`, `vkCmdDispatchIndirect`. Pass at x1000 on the CMF: saxpy, saxpy_ind, atomic (100 groups), atomic256, atomic_ind, scan (64x256, shared memory + barriers). Offline check passes on v11 and v13.
- v11 requires COHERENT_LOCAL (0x800) BOs: without it SSBO atomics run per core in L1 and updates from different cores are lost (count about half from 8 groups). It is now the default on arch 11; `MALI_BO_COHERENT_LOCAL=0/1` overrides.
- Task split: axis X, increment 1. Compute SPD stage 1 (v11 bits 9:8 cleared). Indirect: CS `LOAD_MULTIPLE` into r37-39 then waits on the LS slot.
- WLS: instances <= 16, fixed core-id range 16, total capped at 384 KiB. Worked on the CMF without real GPU properties.
- Not supported: `num_workgroups` / DispatchBase sysvals, TLS-spilling compute shaders (TLS size > 0 rejected). v13 compute is offline-only (not run on hardware). Indirect draws and dEQP compute coverage not measured.

## M14 status (WSI: passed on CMF, 9.3 min instead of 10; Poco/dEQP/PanVK pending)

- **Driver**: `src/mali_wsi.c` (`wsi_device_init`, `supports_modifiers`), `KHR_surface`/`KHR_wayland_surface`/`KHR_get_surface_capabilities2`, device `KHR_swapchain`, `KHR_swapchain_mutable_format`, `EXT_image_drm_format_modifier` (LINEAR only). Swapchain images are our linear images exported as dma-buf (`vkGetMemoryFdKHR`).
- **K8 resolved as implicit sync**: wsi_common exports a sync_file from the present semaphore and imports it into the dma-buf; no CPU wait. No explicit-sync protocol (gamescope has none).
- **Build**: Wayland-enabled Mesa in `third_party/build-vk-a64-glibc-wl` (`-Dplatforms=wayland`; wayland-scanner version wrapper in `vk/cross/hostbin`). `cd vk; MESA_BUILD=$PWD/../third_party/build-vk-a64-glibc-wl MALI_TARGET=a64-glibc-wl ./build.sh`. Native `check.sh --no-build` passes.
- **Test**: `vk/tests/t_vk_present.c` (loader-free vkcube-like textured depth-tested cube; `tests/build-t_vk_present.sh`); CMF run with `scripts/quick/qvkpresent <out> [--mode fifo|mailbox|immediate] [--resize-test]`. Archives: `runs/20261117-cmf-m14/`.
- **Exit-code bug (fixed)**: SIGSEGV in `vkDestroySwapchainKHR`: wsi_common calls `DestroyQueryPool(VK_NULL_HANDLE)` per image and the driver has no queries entrypoint (NULL). `mali_wsi.c` now guards that callback. Revisit when M12 adds queries (guard stays harmless).
- **CMF results** (1280x720, 4 images, XB24/AB24 linear via gamescope, 0 client copies/frame, screenshot `mailbox/shot2.png` shows the textured cube plus the DroidDeck HUD):
  - fifo 560 s (guest timeout cap 600 s): 63557 frames, **113.5 fps** (per-second 86-122), mean 8.81 ms, p50 8.38, p99 19.1, max 53.4, **3 frames >50 ms**, rc 0.
  - mailbox 12 s: 140.9 fps, p99 26.0 ms, 0 >50 ms (HUD 132 fps).
  - fifo + `--resize-test` (1280x720 to 640x360 mid-run, oldSwapchain): clean, pass, 1 recreate, 0 out-of-date.
  - Note: fifo is paced at ~113-117 fps, not the 120 Hz panel rate exactly (within the ±5 % target). Fifo vs. mailbox via wsi on gamescope is unchanged by the driver.
- **Limits / pending**: `BindImageMemory2` with swapchain info unsupported; only R8G8B8A8_UNORM swapchain; 1.0 device (render passes). Not done: Poco run, dEQP `wsi.wayland`, PanVK/bridge comparison, test teardown leaks 5 BOs (test resources not freed).

- **BC decode (R4-M7) uses the M12 meta compute path**: new compute jobs (`mali_bc*`, shaders `bc1/bc4/bc6h/bc7`) need a meta materialise path for shader ids >= `MALI_META_NSHADERS`; see `vk/R4-GAP.md` 'BC'.

## M12/M13 status (wiring + CMF hardware, in progress 2026-11-17)
- Offline (earlier): meta planner/reference (`mali_meta*`), query core, ops-frame CS builder (`mali_event*`), `occlusion-cs-emit` (since merged, M13). Not wired before this section.
- Wiring plan (one worker owns `vk/src`): (1) `mali_frame` gets an ops list (`nops/ops`), `mali_frame_build` dispatches to `mali_ops_build`; (2) `mali_cmd.c` exports
  `mali_cmd_push_frame / mali_cmd_push_ops / mali_cmd_set_occlusion`, ops recorded inside a render pass are deferred to after the pass frame; (3) occlusion patch
  applied to `mali_cs_emit.[ch]` (clean), `mali_draw.occ_*` filled from the command buffer; (4) `mali_meta_set_dev` installed in `mali_hw_init`; (5) events
  (`vkCreateEvent/SetEvent/GetEventStatus/CmdSetEvent2/CmdResetEvent2/CmdWaitEvents2`) and `CmdPipelineBarrier2` (flush op) in new `mali_sync2.c`;
  (6) clears: render-target fast path for RGBA8 stays in `mali_cmd.c`, everything else falls to `mali_meta_CmdClear*Image`;
  (7) build lists: build.sh, tests/build-t_cmd.sh, cross/build-cross.sh (via build.sh).
- Status of each item is appended below as it lands. HW results: see "Results".

### M12/M13 results (CMF G615 MC2, 2026-11-17, logs `runs/20261117-cmf-m12/`)
- All wired: ops frames, deferred in-pass ops, meta clears/copies/blits/resolve, events (`mali_event_vk.c`), timestamps, occlusion (patch applied), meta device seam in `mali_hw_init`; build.sh/t_cmd lists updated (`t_vk_meta` added to build.sh/check.sh).
- Offline: `check.sh` OK (v11+v13, incl. `vk_meta`), `t_cmd` incl. `--m13` v11/v13, t_meta/t_query/t_qpool pass. Decoder quirk: decode_stream carries `saw_run`/scoreboard state across rings, so a dump with two RUN frames gives a false failure; `t_cmd --m13` dumps only its own frames.
- HW: `t_vk_meta` buf/fill/img/copyimg/blit/clearc/cleard/resolve(4x MSAA)/event/ts all pass, 1000 loops (24 s); `t_vk_draw --only occlusion` 1000 loops exact (samples == covered px, empty query 0); M9 draw (all cases) and M11 compute x20 still pass; M14 WSI fifo rc 0.
- Bugs found on HW and fixed: MSAA colour clear only cleared sample 0 (`mali_meta_vk.c clear_range`); events not zeroed on create (recycled device memory); `meta_qcopy.comp` read value from y/z instead of z/w (shader regenerated).
- M14 test leak: 5 BOs -> 1 left at teardown (remaining one not identified).
- Unresolved: `timestampPeriod` stays 1.0f, unverified against the GPU timestamp counter (delta ~48000 ticks for a 1 MiB fill). Not a git repo: nothing committed. Events/barriers: frames are serialised, device-side wait only exercised for host/device set.
