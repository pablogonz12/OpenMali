# Architecture of `vk/` and `libkbase/`

Orientation for the two code trees: how a Vulkan call becomes a CSF command stream, where memory and
synchronisation live, how frames reach the compositor, and how shaders are compiled. Line numbers are
pointers as of this commit and drift; search for the symbol if one is off. Design intent and the staged plan are in
`ARCHITECTURE-proposal.md`, `ROADMAP.md` and `R3-PLAN.md`/`R4-PLAN.md`; this file describes what the code does.

## 1. Layers

```
 Vulkan app / loader
      |  vk_icdGetInstanceProcAddr                      vk/src/mali_instance.c:263
 vk/src/mali_*.c        Vulkan front end on Mesa's vk_* runtime (instance, device, queue, cmd, pipeline, descriptors, WSI)
      |  struct mali_hw (callbacks: alloc, submit, drain)     vk/src/mali_cs_emit.h, bound in mali_queue.c:403
 vk/src/mali_cs_emit.c  frame -> CS instruction stream + descriptors (v10/v11/v13)
      |  struct mali_backend_ops                        vk/src/mali_backend.h:59
 vk/src/mali_backend_kbase.c  |  vk/src/mali_backend_fake.c    (real /dev/mali0  |  offline, no GPU)
      |  kb_* API                                       libkbase/include/kbase.h
 libkbase/src/          A ctx.c, B bo.c, C queue.c, D sync.c, E heap.c, plus descriptor packers fb.c draw.c draw_v13.c
      |  ioctl / mmap on /dev/mali0 (uAPI subset in include/kbase_uapi.h)
 kernel mali_kbase (CSF)
```

`libkbase` is standalone C11 with libc only and no Mesa dependency (`libkbase/README.md`). Only `vk/` links Mesa
(runtime, WSI, NIR, SPIR-V, panfrost compiler); PanVK is built only to obtain those static libraries and is not linked
(`vk/README.md`, "The PanVK target"). The cross build uses 0 Mesa patches (`vk/README.md`); `vk/patches/` holds pending patches to our own hot files.

### libkbase (`libkbase/src`)

| Layer | File | Role |
|---|---|---|
| A context | `ctx.c` | `kb_open` (`ctx.c:191`): version handshake, GPU props parse (`ctx.c:81`), CSF setup, `MEM_EXEC_INIT` + `MEM_JIT_INIT`. Capability table keyed by uAPI version (`kb_caps_lookup`, `ctx.c:72`); other versions are refused. `kb_ioctl` retries EINTR (`ctx.c:56`). |
| B memory | `bo.c` | `kb_bo_alloc` (`bo.c:20`), `kb_bo_import_dmabuf` (`bo.c:84`), cache maintenance `kb_bo_cache_flush_inv` (`bo.c:168`), dma-heap allocation (`bo.c:195`), dma-buf sync_file import/export (`bo.c:217`, `bo.c:226`). |
| C queue | `queue.c` | One CSF group with one queue. `kb_queue_create` (`queue.c:17`), `kb_queue_submit` (`queue.c:123`: write ring, bump `CS_INSERT`, doorbell or ioctl kick), `kb_queue_extract/idle/faulted` (`queue.c:119-121`), notification decode `kb_event_read` (`queue.c:213`). |
| D sync | `sync.c` | KCPU queue: CQS wait (`sync.c:52`), CQS set (`sync.c:71`), fence signal returning a sync_file fd (`sync.c:90`), `kb_fence_wait` (`sync.c:118`). |
| E tiler heap | `heap.c` | `kb_tiler_heap_create` (`heap.c:10`); needs `MEM_JIT_INIT` from `kb_open`. |
| Encoders | `include/kbase_cs.h`, `kbase_cs_v13.h` | CS instruction encoders (`kb_cs_*`, e.g. `kb_cs_move48` at `kbase_cs.h:46`). |
| Packers | `fb.c`, `draw.c`, `draw_v13.c` | FBD and render-target packers (`kb_fb_pack`, `fb.c:41`); v11 IDVS descriptor packers (`draw.c`, per `kbase_draw.h`); arch-13 differences (`draw_v13.c:9` tiler ctx, `:20` SPD, `:68` FBD HSR). Plain arena allocator `kb_arena_alloc` (`draw.c:21`). |
| Fake | `backend_fake.c` | `KB_FAKE` mode: interposes `open/ioctl/mmap` and dumps submitted streams as hex, for offline stream checks (`backend_fake.c:193`, kick handler `:92`). |

