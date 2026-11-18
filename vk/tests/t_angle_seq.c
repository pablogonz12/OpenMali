/* SPDX-License-Identifier: MIT */
/* The Vulkan call sequence ANGLE (Chromium 126 vk_renderer.cpp, Renderer::initialize and what runs on its first context) and zink
 * issue after vkCreateDevice, replayed on the ICD under the fake backend so ASan/UBSan (MALI_SAN=1) and the exit code catch a null
 * pNext handler, an unhandled struct type or a NULL entry point:
 *   - vkGetPhysicalDeviceFeatures2/Properties2 with every extension struct ANGLE chains (queryDeviceExtensionFeatures), plus one
 *     unknown sType in the middle of the chain (must be skipped, not touched),
 *   - vkCreateDevice with every advertised extension and that chain as pNext,
 *   - every vk.xml command of every advertised extension and every core 1.0-1.3 command is non-NULL through GIPA/GDPA (generated into
 *     procaddr_table.h by gen_procaddr.py),
 *   - vkGetPhysicalDeviceFormatProperties2 (with/without VkFormatProperties3), ImageFormatProperties2 (external memory, ycbcr output,
 *     format list, unknown handle types), External{Buffer,Semaphore,Fence}Properties for every handle type, MemoryProperties2 with the
 *     budget struct, QueueFamilyProperties2 with global priority,
 *   - vkCreatePipelineCache with real, truncated, garbage and zero-length initial data, descriptor pools, samplers (custom border
 *     colour), command pool/buffer, fence/semaphore (exportable), first image/buffer with external memory info.
 *   t_angle_seq --direct <icd.so>
 * Exit 0 = pass. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "procaddr_table.h"

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

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;

struct chain_item { VkStructureType type; size_t size; };
#define ITEM(T, ST) {ST, sizeof(T)}

/* the feature structs ANGLE chains (appendDeviceExtensionFeatures*) and the property structs it chains */
static const struct chain_item feat_items[] = {
   ITEM(VkPhysicalDeviceLineRasterizationFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT),
   ITEM(VkPhysicalDeviceProvokingVertexFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT),
   ITEM(VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT),
   ITEM(VkPhysicalDeviceTransformFeedbackFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT),
   ITEM(VkPhysicalDeviceIndexTypeUint8FeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT),
   ITEM(VkPhysicalDeviceDeviceMemoryReportFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT),
   ITEM(VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_FEATURES_EXT),
   ITEM(VkPhysicalDeviceImage2DViewOf3DFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT),
   ITEM(VkPhysicalDeviceCustomBorderColorFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT),
   ITEM(VkPhysicalDeviceDepthClampZeroOneFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_ZERO_ONE_FEATURES_EXT),
   ITEM(VkPhysicalDeviceDepthClipControlFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT),
   ITEM(VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT),
   ITEM(VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT),
   ITEM(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT),
   ITEM(VkPhysicalDeviceFragmentShadingRateFeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR),
   ITEM(VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT),
   ITEM(VkPhysicalDevicePipelineRobustnessFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT),
   ITEM(VkPhysicalDevicePipelineProtectedAccessFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROTECTED_ACCESS_FEATURES_EXT),
   ITEM(VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT),
   ITEM(VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT),
   ITEM(VkPhysicalDeviceHostImageCopyFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT),
   ITEM(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT),
   ITEM(VkPhysicalDeviceProtectedMemoryFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES),
   ITEM(VkPhysicalDeviceSamplerYcbcrConversionFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES),
   ITEM(VkPhysicalDeviceMultiviewFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES),
   ITEM(VkPhysicalDevice16BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES),
   ITEM(VkPhysicalDeviceShaderFloat16Int8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES),
   ITEM(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES),
   ITEM(VkPhysicalDeviceHostQueryResetFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES),
   ITEM(VkPhysicalDeviceImagelessFramebufferFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES),
   ITEM(VkPhysicalDeviceTimelineSemaphoreFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES),
   ITEM(VkPhysicalDevice8BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES),
   ITEM(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT),
   ITEM(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT),
   ITEM(VkPhysicalDeviceSynchronization2Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES),
   ITEM(VkPhysicalDeviceRobustness2FeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT),
   ITEM(VkPhysicalDeviceVulkan11Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES),
   ITEM(VkPhysicalDeviceVulkan12Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES),
   ITEM(VkPhysicalDeviceVulkan13Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES),
};

