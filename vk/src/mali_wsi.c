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
/* Probe of the X server behind $DISPLAY, made before wsi_common sees a surface: DRI3 decides whether the
 * dma-buf path can work at all, MIT-SHM/Present say what the software path will use. MALI_WSI_FORCE_NO_DRI3=1
 * pretends DRI3 is absent (host test of the fallback on a server that has it). */
struct mali_x11_server {
   bool ok, dri3, present, mit_shm;
};

static bool
mali_wsi_force_no_dri3(void)
{
   const char *force = getenv("MALI_WSI_FORCE_NO_DRI3");   /* test hook: behave like a server without DRI3 */
   return force && *force && strcmp(force, "0");
}

#if defined(VK_USE_PLATFORM_XCB_KHR)
static bool
mali_wsi_xcb_extension(xcb_connection_t *conn, const char *name)
{
   xcb_query_extension_reply_t *r =
      xcb_query_extension_reply(conn, xcb_query_extension(conn, strlen(name), name), NULL);
   bool present = r && r->present;
   free(r);
   return present;
}

static void
mali_x11_probe_server(struct mali_x11_server *srv)
{
   memset(srv, 0, sizeof(*srv));
   const char *dpy = getenv("DISPLAY");
   if (!dpy || !*dpy)
      return;
   int screen = 0;
   xcb_connection_t *conn = xcb_connect(dpy, &screen);
   if (xcb_connection_has_error(conn)) {
      xcb_disconnect(conn);
      return;
   }
   srv->ok = true;
   srv->dri3 = mali_wsi_xcb_extension(conn, "DRI3");
   srv->present = mali_wsi_xcb_extension(conn, "Present");
   srv->mit_shm = mali_wsi_xcb_extension(conn, "MIT-SHM");
   xcb_disconnect(conn);
   if (mali_wsi_force_no_dri3())
      srv->dri3 = false;
}
#else
static void
mali_x11_probe_server(struct mali_x11_server *srv)
{
   memset(srv, 0, sizeof(*srv));
}
#endif

static bool
mali_wsi_use_sw(const struct mali_physical_device *pdev)
{
   const char *e = getenv("MALI_WSI");
   if (e && !strcmp(e, "sw"))
      return true;
   if (e && !strcmp(e, "dmabuf") && !mali_wsi_force_no_dri3())
      return false;
   if (pdev->gpu.fake)
      return true;
   /* Hardware, no MALI_WSI: choose before wsi_common creates its per-connection state, so the software path
    * (shm/PutImage) is in place from the first surface. A server without DRI3 cannot import a single dma-buf
    * image (x11_image_init -> xcb_dri3_pixmap_from_buffer -> VK_ERROR_INITIALIZATION_FAILED), which is the
    * "zink: could not create swapchain" a GLX client gets on Xwayland with no DRM node. */
   struct mali_x11_server srv;
   mali_x11_probe_server(&srv);
   if (srv.ok && !srv.dri3) {
      fprintf(stderr, "mali_vk: wsi: X server without DRI3: X11 swapchains use the software (shm/PutImage) path "
              "for the whole device\n");
      return true;
   }
   return false;
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
   int win_w, win_h;
};


/* Reads the client's own X connection (the one the surface was made on): DRI3/Present/MIT-SHM and the window size
 * the server reports. XLIB surfaces are not probed here (that would need libX11 in the ICD); their log line says
 * so, and the fallback below is skipped for them. */
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
   p->have_conn = true;
   p->dri3 = mali_wsi_xcb_extension(s->connection, "DRI3") && !mali_wsi_force_no_dri3();
   p->present = mali_wsi_xcb_extension(s->connection, "Present");
   p->mit_shm = mali_wsi_xcb_extension(s->connection, "MIT-SHM");
   xcb_get_geometry_reply_t *g =
      xcb_get_geometry_reply(s->connection, xcb_get_geometry(s->connection, s->window), NULL);
   if (g) {
      p->win_w = g->width;
      p->win_h = g->height;
      free(g);
   }
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

   /* No DRI3 on the connection the surface lives on: every image of a dma-buf swapchain fails in
    * x11_image_init, so move the device to the software path before Mesa gets there. The connection's
    * MIT-SHM probe was made while sw was false and stays "no shm", which selects wsi_common's per-frame
    * PutImage present path (also correct, just slower). Once only, and not over MALI_WSI=dmabuf (tests use it
    * precisely to see the dma-buf path fail). */
   const char *forced = getenv("MALI_WSI");
   bool forced_dmabuf = forced && !strcmp(forced, "dmabuf");
   if (!wsi_dev->sw && !forced_dmabuf && probe.have_conn && !probe.dri3) {
      wsi_dev->sw = true;
      wsi_dev->supports_modifiers = false;
      fprintf(stderr, "mali_vk: wsi: the X server on %s has no DRI3%s: switching X11 swapchains to the software "
              "(shm/PutImage) path; the dma-buf path cannot present there\n",
              mali_wsi_platform_name(surface->platform),
              probe.present ? "" : " (and no Present)");
   }

   if (mali_wsi_debug() && surface->platform != VK_ICD_WSI_PLATFORM_WAYLAND)
      fprintf(stderr, "mali_vk: x11 present path: shm=%d dri3=%d (window %dx%d%s)\n",
              (int)(probe.mit_shm && wsi_dev->has_import_memory_host), (int)probe.dri3,
              probe.win_w, probe.win_h,
              probe.mit_shm && !wsi_dev->has_import_memory_host ? ", server shm unused without EXT_external_memory_host: PutImage" : "");

   VkResult r = wsi_CreateSwapchainKHR(device, ci, alloc, out);

   if (mali_wsi_debug() || r != VK_SUCCESS) {
      fprintf(stderr, "mali_vk: wsi create_swapchain: platform=%s format=%u colorSpace=%u usage=0x%x mode=%u "
              "extent=%ux%u minImageCount=%u compositeAlpha=0x%x flags=0x%x oldSwapchain=%d sw=%d "
              "dri3=%d present=%d mit_shm=%d window=%dx%d -> %s (%d)\n",
              mali_wsi_platform_name(surface->platform), ci->imageFormat, ci->imageColorSpace,
              ci->imageUsage, ci->presentMode, ci->imageExtent.width, ci->imageExtent.height,
              ci->minImageCount, ci->compositeAlpha, ci->flags, ci->oldSwapchain != VK_NULL_HANDLE,
              (int)wsi_dev->sw, (int)probe.dri3, (int)probe.present, (int)probe.mit_shm,
              probe.win_w, probe.win_h, vk_Result_to_str(r), (int)r);
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
