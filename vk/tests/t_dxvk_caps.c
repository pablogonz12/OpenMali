/* SPDX-License-Identifier: MIT */
/* R4-M2 test: DXVK-style instance + device create. Loader-free (dlopen the ICD).
 *   t_dxvk_caps --direct <icd.so>
 * 1. instance with apiVersion 1.3 (+ the instance extensions DXVK asks for that we have).
 * 2. DXVK's hard-required feature list (R4-GAP.md sec. 3, DXVK info.cpp) is queried; every item is
 *    printed as ok/MISSING (honest report; missing items are the R4 backlog, not test failures).
 * 3. Device create with exactly the supported subset of that list + the DXVK extension list that we
 *    advertise MUST succeed; creating with the full list (some unsupported) MUST return
 *    VK_ERROR_FEATURE_NOT_PRESENT / EXTENSION_NOT_PRESENT (no silent acceptance).
 * Exit 0 = pass. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                       \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                               \
      }                                                                                            \
   } while (0)

/* DXVK required device extensions (3.x master; 2.7 needs fewer). */
static const char *const dxvk_exts[] = {
   "VK_KHR_swapchain", "VK_EXT_depth_clip_enable", "VK_EXT_robustness2", "VK_KHR_maintenance5",
   "VK_KHR_maintenance6", "VK_KHR_load_store_op_none", "VK_EXT_extended_dynamic_state3",
   "VK_EXT_vertex_attribute_divisor", "VK_EXT_memory_budget", "VK_KHR_calibrated_timestamps",
   "VK_EXT_custom_border_color", "VK_EXT_graphics_pipeline_library", "VK_KHR_pipeline_library",
};

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   int expect_missing = -1;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--expect-missing") && i + 1 < argc) expect_missing = atoi(argv[++i]);
   if (!icd) { printf("usage: t_dxvk_caps --direct <icd.so>\n"); return 2; }
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "dxvk-style", .apiVersion = VK_API_VERSION_1_3};
   const char *iext[] = {"VK_KHR_get_surface_capabilities2", "VK_KHR_surface"};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext};
   VkInstance inst = NULL;
   CHECK(CreateInstance(&ici, NULL, &inst) == VK_SUCCESS, "instance 1.3 + surface exts");
   if (!inst) return 1;
