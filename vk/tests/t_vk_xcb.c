/* SPDX-License-Identifier: MIT */
/* X11 WSI test (STEAM-GAPS.md "X11 WSI"): xcb window -> VK_KHR_xcb_surface -> swapchain -> acquire -> present, 3 frames.
 *   t_vk_xcb [--direct <icd.so>] [--pixels] [--draw|--textured|--depth|--blend [--spv-dir d]] [--expect-name <substr>]
 *   Each draw mode renders a full-screen quad into the *swapchain image* with vkCmdBeginRendering + vkCmdDraw (the path
 *   HL2 takes: dynamic rendering, loadOp CLEAR, storeOp STORE) instead of vkCmdClearColorImage, and with --pixels the
 *   window must read back that mode's colour. Together they bisect a client that draws nothing, one path at a time:
 *   --draw      solid fragment colour, no texture, no depth, no blend      -> red   (BGR 0,0,255)
 *   --textured  sample a 2x2 magenta texture in the fragment shader        -> magenta (BGR 255,0,255)
 *   --depth     D32 attachment cleared to 1.0, depth test+write, quad z=0.5 -> red   (BGR 0,0,255)
 *   --blend     red 50% alpha over a blue clear (srcAlpha/1-srcAlpha)      -> purple (BGR 128,0,128)
 *   --blit      render the quad into an offscreen 1600x720 R8G8B8A8 image, then vkCmdBlitImage into the swapchain:
 *               frame 0 a 1:1 WxH sub-rect, frame 1 a half-size blit (2Wx2H -> WxH), frame 2 the whole 1600x720
 *               scaled to the window (LINEAR). On a B8G8R8A8 swapchain this is also a format conversion (RGBA->BGRA).
 *                                                                              -> red   (BGR 0,0,255) each frame
 *   --copy      render into an offscreen image of the swapchain's own format and vkCmdCopyImage a WxH region
 *               into the swapchain image (1:1, no filter, no conversion)     -> red   (BGR 0,0,255)
 *   --rtt <how> render-to-texture: an offscreen R8G8B8A8_UNORM image (COLOR_ATTACHMENT|SAMPLED, optimal tiling, the
 *               window's size) is written by vkCmdClearColorImage (how=cmdclear) or by a render pass with loadOp CLEAR
 *               to (0.1,0.45,0.9) (how=clear) or a full-screen quad of that colour (how=draw); a barrier
 *               COLOR_ATTACHMENT_WRITE|TRANSFER_WRITE -> SHADER_READ with the layout going to SHADER_READ_ONLY_OPTIMAL;
 *               then a full-screen draw samples it into the swapchain image and presents.
 *                                                                              -> blue   (BGR 230,115,26)
 *               A black window means the sampling draw read zeros, i.e. the GPU-written image was not visible to the
 *               fragment shader (the zink glBlitFramebuffer path).
 *   --swap-usage <hex> --swap-flags <n> --swap-images <n>  exact VkSwapchainCreateInfoKHR fields (defaults: usage
 *               TRANSFER_DST|COLOR_ATTACHMENT, flags 0, minImageCount caps.minImageCount+1): zink's window swapchain is
 *               usage=0x97 flags=MUTABLE_FORMAT with 3 images, so a black window can be bisected on these three knobs.
 *               A refused combination is printed as the driver's VkResult.
 *   --topology list|fan|strip  the quad's primitive (default list): glBlitFramebuffer through u_blitter is a 4-vertex
 *               TRIANGLE_FAN, our own quad a 6-vertex list, and that was the last difference left in the draw record.
 *   --rtt-usage <hex>  with --rtt: the source image's usage (default 0x14 = COLOR_ATTACHMENT|SAMPLED); zink's RTT
 *               texture is 0x97, so the ladder's image can be matched too.
 *   --rtt-split  with --rtt: the offscreen write and the sampling draw go into TWO vkQueueSubmits, joined by a
 *               semaphore (rules out "one submission is what makes it work").
 *   --rtt-blit <clear|draw>  an offscreen R8G8B8A8_UNORM image (COLOR_ATTACHMENT|TRANSFER_SRC|SAMPLED) is written by a
 *               RENDER PASS (loadOp CLEAR to (0.1,0.45,0.9,1); how=draw adds the full-screen quad too) and then copied
 *               with vkCmdBlitImage (NEAREST, same extent) into the swapchain image: the shape of gl-ladder step 1/5
 *               and of its --hold redraw, which is a glBlitFramebuffer every frame (gl-ladder.c:806).
 *                                                                              -> blue   (BGR 230,115,26)
 *   --rtt-blit-split  with --rtt-blit: render pass in submit 1, blit in submit 2, joined by a semaphore.
 *   --hold <s>  keep presenting for <s> seconds instead of 3 frames (a window to screenshot).
 *   SPIR-V comes from --spv-dir (default "shader"): quad.vert.spv + solid.frag.spv, wsitex.frag.spv (--textured,
 *   --rtt), wsiblend.frag.spv (--blend), rtt.frag.spv (--rtt draw).
 * Needs an X server ($DISPLAY). Exit 0 = pass, 77 = skipped (no X, or the ICD does not advertise VK_KHR_xcb_surface: Mesa configured
 * without -Dplatforms=x11), 1 = fail.
 *   --pixels  read the window back with xcb_get_image and require the clear colour (hardware only: the fake backend executes
 *             nothing, so its images stay zero).
 * Without --direct the Vulkan loader is used (VK_DRIVER_FILES selects the ICD). */
#define _GNU_SOURCE
#define VK_USE_PLATFORM_XCB_KHR
#if defined(HAVE_WAYLAND_CLIENT)
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#endif
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>
#if defined(HAVE_WAYLAND_CLIENT)
#include <wayland-client.h>
#endif

#define W 128
#define H 96
#define FRAMES 3

#define CK(x)                                                                                    \
   do {                                                                                          \
      VkResult r_ = (x);                                                                         \
      if (r_ != VK_SUCCESS) {                                                                    \
         fprintf(stderr, "FAIL %s:%d %s = %d\n", __FILE__, __LINE__, #x, (int)r_);               \
         return 1;                                                                               \
      }                                                                                          \
   } while (0)
#define REQ(c, m)                                                                                \
   do {                                                                                          \
      if (!(c)) {                                                                                \
         fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, m);                              \
         return 1;                                                                               \
      }                                                                                          \
   } while (0)

static PFN_vkGetInstanceProcAddr gipa;

/* A memory type index for (typeBits, required property bits), -1 if the device has none. */
static int
mem_type(VkPhysicalDevice pd, uint32_t type_bits, VkMemoryPropertyFlags want, PFN_vkGetPhysicalDeviceMemoryProperties gpmp)
{
   VkPhysicalDeviceMemoryProperties mp;
   gpmp(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount && i < 32; i++)
      if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

/* A SPIR-V file from --spv-dir (the tests' shader directory; the driver itself never reads files). */
static void *
read_spv(const char *dir, const char *name, size_t *sz)
{
   char path[512];
   snprintf(path, sizeof path, "%s/%s", dir, name);
   FILE *f = fopen(path, "rb");
   if (!f) { fprintf(stderr, "FAIL open %s: %s\n", path, strerror(errno)); return NULL; }
   if (fseek(f, 0, SEEK_END) || (*sz = (size_t)ftell(f)) == 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
   void *p = malloc(*sz);
   if (!p || fread(p, 1, *sz, f) != *sz) { fclose(f); free(p); return NULL; }
   fclose(f);
   return p;
}

#define IPFN(i, n) PFN_vk##n n = (PFN_vk##n)gipa(i, "vk" #n)

/* --pres-support: vkGetPhysicalDeviceXcbPresentationSupportKHR *before* any surface exists, which Vulkan allows and
 * zink's kopper path does. On a server without DRI3 the driver has to make the software decision inside that query
 * (wsi_common_x11.c:646 gates it on wsi_device->sw); if it only decides on a later surface query the answer here is
 * FALSE and the client gives up before it ever asks about a surface (CR #323 T6). Exit 0 = TRUE, 1 = FALSE, 77 = skip. */
static int
pres_support_first(void)
{
   int scr;
   xcb_connection_t *c = xcb_connect(NULL, &scr);
   if (xcb_connection_has_error(c)) { printf("skipped: no X server ($DISPLAY)\n"); return 77; }
   IPFN(NULL, EnumerateInstanceExtensionProperties);
   uint32_t n = 0;
   CK(EnumerateInstanceExtensionProperties(NULL, &n, NULL));
   VkExtensionProperties *ep = calloc(n + 1, sizeof *ep);
   CK(EnumerateInstanceExtensionProperties(NULL, &n, ep));
   int has_surf = 0, has_xcb = 0;
   for (uint32_t i = 0; i < n; i++) {
      has_surf |= !strcmp(ep[i].extensionName, "VK_KHR_surface");
      has_xcb |= !strcmp(ep[i].extensionName, "VK_KHR_xcb_surface");
   }
   free(ep);
   if (!has_surf || !has_xcb) { printf("skipped: ICD without xcb surface support\n"); return 77; }
   const char *iexts[] = {"VK_KHR_surface", "VK_KHR_xcb_surface"};
   VkInstance inst;
   CK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(
      &(VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &(VkApplicationInfo){.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                                                       .pApplicationName = "t_vk_xcb --pres-support",
                                                                       .apiVersion = VK_API_VERSION_1_3},
                              .enabledExtensionCount = 2,
                              .ppEnabledExtensionNames = iexts},
      NULL, &inst));
   IPFN(inst, EnumeratePhysicalDevices);
   IPFN(inst, GetPhysicalDeviceQueueFamilyProperties);
   IPFN(inst, GetPhysicalDeviceXcbPresentationSupportKHR);
   IPFN(inst, DestroyInstance);
   uint32_t np = 1;
   VkPhysicalDevice pd;
   VkResult r = EnumeratePhysicalDevices(inst, &np, &pd);
   REQ((r == VK_SUCCESS || r == VK_INCOMPLETE) && np >= 1, "no physical device");
   uint32_t nq = 0;
   GetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
   GetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   uint32_t fam = ~0u;
   for (uint32_t i = 0; i < nq; i++)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { fam = i; break; }
   free(qf);
   REQ(fam != ~0u, "no graphics queue");
   xcb_screen_iterator_t sit = xcb_setup_roots_iterator(xcb_get_setup(c));
   for (int i = 0; i < scr; i++) xcb_screen_next(&sit);
   xcb_screen_t *s = sit.data;
   REQ(s, "no X screen");
   VkBool32 sup = GetPhysicalDeviceXcbPresentationSupportKHR(pd, fam, c, s->root_visual);
   printf("x11 presentation support before any surface: %s\n", sup ? "TRUE" : "FALSE");
   DestroyInstance(inst, NULL);
   xcb_disconnect(c);
   return sup ? 0 : 1;
}

/* --wl-after-x11: an X11 surface query on a server WITHOUT DRI3 must not change the present path of a later
 * Wayland swapchain of the same device (STEAM-GAPS.md "X11 WSI"). Mesa's wsi_device->sw is device-wide and
 * wsi_common_wayland.c:3940 turns every Wayland swapchain into a wl_shm/CPU one while it is set, so the
 * no-DRI3 fallback #322 takes for zink/kopper (a zink client asks surface support first, zink_kopper.c:130)
 * has to stay an X11 decision: on X8 it otherwise took the dma-buf path away from the gamescope WSI layer's
 * Wayland swapchain and the compositor was handed shm (memfd) buffers that no dma-buf import can take
 * (vkGetMemoryFdPropertiesKHR -> VK_ERROR_INVALID_EXTERNAL_HANDLE, 298x in the x8-1121-005438 session).
 *
 * Needs $DISPLAY on a server without DRI3 (tests/xwsi-wl-after-x11.sh starts one) and $WAYLAND_DISPLAY on a
 * compositor with zwp_linux_dmabuf_v1 (gamescope). The harness also asserts the driver's own
 * "platform=wayland ... sw=0" line (MALI_WSI_DEBUG=1). Exit 0 = pass, 77 = skipped, 1 = fail. */
#if defined(HAVE_WAYLAND_CLIENT)

static struct wl_compositor *g_comp;

static void
wl_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
   (void)data; (void)ver;
   if (!strcmp(iface, wl_compositor_interface.name))
      g_comp = wl_registry_bind(reg, name, &wl_compositor_interface, 1);
}

