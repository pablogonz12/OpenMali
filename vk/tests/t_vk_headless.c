/* SPDX-License-Identifier: MIT */
/* VK_EXT_headless_surface + VK_EXT_swapchain_colorspace on the ICD (ANGLE's VerifyExtensionsPresent needs the first; a build without the
 * headless platform in wsi_common must fail here): both are in the instance extension list, vkCreateHeadlessSurfaceEXT resolves and
 * creates a surface, the surface reports formats (sRGB-nonlinear present), a swapchain is created on it with the device's swapchain
 * extension, and three frames are acquired and presented on the fake backend, each one transitioning the image
 * it actually got (oldLayout per image, not per frame).
 *   t_vk_headless --direct <icd.so>      Exit 0 = pass. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)
#define REQ(c, ...) do { int ok_ = !!(c); CHECK(ok_, __VA_ARGS__); if (!ok_) return 1; } while (0)

int
main(int argc, char **argv)
{
   const char *icd = (argc == 3 && !strcmp(argv[1], "--direct")) ? argv[2] : NULL;
   if (!icd) { printf("usage: t_vk_headless --direct <icd.so>\n"); return 2; }
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   REQ(lib, "dlopen %s", dlerror());
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   PFN_vkEnumerateInstanceExtensionProperties eiep = (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");

   uint32_t n = 0; VkExtensionProperties ep[64];
   eiep(NULL, &n, NULL); if (n > 64) n = 64; eiep(NULL, &n, ep);
   int hl = 0, cs = 0;
   for (uint32_t i = 0; i < n; i++) { hl |= !strcmp(ep[i].extensionName, "VK_EXT_headless_surface"); cs |= !strcmp(ep[i].extensionName, "VK_EXT_swapchain_colorspace"); }
   REQ(hl, "VK_EXT_headless_surface in the instance extension list");
   REQ(cs, "VK_EXT_swapchain_colorspace in the instance extension list");

   const char *iexts[] = {"VK_KHR_surface", "VK_EXT_headless_surface", "VK_EXT_swapchain_colorspace", "VK_KHR_get_surface_capabilities2"};
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = 4, .ppEnabledExtensionNames = iexts};
   VkInstance inst = NULL;
   REQ(ci(&ici, NULL, &inst) == VK_SUCCESS, "vkCreateInstance with the headless + colorspace extensions");
#define I(x) PFN_vk##x x = (PFN_vk##x)gipa(inst, "vk" #x)
   I(CreateHeadlessSurfaceEXT); I(DestroySurfaceKHR); I(EnumeratePhysicalDevices); I(CreateDevice); I(GetDeviceProcAddr);
   I(GetPhysicalDeviceSurfaceFormatsKHR); I(GetPhysicalDeviceSurfaceCapabilitiesKHR); I(GetPhysicalDeviceSurfaceSupportKHR);
   I(GetPhysicalDeviceSurfacePresentModesKHR); I(DestroyInstance);
   REQ(CreateHeadlessSurfaceEXT, "vkCreateHeadlessSurfaceEXT resolves through vkGetInstanceProcAddr");

   VkHeadlessSurfaceCreateInfoEXT hci = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
   VkSurfaceKHR surf = VK_NULL_HANDLE;
   REQ(CreateHeadlessSurfaceEXT(inst, &hci, NULL, &surf) == VK_SUCCESS && surf, "vkCreateHeadlessSurfaceEXT");

   uint32_t np = 1; VkPhysicalDevice pd;
   REQ(EnumeratePhysicalDevices(inst, &np, &pd) >= 0 && np == 1, "one physical device");
   VkBool32 sup = 0;
   GetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &sup);
   CHECK(sup, "queue family 0 supports presenting to the headless surface");
   uint32_t nf = 0; VkSurfaceFormatKHR sf[32];
   GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL); if (nf > 32) nf = 32;
   REQ(GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, sf) >= 0 && nf > 0, "surface formats");
   int srgb = 0;
   for (uint32_t i = 0; i < nf; i++) srgb |= sf[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   CHECK(srgb, "sRGB-nonlinear colour space offered");
   uint32_t npm = 0; GetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &npm, NULL);
   CHECK(npm > 0, "present modes");
   VkSurfaceCapabilitiesKHR caps;
   REQ(GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps) == VK_SUCCESS, "surface capabilities");

   float prio = 1;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
   const char *dexts[] = {"VK_KHR_swapchain"};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts};
   VkDevice dev;
   REQ(CreateDevice(pd, &di, NULL, &dev) == VK_SUCCESS, "vkCreateDevice with VK_KHR_swapchain");
#define D(x) PFN_vk##x x = (PFN_vk##x)GetDeviceProcAddr(dev, "vk" #x)
   D(GetDeviceQueue); D(CreateSwapchainKHR); D(GetSwapchainImagesKHR); D(AcquireNextImageKHR); D(QueuePresentKHR); D(DestroySwapchainKHR);
   D(CreateFence); D(DestroyFence); D(WaitForFences); D(DestroyDevice);
   D(CreateCommandPool); D(DestroyCommandPool); D(AllocateCommandBuffers); D(FreeCommandBuffers);
   D(BeginCommandBuffer); D(EndCommandBuffer); D(CmdPipelineBarrier); D(QueueSubmit); D(QueueWaitIdle);
   VkQueue q; GetDeviceQueue(dev, 0, 0, &q);

   VkSwapchainCreateInfoKHR sc = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf,
      .minImageCount = caps.minImageCount > 2 ? caps.minImageCount : 2, .imageFormat = sf[0].format, .imageColorSpace = sf[0].colorSpace,
      .imageExtent = {64, 64}, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
   VkSwapchainKHR chain = VK_NULL_HANDLE;
   VkResult r = CreateSwapchainKHR(dev, &sc, NULL, &chain);
   REQ(r == VK_SUCCESS && chain, "vkCreateSwapchainKHR on the headless surface (%d)", r);
   uint32_t ni = 0; GetSwapchainImagesKHR(dev, chain, &ni, NULL);
   CHECK(ni >= 2, "swapchain has %u images", ni);
   VkImage imgs[16];
   uint32_t ni_have = ni < 16 ? ni : 16;
   REQ(GetSwapchainImagesKHR(dev, chain, &ni_have, imgs) >= 0, "swapchain image handles");
   CHECK(ni_have == ni, "%u of %u image handles", ni_have, ni);

   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0};
   VkCommandPool pool;
   REQ(CreateCommandPool(dev, &cpi, NULL, &pool) == VK_SUCCESS, "command pool");
   VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cb;
   REQ(AllocateCommandBuffers(dev, &cbi, &cb) == VK_SUCCESS, "command buffer");

   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence f; REQ(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   /* Tracked per image, not per frame: a swapchain hands out its images in any order, so a second image
    * acquired after the first was presented has never been transitioned and is still UNDEFINED. */
   bool imgs_presented[16] = {false};
   for (int frame = 0; frame < 3; frame++) {
      uint32_t idx = ~0u;
      r = AcquireNextImageKHR(dev, chain, UINT64_MAX, VK_NULL_HANDLE, f, &idx);
      CHECK(r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR, "acquire frame %d (%d)", frame, r);
      /* idx is only written on success: using it after a failed acquire would index imgs[] out of bounds. */
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) break;
      CHECK(idx < ni_have, "image index %u of %u", idx, ni_have);
      if (idx >= ni_have) break;
      CHECK(WaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull) == VK_SUCCESS, "acquire fence signals");
      /* reset not loaded: use a fresh fence each frame */
      DestroyFence(dev, f, NULL);
      /* A failed recreate would leave f invalid for the next acquire/wait while the test still reported success. */
      REQ(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "recreate fence for frame %d", frame);
      /* This image is UNDEFINED on its first use and PRESENT_SRC once it has been presented; QueuePresentKHR
       * requires a presentation layout, so transition it (from the layout it is really in) before presenting. */
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      REQ(BeginCommandBuffer(cb, &bi) == VK_SUCCESS, "begin transition %d", frame);
      VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                 .oldLayout = imgs_presented[idx] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
                                 .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = imgs[idx],
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, 1, &ib);
      REQ(EndCommandBuffer(cb) == VK_SUCCESS, "end transition %d", frame);
      VkSubmitInfo tsi = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
      REQ(QueueSubmit(q, 1, &tsi, VK_NULL_HANDLE) == VK_SUCCESS, "submit transition %d", frame);
      REQ(QueueWaitIdle(q) == VK_SUCCESS, "wait transition %d", frame);
      imgs_presented[idx] = true;   /* imgs[idx] is in PRESENT_SRC from here on, whatever frame presented it */
      VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 1, .pSwapchains = &chain, .pImageIndices = &idx};
      r = QueuePresentKHR(q, &pi);
      CHECK(r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR, "present frame %d (%d)", frame, r);
   }
   FreeCommandBuffers(dev, pool, 1, &cb);
   DestroyCommandPool(dev, pool, NULL);
   DestroyFence(dev, f, NULL);
   DestroySwapchainKHR(dev, chain, NULL);
   DestroyDevice(dev, NULL);
   DestroySurfaceKHR(inst, surf, NULL);
   if (DestroyInstance)
      DestroyInstance(inst, NULL);
   printf(g_fail ? "FAIL\n" : "ok: headless surface + swapchain acquire/present\n");
   return g_fail;
}
