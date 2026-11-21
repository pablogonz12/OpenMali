/* SPDX-License-Identifier: MIT */
/* M14: window system integration through Mesa's wsi_common (Wayland platform). Swapchain images are
 * ordinary driver images made linear (scanout flag / DRM modifier LINEAR) on exportable dma-buf BOs
 * (MALI_BO_EXPORT via VkExportMemoryAllocateInfo); presentation sync is implicit: wsi_common exports a
 * sync_file from a semaphore the present submission signals (mali_sync export_sync_file) and imports it
 * into the image's dma-buf (DMA_BUF_IOCTL_IMPORT_SYNC_FILE). No explicit-sync protocol (gamescope has none).
 * Everything else lives in the runtime, wsi_common and the entrypoints in mali_image.c / mali_memory.c. */
#include "mali_private.h"

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_util.h"

#include <stdlib.h>
#include <string.h>

#define MALI_DRM_FORMAT_MOD_LINEAR 0ull
const uint64_t mali_wsi_modifier = MALI_DRM_FORMAT_MOD_LINEAR;

static VKAPI_PTR PFN_vkVoidFunction
mali_wsi_proc_addr(VkPhysicalDevice h, const char *name)
{
   VK_FROM_HANDLE(mali_physical_device, pdev, h);
   return vk_instance_get_proc_addr_unchecked(pdev->vk.instance, name);
}

/* wsi_common calls DestroyQueryPool(VK_NULL_HANDLE) unconditionally per image; the driver may not implement
 * queries yet (entrypoint NULL), so route through a guard that tolerates both. */
static PFN_vkDestroyQueryPool mali_real_destroy_query_pool;
static VKAPI_ATTR void VKAPI_CALL
mali_wsi_destroy_query_pool(VkDevice dev, VkQueryPool pool, const VkAllocationCallbacks *a)
{
   if (pool != VK_NULL_HANDLE && mali_real_destroy_query_pool)
      mali_real_destroy_query_pool(dev, pool, a);
}

/* X11 WSI (wsi_common_x11, see STEAM-GAPS.md "X11 WSI"): two present paths, chosen once per device.
 *   dma-buf (default on hardware): images are the linear exportable BOs above; the X server imports them through DRI3
 *     (Present). Needs an X server that can import our dma-bufs: Xwayland with glamor on a render node. DroidDeck's
 *     Xwayland on Mali has no DRM node, hence no DRI3 (docs/development/mali-venus.md).
 *   software (fake backend, or MALI_WSI=sw): wsi_common copies each frame into a CPU-visible linear buffer and the X server
 *     gets it by MIT-SHM / PutImage (what lavapipe does, and what the vendor bridge does for X11). Needs no dma-buf at all,
 *     so it also works on the fake backend (whose BO fds are not dma-bufs). The device-wide switch also moves the Wayland
 *     platform to wl_shm: do not use it in a process whose Wayland swapchains must stay zero-copy (the gamescope layer's).
 * MALI_WSI=dmabuf forces the first path on the fake backend (it then fails at swapchain creation; for tests). */
static bool
mali_wsi_use_sw(const struct mali_physical_device *pdev)
{
   const char *e = getenv("MALI_WSI");
   if (e && !strcmp(e, "sw"))
      return true;
   if (e && !strcmp(e, "dmabuf"))
      return false;
   return pdev->gpu.fake;
}

VkResult
mali_wsi_init(struct mali_physical_device *pdev)
{
   bool sw = mali_wsi_use_sw(pdev);
   VkResult r = wsi_device_init(&pdev->wsi, mali_physical_device_to_handle(pdev), mali_wsi_proc_addr,
                                &pdev->vk.instance->alloc, -1, NULL,
                                &(struct wsi_device_options){.sw_device = sw});
   if (r != VK_SUCCESS)
      return r;
   /* No DRM node behind kbase: wsi sees no render/primary device and assumes the compositor is on our GPU. */
   pdev->wsi.supports_modifiers = true;
   mali_real_destroy_query_pool = pdev->wsi.DestroyQueryPool;
   pdev->wsi.DestroyQueryPool = mali_wsi_destroy_query_pool;
   pdev->vk.wsi_device = &pdev->wsi;
   return VK_SUCCESS;
}

void
mali_wsi_finish(struct mali_physical_device *pdev)
{
   pdev->vk.wsi_device = NULL;
   wsi_device_finish(&pdev->wsi, &pdev->vk.instance->alloc);
}

/* MALI_TRACE: log every acquire / present with its semaphore, fence, image index and result (the runtime's wsi_* entrypoints do the work; these
 * wrappers only time and print). Installed on the device dispatch table only when tracing is on. */
static PFN_vkAcquireNextImageKHR trace_real_acquire;
static PFN_vkAcquireNextImage2KHR trace_real_acquire2;
static PFN_vkQueuePresentKHR trace_real_present;

static VKAPI_ATTR VkResult VKAPI_CALL
trace_acquire(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *idx)
{
   mali_trace("acquire sc=%p sem=%p fence=%p timeout=%llu", (void *)(uintptr_t)sc, (void *)(uintptr_t)sem, (void *)(uintptr_t)fence, (unsigned long long)timeout);
   VkResult r = trace_real_acquire(d, sc, timeout, sem, fence, idx);
   mali_trace("  acquire -> %d idx=%u", (int)r, idx ? *idx : 0);
   return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
trace_acquire2(VkDevice d, const VkAcquireNextImageInfoKHR *i, uint32_t *idx)
{
   mali_trace("acquire2 sc=%p sem=%p fence=%p timeout=%llu", (void *)(uintptr_t)i->swapchain, (void *)(uintptr_t)i->semaphore, (void *)(uintptr_t)i->fence,
              (unsigned long long)i->timeout);
   VkResult r = trace_real_acquire2(d, i, idx);
   mali_trace("  acquire2 -> %d idx=%u", (int)r, idx ? *idx : 0);
   return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
trace_present(VkQueue q, const VkPresentInfoKHR *p)
{
   for (uint32_t i = 0; i < p->swapchainCount; i++)
      mali_trace("present sc=%p idx=%u waits=%u", (void *)(uintptr_t)p->pSwapchains[i], p->pImageIndices[i], p->waitSemaphoreCount);
   VkResult r = trace_real_present(q, p);
   mali_trace("  present -> %d", (int)r);
   return r;
}

void
mali_wsi_trace_hook(struct vk_device_dispatch_table *t)
{
   if (!mali_trace_on() || trace_real_present)
      return;
   trace_real_acquire = t->AcquireNextImageKHR;
   trace_real_acquire2 = t->AcquireNextImage2KHR;
   trace_real_present = t->QueuePresentKHR;
   if (trace_real_acquire)
      t->AcquireNextImageKHR = trace_acquire;
   if (trace_real_acquire2)
      t->AcquireNextImage2KHR = trace_acquire2;
   if (trace_real_present)
      t->QueuePresentKHR = trace_present;
}
