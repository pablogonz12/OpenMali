/* SPDX-License-Identifier: MIT */
/* M4: memory, sync and queue submit on the kbase backend seam (mali_backend.h). */
#ifndef MALI_QUEUE_H
#define MALI_QUEUE_H

#include "mali_private.h"
#include "vk_device_memory.h"

/* Binary sync on a sync_file. States: unsignaled (no fd, !signaled), pending (fd), signaled. */
struct mali_sync {
   struct vk_sync vk;
   int fd;
   bool signaled;
   const void *owner;   /* queue whose submission attached fd (in-order queue: a later wait of that queue on it is already satisfied) */
};

extern const struct vk_sync_type mali_sync_type;

/* Fills pdev->sync_types / timeline_type and sets vk.supported_sync_types. */
void mali_sync_init_physical(struct mali_physical_device *pdev);

/* M9 (mali_queue.c): per-device frame resources (descriptor arena, geometry buffer, tiler heap, retire counter), installed into
 * mali_cmd.c as the (process-wide, one device at a time) hardware seam. */
VkResult mali_hw_init(struct mali_device *dev);
void mali_hw_finish(struct mali_device *dev);
/* Metadata BOs (events, query pools): allocated/freed through the NAMED device, independent of the global binding. */
void *mali_hw_meta_alloc(struct mali_device *dev, size_t size, size_t align, uint64_t *va);
void mali_hw_meta_free(struct mali_device *dev, void *cpu);
struct mali_cmd_res_ops;
extern const struct mali_cmd_res_ops mali_cmd_res;

/* Per-device lock/cv, queue hooks (driver_submit, check_status). */
VkResult mali_queue_device_init(struct mali_device *dev);
void mali_queue_device_finish(struct mali_device *dev);
void mali_queue_setup(struct mali_device *dev); /* after vk_queue_init */

/* A dup of the last submission's sync_file (-1 if there is none): the X11 shared-memory present has no CPU-visible
 * semaphore to wait (binary semaphores are not observable through vk_sync_wait in this driver), so it waits this
 * fence before reading the image that submission wrote. The caller closes it. */
int mali_queue_last_fence_dup(struct mali_device *dev);

static inline struct mali_physical_device *
mali_device_pdev(struct mali_device *dev)
{
   return container_of(dev->vk.physical, struct mali_physical_device, vk);
}

/* Memory */
struct mali_memory {
   struct vk_device_memory vk;
   struct mali_bo bo;
   bool mapped;
   uint8_t isync;   /* MALI_ISYNC_BO_* (mali_isync.h): exported/imported dma-buf, or a swapchain image */
   struct mali_memory *live_next;   /* mali_memory.c live registry (mali_memory_live) */
};
VK_DEFINE_NONDISP_HANDLE_CASTS(mali_memory, vk.base, VkDeviceMemory, VK_OBJECT_TYPE_DEVICE_MEMORY)

/* The live VkDeviceMemory objects of this process (mali_memory.c). A command buffer keeps raw struct mali_memory *
 * from RECORD time in its wrote[]/read[] marks, and the app may vkFreeMemory before it submits: a mark can point at
 * freed memory. The implicit-sync submit and a readback diagnostic read those marks, so they ask here first, and
 * mali_memory_guard() also stops vkFreeMemory from releasing the memory under the read (it takes the same lock).
 * guard: true = the pointer was live and the registry lock is HELD, call mali_memory_unguard() after the access;
 * false = gone, the lock is released. Not re-entrant. */
bool mali_memory_live(const struct mali_memory *m);
bool mali_memory_guard(struct mali_memory *m);
void mali_memory_unguard(void);

#endif
