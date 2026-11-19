# Memory types, heaps, and what a device-local type can honestly be (M15)

Host-only analysis, 2026-11-19. No phone was used: every statement about behaviour below is
either from this tree's code (line numbers) or from upstream DXVK 2.7.1's source. Anything that
needs a GPU is marked **unverified**.

## 1. What we advertise today

`mali_physical_device.c:340-356` (built at physical-device creation, same for v10/v11/v13):

| # | propertyFlags | kbase backing |
|---|---|---|
| 0 | `DEVICE_LOCAL \| HOST_VISIBLE \| HOST_COHERENT` | native BO, `COHERENT_LOCAL`, CPU mapping write-combined (`mali_memory.c:53`) |
| 1 | `DEVICE_LOCAL \| HOST_VISIBLE \| HOST_CACHED` | `KB_HEAP_SYSTEM` dma-buf (CPU-cached), flush/invalidate via `bo_cache` (`mali_memory.c:126-161`) |

One heap, flagged `DEVICE_LOCAL`, size `gpu.heap_bytes` (`mali_physical_device.c:354-355`).
`MALI_MEM_TYPE_COUNT` is hardcoded 2 (`mali_memory.c:15`), the policy is `index == 1` means cached
(`mali_memory.c:52`), and every buffer/image requirement returns *all* bits
(`mali_buffer.c:59`, `mali_image.c:368` via `fill_reqs`), so the app can pick either type.

**Correction to the earlier notes** (`/tmp/oc/submit120.md`, `/tmp/oc/megabonk-gaps.md`): it is not
true that we "expose no device-local memory type". Both types carry `DEVICE_LOCAL`, and the single
heap is a device-local heap. What is missing is a type that is **not** `HOST_VISIBLE` — the thing
DXVK calls "Video memory" (`assignMemoryDebugName`, dxvk_memory.cpp). The rest of the earlier
reasoning still holds, because of how DXVK treats `DEVICE_LOCAL|HOST_VISIBLE`.

## 2. How DXVK 2.7.1 reacts

Source: dxvk_memory.cpp @ v2.7.1 (`DxvkMemoryAllocator`).

- Type classification (`determineMemoryTypesWithPropertyFlags`): for a requested property mask it
  keeps `sysmemMask` (types **without** `DEVICE_LOCAL`) if non-empty, else `vidmemMask`. We have no
  non-device-local type, so every mask resolves to our two device-local types.
- Allocation (`allocateMemory`): a `DEVICE_LOCAL` request is clamped to the heap of the lowest-bit
  candidate type — heap 0, i.e. everything — then walks the mask from the lowest bit. **It therefore
  always picks type 0**, the write-combined, `COHERENT_LOCAL` one. Type 1 (cached) is only reachable
  when the caller explicitly asks for `HOST_CACHED` (readback).
- Because type 0 is `HOST_VISIBLE`, DXVK maps the chunk (`mapDeviceMemory`) and `createAllocation`
  hands every sub-allocation a `m_mapPtr`. So **texture uploads, vertex/index/uniform buffer
  updates, and the global buffer are plain CPU stores** that DXVK expects the driver to make
  coherent — no staging buffer, no copy command, no flush. With `COHERENT_LOCAL` those stores also
  keep the GPU from using its caches for the memory it reads back.
- It never *requires* `DEVICE_LOCAL|HOST_VISIBLE`: with a non-host-visible type present, DXVK would
  pick it for device-local resources (lowest bit), stage uploads through a host-visible buffer with
  `vkCmdCopyBuffer`/`vkCmdCopyBufferToImage`, mark allocations `CanMove`, and never map them.

Unity (native Vulkan backend, if the Linux depot runs) queries memory properties the same way and
picks the first type whose flags contain what it needs; a `DEVICE_LOCAL`-only type is used for
textures/vertex buffers with staging uploads, exactly like on a discrete GPU. If Megabonk runs under
Proton it is DXVK's behaviour above that decides.

## 3. What kbase can actually deliver

kbase has no "GPU-exclusive" memory: `kb_bo_alloc` always gives the caller a CPU mapping
(`mali_backend.h:52`, `kb_bo.cpu` "always CPU mapped"). The levers that exist are:

| kbase flag | effect | usable as |
|---|---|---|
| `KB_BO_CPU_CACHED` (`BASE_MEM_CACHED_CPU`) | CPU-cached mapping, needs `dc civac` | host-visible *cached* type (type 1) |
| `COHERENT_LOCAL` (`kb_bo_extra_flags`, set on arch 11/13 by default) | GPU accesses are host-coherent, so they bypass GPU caches; per-submit `MEM_SYNC` required | host-visible *coherent* type (type 0) |
| `KB_BO_NOCOH` (no `COHERENT_LOCAL`) | GPU may cache; CPU must do explicit `MEM_SYNC`; no MEM_SYNC if the CPU never touches it | the only honest basis for a device-local type |
| `KB_BO_GPU_UNCACHED` (`BASE_MEM_UNCACHED_GPU`) | GPU *uncached*, i.e. the opposite | not this |