Hard-won hardware behaviour (uAPI 1.21 quirks, ioctl kick vs doorbell, non-coherent GPU, SIGBUS on imported mappings)
is listed in `libkbase/README.md` "Notes learned" and `libkbase/INTERMITTENT.md`; read those before changing
`queue.c` or `bo.c`.

### vk front end (`vk/src`)

| Area | Files |
|---|---|
| Instance, trace, ICD entry | `mali_instance.c` (`mali_GetInstanceProcAddr` `:252`, trace/fatal-signal helpers `:50-173`) |
| Physical device, properties, memory types | `mali_physical_device.c` (`mali_enumerate_physical_devices` `:302` calls `mali_backend_select`, `:305`; properties `:18`) |
| Device, entrypoint table per arch | `mali_device.c` (`mali_arch_entrypoints` `:15`) |
| Queue, submission, frame retirement | `mali_queue.c`, `mali_sync.c`, `mali_event*.c`, `mali_query*.c` |
| Command buffers (record) | `mali_cmd.c` |
| CS and descriptor emission (build) | `mali_cs_emit.c`, `mali_render.c` (FBD), `mali_meta*.c`, `mali_bc.c` |
| Pipelines and shaders | `mali_pipeline.c`, `mali_pipeline_layout*.c`, `mali_compile.c`, `mali_descriptor*.c` |
| Resources | `mali_memory.c`, `mali_buffer.c`, `mali_image.c`, `mali_sampler.c`, `mali_format.c`, `mali_resources.c` |
| WSI | `mali_wsi.c` |

`cs_emit.c`, `cmd.c` and `pipeline.c` are the largest files and the most contended; changes to them are normally
delivered as `vk/patches/` plus a note rather than in-place edits during parallel work.

## 2. Command flow

Recording and building are deliberately separate: `vkCmd*` only snapshots state; no CS is written until submit.

1. **Record** (`mali_cmd.c`). `vkCmdBindPipeline` (`:147`), vertex/index/descriptor/push-constant binds (`:264-351`)
   update `mali_cmd_buffer` state. `snapshot_draw` (`mali_cmd.c:358`) copies the current state into an immutable
   `mali_rec`/`mali_draw` record in a frame pool (`mali_cs_emit.c:1142-1215`: `mali_frame_blk`, `mali_frame_add_draw`).
   Draws go into the open frame; `vkCmdBeginRendering` (`:800`) opens a frame and `vkCmdEndRendering` (`:1019`) closes
   it (`mali_frame_finish`, `mali_cs_emit.c:1247`). Dispatches become compute frames (`record_dispatch`, `:708`).
   Transfer, clear, blit and BC work are expressed as meta shader jobs (`mali_cmd_push_ops`, `:1044`) planned by
   `mali_meta_plan_*` (`mali_meta.c:129-205`) and executed as compute. Anything unsupported fails loudly through
   `unsupported()` (`mali_cmd.c:50`).
2. **Submit** (`mali_queue_submit`, `mali_queue.c:505`). Waits are resolved first (section 4), then
   `mali_cmd_submit` (`mali_cmd.c:1262`) runs per command buffer. Frames that read indirect arguments on the CPU force a
   drain of earlier work (`mali_cmd.c:1280-1291`, `expand_indirect` `:641`). All frames of one command buffer are built
   and concatenated into one stream.
