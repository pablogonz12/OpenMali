/* SPDX-License-Identifier: MIT */
/* One blocked vkAcquireNextImageKHR must not stall another thread (CR #323 N4).
 *
 * The per-call present mode is a device-wide pointer (pdev->vk.wsi_device) and our wrappers used to hold the wsi
 * lock for the whole call. A vkAcquireNextImageKHR that waits - timeout UINT64_MAX is what a real client passes -
 * therefore held the lock for as long as it waited, and every other thread of the same device queued behind it:
 * surface queries, swapchain creation, present on another swapchain. With Steam's client + web helper that is a
 * frozen UI, indistinguishable from a driver hang. Acquire never reads the mode (it is only consulted when images
 * are created and when they are presented), so it must take neither the lock nor the mode.
 *
 *   t_vk_threads --direct <icd.so> [--expect-name <substr>] [--watchdog-ms N]
 *
 * Exhausts swapchain A, blocks a thread in acquire(A) for 5 s, then times a surface query, a second swapchain
 * creation and a present on B in the main thread. Exit 0 = every main-thread call finished inside the watchdog
 * (default 2000 ms), 77 = skipped (no X server / no xcb surface), 1 = something waited behind the blocked acquire.
 */
#define _GNU_SOURCE
#define VK_USE_PLATFORM_XCB_KHR
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

#define W 128
#define H 96
#define ACQUIRE_TIMEOUT_NS (5ull * 1000 * 1000 * 1000)

#define CK(x)                                                                                     \
   do {                                                                                           \
      VkResult r_ = (x);                                                                          \
      if (r_ != VK_SUCCESS) {                                                                     \
         fprintf(stderr, "FAIL %s:%d %s = %d\n", __FILE__, __LINE__, #x, (int)r_);                \
         return 1;                                                                                \
      }                                                                                           \
   } while (0)
#define REQ(c, m)                                                                                 \
   do {                                                                                           \
      if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, m); return 1; }           \
   } while (0)

static PFN_vkGetInstanceProcAddr gipa;
#define IPFN(i, n) PFN_vk##n n = (PFN_vk##n)gipa(i, "vk" #n)


static const char *
res_name(VkResult r)
{
   switch ((int)r) {
   case VK_SUCCESS: return "VK_SUCCESS";
   case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
   case VK_TIMEOUT: return "VK_TIMEOUT";
   case VK_NOT_READY: return "VK_NOT_READY";
   case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
   case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
   default: return "other";
   }
}

static double now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

struct blocked {
   PFN_vkAcquireNextImageKHR acquire;
   VkDevice dev;
   VkSwapchainKHR sc;
   VkResult result;
   double started, finished;
};

static void *
acquire_thread(void *arg)
{
   struct blocked *b = arg;
   b->started = now_ms();
   b->result = b->acquire(b->dev, b->sc, ACQUIRE_TIMEOUT_NS, VK_NULL_HANDLE, VK_NULL_HANDLE, &(uint32_t){0});
   b->finished = now_ms();
   return NULL;
}