So a device-local type cannot be a physical property here. It can be an honest **contract**:
`DEVICE_LOCAL` without `HOST_VISIBLE`, where

- the BO is allocated without `COHERENT_LOCAL` (`KB_BO_NOCOH`), so the GPU may cache it;
- the app can never map it (`vkMapMemory` -> `VK_ERROR_MEMORY_MAP_FAILED`), so the missing host
  flush/invalidate cannot be observed by the app;
- every upload goes through a GPU copy from a host-visible staging buffer — our copy paths already
  do exactly that (`mali_meta_vk.c:208-320`, all VA-based meta jobs; BC decode is also GPU-side).

That is the same trade real drivers make: property flags are a policy the driver may choose, and
`HOST_VISIBLE` is not a claim about the kernel's internal mapping. **Unverified on hardware**: that
dropping `COHERENT_LOCAL` on a *buffer/image* BO is safe on v13 (the comment at
`mali_backend_kbase.c:289-295` records that v13 internal BOs keep it because "the v13 tiler bins
nothing without it"), and that GPU-cached reads are measurably faster for this workload. The
implementation is therefore opt-in (`MALI_DEVICE_LOCAL=1`) until a phone A/B exists.

### Resources that must stay host-visible

- Anything the CPU reads at submit time: computed indirect draws are expanded on the CPU
  (`mali_cmd.c:602`, `expand_indirect`), so `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT` memory must be
  host-visible, and indirect buffers are `bo_host_touch`ed at bind (`mali_buffer.c:78-80`).
- DXVK's "global buffer" mixes `INDIRECT` into its usage mask, so it keeps landing in a
  host-visible type; vertex/uniform writes therefore do not benefit until indirect draws are
  expanded on the GPU.

## 4. The per-wait CSYNC cost (mem_sync_all)

`kbase_fence_wait` calls `mem_sync_all(k, 2)` after **every** successful wait
(`mali_backend_kbase.c:510`), and `kbase_wait_counter` does the same (`:616-619`). `mem_sync_bos`
(`:551-575`) takes `bo_lock` and issued one `MEM_SYNC` ioctl for **every live BO** that is not
`host_lazy` and not `NOCOH` — the number scaled with allocations, not with what the GPU wrote or
what the CPU will read. M15 replaces that sweep with per-BO write tracking:

- The front end marks every GPU write (`mali_cmd_mark_written`, `mali_cmd.h`): render targets,
  depth/stencil attachments, clears, copy destinations (`mali_meta_vk.c`), and storage descriptors
  when the set is bound (`mali_dset_mark_write`). Marks reach the backend at submit
  (`mali_cmd_submit`), before the fence is created.
- `kb_bo` carries two monotonic counters, `gpu_writes` (marks) and `synced_writes` (what the last
  device->CPU `MEM_SYNC` covered). A wait syncs a BO only when they differ, so a mark made while an
  earlier frame is still in flight is synced (again) rather than lost, and a BO already covered is
  not synced twice.
- Escape hatches: `MALI_SYNC_ALL=1` restores the old sweep; `MALI_SYNC_TRACK=0` disables tracking.
  A write the front end cannot place (`bo_wrote(NULL)`, more than 64 BOs or 8 storage descriptors
  per set) triggers one full sweep at the next wait.
- **Measured on the host fake kernel** (`t_memsync`: 300 fence waits, 5 app-mapped BOs, one BO
  written per frame): 1500 -> 300 `MEM_SYNC` ioctls (5.0x fewer), 1.54-1.66 -> 0.36-0.39 us per
  wait (4.3-5.2x). That is a host function-call count; the ioctl cost on a phone is **unverified**.
- Correctness: the fake kernel can model a non-coherent GPU now (`kb_fake_gpu_write` writes a
  shadow that only `MEM_SYNC` type 2 reveals), so an unmarked GPU write reads back as stale data.
  `t_memsync` asserts that stale case on purpose, then that marking the BO makes the data visible,
  that unmarked BOs of the same device are not synced, and that two marks before one wait are both
  covered. Dropping the mark in `kbase_bo_wrote` makes five of those checks fail.

Residual risk, **unverified on hardware**: a GPU write path that never marks (a command added later,
or a write mechanism not in the list above) would leave stale data; `MALI_SYNC_ALL=1` isolates such a
report.

## 5. Summary

| question | answer |
|---|---|
| Do we advertise a device-local memory type? | Yes, on both types and on the heap — but only as `HOST_VISIBLE`. |
| Is a non-host-visible device-local type honest here? | Yes, as a policy: no `COHERENT_LOCAL`, no mapping, GPU-side copies. Not a physical property. |
| Does it help DXVK? | It changes which type DXVK picks for images/buffers (0 -> the device-local type), removes the per-frame CPU store-into-VRAM path, and removes those BOs from the CSYNC sweep. **Unmeasured on hardware.** |
| What is the risk? | v13 `COHERENT_LOCAL`/tiler interaction and GPU-cache maintenance, both hardware-only questions. Hence opt-in. |
