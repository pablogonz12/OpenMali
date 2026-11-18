/* SPDX-License-Identifier: MIT */
/* X11 WSI test (STEAM-GAPS.md "X11 WSI"): xcb window -> VK_KHR_xcb_surface -> swapchain -> acquire -> clear -> present, 3 frames.
 *   t_vk_xcb [--direct <icd.so>] [--pixels] [--expect-name <substr>]
 * Needs an X server ($DISPLAY). Exit 0 = pass, 77 = skipped (no X, or the ICD does not advertise VK_KHR_xcb_surface: Mesa configured
 * without -Dplatforms=x11), 1 = fail.
 *   --pixels  read the window back with xcb_get_image and require the clear colour (hardware only: the fake backend executes
 *             nothing, so its images stay zero).
 * Without --direct the Vulkan loader is used (VK_DRIVER_FILES selects the ICD). */
#define _GNU_SOURCE
#define VK_USE_PLATFORM_XCB_KHR
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

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

#define IPFN(i, n) PFN_vk##n n = (PFN_vk##n)gipa(i, "vk" #n)

int
main(int argc, char **argv)
{
   const char *direct = NULL, *expect = NULL;
   int pixels = 0;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) direct = argv[++i];
      else if (!strcmp(argv[i], "--pixels")) pixels = 1;
      else if (!strcmp(argv[i], "--expect-name") && i + 1 < argc) expect = argv[++i];
      else { fprintf(stderr, "usage: t_vk_xcb [--direct icd.so] [--pixels] [--expect-name s]\n"); return 2; }
   }

   void *lib = direct ? dlopen(direct, RTLD_NOW | RTLD_LOCAL) : dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!lib) { fprintf(stderr, "FAIL dlopen: %s\n", dlerror()); return 1; }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, direct ? "vk_icdGetInstanceProcAddr" : "vkGetInstanceProcAddr");
   REQ(gipa, "no GetInstanceProcAddr");

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
   xcb_create_window(c, XCB_COPY_FROM_PARENT, win, s->root, 0, 0, W, H, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
                     XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
   xcb_map_window(c, win);
   xcb_flush(c);

   const char *iexts[] = {"VK_KHR_surface", "VK_KHR_xcb_surface"};
   VkInstance inst;
   CK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(
      &(VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &(VkApplicationInfo){.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                                                       .pApplicationName = "t_vk_xcb",
                                                                       .apiVersion = VK_API_VERSION_1_1},
                              .enabledExtensionCount = 2,
                              .ppEnabledExtensionNames = iexts},
      NULL, &inst));
   IPFN(inst, EnumeratePhysicalDevices);
   IPFN(inst, GetPhysicalDeviceProperties);
   IPFN(inst, GetPhysicalDeviceQueueFamilyProperties);
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
   VkDevice dev;
   CK(CreateDevice(pd, &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
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
   VkQueue q;
   GetDeviceQueue(dev, fam, 0, &q);

   VkExtent2D ext = caps.currentExtent;
   if (ext.width == 0xFFFFFFFFu) ext = (VkExtent2D){W, H};
   uint32_t want = caps.minImageCount + 1;
   if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
   VkSwapchainKHR sc;
   CK(CreateSwapchainKHR(dev, &(VkSwapchainCreateInfoKHR){.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                                          .surface = surf, .minImageCount = want,
                                                          .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
                                                          .imageExtent = ext, .imageArrayLayers = 1,
                                                          .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                          .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                                          .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                                          .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                                          .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE}, NULL, &sc));
   uint32_t ni = 0;
   CK(GetSwapchainImagesKHR(dev, sc, &ni, NULL));
   REQ(ni >= want, "fewer swapchain images than requested");
   VkImage imgs[16];
   if (ni > 16) ni = 16;
   CK(GetSwapchainImagesKHR(dev, sc, &ni, imgs));
   printf("swapchain: %u images %ux%u\n", ni, ext.width, ext.height);

   VkCommandPool pool;
   CK(CreateCommandPool(dev, &(VkCommandPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                                        .queueFamilyIndex = fam}, NULL, &pool));
   VkCommandBuffer cb;
   CK(AllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                                 .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                                 .commandBufferCount = 1}, &cb));
   VkSemaphore acq, done;
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   CK(CreateSemaphore(dev, &sci, NULL, &acq));
   CK(CreateSemaphore(dev, &sci, NULL, &done));
   VkFence fence;
   CK(CreateFence(dev, &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &fence));

   const VkClearColorValue clr = {.float32 = {0.2f, 0.4f, 0.8f, 1.0f}};   /* BGRA8: 51,102,204 */
   for (int f = 0; f < FRAMES; f++) {
      uint32_t idx;
      CK(AcquireNextImageKHR(dev, sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx));
      REQ(idx < ni, "acquired index out of range");
      CK(ResetCommandBuffer(cb, 0));
      CK(BeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
      VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
      CK(EndCommandBuffer(cb));
      VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
      CK(QueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                           .pWaitSemaphores = &acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1,
                                           .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &done},
                     fence));
      VkResult pres = VK_SUCCESS;
      r = QueuePresentKHR(q, &(VkPresentInfoKHR){.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                                 .pWaitSemaphores = &done, .swapchainCount = 1, .pSwapchains = &sc,
                                                 .pImageIndices = &idx, .pResults = &pres});
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { fprintf(stderr, "FAIL present frame %d = %d\n", f, (int)r); return 1; }
      REQ(pres == VK_SUCCESS || pres == VK_SUBOPTIMAL_KHR, "per-swapchain present result");
      CK(WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
      CK(ResetFences(dev, 1, &fence));
      printf("frame %d: image %u acquired, cleared, presented\n", f, idx);
   }
   CK(QueueWaitIdle(q));
   xcb_flush(c);

   int rc = 0;
   if (pixels) {
      /* the present may complete asynchronously (Present extension / the copy of the software path): give it a few round trips */
      uint8_t px[4] = {0};
      int ok = 0;
      for (int t = 0; t < 50 && !ok; t++) {
         xcb_get_image_reply_t *im = xcb_get_image_reply(
            c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, win, ext.width / 2, ext.height / 2, 1, 1, ~0u), NULL);
         if (im && xcb_get_image_data_length(im) >= 3) {
            memcpy(px, xcb_get_image_data(im), 3);
            ok = abs(px[0] - 204) <= 2 && abs(px[1] - 102) <= 2 && abs(px[2] - 51) <= 2;   /* X pixel = B,G,R */
         }
         free(im);
         if (!ok) {
            struct timespec ts = {0, 20 * 1000 * 1000};
            nanosleep(&ts, NULL);
         }
      }
      printf("window pixel BGR = %u,%u,%u (want 204,102,51): %s\n", px[0], px[1], px[2], ok ? "ok" : "MISMATCH");
      if (!ok) rc = 1;
   }

   DeviceWaitIdle(dev);
   DestroyFence(dev, fence, NULL);
   DestroySemaphore(dev, acq, NULL);
   DestroySemaphore(dev, done, NULL);
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
