/* SPDX-License-Identifier: MIT */
/* Own Mali Vulkan front end on Mesa's common Vulkan runtime (vk_*). Nothing here is derived from
 * src/panfrost/vulkan. */
#ifndef MALI_PRIVATE_H
#define MALI_PRIVATE_H

#include "vk_device.h"
#include "vk_instance.h"
#include "vk_log.h"
#include "vk_physical_device.h"
#include "vk_queue.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"
#include <pthread.h>

#include "mali_backend.h"
#include "wsi_common.h"

#define MALI_VENDOR_ID_ARM 0x13B5

struct mali_instance {
   struct vk_instance vk;
};

struct mali_physical_device {
   struct vk_physical_device vk;

   const struct mali_backend_ops *backend;
   struct mali_gpu_info gpu;
   VkPhysicalDeviceMemoryProperties memory;

   /* M4: binary fences/semaphores on kbase fences + the runtime's emulated timeline on top. */
   struct vk_sync_timeline_type timeline_type;
   const struct vk_sync_type *sync_types[3];

   /* M14 (mali_wsi.c): Mesa wsi_common, Wayland platform, linear dma-buf swapchain images. */
   struct wsi_device wsi;
};

struct mali_queue {
   struct vk_queue vk;
};

struct mali_device {
   struct vk_device vk;
   struct vk_device_dispatch_table cmd_dispatch;   /* primary command buffers: the driver's own Cmd* (secondaries record into the runtime cmd queue) */

   struct mali_kdev *kdev;
   struct mali_queue queue;
   bool has_queue;

   /* M4 (mali_sync.c, mali_memory.c, mali_queue.c) */
   pthread_mutex_t sync_lock; /* guards every mali_sync's state */
   pthread_cond_t sync_cv;    /* a sync got a fence or was signalled */
   int32_t mem_count;         /* live VkDeviceMemory, vs maxMemoryAllocationCount */
   struct mali_hwctx *hw;     /* M9: per-device frame resources (mali_queue.c) */
};

VK_DEFINE_HANDLE_CASTS(mali_instance, vk.base, VkInstance, VK_OBJECT_TYPE_INSTANCE)
VK_DEFINE_HANDLE_CASTS(mali_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)
VK_DEFINE_HANDLE_CASTS(mali_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

/* Instance-level API version. The instance level is implemented by the common runtime, so it can
 * be ahead of the device apiVersion, which starts at 1.0 and is raised per milestone. */
#define MALI_INSTANCE_API_VERSION VK_MAKE_API_VERSION(0, 1, 3, VK_HEADER_VERSION)
#define MALI_DEVICE_API_VERSION   VK_MAKE_API_VERSION(0, 1, 0, VK_HEADER_VERSION)

VkResult mali_enumerate_physical_devices(struct vk_instance *vk_instance);
/* M5/M7 seams (mali_resources.c): installs the descriptor/image resolvers and the BO-backed GPU pools. Idempotent. */
void mali_resources_init(void);
/* M14 */
VkResult mali_wsi_init(struct mali_physical_device *pdev);
void mali_wsi_finish(struct mali_physical_device *pdev);
extern const uint64_t mali_wsi_modifier; /* DRM_FORMAT_MOD_LINEAR, the only one advertised */
void mali_destroy_physical_device(struct vk_physical_device *vk_pdev);

#endif
