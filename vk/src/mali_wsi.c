/* SPDX-License-Identifier: MIT */
/* M14: window system integration through Mesa's wsi_common (Wayland platform). Swapchain images are
 * ordinary driver images made linear (scanout flag / DRM modifier LINEAR) on exportable dma-buf BOs
 * (MALI_BO_EXPORT via VkExportMemoryAllocateInfo); presentation sync is implicit: wsi_common exports a
 * sync_file from a semaphore the present submission signals (mali_sync export_sync_file) and imports it
 * into the image's dma-buf (DMA_BUF_IOCTL_IMPORT_SYNC_FILE). No explicit-sync protocol (gamescope has none).
 * Everything else lives in the runtime, wsi_common and the entrypoints in mali_image.c / mali_memory.c. */
#include "mali_private.h"
#include "mali_queue.h"

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
static VKAPI_ATTR void VKAPI_CALL
mali_wsi_destroy_query_pool(VkDevice _dev, VkQueryPool pool, const VkAllocationCallbacks *a)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   PFN_vkDestroyQueryPool real = mali_device_pdev(dev)->wsi_real_destroy_query_pool;   /* per physical device, no process-global */
   if (pool != VK_NULL_HANDLE && real)
      real(_dev, pool, a);
}

/* wsi_common waits on its own per-image throttle fence before every present (wsi_common.c:2469 in
 * wsi_common_queue_present, with a ~0ull timeout; the X11 present thread at wsi_common_x11.c:2557; the software path
 * at wsi_common.c:2890). No CPU read of driver memory follows any of those waits: the driver submits the present next
 * and the app gets no host-coherent snapshot out of them. The device->CPU CSYNC sweep in kbase_fence_wait (one
 * MEM_SYNC ioctl per mapped BO, per present) therefore buys nothing there, so flag the thread while the wait runs.
 * An app's own vkWaitForFences/vkGetFenceStatus runs with the flag clear and still invalidates. */
static VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_wait_for_fences(VkDevice _dev, uint32_t fence_count, const VkFence *fences, VkBool32 wait_all, uint64_t timeout)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   PFN_vkWaitForFences real = mali_device_pdev(dev)->wsi_real_wait_for_fences;
   if (!real)
      return vk_error(dev, VK_ERROR_INITIALIZATION_FAILED);
   mali_fence_wait_no_csync++;
   VkResult r = real(_dev, fence_count, fences, wait_all, timeout);
   mali_fence_wait_no_csync--;
   return r;
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
   /* No DRM node behind kbase: wsi sees no render/primary device and assumes the compositor is on our GPU.
    * Modifiers (linear only) exist only on the dma-buf path; the software path copies into CPU-visible buffers. */
   pdev->wsi.supports_modifiers = !sw;
   pdev->wsi_real_destroy_query_pool = pdev->wsi.DestroyQueryPool;
   pdev->wsi.DestroyQueryPool = mali_wsi_destroy_query_pool;
   pdev->wsi_real_wait_for_fences = pdev->wsi.WaitForFences;
   pdev->wsi.WaitForFences = mali_wsi_wait_for_fences;
   pdev->vk.wsi_device = &pdev->wsi;
   return VK_SUCCESS;
}

void
mali_wsi_finish(struct mali_physical_device *pdev)
{
   pdev->vk.wsi_device = NULL;
   wsi_device_finish(&pdev->wsi, &pdev->vk.instance->alloc);
}