3. **Build** (`mali_frame_build`, `mali_cs_emit.c:975`). Picks the path: meta ops (`mali_ops_build`), compute
   (`build_compute`, `:781`), or graphics. Graphics emits tiler context, shader program descriptors, attributes,
   blend/ZS, DCDs and the FBD (`mali_render.c`, built on `kb_fb_pack`), then `RUN_IDVS` and `RUN_FRAGMENT`. Arch is
   selected at runtime (`env->arch` 10, 11 or 13, `mali_cs_emit.c:979`; `mali_hw_init` maps arch_major to it,
   `mali_queue.c:403+`). Redundant register `MOVE`s are elided through a known-register cache (`known/set_known`,
   `:58-63`).
4. **Hand-off** (`hw_submit_multi`, `mali_queue.c:307`). Calls the backend `submit_cs`, then records per-frame completion expectations in a small ring of in-flight frames
   (`MALI_MAX_INFLIGHT` 3, `mali_queue.c:23`); `hw_retire` (`:238`) waits for the oldest and recycles its arena.
5. **Kernel** (`kbase_submit_cs`, `mali_backend_kbase.c:484`). Under `submit_lock`, splits the stream into pieces that
   fit the ring (`wait_ring_space`, `:309`), and calls `kb_queue_submit(..., KB_KICK_IOCTL)`.
   An unrecoverable error sets `k->lost`, which surfaces as `VK_ERROR_DEVICE_LOST`
   (`mali_cmd.c` error mapping, `mali_device_check_status`, `mali_queue.c:580`).

Fault handling: `kbase_wait_counter` (`mali_backend_kbase.c:511`) polls a counter while draining `kb_event_read`
for fatal notifications and checks `kb_queue_faulted`; a fatal group error is terminal for the device.

### Debugging aids

`MALI_PROF` (per-phase timing, `mali_queue.c:550`), `MALI_PIPE_DBG`, `MALI_FRAME_TIMEOUT_MS`, `MALI_BACKEND=fake`
with `MALI_FAKE_GPU=v11|v13`, and the stream dump of the fake backend (`mali_backend_fake.c:182`) with golden
decoded streams under `vk/tests/golden/` (regenerate with `vk/check.sh --bless`). Full list: `vk/README.md` "Environment".

## 3. Memory

Backend BO interface: `alloc`, `import`, `free`, `cache`, `export`, defined in `mali_backend.h:59` with flags at
`mali_backend.h:33-41`:

| Flag | Meaning |
|---|---|
| `MALI_BO_CACHED` | CPU-cached mapping; explicit `bo_cache()` (clean before GPU reads, invalidate after GPU writes). |
| `MALI_BO_EXPORT` | Backed by dma-heap, exportable as a dma-buf fd (WSI images, `VkExportMemoryAllocateInfo`). |
| `MALI_BO_EVENT` | CQS words the CS updates; kernel-visible, uncached. |
| `MALI_BO_EXEC` | GPU-executable (shader binaries). |
| `MALI_BO_HOT` | Rewritten by the CPU every frame (arena): `MEM_SYNC` at every CS submit. |
| `MALI_BO_HOSTLAZY`, `MALI_BO_NOCOH`, `MALI_BO_COH` | Control per-submit `MEM_SYNC` and `COHERENT_LOCAL` for device-only, write-combined and atomically written driver BOs. |

The GPU is not IO-coherent, so the kbase backend performs `MEM_SYNC` at submit and after waits (`mem_sync_all`/
`mem_sync_bos`, `mali_backend_kbase.c:446-478`; `kbase_wait_counter` calls it on success, `:515`). BO tracking for these
syncs is `track_bo` (`:163`); allocation policy is `kbase_bo_alloc` (`:189`), import `kbase_bo_import` (`:254`).