static void
wl_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
   (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener wl_reg_listener = {wl_global, wl_global_remove};

static int
wl_after_x11(void)
{
   int scr;
   xcb_connection_t *c = xcb_connect(NULL, &scr);
   if (xcb_connection_has_error(c)) { printf("skipped: no X server ($DISPLAY)\n"); return 77; }
   struct wl_display *wd = wl_display_connect(NULL);
   REQ(wd, "no Wayland display ($WAYLAND_DISPLAY)");

   IPFN(NULL, EnumerateInstanceExtensionProperties);
   uint32_t n = 0;
   CK(EnumerateInstanceExtensionProperties(NULL, &n, NULL));
   VkExtensionProperties *ep = calloc(n + 1, sizeof *ep);
   CK(EnumerateInstanceExtensionProperties(NULL, &n, ep));
   int has_surf = 0, has_xcb = 0, has_wl = 0;
   for (uint32_t i = 0; i < n; i++) {
      has_surf |= !strcmp(ep[i].extensionName, "VK_KHR_surface");
      has_xcb |= !strcmp(ep[i].extensionName, "VK_KHR_xcb_surface");
      has_wl |= !strcmp(ep[i].extensionName, "VK_KHR_wayland_surface");
   }
   free(ep);
   if (!has_surf || !has_xcb || !has_wl) { printf("skipped: ICD without xcb and wayland surface support\n"); return 77; }

   const char *iexts[] = {"VK_KHR_surface", "VK_KHR_xcb_surface", "VK_KHR_wayland_surface"};
   VkInstance inst;
   CK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(
      &(VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &(VkApplicationInfo){.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                                                       .pApplicationName = "t_vk_xcb --wl-after-x11",
                                                                       .apiVersion = VK_API_VERSION_1_3},
                              .enabledExtensionCount = 3,
                              .ppEnabledExtensionNames = iexts},
      NULL, &inst));
   IPFN(inst, EnumeratePhysicalDevices);
   IPFN(inst, GetPhysicalDeviceQueueFamilyProperties);
   IPFN(inst, CreateXcbSurfaceKHR);
   IPFN(inst, CreateWaylandSurfaceKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceSupportKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceCapabilitiesKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceFormatsKHR);
   IPFN(inst, CreateDevice);
   IPFN(inst, GetDeviceProcAddr);
   IPFN(inst, DestroyInstance);

   uint32_t np = 1;
   VkPhysicalDevice pd;
   VkResult r = EnumeratePhysicalDevices(inst, &np, &pd);
   REQ((r == VK_SUCCESS || r == VK_INCOMPLETE) && np >= 1, "no physical device");
   uint32_t nq = 0;
   GetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
   GetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   uint32_t fam = ~0u;
   for (uint32_t i = 0; i < nq; i++)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { fam = i; break; }
   free(qf);
   REQ(fam != ~0u, "no graphics queue");

   /* 1. X11 surface on the no-DRI3 server: support must be TRUE (#322's gain for zink/kopper). */
   const xcb_setup_t *setup = xcb_get_setup(c);
   xcb_screen_iterator_t sit = xcb_setup_roots_iterator(setup);
   for (int i = 0; i < scr; i++) xcb_screen_next(&sit);
   xcb_screen_t *s = sit.data;
   xcb_window_t win = xcb_generate_id(c);
   uint32_t vals[2] = {s->black_pixel, XCB_EVENT_MASK_EXPOSURE};
   xcb_create_window(c, XCB_COPY_FROM_PARENT, win, s->root, 0, 0, W, H, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
                     XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
   xcb_map_window(c, win);
   xcb_flush(c);
   VkSurfaceKHR xs;
   CK(CreateXcbSurfaceKHR(inst, &(VkXcbSurfaceCreateInfoKHR){.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
                                                             .connection = c, .window = win}, NULL, &xs));
   VkBool32 xsup = VK_FALSE;
   CK(GetPhysicalDeviceSurfaceSupportKHR(pd, fam, xs, &xsup));
   REQ(xsup, "x11 surface not supported (the no-DRI3 answer of #322 is gone)");
   printf("x11: surface support TRUE\n");

   /* 2. Wayland surface, same device: its swapchain must be the dma-buf one, not wsi's wl_shm fallback. */
   struct wl_registry *reg = wl_display_get_registry(wd);
   wl_registry_add_listener(reg, &wl_reg_listener, NULL);
   wl_display_roundtrip(wd);
   REQ(g_comp, "the compositor has no wl_compositor");
   struct wl_surface *wls = wl_compositor_create_surface(g_comp);
   VkSurfaceKHR ws;
   CK(CreateWaylandSurfaceKHR(inst, &(VkWaylandSurfaceCreateInfoKHR){
                                 .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
                                 .display = wd, .surface = wls}, NULL, &ws));
   VkBool32 wsup = VK_FALSE;
   CK(GetPhysicalDeviceSurfaceSupportKHR(pd, fam, ws, &wsup));
   printf("wayland: surface support %s\n", wsup ? "TRUE" : "FALSE");
   REQ(wsup, "wayland surface not supported");

   VkSurfaceCapabilitiesKHR caps;
   CK(GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, ws, &caps));
   uint32_t nf = 0;
   CK(GetPhysicalDeviceSurfaceFormatsKHR(pd, ws, &nf, NULL));
   REQ(nf > 0, "no wayland surface formats");
   VkSurfaceFormatKHR *fm = calloc(nf, sizeof *fm);
   CK(GetPhysicalDeviceSurfaceFormatsKHR(pd, ws, &nf, fm));
   VkExtent2D ext = caps.currentExtent.width == 0xFFFFFFFFu ? (VkExtent2D){W, H} : caps.currentExtent;
   VkDevice dev;
   float prio = 1.0f;
   const char *dexts[] = {"VK_KHR_swapchain"};
   CK(CreateDevice(pd, &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                            .queueCreateInfoCount = 1,
                                            .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
                                               .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                               .queueFamilyIndex = fam, .queueCount = 1, .pQueuePriorities = &prio},
                                            .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts},
                   NULL, &dev));
