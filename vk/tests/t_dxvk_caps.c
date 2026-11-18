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
#include <time.h>

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
   /* Extension feature structs: an advertised extension must report its feature and accept it at device creation. */
   VkPhysicalDeviceDepthClipEnableFeaturesEXT dce = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
   {
      VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &dce};
      GetPhysicalDeviceFeatures2(pd, &q);
      int adv = 0;
      for (unsigned i = 0; i < nhave; i++) adv |= !strcmp(have_ext[i], "VK_EXT_depth_clip_enable");
      CHECK(adv == (int)dce.depthClipEnable, "VK_EXT_depth_clip_enable advertised=%d but depthClipEnable=%u", adv, dce.depthClipEnable);
   }
   VkPhysicalDeviceMaintenance6FeaturesKHR m6 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR, .pNext = &dce};
   {
      VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &m6};
      GetPhysicalDeviceFeatures2(pd, &q);
      int adv = 0;
      for (unsigned i = 0; i < nhave; i++) adv |= !strcmp(have_ext[i], "VK_KHR_maintenance6");
      CHECK(adv == (int)m6.maintenance6, "VK_KHR_maintenance6 advertised=%d but maintenance6=%u", adv, m6.maintenance6);
   }
   {
      VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
      VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &eds2};
      GetPhysicalDeviceFeatures2(pd, &q);
      int adv = 0;
      for (uint32_t i = 0; i < ne; i++) adv |= !strcmp(eps[i].extensionName, "VK_EXT_extended_dynamic_state2");
      CHECK(adv == 1 && eds2.extendedDynamicState2 == 1, "VK_EXT_extended_dynamic_state2 advertised=%d extendedDynamicState2=%u (want 1/1)", adv, eds2.extendedDynamicState2);
      /* the driver does not honour these: logic op is unsupported() in the draw snapshot, there is no tessellation */
      CHECK(!eds2.extendedDynamicState2LogicOp && !eds2.extendedDynamicState2PatchControlPoints, "EDS2 logicOp=%u patchControlPoints=%u must stay false", eds2.extendedDynamicState2LogicOp, eds2.extendedDynamicState2PatchControlPoints);
   }
   /* VK_KHR_unified_image_layouts: name and feature bit move together, and the feature is reported through vkGetPhysicalDeviceFeatures2. */
   VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR uil = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};
   {
      VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &uil};
      GetPhysicalDeviceFeatures2(pd, &q);
      int adv = 0;
      for (uint32_t i = 0; i < ne; i++) adv |= !strcmp(eps[i].extensionName, "VK_KHR_unified_image_layouts");
      CHECK(adv == 1 && uil.unifiedImageLayouts == 1, "VK_KHR_unified_image_layouts advertised=%d unifiedImageLayouts=%u (want 1/1)", adv, uil.unifiedImageLayouts);
      CHECK(uil.unifiedImageLayoutsVideo == 0, "unifiedImageLayoutsVideo must stay false (no video)");
   }
   VkPhysicalDeviceFeatures2 e2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e11};
   e2.features = f2.features; e11 = f11; e11.pNext = &e12; e12 = f12; e12.pNext = &e13; e13 = f13; e13.pNext = &m6;   /* m6 -> dce, queried above */
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                  .pQueuePriorities = (float[]){1.0f}};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &e2, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qci, .enabledExtensionCount = nhave, .ppEnabledExtensionNames = have_ext};
   VkDevice dev = NULL;
   VkResult r = CreateDevice(pd, &dci, NULL, &dev);
   CHECK(r == VK_SUCCESS, "device with the supported subset: %d", r);
   if (dev) {
      PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)((PFN_vkGetInstanceProcAddr)gipa)(inst, "vkGetDeviceProcAddr");
      int has_budget = 0, has_cal = 0;
      for (unsigned i = 0; i < nhave; i++) {
         has_budget |= !strcmp(have_ext[i], "VK_EXT_memory_budget");
         has_cal |= !strcmp(have_ext[i], "VK_KHR_calibrated_timestamps");
      }
      if (has_budget) {
         /* budget/usage must follow vkAllocateMemory / vkFreeMemory */
         PFN_vkGetPhysicalDeviceMemoryProperties2 gmp = (PFN_vkGetPhysicalDeviceMemoryProperties2)((PFN_vkGetInstanceProcAddr)gipa)(inst, "vkGetPhysicalDeviceMemoryProperties2");
         PFN_vkAllocateMemory am = (PFN_vkAllocateMemory)gdpa(dev, "vkAllocateMemory");
         PFN_vkFreeMemory fm = (PFN_vkFreeMemory)gdpa(dev, "vkFreeMemory");
         VkPhysicalDeviceMemoryBudgetPropertiesEXT b0 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT}, b1 = b0, b2 = b0;
         VkPhysicalDeviceMemoryProperties2 m = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &b0};
         gmp(pd, &m);
         CHECK(b0.heapBudget[0] >= b0.heapUsage[0] && b0.heapBudget[0] > 0, "budget %llu usage %llu", (unsigned long long)b0.heapBudget[0], (unsigned long long)b0.heapUsage[0]);
         VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 4u << 20, .memoryTypeIndex = 0};
         VkDeviceMemory mem = VK_NULL_HANDLE;
         CHECK(am(dev, &ai, NULL, &mem) == VK_SUCCESS, "alloc 4 MiB");
         m.pNext = &b1; gmp(pd, &m);
         CHECK(b1.heapUsage[0] >= b0.heapUsage[0] + (4u << 20), "usage after alloc %llu vs %llu", (unsigned long long)b1.heapUsage[0], (unsigned long long)b0.heapUsage[0]);
         fm(dev, mem, NULL);
         m.pNext = &b2; gmp(pd, &m);
         CHECK(b2.heapUsage[0] == b0.heapUsage[0], "usage after free %llu vs %llu", (unsigned long long)b2.heapUsage[0], (unsigned long long)b0.heapUsage[0]);
      }
      if (has_cal) {
         PFN_vkGetCalibratedTimestampsKHR gct = (PFN_vkGetCalibratedTimestampsKHR)gdpa(dev, "vkGetCalibratedTimestampsKHR");
         PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR gtd = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)((PFN_vkGetInstanceProcAddr)gipa)(inst, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR");
         VkTimeDomainKHR doms[8];
         uint32_t nd = 8;
         int dev_dom = 0, raw_dom = 0;
         CHECK(gtd(pd, &nd, doms) == VK_SUCCESS, "time domains");
         for (uint32_t i = 0; i < nd; i++) dev_dom |= doms[i] == VK_TIME_DOMAIN_DEVICE_KHR, raw_dom |= doms[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR;
         CHECK(dev_dom && raw_dom, "device and CLOCK_MONOTONIC_RAW domains listed");
         VkCalibratedTimestampInfoKHR ti[2] = {{.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR},
                                               {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR}};
         uint64_t t1[2] = {0}, t2[2] = {0}, dev1 = 0, dev2 = 0;
         struct timespec ts;
         VkResult c1 = gct(dev, 2, ti, t1, &dev1);
         clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
         uint64_t host = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
         VkResult c2 = gct(dev, 2, ti, t2, &dev2);
         CHECK(c1 == VK_SUCCESS && c2 == VK_SUCCESS, "GetCalibratedTimestamps %d %d", c1, c2);
         CHECK(t1[0] != 0 && t2[0] >= t1[0], "device time not advancing: %llu -> %llu", (unsigned long long)t1[0], (unsigned long long)t2[0]);
         CHECK(t1[1] <= host && host <= t2[1], "raw domain %llu .. host %llu .. %llu", (unsigned long long)t1[1], (unsigned long long)host, (unsigned long long)t2[1]);
         CHECK(dev1 < 1000000000ull && dev2 < 1000000000ull, "max deviation %llu ns", (unsigned long long)dev1);
      }
   }
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
