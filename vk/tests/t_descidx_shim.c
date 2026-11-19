/* SPDX-License-Identifier: MIT */
/* MALI_ADVERTISE_DESCIDX compatibility shim (src/mali_descidx_shim.h), both env states. Loader-free.
 *   t_descidx_shim --direct <icd.so> --expect on|off
 * off (default): Vulkan12Features.descriptorIndexing false, CreateDevice requesting it is rejected.
 * on  (MALI_ADVERTISE_DESCIDX=1): true, the device is created with it, and the honest sub-features are unchanged: the sampled-image set stays true,
 *     buffer/storage-image/texel/input-attachment indexing, their update-after-bind and variableDescriptorCount stay false, limits unchanged. Not conformance. */
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
   if (!icd || !expect || (strcmp(expect, "on") && strcmp(expect, "off"))) { printf("usage: t_descidx_shim --direct <icd.so> --expect on|off\n"); return 2; }
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
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceFeatures2); I(GetPhysicalDeviceProperties2); I(CreateDevice); I(GetDeviceProcAddr); I(DestroyInstance);
   uint32_t n = 1;
   VkPhysicalDevice pd;
   CHECK(EnumeratePhysicalDevices(inst, &n, &pd) == VK_SUCCESS && n == 1, "enumerate");

   VkPhysicalDeviceDescriptorIndexingFeatures di = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
   VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &di};
   VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
   GetPhysicalDeviceFeatures2(pd, &f2);
   CHECK(f12.descriptorIndexing == (on ? VK_TRUE : VK_FALSE), "vk12.descriptorIndexing %u (expect %s)", f12.descriptorIndexing, expect);
   /* the sub-features DXVK uses are advertised either way (t_vk_feat didx) */
   CHECK(f12.runtimeDescriptorArray && f12.descriptorBindingPartiallyBound && f12.descriptorBindingSampledImageUpdateAfterBind &&
         f12.descriptorBindingUpdateUnusedWhilePending && f12.shaderSampledImageArrayNonUniformIndexing, "sampled-image subset must stay on");
   CHECK(f2.features.shaderSampledImageArrayDynamicIndexing, "core sampled-image dynamic indexing must stay on");
   /* the parts of the spec minimum set that are NOT honoured must stay false even with the shim */
   CHECK(!f12.shaderUniformBufferArrayNonUniformIndexing && !f12.shaderStorageBufferArrayNonUniformIndexing &&
         !f12.shaderStorageImageArrayNonUniformIndexing && !f12.shaderUniformTexelBufferArrayNonUniformIndexing &&
         !f12.shaderStorageTexelBufferArrayNonUniformIndexing && !f12.shaderInputAttachmentArrayNonUniformIndexing, "non-uniform indexing of other types must stay off");
   CHECK(!f12.shaderInputAttachmentArrayDynamicIndexing && !f12.shaderUniformTexelBufferArrayDynamicIndexing && !f12.shaderStorageTexelBufferArrayDynamicIndexing,
         "dynamic indexing of other types must stay off");
   CHECK(!f12.descriptorBindingUniformBufferUpdateAfterBind && !f12.descriptorBindingStorageImageUpdateAfterBind &&
         !f12.descriptorBindingStorageBufferUpdateAfterBind && !f12.descriptorBindingUniformTexelBufferUpdateAfterBind &&
         !f12.descriptorBindingStorageTexelBufferUpdateAfterBind, "update-after-bind on other types must stay off");
   CHECK(!f12.descriptorBindingVariableDescriptorCount, "variableDescriptorCount must stay off (PR #81 is separate)");
   CHECK(di.runtimeDescriptorArray == f12.runtimeDescriptorArray && di.descriptorBindingPartiallyBound == f12.descriptorBindingPartiallyBound &&
         di.shaderStorageBufferArrayNonUniformIndexing == f12.shaderStorageBufferArrayNonUniformIndexing, "descriptor-indexing mirror struct disagrees with Vulkan12Features");

   VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
   VkPhysicalDeviceProperties2 pr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
   GetPhysicalDeviceProperties2(pd, &pr);
   CHECK(p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers < 500000 && p12.maxPerStageDescriptorUpdateAfterBindStorageImages < 500000,
         "limits must NOT be inflated by the shim");

   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = (float[]){1.0f}};
   VkPhysicalDeviceVulkan12Features want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .descriptorIndexing = VK_TRUE};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &want, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   VkDevice dev = NULL;
   VkResult r = CreateDevice(pd, &dci, NULL, &dev);
   if (on) {
      CHECK(r == VK_SUCCESS && dev, "CreateDevice with descriptorIndexing: %d", r);
      PFN_vkDestroyDevice DD = dev ? (PFN_vkDestroyDevice)GetDeviceProcAddr(dev, "vkDestroyDevice") : NULL;
      if (DD) DD(dev, NULL);
   } else {
      CHECK(r == VK_ERROR_FEATURE_NOT_PRESENT, "descriptorIndexing must be rejected with the shim off: %d", r);
   }
   DestroyInstance(inst, NULL);
   printf("{\"test\":\"t_descidx_shim\",\"expect\":\"%s\",\"pass\":%s}\n", expect, g_fail ? "false" : "true");
   return g_fail;
}
