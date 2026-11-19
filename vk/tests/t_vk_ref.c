/* SPDX-License-Identifier: MIT */
/* One reference scene through whichever Vulkan implementation is under the binary. Two uses, same code:
 *
 *   phone  : VK_DRIVER_FILES=... t_vk_ref --direct libvulkan_mali.so --spv-dir vk/shader --out phone.ppm
 *   desktop: VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json t_vk_ref --spv-dir vk/shader --out ref.ppm
 *
 * The scene (geometry, texture, clears, formats, state) is vk/tests/ref_scene.h, so the only difference between the two
 * runs is the driver that executes it. `vk/tests/refdiff.py` compares the two images and reports which 64-row blocks
 * differ; the JSON line this program prints carries the same per-block digests, so a phone can be checked on the farm
 * without copying a 768 KiB image back. See docs/desktop-gpu.md for what this proves and what it cannot.
 *
 * The colour image is OPTIMAL-tiled on purpose (AFBC on Mali) and read back with vkCmdCopyImageToBuffer: a store or
 * decode that drops/shifts a 64-row block is the failure this test is built to see.
 *
 * Exit 0 = the scene recorded and rendered (or the fake backend recorded it and rendered nothing), 1 = failure, 77 = skip.
 *   t_vk_ref [--direct <icd.so>] [--spv-dir dir] [--out file.ppm] [--json file] [--loop N] [--nearest] [--quiet]
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "ref_scene.h"

static int g_fail;
#define CHECK(c, ...)                                                                                                      \
   do {                                                                                                                    \
      if (!(c)) {                                                                                                          \
         printf("{\"fail\":\"%s:%d\",\"expr\":\"%s\",\"detail\":\"", __FILE__, __LINE__, #c);                            \
         printf(__VA_ARGS__);                                                                                              \
         printf("\"}\n");                                                                                                  \
         g_fail = 1;                                                                                                       \
         return 0;                                                                                                         \
      }                                                                                                                    \
   } while (0)

#define DEV_FNS(X)                                                                                                         \
   X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle) X(CreateImage) X(DestroyImage)                                     \
   X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateBuffer)                 \
   X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(AllocateMemory) X(FreeMemory) X(MapMemory)        \
   X(UnmapMemory) X(CreateShaderModule) X(DestroyShaderModule) X(CreateSampler) X(DestroySampler)                          \
   X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool)             \
   X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout)                      \
   X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer)             \
   X(DestroyFramebuffer) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(FreeCommandBuffers)       \
   X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline)                 \
   X(CmdBindDescriptorSets) X(CmdBindVertexBuffers) X(CmdPushConstants) X(CmdDraw) X(CmdSetViewport) X(CmdSetScissor)      \
   X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(QueueSubmit) X(CreateFence) X(DestroyFence)     \
   X(WaitForFences) X(ResetFences)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
#undef DECL

static VkInstance inst;
static VkPhysicalDevice pd;
static VkPhysicalDeviceMemoryProperties memprops;
static VkDevice dev;
static VkQueue queue;

static const char *g_spv = "shader";
static const char *g_out;
static const char *g_json;
static int g_loop = 1;
static int g_nearest;
static int g_fake;

static double
now_us(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static int
memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

static VkShaderModule
load_spv(const char *name)
{
   char path[512];
   snprintf(path, sizeof path, "%s/%s", g_spv, name);
   FILE *f = fopen(path, "rb");
   if (!f) {
      printf("{\"fail\":\"open %s\"}\n", path);
      return VK_NULL_HANDLE;
   }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *buf = malloc((size_t)n);
   if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
      fclose(f);
      free(buf);
      return VK_NULL_HANDLE;
   }
   fclose(f);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = buf};
   VkShaderModule m = VK_NULL_HANDLE;
   if (CreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS)
      m = VK_NULL_HANDLE;
   free(buf);
   return m;
}

/* ------------------------------------------------------------------ resources */

struct ref {
   VkImage col, dep, tex;
   VkDeviceMemory colm, depm, texm;
   VkImageView colv, depv, texv;
   VkBuffer vb, texb, rdb;
   VkDeviceMemory vbm, texbm, rdbm;
   uint8_t *rd;
   VkSampler smp;
   VkDescriptorSetLayout dsl;
   VkDescriptorPool dpool;
   VkDescriptorSet dset;
   VkPipelineLayout pl;
   VkPipeline pipe;
   VkRenderPass rp1, rp2;
   VkFramebuffer fb;
   VkCommandPool cpool;
   VkCommandBuffer cb;
   VkFence fence;
   unsigned nverts, p2_first;
};

