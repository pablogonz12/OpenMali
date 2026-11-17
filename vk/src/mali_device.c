/* SPDX-License-Identifier: MIT */
#include "mali_private.h"

#include <errno.h>

#include "mali_entrypoints.h"
#include "mali_cmd.h"
#include "mali_queue.h"
#include "vk_alloc.h"

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
   VK_FROM_HANDLE(mali_physical_device, pdev, physicalDevice);
   struct mali_device *dev;
   VkResult result;

   dev = vk_zalloc2(&pdev->vk.instance->alloc, pAllocator, sizeof(*dev), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!dev)
      return vk_error(pdev, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Per-arch entry points (files compiled once per PAN_ARCH, from M6 on) win over the
    * arch-independent ones; the runtime adds its vk_common_* defaults last. */
   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table,
      pdev->gpu.arch_major >= 12 ? &mali_v13_device_entrypoints : &mali_v11_device_entrypoints,
      true);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &mali_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &wsi_device_entrypoints, false);

   result = vk_device_init(&dev->vk, &pdev->vk, &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&pdev->vk.instance->alloc, pAllocator, dev);
      return result;
   }

   mali_resources_init();
   mali_queue_device_init(dev);
   result = pdev->backend->device_create(&pdev->gpu, &dev->kdev);
   if (result != 0) {
      result = vk_error(dev, result == -ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY
                                                : VK_ERROR_INITIALIZATION_FAILED);
      mali_queue_device_finish(dev);
      vk_device_finish(&dev->vk);
      vk_free2(&pdev->vk.instance->alloc, pAllocator, dev);
      return result;
   }

   dev->vk.command_buffer_ops = &mali_cmd_ops;
   result = mali_hw_init(dev);
   if (result != VK_SUCCESS) {
      pdev->backend->device_destroy(dev->kdev);
      mali_queue_device_finish(dev);
      vk_device_finish(&dev->vk);
      vk_free2(&pdev->vk.instance->alloc, pAllocator, dev);
      return vk_error(pdev, result);
   }

   /* One queue family with one queue (see mali_GetPhysicalDeviceQueueFamilyProperties2). */
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *qinfo = &pCreateInfo->pQueueCreateInfos[i];

      if (qinfo->queueFamilyIndex != 0 || qinfo->queueCount != 1 || dev->has_queue) {
         result = vk_error(dev, VK_ERROR_INITIALIZATION_FAILED);
         goto fail;
      }

      result = vk_queue_init(&dev->queue.vk, &dev->vk, qinfo, 0);
      if (result != VK_SUCCESS)
         goto fail;
      dev->has_queue = true;
      mali_queue_setup(dev);
   }

   *pDevice = mali_device_to_handle(dev);
   return VK_SUCCESS;

fail:
   if (dev->has_queue)
      vk_queue_finish(&dev->queue.vk);
   mali_hw_finish(dev);
   pdev->backend->device_destroy(dev->kdev);
   mali_queue_device_finish(dev);
   vk_device_finish(&dev->vk);
   vk_free2(&pdev->vk.instance->alloc, pAllocator, dev);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(mali_device, dev, _device);

   if (!dev)
      return;

   /* vk_device_init stored the effective allocator (pAllocator or the instance's). */
   const VkAllocationCallbacks alloc = dev->vk.alloc;

   if (dev->has_queue)
      vk_queue_finish(&dev->queue.vk);
   struct mali_physical_device *pdev = container_of(dev->vk.physical, struct mali_physical_device, vk);
   mali_hw_finish(dev);
   pdev->backend->device_destroy(dev->kdev);
   mali_queue_device_finish(dev);
   vk_device_finish(&dev->vk);
   vk_free(&alloc, dev);
}
