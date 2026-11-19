# Memory types, heaps, and what a device-local type can honestly be (M15)

Host-only analysis, 2026-11-19. No phone was used: every statement about behaviour below is
either from this tree's code (line numbers) or from upstream DXVK 2.7.1's source. Anything that
needs a GPU is marked **unverified**.

## 1. What we advertise today

`mali_physical_device.c:340-356` (built at physical-device creation, same for v10/v11/v13):

| # | propertyFlags | kbase backing |
|---|---|---|
| 0 | `DEVICE_LOCAL \| HOST_VISIBLE \| HOST_COHERENT` | native BO, `COHERENT_LOCAL`, CPU mapping write-combined (`mali_memory.c:53`) |
| 1 | `DEVICE_LOCAL \| HOST_VISIBLE \| HOST_CACHED` | native BO with `KB_BO_CPU_CACHED` (CPU-cached mapping), flush/invalidate via `bo_cache` (`mali_memory.c:126-161`); an *exported* type-1 BO instead gets a `KB_HEAP_SYSTEM` dma-buf (`mali_backend_kbase.c:306-315`), and `bo_cache` is the cache op for both paths |

One heap, flagged `DEVICE_LOCAL`, size `gpu.heap_bytes` (`mali_physical_device.c:354-355`).
`memoryTypeCount` is 2 by default and 3 with `MALI_DEVICE_LOCAL=1` (device-local first, then the two
host-visible types), the policy comes from the advertised flags rather than the index
(`mali_memory.c:52-57`), and every buffer/image requirement returns *all* bits except the
device-local type for `INDIRECT` buffers (`mali_buffer.c:59-70`)
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
- Because type 0 is `HOST_VISIBLE`, DXVK *can* map a chunk that selects it — but only when the request also
  asked for `HOST_VISIBLE`: `mapDeviceMemory` tests the requested `properties`, and `createAllocation` passes
  `allocationInfo.properties` down. A `DEVICE_LOCAL`-only request that lands on type 0 therefore stays
  unmapped (`m_mapPtr` stays null) and uploads to it go through a staging buffer plus a copy. So the
  prediction depends on the request: **host-visible** requests that select type 0 get `m_mapPtr` and their
  texture/vertex/uniform updates are plain CPU stores, while `DEVICE_LOCAL`-only resources are not mapped and
  must not be predicted to take the CPU-store upload path. The upload benefit in §5 is therefore limited to
  the host-visible requests, and those still keep `COHERENT_LOCAL`.
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
implementation is therefore opt-in (`MALI_DEVICE_LOCAL=1`, read by `mali_device_local_enabled()` in
`mali_private.h:21-31` at physical-device creation) until a phone A/B exists.

### Constraint: GPU-written memory and SSBO atomics (arch 11)

`KB_BO_NOCOH` removes `COHERENT_LOCAL`, and on arch 11 that is not only a CPU-cache question: plain
SSBO atomics then run in each core's L1 and updates from different cores are lost. The comment at
`mali_backend_kbase.c:131-134` records exactly this (CMF, 2 cores: `atomicAdd` count ~half), which is
why `COHERENT_LOCAL` is the arch-11 default for application `VkDeviceMemory` in the first place.
Unmapping the BO does not help: the loss is GPU-side, so `vkMapMemory` failing says nothing about it.

Therefore a device-local BO is only honest for resources that are **read** on the GPU (sampled
images, vertex/uniform/index data, storage buffers without atomics) or written only by one workgroup
without cross-core visibility requirements. A storage buffer that the application updates with
atomics, or that is written by one dispatch and read by another without a barrier that implies
coherence, must stay in a host-visible (`COHERENT_LOCAL`) type on arch 11. Until the switch has a
per-resource policy (or arch 11 grows a GPU-cache-maintenance story for these BOs), treat
`MALI_DEVICE_LOCAL=1` as usable only for front ends/games without SSBO-atomic workloads.

### Resources that must stay host-visible

- Anything the CPU reads at submit time: computed indirect draws are expanded on the CPU
  (`mali_cmd.c:602`, `expand_indirect`), so `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT` memory must be
  host-visible, and indirect buffers are `bo_host_touch`ed at bind (`mali_buffer.c:78-80`). The
  device-local type is therefore masked out of `memoryTypeBits` for that usage in
  `mali_GetDeviceBufferMemoryRequirements` (`mali_buffer.c:53-72`): binding one would keep the BO
  host-lazy (`kbase_bo_host_touch` skips `MALI_BO_DEVICE_LOCAL`) and it never gets a CPU-side
  `MEM_SYNC`, so those CPU reads would be unsynchronised.
- DXVK's "global buffer" mixes `INDIRECT` into its usage mask, so it keeps landing in a
  host-visible type; vertex/uniform writes therefore do not benefit until indirect draws are
  expanded on the GPU.

## 4. The per-wait CSYNC cost (mem_sync_all)

`kbase_fence_wait` calls `mem_sync_all(k, 2)` after **every** successful wait
(`mali_backend_kbase.c:516`), and `kbase_wait_counter` does the same (`:618-624`). `mem_sync_bos`
(`:557-584`) takes `bo_lock` and issues one `MEM_SYNC` ioctl for **every live BO** that is not
`host_lazy` and not `NOCOH` — the number scales with allocations, not with what the GPU wrote or
what the CPU will read. This is the next real submit-path cost and is tracked separately (per-BO
write tracking, PR that follows this one).

## 5. Summary

| question | answer |
|---|---|
| Do we advertise a device-local memory type? | Yes, on both types and on the heap — but only as `HOST_VISIBLE`. |
| Is a non-host-visible device-local type honest here? | Yes, as a policy: no `COHERENT_LOCAL`, no mapping, GPU-side copies. Not a physical property. |
| Does it help DXVK? | It changes which type DXVK picks for `DEVICE_LOCAL`-only images/buffers (0 -> the device-local type) and removes those BOs from the CSYNC sweep; uploads to them then go through a staging buffer + copy instead of the CPU-store path, because they are no longer mapped. Host-visible requests are unaffected. **Unmeasured on hardware.** |
| What is the risk? | v13 `COHERENT_LOCAL`/tiler interaction and GPU-cache maintenance, both hardware-only questions. Hence opt-in. |
