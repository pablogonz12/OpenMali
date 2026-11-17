/* SPDX-License-Identifier: MIT */
#include "mali_private.h"
#include "mali_queue.h"
#include "mali_format.h"

#include <stdio.h>
#include <string.h>

#include "mali_entrypoints.h"
#include "util/list.h"
#include "vk_alloc.h"
#include "vk_util.h"

/* M1 policy: every limit is the Vulkan 1.0 required minimum (spec table "Required Limits").
 * The milestone that implements a feature raises its limits to what the hardware does. */
static void
mali_get_properties(const struct mali_gpu_info *gpu, struct vk_properties *p)
{
   const VkSampleCountFlags samples_1_4 = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;

   *p = (struct vk_properties){
      .apiVersion = MALI_DEVICE_API_VERSION,
      .driverVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
      .vendorID = MALI_VENDOR_ID_ARM,
      .deviceID = gpu->gpu_id >> 16,
      .deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
      /* pipelineCacheUUID: zero until the shader cache exists (M6, from the build-id). */

      .maxImageDimension1D = 4096,
      .maxImageDimension2D = 4096,
      .maxImageDimension3D = 256,
      .maxImageDimensionCube = 4096,
      .maxImageArrayLayers = 256,
      .maxTexelBufferElements = 65536,
      .maxUniformBufferRange = 16384,
      .maxStorageBufferRange = 1u << 27,
      .maxPushConstantsSize = 128,
      .maxMemoryAllocationCount = 4096,
      .maxSamplerAllocationCount = 4000,
      .bufferImageGranularity = 1,
      .maxBoundDescriptorSets = 4,
      .maxPerStageDescriptorSamplers = 16,
      .maxPerStageDescriptorUniformBuffers = 12,
      .maxPerStageDescriptorStorageBuffers = 4,
      .maxPerStageDescriptorSampledImages = 16,
      .maxPerStageDescriptorStorageImages = 4,
      .maxPerStageDescriptorInputAttachments = 4,
      .maxPerStageResources = 128,
      .maxDescriptorSetSamplers = 96,
      .maxDescriptorSetUniformBuffers = 72,
      .maxDescriptorSetUniformBuffersDynamic = 8,
      .maxDescriptorSetStorageBuffers = 24,
      .maxDescriptorSetStorageBuffersDynamic = 4,
      .maxDescriptorSetSampledImages = 96,
      .maxDescriptorSetStorageImages = 24,
      .maxDescriptorSetInputAttachments = 4,
      .maxVertexInputAttributes = 16,
      .maxVertexInputBindings = 16,
      .maxVertexInputAttributeOffset = 2047,
      .maxVertexInputBindingStride = 2048,
      .maxVertexOutputComponents = 64,
      .maxFragmentInputComponents = 64,
      .maxFragmentOutputAttachments = 4,
      .maxFragmentCombinedOutputResources = 4,
      .maxComputeSharedMemorySize = 16384,
      .maxComputeWorkGroupCount = {65535, 65535, 65535},
      .maxComputeWorkGroupInvocations = 128,
      .maxComputeWorkGroupSize = {128, 128, 64},
      .subPixelPrecisionBits = 4,
      .subTexelPrecisionBits = 4,
      .mipmapPrecisionBits = 4,
      .maxDrawIndexedIndexValue = 0xffffffffu,
      .maxDrawIndirectCount = 1,
      .maxSamplerLodBias = 2.0f,
      .maxSamplerAnisotropy = 16.0f,
      .maxViewports = 1,
      .maxViewportDimensions = {4096, 4096},
      .viewportBoundsRange = {-8192.0f, 8191.0f},
      .minMemoryMapAlignment = 64,
      .minTexelBufferOffsetAlignment = 256,
      .minUniformBufferOffsetAlignment = 256,
      .minStorageBufferOffsetAlignment = 256,
      .minTexelOffset = -8,
      .maxTexelOffset = 7,
      .maxFramebufferWidth = 4096,
      .maxFramebufferHeight = 4096,
      .maxFramebufferLayers = 256,
      .framebufferColorSampleCounts = samples_1_4,
      .framebufferDepthSampleCounts = samples_1_4,
      .framebufferStencilSampleCounts = samples_1_4,
      .framebufferNoAttachmentsSampleCounts = samples_1_4,
      .maxColorAttachments = 4,
      .sampledImageColorSampleCounts = samples_1_4,
      .sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .sampledImageDepthSampleCounts = samples_1_4,
      .sampledImageStencilSampleCounts = samples_1_4,
      .storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .maxSampleMaskWords = 1,
      .timestampPeriod = 1.0f,
      .discreteQueuePriorities = 2,
      .pointSizeRange = {1.0f, 1.0f},
      .lineWidthRange = {1.0f, 8.0f},
      .lineWidthGranularity = 0.125f,
      .standardSampleLocations = true,
      .optimalBufferCopyOffsetAlignment = 1,
      .optimalBufferCopyRowPitchAlignment = 1,
      .nonCoherentAtomSize = 64,
      .maxTimelineSemaphoreValueDifference = UINT64_MAX,
      /* descriptor indexing: only sampled images and samplers have update-after-bind (t_vk_feat didx); everything else reports 0 */
      .maxUpdateAfterBindDescriptorsInAllPools = 1024, .shaderSampledImageArrayNonUniformIndexingNative = false,
      .quadDivergentImplicitLod = false,
      .maxPerStageDescriptorUpdateAfterBindSamplers = 16, .maxPerStageDescriptorUpdateAfterBindSampledImages = 16,
      .maxPerStageUpdateAfterBindResources = 32, .maxDescriptorSetUpdateAfterBindSamplers = 96,
      .maxDescriptorSetUpdateAfterBindSampledImages = 96,
   };

   snprintf(p->deviceName, sizeof(p->deviceName), "%s (mali-own%s)", gpu->name,
            gpu->fake ? ", fake backend" : "");

   /* ---- Vulkan 1.1 ---- R4-M1: only what is true. No subgroup operation or stage is advertised yet
    * (R4-M4); the warp is 16 lanes on Valhall/5th gen (PanVK oracle on the same GPU reports 16). */
   memcpy(p->driverUUID, "mali-own-vk\0\0\0\0\0", VK_UUID_SIZE);
   memcpy(p->deviceUUID, &gpu->gpu_id, sizeof(gpu->gpu_id));
   p->subgroupSize = 16;
   /* subgroup ops: same set as PanVK on this compiler (bifrost_nir.c lowers the rest); not in vertex shaders (PanVK: no helper-lane guarantees) */
   p->subgroupSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT;   /* fragment stage only after a pixel test */
   p->subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                    VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT |
                                    VK_SUBGROUP_FEATURE_CLUSTERED_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;   /* t_vk_feat subgroup */
   p->subgroupQuadOperationsInAllStages = false;
   p->pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
   p->maxPerSetDescriptors = 1024;
   p->maxMemoryAllocationSize = 1u << 30;

   /* ---- Vulkan 1.2 ---- driverID has no Mali-open value; PanVK is the nearest sibling (same compiler). */
   p->driverID = VK_DRIVER_ID_MESA_PANVK;
   snprintf(p->driverName, sizeof(p->driverName), "mali-own");
   snprintf(p->driverInfo, sizeof(p->driverInfo), "open Mali driver over kbase (R4-M1, not conformant)");
   /* conformanceVersion stays 0.0.0.0: we do not claim conformance. */
   p->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
   p->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
   p->framebufferIntegerColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;

   /* ---- Vulkan 1.3 ---- */
   p->minSubgroupSize = 16;
   p->maxSubgroupSize = 16;
   p->requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT;   /* min == max == 16: the only size; t_vk_feat sgctl */
   p->maxComputeWorkgroupSubgroups = p->maxComputeWorkGroupInvocations / 16;
   p->storageTexelBufferOffsetAlignmentBytes = p->minTexelBufferOffsetAlignment;
   p->uniformTexelBufferOffsetAlignmentBytes = p->minTexelBufferOffsetAlignment;
   p->maxBufferSize = 1u << 30;
}