#define WLFN(n) PFN_vk##n wl_##n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n)
   WLFN(CreateSwapchainKHR); WLFN(GetSwapchainImagesKHR); WLFN(DestroyDevice); WLFN(DestroySwapchainKHR);
   VkSwapchainKHR sc;
   CK(wl_CreateSwapchainKHR(dev, &(VkSwapchainCreateInfoKHR){
                                  .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = ws,
                                  .minImageCount = caps.minImageCount + 1 < 3 ? 3 : caps.minImageCount + 1,
                                  .imageFormat = fm[0].format, .imageColorSpace = fm[0].colorSpace,
                                  .imageExtent = ext, .imageArrayLayers = 1,
                                  .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                  .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                  .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                  .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                  .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE}, NULL, &sc));
   uint32_t ni = 0;
   CK(wl_GetSwapchainImagesKHR(dev, sc, &ni, NULL));
   REQ(ni >= 1, "wayland swapchain has no images");
   printf("wayland swapchain: %u images %ux%u\n", ni, ext.width, ext.height);
   VkImage *imgs = calloc(ni, sizeof *imgs);
   REQ(imgs != NULL, "out of memory");
   CK(wl_GetSwapchainImagesKHR(dev, sc, &ni, imgs));

   /* 3. Present one frame through this dma-buf swapchain (the product path: the gamescope WSI layer hands the
    * compositor a Wayland dma-buf, not X11 shm). The image is cleared, so a MALI_WSI_DUMP written at
    * vkQueuePresentKHR has a known colour to check. The present result is printed but not fatal: a headless
    * gamescope may refuse a dma-buf whose memory the fake backend cannot really export, and the dump is written
    * before wsi_common sees the frame either way. */
   WLFN(GetDeviceQueue); WLFN(AcquireNextImageKHR); WLFN(CreateSemaphore); WLFN(CreateCommandPool);
   WLFN(AllocateCommandBuffers); WLFN(BeginCommandBuffer); WLFN(EndCommandBuffer); WLFN(CmdPipelineBarrier);
   WLFN(CmdBeginRendering); WLFN(CmdEndRendering); WLFN(QueueSubmit); WLFN(QueueWaitIdle); WLFN(QueuePresentKHR);
   WLFN(CreateImageView); WLFN(DestroyImageView); WLFN(DestroyCommandPool); WLFN(DestroySemaphore);
   VkQueue q;
   wl_GetDeviceQueue(dev, fam, 0, &q);
   VkSemaphore sa = VK_NULL_HANDLE, sd = VK_NULL_HANDLE;
   CK(wl_CreateSemaphore(dev, &(VkSemaphoreCreateInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}, NULL, &sa));
   CK(wl_CreateSemaphore(dev, &(VkSemaphoreCreateInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}, NULL, &sd));
   VkCommandPool wpool;
   CK(wl_CreateCommandPool(dev, &(VkCommandPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                           .queueFamilyIndex = fam}, NULL, &wpool));
   VkCommandBuffer wcb;
   CK(wl_AllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                                    .commandPool = wpool,
                                                                    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                                    .commandBufferCount = 1}, &wcb));
   uint32_t wi = 0;
   VkResult acq = wl_AcquireNextImageKHR(dev, sc, UINT64_MAX, sa, VK_NULL_HANDLE, &wi);
   printf("wayland present: acquire %d image %u\n", (int)acq, wi);
   if (acq == VK_SUCCESS || acq == VK_SUBOPTIMAL_KHR) {
      VkImageView wview;
      CK(wl_CreateImageView(dev, &(VkImageViewCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                                          .image = imgs[wi], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                                          .format = fm[0].format,
                                                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
                            NULL, &wview));
      CK(wl_BeginCommandBuffer(wcb, &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
      VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                 .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = imgs[wi],
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      wl_CmdPipelineBarrier(wcb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            0, 0, NULL, 0, NULL, 1, &ib);
      /* Same colour as the X11 path: BGRA8 51,102,204 (R,G,B), so the two dumps are comparable. */
      VkRenderingAttachmentInfo ra = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = wview,
                                      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                      .clearValue = {.color = {.float32 = {0.2f, 0.4f, 0.8f, 1.0f}}}};
      VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                            .renderArea = {{0, 0}, {ext.width, ext.height}}, .layerCount = 1,
                            .colorAttachmentCount = 1, .pColorAttachments = &ra};
      wl_CmdBeginRendering(wcb, &ri);
      wl_CmdEndRendering(wcb);
      ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      ib.dstAccessMask = 0;
      ib.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      ib.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      wl_CmdPipelineBarrier(wcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            0, 0, NULL, 0, NULL, 1, &ib);
      CK(wl_EndCommandBuffer(wcb));
      VkResult sr = wl_QueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                                         .waitSemaphoreCount = 1, .pWaitSemaphores = &sa,
                                                         .pWaitDstStageMask = (VkPipelineStageFlags[]){VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
                                                         .commandBufferCount = 1, .pCommandBuffers = &wcb,
                                                         .signalSemaphoreCount = 1, .pSignalSemaphores = &sd},
                                   VK_NULL_HANDLE);
      printf("wayland present: submit %d\n", (int)sr);
      if (sr == VK_SUCCESS) {
         VkResult pr = wl_QueuePresentKHR(q, &(VkPresentInfoKHR){.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                                                .waitSemaphoreCount = 1, .pWaitSemaphores = &sd,
                                                                .swapchainCount = 1, .pSwapchains = &sc,
                                                                .pImageIndices = &wi});
         printf("wayland present: result %d%s\n", (int)pr, pr == VK_SUCCESS ? "" : " (not fatal: see the header)");
      }
      wl_QueueWaitIdle(q);
      wl_DestroyImageView(dev, wview, NULL);
   }
   free(imgs);
   /* Child objects before their parents (Vulkan lifetime rules): the swapchain, then both surfaces. */
   IPFN(inst, DestroySurfaceKHR);
   wl_DestroySwapchainKHR(dev, sc, NULL);
   wl_DestroyCommandPool(dev, wpool, NULL);
   wl_DestroySemaphore(dev, sa, NULL);
   wl_DestroySemaphore(dev, sd, NULL);
   DestroySurfaceKHR(inst, ws, NULL);
   DestroySurfaceKHR(inst, xs, NULL);
   wl_DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   wl_display_disconnect(wd);
   xcb_disconnect(c);
   return 0;
}

#endif /* HAVE_WAYLAND_CLIENT */

/* --rtt-dyn: the dynamic states this test can drive, indexed the way Mesa numbers them (MESA_VK_DYNAMIC_*), which
 * is also the bitmask our driver prints as `dyn=` at pipeline creation. Only states we advertise are listed: the core
 * ones (viewport, scissor, line width, depth bias, blend constants, stencil masks/reference) plus
 * VK_EXT_extended_dynamic_state (+2) and extendedDynamicState3PolygonMode. */
static const struct {
   unsigned bit;
   VkDynamicState st;
} dyn_tab[] = {
   {2, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE}, {3, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY},
   {4, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE},    {8, VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT},
   {9, VK_DYNAMIC_STATE_VIEWPORT},                    {10, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT},
   {11, VK_DYNAMIC_STATE_SCISSOR},                    {17, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE},
   {20, VK_DYNAMIC_STATE_POLYGON_MODE_EXT},           {21, VK_DYNAMIC_STATE_CULL_MODE},
   {22, VK_DYNAMIC_STATE_FRONT_FACE},                 {28, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE},
   {29, VK_DYNAMIC_STATE_DEPTH_BIAS},                 {30, VK_DYNAMIC_STATE_LINE_WIDTH},
   {41, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE},          {42, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE},
   {43, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP},           {46, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE},
   {47, VK_DYNAMIC_STATE_STENCIL_OP},                 {48, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK},
   {49, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK},         {50, VK_DYNAMIC_STATE_STENCIL_REFERENCE},
   {58, VK_DYNAMIC_STATE_BLEND_CONSTANTS},
};

/* set every state of the mask to the value the static pipeline above uses (rasterizerDiscardEnable FALSE, cull NONE,
 * front face CCW, topology TRIANGLE_LIST, no restart, depth bias off, line width 1, blend constants 0, stencil
 * KEEP/KEEP/KEEP with full masks, sample-free) so a difference in the picture is the driver's handling of the state
 * and nothing else */