static const struct chain_item prop_items[] = {
   ITEM(VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT),
   ITEM(VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT),
   ITEM(VkPhysicalDeviceFragmentShadingRatePropertiesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR),
   ITEM(VkPhysicalDeviceDrmPropertiesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT),
   ITEM(VkPhysicalDeviceHostImageCopyPropertiesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES_EXT),
   ITEM(VkPhysicalDeviceSubgroupProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES),
   ITEM(VkPhysicalDeviceMultiviewProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES),
   ITEM(VkPhysicalDeviceDepthStencilResolveProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES),
   ITEM(VkPhysicalDeviceDriverProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES),
   ITEM(VkPhysicalDeviceIDProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES),
   ITEM(VkPhysicalDeviceMaintenance3Properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES),
   ITEM(VkPhysicalDevicePointClippingProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES),
   ITEM(VkPhysicalDeviceFloatControlsProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES),
   ITEM(VkPhysicalDeviceDescriptorIndexingProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES),
   ITEM(VkPhysicalDeviceTimelineSemaphoreProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES),
   ITEM(VkPhysicalDeviceRobustness2PropertiesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT),
   ITEM(VkPhysicalDeviceCustomBorderColorPropertiesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT),
   ITEM(VkPhysicalDeviceTransformFeedbackPropertiesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT),
   ITEM(VkPhysicalDeviceLineRasterizationPropertiesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_PROPERTIES_EXT),
   ITEM(VkPhysicalDeviceVulkan11Properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES),
   ITEM(VkPhysicalDeviceVulkan12Properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES),
   ITEM(VkPhysicalDeviceVulkan13Properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES),
};

/* a chain of zero-filled structs (sType set) with one unknown sType (0x7fff0000-range, never defined) after item 0 and at the tail */
struct chain { void *mem[64]; unsigned n; };
static void *chain_build(struct chain *c, const struct chain_item *it, unsigned n)
{
   VkBaseOutStructure *head = NULL, **tail = &head;
   c->n = 0;
   for (unsigned i = 0; i <= n; i++) {
      for (int pass = 0; pass < 2; pass++) {
         VkStructureType st = pass == 0 ? (i < n ? it[i].type : 0) : 0x7fff7001u;
         size_t sz = pass == 0 ? (i < n ? it[i].size : 0) : sizeof(VkBaseOutStructure) + 64;
         if (pass == 0 && i == n)
            continue;
         if (pass == 1 && i != 0 && i != n)
            continue;
         VkBaseOutStructure *s = calloc(1, sz);
         s->sType = st;
         *tail = s;
         tail = &s->pNext;
         c->mem[c->n++] = s;
      }
   }
   return head;
}
static void chain_free(struct chain *c) { for (unsigned i = 0; i < c->n; i++) free(c->mem[i]); }

static int has(const VkExtensionProperties *p, uint32_t n, const char *name)
{
   for (uint32_t i = 0; i < n; i++)
      if (!strcmp(p[i].extensionName, name))
         return 1;
   return 0;
}

#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n); CHECK(n, "vk" #n " is NULL through GIPA")
#define D(n) PFN_vk##n d_##n = (PFN_vk##n)gdpa(dev, "vk" #n); CHECK(d_##n, "vk" #n " is NULL through GDPA")