static VkResult
mali_physical_device_init(struct mali_physical_device *pdev, struct mali_instance *instance,
                          const struct mali_backend_ops *backend, const struct mali_gpu_info *gpu)
{
   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table,
                                                      &mali_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table, &wsi_physical_device_entrypoints, false);

   /* No device extensions and no optional features yet. */
   const struct vk_device_extension_table extensions = {
      .KHR_timeline_semaphore = true,
      .KHR_descriptor_update_template = true, /* M7: templates (core only from 1.1) */
      .KHR_external_memory = true,
      .KHR_external_memory_fd = true,
      .EXT_external_memory_dma_buf = true,
      .KHR_external_semaphore = true,
      .KHR_external_semaphore_fd = true,
      .KHR_external_fence = true,
      .KHR_external_fence_fd = true,
      .KHR_swapchain = true,                 /* M14 */
      .KHR_swapchain_mutable_format = true,
      .EXT_image_drm_format_modifier = true, /* LINEAR only */
      .EXT_robustness2 = true,               /* nullDescriptor only (t_vk_feat nulld) */
   };
   /* The ONE feature table (R4-PLAN rule). A bit is true only if tests/t_vk_info.c probes it and passes;
    * dynamicRendering is probed by t_vk_draw/t_vk_meta (BeginRendering draws). Everything else stays false. */
   const struct vk_features features = {
      .timelineSemaphore = true,
      .textureCompressionBC = true,
      .hostQueryReset = true,
      .synchronization2 = true,
      .dynamicRendering = true,
      .maintenance4 = true,
      .privateData = true,
      .bufferDeviceAddress = true, /* t_vk_feat bda */
      .shaderSampledImageArrayDynamicIndexing = true, .runtimeDescriptorArray = true,    /* t_vk_feat didx */
      .shaderSampledImageArrayNonUniformIndexing = true, .descriptorBindingPartiallyBound = true,
      .descriptorBindingSampledImageUpdateAfterBind = true, .descriptorBindingUpdateUnusedWhilePending = true,
      .multiDrawIndirect = true,
      .depthClamp = true,          /* t_vk_draw fx_depth_clamp */
      .drawIndirectFirstInstance = true, .fullDrawIndexUint32 = true, .samplerAnisotropy = true, .wideLines = true, .shaderStorageImageExtendedFormats = true, .samplerMirrorClampToEdge = true,
      .imageCubeArray = true,      /* t_vk_feat (all five) */
      .scalarBlockLayout = true, .uniformBufferStandardLayout = true,   /* t_vk_feat scalar / ubostd */
      .shaderZeroInitializeWorkgroupMemory = true,                      /* t_vk_feat zinit */
      .shaderDrawParameters = true,                                     /* t_vk_feat dparams */
      .shaderClipDistance = true,                                       /* t_vk_feat clip */
      .shaderInt64 = true,                                              /* t_vk_feat int64 */
      .shaderInt16 = true, .storageBuffer16BitAccess = true,            /* t_vk_feat int16 */
      .shaderInt8 = true, .storageBuffer8BitAccess = true,              /* t_vk_feat int8 */
      .subgroupSizeControl = true, .computeFullSubgroups = true,        /* t_vk_feat sgctl */
      .shaderDemoteToHelperInvocation = true,                           /* t_vk_feat demote */
      .nullDescriptor = true,      /* t_vk_feat nulld */
      .depthBiasClamp = true,      /* t_vk_draw fx_bias_*_clamp */
      .occlusionQueryPrecise = true, /* t_vk_draw occlusion: exact covered-pixel counts with the PRECISE bit */
      .independentBlend = true,    /* t_vk_draw fx_blend_* (colour attachment 0; the driver renders one colour attachment) */
   };
   struct vk_properties properties;
   mali_get_properties(gpu, &properties);

   VkResult result = vk_physical_device_init(&pdev->vk, &instance->vk, &extensions, &features,
                                             &properties, &dispatch_table);
   if (result != VK_SUCCESS)
      return result;

   pdev->backend = backend;
   pdev->gpu = *gpu;
   mali_sync_init_physical(pdev);

   /* One heap. Type 0 is the mandatory HOST_VISIBLE|HOST_COHERENT type; type 1 is CPU-cached for
    * readback. Which kbase allocation flags back them is decided in M3 on hardware. */
   pdev->memory = (VkPhysicalDeviceMemoryProperties){
      .memoryTypeCount = 2,
      .memoryTypes = {
         {.propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
          .heapIndex = 0},
         {.propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
          .heapIndex = 0},
      },
      .memoryHeapCount = 1,
      .memoryHeaps = {{.size = gpu->heap_bytes, .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT}},
   };

   result = mali_wsi_init(pdev);
   if (result != VK_SUCCESS) {
      vk_physical_device_finish(&pdev->vk);
      return result;
   }

   return VK_SUCCESS;
}

