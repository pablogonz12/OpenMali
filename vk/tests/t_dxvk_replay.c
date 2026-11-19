/* SPDX-License-Identifier: MIT */
/* Host replay of a D3D11-over-DXVK game (Unity D3D11 frame, DXVK 2.x master) on the ICD under the fake backend, run with ASan+UBSan
 * (MALI_SAN=1) and a watchdog, so a driver crash / hang / leak shows up before the game ever runs on a phone:
 *   - bring-up: instance 1.3, vkCreateDevice with every advertised extension and DXVK's feature chain (Features2 + Vulkan11/12/13 +
 *     robustness2, custom border colour, vertex attribute divisor, line rasterization, ... whatever is advertised, as pNext),
 *   - vkCreatePipelineCache (+ vkGetPipelineCacheData, MergePipelineCaches, re-create from the blob),
 *   - 48 graphics pipelines (dynamic rendering, specialization constants, dynamic viewport/scissor/depth bias/stencil ref/blend consts/
 *     line width + the core 1.3 extended dynamic state set, vertex input with divisor-free per-vertex/per-instance bindings), compute,
 *   - descriptor sets (dynamic UBO + combined sampler, descriptor pool reset per frame, push descriptors when advertised),
 *   - timeline semaphores (QueueSubmit with VkTimelineSemaphoreSubmitInfo and QueueSubmit2, vkWaitSemaphores, GetSemaphoreCounterValue,
 *     host vkSignalSemaphore), fences,
 *   - occlusion + timestamp queries (CmdResetQueryPool, Begin/EndQuery, WriteTimestamp2, CmdCopyQueryPoolResults, GetQueryPoolResults,
 *     host ResetQueryPool),
 *   - CmdBlitImage / CmdResolveImage / CmdCopyBufferToImage / mip chain generation, CmdFillBuffer/UpdateBuffer, compute dispatch,
 *   - NFRAMES frames (default 300) of the above with per-frame resource churn.
 *   t_dxvk_replay --direct <icd.so> [--frames N] [--msaa-gate]
 *   --msaa-gate: the run has MALI_EXPERIMENTAL_MSAA=1, so every 4x pipeline must build and 4x frames + resolves must be recorded;
 *   without it the known 4x pipeline rejection is tolerated and noted.
 * Exit 0 = pass, 1 = a call failed, 3 = watchdog (hang). */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, ...)                                                      \
   do {                                                                    \
      if (!(c)) {                                                          \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);               \
         printf(__VA_ARGS__);                                              \
         printf("\n");                                                     \
         g_fail = 1;                                                       \
      }                                                                    \
   } while (0)
#define OK(call) do { VkResult r_ = (call); CHECK(r_ == VK_SUCCESS, "%s -> %d", #call, r_); } while (0)

static const uint32_t spv_vs[] = {
#include "t_dxvk_replay_vs_vert.inc"
};
static const uint32_t spv_fs[] = {
#include "t_dxvk_replay_fs_frag.inc"
};
static const uint32_t spv_cs[] = {
#include "t_dxvk_replay_cs_comp.inc"
};

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;

#define IFNS(X)                                                                                                    \
   X(EnumeratePhysicalDevices) X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceProperties2)                      \
   X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(CreateDevice)                \
   X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceFormatProperties) X(GetDeviceProcAddr)               \
   X(DestroyInstance) X(GetPhysicalDeviceImageFormatProperties)
#define DFNS(X)                                                                                                    \
   X(DestroyDevice) X(GetDeviceQueue) X(QueueSubmit) X(QueueSubmit2) X(QueueWaitIdle) X(DeviceWaitIdle)           \
   X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges)                         \
   X(InvalidateMappedMemoryRanges) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements)                \
   X(BindBufferMemory) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory)            \
   X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) X(CreateShaderModule)               \
   X(DestroyShaderModule) X(CreatePipelineCache) X(DestroyPipelineCache) X(GetPipelineCacheData)                  \
   X(MergePipelineCaches) X(CreateGraphicsPipelines) X(CreateComputePipelines) X(DestroyPipeline)                \
   X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)    \
   X(CreateDescriptorPool) X(DestroyDescriptorPool) X(ResetDescriptorPool) X(AllocateDescriptorSets)              \
   X(UpdateDescriptorSets) X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool)                         \
   X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)                      \
   X(ResetCommandBuffer) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(GetFenceStatus)         \
   X(CreateSemaphore) X(DestroySemaphore) X(WaitSemaphores) X(SignalSemaphore) X(GetSemaphoreCounterValue)       \
   X(CreateQueryPool) X(DestroyQueryPool) X(ResetQueryPool) X(GetQueryPoolResults) X(CmdResetQueryPool)           \
   X(CmdBeginQuery) X(CmdEndQuery) X(CmdWriteTimestamp2) X(CmdCopyQueryPoolResults) X(CmdBlitImage)               \
   X(CmdResolveImage) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdCopyBuffer) X(CmdCopyImage)            \
   X(CmdFillBuffer) X(CmdUpdateBuffer) X(CmdClearColorImage) X(CmdPipelineBarrier2) X(CmdBindPipeline)            \
   X(CmdBindDescriptorSets) X(CmdBindVertexBuffers2) X(CmdBindIndexBuffer) X(CmdDraw) X(CmdDrawIndexed)           \
   X(CmdDrawIndirect) X(CmdDispatch) X(CmdSetViewport) X(CmdSetScissor) X(CmdSetDepthBias)                        \
   X(CmdSetStencilReference) X(CmdSetBlendConstants) X(CmdSetLineWidth) X(CmdSetDepthBounds)                      \
   X(CmdSetStencilCompareMask) X(CmdSetStencilWriteMask) X(CmdSetCullMode) X(CmdSetFrontFace)                     \
   X(CmdSetPrimitiveTopology) X(CmdSetViewportWithCount) X(CmdSetScissorWithCount) X(CmdSetDepthTestEnable)       \
   X(CmdSetDepthWriteEnable) X(CmdSetDepthCompareOp) X(CmdSetDepthBoundsTestEnable) X(CmdSetStencilTestEnable)    \
   X(CmdSetStencilOp) X(CmdSetRasterizerDiscardEnable) X(CmdSetDepthBiasEnable) X(CmdSetPrimitiveRestartEnable)   \
   X(CmdBeginRendering) X(CmdEndRendering) X(CmdPushConstants) X(CmdClearAttachments)