int main(int argc, char **argv)
{
   if (argc < 3 || strcmp(argv[1], "--direct")) {
      fprintf(stderr, "usage: %s --direct <icd.so>\n", argv[0]);
      return 2;
   }
   void *lib = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "dlopen: %s\n", dlerror());
      return 2;
   }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa)
      return 2;

   PFN_vkEnumerateInstanceExtensionProperties eiep = (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   CHECK(eiep && ci, "global commands");
   uint32_t nie = 0;
   eiep(NULL, &nie, NULL);
   VkExtensionProperties *ie = calloc(nie + 1, sizeof *ie);
   eiep(NULL, &nie, ie);
   const char **iuse = calloc(nie + 1, sizeof *iuse);
   for (uint32_t i = 0; i < nie; i++)
      iuse[i] = ie[i].extensionName;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pEngineName = "mesa zink", .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = nie, .ppEnabledExtensionNames = iuse};
   VkInstance inst = NULL;
   VkResult r = ci(&ici, NULL, &inst);
   CHECK(r == VK_SUCCESS, "vkCreateInstance with all %u instance extensions: %d", nie, r);
   if (!inst)
      return 1;

   I(EnumeratePhysicalDevices);
   I(GetPhysicalDeviceFeatures2);
   I(GetPhysicalDeviceProperties2);
   I(GetPhysicalDeviceFormatProperties2);
   I(GetPhysicalDeviceImageFormatProperties2);
   I(GetPhysicalDeviceExternalBufferProperties);
   I(GetPhysicalDeviceExternalSemaphoreProperties);
   I(GetPhysicalDeviceExternalFenceProperties);
   I(GetPhysicalDeviceMemoryProperties2);
   I(GetPhysicalDeviceQueueFamilyProperties2);
   I(EnumerateDeviceExtensionProperties);
   I(CreateDevice);
   I(GetDeviceProcAddr);
   I(DestroyInstance);
   gdpa = GetDeviceProcAddr;

   uint32_t np = 1;
   VkPhysicalDevice pd = NULL;
   EnumeratePhysicalDevices(inst, &np, &pd);
   CHECK(np >= 1 && pd, "no physical device");
   if (!pd)
      return 1;

   uint32_t ne = 0;
   EnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
   VkExtensionProperties *eps = calloc(ne + 1, sizeof *eps);
   EnumerateDeviceExtensionProperties(pd, NULL, &ne, eps);
   const char **use = calloc(ne + 1, sizeof *use);
   for (uint32_t i = 0; i < ne; i++)
      use[i] = eps[i].extensionName;

   /* 1. features/properties with ANGLE's chains (plus unknown sTypes) */
   struct chain fc, pc;
   VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = chain_build(&fc, feat_items, sizeof feat_items / sizeof feat_items[0])};
   VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = chain_build(&pc, prop_items, sizeof prop_items / sizeof prop_items[0])};
   GetPhysicalDeviceFeatures2(pd, &f2);
   GetPhysicalDeviceProperties2(pd, &p2);
   CHECK(p2.properties.apiVersion >= VK_API_VERSION_1_1, "api version");
   /* each struct of the chain must still carry its own sType and a pNext that leads to the next one (nothing overwritten) */
   {
      unsigned k = 0;
      for (VkBaseOutStructure *s = f2.pNext; s; s = s->pNext, k++)
         CHECK(k < fc.n && (void *)s == fc.mem[k], "features chain relinked at %u", k);
      CHECK(k == fc.n, "features chain length %u != %u", k, fc.n);
      k = 0;
      for (VkBaseOutStructure *s = p2.pNext; s; s = s->pNext, k++)
         CHECK(k < pc.n && (void *)s == pc.mem[k], "properties chain relinked at %u", k);
      CHECK(k == pc.n, "properties chain length %u != %u", k, pc.n);
   }

   /* 2. the device: every advertised extension, the features chain as pNext (what ANGLE enables is a subset of what it was told) */
   float prio = 0.0f;
   uint32_t nq = 0;
   GetPhysicalDeviceQueueFamilyProperties2(pd, &nq, NULL);
   VkQueueFamilyGlobalPriorityPropertiesKHR gp = {.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES_KHR};
   VkQueueFamilyProperties2 *qf = calloc(nq + 1, sizeof *qf);
   for (uint32_t i = 0; i < nq; i++) {
      qf[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
      qf[i].pNext = i == 0 ? (void *)&gp : NULL;
   }
   GetPhysicalDeviceQueueFamilyProperties2(pd, &nq, qf);
   uint32_t qfi = 0;
   for (uint32_t i = 0; i < nq; i++)
      if (qf[i].queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
   VkDeviceQueueCreateInfo dq = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq,
                             .enabledExtensionCount = ne, .ppEnabledExtensionNames = use};
   VkDevice dev = NULL;
   r = CreateDevice(pd, &dci, NULL, &dev);
   printf("vkCreateDevice with %u extensions and %u chained feature structs: %d\n", ne, fc.n, r);
   CHECK(r == VK_SUCCESS, "vkCreateDevice: %d", r);
   if (!dev)
      return 1;

   /* 3. procaddr: every command of every advertised extension, and the core ones */
   {
      unsigned checked = 0, missing = 0;
      for (unsigned i = 0; i < sizeof procaddr_exts / sizeof procaddr_exts[0]; i++) {
         const struct pa_ext *e = &procaddr_exts[i];
         int adv = e->core ? 1 : (e->instance ? 0 : has(eps, ne, e->name));
         if (e->instance && !e->core)
            for (uint32_t k = 0; k < nie; k++)
               adv |= !strcmp(ie[k].extensionName, e->name);
         if (!adv)
            continue;
         for (unsigned c = 0; c < e->ncmds; c++) {
            const struct pa_cmd *cm = &e->cmds[c];
            PFN_vkVoidFunction f = cm->device ? gdpa(dev, cm->name) : gipa(inst, cm->name);
            if (!f && cm->device)
               f = gipa(inst, cm->name);   /* the loader's trampolines may serve device commands through the instance */
            checked++;
            if (!f) {
               missing++;
               printf("MISSING %s %s (%s)\n", cm->device ? "device" : "instance", cm->name, e->name);
            }
         }
      }
      printf("procaddr: %u commands of advertised extensions + core checked, %u NULL\n", checked, missing);
      CHECK(missing == 0, "%u commands of advertised extensions resolve to NULL (a caller crashes jumping to 0)", missing);
   }

   /* 4. ANGLE's first-context queries */
   {
      VkFormat fmts[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                         VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM,
                         VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
                         VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM, VK_FORMAT_R5G6B5_UNORM_PACK16,
                         VK_FORMAT_UNDEFINED, VK_FORMAT_R8_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
      for (unsigned i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
         VkFormatProperties3 fp3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
         VkDrmFormatModifierPropertiesListEXT ml = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT, .pNext = &fp3};
         VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &ml};
         GetPhysicalDeviceFormatProperties2(pd, fmts[i], &fp);
         uint32_t nm = ml.drmFormatModifierCount;
         VkDrmFormatModifierPropertiesEXT *mods = calloc(nm + 1, sizeof *mods);
         ml.pDrmFormatModifierProperties = mods;
         GetPhysicalDeviceFormatProperties2(pd, fmts[i], &fp);
         free(mods);
         VkFormatProperties2 fp_bare = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
         GetPhysicalDeviceFormatProperties2(pd, fmts[i], &fp_bare);
      }

      VkExternalMemoryHandleTypeFlagBits hts[] = {VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                                  VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
                                                  VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, 0, (VkExternalMemoryHandleTypeFlagBits)0x40000000};
      for (unsigned i = 0; i < sizeof hts / sizeof hts[0]; i++) {
         for (int tiling = 0; tiling < 2; tiling++) {
            VkImageFormatListCreateInfo fl = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2,
                                              .pViewFormats = (VkFormat[]){VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}};
            VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
                                                                .pNext = &fl, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
            VkPhysicalDeviceExternalImageFormatInfo ei = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
                                                          .pNext = tiling ? (void *)&mi : (void *)&fl, .handleType = hts[i]};
            VkPhysicalDeviceImageFormatInfo2 fi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = i == 4 ? NULL : &ei,
                                                   .format = VK_FORMAT_R8G8B8A8_UNORM, .type = VK_IMAGE_TYPE_2D,
                                                   .tiling = tiling ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_OPTIMAL,
                                                   .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkSamplerYcbcrConversionImageFormatProperties yp = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES};
            VkExternalImageFormatProperties eo = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES, .pNext = &yp};
            VkImageFormatProperties2 fo = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &eo};
            VkResult rr = GetPhysicalDeviceImageFormatProperties2(pd, &fi, &fo);
            CHECK(rr == VK_SUCCESS || rr == VK_ERROR_FORMAT_NOT_SUPPORTED, "ImageFormatProperties2 handle 0x%x tiling %d: %d", hts[i], tiling, rr);
            VkImageFormatProperties2 fo2 = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
            rr = GetPhysicalDeviceImageFormatProperties2(pd, &fi, &fo2);
            CHECK(rr == VK_SUCCESS || rr == VK_ERROR_FORMAT_NOT_SUPPORTED, "ImageFormatProperties2 bare output: %d", rr);
         }
         VkPhysicalDeviceExternalBufferInfo bi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .handleType = hts[i]};
         VkExternalBufferProperties bo = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
         GetPhysicalDeviceExternalBufferProperties(pd, &bi, &bo);
         VkPhysicalDeviceExternalSemaphoreInfo si = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, .handleType = (VkExternalSemaphoreHandleTypeFlagBits)hts[i]};
         VkExternalSemaphoreProperties so = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
         GetPhysicalDeviceExternalSemaphoreProperties(pd, &si, &so);
         VkPhysicalDeviceExternalFenceInfo fi2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO, .handleType = (VkExternalFenceHandleTypeFlagBits)hts[i]};
         VkExternalFenceProperties fo3 = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
         GetPhysicalDeviceExternalFenceProperties(pd, &fi2, &fo3);
      }

      VkPhysicalDeviceMemoryBudgetPropertiesEXT mb = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
      VkPhysicalDeviceMemoryProperties2 mp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &mb};
      GetPhysicalDeviceMemoryProperties2(pd, &mp);
      CHECK(mp.memoryProperties.memoryHeapCount > 0, "no memory heap");
      if (has(eps, ne, "VK_EXT_memory_budget"))
         CHECK(mb.heapBudget[0] > 0, "memory budget struct left untouched with the extension advertised");
   }

   /* 5. objects ANGLE creates before its first draw */
   {
      D(GetDeviceQueue); D(CreatePipelineCache); D(GetPipelineCacheData); D(DestroyPipelineCache); D(MergePipelineCaches);
      D(CreateDescriptorPool); D(DestroyDescriptorPool); D(CreateSampler); D(DestroySampler); D(CreateCommandPool); D(DestroyCommandPool);
      D(AllocateCommandBuffers); D(CreateFence); D(DestroyFence); D(CreateSemaphore); D(DestroySemaphore); D(CreateBuffer); D(DestroyBuffer);
      D(GetBufferMemoryRequirements2); D(CreateImage); D(DestroyImage); D(GetImageMemoryRequirements2); D(DestroyDevice);
      D(CreateDescriptorSetLayout); D(DestroyDescriptorSetLayout); D(CreatePipelineLayout); D(DestroyPipelineLayout);
      D(CreateRenderPass2); D(DestroyRenderPass); D(CreateShaderModule); D(DestroyShaderModule); D(DeviceWaitIdle); D(CreateQueryPool); D(DestroyQueryPool);
      D(GetSemaphoreCounterValue); D(ResetQueryPool);
      VkQueue q = NULL;
      d_GetDeviceQueue(dev, qfi, 0, &q);
      CHECK(q, "queue");

      /* pipeline cache: empty, real (round trip), truncated header, garbage, a header of another device */
      VkPipelineCache pcache = NULL, pc2 = NULL;
      VkPipelineCacheCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
      CHECK(d_CreatePipelineCache(dev, &pci, NULL, &pcache) == VK_SUCCESS, "empty pipeline cache");
      size_t sz = 0;
      CHECK(d_GetPipelineCacheData(dev, pcache, &sz, NULL) == VK_SUCCESS, "pipeline cache size");
      unsigned char *blob = calloc(1, sz + 256);
      CHECK(d_GetPipelineCacheData(dev, pcache, &sz, blob) == VK_SUCCESS, "pipeline cache data");
      pci.initialDataSize = sz;
      pci.pInitialData = blob;
      CHECK(d_CreatePipelineCache(dev, &pci, NULL, &pc2) == VK_SUCCESS, "pipeline cache from its own data (%zu bytes)", sz);
      if (pc2) {
         VkPipelineCache m[] = {pc2};
         d_MergePipelineCaches(dev, pcache, 1, m);
         d_DestroyPipelineCache(dev, pc2, NULL);
      }
      static const size_t lens[] = {1, 4, 15, 16, 31, 32, 33, 48, 255};
      for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++) {
         unsigned char junk[256];
         for (unsigned k = 0; k < sizeof junk; k++)
            junk[k] = (unsigned char)(k * 37 + i);
         for (int hdr = 0; hdr < 2; hdr++) {
            if (hdr && sz >= 16)
               memcpy(junk, blob, 16 < sizeof junk ? 16 : sizeof junk);   /* a plausible header followed by junk */
            VkPipelineCacheCreateInfo pj = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = lens[i], .pInitialData = junk};
            VkPipelineCache pk = NULL;
            VkResult rr = d_CreatePipelineCache(dev, &pj, NULL, &pk);   /* any result is legal, a crash is not; a created cache must be usable */
            CHECK(rr == VK_SUCCESS || rr < 0, "pipeline cache from %zu junk bytes: %d", lens[i], rr);
            if (pk) {
               size_t s2 = 0;
               d_GetPipelineCacheData(dev, pk, &s2, NULL);
               d_DestroyPipelineCache(dev, pk, NULL);
            }
         }
      }
      free(blob);
      d_DestroyPipelineCache(dev, pcache, NULL);

      /* descriptor pools: ANGLE's kMaxDescriptorSets pools of every type, then the update-after-bind variant and a free-descriptor-set pool */
      VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16},
                                   {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16}, {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 4}};
      for (int flags = 0; flags < 3; flags++) {
         VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 128,
                                           .flags = flags == 1 ? VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT : flags == 2 ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0,
                                           .poolSizeCount = sizeof ps / sizeof ps[0], .pPoolSizes = ps};
         VkDescriptorPool dp = NULL;
         VkResult rr = d_CreateDescriptorPool(dev, &dpi, NULL, &dp);
         CHECK(rr == VK_SUCCESS, "descriptor pool flags %d: %d", flags, rr);
         if (dp)
            d_DestroyDescriptorPool(dev, dp, NULL);
      }

      /* samplers: plain, anisotropic, compare, custom border colour (with and without the format) */
      for (int v = 0; v < 4; v++) {
         VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                                                       .customBorderColor = {.float32 = {0.25f, 0.5f, 0.75f, 1.0f}}, .format = v == 3 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED};
         VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = v >= 2 ? (void *)&cb : NULL,
                                    .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE,
                                    .anisotropyEnable = v == 1, .maxAnisotropy = v == 1 ? 4.0f : 1.0f, .compareEnable = v == 0, .compareOp = VK_COMPARE_OP_LESS,
                                    .borderColor = v >= 2 ? VK_BORDER_COLOR_FLOAT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK};
         VkSampler s = NULL;
         VkResult rr = d_CreateSampler(dev, &sci, NULL, &s);
         CHECK(rr == VK_SUCCESS || rr == VK_ERROR_FEATURE_NOT_PRESENT || rr == VK_ERROR_OUT_OF_HOST_MEMORY || rr < 0, "sampler variant %d: %d", v, rr);
         if (s)
            d_DestroySampler(dev, s, NULL);
      }

      /* command pool + buffers, exportable fence/semaphore, timeline semaphore, first external buffer and image */
      VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfi};
      VkCommandPool cp = NULL;
      CHECK(d_CreateCommandPool(dev, &cpi, NULL, &cp) == VK_SUCCESS, "command pool");
      VkCommandBuffer cbs[4] = {0};
      VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 4};
      CHECK(d_AllocateCommandBuffers(dev, &cai, cbs) == VK_SUCCESS, "command buffers");

      VkExportFenceCreateInfo ef = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO, .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
      VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = &ef};
      VkFence fence = NULL;
      VkResult rr = d_CreateFence(dev, &fci, NULL, &fence);
      CHECK(rr == VK_SUCCESS || rr == VK_ERROR_INVALID_EXTERNAL_HANDLE || rr < 0, "exportable fence: %d", rr);
      if (fence)
         d_DestroyFence(dev, fence, NULL);
      VkSemaphoreTypeCreateInfo tsi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 3};
      VkExportSemaphoreCreateInfo es = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .pNext = &tsi, .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
      VkSemaphoreCreateInfo sci2 = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &es};
      VkSemaphore sem = NULL;
      rr = d_CreateSemaphore(dev, &sci2, NULL, &sem);
      CHECK(rr == VK_SUCCESS || rr == VK_ERROR_INVALID_EXTERNAL_HANDLE || rr < 0, "exportable timeline semaphore: %d", rr);
      if (sem) {
         uint64_t v = 0;
         if (rr == VK_SUCCESS && d_GetSemaphoreCounterValue(dev, sem, &v) == VK_SUCCESS)
            CHECK(v == 3, "timeline initial value %llu", (unsigned long long)v);
         d_DestroySemaphore(dev, sem, NULL);
      }

      VkExternalMemoryBufferCreateInfo emb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
      VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &emb, .size = 65536, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
      VkBuffer buf = NULL;
      rr = d_CreateBuffer(dev, &bci, NULL, &buf);
      CHECK(rr == VK_SUCCESS || rr < 0, "external buffer: %d", rr);
      if (buf) {
         VkBufferMemoryRequirementsInfo2 bri = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = buf};
         VkMemoryDedicatedRequirements dr = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
         VkMemoryRequirements2 mr = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr};
         d_GetBufferMemoryRequirements2(dev, &bri, &mr);
         CHECK(mr.memoryRequirements.size >= 65536, "buffer requirements size %llu", (unsigned long long)mr.memoryRequirements.size);
         d_DestroyBuffer(dev, buf, NULL);
      }
      VkExternalMemoryImageCreateInfo emi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
      VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                              .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
      VkImage img = NULL;
      rr = d_CreateImage(dev, &ii, NULL, &img);
      CHECK(rr == VK_SUCCESS || rr < 0, "external image: %d", rr);
      if (img) {
         VkImageMemoryRequirementsInfo2 iri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = img};
         VkMemoryDedicatedRequirements dr = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
         VkMemoryRequirements2 mr = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr};
         d_GetImageMemoryRequirements2(dev, &iri, &mr);
         d_DestroyImage(dev, img, NULL);
      }
      d_DestroyCommandPool(dev, cp, NULL);
      d_DeviceWaitIdle(dev);
      d_DestroyDevice(dev, NULL);
   }

   chain_free(&fc);
   chain_free(&pc);
   DestroyInstance(inst, NULL);
   free(ie);
   free(iuse);
   free(eps);
   free(use);
   free(qf);
   printf("%s\n", g_fail ? "FAILED t_angle_seq" : "OK t_angle_seq");
   return g_fail;
}