static int
alloc_mem(VkDeviceMemory *out, VkMemoryRequirements mr, VkMemoryPropertyFlags want)
{
   int mt = memtype(mr.memoryTypeBits, want);
   if (mt < 0)
      mt = memtype(mr.memoryTypeBits, 0);
   if (mt < 0)
      return 0;
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = (uint32_t)mt};
   return AllocateMemory(dev, &ai, NULL, out) == VK_SUCCESS;
}

static VkImage
make_image(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageTiling tiling,
           VkImageLayout init, VkDeviceMemory *mem)
{
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                           .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = tiling, .usage = usage,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = init};
   VkImage img = VK_NULL_HANDLE;
   if (CreateImage(dev, &ii, NULL, &img) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, img, &mr);
   if (!alloc_mem(mem, mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || BindImageMemory(dev, img, *mem, 0) != VK_SUCCESS) {
      DestroyImage(dev, img, NULL);
      return VK_NULL_HANDLE;
   }
   return img;
}

static VkBuffer
make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *mem, void **map)
{
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer b = VK_NULL_HANDLE;
   if (CreateBuffer(dev, &bi, NULL, &b) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, b, &mr);
   if (!alloc_mem(mem, mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
       BindBufferMemory(dev, b, *mem, 0) != VK_SUCCESS) {
      DestroyBuffer(dev, b, NULL);
      return VK_NULL_HANDLE;
   }
   if (map && MapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   return b;
}

static void
barrier_image(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src,
              VkPipelineStageFlags dst, VkAccessFlags sa, VkAccessFlags da)
{
   VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                             .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                             .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CmdPipelineBarrier(cb, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

static int
ref_init(struct ref *r)
{
   memset(r, 0, sizeof *r);
   r->col = make_image(VK_FORMAT_R8G8B8A8_UNORM, REF_W, REF_H,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_LAYOUT_UNDEFINED, &r->colm);
   CHECK(r->col, "colour image (OPTIMAL, RGBA8, transfer src)");
   r->dep = make_image(VK_FORMAT_D32_SFLOAT, REF_W, REF_H, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_LAYOUT_UNDEFINED, &r->depm);
   CHECK(r->dep, "depth image (OPTIMAL, D32_SFLOAT)");
   r->tex = make_image(VK_FORMAT_R8G8B8A8_UNORM, REF_TEX, REF_TEX,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL,
                       VK_IMAGE_LAYOUT_UNDEFINED, &r->texm);
   CHECK(r->tex, "texture image (OPTIMAL, RGBA8, sampled)");

   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   vi.image = r->col;
   CHECK(CreateImageView(dev, &vi, NULL, &r->colv) == VK_SUCCESS, "colour view");
   vi.image = r->tex;
   CHECK(CreateImageView(dev, &vi, NULL, &r->texv) == VK_SUCCESS, "texture view");
   VkImageViewCreateInfo dv = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = r->dep,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
                               .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
   CHECK(CreateImageView(dev, &dv, NULL, &r->depv) == VK_SUCCESS, "depth view");

   struct ref_vertex verts[REF_MAX_VERTS];
   r->nverts = ref_build_vertices(verts, &r->p2_first);
   void *vmap = NULL;
   r->vb = make_buffer(sizeof verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &r->vbm, &vmap);
   CHECK(r->vb && vmap, "vertex buffer");
   memcpy(vmap, verts, sizeof verts);
   UnmapMemory(dev, r->vbm);

   uint8_t *texels = malloc((size_t)REF_TEX * REF_TEX * 4);
   CHECK(texels, "texture staging");
   ref_texture(texels);
   void *tmap = NULL;
   r->texb = make_buffer((VkDeviceSize)REF_TEX * REF_TEX * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &r->texbm, &tmap);
   CHECK(r->texb && tmap, "texture staging buffer");
   memcpy(tmap, texels, (size_t)REF_TEX * REF_TEX * 4);
   UnmapMemory(dev, r->texbm);
   free(texels);

   void *rmap = NULL;
   r->rdb = make_buffer((VkDeviceSize)REF_W * REF_H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &r->rdbm, &rmap);
   CHECK(r->rdb && rmap, "readback buffer");
   r->rd = rmap;

   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                             .magFilter = g_nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
                             .minFilter = g_nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                             .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                             .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                             .maxLod = 0.0f};
   CHECK(CreateSampler(dev, &si, NULL, &r->smp) == VK_SUCCESS, "sampler");

   VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1, .pBindings = &lb};
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &r->dsl) == VK_SUCCESS, "descriptor set layout");
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                     .poolSizeCount = 1, .pPoolSizes = &ps};
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &r->dpool) == VK_SUCCESS, "descriptor pool");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = r->dpool, .descriptorSetCount = 1, .pSetLayouts = &r->dsl};
   CHECK(AllocateDescriptorSets(dev, &dai, &r->dset) == VK_SUCCESS, "descriptor set");
   VkDescriptorImageInfo dii = {.sampler = r->smp, .imageView = r->texv,
                                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = r->dset, .dstBinding = 0,
                             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             .pImageInfo = &dii};
   UpdateDescriptorSets(dev, 1, &w, 0, NULL);

   VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &r->dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &r->pl) == VK_SUCCESS, "pipeline layout");

   VkAttachmentDescription ad[2] = {
      {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
   };
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                              .pColorAttachments = &cref, .pDepthStencilAttachment = &dref};
   /* Order the attachment writes against the next pass's LOAD and the copy. */
   VkSubpassDependency dep[2] = {
      {VK_SUBPASS_EXTERNAL, 0,
       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0},
      {0, VK_SUBPASS_EXTERNAL,
       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
         VK_PIPELINE_STAGE_TRANSFER_BIT,
       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
         VK_ACCESS_TRANSFER_READ_BIT, 0},
   };
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2,
                                 .pAttachments = ad, .subpassCount = 1, .pSubpasses = &sp,
                                 .dependencyCount = 2, .pDependencies = dep};
   CHECK(CreateRenderPass(dev, &rpi, NULL, &r->rp1) == VK_SUCCESS, "render pass (clear)");
   ad[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
   ad[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   ad[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   ad[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
   ad[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
   CHECK(CreateRenderPass(dev, &rpi, NULL, &r->rp2) == VK_SUCCESS, "render pass (load)");

   VkImageView atts[2] = {r->colv, r->depv};
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = r->rp1,
                                  .attachmentCount = 2, .pAttachments = atts, .width = REF_W, .height = REF_H,
                                  .layers = 1};
   CHECK(CreateFramebuffer(dev, &fbi, NULL, &r->fb) == VK_SUCCESS, "framebuffer");

   VkShaderModule vs = load_spv("ref.vert.spv"), fs = load_spv("ref.frag.spv");
   CHECK(vs && fs, "shader modules (--spv-dir)");
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = fs, .pName = "main"},
   };
   VkVertexInputBindingDescription vbd = {.binding = 0, .stride = sizeof(struct ref_vertex),
                                          .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription vad[3] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(struct ref_vertex, pos)},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(struct ref_vertex, uv)},
      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct ref_vertex, col)},
   };
   VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbd,
                                               .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = vad};
   VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                           .viewportCount = 1, .scissorCount = 1};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                              .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                               .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                               .depthCompareOp = VK_COMPARE_OP_LESS};
   VkPipelineColorBlendAttachmentState cba = {.blendEnable = VK_TRUE,
                                              .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                                              .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              .colorBlendOp = VK_BLEND_OP_ADD,
                                              .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                              .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              .alphaBlendOp = VK_BLEND_OP_ADD,
                                              .colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                             .attachmentCount = 1, .pAttachments = &cba};
   VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo dyni = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 2, .pDynamicStates = dyn};
   VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                      .pStages = st, .pVertexInputState = &vin, .pInputAssemblyState = &ia,
                                      .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                      .pDepthStencilState = &ds, .pColorBlendState = &cb,
                                      .pDynamicState = &dyni, .layout = r->pl, .renderPass = r->rp1};
   CHECK(CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &r->pipe) == VK_SUCCESS, "graphics pipeline");
   DestroyShaderModule(dev, vs, NULL);
   DestroyShaderModule(dev, fs, NULL);

   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = 0};
   CHECK(CreateCommandPool(dev, &cpi, NULL, &r->cpool) == VK_SUCCESS, "command pool");
   VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = r->cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
   CHECK(AllocateCommandBuffers(dev, &cbi, &r->cb) == VK_SUCCESS, "command buffer");
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &r->fence) == VK_SUCCESS, "fence");
   return 1;
}