#define X(n) static PFN_vk##n vk##n;
IFNS(X) DFNS(X)
#undef X
static PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR;

static void watchdog(int sig)
{
   (void)sig;
   static const char m[] = "WATCHDOG: replay hung (no progress for the whole budget)\n";
   if (write(1, m, sizeof m - 1) < 0) {}
   _exit(3);
}

static VkPhysicalDeviceMemoryProperties memprops;
static int find_mem(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

struct buf { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize size; };
static struct buf mkbuf(VkDeviceSize size, VkBufferUsageFlags usage, int host)
{
   struct buf o = {.size = size};
   VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   OK(vkCreateBuffer(dev, &ci, NULL, &o.b));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, o.b, &mr);
   int t = find_mem(mr.memoryTypeBits, host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (t < 0)
      t = find_mem(mr.memoryTypeBits, 0);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)t};
   OK(vkAllocateMemory(dev, &ai, NULL, &o.m));
   OK(vkBindBufferMemory(dev, o.b, o.m, 0));
   if (host)
      OK(vkMapMemory(dev, o.m, 0, VK_WHOLE_SIZE, 0, &o.p));
   return o;
}
static void rmbuf(struct buf *b)
{
   if (b->p) vkUnmapMemory(dev, b->m);
   vkDestroyBuffer(dev, b->b, NULL);
   vkFreeMemory(dev, b->m, NULL);
   memset(b, 0, sizeof *b);
}

struct img { VkImage i; VkDeviceMemory m; VkImageView v; VkFormat fmt; uint32_t w, h, levels; VkImageAspectFlags aspect; };
static struct img mkimg(VkFormat fmt, uint32_t w, uint32_t h, uint32_t levels, VkSampleCountFlagBits s, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
   struct img o = {.fmt = fmt, .w = w, .h = h, .levels = levels, .aspect = aspect};
   VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1},
                           .mipLevels = levels, .arrayLayers = 1, .samples = s, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   OK(vkCreateImage(dev, &ci, NULL, &o.i));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, o.i, &mr);
   int t = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (t < 0)
      t = find_mem(mr.memoryTypeBits, 0);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)t};
   OK(vkAllocateMemory(dev, &ai, NULL, &o.m));
   OK(vkBindImageMemory(dev, o.i, o.m, 0));
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o.i, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                               .subresourceRange = {aspect, 0, levels, 0, 1}};
   OK(vkCreateImageView(dev, &vi, NULL, &o.v));
   return o;
}
static void rmimg(struct img *i)
{
   vkDestroyImageView(dev, i->v, NULL);
   vkDestroyImage(dev, i->i, NULL);
   vkFreeMemory(dev, i->m, NULL);
   memset(i, 0, sizeof *i);
}

static void barrier(VkCommandBuffer cb, struct img *im, VkImageLayout from, VkImageLayout to, uint32_t base, uint32_t n)
{
   VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = im->i, .subresourceRange = {im->aspect, base, n, 0, 1}};
   VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
   vkCmdPipelineBarrier2(cb, &d);
}
static void membarrier(VkCommandBuffer cb)
{
   VkMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
   VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m};
   vkCmdPipelineBarrier2(cb, &d);
}

static int has_ext(const VkExtensionProperties *p, uint32_t n, const char *name)
{
   for (uint32_t i = 0; i < n; i++)
      if (!strcmp(p[i].extensionName, name))
         return 1;
   return 0;
}

#define RT_W 256
#define RT_H 256
#define NPIPE 48
#define NFIF 2

struct vtx { float pos[4]; float uv[2]; float col[4]; };

int main(int argc, char **argv)
{
   int nframes = 300;
   const char *icd = NULL;
   int msaa_gate = 0;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--msaa-gate")) msaa_gate = 1;
      else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
         char *end = NULL;
         errno = 0;
         long v = strtol(argv[++i], &end, 10);
         if (errno || end == argv[i] || *end || v < 1 || v > 100000) {
            fprintf(stderr, "bad --frames value '%s' (want 1..100000)\n", argv[i]);
            return 2;
         }
         nframes = (int)v;
      } else {
         fprintf(stderr, "unknown or incomplete argument '%s'\n", argv[i]);
         return 2;
      }
   }
   if (!icd) {
      fprintf(stderr, "usage: %s --direct <icd.so> [--frames N] [--msaa-gate]\n", argv[0]);
      return 2;
   }
   setvbuf(stdout, NULL, _IONBF, 0);
   signal(SIGALRM, watchdog);
   alarm(240);   /* the whole run; ASan frames take ~1 s per 20 */
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) return 2;
   PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Unity D3D11", .pEngineName = "DXVK",
                            .engineVersion = VK_MAKE_VERSION(2, 7, 0), .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   OK(createInstance(&ici, NULL, &inst));
#define X(n) vk##n = (PFN_vk##n)gipa(inst, "vk" #n);
   IFNS(X)