static void
dyn_set_all(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa, VkCommandBuffer cb, unsigned long long mask, VkBool32 depth,
            const VkViewport *vp, const VkRect2D *sc)
{
#define DP(n) PFN_vk##n n = (PFN_vk##n)gdpa(dev, "vk" #n)
   DP(CmdSetViewport); DP(CmdSetScissor); DP(CmdSetViewportWithCount); DP(CmdSetScissorWithCount);
   DP(CmdSetLineWidth); DP(CmdSetDepthBias); DP(CmdSetDepthBiasEnable); DP(CmdSetBlendConstants);
   DP(CmdSetStencilCompareMask); DP(CmdSetStencilWriteMask); DP(CmdSetStencilReference);
   DP(CmdSetDepthTestEnable); DP(CmdSetDepthWriteEnable); DP(CmdSetDepthCompareOp);
   DP(CmdSetStencilTestEnable); DP(CmdSetStencilOp); DP(CmdSetRasterizerDiscardEnable);
   DP(CmdSetCullMode); DP(CmdSetFrontFace); DP(CmdSetPrimitiveTopology); DP(CmdSetPrimitiveRestartEnable);
   DP(CmdSetPolygonModeEXT);
#define NEED(bit, fn)                                                                                                  \
   do {                                                                                                                \
      if ((mask & (1ull << (bit))) && !fn)                                                                               \
         fprintf(stderr, "MISSING %s (bit %u)\n", #fn, (unsigned)(bit));                                              \
   } while (0)
   NEED(9, CmdSetViewport); NEED(11, CmdSetScissor); NEED(8, CmdSetViewportWithCount); NEED(10, CmdSetScissorWithCount);
   NEED(30, CmdSetLineWidth); NEED(29, CmdSetDepthBias); NEED(28, CmdSetDepthBiasEnable);
   NEED(58, CmdSetBlendConstants); NEED(48, CmdSetStencilCompareMask); NEED(49, CmdSetStencilWriteMask);
   NEED(50, CmdSetStencilReference); NEED(41, CmdSetDepthTestEnable); NEED(42, CmdSetDepthWriteEnable);
   NEED(43, CmdSetDepthCompareOp); NEED(46, CmdSetStencilTestEnable); NEED(47, CmdSetStencilOp);
   NEED(17, CmdSetRasterizerDiscardEnable); NEED(21, CmdSetCullMode); NEED(22, CmdSetFrontFace);
   NEED(3, CmdSetPrimitiveTopology); NEED(4, CmdSetPrimitiveRestartEnable); NEED(20, CmdSetPolygonModeEXT);
   if ((mask & (1ull << 9)) && CmdSetViewport) CmdSetViewport(cb, 0, 1, vp);
   if ((mask & (1ull << 11)) && CmdSetScissor) CmdSetScissor(cb, 0, 1, sc);
   if ((mask & (1ull << 8)) && CmdSetViewportWithCount) CmdSetViewportWithCount(cb, 1, vp);
   if ((mask & (1ull << 10)) && CmdSetScissorWithCount) CmdSetScissorWithCount(cb, 1, sc);
   if ((mask & (1ull << 30)) && CmdSetLineWidth) CmdSetLineWidth(cb, 1.0f);
   if ((mask & (1ull << 29)) && CmdSetDepthBias) CmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
   if ((mask & (1ull << 28)) && CmdSetDepthBiasEnable) CmdSetDepthBiasEnable(cb, VK_FALSE);
   if ((mask & (1ull << 58)) && CmdSetBlendConstants) CmdSetBlendConstants(cb, (const float [4]){0.0f, 0.0f, 0.0f, 0.0f});
   if ((mask & (1ull << 48)) && CmdSetStencilCompareMask) CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
   if ((mask & (1ull << 49)) && CmdSetStencilWriteMask) CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
   if ((mask & (1ull << 50)) && CmdSetStencilReference) CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
   if ((mask & (1ull << 41)) && CmdSetDepthTestEnable) CmdSetDepthTestEnable(cb, depth);
   if ((mask & (1ull << 42)) && CmdSetDepthWriteEnable) CmdSetDepthWriteEnable(cb, depth);
   if ((mask & (1ull << 43)) && CmdSetDepthCompareOp) CmdSetDepthCompareOp(cb, VK_COMPARE_OP_LESS_OR_EQUAL);
   if ((mask & (1ull << 46)) && CmdSetStencilTestEnable) CmdSetStencilTestEnable(cb, VK_FALSE);
   if ((mask & (1ull << 47)) && CmdSetStencilOp)
      CmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
   if ((mask & (1ull << 17)) && CmdSetRasterizerDiscardEnable) CmdSetRasterizerDiscardEnable(cb, VK_FALSE);
   if ((mask & (1ull << 21)) && CmdSetCullMode) CmdSetCullMode(cb, VK_CULL_MODE_NONE);
   if ((mask & (1ull << 22)) && CmdSetFrontFace) CmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
   if ((mask & (1ull << 3)) && CmdSetPrimitiveTopology) CmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
   if ((mask & (1ull << 4)) && CmdSetPrimitiveRestartEnable) CmdSetPrimitiveRestartEnable(cb, VK_FALSE);
   if ((mask & (1ull << 20)) && CmdSetPolygonModeEXT) CmdSetPolygonModeEXT(cb, VK_POLYGON_MODE_FILL);
#undef NEED
#undef DP
}

int
main(int argc, char **argv)
{
   const char *direct = NULL, *expect = NULL, *spv_dir = "shader";
   int pixels = 0, draw = 0, textured = 0, depth = 0, blend = 0, blit = 0, copyimg = 0, wlfirst = 0, pressup = 0;
   int rtt = 0, rtt_split = 0, hold = 0, rttblit = 0, rttblit_split = 0;
   unsigned long long swap_usage = 0, rtt_usage = 0;
   /* --topology: the quad's primitive. quad.vert's 6 positions are (0,0),(1,0),(0,1),(0,1),(1,0),(1,1), whose fan and
    * strip interpretations still cover the whole quad (fan: TL,TR,BL + TL,TR,BR; strip: TL,TR,BL + BL,TR,BR), so only the
    * topology changes and the coverage stays identical. */
   VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   unsigned swap_flags = 0, swap_images = 0;
   unsigned long long rtt_dyn = 0;
   const char *rtt_how = "clear";
   unsigned win_w = W, win_h = H;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--size") && i + 1 < argc) {
         /* The window size is the swapchain size on X11 (caps.currentExtent is the window). Small windows are what
          * Steam creates (32x32); 800x600 and 33x17 cover an ordinary and an odd size. */
         if (sscanf(argv[++i], "%ux%u", &win_w, &win_h) != 2 || !win_w || !win_h) {
            fprintf(stderr, "usage: --size WxH\n");
            return 2;
         }
      } else if (!strcmp(argv[i], "--direct") && i + 1 < argc) direct = argv[++i];
      else if (!strcmp(argv[i], "--pixels")) pixels = 1;
      else if (!strcmp(argv[i], "--draw")) draw = 1;
      else if (!strcmp(argv[i], "--textured")) textured = 1;
      else if (!strcmp(argv[i], "--depth")) depth = 1;
      else if (!strcmp(argv[i], "--blend")) blend = 1;
      else if (!strcmp(argv[i], "--blit")) blit = 1;
      else if (!strcmp(argv[i], "--copy")) copyimg = 1;
      else if (!strcmp(argv[i], "--rtt") && i + 1 < argc) {
         rtt_how = argv[++i];
         if (strcmp(rtt_how, "clear") && strcmp(rtt_how, "draw") && strcmp(rtt_how, "cmdclear")) {
            fprintf(stderr, "usage: --rtt clear|draw|cmdclear\n"); return 2;
         }
         rtt = 1;
      }
      else if (!strcmp(argv[i], "--rtt-split")) rtt_split = 1;
      else if (!strcmp(argv[i], "--rtt-blit") && i + 1 < argc) {
         const char *how = argv[++i];
         if (!strcmp(how, "clear")) rttblit = 1;
         else if (!strcmp(how, "draw")) rttblit = 2;
         else { fprintf(stderr, "usage: --rtt-blit clear|draw\n"); return 2; }
      }
      else if (!strcmp(argv[i], "--rtt-blit-split")) rttblit_split = 1;
      else if (!strcmp(argv[i], "--swap-usage") && i + 1 < argc) swap_usage = strtoull(argv[++i], NULL, 16);
      else if (!strcmp(argv[i], "--swap-flags") && i + 1 < argc) swap_flags = (unsigned)strtoul(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--swap-images") && i + 1 < argc) swap_images = (unsigned)strtoul(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--rtt-usage") && i + 1 < argc) rtt_usage = strtoull(argv[++i], NULL, 16);
      else if (!strcmp(argv[i], "--topology") && i + 1 < argc) {
         const char *t = argv[++i];
         if (!strcmp(t, "list")) topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
         else if (!strcmp(t, "fan")) topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
         else if (!strcmp(t, "strip")) topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
         else { fprintf(stderr, "usage: --topology list|fan|strip\n"); return 2; }
      }
      else if (!strcmp(argv[i], "--rtt-dyn") && i + 1 < argc) {
         /* --rtt-dyn <mask>: the pipeline's dynamic states, bit i = the i-th entry of Mesa's MESA_VK_DYNAMIC_*
          * list (third_party/mesa/src/vulkan/runtime/vk_graphics_state.h). That is the numbering our driver prints
          * as `dyn=` in mali_pipeline.c ("state … dyn=60620f1c"), so a mask copied from a zink session replays
          * zink's own dynamic set. Unknown bits are named and refused. */
         char *end = NULL;
         rtt_dyn = strtoull(argv[++i], &end, 16);
         if (!rtt_dyn || (end && *end)) { fprintf(stderr, "usage: --rtt-dyn <mask-hex>, e.g. 0xa00 or 0x60620f1c\n"); return 2; }
         for (unsigned b = 0; b < 64; b++) {
            if (!(rtt_dyn & (1ull << b))) continue;
            int known = 0;
            for (unsigned k = 0; k < sizeof dyn_tab / sizeof dyn_tab[0]; k++)
               known |= dyn_tab[k].bit == b;
            if (!known) { fprintf(stderr, "usage: --rtt-dyn: bit %u is not a dynamic state this test can set\n", b); return 2; }
         }
      }
      else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--wl-after-x11")) wlfirst = 1;
      else if (!strcmp(argv[i], "--pres-support")) pressup = 1;
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) spv_dir = argv[++i];
      else if (!strcmp(argv[i], "--expect-name") && i + 1 < argc) expect = argv[++i];
      else { fprintf(stderr, "usage: t_vk_xcb [--direct icd.so] [--size WxH] [--pixels] "
                            "[--draw|--textured|--depth|--blend|--blit|--copy] [--rtt clear|draw|cmdclear [--rtt-split] [--rtt-dyn m]] "
                            "[--rtt-blit clear|draw [--rtt-blit-split]] [--swap-usage h] [--swap-flags n] [--swap-images n] "
                            "[--hold s] [--wl-after-x11] [--pres-support] [--spv-dir d] [--expect-name s]\n"); return 2; }
   }

   void *lib = direct ? dlopen(direct, RTLD_NOW | RTLD_LOCAL) : dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!lib) { fprintf(stderr, "FAIL dlopen: %s\n", dlerror()); return 1; }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, direct ? "vk_icdGetInstanceProcAddr" : "vkGetInstanceProcAddr");
   REQ(gipa, "no GetInstanceProcAddr");
   if (pressup)
      return pres_support_first();
#if defined(HAVE_WAYLAND_CLIENT)
   if (wlfirst)
      return wl_after_x11();
#else
   if (wlfirst) { printf("skipped: t_vk_xcb built without wayland-client\n"); return 77; }