Vulkan view (`mali_memory.c`, `mali_physical_device.c:277`): one device-local heap and two memory types, type 0
HOST_VISIBLE|COHERENT (uncached, write-combined mapping) and type 1 HOST_VISIBLE|CACHED (cached mapping; `vkFlush/
InvalidateMappedMemoryRanges` do the `bo_cache` work, `mali_memory.c:127-160`). Allocation size is capped by
`gpu.heap_bytes` (`mali_memory.c:39`); exportable memory takes the dma-heap path (`:68`).

Driver-internal memory: a per-device arena (`MALI_BO_HOT`) allocated in `mali_hw_init` (`mali_queue.c:403`) holds
descriptors and the CS for one frame; overflow BOs are allocated, tagged with the frame counter, reaped on retirement
and pooled up to 256 MiB (`hw_alloc_ovf` `:132`, `hw_ovf_reap` `:219`). Thread-local storage comes from `hw_tls`
(`:283`). The tiler heap is `kbase_heap_create` (`mali_backend_kbase.c:541`) over `kb_tiler_heap_create`. Meta shaders
and their binaries live in executable BOs (`meta_upload_shader`, `mali_queue.c:105`; `shader_get_locked`,
`mali_meta.c:401`).

## 4. Synchronisation

The device has one in-order CS ring, so work submitted to it is ordered and ordering across submissions is free; only
CPU/GPU and cross-process edges need fences.

- **Per-submission fence.** `kbase_submit_signal` (`mali_backend_kbase.c:329`) appends a cache flush + wait and a
  `SYNC_ADD64` of a monotonically increasing counter in a CQS BO, then enqueues, on the KCPU queue, a CQS wait
  (`counter > target-1`) and a fence signal, and returns the sync_file fd. KCPU commands go in first, as in `t_submit`.
  Libkbase side: `sync.c:52`, `:90`.
- **vk_sync type.** `mali_sync` (`mali_sync.c`) is a sync_file-backed `vk_sync`; it supports import/export of sync_files
  (`:167`, `:190`) and is the point type under the runtime's emulated timeline semaphores. `mali_sync_init_physical`
  (`:224`) registers it. Binary semaphores and fences are signalled with the fd from `submit_signal` and the queue
  remembers the owner (`mali_queue.c:538-546`).
- **Waits.** `vkQueueSubmit` waits on the CPU (`vk_sync_wait`, `mali_queue.c:520`), except for a semaphore signalled
  earlier by this same queue, which is skipped because the ring already orders it (`:511-519`). This shortcut is valid
  only if present carries the frame's fence to the compositor, decided by `mali_present_fence_ok` (`:491`;
  overridden by `MALI_PRESENT_FENCE` or `MALI_QUEUE_CPU_WAIT`). GPU-side waits for foreign sync_files are not
  implemented.
- **Events and queries.** CS-updated words in `MALI_BO_EVENT` memory (`mali_event.c`, `mali_event_vk.c`,
  `mali_query_core.c`, `mali_query.c`).
- **Device loss.** Any failed submit or fault marks `k->lost`; `vk_queue_set_lost` is reported to the app.
- **Fence waits.** `kbase_fence_wait` (`mali_backend_kbase.c:393`) over `kb_fence_wait`; counter waits as above.

## 5. WSI

Presentation uses Mesa `wsi_common`, initialised in `mali_wsi_init` (`mali_wsi.c:58`) with Wayland (optionally X11,
see `vk/README.md`). There is no DRM node behind kbase, so `supports_modifiers` is forced and images are linear
(`mali_wsi_modifier` = `DRM_FORMAT_MOD_LINEAR`, `mali_wsi.c:17-18`).

- **dma-buf path (real GPU).** Swapchain images are ordinary driver images on `MALI_BO_EXPORT` BOs (dma-heap), exported as
  dma-bufs for the compositor (zero copy). `wsi_common` attaches the frame's sync_file to the buffer
  (`DMA_BUF_IOCTL_IMPORT/EXPORT_SYNC_FILE`, libkbase `bo.c:217-226`) so the compositor does not sample a half-drawn frame.
