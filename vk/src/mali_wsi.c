/* SPDX-License-Identifier: MIT */
/* M14: window system integration through Mesa's wsi_common (Wayland platform). Swapchain images are
 * ordinary driver images made linear (scanout flag / DRM modifier LINEAR) on exportable dma-buf BOs
 * (MALI_BO_EXPORT via VkExportMemoryAllocateInfo); presentation sync is implicit: wsi_common exports a
 * sync_file from a semaphore the present submission signals (mali_sync export_sync_file) and imports it
 * into the image's dma-buf (DMA_BUF_IOCTL_IMPORT_SYNC_FILE). No explicit-sync protocol (gamescope has none).
 * Everything else lives in the runtime, wsi_common and the entrypoints in mali_image.c / mali_memory.c. */
#include "mali_private.h"
#include "mali_queue.h"

#include "wsi_common_private.h"   /* struct wsi_interface (pdev->wsi.wsi[]) */

/* wsi_common.c: what the device dispatch would have called; not in wsi_common.h in this tree. */
VKAPI_ATTR VkResult VKAPI_CALL wsi_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain);
VKAPI_ATTR VkResult VKAPI_CALL wsi_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                       VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
VKAPI_ATTR VkResult VKAPI_CALL wsi_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_util.h"

#include <stdlib.h>
#include <string.h>

#if defined(VK_USE_PLATFORM_XCB_KHR)
#include <xcb/xcb.h>
#endif

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

/* X11 WSI (wsi_common_x11, see STEAM-GAPS.md "X11 WSI"): two present paths, chosen once per device.
 *   dma-buf (default on hardware): images are the linear exportable BOs above; the X server imports them through DRI3
 *     (Present). Needs an X server that can import our dma-bufs: Xwayland with glamor on a render node. DroidDeck's
 *     Xwayland on Mali has no DRM node, hence no DRI3 (docs/development/mali-venus.md).
 *   software (fake backend, or MALI_WSI=sw): wsi_common copies each frame into a CPU-visible linear buffer and the X server
 *     gets it by MIT-SHM / PutImage (what lavapipe does, and what the vendor bridge does for X11). Needs no dma-buf at all,
 *     so it also works on the fake backend (whose BO fds are not dma-bufs). The device-wide switch also moves the Wayland
 *     platform to wl_shm: do not use it in a process whose Wayland swapchains must stay zero-copy (the gamescope layer's).
 * MALI_WSI=dmabuf forces the first path on the fake backend (it then fails at swapchain creation; for tests). */
/* Nothing here touches the X server until an X11 surface exists: mali_wsi_init() runs inside
 * vkEnumeratePhysicalDevices for every physical device, whatever the platform, so a blocking xcb_connect() there
 * hung Wayland clients (vkcube said nothing at all) and X11 clients before their first log line. Extension
 * presence is read from the client's own setup reply (no round trip, no connect). */
#if defined(VK_USE_PLATFORM_XCB_KHR)
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/shm.h>
#endif

static bool
mali_wsi_force_no_dri3(void)
{
   const char *force = getenv("MALI_WSI_FORCE_NO_DRI3");   /* test hook: behave like a server without DRI3 */
   return force && *force && strcmp(force, "0");
}

static bool
mali_wsi_use_sw(const struct mali_physical_device *pdev)
{
   const char *e = getenv("MALI_WSI");
   if (e && !strcmp(e, "sw"))
      return true;
   if (e && !strcmp(e, "dmabuf"))
      return false;
   /* No X probe here (see above): the software path is chosen per surface in mali_wsi_create_swapchain(). */
   return pdev->gpu.fake;
}