#endif

   int scr;
   xcb_connection_t *c = xcb_connect(NULL, &scr);
   if (xcb_connection_has_error(c)) { printf("skipped: no X server ($DISPLAY)\n"); return 77; }

   IPFN(NULL, EnumerateInstanceExtensionProperties);
   uint32_t n = 0;
   CK(EnumerateInstanceExtensionProperties(NULL, &n, NULL));
   VkExtensionProperties *ep = calloc(n + 1, sizeof *ep);
   CK(EnumerateInstanceExtensionProperties(NULL, &n, ep));
   int has_surf = 0, has_xcb = 0;
   for (uint32_t i = 0; i < n; i++) {
      has_surf |= !strcmp(ep[i].extensionName, "VK_KHR_surface");
      has_xcb |= !strcmp(ep[i].extensionName, "VK_KHR_xcb_surface");
   }
   free(ep);
   if (!has_surf || !has_xcb) { printf("skipped: ICD does not advertise VK_KHR_xcb_surface\n"); return 77; }

   const xcb_setup_t *setup = xcb_get_setup(c);
   xcb_screen_iterator_t sit = xcb_setup_roots_iterator(setup);
   for (int i = 0; i < scr; i++) xcb_screen_next(&sit);
   xcb_screen_t *s = sit.data;
   xcb_window_t win = xcb_generate_id(c);
   uint32_t vals[2] = {s->black_pixel, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
   xcb_create_window(c, XCB_COPY_FROM_PARENT, win, s->root, 0, 0, win_w, win_h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     s->root_visual,
                     XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
   xcb_map_window(c, win);
   xcb_flush(c);

   const char *iexts[] = {"VK_KHR_surface", "VK_KHR_xcb_surface"};
   VkInstance inst;
   CK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(
      &(VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &(VkApplicationInfo){.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                                                       .pApplicationName = "t_vk_xcb",
                                                                       .apiVersion = VK_API_VERSION_1_3},
                              .enabledExtensionCount = 2,
                              .ppEnabledExtensionNames = iexts},
      NULL, &inst));
   IPFN(inst, EnumeratePhysicalDevices);
   IPFN(inst, GetPhysicalDeviceProperties);
   IPFN(inst, GetPhysicalDeviceQueueFamilyProperties);
   IPFN(inst, GetPhysicalDeviceMemoryProperties);
   IPFN(inst, CreateXcbSurfaceKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceSupportKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceCapabilitiesKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceFormatsKHR);
   IPFN(inst, GetPhysicalDeviceSurfacePresentModesKHR);
   IPFN(inst, GetPhysicalDeviceXcbPresentationSupportKHR);
   IPFN(inst, DestroySurfaceKHR);
   IPFN(inst, CreateDevice);
   IPFN(inst, GetDeviceProcAddr);
   IPFN(inst, DestroyInstance);

   uint32_t np = 1;
   VkPhysicalDevice pd;
   VkResult r = EnumeratePhysicalDevices(inst, &np, &pd);
   REQ((r == VK_SUCCESS || r == VK_INCOMPLETE) && np >= 1, "no physical device");
   VkPhysicalDeviceProperties props;
   GetPhysicalDeviceProperties(pd, &props);
   printf("device: %s\n", props.deviceName);
   if (expect) REQ(strstr(props.deviceName, expect), "device name mismatch");

   uint32_t nq = 0;
   GetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
   GetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   uint32_t fam = ~0u;
   for (uint32_t i = 0; i < nq; i++)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { fam = i; break; }
   REQ(fam != ~0u, "no graphics queue");
   free(qf);

   VkSurfaceKHR surf;
   CK(CreateXcbSurfaceKHR(inst, &(VkXcbSurfaceCreateInfoKHR){.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
                                                             .connection = c, .window = win}, NULL, &surf));
   VkBool32 sup = VK_FALSE;
   CK(GetPhysicalDeviceSurfaceSupportKHR(pd, fam, surf, &sup));
   REQ(sup, "surface not supported by the graphics queue");
   REQ(GetPhysicalDeviceXcbPresentationSupportKHR(pd, fam, c, s->root_visual), "xcb presentation support is false");

   VkSurfaceCapabilitiesKHR caps;
   CK(GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
   printf("caps: images %u..%u extent %ux%u usage 0x%x\n", caps.minImageCount, caps.maxImageCount, caps.currentExtent.width,
          caps.currentExtent.height, caps.supportedUsageFlags);
   REQ(caps.minImageCount >= 1, "minImageCount 0");
   REQ(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT, "no TRANSFER_DST usage");
   REQ(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "no COLOR_ATTACHMENT usage");

   uint32_t nf = 0;
   CK(GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL));
   REQ(nf > 0, "no surface formats");
   VkSurfaceFormatKHR *fm = calloc(nf, sizeof *fm);
   CK(GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fm));
   VkSurfaceFormatKHR fmt = fm[0];
   for (uint32_t i = 0; i < nf; i++)
      if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM) fmt = fm[i];
   free(fm);
   uint32_t nm = 0;
   CK(GetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, NULL));
   VkPresentModeKHR modes[8];
   if (nm > 8) nm = 8;
   CK(GetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, modes));
   int fifo = 0;
   for (uint32_t i = 0; i < nm; i++) fifo |= modes[i] == VK_PRESENT_MODE_FIFO_KHR;
   REQ(fifo, "FIFO present mode missing");
   printf("format %d, %u present modes\n", (int)fmt.format, nm);

   const char *dexts[] = {"VK_KHR_swapchain"};
   float prio = 1.0f;
   /* --draw: the dynamic-rendering path (core 1.3 here, same as the zink/HL2 command streams). */
   VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                           .dynamicRendering = (draw || textured || depth || blend || blit || copyimg || rtt) ? VK_TRUE : VK_FALSE};
   /* --rtt-dyn: enable the dynamic-state features the mask needs, exactly as zink enables what it uses. Our driver
    * advertises extendedDynamicState, extendedDynamicState2 and extendedDynamicState3PolygonMode only. */
   const unsigned long long DYN1 = (1ull << 2) | (1ull << 3) | (1ull << 8) | (1ull << 10) | (1ull << 41) |
                                   (1ull << 42) | (1ull << 43) | (1ull << 46) | (1ull << 47);
   const unsigned long long DYN2 = (1ull << 4) | (1ull << 17) | (1ull << 28);
   VkPhysicalDeviceExtendedDynamicStateFeaturesEXT fed1 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
      .extendedDynamicState = (rtt_dyn & DYN1) ? VK_TRUE : VK_FALSE};
   VkPhysicalDeviceExtendedDynamicState2FeaturesEXT fed2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, .pNext = &fed1,
      .extendedDynamicState2 = (rtt_dyn & DYN2) ? VK_TRUE : VK_FALSE};
   VkPhysicalDeviceExtendedDynamicState3FeaturesEXT fed3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, .pNext = &fed2,
      .extendedDynamicState3PolygonMode = (rtt_dyn & (1ull << 20)) ? VK_TRUE : VK_FALSE};
   if (rtt_dyn) f13.pNext = &fed3;

   VkDevice dev;
   CK(CreateDevice(pd, &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13,
                                             .queueCreateInfoCount = 1,
                                             .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
                                                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                .queueFamilyIndex = fam, .queueCount = 1, .pQueuePriorities = &prio},
                                             .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts}, NULL, &dev));