static int
ref_frame(struct ref *r, int upload)
{
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   if (BeginCommandBuffer(r->cb, &bi) != VK_SUCCESS)
      return 0;
   if (upload) {
      barrier_image(r->cb, r->tex, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
      VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             .imageExtent = {REF_TEX, REF_TEX, 1}};
      CmdCopyBufferToImage(r->cb, r->texb, r->tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
      barrier_image(r->cb, r->tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
   }

   VkViewport vp = {0, 0, REF_W, REF_H, 0.0f, 1.0f};
   VkRect2D sc = {{0, 0}, {REF_W, REF_H}};
   CmdSetViewport(r->cb, 0, 1, &vp);
   CmdSetScissor(r->cb, 0, 1, &sc);

   VkClearValue cv[2];
   cv[0].color.float32[0] = REF_CLEAR_R;
   cv[0].color.float32[1] = REF_CLEAR_G;
   cv[0].color.float32[2] = REF_CLEAR_B;
   cv[0].color.float32[3] = REF_CLEAR_A;
   cv[1].depthStencil.depth = 1.0f;
   cv[1].depthStencil.stencil = 0;
   VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = r->rp1,
                                .framebuffer = r->fb, .renderArea = sc, .clearValueCount = 2, .pClearValues = cv};
   CmdBeginRenderPass(r->cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(r->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe);
   CmdBindDescriptorSets(r->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pl, 0, 1, &r->dset, 0, NULL);
   VkDeviceSize off = 0;
   CmdBindVertexBuffers(r->cb, 0, 1, &r->vb, &off);
   for (unsigned i = 0; i < sizeof ref_draws_p1 / sizeof *ref_draws_p1; i++) {
      const struct ref_draw *d = &ref_draws_p1[i];
      float pc[4] = {d->z, d->alpha, d->tex, 0.0f};
      CmdPushConstants(r->cb, r->pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
      CmdDraw(r->cb, d->count, 1, d->first, 0);
   }
   CmdEndRenderPass(r->cb);

   rbi.renderPass = r->rp2;
   CmdBeginRenderPass(r->cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(r->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe);
   CmdBindDescriptorSets(r->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pl, 0, 1, &r->dset, 0, NULL);
   CmdBindVertexBuffers(r->cb, 0, 1, &r->vb, &off);
   float pc[4] = {REF_P2_Z, 1.0f, 1.0f, 0.0f};
   CmdPushConstants(r->cb, r->pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
   CmdDraw(r->cb, 6, 1, r->p2_first, 0);
   CmdEndRenderPass(r->cb);

   VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .imageExtent = {REF_W, REF_H, 1}};
   CmdCopyImageToBuffer(r->cb, r->col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->rdb, 1, &c);
   if (EndCommandBuffer(r->cb) != VK_SUCCESS)
      return 0;

   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &r->cb};
   if (QueueSubmit(queue, 1, &si, r->fence) != VK_SUCCESS)
      return 0;
   if (WaitForFences(dev, 1, &r->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
      return 0;
   ResetFences(dev, 1, &r->fence);
   return 1;
}

static void
write_ppm(const char *path, const uint8_t *px)
{
   FILE *f = fopen(path, "wb");
   if (!f) {
      printf("{\"fail\":\"open %s for writing\"}\n", path);
      g_fail = 1;
      return;
   }
   fprintf(f, "P6\n%u %u\n255\n", REF_W, REF_H);
   for (unsigned y = 0; y < REF_H; y++)
      for (unsigned x = 0; x < REF_W; x++)
         fwrite(px + (y * REF_W + x) * 4, 1, 3, f); /* drop alpha: P6 is RGB */
   fclose(f);
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc)
         icd = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc)
         g_spv = argv[++i];
      else if (!strcmp(argv[i], "--out") && i + 1 < argc)
         g_out = argv[++i];
      else if (!strcmp(argv[i], "--json") && i + 1 < argc)
         g_json = argv[++i];
      else if (!strcmp(argv[i], "--loop") && i + 1 < argc)
         g_loop = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--nearest"))
         g_nearest = 1;
      else {
         fprintf(stderr,
                 "usage: t_vk_ref [--direct icd.so] [--spv-dir dir] [--out file.ppm] [--json file] [--loop N] "
                 "[--nearest]\n");
         return 2;
      }
   }
   const char *be = getenv("MALI_BACKEND");
   g_fake = be && !strcmp(be, "fake");

   void *lib = dlopen(icd ? icd : "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      printf("{\"fail\":\"dlopen: %s\"}\n", dlerror());
      return 1;
   }
   PFN_vkGetInstanceProcAddr gipa;
   if (icd) {
      gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
      VkResult (*negotiate)(uint32_t *) =
         (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
      uint32_t iv = 5;
      if (!gipa || !negotiate || negotiate(&iv)) {
         printf("{\"fail\":\"not an ICD: %s\"}\n", icd);
         return 1;
      }
   } else {
      gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
      if (!gipa) {
         printf("{\"fail\":\"no vkGetInstanceProcAddr\"}\n");
         return 1;
      }
   }
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
      printf("{\"fail\":\"vkCreateInstance\"}\n");
      return 1;
   }
#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices);
   I(GetPhysicalDeviceProperties);
   I(GetPhysicalDeviceMemoryProperties);
   I(GetPhysicalDeviceQueueFamilyProperties);
   I(CreateDevice);
   I(GetDeviceProcAddr);
#undef I
   uint32_t n = 1;
   VkResult r = EnumeratePhysicalDevices(inst, &n, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n < 1) {
      printf("{\"fail\":\"no physical device\"}\n");
      return 1;
   }
   VkPhysicalDeviceProperties pp;
   GetPhysicalDeviceProperties(pd, &pp);
   GetPhysicalDeviceMemoryProperties(pd, &memprops);

   uint32_t nqf = 0;
   GetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
   VkQueueFamilyProperties qf[16];
   if (nqf > 16)
      nqf = 16;
   GetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qf);
   uint32_t qfi = UINT32_MAX;
   for (uint32_t i = 0; i < nqf; i++)
      if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qfi == UINT32_MAX)
         qfi = i;
   if (qfi == UINT32_MAX) {
      printf("{\"fail\":\"no graphics queue\"}\n");
      return 1;
   }
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi,
                                 .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi};
   if (CreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) {
      printf("{\"fail\":\"vkCreateDevice\"}\n");
      return 1;
   }
   PFN_vkGetDeviceProcAddr gdpa = GetDeviceProcAddr;