/* X11 swapchain diagnostics and the no-DRI3 fallback (STEAM-GAPS.md "X11 WSI").
 *
 * wsi_common_x11's dma-buf path imports every swapchain image into an X pixmap with DRI3
 * (wsi_common_x11.c x11_image_init -> xcb_dri3_pixmap_from_buffer / _from_buffers). An X server without DRI3
 * answers that request with an error, x11_image_init returns VK_ERROR_INITIALIZATION_FAILED and the whole
 * vkCreateSwapchainKHR fails: zink reports it as "could not create swapchain" (zink_resource.c:1974) and the GL
 * client then dies on its GLX drawable. Xwayland on a GPU with no DRM node is exactly such a server
 * (docs/development/mali-venus.md), and the software path (shm/PutImage) is the only one that works there.
 *
 * MALI_WSI_DEBUG=1 prints one line per X11 swapchain creation with the request, what the surface offers, what the
 * connection can do and the VkResult; a failed creation always prints a line. */
/* A software swapchain image is a CPU buffer with no dma-buf fd, so the dma-buf implicit-sync path cannot work on
 * it: wsi_common creates a dma-buf semaphore when wsi_drm_check_dma_buf_sync_file_import_export()
 * (wsi_common_drm.c:97) answers "works", and then every present calls
 * wsi_dma_buf_import_sync_file(image->dma_buf_fd == -1, ...), which fails with EBADF and is mapped to
 * VK_ERROR_FEATURE_NOT_PRESENT (wsi_common_drm.c:214) before the frame is put. Declare the capability we have
 * (none) in wsi's cache, which stores result - 1. This has to be applied when the *swapchain* is created, not only
 * when the device was built in software mode: on hardware the software path is chosen per surface. */
static void
mali_wsi_disable_dmabuf_sync(struct wsi_device *wsi)
{
   wsi->cached_sync_file_import_export_result = (uint32_t)(VK_ERROR_FEATURE_NOT_PRESENT - 1);
}

static bool
mali_wsi_debug(void)
{
   static int on = -1;
   if (on < 0) {
      const char *e = getenv("MALI_WSI_DEBUG");
      on = e && *e && strcmp(e, "0") ? 1 : 0;
   }
   return on;
}

static const char *
mali_wsi_platform_name(int platform)
{
   switch (platform) {
   case VK_ICD_WSI_PLATFORM_XCB:     return "xcb";
   case VK_ICD_WSI_PLATFORM_XLIB:    return "xlib";
   case VK_ICD_WSI_PLATFORM_WAYLAND: return "wayland";
   default:                          return "other";
   }
}

struct mali_wsi_x11_probe {
   bool have_conn;
   bool dri3, present, mit_shm;
};

/* Reads the client's own X connection (the one the surface was made on). Extension presence comes from the
 * connection's prefetched setup reply (xcb_get_extension_data never sends a request), so this cannot block on an
 * unresponsive server. XLIB surfaces are not probed (that would need libX11 in the ICD) and take software. */
static void
mali_wsi_x11_probe(VkIcdSurfaceBase *surface, struct mali_wsi_x11_probe *p)
{
   memset(p, 0, sizeof(*p));
#if defined(VK_USE_PLATFORM_XCB_KHR)
   if (surface->platform != VK_ICD_WSI_PLATFORM_XCB)
      return;
   VkIcdSurfaceXcb *s = (VkIcdSurfaceXcb *)surface;
   if (!s->connection)
      return;
   /* Batch the three queries into one round trip, and ask for them up front: retrieving a reply can still
    * block while the server answers, unlike a lookup in an already-fetched setup reply. */
   xcb_prefetch_extension_data(s->connection, &xcb_dri3_id);
   xcb_prefetch_extension_data(s->connection, &xcb_present_id);
   xcb_prefetch_extension_data(s->connection, &xcb_shm_id);
   const struct xcb_query_extension_reply_t *dri3 = xcb_get_extension_data(s->connection, &xcb_dri3_id);
   const struct xcb_query_extension_reply_t *present = xcb_get_extension_data(s->connection, &xcb_present_id);
   const struct xcb_query_extension_reply_t *shm = xcb_get_extension_data(s->connection, &xcb_shm_id);
   p->have_conn = true;
   p->dri3 = dri3 && dri3->present && !mali_wsi_force_no_dri3();
   p->present = present && present->present;
   p->mit_shm = shm && shm->present;
#else
   (void)surface;
#endif
}