#undef X
   uint32_t np = 1;
   VkPhysicalDevice pd;
   OK(vkEnumeratePhysicalDevices(inst, &np, &pd));
   vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
   {
      /* the instance asks for 1.3 and the replay chains the 1.3 feature structs: the device must report at least that.
       * (the v11/v13 in check.sh are the Mali GPU architecture classes, not Vulkan versions) */
      VkPhysicalDeviceProperties2 pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      vkGetPhysicalDeviceProperties2(pd, &pp);
      printf("device apiVersion %u.%u.%u\n", VK_VERSION_MAJOR(pp.properties.apiVersion), VK_VERSION_MINOR(pp.properties.apiVersion),
             VK_VERSION_PATCH(pp.properties.apiVersion));
      CHECK(pp.properties.apiVersion >= VK_API_VERSION_1_3, "device apiVersion 0x%x < 1.3 but the replay uses 1.3 features", pp.properties.apiVersion);
      if (g_fail) return 1;
   }

   uint32_t ne = 0;
   vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
   VkExtensionProperties *eps = calloc(ne + 1, sizeof *eps);
   vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, eps);
   const char **use = calloc(ne + 1, sizeof *use);
   for (uint32_t i = 0; i < ne; i++) use[i] = eps[i].extensionName;
   int have_push = has_ext(eps, ne, "VK_KHR_push_descriptor");

   /* DXVK's feature chain: query everything it asks for, enable exactly what is true */
   VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
   VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f11};
   VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &f12};
   VkPhysicalDeviceRobustness2FeaturesEXT frb = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .pNext = &f13};
   VkPhysicalDeviceCustomBorderColorFeaturesEXT fcb = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .pNext = &frb};
   VkPhysicalDeviceDepthClipEnableFeaturesEXT fdc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, .pNext = &fcb};
   VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT fvd = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .pNext = &fdc};
   VkPhysicalDeviceLineRasterizationFeaturesEXT flr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, .pNext = &fvd};
   VkPhysicalDeviceMaintenance5FeaturesKHR fm5 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, .pNext = &flr};
   VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fm5};
   vkGetPhysicalDeviceFeatures2(pd, &f2);
   CHECK(f13.dynamicRendering && f13.synchronization2 && f12.timelineSemaphore, "DXVK needs dynamicRendering/sync2/timeline");

   uint32_t nq = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties *qf = calloc(nq + 1, sizeof *qf);
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   uint32_t qfi = 0;
   int qfound = 0;
   for (uint32_t i = 0; i < nq; i++)
      if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) &&
          qf[i].timestampValidBits > 0) { qfi = i; qfound = 1; break; }
   CHECK(qfound, "no graphics+compute queue family with timestampValidBits > 0 (timestamp queries are replayed)");
   CHECK(f12.hostQueryReset, "hostQueryReset is not advertised: DXVK resets query pools from the host");
   if (g_fail) return 1;
   /* precise occlusion is optional: without it the replay uses imprecise occlusion queries and says so */
   int have_precise = f2.features.occlusionQueryPrecise;
   if (!have_precise) printf("note: occlusionQueryPrecise is false: replaying imprecise occlusion queries only\n");
   float prio = 1.0f;
   VkDeviceQueueCreateInfo dq = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq,
                             .enabledExtensionCount = ne, .ppEnabledExtensionNames = use};
   OK(vkCreateDevice(pd, &dci, NULL, &dev));
   if (!dev) return 1;
   vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   gdpa = vkGetDeviceProcAddr;
#define X(n) vk##n = (PFN_vk##n)gdpa(dev, "vk" #n); CHECK(vk##n, "no vk" #n);
   DFNS(X)