#define DPFN(n) PFN_vk##n n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n)
   DPFN(GetDeviceQueue); DPFN(CreateSwapchainKHR); DPFN(GetSwapchainImagesKHR); DPFN(AcquireNextImageKHR);
   DPFN(QueuePresentKHR); DPFN(DestroySwapchainKHR); DPFN(CreateCommandPool); DPFN(AllocateCommandBuffers);
   DPFN(BeginCommandBuffer); DPFN(EndCommandBuffer); DPFN(CmdPipelineBarrier); DPFN(CmdClearColorImage);
   DPFN(CreateSemaphore); DPFN(CreateFence); DPFN(WaitForFences); DPFN(ResetFences); DPFN(QueueSubmit);
   DPFN(QueueWaitIdle); DPFN(DeviceWaitIdle); DPFN(DestroyDevice); DPFN(ResetCommandBuffer);
   DPFN(DestroySemaphore); DPFN(DestroyFence); DPFN(DestroyCommandPool);
   DPFN(CreateShaderModule); DPFN(DestroyShaderModule); DPFN(CreatePipelineLayout); DPFN(DestroyPipelineLayout);
   DPFN(CreateGraphicsPipelines); DPFN(DestroyPipeline); DPFN(CreateImageView); DPFN(DestroyImageView);
   DPFN(CmdBeginRendering); DPFN(CmdEndRendering); DPFN(CmdBindPipeline); DPFN(CmdSetViewport); DPFN(CmdSetScissor);
   DPFN(CmdPushConstants); DPFN(CmdDraw); DPFN(CmdBindDescriptorSets); DPFN(CmdBlitImage); DPFN(CmdCopyImage);
   DPFN(CreateImage); DPFN(DestroyImage); DPFN(BindImageMemory); DPFN(GetImageMemoryRequirements);
   DPFN(GetImageSubresourceLayout); DPFN(AllocateMemory); DPFN(FreeMemory); DPFN(MapMemory); DPFN(UnmapMemory);
   DPFN(CreateSampler); DPFN(DestroySampler);
   DPFN(CreateDescriptorSetLayout); DPFN(DestroyDescriptorSetLayout); DPFN(CreateDescriptorPool);
   DPFN(DestroyDescriptorPool); DPFN(AllocateDescriptorSets); DPFN(UpdateDescriptorSets);
   VkQueue q;
   GetDeviceQueue(dev, fam, 0, &q);

   VkExtent2D ext = caps.currentExtent;
   if (ext.width == 0xFFFFFFFFu) ext = (VkExtent2D){win_w, win_h};
   uint32_t want = swap_images ? swap_images : caps.minImageCount + 1;
   if (want < caps.minImageCount) want = caps.minImageCount;
   if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
   /* --swap-usage/--swap-flags/--swap-images: the ladder's (zink's) window swapchain is usage 0x97
    * (TRANSFER_SRC|TRANSFER_DST|SAMPLED|COLOR_ATTACHMENT|INPUT_ATTACHMENT), flags 0x4 (MUTABLE_FORMAT) and 3 images,
    * while this test's default is 0x12 flags 0x0 — the one difference left between the black and the painting session. */
   VkImageFormatListCreateInfoKHR fli = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
                                        .viewFormatCount = 1, .pViewFormats = &fmt.format};
   VkSwapchainCreateInfoKHR swci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                   .pNext = (swap_flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) ? &fli : NULL,
                                   .surface = surf, .minImageCount = want,
                                   .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
                                   .imageExtent = ext, .imageArrayLayers = 1,
                                   .imageUsage = swap_usage ? (VkImageUsageFlags)swap_usage
                                                            : (VkImageUsageFlags)(VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
                                   .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                   .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                   .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
                                   .flags = swap_flags};
   printf("swap-request: minImageCount=%u (caps %u..%u, asked %u) usage=0x%x flags=0x%x\n", want, caps.minImageCount,
          caps.maxImageCount, swap_images, (unsigned)swci.imageUsage, swci.flags);
   VkSwapchainKHR sc;
   CK(CreateSwapchainKHR(dev, &swci, NULL, &sc));
   uint32_t ni = 0;
   CK(GetSwapchainImagesKHR(dev, sc, &ni, NULL));
   REQ(ni >= want, "fewer swapchain images than requested");
   VkImage imgs[16];
   if (ni > 16) ni = 16;
   CK(GetSwapchainImagesKHR(dev, sc, &ni, imgs));
   printf("swapchain: %u images %ux%u\n", ni, ext.width, ext.height);

   /* Draw modes: one 2D colour view per swapchain image + quad.vert (full-screen quad from gl_VertexIndex, push constant
    * s = (1,1,0,0) so uv = q) with a per-mode fragment shader. --textured adds a 2x2 magenta texture + sampler,
    * --depth a D32 attachment, --blend a srcAlpha/1-srcAlpha blend over a blue clear. */
   const int draw_like = draw || textured || depth || blend || blit || copyimg || rtt || rttblit;
   VkImageView views[16] = {0};
   VkPipeline pipe = VK_NULL_HANDLE, pipe2 = VK_NULL_HANDLE;   /* pipe2: --rtt draw, colour attachment R8G8B8A8 */
   VkPipelineLayout play = VK_NULL_HANDLE;
   VkDescriptorSet dset = VK_NULL_HANDLE;
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
   VkDescriptorPool dpool = VK_NULL_HANDLE;
   VkImage tex = VK_NULL_HANDLE, dimg = VK_NULL_HANDLE, off = VK_NULL_HANDLE;
   VkImageView offview = VK_NULL_HANDLE;
   VkDeviceMemory offmem = VK_NULL_HANDLE;
   uint32_t offw = 0, offh = 0;          /* --blit/--copy offscreen extent */
   VkFormat ofmt = VK_FORMAT_UNDEFINED;  /* its format (the pipeline's colour attachment while blitting/copying) */
   VkImageView texview = VK_NULL_HANDLE, dview = VK_NULL_HANDLE;
   VkSampler samp = VK_NULL_HANDLE;
   VkDeviceMemory texmem = VK_NULL_HANDLE, dmem = VK_NULL_HANDLE;
   if (draw_like) {
      for (uint32_t i = 0; i < ni; i++)
         CK(CreateImageView(dev, &(VkImageViewCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = imgs[i],
                                                          .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt.format,
                                                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}, NULL, &views[i]));
      if (rtt || rttblit) {
         /* --rtt: an offscreen R8G8B8A8_UNORM colour attachment the GPU writes on its own, then a draw samples it
          * into the swapchain image. Optimal tiling at the window's size, i.e. the shape of a game's render texture.
          * --rtt-blit: the same image, but written by a render pass and copied with vkCmdBlitImage (TRANSFER_SRC). */
         VkImageUsageFlags uu = rtt_usage ? (VkImageUsageFlags)rtt_usage
                                          : (VkImageUsageFlags)(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
         if (rttblit) uu |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
         if (rtt && !strcmp(rtt_how, "cmdclear")) uu |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
         CK(CreateImage(dev, &(VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                                  .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {ext.width, ext.height, 1},
                                                  .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                                  .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = uu,
                                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED}, NULL, &off));
         VkMemoryRequirements rmr;
         GetImageMemoryRequirements(dev, off, &rmr);
         const int rmt = mem_type(pd, rmr.memoryTypeBits, 0, GetPhysicalDeviceMemoryProperties);
         REQ(rmt >= 0, "no memory type for the render-to-texture image");
         CK(AllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                       .allocationSize = rmr.size, .memoryTypeIndex = (uint32_t)rmt},
                           NULL, &offmem));
         CK(BindImageMemory(dev, off, offmem, 0));
         CK(CreateImageView(dev, &(VkImageViewCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = off,
                                                         .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}, NULL, &offview));
         if (rttblit)
            printf("rtt-blit: %ux%u R8G8B8A8_UNORM optimal usage=0x%x source=render-pass(%s) split=%d\n", ext.width,
                   ext.height, (unsigned)uu, rttblit == 2 ? "draw" : "clear", rttblit_split);
         else
            printf("rtt: %ux%u R8G8B8A8_UNORM optimal usage=0x%x how=%s split=%d\n", ext.width, ext.height, (unsigned)uu,
                   rtt_how, rtt_split);
      }
      if (textured || rtt) {
         if (textured) {
         /* 2x2 R8G8B8A8 magenta, LINEAR tiling in host-visible memory: the sampler must return exactly (1,0,1). */
         CK(CreateImage(dev, &(VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                                  .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {2, 2, 1}, .mipLevels = 1,
                                                  .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                                  .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED}, NULL, &tex));
         VkMemoryRequirements mr;
         GetImageMemoryRequirements(dev, tex, &mr);
         const int mt = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, GetPhysicalDeviceMemoryProperties);
         REQ(mt >= 0, "no host-visible memory type for the texture");
         CK(AllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt},
                          NULL, &texmem));
         VkSubresourceLayout sl = {0};
         GetImageSubresourceLayout(dev, tex, &(VkImageSubresource){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}, &sl);
         void *tp = NULL;
         CK(MapMemory(dev, texmem, 0, VK_WHOLE_SIZE, 0, &tp));
         for (unsigned y = 0; y < 2; y++)
            for (unsigned x = 0; x < 2; x++)
               memcpy((uint8_t *)tp + sl.offset + y * sl.rowPitch + x * 4, (const uint8_t [4]){255, 0, 255, 255}, 4);
         UnmapMemory(dev, texmem);
         CK(BindImageMemory(dev, tex, texmem, 0));
         CK(CreateImageView(dev, &(VkImageViewCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = tex,
                                                          .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}, NULL, &texview));
         }
         CK(CreateSampler(dev, &(VkSamplerCreateInfo){.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                                      .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                      .maxLod = 0.0f}, NULL, &samp));
         CK(CreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
               .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
               .pBindings = &(VkDescriptorSetLayoutBinding){.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}},
             NULL, &dsl));
         CK(CreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                                    .maxSets = 1, .poolSizeCount = 1,
                                                                    .pPoolSizes = &(VkDescriptorPoolSize){
                                                                       .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                                       .descriptorCount = 1}}, NULL, &dpool));
         CK(AllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                                      .descriptorPool = dpool, .descriptorSetCount = 1,
                                                                      .pSetLayouts = &dsl}, &dset));
         UpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
                                 .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
                                 .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 .pImageInfo = &(VkDescriptorImageInfo){
                                    .sampler = samp, .imageView = textured ? texview : offview,
                                    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}, 0, NULL);
      }
      /* --blit/--copy: the quad is rendered into this image first, then blitted/copied into the swapchain image.
       * --blit uses R8G8B8A8_UNORM (so a B8G8R8A8 swapchain also exercises the channel swizzle); --copy needs the
       * swapchain's own format (Vulkan copy has no conversion). */
      offw = blit ? 1600 : ext.width;
      offh = blit ? 720 : ext.height;
      ofmt = copyimg ? fmt.format : VK_FORMAT_R8G8B8A8_UNORM;
      if (blit || copyimg) {
         CK(CreateImage(dev, &(VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                                  .format = ofmt, .extent = {offw, offh, 1}, .mipLevels = 1,
                                                  .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                                  .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED}, NULL, &off));
         VkMemoryRequirements mr;
         GetImageMemoryRequirements(dev, off, &mr);
         const int mt = mem_type(pd, mr.memoryTypeBits, 0, GetPhysicalDeviceMemoryProperties);
         REQ(mt >= 0, "no memory type for the offscreen image");
         CK(AllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt},
                          NULL, &offmem));
         CK(BindImageMemory(dev, off, offmem, 0));
         CK(CreateImageView(dev, &(VkImageViewCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = off,
                                                          .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ofmt,
                                                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}, NULL, &offview));
         printf("offscreen: %ux%u %s, blit=%d copy=%d\n", offw, offh,
                ofmt == VK_FORMAT_B8G8R8A8_UNORM ? "B8G8R8A8_UNORM" : "R8G8B8A8_UNORM", blit, copyimg);
      }
      if (depth) {
         CK(CreateImage(dev, &(VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                                  .format = VK_FORMAT_D32_SFLOAT, .extent = {ext.width, ext.height, 1},
                                                  .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                                  .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED}, NULL, &dimg));
         VkMemoryRequirements mr;
         GetImageMemoryRequirements(dev, dimg, &mr);
         const int mt = mem_type(pd, mr.memoryTypeBits, 0, GetPhysicalDeviceMemoryProperties);
         REQ(mt >= 0, "no memory type for the depth attachment");
         CK(AllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt},
                          NULL, &dmem));
         CK(BindImageMemory(dev, dimg, dmem, 0));
         CK(CreateImageView(dev, &(VkImageViewCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = dimg,
                                                          .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
                                                          .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}}, NULL, &dview));
      }
      const char *fname = (textured || rtt) ? "wsitex.frag.spv" : blend ? "wsiblend.frag.spv" : "solid.frag.spv";
      size_t vsz = 0, fsz = 0;
      void *vcode = read_spv(spv_dir, "quad.vert.spv", &vsz), *fcode = read_spv(spv_dir, fname, &fsz);
      REQ(vcode && fcode, "SPIR-V not found (--spv-dir)");
      VkShaderModule vm, fm;
      CK(CreateShaderModule(dev, &(VkShaderModuleCreateInfo){.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                             .codeSize = vsz, .pCode = vcode}, NULL, &vm));
      CK(CreateShaderModule(dev, &(VkShaderModuleCreateInfo){.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                             .codeSize = fsz, .pCode = fcode}, NULL, &fm));
      free(vcode); free(fcode);
      CK(CreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                                 .setLayoutCount = (textured || rtt) ? 1 : 0, .pSetLayouts = &dsl,
                                                                 .pushConstantRangeCount = 1,
                                                                 .pPushConstantRanges = &(VkPushConstantRange){
                                                                    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .size = 16}},
                             NULL, &play));
      VkPipelineShaderStageCreateInfo st[2] = {
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vm, .pName = "main"},
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fm, .pName = "main"},
      };
      VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
      if (blend) {   /* classic alpha blend: the fragment shader emits red at alpha 0.5 */
         cba.blendEnable = VK_TRUE;
         cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
         cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
         cba.colorBlendOp = VK_BLEND_OP_ADD;
         cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
         cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
         cba.alphaBlendOp = VK_BLEND_OP_ADD;
      }
      VkPipelineDepthStencilStateCreateInfo dss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                   .depthTestEnable = depth ? VK_TRUE : VK_FALSE,
                                                   .depthWriteEnable = depth ? VK_TRUE : VK_FALSE,
                                                   .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
      VkDynamicState dyn2[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      /* --rtt-dyn: exactly the states in the mask; each one is then set before every draw by dyn_set_all() */
      VkDynamicState dynm[32];
      uint32_t ndyn = 0;
      for (unsigned k = 0; k < sizeof dyn_tab / sizeof dyn_tab[0]; k++)
         if (rtt_dyn & (1ull << dyn_tab[k].bit)) dynm[ndyn++] = dyn_tab[k].st;
      VkDynamicState *dyn = ndyn ? dynm : dyn2;
      const uint32_t dyn_count = ndyn ? ndyn : 2;
      if (rtt_dyn) printf("rtt-dyn: mask 0x%llx, %u pipeline dynamic states\n", rtt_dyn, (unsigned)dyn_count);
      VkFormat dfmt = VK_FORMAT_D32_SFLOAT;
      VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = 2, .pStages = st,
         .pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO},
         .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                                         .topology = topo},
         .pViewportState = &(VkPipelineViewportStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                               .viewportCount = 1, .scissorCount = 1},
         .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                                          .polygonMode = VK_POLYGON_MODE_FILL,
                                                                          .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                                          .lineWidth = 1.0f},
         .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                                      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT},
         .pDepthStencilState = &dss,
         .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                                    .attachmentCount = 1, .pAttachments = &cba},
         .pDynamicState = &(VkPipelineDynamicStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                              .dynamicStateCount = dyn_count, .pDynamicStates = dyn},
         .layout = play,
         .pNext = &(VkPipelineRenderingCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                   .colorAttachmentCount = 1,
                                                   .pColorAttachmentFormats = (blit || copyimg) ? &ofmt : &fmt.format,
                                                   .depthAttachmentFormat = depth ? dfmt : VK_FORMAT_UNDEFINED},
         .renderPass = VK_NULL_HANDLE};
      CK(CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
      if ((rtt && !strcmp(rtt_how, "draw")) || rttblit == 2) {
         /* the writer: the same full-screen quad, but a fragment colour of its own into the R8G8B8A8 offscreen image */
         size_t f2sz = 0;
         void *f2 = read_spv(spv_dir, "rtt.frag.spv", &f2sz);
         REQ(f2, "SPIR-V not found for --rtt draw (--spv-dir)");
         VkShaderModule fm2;
         CK(CreateShaderModule(dev, &(VkShaderModuleCreateInfo){.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                               .codeSize = f2sz, .pCode = f2}, NULL, &fm2));
         free(f2);
         VkPipelineShaderStageCreateInfo st2[2] = { st[0], st[1] };
         st2[1].module = fm2;
         VkFormat rfmt = VK_FORMAT_R8G8B8A8_UNORM;
         VkGraphicsPipelineCreateInfo g2 = gpci;
         g2.pStages = st2;
         g2.pNext = &(VkPipelineRenderingCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                    .colorAttachmentCount = 1, .pColorAttachmentFormats = &rfmt};
         CK(CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &g2, NULL, &pipe2));
         DestroyShaderModule(dev, fm2, NULL);
      }
      DestroyShaderModule(dev, vm, NULL);
      DestroyShaderModule(dev, fm, NULL);
      printf("mode: %s (full-screen quad, dynamic rendering, loadOp CLEAR, STORE)\n",
             rttblit ? "render-to-texture, then vkCmdBlitImage into the swapchain image" :
             rtt ? "render-to-texture, then a draw sampling it into the swapchain image" :
             blit ? "offscreen render + vkCmdBlitImage into the swapchain image" :
             copyimg ? "offscreen render + vkCmdCopyImage into the swapchain image" :
             textured ? "textured 2x2 magenta" : depth ? "depth-tested, D32 clear 1.0" :
             blend ? "alpha blend over a blue clear" : "solid red");
   }

   VkCommandPool pool;
   CK(CreateCommandPool(dev, &(VkCommandPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                                        .queueFamilyIndex = fam}, NULL, &pool));
   VkCommandBuffer cbs[2];
   CK(AllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                                 .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                                 .commandBufferCount = 2}, cbs));
   VkCommandBuffer cb = cbs[0], cb2 = cbs[1];   /* --rtt-split: cb writes the image, cb2 samples it, in two submissions */
   VkSemaphore acq, done, mid;
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   CK(CreateSemaphore(dev, &sci, NULL, &acq));
   CK(CreateSemaphore(dev, &sci, NULL, &done));
   CK(CreateSemaphore(dev, &sci, NULL, &mid));
   VkFence fence;
   CK(CreateFence(dev, &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &fence));

   const VkClearColorValue clr = {.float32 = {0.2f, 0.4f, 0.8f, 1.0f}};   /* BGRA8: 51,102,204 */
   int blitrc = 0;
   struct timespec t0 = {0, 0};
   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (int f = 0; f < FRAMES || hold > 0; f++) {
      if (hold > 0 && f >= FRAMES) {   /* --hold: keep presenting until the deadline, so a screenshot has a window */
         struct timespec tn = {0, 0};
         clock_gettime(CLOCK_MONOTONIC, &tn);
         if (tn.tv_sec - t0.tv_sec >= hold) break;
         struct timespec ts = {0, 16 * 1000 * 1000};
         nanosleep(&ts, NULL);
      }
      uint32_t idx;
      CK(AcquireNextImageKHR(dev, sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx));
      REQ(idx < ni, "acquired index out of range");
      CK(ResetCommandBuffer(cb, 0));
      CK(BeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
      VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (rttblit) {
         /* --rtt-blit: the source is written by a RENDER PASS (loadOp CLEAR, plus a quad for `draw`) and then copied
          * with vkCmdBlitImage into the swapchain image: the shape of gl-ladder step 1/5, whose --hold loop blits a
          * freshly rendered image every frame (gl-ladder.c:806). A black window here means the render pass' write is
          * not visible to the blit. */
         const VkClearColorValue rclr = {.float32 = {0.1f, 0.45f, 0.9f, 1.0f}};   /* RGB 26,115,230 */
         VkImageMemoryBarrier ob = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = off, .subresourceRange = rng};
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            0, 0, NULL, 0, NULL, 1, &ob);
         VkClearValue cv = {0};
         memcpy(cv.color.float32, rclr.float32, sizeof cv.color.float32);
         VkRenderingAttachmentInfo rra = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = offview,
                                         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                         .clearValue = cv};
         VkRenderingInfo rri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                .renderArea = {{0, 0}, {ext.width, ext.height}}, .layerCount = 1,
                                .colorAttachmentCount = 1, .pColorAttachments = &rra};
         CmdBeginRendering(cb, &rri);
         if (rttblit == 2) {   /* `draw`: the same colour from the quad, so a render pass with a real draw */
            VkViewport rvp = {0.0f, 0.0f, (float)ext.width, (float)ext.height, 0.0f, 1.0f};
            VkRect2D rsr = {{0, 0}, {ext.width, ext.height}};
            CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe2);
            CmdSetViewport(cb, 0, 1, &rvp);
            CmdSetScissor(cb, 0, 1, &rsr);
            CmdPushConstants(cb, play, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, (const float [4]){1.0f, 1.0f, 0.0f, 0.0f});
            CmdDraw(cb, 6, 1, 0, 0);
         }
         CmdEndRendering(cb);
         /* the render-pass write -> a transfer read */
         ob.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
         ob.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
         ob.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
         ob.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, NULL, 0, NULL, 1, &ob);
         VkCommandBuffer bcb = cb;
         if (rttblit_split) {   /* the render pass in submit 1, the blit in submit 2 (semaphore `mid`) */
            CK(EndCommandBuffer(cb));
            CK(ResetCommandBuffer(cb2, 0));
            CK(BeginCommandBuffer(cb2, &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
            bcb = cb2;
         }
         VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = imgs[idx], .subresourceRange = rng};
         CmdPipelineBarrier(bcb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
         CmdBlitImage(bcb, off, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                      &(VkImageBlit){.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .srcOffsets = {{0, 0, 0}, {(int32_t)ext.width, (int32_t)ext.height, 1}},
                                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .dstOffsets = {{0, 0, 0}, {(int32_t)ext.width, (int32_t)ext.height, 1}}},
                      VK_FILTER_NEAREST);
         b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
         b.dstAccessMask = 0;
         b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
         b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
         CmdPipelineBarrier(bcb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      } else if (rtt) {
         const int cmdclear = !strcmp(rtt_how, "cmdclear"), howdraw = !strcmp(rtt_how, "draw");
         /* 1. the GPU writes the offscreen image on its own: vkCmdClearColorImage, a render-pass clear, or a quad */
         const VkClearColorValue rclr = {.float32 = {0.1f, 0.45f, 0.9f, 1.0f}};   /* RGB 26,115,230 */
         VkImageMemoryBarrier ob = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = cmdclear ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = cmdclear ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = off, .subresourceRange = rng};
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            cmdclear ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            0, 0, NULL, 0, NULL, 1, &ob);
         if (cmdclear) {
            CmdClearColorImage(cb, off, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &rclr, 1, &rng);
         } else {
            /* --rtt draw: black clear, the quad supplies the colour; --rtt clear: the colour is the clear itself */
            VkClearValue cv = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}};
            if (!howdraw) memcpy(cv.color.float32, rclr.float32, sizeof cv.color.float32);
            VkRenderingAttachmentInfo rra = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = offview,
                                            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                            .clearValue = cv};
            VkRenderingInfo rri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                   .renderArea = {{0, 0}, {ext.width, ext.height}}, .layerCount = 1,
                                   .colorAttachmentCount = 1, .pColorAttachments = &rra};
            CmdBeginRendering(cb, &rri);
            if (howdraw) {
               CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe2);
               VkViewport rvp = {0.0f, 0.0f, (float)ext.width, (float)ext.height, 0.0f, 1.0f};
               VkRect2D rsr = {{0, 0}, {ext.width, ext.height}};
               CmdSetViewport(cb, 0, 1, &rvp);
               CmdSetScissor(cb, 0, 1, &rsr);
               if (rtt_dyn) dyn_set_all(dev, GetDeviceProcAddr, cb, rtt_dyn, depth ? VK_TRUE : VK_FALSE, &rvp, &rsr);
               CmdPushConstants(cb, play, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, (const float [4]){1.0f, 1.0f, 0.0f, 0.0f});
               CmdDraw(cb, 6, 1, 0, 0);
            }
            CmdEndRendering(cb);
         }
         /* 2. the write -> a fragment-shader read, layout to SHADER_READ_ONLY_OPTIMAL */
         const VkImageLayout from = ob.newLayout;
         ob.srcAccessMask = ob.dstAccessMask;
         ob.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         ob.oldLayout = from;
         ob.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         CmdPipelineBarrier(cb, cmdclear ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ob);
         /* 3. --rtt-split: the sampling draw goes into a second command buffer, so into a second submission */
         VkCommandBuffer pcb = cb;
         if (rtt_split) {
            CK(EndCommandBuffer(cb));
            CK(ResetCommandBuffer(cb2, 0));
            CK(BeginCommandBuffer(cb2, &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
            pcb = cb2;
         }
         VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = imgs[idx], .subresourceRange = rng};
         CmdPipelineBarrier(pcb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
         VkClearValue wblack = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}};   /* a lost sample reads 0,0,0 */
         VkRenderingAttachmentInfo wra = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = views[idx],
                                         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                         .clearValue = wblack};
         VkRenderingInfo wri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                .renderArea = {{0, 0}, {ext.width, ext.height}}, .layerCount = 1,
                                .colorAttachmentCount = 1, .pColorAttachments = &wra};
         CmdBeginRendering(pcb, &wri);
         CmdBindPipeline(pcb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
         CmdBindDescriptorSets(pcb, VK_PIPELINE_BIND_POINT_GRAPHICS, play, 0, 1, &dset, 0, NULL);
         VkViewport wvp = {0.0f, 0.0f, (float)ext.width, (float)ext.height, 0.0f, 1.0f};
         VkRect2D wsr = {{0, 0}, {ext.width, ext.height}};
         CmdSetViewport(pcb, 0, 1, &wvp);
         CmdSetScissor(pcb, 0, 1, &wsr);
         if (rtt_dyn) dyn_set_all(dev, GetDeviceProcAddr, pcb, rtt_dyn, depth ? VK_TRUE : VK_FALSE, &wvp, &wsr);
         CmdPushConstants(pcb, play, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, (const float [4]){1.0f, 1.0f, 0.0f, 0.0f});
         CmdDraw(pcb, 6, 1, 0, 0);
         CmdEndRendering(pcb);
         b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
         b.dstAccessMask = 0;
         b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
         b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
         CmdPipelineBarrier(pcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      } else if (blit || copyimg) {

         VkImageMemoryBarrier ob = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = off, .subresourceRange = rng};
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &ob);
         VkClearValue black = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}};
         VkRenderingAttachmentInfo ora = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = offview,
                                         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                         .clearValue = black};
         VkRenderingInfo ori = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                .renderArea = {{0, 0}, {offw, offh}}, .layerCount = 1,
                                .colorAttachmentCount = 1, .pColorAttachments = &ora};
         CmdBeginRendering(cb, &ori);
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
         VkViewport ovp = {0.0f, 0.0f, (float)offw, (float)offh, 0.0f, 1.0f};
         VkRect2D osr = {{0, 0}, {offw, offh}};
         CmdSetViewport(cb, 0, 1, &ovp);
         CmdSetScissor(cb, 0, 1, &osr);
         CmdPushConstants(cb, play, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, (const float [4]){1.0f, 1.0f, 0.0f, 0.0f});
         CmdDraw(cb, 6, 1, 0, 0);
         CmdEndRendering(cb);
         /* 2. offscreen -> TRANSFER_SRC, swapchain image -> TRANSFER_DST */
         ob.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
         ob.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
         ob.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
         ob.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ob);
         VkImageMemoryBarrier sb = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = imgs[idx], .subresourceRange = rng};
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &sb);
         VkImageSubresourceLayers sl = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
         if (copyimg) {
            VkImageCopy cpy = {.srcSubresource = sl, .dstSubresource = sl, .extent = {ext.width, ext.height, 1}};
            CmdCopyImage(cb, off, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
         } else {
            /* frame 0: a 1:1 WxH sub-rect; frame 1: half size (2W x 2H -> W x H); frame 2: the whole 1600x720 scaled */
            VkOffset3D s0 = {0, 0, 0}, d0 = {0, 0, 0}, d1 = {(int32_t)ext.width, (int32_t)ext.height, 1}, s1;
            const int ph = f % 3;   /* --hold repeats the three cases */
            if (ph == 0)      s1 = (VkOffset3D){(int32_t)ext.width, (int32_t)ext.height, 1};
            else if (ph == 1) s1 = (VkOffset3D){(int32_t)ext.width * 2, (int32_t)ext.height * 2, 1};
            else             s1 = (VkOffset3D){(int32_t)offw, (int32_t)offh, 1};
            VkImageBlit bl = {.srcSubresource = sl, .dstSubresource = sl, .srcOffsets = {s0, s1}, .dstOffsets = {d0, d1}};
            CmdBlitImage(cb, off, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &bl, VK_FILTER_LINEAR);
         }
         sb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
         sb.dstAccessMask = 0;
         sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
         sb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &sb);
      } else if (draw_like) {
         VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = imgs[idx], .subresourceRange = rng};
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
         /* blend: a blue field the half-alpha red quad is blended onto; every other mode: black, so a lost draw
          * reads back as 0,0,0 rather than as the clear. */
         VkClearValue clearv = {.color = {.float32 = {0.0f, 0.0f, blend ? 1.0f : 0.0f, 1.0f}}};
         VkRenderingAttachmentInfo ra = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = views[idx],
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                        .clearValue = clearv};
         VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dview,
                                        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                        .clearValue = {.depthStencil = {1.0f, 0}}};
         VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                               .renderArea = {{0, 0}, {ext.width, ext.height}}, .layerCount = 1,
                               .colorAttachmentCount = 1, .pColorAttachments = &ra,
                               .pDepthAttachment = depth ? &da : NULL};
         CmdBeginRendering(cb, &ri);
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
         if (textured)
            CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, play, 0, 1, &dset, 0, NULL);
         VkViewport vp = {0.0f, 0.0f, (float)ext.width, (float)ext.height, 0.0f, 1.0f};
         VkRect2D sr = {{0, 0}, {ext.width, ext.height}};
         CmdSetViewport(cb, 0, 1, &vp);
         CmdSetScissor(cb, 0, 1, &sr);
         CmdPushConstants(cb, play, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, (const float [4]){1.0f, 1.0f, 0.0f, 0.0f});
         CmdDraw(cb, 6, 1, 0, 0);
         CmdEndRendering(cb);
         b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
         b.dstAccessMask = 0;
         b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
         b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      } else {
      VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = imgs[idx], .subresourceRange = rng};
      CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      CmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &rng);
      b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      b.dstAccessMask = 0;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      }
      const int split2 = (rtt && rtt_split) || (rttblit && rttblit_split);
      CK(EndCommandBuffer(split2 ? cb2 : cb));
      VkPipelineStageFlags ws = draw_like ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
      if (split2) {
         /* two submissions joined by a semaphore: the writer (render pass), then the consumer (draw or blit) */
         VkPipelineStageFlags ws2 = rttblit ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
         CK(QueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                              .pWaitSemaphores = &acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1,
                                              .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &mid},
                        fence));
         CK(QueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                              .pWaitSemaphores = &mid, .pWaitDstStageMask = &ws2, .commandBufferCount = 1,
                                              .pCommandBuffers = &cb2, .signalSemaphoreCount = 1, .pSignalSemaphores = &done},
                        VK_NULL_HANDLE));
      } else {
         CK(QueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                              .pWaitSemaphores = &acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1,
                                              .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &done},
                        fence));
      }
      VkResult pres = VK_SUCCESS;
      r = QueuePresentKHR(q, &(VkPresentInfoKHR){.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                                 .pWaitSemaphores = &done, .swapchainCount = 1, .pSwapchains = &sc,
                                                 .pImageIndices = &idx, .pResults = &pres});
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { fprintf(stderr, "FAIL present frame %d = %d\n", f, (int)r); return 1; }
      REQ(pres == VK_SUCCESS || pres == VK_SUBOPTIMAL_KHR, "per-swapchain present result");
      CK(WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
      CK(ResetFences(dev, 1, &fence));
      printf("frame %d: image %u acquired, cleared, presented\n", f, idx);
      if (pixels && (blit || copyimg)) {
         /* each frame is a different blit/copy case, so read the window back per frame instead of once at the end */
         uint8_t px[3] = {0};
         int ok = 0;
         for (int t = 0; t < 50 && !ok; t++) {
            xcb_get_image_reply_t *im = xcb_get_image_reply(
               c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, win, ext.width / 2, ext.height / 2, 1, 1, ~0u), NULL);
            if (im && xcb_get_image_data_length(im) >= 3) {
               memcpy(px, xcb_get_image_data(im), 3);
               ok = px[0] <= 2 && px[1] <= 2 && px[2] >= 253;   /* opaque red */
            }
            free(im);
            if (!ok) {
               struct timespec ts = {0, 20 * 1000 * 1000};
               nanosleep(&ts, NULL);
            }
         }
         printf("frame %d (%s): pixel BGR = %u,%u,%u (want 0,0,255): %s\n", f,
                copyimg ? "copy 1:1" : f % 3 == 0 ? "blit 1:1" : f % 3 == 1 ? "blit half size" : "blit full scaled",
                px[0], px[1], px[2], ok ? "ok" : "MISMATCH");
         if (!ok)
            blitrc = 1;
      }
   }
   CK(QueueWaitIdle(q));
   xcb_flush(c);

   int rc = blitrc;
   if (pixels && !(blit || copyimg)) {
      /* the present may complete asynchronously (Present extension / the copy of the software path): give it a few round trips */
      uint8_t px[4] = {0};
      int ok = 0;
      for (int t = 0; t < 50 && !ok; t++) {
         xcb_get_image_reply_t *im = xcb_get_image_reply(
            c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, win, ext.width / 2, ext.height / 2, 1, 1, ~0u), NULL);
         if (im && xcb_get_image_data_length(im) >= 3) {
            memcpy(px, xcb_get_image_data(im), 3);
            /* X pixel = B,G,R. --textured: magenta (255,0,255); --blend: 50% red over 50% blue (128,0,128);
             * --depth / --draw: opaque red (0,0,255); the clear path: 0.2,0.4,0.8 (204,102,51). */
            if (rtt)
               ok = abs(px[0] - 230) <= 4 && abs(px[1] - 115) <= 4 && abs(px[2] - 26) <= 4;
            else if (textured)
               ok = px[0] >= 253 && px[1] <= 2 && px[2] >= 253;
            else if (blend)
               ok = abs(px[0] - 128) <= 8 && px[1] <= 8 && abs(px[2] - 128) <= 8;
            else if (draw_like)
               ok = px[0] <= 2 && px[1] <= 2 && px[2] >= 253;
            else
               ok = abs(px[0] - 204) <= 2 && abs(px[1] - 102) <= 2 && abs(px[2] - 51) <= 2;
         }
         free(im);
         if (!ok) {
            struct timespec ts = {0, 20 * 1000 * 1000};
            nanosleep(&ts, NULL);
         }
      }
      printf("window pixel BGR = %u,%u,%u (want %s): %s\n", px[0], px[1], px[2],
             rtt ? "230,115,26 (the sampled render texture)" :
             textured ? "255,0,255 (textured quad)" : blend ? "128,0,128 (blended quad)" :
             depth ? "0,0,255 (depth-tested quad)" : draw ? "0,0,255 (the drawn quad)" : "204,102,51 (the clear)",
             ok ? "ok" : "MISMATCH");
      if (!ok) rc = 1;
   }

   DeviceWaitIdle(dev);
   if (draw_like) {
      DestroyPipeline(dev, pipe, NULL);
      if (pipe2)
         DestroyPipeline(dev, pipe2, NULL);
      DestroyPipelineLayout(dev, play, NULL);
      for (uint32_t i = 0; i < ni; i++)
         DestroyImageView(dev, views[i], NULL);
      if (textured || rtt) {
         DestroyDescriptorPool(dev, dpool, NULL);
         DestroyDescriptorSetLayout(dev, dsl, NULL);
         DestroySampler(dev, samp, NULL);
         if (textured) {
            DestroyImageView(dev, texview, NULL);
            DestroyImage(dev, tex, NULL);
            FreeMemory(dev, texmem, NULL);
         }
      }
      if (blit || copyimg || rtt) {
         DestroyImageView(dev, offview, NULL);
         DestroyImage(dev, off, NULL);
         FreeMemory(dev, offmem, NULL);
      }
      if (depth) {
         DestroyImageView(dev, dview, NULL);
         DestroyImage(dev, dimg, NULL);
         FreeMemory(dev, dmem, NULL);
      }
   }
   DestroyFence(dev, fence, NULL);
   DestroySemaphore(dev, acq, NULL);
   DestroySemaphore(dev, done, NULL);
   DestroySemaphore(dev, mid, NULL);
   DestroyCommandPool(dev, pool, NULL);
   DestroySwapchainKHR(dev, sc, NULL);
   DestroyDevice(dev, NULL);
   DestroySurfaceKHR(inst, surf, NULL);
   DestroyInstance(inst, NULL);
   xcb_destroy_window(c, win);
   xcb_disconnect(c);
   printf(rc ? "FAIL\n" : "PASS\n");
   return rc;
}