#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceFeatures2); I(CreateDevice); I(DestroyInstance);
   I(EnumerateDeviceExtensionProperties); I(GetPhysicalDeviceProperties2);
   uint32_t n = 1;
   VkPhysicalDevice pd;
   CHECK(EnumeratePhysicalDevices(inst, &n, &pd) == VK_SUCCESS && n == 1, "enumerate");

   VkPhysicalDeviceProperties2 pr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
   GetPhysicalDeviceProperties2(pd, &pr);
   CHECK(pr.properties.apiVersion >= VK_API_VERSION_1_3, "DXVK needs 1.3 (info.cpp:115)");

   VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
   VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f11};
   VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &f12};
   VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
   GetPhysicalDeviceFeatures2(pd, &f2);

   /* The required list, as (name, supported?). */
   const VkPhysicalDeviceFeatures *c = &f2.features;
   struct { const char *name; VkBool32 have; } req[] = {
#define C(x) {"core." #x, c->x}
      C(depthBiasClamp), C(depthClamp), C(dualSrcBlend), C(fillModeNonSolid), C(fragmentStoresAndAtomics),
      C(fullDrawIndexUint32), C(imageCubeArray), C(independentBlend), C(multiDrawIndirect), C(multiViewport),
      C(occlusionQueryPrecise), C(robustBufferAccess), C(sampleRateShading), C(samplerAnisotropy),
      C(shaderClipDistance), C(shaderCullDistance), C(shaderImageGatherExtended), C(shaderInt16), C(shaderInt64),
      C(shaderSampledImageArrayDynamicIndexing), C(textureCompressionBC), C(geometryShader),
#undef C
#define V(s, x) {#s "." #x, f##s.x}
      V(11, shaderDrawParameters), V(11, storageBuffer16BitAccess),
      V(12, bufferDeviceAddress), V(12, descriptorIndexing), V(12, shaderSampledImageArrayNonUniformIndexing),
      V(12, descriptorBindingSampledImageUpdateAfterBind), V(12, descriptorBindingUpdateUnusedWhilePending),
      V(12, descriptorBindingPartiallyBound), V(12, runtimeDescriptorArray), V(12, storageBuffer8BitAccess),
      V(12, hostQueryReset), V(12, samplerMirrorClampToEdge), V(12, scalarBlockLayout), V(12, shaderInt8),
      V(12, timelineSemaphore), V(12, uniformBufferStandardLayout), V(12, vulkanMemoryModel),
      V(13, inlineUniformBlock), V(13, computeFullSubgroups), V(13, dynamicRendering), V(13, maintenance4),
      V(13, shaderDemoteToHelperInvocation), V(13, shaderZeroInitializeWorkgroupMemory),
      V(13, subgroupSizeControl), V(13, synchronization2),
#undef V
   };
   unsigned nreq = sizeof(req) / sizeof(req[0]), missing = 0;
   for (unsigned i = 0; i < nreq; i++) {
      printf("%s %s\n", req[i].have ? "ok     " : "MISSING", req[i].name);
      missing += !req[i].have;
   }

   uint32_t ne = 0;
   EnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
   VkExtensionProperties eps[128];
   if (ne > 128) ne = 128;
   EnumerateDeviceExtensionProperties(pd, NULL, &ne, eps);
   const char *have_ext[16];
   unsigned nhave = 0, next_missing = 0;
   for (unsigned i = 0; i < sizeof(dxvk_exts) / sizeof(dxvk_exts[0]); i++) {
      int h = 0;
      for (uint32_t j = 0; j < ne; j++) h |= !strcmp(eps[j].extensionName, dxvk_exts[i]);
      printf("%s ext %s\n", h ? "ok     " : "MISSING", dxvk_exts[i]);
      if (h) have_ext[nhave++] = dxvk_exts[i]; else next_missing++;
   }
   printf("summary: %u/%u required features missing, %u/%zu DXVK extensions missing\n", missing, nreq, next_missing,
          sizeof(dxvk_exts) / sizeof(dxvk_exts[0]));

   /* The advertised set only changes together with a test and this number (R4 honesty rule). */
   CHECK(expect_missing < 0 || (int)missing == expect_missing, "required features missing: %u, expected %d", missing, expect_missing);

   /* Device create with the supported subset (what DXVK would get with its requirement list patched down). */
   VkPhysicalDeviceVulkan13Features e13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
   VkPhysicalDeviceVulkan12Features e12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &e13};
   VkPhysicalDeviceVulkan11Features e11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &e12};
   VkPhysicalDeviceFeatures2 e2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e11};
   e2.features = f2.features; e11 = f11; e11.pNext = &e12; e12 = f12; e12.pNext = &e13; e13 = f13; e13.pNext = NULL;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                  .pQueuePriorities = (float[]){1.0f}};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &e2, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qci, .enabledExtensionCount = nhave, .ppEnabledExtensionNames = have_ext};
   VkDevice dev = NULL;
   VkResult r = CreateDevice(pd, &dci, NULL, &dev);
   CHECK(r == VK_SUCCESS, "device with the supported subset: %d", r);
   if (dev) ((PFN_vkDestroyDevice)((PFN_vkGetInstanceProcAddr)gipa)(inst, "vkDestroyDevice"))(dev, NULL);

   /* Full DXVK request: must be refused while anything is missing. */
   if (missing || next_missing) {
      VkPhysicalDeviceFeatures full;
      memset(&full, 1, sizeof(full));
      VkDeviceCreateInfo d2 = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .pEnabledFeatures = &full,
                               .enabledExtensionCount = sizeof(dxvk_exts) / sizeof(dxvk_exts[0]), .ppEnabledExtensionNames = dxvk_exts};
      VkDevice d = NULL;
      r = CreateDevice(pd, &d2, NULL, &d);
      CHECK(r == VK_ERROR_FEATURE_NOT_PRESENT || r == VK_ERROR_EXTENSION_NOT_PRESENT, "full DXVK request must be refused, got %d", r);
   }
   DestroyInstance(inst, NULL);
   printf("{\"test\":\"t_dxvk_caps\",\"pass\":%s,\"missing_features\":%u,\"missing_exts\":%u}\n", g_fail ? "false" : "true", missing, next_missing);
   return g_fail;
}