#undef X
   vkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)gdpa(dev, "vkCmdPushDescriptorSetKHR");
   if (g_fail) return 1;
   VkQueue q;
   vkGetDeviceQueue(dev, qfi, 0, &q);
   printf("device up: %u extensions, push_descriptor %d, queue family %u\n", ne, have_push, qfi);

   /* format choices as DXVK does: D32S8 if the depth format supports attachment use, else D24S8 */
   VkFormat dfmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
   {
      VkFormatProperties fp;
      vkGetPhysicalDeviceFormatProperties(pd, dfmt, &fp);
      if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) dfmt = VK_FORMAT_D24_UNORM_S8_UINT;
      vkGetPhysicalDeviceFormatProperties(pd, dfmt, &fp);
      CHECK(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "no depth-stencil format");
   }
   VkImageFormatProperties ifp;
   vkGetPhysicalDeviceImageFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &ifp);
   VkSampleCountFlagBits msaa = (ifp.sampleCounts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;

   /* pipeline cache: empty, then rebuilt from the blob below */
   VkPipelineCacheCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
   VkPipelineCache pcache, pcache2;
   OK(vkCreatePipelineCache(dev, &pci, NULL, &pcache));
   OK(vkCreatePipelineCache(dev, &pci, NULL, &pcache2));

   /* descriptor layouts: set 0 = dynamic UBO (VS), set 1 = combined sampler (FS); optional push-descriptor layout for set 0 */
   VkDescriptorSetLayoutBinding b0 = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};
   VkDescriptorSetLayoutBinding b1 = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutBinding bcs = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1};
   VkDescriptorSetLayout dsl0, dsl1, dslcs, dslpush = VK_NULL_HANDLE;
   dl.pBindings = &b0; OK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl0));
   dl.pBindings = &b1; OK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl1));
   dl.pBindings = &bcs; OK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dslcs));
   if (have_push) {
      VkDescriptorSetLayoutBinding bp = b0;
      bp.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; dl.pBindings = &bp;
      OK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dslpush));
      dl.flags = 0;
   }
   VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
   VkDescriptorSetLayout sets01[2] = {dsl0, dsl1}, setsp[2] = {dslpush, dsl1};
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = sets01,
                                      .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   VkPipelineLayout pl, plp = VK_NULL_HANDLE, plcs;
   OK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
   if (have_push) { plci.pSetLayouts = setsp; OK(vkCreatePipelineLayout(dev, &plci, NULL, &plp)); }
   VkPipelineLayoutCreateInfo plcc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dslcs};
   OK(vkCreatePipelineLayout(dev, &plcc, NULL, &plcs));

   VkShaderModule vs, fs, cs;
   VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   smi.codeSize = sizeof spv_vs; smi.pCode = spv_vs; OK(vkCreateShaderModule(dev, &smi, NULL, &vs));
   smi.codeSize = sizeof spv_fs; smi.pCode = spv_fs; OK(vkCreateShaderModule(dev, &smi, NULL, &fs));
   smi.codeSize = sizeof spv_cs; smi.pCode = spv_cs; OK(vkCreateShaderModule(dev, &smi, NULL, &cs));

   /* NPIPE graphics pipelines: spec constants x topology x cull x blend x depth x stencil x sample count, all with DXVK's dynamic set */
   VkPipeline pipes[NPIPE];
   int msaa_gap = 0;
   VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                           VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                           VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                           VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                           VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
                           VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
   VkVertexInputBindingDescription vb[2] = {{0, sizeof(struct vtx), VK_VERTEX_INPUT_RATE_VERTEX}, {1, 16, VK_VERTEX_INPUT_RATE_INSTANCE}};
   VkVertexInputAttributeDescription va[4] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 16}, {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 24},
                                              {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};
   for (int i = 0; i < NPIPE; i++) {
      VkBool32 sc_flip = i & 1; uint32_t sc_n = 1 + (i % 4); int32_t sc_mode = (i >> 1) & 1;
      VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
      struct { VkBool32 a; uint32_t b; int32_t c; } sd = {sc_flip, sc_n, sc_mode};
      VkSpecializationInfo si_v = {2, me, 8, &sd};
      VkSpecializationMapEntry me_f = {2, 8, 4};
      VkSpecializationInfo si_f = {1, &me_f, 12, &sd};
      VkPipelineShaderStageCreateInfo st[2] = {
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main", .pSpecializationInfo = &si_v},
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main", .pSpecializationInfo = &si_f}};
      VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 2,
                                                 .pVertexBindingDescriptions = vb, .vertexAttributeDescriptionCount = 4, .pVertexAttributeDescriptions = va};
      static const VkPrimitiveTopology topo[4] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                                                  VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
      VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topo[(i >> 2) & 3]};
      VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
      VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
                                                   .cullMode = (i >> 4) % 3, .frontFace = VK_FRONT_FACE_CLOCKWISE, .depthBiasEnable = (i >> 3) & 1, .lineWidth = 1.0f};
      VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                 .rasterizationSamples = (i % 5 == 4) ? msaa : VK_SAMPLE_COUNT_1_BIT};
      VkStencilOpState sop = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 1};
      VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = 1, .depthWriteEnable = 1,
                                                  .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL, .stencilTestEnable = i & 2, .front = sop, .back = sop};
      VkPipelineColorBlendAttachmentState cba = {.blendEnable = (i >> 1) & 1, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                                                 .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
                                                 .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                 .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
      VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
      VkFormat cfmt = VK_FORMAT_R8G8B8A8_UNORM;
      VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &cfmt,
                                          .depthAttachmentFormat = dfmt, .stencilAttachmentFormat = dfmt};
      VkPipelineDynamicStateCreateInfo dyi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof dyn[0], .pDynamicStates = dyn};
      VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
                                         .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                         .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dyi,
                                         .layout = (have_push && (i % 7 == 3)) ? plp : pl, .subpass = 0, .basePipelineIndex = -1};
      VkResult gr = vkCreateGraphicsPipelines(dev, i < NPIPE / 2 ? pcache : pcache2, 1, &gp, NULL, &pipes[i]);
      if (gr == VK_ERROR_FEATURE_NOT_PRESENT && ms.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT && !msaa_gate) {
         /* KNOWN GAP (mali_pipeline.c, vk/README.md): 4x is advertised but 4x pipelines need MALI_EXPERIMENTAL_MSAA=1 */
         msaa_gap++;
         pipes[i] = VK_NULL_HANDLE;
         continue;
      }
      CHECK(gr == VK_SUCCESS, "pipeline %d (topology %d cull %d samples %d depthBias %d stencil %d blend %d) -> %d", i, ia.topology, rs.cullMode,
            ms.rasterizationSamples, rs.depthBiasEnable, ds.stencilTestEnable, cba.blendEnable, gr);
   }
   if (msaa_gate) {
      CHECK(msaa == VK_SAMPLE_COUNT_4_BIT, "MALI_EXPERIMENTAL_MSAA run but 4x is not reported for RGBA8 colour attachments");
      CHECK(!msaa_gap, "MALI_EXPERIMENTAL_MSAA run rejected %d 4x pipelines", msaa_gap);
   }
   VkPipeline cpipe;
   {
      uint32_t local = 16;
      VkSpecializationMapEntry me = {0, 0, 4};
      VkSpecializationInfo si = {1, &me, 4, &local};
      VkComputePipelineCreateInfo cc = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = plcs, .basePipelineIndex = -1,
                                        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                  .module = cs, .pName = "main", .pSpecializationInfo = &si}};
      OK(vkCreateComputePipelines(dev, pcache, 1, &cc, NULL, &cpipe));
   }
   /* cache blob round trip */
   {
      OK(vkMergePipelineCaches(dev, pcache, 1, &pcache2));
      size_t sz = 0;
      OK(vkGetPipelineCacheData(dev, pcache, &sz, NULL));
      void *blob = malloc(sz ? sz : 1);
      size_t sz2 = sz;
      OK(vkGetPipelineCacheData(dev, pcache, &sz2, blob));
      VkPipelineCacheCreateInfo pc3 = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz2, .pInitialData = blob};
      VkPipelineCache pcache3;
      OK(vkCreatePipelineCache(dev, &pc3, NULL, &pcache3));
      /* truncated read (DXVK passes the size it got): VK_INCOMPLETE is the contract */
      if (sz > 8) {
         size_t part = sz / 2;
         VkResult r = vkGetPipelineCacheData(dev, pcache, &part, blob);
         CHECK(r == VK_INCOMPLETE, "truncated GetPipelineCacheData %d (want VK_INCOMPLETE)", r);
      }
      vkDestroyPipelineCache(dev, pcache3, NULL);
      free(blob);
      printf("pipeline cache: %zu bytes, %d pipelines + 1 compute\n", sz, NPIPE);
   }

   /* resources */
   struct img rt = mkimg(VK_FORMAT_R8G8B8A8_UNORM, RT_W, RT_H, 1, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
   struct img msrt = mkimg(VK_FORMAT_R8G8B8A8_UNORM, RT_W, RT_H, 1, msaa, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
   struct img dep = mkimg(dfmt, RT_W, RT_H, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
   struct img msdep = mkimg(dfmt, RT_W, RT_H, 1, msaa, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
   struct img half = mkimg(VK_FORMAT_R8G8B8A8_UNORM, RT_W / 2, RT_H / 2, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
   struct img tex = mkimg(VK_FORMAT_R8G8B8A8_UNORM, 256, 256, 9, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
   struct buf vbuf = mkbuf(4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
   struct buf ubuf = mkbuf(64 * 1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1);
   struct buf stag = mkbuf(256 * 256 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
   struct buf sbuf = mkbuf(4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0);
   struct buf qbuf = mkbuf(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
   struct buf indbuf = mkbuf(256, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, 1);
   if (g_fail || !vbuf.p || !ubuf.p || !stag.p || !qbuf.p || !indbuf.p) { printf("FAIL: setup buffer allocation or map failed\n"); return 1; }
   memset(stag.p, 0x7f, stag.size);
   {
      struct vtx v[4] = {{{-1, -1, 0.5f, 1}, {0, 0}, {1, 0, 0, 1}}, {{1, -1, 0.5f, 1}, {1, 0}, {0, 1, 0, 1}},
                         {{-1, 1, 0.5f, 1}, {0, 1}, {0, 0, 1, 1}}, {{1, 1, 0.5f, 1}, {1, 1}, {1, 1, 1, 1}}};
      memcpy(vbuf.p, v, sizeof v);
      float inst[8] = {1, 1, 1, 1, 0.5f, 0.75f, 1, 1};   /* per-instance data, binding 1 at vbuf+1024 */
      memcpy((char *)vbuf.p + 1024, inst, sizeof inst);
      uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
      memcpy((char *)vbuf.p + 2048, idx, sizeof idx);
      VkDrawIndirectCommand dc = {4, 1, 0, 0};
      memcpy(indbuf.p, &dc, sizeof dc);
      float *u = ubuf.p;
      for (int i = 0; i < 64 * 1024 / 4; i++) u[i] = (i % 20 == 0 || i % 20 == 5 || i % 20 == 10 || i % 20 == 15) ? 1.0f : 0.25f;
   }
   VkSamplerCustomBorderColorCreateInfoEXT cbc = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT, .customBorderColor = {.float32 = {0.1f, 0.2f, 0.3f, 0.4f}}, .format = VK_FORMAT_R8G8B8A8_UNORM};
   VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                              .maxLod = VK_LOD_CLAMP_NONE, .borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT};
   if (!fcb.customBorderColors) sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; else sci.pNext = &cbc;
   VkSampler smp;
   OK(vkCreateSampler(dev, &sci, NULL, &smp));

   VkQueryPoolCreateInfo qpo = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 16};
   VkQueryPoolCreateInfo qpt = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 8};
   VkQueryPool qo[NFIF], qt[NFIF];
   for (int i = 0; i < NFIF; i++) { OK(vkCreateQueryPool(dev, &qpo, NULL, &qo[i])); OK(vkCreateQueryPool(dev, &qpt, NULL, &qt[i])); }

   VkDescriptorPoolSize dps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
   VkDescriptorPoolCreateInfo dpc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 192, .poolSizeCount = 3, .pPoolSizes = dps};
   VkDescriptorPool dpool[NFIF];
   for (int i = 0; i < NFIF; i++) OK(vkCreateDescriptorPool(dev, &dpc, NULL, &dpool[i]));

   VkCommandPoolCreateInfo cpc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfi};
   VkCommandPool cpool[NFIF];
   VkCommandBuffer cbs[NFIF];
   VkFence fences[NFIF];
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
   for (int i = 0; i < NFIF; i++) {
      OK(vkCreateCommandPool(dev, &cpc, NULL, &cpool[i]));
      VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool[i], .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
      OK(vkAllocateCommandBuffers(dev, &cai, &cbs[i]));
      OK(vkCreateFence(dev, &fci, NULL, &fences[i]));
   }
   VkSemaphoreTypeCreateInfo sti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
   VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &sti};
   VkSemaphore tl, tl_host;
   OK(vkCreateSemaphore(dev, &semi, NULL, &tl));
   OK(vkCreateSemaphore(dev, &semi, NULL, &tl_host));

   /* texture upload (staging -> mip 0), once */
   {
      VkCommandBuffer c = cbs[0];
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      OK(vkResetFences(dev, 1, &fences[0]));
      OK(vkBeginCommandBuffer(c, &bi));
      barrier(c, &tex, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 9);
      VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {256, 256, 1}};
      vkCmdCopyBufferToImage(c, stag.b, tex.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
      OK(vkEndCommandBuffer(c));
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c};
      OK(vkQueueSubmit(q, 1, &si, fences[0]));
      OK(vkWaitForFences(dev, 1, &fences[0], VK_TRUE, 10ull * 1000000000ull));
   }

   uint64_t tl_val = 0, host_val = 0;
   VkImageLayout tex_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   uint64_t frame_done[NFIF] = {0, 0};
   int msaa_frames = 0;
   for (int f = 0; f < nframes && !g_fail; f++) {
      alarm(240);
      int s = f % NFIF;
      /* wait for the frame that used this slot: timeline wait with a timeout (never UINT64_MAX), then fence */
      if (frame_done[s]) {
         VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &tl, .pValues = &frame_done[s]};
         VkResult r = vkWaitSemaphores(dev, &wi, 10ull * 1000000000ull);
         CHECK(r == VK_SUCCESS, "frame %d: vkWaitSemaphores(%llu) -> %d", f, (unsigned long long)frame_done[s], r);
         if (g_fail) break;   /* the GPU may still own this slot's objects: do not reset them */
      }
      OK(vkWaitForFences(dev, 1, &fences[s], VK_TRUE, 10ull * 1000000000ull));
      if (g_fail) break;
      OK(vkResetFences(dev, 1, &fences[s]));
      uint64_t cur = 0;
      OK(vkGetSemaphoreCounterValue(dev, tl, &cur));
      CHECK(cur >= frame_done[s], "frame %d: counter %llu < waited %llu", f, (unsigned long long)cur, (unsigned long long)frame_done[s]);
      if (f % 3 == 0) OK(vkResetCommandPool(dev, cpool[s], f % 6 == 0 ? VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT : 0));
      else OK(vkResetCommandBuffer(cbs[s], 0));
      OK(vkResetDescriptorPool(dev, dpool[s], 0));
      /* previous results of this slot (frame f-2 is complete: timeline + fence waited), host read BEFORE any reset.
       * It wrote occlusion queries 0..15 and timestamps 0 and 1: the ones read here must all be available. */
      if (f >= NFIF) {
         uint64_t res[4 * 2];
         VkResult r = vkGetQueryPoolResults(dev, qo[s], 0, 4, sizeof res, res, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
         CHECK(r == VK_SUCCESS, "GetQueryPoolResults occlusion %d", r);
         for (int k = 0; k < 4; k++) CHECK(r != VK_SUCCESS || res[2 * k + 1] != 0, "frame %d: occlusion query %d unavailable", f, k);
         uint64_t ts[2 * 2];
         r = vkGetQueryPoolResults(dev, qt[s], 0, 2, sizeof ts, ts, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
         CHECK(r == VK_SUCCESS, "GetQueryPoolResults timestamp %d", r);
         for (int k = 0; k < 2; k++) CHECK(r != VK_SUCCESS || ts[2 * k + 1] != 0, "frame %d: timestamp %d unavailable", f, k);
      }
      /* host query reset (hostQueryReset, required above) every 4th frame, command reset otherwise */
      int host_reset = f % 4 == 1;
      if (host_reset) { vkResetQueryPool(dev, qo[s], 0, 16); vkResetQueryPool(dev, qt[s], 0, 8); }

      VkCommandBuffer c = cbs[s];
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      OK(vkBeginCommandBuffer(c, &bi));
      if (!host_reset) { vkCmdResetQueryPool(c, qo[s], 0, 16); vkCmdResetQueryPool(c, qt[s], 0, 8); }
      vkCmdWriteTimestamp2(c, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qt[s], 0);

      /* descriptor sets as DXVK: allocate per draw group from the per-frame pool, update, bind with a dynamic offset */
      VkDescriptorSetLayout al[3] = {dsl0, dsl1, dslcs};
      VkDescriptorSet ds[3];
      VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool[s], .descriptorSetCount = 3, .pSetLayouts = al};
      OK(vkAllocateDescriptorSets(dev, &dai, ds));
      VkDescriptorBufferInfo dbi0 = {ubuf.b, 0, 320};
      VkDescriptorImageInfo dii = {smp, tex.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorBufferInfo dbi2 = {sbuf.b, 0, VK_WHOLE_SIZE};
      VkWriteDescriptorSet w[3] = {
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds[0], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &dbi0},
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds[1], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii},
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds[2], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi2}};
      vkUpdateDescriptorSets(dev, 3, w, 0, NULL);

      /* mip chain regeneration every 5th frame: blit level n-1 -> n */
      if (f % 5 == 0) {
         if (tex_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) barrier(c, &tex, tex_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 9);
         for (uint32_t l = 1; l < 9; l++) {
            barrier(c, &tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l - 1, 1);
            VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1}, .srcOffsets = {{0, 0, 0}, {256 >> (l - 1), 256 >> (l - 1), 1}},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}, .dstOffsets = {{0, 0, 0}, {256 >> l, 256 >> l, 1}}};
            vkCmdBlitImage(c, tex.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
         }
         barrier(c, &tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 8);
         barrier(c, &tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 8, 1);
         tex_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      if (tex_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
         barrier(c, &tex, tex_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 9);
         tex_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }

      /* buffer ops, compute */
      vkCmdFillBuffer(c, sbuf.b, 0, 1024, f);
      uint32_t upd[4] = {1, 2, 3, 4};
      vkCmdUpdateBuffer(c, sbuf.b, 1024, sizeof upd, upd);
      membarrier(c);
      vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
      vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, plcs, 0, 1, &ds[2], 0, NULL);
      vkCmdDispatch(c, 16, 1, 1);
      membarrier(c);

      int msaa_frame = f % 5 == 4 && !msaa_gap && msaa == VK_SAMPLE_COUNT_4_BIT;
      if (msaa_frame) msaa_frames++;
      struct img *target = msaa_frame ? &msrt : &rt;
      barrier(c, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
      struct img *dtarget = msaa_frame ? &msdep : &dep;
      barrier(c, dtarget, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1);
      VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = target->v, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = {.color = {.float32 = {0.1f, 0.2f, 0.3f, 1}}}};
      VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dtarget->v, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .clearValue = {.depthStencil = {1.0f, 0}}};
      VkRenderingInfo rinfo = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {RT_W, RT_H}}, .layerCount = 1, .colorAttachmentCount = 1,
                               .pColorAttachments = &ca, .pDepthAttachment = &da, .pStencilAttachment = &da};
      /* the MSAA pipelines are every 5th index and render into a 1-sample target only if the sample counts match: keep them separate */
      vkCmdBeginRendering(c, &rinfo);
      VkViewport vpt = {0, 0, RT_W, RT_H, 0, 1};
      VkRect2D sc = {{0, 0}, {RT_W, RT_H}};
      vkCmdSetViewport(c, 0, 1, &vpt);
      vkCmdSetScissor(c, 0, 1, &sc);
      float bconst[4] = {0.5f, 0.5f, 0.5f, 0.5f};
      vkCmdSetBlendConstants(c, bconst);
      vkCmdSetLineWidth(c, 1.0f);
      VkBuffer vbs[2] = {vbuf.b, vbuf.b};
      VkDeviceSize vo[2] = {0, 1024}, vsz[2] = {sizeof(struct vtx) * 4, 32}, vst[2] = {sizeof(struct vtx), 16};
      vkCmdBindVertexBuffers2(c, 0, 2, vbs, vo, vsz, vst);
      vkCmdBindIndexBuffer(c, vbuf.b, 2048, VK_INDEX_TYPE_UINT16);
      int nd = 0;
      for (int i = 0; i < NPIPE; i++) {
         int pipe_msaa = (i % 5 == 4) && !msaa_gap && msaa == VK_SAMPLE_COUNT_4_BIT;
         if (pipe_msaa != msaa_frame) continue;
         if (!pipes[i]) continue;
         vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[i]);
         uint32_t dyn_off = (uint32_t)((i * 256) % (60 * 1024));
         int pushed = have_push && (i % 7 == 3);
         VkPipelineLayout use_pl = pushed ? plp : pl;
         if (pushed) {
            VkDescriptorBufferInfo pb = {ubuf.b, dyn_off, 320};
            VkWriteDescriptorSet pw = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &pb};
            vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_GRAPHICS, plp, 0, 1, &pw);
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, use_pl, 1, 1, &ds[1], 0, NULL);
         } else {
            VkDescriptorSet both[2] = {ds[0], ds[1]};
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, use_pl, 0, 2, both, 1, &dyn_off);
         }
         uint32_t pcv[4] = {(uint32_t)i, (uint32_t)f, 0, 0};
         vkCmdPushConstants(c, use_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pcv);
         vkCmdSetDepthBias(c, 1.0f + (i % 3), 0.0f, 2.0f);
         vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)(i & 0xff));
         vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
         vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
         vkCmdSetCullMode(c, (i + f) % 3);
         vkCmdSetFrontFace(c, (i & 1) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
         vkCmdSetDepthTestEnable(c, 1);
         vkCmdSetDepthWriteEnable(c, i & 1);
         vkCmdSetDepthCompareOp(c, VK_COMPARE_OP_LESS_OR_EQUAL);
         vkCmdSetStencilTestEnable(c, (i >> 1) & 1);
         vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
         vkCmdSetDepthBiasEnable(c, i & 1);
         vkCmdSetPrimitiveRestartEnable(c, 0);
         int q_i = nd % 16;
         int use_q = nd < 16;
         if (use_q) vkCmdBeginQuery(c, qo[s], q_i, ((i & 1) && have_precise) ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
         if (i % 3 == 0) vkCmdDrawIndexed(c, 6, 1 + (i % 2), 0, 0, 0);
         else if (i % 3 == 1) vkCmdDraw(c, 4, 1, 0, 0);
         else vkCmdDrawIndirect(c, indbuf.b, 0, 1, 16);
         if (use_q) vkCmdEndQuery(c, qo[s], q_i);
         nd++;
      }
      /* clear attachments inside the pass (DXVK's ClearRenderTarget on a bound target) */
      VkClearAttachment cla = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0, .clearValue = {.color = {.float32 = {0, 0, 0, 0}}}};
      VkClearRect clr = {.rect = {{16, 16}, {32, 32}}, .layerCount = 1};
      if (!msaa_frame) /* known gap: ClearAttachments in an MSAA pass is unsupported (mali_cmd.c M8) */
         vkCmdClearAttachments(c, 1, &cla, 1, &clr);
      vkCmdEndRendering(c);
      vkCmdWriteTimestamp2(c, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qt[s], 1);

      /* resolve / blit chain: msaa -> rt (resolve), rt -> half (blit, linear), half -> staging readback */
      if (msaa_frame) {
         barrier(c, &msrt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
         barrier(c, &rt, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
         VkImageResolve rr = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {RT_W, RT_H, 1}};
         vkCmdResolveImage(c, msrt.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rt.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rr);
         barrier(c, &rt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
      } else {
         barrier(c, &rt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
      }
      barrier(c, &half, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
      VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {RT_W, RT_H, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {RT_W / 2, RT_H / 2, 1}}};
      vkCmdBlitImage(c, rt.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, half.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
      {  /* the flipped blit overwrites part of the full blit's destination: order write-after-write */
         VkMemoryBarrier2 waw = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                                 .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                                 .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
         VkDependencyInfo wd = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &waw};
         vkCmdPipelineBarrier2(c, &wd);
      }
      /* flipped + partial blit (D3D11 CopySubresourceRegion with mismatching size) */
      VkImageBlit bl2 = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{10, 10, 0}, {200, 200, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{100, 100, 0}, {4, 4, 1}}};
      vkCmdBlitImage(c, rt.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, half.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl2, VK_FILTER_NEAREST);
      barrier(c, &half, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
      VkBufferImageCopy rb = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {RT_W / 2, RT_H / 2, 1}};
      vkCmdCopyImageToBuffer(c, half.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stag.b, 1, &rb);
      VkImageCopy ic = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {64, 64, 1}};
      (void)ic;
      membarrier(c);
      /* query results to a buffer (DXVK resolves D3D11 queries on the GPU) */
      vkCmdCopyQueryPoolResults(c, qo[s], 0, 8, qbuf.b, 0, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      vkCmdCopyQueryPoolResults(c, qt[s], 0, 2, qbuf.b, 1024, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      vkCmdCopyQueryPoolResults(c, qo[s], 8, 8, qbuf.b, 2048, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_PARTIAL_BIT);
      OK(vkEndCommandBuffer(c));

      /* submit: odd frames vkQueueSubmit + timeline info, even frames vkQueueSubmit2 with timeline + a host-signalled wait */
      uint64_t sig = ++tl_val;
      uint64_t hw = 0;
      if (f % 4 == 2) {
         /* host timeline value the GPU submission waits on, signalled before submit (DXVK's cross-queue sync pattern) */
         hw = ++host_val;
         VkSemaphoreSignalInfo hs = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = tl_host, .value = hw};
         OK(vkSignalSemaphore(dev, &hs));
      }
      if (f & 1) {
         VkTimelineSemaphoreSubmitInfo tsi = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = hw ? 1 : 0, .pWaitSemaphoreValues = &hw,
                                             .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sig};
         VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
         VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi, .waitSemaphoreCount = hw ? 1 : 0, .pWaitSemaphores = &tl_host, .pWaitDstStageMask = &ws,
                            .commandBufferCount = 1, .pCommandBuffers = &c, .signalSemaphoreCount = 1, .pSignalSemaphores = &tl};
         OK(vkQueueSubmit(q, 1, &si, fences[s]));
      } else {
         VkSemaphoreSubmitInfo wsi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = tl_host, .value = hw, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
         VkSemaphoreSubmitInfo ssi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = tl, .value = sig, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
         VkCommandBufferSubmitInfo csi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = c};
         VkSubmitInfo2 s2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = hw ? 1 : 0, .pWaitSemaphoreInfos = &wsi, .commandBufferInfoCount = 1,
                             .pCommandBufferInfos = &csi, .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &ssi};
         OK(vkQueueSubmit2(q, 1, &s2, fences[s]));
      }
      frame_done[s] = sig;

      /* resource churn every 25 frames: a transient buffer + image created, used for a copy, destroyed after the queue drains (D3D11 Map(DISCARD) / dynamic textures) */
      if (f % 25 == 24) {
         OK(vkQueueWaitIdle(q));
         struct buf tb = mkbuf(1 << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, f % 50 == 24);
         struct img ti = mkimg(VK_FORMAT_R8G8B8A8_UNORM, 128 + f % 64, 64, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
         rmimg(&ti);
         rmbuf(&tb);
      }
   }
   OK(vkDeviceWaitIdle(dev));
   uint64_t fin = 0;
   OK(vkGetSemaphoreCounterValue(dev, tl, &fin));
   CHECK(fin == tl_val || g_fail, "final timeline %llu != submitted %llu", (unsigned long long)fin, (unsigned long long)tl_val);

   /* teardown, as DXVK's device destructor */
   for (int i = 0; i < NFIF; i++) {
      vkDestroyFence(dev, fences[i], NULL);
      vkDestroyCommandPool(dev, cpool[i], NULL);
      vkDestroyDescriptorPool(dev, dpool[i], NULL);
      vkDestroyQueryPool(dev, qo[i], NULL);
      vkDestroyQueryPool(dev, qt[i], NULL);
   }
   vkDestroySemaphore(dev, tl, NULL);
   vkDestroySemaphore(dev, tl_host, NULL);
   vkDestroySampler(dev, smp, NULL);
   rmbuf(&vbuf); rmbuf(&ubuf); rmbuf(&stag); rmbuf(&sbuf); rmbuf(&qbuf); rmbuf(&indbuf);
   rmimg(&rt); rmimg(&msrt); rmimg(&dep); rmimg(&msdep); rmimg(&half); rmimg(&tex);
   for (int i = 0; i < NPIPE; i++) if (pipes[i]) vkDestroyPipeline(dev, pipes[i], NULL);
   vkDestroyPipeline(dev, cpipe, NULL);
   vkDestroyShaderModule(dev, vs, NULL); vkDestroyShaderModule(dev, fs, NULL); vkDestroyShaderModule(dev, cs, NULL);
   vkDestroyPipelineLayout(dev, pl, NULL);
   if (plp) vkDestroyPipelineLayout(dev, plp, NULL);
   vkDestroyPipelineLayout(dev, plcs, NULL);
   vkDestroyDescriptorSetLayout(dev, dsl0, NULL); vkDestroyDescriptorSetLayout(dev, dsl1, NULL); vkDestroyDescriptorSetLayout(dev, dslcs, NULL);
   if (dslpush) vkDestroyDescriptorSetLayout(dev, dslpush, NULL);
   vkDestroyPipelineCache(dev, pcache, NULL);
   vkDestroyPipelineCache(dev, pcache2, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   free(eps); free(use); free(qf);
   if (msaa_gate) CHECK(msaa_frames > 0 || g_fail, "MALI_EXPERIMENTAL_MSAA run recorded no 4x frame / resolve");
   if (msaa_gap) printf("note: %d of %d pipelines (4x MSAA) rejected: known gap, advertised 4x but MALI_EXPERIMENTAL_MSAA is off\n", msaa_gap, NPIPE);
   printf("%s: %d frames\n", g_fail ? "FAIL" : "dxvk_replay ok", nframes);
   return g_fail;
}