/* What the surface offers, as wsi_common_x11 answers it, so a failed creation can be read without a second
 * process: the requested format/usage/mode/extent/count next to the accepted ones. */
static void
mali_wsi_x11_log_surface(struct mali_physical_device *pdev, VkIcdSurfaceBase *surface)
{
   struct wsi_interface *iface = pdev->wsi.wsi[surface->platform];
   if (!iface)
      return;
   VkSurfaceCapabilities2KHR caps = { .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR };
   if (iface->get_capabilities2 &&
       iface->get_capabilities2(surface, &pdev->wsi, NULL, &caps) == VK_SUCCESS) {
      const VkSurfaceCapabilitiesKHR *c = &caps.surfaceCapabilities;
      fprintf(stderr, "mali_vk: wsi surface caps: currentExtent=%ux%u minImageCount=%u maxImageCount=%u "
              "supportedUsage=0x%x compositeAlpha=0x%x transform=0x%x\n",
              c->currentExtent.width, c->currentExtent.height, c->minImageCount, c->maxImageCount,
              c->supportedUsageFlags, c->supportedCompositeAlpha, c->currentTransform);
   }
   VkSurfaceFormatKHR fmt[16];
   uint32_t n = 16;
   if (iface->get_formats && iface->get_formats(surface, &pdev->wsi, &n, fmt) == VK_SUCCESS) {
      fprintf(stderr, "mali_vk: wsi surface formats:");
      for (uint32_t i = 0; i < n && i < 16; i++)
         fprintf(stderr, " %u/%u", fmt[i].format, fmt[i].colorSpace);
      fprintf(stderr, "%s\n", n > 16 ? " ..." : "");
   }
   VkPresentModeKHR mode[8];
   n = 8;
   if (iface->get_present_modes && iface->get_present_modes(surface, &pdev->wsi, &n, mode) == VK_SUCCESS) {
      fprintf(stderr, "mali_vk: wsi present modes:");
      for (uint32_t i = 0; i < n && i < 8; i++)
         fprintf(stderr, " %u", mode[i]);
      fprintf(stderr, "%s\n", n > 8 ? " ..." : "");
   }
}

/* MALI_WSI_DEBUG=1: one line per acquire/present so a stall shows as an "enter" without its matching result
 * (the software present path copies the whole image and PutImage is fire-and-forget, so this is also the only way
 * to see how far a client got). Both are thin wrappers around the runtime's wsi entrypoints. */
VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_acquire_next_image(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                            VkFence fence, uint32_t *image_index)
{
   VK_FROM_HANDLE(mali_device, dev, device);
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   static uint64_t seq;
   uint64_t n = p_atomic_inc_return(&seq);
   if (mali_wsi_debug())
      fprintf(stderr, "mali_vk: wsi acquire #%llu: timeout=%llu sem=%d fence=%d sw=%d\n", (unsigned long long)n,
              (unsigned long long)timeout, semaphore != VK_NULL_HANDLE, fence != VK_NULL_HANDLE,
              (int)pdev->wsi.sw);
   VkResult r = wsi_AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, image_index);
   if (mali_wsi_debug())
      fprintf(stderr, "mali_vk: wsi acquire #%llu -> %s image=%u\n", (unsigned long long)n,
              vk_Result_to_str(r), r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR ? *image_index : 0);
   return r;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_queue_present(VkQueue queue, const VkPresentInfoKHR *info)
{
   static uint64_t seq;
   uint64_t n = p_atomic_inc_return(&seq);
   if (mali_wsi_debug()) {
      VK_FROM_HANDLE(vk_queue, vq, queue);
      const struct mali_physical_device *qpdev =
         mali_device_pdev(container_of(vq->base.device, struct mali_device, vk));
      fprintf(stderr, "mali_vk: wsi present #%llu: swapchains=%u wait_sems=%u dma_buf_sync=%d sw=%d\n",
              (unsigned long long)n, info->swapchainCount, info->waitSemaphoreCount,
              __atomic_load_n(&qpdev->wsi.cached_sync_file_import_export_result, __ATOMIC_RELAXED) == UINT32_MAX,
              (int)qpdev->wsi.sw);
   }
   VkResult r = wsi_QueuePresentKHR(queue, info);
   /* wsi_common_wayland only pumps its per-swapchain present-id queue inside dispatch_present_id_queue(), i.e. at
    * the next present or inside vkWaitForPresentKHR. An app that presents its last frame and destroys the
    * swapchain never gets that pump, so the frame callback/presentation feedback of that commit is still
    * outstanding and wsi_wl_swapchain_chain_free() asserts (!chain->present_ids.dispatch_in_progress)
    * (wsi_common_wayland.c:3782, and the same in the retire path at :2779). Drain it here, bounded: a slow
    * compositor costs at most the cap and never blocks the frame. Only when the swapchain asked for present-id
    * waiting: without that, wsi does not queue an id at all and there is nothing to drain. */
   const VkPresentIdKHR *ids = vk_find_struct_const(info->pNext, PRESENT_ID_KHR);
   if (r == VK_SUCCESS && ids) {
      for (uint32_t i = 0; i < info->swapchainCount && i < ids->swapchainCount; i++) {
         VK_FROM_HANDLE(wsi_swapchain, chain, info->pSwapchains[i]);
         if (chain && chain->present_wait_enabled && chain->wait_for_present2 && ids->pPresentIds &&
             ids->pPresentIds[i])
            chain->wait_for_present2(chain, ids->pPresentIds[i], 20ull * 1000 * 1000);   /* ns, bounded */
      }
   }
   /* SUBOPTIMAL/OUT_OF_DATE are normal swapchain states, not errors: one line per frame would be noise. */
   bool quiet = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR;
   if (mali_wsi_debug() || !quiet)
      fprintf(stderr, "mali_vk: wsi present #%llu -> %s (%d)\n", (unsigned long long)n, vk_Result_to_str(r), (int)r);
   return r;
}

/* Device entrypoint (installed in mali_device.c) in front of wsi_CreateSwapchainKHR: it only observes and, for an
 * X11 surface on a server without DRI3, flips the device to the software path before Mesa allocates images. The
 * real wsi_common_x11 interface (and the struct wsi_x11 it is embedded in) is left exactly as Mesa set it up:
 * wsi_x11_get_connection() casts wsi_device->wsi[XCB] back to struct wsi_x11, so it must not be replaced. */
VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                          const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
   VK_FROM_HANDLE(mali_device, dev, device);
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   struct wsi_device *wsi_dev = &pdev->wsi;
   VkIcdSurfaceBase *surface = (VkIcdSurfaceBase *)(uintptr_t)ci->surface;
   struct mali_wsi_x11_probe probe;

   mali_wsi_x11_probe(surface, &probe);

   /* X11 surfaces default to the software (PutImage) path, decided here, per surface, so Wayland clients never
    * touch the X server: the old unconditional probe ran inside vkEnumeratePhysicalDevices and its xcb_connect()
    * hung them before their first log line. MALI_WSI=dmabuf opts back into dma-buf, where a broken import fails
    * loudly instead of silently dropping presents (the hang in wsi_common_x11.c:2202). The fake backend stays
    * software from init and its tests use MALI_WSI=dmabuf to see the dma-buf path fail on purpose. */
   const char *forced = getenv("MALI_WSI");
   bool forced_dmabuf = forced && !strcmp(forced, "dmabuf");
   bool x11_surface = surface->platform == VK_ICD_WSI_PLATFORM_XCB ||
                      surface->platform == VK_ICD_WSI_PLATFORM_XLIB;
   if (!wsi_dev->sw && !forced_dmabuf && x11_surface) {
      wsi_dev->sw = true;
      wsi_dev->supports_modifiers = false;
      mali_wsi_disable_dmabuf_sync(wsi_dev);
      fprintf(stderr, "mali_vk: wsi: %s surface: software (PutImage) present path (MALI_WSI=dmabuf for dma-buf)\n",
              mali_wsi_platform_name(surface->platform));
   }

   if (mali_wsi_debug() && surface->platform != VK_ICD_WSI_PLATFORM_WAYLAND)
      fprintf(stderr, "mali_vk: wsi hooks: GetSemaphoreFdKHR=%s GetMemoryFdKHR=%s cached_sync_file=%u (dma_buf_sync=%d)\n",
              wsi_dev->GetSemaphoreFdKHR ? "set" : "NULL", wsi_dev->GetMemoryFdKHR ? "set" : "NULL",
              __atomic_load_n(&wsi_dev->cached_sync_file_import_export_result, __ATOMIC_RELAXED),
              __atomic_load_n(&wsi_dev->cached_sync_file_import_export_result, __ATOMIC_RELAXED) == UINT32_MAX);
   if (mali_wsi_debug() && surface->platform != VK_ICD_WSI_PLATFORM_WAYLAND)
      fprintf(stderr, "mali_vk: x11 present path: shm=%d dri3=%d%s\n",
              (int)(probe.mit_shm && wsi_dev->has_import_memory_host), (int)probe.dri3,
              wsi_dev->sw ? " (software)" : " (dma-buf)");

   VkResult r = wsi_CreateSwapchainKHR(device, ci, alloc, out);

   if (mali_wsi_debug() || r != VK_SUCCESS) {
      fprintf(stderr, "mali_vk: wsi create_swapchain: platform=%s format=%u colorSpace=%u usage=0x%x mode=%u "
              "extent=%ux%u minImageCount=%u compositeAlpha=0x%x flags=0x%x oldSwapchain=%d sw=%d "
              "dri3=%d present=%d mit_shm=%d -> %s (%d)\n",
              mali_wsi_platform_name(surface->platform), ci->imageFormat, ci->imageColorSpace,
              ci->imageUsage, ci->presentMode, ci->imageExtent.width, ci->imageExtent.height,
              ci->minImageCount, ci->compositeAlpha, ci->flags, ci->oldSwapchain != VK_NULL_HANDLE,
              (int)wsi_dev->sw, (int)probe.dri3, (int)probe.present, (int)probe.mit_shm,
              vk_Result_to_str(r), (int)r);
      if (r != VK_SUCCESS && !probe.have_conn)
         fprintf(stderr, "mali_vk: wsi: %s surface: the X connection was not probed (xlib), see the X server's "
                 "extensions (xdpyinfo -queryExtensions)\n", mali_wsi_platform_name(surface->platform));
      if (r != VK_SUCCESS && !probe.present && probe.have_conn)
         fprintf(stderr, "mali_vk: wsi: the X server has no Present extension: wsi_common_x11 cannot present at "
                 "all there, so no swapchain can be created\n");
      mali_wsi_x11_log_surface(pdev, surface);
   }

   return r;
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
   /* Capability rule for the software present path: wsi_cpu_image_needs_buffer_blit() (wsi_common.c:3599) returns
    * true unless wsi->wants_linear, and a buffer blit makes wsi record vkCmdBlitImage/vkCmdCopyImage between a
    * CPU buffer image and the presentable one. We do not implement those transfer commands, so vkEndCommandBuffer
    * failed with VK_ERROR_FEATURE_NOT_PRESENT inside the present and vkQueuePresentKHR returned -8 *after* the
    * frame had been copied (mali_cmd.c:1330/1370/1386). wants_linear makes wsi allocate the linear CPU-visible
    * image itself: no blit, the present is a plain PutImage of that memory, and the pixels the app rendered are the
    * pixels that get displayed. It also removes the SHM/blit branch that needs EXT_external_memory_host, which we
    * do not implement: SHM is only used where the extension exists, otherwise PutImage, and both report VK_SUCCESS. */
   pdev->wsi.wants_linear = true;
   if (sw)
      mali_wsi_disable_dmabuf_sync(&pdev->wsi);
   pdev->wsi_real_destroy_query_pool = pdev->wsi.DestroyQueryPool;
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