- **Software path.** On the fake backend, or with `MALI_WSI=sw`, wsi copies into a CPU-visible linear buffer
  (`mali_wsi_use_sw`, `mali_wsi.c:47`); `MALI_WSI=dmabuf` forces the first path.
- Details of the Steam/gamescope present leg and X11 are in `vk/STEAM-GAPS.md`; frame pacing and the in-flight limit
  interplay is `MALI_MAX_INFLIGHT` plus the wait elision in section 4.

## 6. Compiler path

`mali_compile` (`mali_compile.c:572`) is the single entry: SPIR-V in, Valhall (or Bifrost) binary plus metadata out
(`struct mali_compiled`, `mali_compile.h`). No shader compiler of our own: it reuses Mesa.

1. Validate (magic, arch 10/11/13, stage; `:574-584`) and derive FAU/push base defaults per stage and arch (`:585-590`).
2. **Cache.** Key `mkey` (`:155`) hashes SPIR-V, options, layout, specialisation and the reserved/push values; directory
   from `cache_dir` (`:211`: option, `MALI_SHADER_CACHE`, or `$XDG_CACHE_HOME/mali-gaming/shaders`); `cache_load/store`
   at `:263`/`:242`. Hit/miss counters in `mali_compile_stats` (`:612`).
3. **`do_compile`** (`:308`): `spirv_to_nir`, NIR lowering (sysvals `:59`, base instance `:87`, push base `:98`, texture
   handles `:120`, descriptor lowering from `mali_descriptor_lower.c` driven by the pipeline layout, ycbcr lookup `:298`),
   then the panfrost `bi_*` backend. The lowering sequence follows `compiler-spike/spike.c` (PanVK as oracle only; see
   `compiler-spike/NOTES.md`).
4. Blend shaders: `mali_compile_blend` (`:640`).

Callers: graphics pipelines (`mali_pipeline.c:163` VS, `:185` FS), compute pipelines (`:379`), and internal meta shaders
(`mali_meta.c:425`, preload shaders `:526`, `:545`; BC decode in `mali_bc.c`/`mali_meta.c:189`). Pipeline binaries are
uploaded to an executable BO and referenced by VA from the SPD descriptors packed by `kb_draw_spd` / `kb_v13_spd`.
Shader sources for meta/BC kernels are in `vk/shader/` (GLSL + prebuilt `.spv`, embedded as `mali_*_spv.h`).

## 7. Testing without hardware

- `vk/check.sh`: builds the ICD and runs the loader and direct-dlopen cases on the fake backend for v11 and v13, compares
  decoded streams to `vk/tests/golden/`, optional ASan/UBSan (`MALI_SAN=1`) and host dEQP (`--deqp-host`).
- `libkbase/check.sh`: libkbase with the `KB_FAKE` backend and `libkbase/golden/`.
- Hardware runs (phones) go through the farm and `vk/deqp/`; the fake backend cannot reproduce cache coherency,
  scheduling or fault behaviour, so a stream that matches golden is necessary, not sufficient.

## 8. Where to start

| Task | Start at |
|---|---|
| New Vulkan entry point | `mali_cmd.c` (record) then `mali_cs_emit.c` (emit); add a golden in `vk/tests` |
| New render-target format | `mali_rtfmt_tab.h`, `mali_render.c`, and every per-format switch (preload, resolve, clear) |
| New arch/uAPI | `libkbase/src/ctx.c` caps table, `kbase_cs_v13.h`, `draw_v13.c`, `mali_arch_entrypoints` |
| Sync or present bug | `mali_queue.c:505`, `mali_backend_kbase.c:329`, `mali_wsi.c`, `libkbase/src/sync.c` |
| Wrong colours or stale data | section 3 flags and `MEM_SYNC`; `libkbase/INTERMITTENT.md` |