int
main(int argc, char **argv)
{
   const char *direct = NULL, *expect = NULL;
   int watchdog_ms = 2000;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) direct = argv[++i];
      else if (!strcmp(argv[i], "--expect-name") && i + 1 < argc) expect = argv[++i];
      else if (!strcmp(argv[i], "--watchdog-ms") && i + 1 < argc) watchdog_ms = atoi(argv[++i]);
      else { fprintf(stderr, "usage: t_vk_threads --direct icd.so [--expect-name s] [--watchdog-ms N]\n"); return 2; }
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

   const char *iexts[] = {"VK_KHR_surface", "VK_KHR_xcb_surface"};
   VkInstance inst;
   CK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(
      &(VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &(VkApplicationInfo){.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                                                       .pApplicationName = "t_vk_threads",
                                                                       .apiVersion = VK_API_VERSION_1_3},
                              .enabledExtensionCount = 2, .ppEnabledExtensionNames = iexts}, NULL, &inst));
   IPFN(inst, EnumeratePhysicalDevices);
   IPFN(inst, GetPhysicalDeviceProperties);
   IPFN(inst, GetPhysicalDeviceQueueFamilyProperties);
   IPFN(inst, GetPhysicalDeviceSurfaceSupportKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceCapabilitiesKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceFormatsKHR);
   IPFN(inst, CreateXcbSurfaceKHR);
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
   free(qf);
   REQ(fam != ~0u, "no graphics queue");

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
   VkSurfaceKHR surf;
   CK(CreateXcbSurfaceKHR(inst, &(VkXcbSurfaceCreateInfoKHR){.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
                                                             .connection = c, .window = win}, NULL, &surf));
   VkBool32 sup = VK_FALSE;
   CK(GetPhysicalDeviceSurfaceSupportKHR(pd, fam, surf, &sup));
   REQ(sup, "surface not supported by the graphics queue");

   VkSurfaceCapabilitiesKHR caps;
   CK(GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
   uint32_t nf = 0;
   CK(GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL));
   VkSurfaceFormatKHR *fm = calloc(nf + 1, sizeof *fm);
   CK(GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fm));
   VkExtent2D ext = caps.currentExtent.width == 0xFFFFFFFFu ? (VkExtent2D){W, H} : caps.currentExtent;

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
   DPFN(QueuePresentKHR); DPFN(DeviceWaitIdle); DPFN(DestroyDevice);
   VkQueue q = VK_NULL_HANDLE;
   GetDeviceQueue(dev, fam, 0, &q);

   const VkSwapchainCreateInfoKHR sci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf,
      .minImageCount = 3, .imageFormat = fm[0].format, .imageColorSpace = fm[0].colorSpace,
      .imageExtent = ext, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
   VkSwapchainKHR a, b;
   CK(CreateSwapchainKHR(dev, &sci, NULL, &a));
   uint32_t na = 0;
   CK(GetSwapchainImagesKHR(dev, a, &na, NULL));
   printf("swapchain A: %u images\n", na);

   /* Starve A: every image acquired, none released, so the next acquire really waits (timeout 5 s). */
   for (uint32_t i = 0; i < na; i++) {
      VkResult ar = AcquireNextImageKHR(dev, a, 1000ull * 1000 * 1000, VK_NULL_HANDLE, VK_NULL_HANDLE, &(uint32_t){0});
      if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { printf("skipped: cannot exhaust the swapchain (%d)\n", (int)ar); return 77; }
   }
   struct blocked bl = {.acquire = AcquireNextImageKHR, .dev = dev, .sc = a, .result = VK_SUCCESS};
   pthread_t th;
   REQ(!pthread_create(&th, NULL, acquire_thread, &bl), "pthread_create");
   {   /* let the thread reach its wait */
      double t0 = now_ms();
      while (!bl.started && now_ms() - t0 < watchdog_ms) { struct timespec ts = {0, 2 * 1000 * 1000}; nanosleep(&ts, NULL); }
   }
   REQ(bl.finished == 0.0, "the acquire did not block (swapchain not starved): nothing is being tested");
   {   /* ...and is still inside it */
      struct timespec ts = {0, 200 * 1000 * 1000}; nanosleep(&ts, NULL);
   }
   REQ(bl.finished == 0.0, "the acquire returned too early: increase --watchdog-ms or starve more images");

   /* Main thread, same device: a surface query, a second swapchain and a present on it. All must be served while
    * the other thread sits in acquire. */
   double t = now_ms();
   VkBool32 sup2 = VK_FALSE;
   VkResult qr = GetPhysicalDeviceSurfaceSupportKHR(pd, fam, surf, &sup2);
   double q_ms = now_ms() - t;
   printf("surface query while acquire blocks: %s in %.1f ms\n", res_name(qr), q_ms);
   REQ(q_ms < watchdog_ms, "a surface query waited behind a blocked acquire");

   t = now_ms();
   VkResult cr = CreateSwapchainKHR(dev, &sci, NULL, &b);
   double c_ms = now_ms() - t;
   printf("second swapchain while acquire blocks: %s in %.1f ms\n", res_name(cr), c_ms);
   REQ(cr == VK_SUCCESS, "second swapchain creation failed");
   REQ(c_ms < watchdog_ms, "swapchain creation waited behind a blocked acquire");

   t = now_ms();
   uint32_t ib = 0;
   VkResult abr = AcquireNextImageKHR(dev, b, 1000ull * 1000 * 1000, VK_NULL_HANDLE, VK_NULL_HANDLE, &ib);
   double ab_ms = now_ms() - t;
   printf("acquire on the second swapchain while A blocks: %s in %.1f ms\n", res_name(abr), ab_ms);
   REQ(abr == VK_SUCCESS || abr == VK_SUBOPTIMAL_KHR, "acquire on the second swapchain failed");
   REQ(ab_ms < watchdog_ms, "acquire on another swapchain waited behind the blocked one");

   t = now_ms();
   VkResult pr = QueuePresentKHR(q, &(VkPresentInfoKHR){
                                   .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 1,
                                   .pSwapchains = &b, .pImageIndices = &ib});
   double p_ms = now_ms() - t;
   printf("present on the second swapchain while A blocks: %s in %.1f ms\n", res_name(pr), p_ms);
   REQ(p_ms < watchdog_ms, "present waited behind a blocked acquire");

   pthread_join(th, NULL);
   printf("blocked acquire returned %s after %.0f ms (timeout 5000)\n", res_name(bl.result),
          bl.finished - bl.started);
   DeviceWaitIdle(dev);
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   xcb_disconnect(c);
   printf("ok: no main-thread call waited behind the blocked acquire\n");
   return 0;
}
