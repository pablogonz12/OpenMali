/* SPDX-License-Identifier: MIT */
/* VkDeviceMemory on backend BOs. Heap 0 = device local. Type 0: HOST_VISIBLE|COHERENT (uncached
 * CPU mapping, write-combined); type 1: HOST_VISIBLE|CACHED (cached CPU mapping, explicit
 * flush/invalidate). The memory is always CPU mapped; vkMapMemory only hands out the pointer. */
#include "mali_queue.h"

#include <errno.h>
#include <unistd.h>

#include "mali_entrypoints.h"
#include "util/u_atomic.h"
#include "vk_alloc.h"
#include "vk_util.h"

#define MALI_MAX_ALLOCS 4096
#define MALI_LINE 64 /* nonCoherentAtomSize */

static VkResult
errno_to_vk(struct mali_device *dev, int e)
{
   if (e == -ENOMEM || e == -ENOSPC || e == -EFBIG)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   if (e == -EMFILE || e == -ENFILE)
      return vk_error(dev, VK_ERROR_TOO_MANY_OBJECTS);
   return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_AllocateMemory(VkDevice _device, const VkMemoryAllocateInfo *pAllocateInfo,
                    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory)
{
   VK_FROM_HANDLE(mali_device, dev, _device);
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   const struct mali_backend_ops *be = pdev->backend;

   if (pAllocateInfo->memoryTypeIndex >= pdev->memory.memoryTypeCount)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   if (pAllocateInfo->allocationSize > pdev->gpu.heap_bytes)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   if (p_atomic_inc_return(&dev->mem_count) > MALI_MAX_ALLOCS) {
      p_atomic_dec(&dev->mem_count);
      return vk_error(dev, VK_ERROR_TOO_MANY_OBJECTS);
   }

   struct mali_memory *mem = vk_device_memory_create(&dev->vk, pAllocateInfo, pAllocator, sizeof(*mem));
   if (!mem) {
      p_atomic_dec(&dev->mem_count);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* Policy comes from the advertised property flags, not from the index (MALI_DEVICE_LOCAL reorders
    * the types, vk/MEMORY.md): HOST_CACHED = CPU-cached mapping with explicit flush/invalidate;
    * no HOST_VISIBLE = device-local, GPU-cacheable, never mapped and never CSYNCed by the CPU. */
   const VkMemoryPropertyFlags tprops = pdev->memory.memoryTypes[pAllocateInfo->memoryTypeIndex].propertyFlags;
   const bool cached = (tprops & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
   const bool devlocal = (tprops & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0;
   const uint32_t flags = (cached ? MALI_BO_CACHED : 0) | (devlocal ? MALI_BO_DEVICE_LOCAL : 0);
   const VkImportMemoryFdInfoKHR *imp =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);
   int r;

   mem->bo.dmabuf_fd = -1;
   if (imp && imp->handleType != 0) {
      if (imp->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
         r = -EINVAL;
      } else {
         r = be->bo_import(dev->kdev, imp->fd, pAllocateInfo->allocationSize, flags, &mem->bo);
         if (r == 0)
            close(imp->fd); /* ownership moves to the memory on success */
      }
   } else {
      const bool export = mem->vk.export_handle_types != 0;
      /* device-local: HOSTLAZY even for an export, so no per-submit MEM_SYNC ever covers it. */
      r = be->bo_alloc(dev->kdev, pAllocateInfo->allocationSize,
                       flags | (export ? MALI_BO_EXPORT : MALI_BO_HOSTLAZY) | (devlocal ? MALI_BO_HOSTLAZY : 0), &mem->bo);
   }
   if (r != 0) {
      vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
      p_atomic_dec(&dev->mem_count);
      return errno_to_vk(dev, r);
   }

   mem->props = tprops;
   p_atomic_add(&pdev->mem_used, mem->vk.size);
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
   p_atomic_add(&mali_device_pdev(dev)->mem_used, -(int64_t)mem->vk.size);
   mali_device_pdev(dev)->backend->bo_free(dev->kdev, &mem->bo);
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
   /* A device-local type is not host visible: the app must stage through a host-visible type. That is
    * what makes "no flush/invalidate needed" true instead of a silent stale read. */
   if (!(mem->props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || mem->mapped || offset >= mem->vk.size)
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);
   mem->mapped = true;
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

   if (pGetFdInfo->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
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

   if (handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   pProps->memoryTypeBits = (1u << mali_device_pdev(dev)->memory.memoryTypeCount) - 1;
   return VK_SUCCESS;
}
