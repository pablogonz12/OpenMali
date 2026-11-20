/* SPDX-License-Identifier: MIT */
/* VkDeviceMemory on backend BOs. Heap 0 = device local. Type 0: HOST_VISIBLE|COHERENT (uncached
 * CPU mapping, write-combined); type 1: HOST_VISIBLE|CACHED (cached CPU mapping, explicit
 * flush/invalidate). The memory is always CPU mapped; vkMapMemory only hands out the pointer. */
#include "mali_queue.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "mali_entrypoints.h"
#include "util/u_atomic.h"
#include "vk_alloc.h"
#include "vk_util.h"

#define MALI_MEM_TYPE_COUNT 2
#define MALI_MAX_ALLOCS 4096
#define MALI_LINE 64 /* nonCoherentAtomSize */
#define MALI_PAGE 4096 /* what the backend commits for: bo_alloc rounds every request up to a page */

/* Bytes kbase commits for a VkDeviceMemory of `size`: whole pages. Charging the unrounded request lets 2049
 * one-byte allocations pass the byte check while the backend commits 8 MiB + 4 KiB. */
static uint64_t
mem_charge(uint64_t size)
{
   return (size + MALI_PAGE - 1) & ~(uint64_t)(MALI_PAGE - 1);
}

static VkResult
errno_to_vk(struct mali_device *dev, int e)
{
   if (e == -ENOMEM || e == -ENOSPC || e == -EFBIG)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   if (e == -EMFILE || e == -ENFILE)
      return vk_error(dev, VK_ERROR_TOO_MANY_OBJECTS);
   return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
}

/* True when fd is an open dma-buf: every dma-buf lives on the dmabuf pseudo filesystem (statfs magic DMA_BUF_MAGIC), so
 * no other descriptor (e.g. /dev/kvm, which also answers EINVAL to unknown ioctls) can pass. The fake backend's
 * "dma-bufs" are memfds: only check the fd is open. */
static bool
mali_fd_is_dmabuf(struct mali_device *dev, int fd)
{
   if (fd < 0 || fcntl(fd, F_GETFD) < 0)
      return false;
   if (mali_device_pdev(dev)->gpu.fake)
      return true;
   struct statfs sf;
   return fstatfs(fd, &sf) == 0 && (unsigned long)sf.f_type == DMA_BUF_MAGIC;
}

/* Reserve `size` against the device-wide commit budget before anything is allocated, so an over-budget
 * allocation fails cleanly with nothing half-committed. mem_used counts every live VkDeviceMemory of this
 * physical device; kbase pins the full size of each one at allocation time (libkbase/src/bo.c:45). The
 * compare-exchange loop makes concurrent allocators race correctly. */