#define D(n) n = (PFN_vk##n)gdpa(dev, "vk" #n); (void)n;
   DEV_FNS(D)
#undef D
   GetDeviceQueue(dev, qfi, 0, &queue);

   struct ref sc;
   int ok = ref_init(&sc);
   double t0 = 0.0, t_render = 0.0;
   for (int i = 0; ok && i < g_loop; i++) {
      t0 = now_us();
      ok = ref_frame(&sc, i == 0);
      t_render += now_us() - t0;
   }
   DeviceWaitIdle(dev);

   /* per-64-row-block digests: the AFBC row-block shape of the comparison, usable from a farm JSON without an image */
   char digests[REF_NBLOCK][24];
   uint64_t total = 0;
   for (unsigned b = 0; b < REF_NBLOCK && ok; b++) {
      uint64_t h = ref_block_digest(sc.rd, REF_W * 4, b);
      snprintf(digests[b], sizeof digests[b], "0x%016llx", (unsigned long long)h);
      total ^= h;
   }
   if (ok && g_out && !g_fake)
      write_ppm(g_out, sc.rd);
   if (g_fake) {
      printf("{\"scene\":\"ref-v1\",\"device\":\"%s\",\"skipped\":\"fake backend executes nothing\",\"ok\":true}\n",
             pp.deviceName);
   } else if (ok) {
      printf("{\"scene\":\"ref-v1\",\"device\":\"%s\",\"size\":[%u,%u],\"frames\":%d,\"frame_us\":%.1f,"
             "\"blocks\":[",
             pp.deviceName, REF_W, REF_H, g_loop, t_render / (g_loop ? g_loop : 1));
      for (unsigned b = 0; b < REF_NBLOCK; b++)
         printf("%s\"%s\"", b ? "," : "", digests[b]);
      printf("],\"digest\":\"0x%016llx\",\"ok\":true}\n", (unsigned long long)total);
   }
   int rc = (ok && !g_fail) ? 0 : 1;
   if (g_json) {
      FILE *f = fopen(g_json, "w");
      if (f) {
         fprintf(f, "{\"scene\":\"ref-v1\",\"device\":\"%s\",\"size\":[%u,%u],\"blocks\":[", pp.deviceName, REF_W, REF_H);
         for (unsigned b = 0; b < REF_NBLOCK; b++)
            fprintf(f, "%s\"%s\"", b ? "," : "", digests[b]);
         fprintf(f, "],\"digest\":\"0x%016llx\"}\n", (unsigned long long)total);
         fclose(f);
      }
   }
   return rc;
}