VkResult
mali_enumerate_physical_devices(struct vk_instance *vk_instance)
{
   struct mali_instance *instance = container_of(vk_instance, struct mali_instance, vk);
   const struct mali_backend_ops *backend = mali_backend_select();
   struct mali_gpu_info gpu;

   /* "No compatible device" is VK_SUCCESS with an empty list: the loader fails the whole
    * enumeration if any ICD returns an error. */
   if (backend == NULL || backend->probe(&gpu) != 0)
      return VK_SUCCESS;

   if (gpu.arch_major != 11 && gpu.arch_major != 13) {
      vk_logw(VK_LOG_OBJS(vk_instance), "mali: unsupported GPU arch %u.%u (id 0x%08x)",
              gpu.arch_major, gpu.arch_minor, gpu.gpu_id);
      return VK_SUCCESS;
   }

   struct mali_physical_device *pdev =
      vk_zalloc(&instance->vk.alloc, sizeof(*pdev), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!pdev)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = mali_physical_device_init(pdev, instance, backend, &gpu);
   if (result != VK_SUCCESS) {
      vk_free(&instance->vk.alloc, pdev);
      return result;
   }

   list_addtail(&pdev->vk.link, &instance->vk.physical_devices.list);
   return VK_SUCCESS;
}

void
mali_destroy_physical_device(struct vk_physical_device *vk_pdev)
{
   struct mali_physical_device *pdev = container_of(vk_pdev, struct mali_physical_device, vk);
   struct vk_instance *instance = vk_pdev->instance;

   mali_wsi_finish(pdev);
   vk_physical_device_finish(&pdev->vk);
   vk_free(&instance->alloc, pdev);
}

VKAPI_ATTR void VKAPI_CALL
mali_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                             uint32_t *pQueueFamilyPropertyCount,
                                             VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, pQueueFamilyProperties,
                          pQueueFamilyPropertyCount);

   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p)
   {
      p->queueFamilyProperties = (VkQueueFamilyProperties){
         .queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 1,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1},
      };
   }
}

VKAPI_ATTR void VKAPI_CALL
mali_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                        VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VK_FROM_HANDLE(mali_physical_device, pdev, physicalDevice);

   pMemoryProperties->memoryProperties = pdev->memory;
}

/* Format, image-format and sparse-image-format properties: mali_format.c (M5). */

VKAPI_ATTR void VKAPI_CALL
mali_GetPhysicalDeviceExternalBufferProperties(
   VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
   VkExternalBufferProperties *pExternalBufferProperties)
{
   /* dma-buf only (M4). */
   VkExternalMemoryProperties p = {0};
   if (pExternalBufferInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
      p = (VkExternalMemoryProperties){
         .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                                   VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
         .exportFromImportedHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
         .compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
   pExternalBufferProperties->externalMemoryProperties = p;
}
