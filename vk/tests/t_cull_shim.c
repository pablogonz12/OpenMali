/* SPDX-License-Identifier: MIT */
/* MALI_ADVERTISE_CULL compatibility shim (src/mali_cull_shim.h), both env states. Loader-free.
 *   t_cull_shim --direct <icd.so> --expect on|off
 * off (default driver): shaderCullDistance false, maxCullDistances 0, CreateDevice with shaderCullDistance rejected.
 * on  (MALI_ADVERTISE_CULL=1, run by check.sh): shaderCullDistance true, maxCullDistances 8 and maxCombinedClipAndCullDistances 8 (the Vulkan
 *     minimums), the device is created with the feature. gl_CullDistance writes are ignored (t_vk_feat --only cull draws them). Not conformance. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)

int
main(int argc, char **argv)
{
   const char *icd = NULL, *expect = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--expect") && i + 1 < argc) expect = argv[++i];
   }
   if (!icd || !expect || (strcmp(expect, "on") && strcmp(expect, "off"))) { printf("usage: t_cull_shim --direct <icd.so> --expect on|off\n"); return 2; }
   const int on = !strcmp(expect, "on");
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) return 1;
   VkInstance inst = NULL;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) return 1;
#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceFeatures); I(GetPhysicalDeviceFeatures2); I(GetPhysicalDeviceProperties);
   I(CreateDevice); I(GetDeviceProcAddr); I(DestroyInstance);
   uint32_t n = 1;
   VkPhysicalDevice pd;
   CHECK(EnumeratePhysicalDevices(inst, &n, &pd) == VK_SUCCESS && n == 1, "enumerate");

   VkPhysicalDeviceFeatures f1;
   GetPhysicalDeviceFeatures(pd, &f1);
   VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
   GetPhysicalDeviceFeatures2(pd, &f2);
   CHECK(f1.shaderCullDistance == (on ? VK_TRUE : VK_FALSE), "features.shaderCullDistance %u (expect %s)", f1.shaderCullDistance, expect);
   CHECK(f2.features.shaderCullDistance == f1.shaderCullDistance, "Features2 disagrees with Features");
   CHECK(f1.shaderClipDistance, "shaderClipDistance must stay on");

   VkPhysicalDeviceProperties pr;
   GetPhysicalDeviceProperties(pd, &pr);
   const VkPhysicalDeviceLimits *L = &pr.limits;
   CHECK(L->maxClipDistances == 8 && L->maxCombinedClipAndCullDistances == 8, "clip %u combined %u (expect 8/8)", L->maxClipDistances, L->maxCombinedClipAndCullDistances);
   CHECK(L->maxCullDistances == (on ? 8u : 0u), "maxCullDistances %u (expect %u)", L->maxCullDistances, on ? 8u : 0u);

   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = (float[]){1.0f}};
   VkPhysicalDeviceFeatures want = {.shaderCullDistance = VK_TRUE};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &want};
   VkDevice dev = NULL;
   VkResult r = CreateDevice(pd, &dci, NULL, &dev);
   if (on) {
      CHECK(r == VK_SUCCESS && dev, "CreateDevice with shaderCullDistance: %d", r);
      PFN_vkDestroyDevice DD = dev ? (PFN_vkDestroyDevice)GetDeviceProcAddr(dev, "vkDestroyDevice") : NULL;
      if (DD) DD(dev, NULL);
   } else {
      CHECK(r == VK_ERROR_FEATURE_NOT_PRESENT, "shaderCullDistance must be rejected with the shim off: %d", r);
   }
   DestroyInstance(inst, NULL);
   printf("{\"test\":\"t_cull_shim\",\"expect\":\"%s\",\"pass\":%s}\n", expect, g_fail ? "false" : "true");
   return g_fail;
}
