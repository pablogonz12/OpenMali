/* SPDX-License-Identifier: MIT */
/* VK_EXT_headless_surface + VK_EXT_swapchain_colorspace on the ICD (ANGLE's VerifyExtensionsPresent needs the first; a build without the
 * headless platform in wsi_common must fail here): both are in the instance extension list, vkCreateHeadlessSurfaceEXT resolves and
 * creates a surface, the surface reports formats (sRGB-nonlinear present), a swapchain is created on it with the device's swapchain
 * extension, and one image is acquired and presented on the fake backend.
 *   t_vk_headless --direct <icd.so>      Exit 0 = pass. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)
#define REQ(c, ...) do { CHECK(c, __VA_ARGS__); if (!(c)) return 1; } while (0)

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
   I(GetPhysicalDeviceSurfacePresentModesKHR);
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

   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence f; REQ(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   for (int frame = 0; frame < 3; frame++) {
      uint32_t idx = ~0u;
      r = AcquireNextImageKHR(dev, chain, UINT64_MAX, VK_NULL_HANDLE, f, &idx);
      CHECK(r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR, "acquire frame %d (%d)", frame, r);
      CHECK(idx < ni, "image index %u", idx);
      CHECK(WaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull) == VK_SUCCESS, "acquire fence signals");
      /* reset not loaded: use a fresh fence each frame */
      DestroyFence(dev, f, NULL); CreateFence(dev, &fi, NULL, &f);
      VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 1, .pSwapchains = &chain, .pImageIndices = &idx};
      r = QueuePresentKHR(q, &pi);
      CHECK(r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR, "present frame %d (%d)", frame, r);
   }
   DestroyFence(dev, f, NULL);
   DestroySwapchainKHR(dev, chain, NULL);
   DestroyDevice(dev, NULL);
   DestroySurfaceKHR(inst, surf, NULL);
   printf(g_fail ? "FAIL\n" : "ok: headless surface + swapchain acquire/present\n");
   return g_fail;
}
