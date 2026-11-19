/* SPDX-License-Identifier: MIT */
/* MALI_ADVERTISE_GS compatibility shim (src/mali_gs_shim.h), both env states. Loader-free.
 *   t_gs_shim --direct <icd.so> --expect on|off
 * off (default driver): geometryShader false, maxGeometry* 0, CreateDevice with geometryShader rejected.
 * on  (MALI_ADVERTISE_GS=1, run by check.sh): geometryShader true, maxGeometry* at the Vulkan minimums, the device is created with the feature,
 *     and vkCreateGraphicsPipelines with a GEOMETRY stage STILL returns VK_ERROR_FEATURE_NOT_PRESENT (no pipeline handle). Not conformance. */
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
   if (!icd || !expect || (strcmp(expect, "on") && strcmp(expect, "off"))) { printf("usage: t_gs_shim --direct <icd.so> --expect on|off\n"); return 2; }
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
   CHECK(f1.geometryShader == (on ? VK_TRUE : VK_FALSE), "features.geometryShader %u (expect %s)", f1.geometryShader, expect);
   CHECK(f2.features.geometryShader == f1.geometryShader, "Features2 disagrees with Features");
   CHECK(!f1.tessellationShader, "tessellationShader must stay off");

   VkPhysicalDeviceProperties pr;
   GetPhysicalDeviceProperties(pd, &pr);
   const VkPhysicalDeviceLimits *L = &pr.limits;
   if (on) {
      CHECK(L->maxGeometryShaderInvocations >= 32 && L->maxGeometryInputComponents >= 64 && L->maxGeometryOutputComponents >= 64 &&
            L->maxGeometryOutputVertices >= 256 && L->maxGeometryTotalOutputComponents >= 1024, "maxGeometry* below the Vulkan minimums");
      CHECK(L->maxGeometryShaderInvocations == 32 && L->maxGeometryInputComponents == 64 && L->maxGeometryOutputComponents == 64 &&
            L->maxGeometryOutputVertices == 256 && L->maxGeometryTotalOutputComponents == 1024, "maxGeometry* exactly the minimums");
   } else {
      CHECK(!L->maxGeometryShaderInvocations && !L->maxGeometryInputComponents && !L->maxGeometryOutputComponents &&
            L->maxGeometryOutputVertices == 0 && !L->maxGeometryTotalOutputComponents, "maxGeometry* must be 0 with the shim off");
   }

   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = (float[]){1.0f}};
   VkPhysicalDeviceFeatures want = {.geometryShader = VK_TRUE};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &want};
   VkDevice dev = NULL;
   VkResult r = CreateDevice(pd, &dci, NULL, &dev);
   if (on) {
      CHECK(r == VK_SUCCESS && dev, "CreateDevice with geometryShader: %d", r);
      PFN_vkCreateGraphicsPipelines CGP = dev ? (PFN_vkCreateGraphicsPipelines)GetDeviceProcAddr(dev, "vkCreateGraphicsPipelines") : NULL;
      PFN_vkCreateShaderModule CSM = dev ? (PFN_vkCreateShaderModule)GetDeviceProcAddr(dev, "vkCreateShaderModule") : NULL;
      CHECK(CGP && CSM, "device procs");
      if (CGP && CSM) {
         const uint32_t spv[] = {0x07230203, 0x00010000, 0, 1, 0};   /* header only: the driver must refuse the GEOMETRY stage before reading the code */
         VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof spv, .pCode = spv};
         VkShaderModule m = VK_NULL_HANDLE;
         CHECK(CSM(dev, &smi, NULL, &m) == VK_SUCCESS, "module");
         VkPipelineShaderStageCreateInfo st[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = m, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_GEOMETRY_BIT, .module = m, .pName = "main"}};
         VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st};
         VkPipeline p = (VkPipeline)(uintptr_t)1;
         VkResult pr2 = CGP(dev, VK_NULL_HANDLE, 1, &gp, NULL, &p);
         CHECK(pr2 == VK_ERROR_FEATURE_NOT_PRESENT, "GS pipeline must be rejected: %d", pr2);
         CHECK(p == VK_NULL_HANDLE, "rejected pipeline handle must be VK_NULL_HANDLE");
         PFN_vkDestroyShaderModule DSM = (PFN_vkDestroyShaderModule)GetDeviceProcAddr(dev, "vkDestroyShaderModule");
         if (DSM && m) DSM(dev, m, NULL);   /* no live child objects when the device goes below */
      }
      PFN_vkDestroyDevice DD = dev ? (PFN_vkDestroyDevice)GetDeviceProcAddr(dev, "vkDestroyDevice") : NULL;
      if (DD) DD(dev, NULL);
   } else {
      CHECK(r == VK_ERROR_FEATURE_NOT_PRESENT, "geometryShader must be rejected with the shim off: %d", r);
   }
   DestroyInstance(inst, NULL);
   printf("{\"test\":\"t_gs_shim\",\"expect\":\"%s\",\"pass\":%s}\n", expect, g_fail ? "false" : "true");
   return g_fail;
}