static bool
mem_try_reserve(struct mali_physical_device *pdev, uint64_t size)
{
   uint64_t cur = p_atomic_read(&pdev->mem_used);
   for (;;) {
      if (size > pdev->mem_cap || cur > pdev->mem_cap - size)
         return false;
      if (__atomic_compare_exchange_n(&pdev->mem_used, &cur, cur + size, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
         return true;
   }
}

/* A pooled BO is committed in kbase but used by nobody: before refusing, really free enough of them to make room
 * (a steady alloc/free loop of one size would otherwise have to fail at the cap for the pool's own memory). */
static bool
mem_reserve(struct mali_device *dev, uint64_t size)
{
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   if (mem_try_reserve(pdev, size))
      return true;
   if (size <= pdev->mem_cap && pdev->backend->bo_pool_purge) {
      uint64_t cur = p_atomic_read(&pdev->mem_used);
      uint64_t room = cur < pdev->mem_cap ? pdev->mem_cap - cur : 0;
      if (size > room) {
         pdev->backend->bo_pool_purge(dev->kdev, size - room);
         if (mem_try_reserve(pdev, size))
            return true;
      }
   }
   static bool logged; /* one line per process: a session log shows the first refusal */
   if (!logged && getenv("MALI_WSI_DEBUG")) {
      logged = true;
      fprintf(stderr, "mali_vk: VRAM budget: requested %llu KiB, used %llu MiB of cap %llu MiB -> "
              "VK_ERROR_OUT_OF_DEVICE_MEMORY\n", (unsigned long long)(size >> 10),
              (unsigned long long)(p_atomic_read(&pdev->mem_used) >> 20),
              (unsigned long long)(pdev->mem_cap >> 20));
   }
   return false;
}

static void
mem_release(struct mali_physical_device *pdev, uint64_t size)
{
   __atomic_sub_fetch(&pdev->mem_used, size, __ATOMIC_ACQ_REL);
}

/* Installed on the backend's pool (mali_device.c): a pooled BO was really freed, so its charge goes away. */
void
mali_memory_pool_release(void *pdev, size_t size)
{
   mem_release(pdev, size);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_AllocateMemory(VkDevice _device, const VkMemoryAllocateInfo *pAllocateInfo,
                    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory)
{
   VK_FROM_HANDLE(mali_device, dev, _device);
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   const struct mali_backend_ops *be = pdev->backend;

   if (pAllocateInfo->memoryTypeIndex >= MALI_MEM_TYPE_COUNT)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   if (pAllocateInfo->allocationSize > pdev->gpu.heap_bytes)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   /* What kbase commits: the backend rounds every request up to a page (bo_alloc/bo_import), so that is what is
    * charged and what the free path releases (mem_charge of vk.size, stored unrounded). */
   const uint64_t mem_sz = mem_charge(pAllocateInfo->allocationSize);
   if (!mem_reserve(dev, mem_sz))
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   if (p_atomic_inc_return(&dev->mem_count) > MALI_MAX_ALLOCS) {
      p_atomic_dec(&dev->mem_count);
      mem_release(pdev, mem_sz);
      return vk_error(dev, VK_ERROR_TOO_MANY_OBJECTS);
   }

   struct mali_memory *mem = vk_device_memory_create(&dev->vk, pAllocateInfo, pAllocator, sizeof(*mem));
   if (!mem) {
      p_atomic_dec(&dev->mem_count);
      mem_release(pdev, mem_sz);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   const bool cached = pAllocateInfo->memoryTypeIndex == 1;
   const uint32_t flags = cached ? MALI_BO_CACHED : 0;
   const VkImportMemoryFdInfoKHR *imp =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);
   int r;

   mem->bo.dmabuf_fd = -1;
   if (imp && imp->handleType != 0) {
      if (imp->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
          imp->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
         r = -EINVAL;
      } else if (!mali_fd_is_dmabuf(dev, imp->fd)) {
         r = -EBADF;
      } else {
         /* the memory may not be larger than the dma-buf behind it (lseek SEEK_END is the dma-buf size) */
         off_t sz = lseek(imp->fd, 0, SEEK_END);
         if (sz > 0)
            lseek(imp->fd, 0, SEEK_SET);
         r = (sz > 0 && (uint64_t)sz < pAllocateInfo->allocationSize) ? -EINVAL : 0;
         if (r == 0)
            r = be->bo_import(dev->kdev, imp->fd, pAllocateInfo->allocationSize, flags, &mem->bo);
         if (r == 0)
            close(imp->fd); /* ownership moves to the memory on success */
      }
   } else {
      const bool export = mem->vk.export_handle_types != 0;
      r = be->bo_alloc(dev->kdev, pAllocateInfo->allocationSize,
                       flags | (export ? MALI_BO_EXPORT : MALI_BO_HOSTLAZY), &mem->bo);
   }
   if (r != 0) {
      vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
      p_atomic_dec(&dev->mem_count);
      mem_release(pdev, mem_sz);   /* nothing was committed: give the reservation back */
      return errno_to_vk(dev, r);
   }

   /* mem_used was reserved before the allocation (mem_reserve); it must not be added again here. */
   *pMemory = mali_memory_to_handle(mem);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_FreeMemory(VkDevice _device, VkDeviceMemory _mem, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(mali_device, dev, _device);
   VK_FROM_HANDLE(mali_memory, mem, _mem);

   if (!mem)
      return;
   mali_device_pdev(dev)->backend->bo_free(dev->kdev, &mem->bo);
   /* A BO the kbase pool kept is still committed (kbase_bo_free only moves it to the free list), so its charge stays
    * until the pool really frees it or hands it to the next allocation; that is the pool's release callback. */
   if (!mem->bo.pooled)
      mem_release(mali_device_pdev(dev), mem_charge(mem->vk.size));   /* the size reserved in mali_AllocateMemory */
   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
   p_atomic_dec(&dev->mem_count);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_MapMemory(VkDevice _device, VkDeviceMemory _mem, VkDeviceSize offset, VkDeviceSize size,
               VkMemoryMapFlags flags, void **ppData)
{
   VK_FROM_HANDLE(mali_device, dev, _device);
   VK_FROM_HANDLE(mali_memory, mem, _mem);

   if (!mem) {
      *ppData = NULL;
      return VK_SUCCESS;
   }
   if (mem->mapped || offset >= mem->vk.size)
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);
   mem->mapped = true;
   /* GPU-written, CPU-read memory (the software present reads the rendered image straight out of its mapping):
    * clearing host_lazy here means every later fence wait MEM_SYNCs this BO device->CPU (COHERENT_LOCAL is only
    * inner-shareable, so the CPU cache must be told), which is what makes the PutImage readback see the frame. */
   if (mali_device_pdev(dev)->backend->bo_host_touch)
      mali_device_pdev(dev)->backend->bo_host_touch(dev->kdev, &mem->bo);
   *ppData = (char *)mem->bo.cpu + offset;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_UnmapMemory(VkDevice _device, VkDeviceMemory _mem)
{
   VK_FROM_HANDLE(mali_memory, mem, _mem);

   if (mem)
      mem->mapped = false;
}

static void
range_cache(struct mali_device *dev, const VkMappedMemoryRange *r, bool invalidate)
{
   VK_FROM_HANDLE(mali_memory, mem, r->memory);
   if (!(mem->bo.flags & MALI_BO_CACHED))
      return;

   uint64_t off = r->offset, len = vk_device_memory_range(&mem->vk, r->offset, r->size);
   /* Whole cache lines: round the start down and the end up (the spec makes offset and size
    * multiples of nonCoherentAtomSize, except a range that runs to the end). */
   uint64_t start = off & ~(uint64_t)(MALI_LINE - 1);
   uint64_t end = (off + len + MALI_LINE - 1) & ~(uint64_t)(MALI_LINE - 1);
   if (end > mem->bo.size)
      end = mem->bo.size;
   mali_device_pdev(dev)->backend->bo_cache(dev->kdev, &mem->bo, start, end - start, invalidate);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_FlushMappedMemoryRanges(VkDevice _device, uint32_t count, const VkMappedMemoryRange *ranges)
{
   VK_FROM_HANDLE(mali_device, dev, _device);

   for (uint32_t i = 0; i < count; i++)
      range_cache(dev, &ranges[i], false);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_InvalidateMappedMemoryRanges(VkDevice _device, uint32_t count, const VkMappedMemoryRange *ranges)
{
   VK_FROM_HANDLE(mali_device, dev, _device);

   for (uint32_t i = 0; i < count; i++)
      range_cache(dev, &ranges[i], true);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_GetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory, VkDeviceSize *pCommittedMemoryInBytes)
{
   *pCommittedMemoryInBytes = 0; /* no lazily allocated memory type */
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_GetMemoryFdKHR(VkDevice _device, const VkMemoryGetFdInfoKHR *pGetFdInfo, int *pFd)
{
   VK_FROM_HANDLE(mali_device, dev, _device);
   VK_FROM_HANDLE(mali_memory, mem, pGetFdInfo->memory);

   /* OPAQUE_FD and DMA_BUF are the same object to this driver: kbase hands out a dma-buf fd, and that fd is also a
    * valid opaque fd (the two handle types differ in ownership/transfer semantics, not in what the descriptor is).
    * We advertise VK_KHR_external_memory_fd, so refusing OPAQUE_FD (which gamescope's zink asks for) was a lie. */
   const bool fd_ok = pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ||
                      pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
   if (!fd_ok)
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   /* Only memory allocated for export (or imported from a dma-buf) may be exported. OPAQUE_FD and DMA_BUF are
    * the same descriptor here (#293), so either handle type on either side qualifies. */
   const VkExternalMemoryHandleTypeFlags fd_bits = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
                                                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
   if (!(mem->vk.export_handle_types & fd_bits) && !(mem->vk.import_handle_type & fd_bits))
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   int fd = mali_device_pdev(dev)->backend->bo_export(dev->kdev, &mem->bo);
   if (fd < 0)
      return errno_to_vk(dev, fd);
   *pFd = fd;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_GetMemoryFdPropertiesKHR(VkDevice _device, VkExternalMemoryHandleTypeFlagBits handleType, int fd,
                              VkMemoryFdPropertiesKHR *pProps)
{
   VK_FROM_HANDLE(mali_device, dev, _device);

   if ((handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
        handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) ||
       (handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && !mali_fd_is_dmabuf(dev, fd)))
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   pProps->memoryTypeBits = (1u << MALI_MEM_TYPE_COUNT) - 1;
   return VK_SUCCESS;
}
