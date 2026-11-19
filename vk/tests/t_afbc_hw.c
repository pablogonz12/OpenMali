/* SPDX-License-Identifier: MIT */
/* AFBC stage-5 hardware test (first phone test). Standalone Vulkan app, loader-free like t_vk_draw: dlopens the ICD.
 *   t_afbc_hw --direct <icd.so> [--spv-dir dir] [--case clear|draw|sample]      run with MALI_AFBC=1 for the AFBC path
 * It renders into a 128x128 RGBA8 colour attachment: a clear (case clear), then a clear + one triangle draw (case draw).
 *  MALI_AFBC unset (reference): the image is TILING_LINEAR + TRANSFER_SRC; the pixels are read back and compared (clear colour at a corner,
 *    triangle colour near the centre). This proves the test itself and the default path.
 *  MALI_AFBC=1: the image is OPTIMAL + COLOR_ATTACHMENT only (render-only AFBC eligible). The GPU result cannot be decoded yet (no readback
 *    path: see docs/afbc-plan.md stage 6), so the PASS criteria are what the host can see without decoding:
 *      - the submit completes (fence signalled, no DEVICE_LOST / fault),
 *      - the headers of every superblock inside the image were rewritten by the GPU (the driver zeroed them at bind time),
 *      - the guard bytes after the image (sentinel 0xA5) are intact (no write past the AFBC allocation),
 *      - the headers differ between the clear-only and the draw case (the triangle really changed the compressed image).
 *    The first headers are printed as hex so stage 6 can build the decoder from real data.
 *  MALI_BACKEND=fake (host, check.sh): nothing renders. Reference mode only checks the frame completes; AFBC mode requires the opposite of the
 *    hardware criteria: every header is still ZERO (written by BindImageMemory2 and by nothing else), the image is at least the AFBC size, guard intact.
 *    This covers the bind-time hook through the real ICD (the one-line hook is not covered by t_image).
 * Output: one JSON line per step, final line {"afbc_hw":"PASS"|"FAIL"}. Exit 0 = PASS, 1 = FAIL, 2 = usage. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
#include "../src/mali_afbc.h"

#define IW 128
#define IH 128
#define GUARD 4096u

#define FAIL(...) do { printf("{\"fail\":\""); printf(__VA_ARGS__); printf("\",\"line\":%d}\n", __LINE__); return 0; } while (0)
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) FAIL("%s = %d", #x, (int)r_); } while (0)

#define DEV_FNS(X) \
   X(GetDeviceQueue) X(AllocateMemory) X(MapMemory) X(CreateFence) X(WaitForFences) X(QueueSubmit) X(CreateImage) \
   X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) X(GetImageSubresourceLayout) X(CreateBuffer) \
   X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateShaderModule) X(CreatePipelineLayout) X(CreateRenderPass) \
   X(CreateFramebuffer) X(CreateGraphicsPipelines) X(CreateCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) \
   X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdBindVertexBuffers) X(CmdPushConstants) \
   X(CmdDraw) X(CreateSampler) X(CreateDescriptorSetLayout) X(CreateDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
   X(CmdBindDescriptorSets)
#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)

static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;
static const char *g_spv = "shader";
static int g_fake;   /* MALI_BACKEND=fake: no GPU runs, so the frame leaves memory untouched: only the bind-time part is checked */

static int
memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return (int)i;
   return -1;
}

static VkShaderModule
load_spv(const char *name)
{
   char path[512];
   snprintf(path, sizeof path, "%s/%s", g_spv, name);
   FILE *f = fopen(path, "rb");
   if (!f) { printf("{\"fail\":\"open %s\"}\n", path); return VK_NULL_HANDLE; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *buf = malloc((size_t)n);
   if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return VK_NULL_HANDLE; }
   fclose(f);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = buf};
   VkShaderModule m = VK_NULL_HANDLE;
   if (CreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
   free(buf);
   return m;
}

struct run;
static int sample_phase(struct run *r, VkImageView src);

struct run {
   int afbc;                 /* MALI_AFBC=1 */
   int draw;                 /* clear + triangle, else clear only */
   int sample;               /* case sample: the image is also SAMPLED, and a second pass draws it 1:1 into a linear target (tmap) */
   uint8_t *tmap;            /* sample case: target pixels (linear) */
   uint32_t tpitch;
   uint8_t *map;             /* whole memory, mapped */
   uint64_t alloc, img_size; /* allocation size, size the driver reports for the image */
   uint32_t pitch;           /* reference mode only */
   uint8_t *hdr;             /* copy of the headers after the frame (AFBC mode) */
   uint32_t nhdr, nonzero;
};

/* Sample pass: draw `src` (the frame the first pass rendered, GENERAL layout) 1:1 into a linear 128x128 target with a nearest sampler and
 * the tex shaders. With AFBC this reads the image through the texture AFBC Plane descriptor, so the target pixels ARE the decode check. */
static int
sample_phase(struct run *r, VkImageView src)
{
   VkFence fence;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CK(CreateFence(dev, &fi, NULL, &fence));
   VkImageCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                           .extent = {IW, IH, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                           .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage timg;
   CK(CreateImage(dev, &ti, NULL, &timg));
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, timg, &mr);
   int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   if (mt < 0) FAIL("no host visible memory type for the target");
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt};
   VkDeviceMemory tmem;
   CK(AllocateMemory(dev, &ai, NULL, &tmem));
   uint8_t *tp;
   CK(MapMemory(dev, tmem, 0, VK_WHOLE_SIZE, 0, (void **)&tp));
   memset(tp, 0xA5, mr.size);
   CK(BindImageMemory(dev, timg, tmem, 0));
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = timg, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView tview;
   CK(CreateImageView(dev, &vi, NULL, &tview));
   VkImageSubresource sr = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubresourceLayout sl;
   GetImageSubresourceLayout(dev, timg, &sr, &sl);
   r->tpitch = (uint32_t)sl.rowPitch;
   r->tmap = tp + sl.offset;

   VkAttachmentDescription ad = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
                                 .subpassCount = 1, .pSubpasses = &sp};
   VkRenderPass rp;
   CK(CreateRenderPass(dev, &rpi, NULL, &rp));
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
                                  .pAttachments = &tview, .width = IW, .height = IH, .layers = 1};
   VkFramebuffer fb;
   CK(CreateFramebuffer(dev, &fbi, NULL, &fb));

   VkSamplerCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
   VkSampler smp;
   CK(CreateSampler(dev, &smi, NULL, &smp));
   VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
                                      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb};
   VkDescriptorSetLayout dsl;
   CK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool dpool;
   CK(CreateDescriptorPool(dev, &dpi, NULL, &dpool));
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1,
                                      .pSetLayouts = &dsl};
   VkDescriptorSet dset;
   CK(AllocateDescriptorSets(dev, &dai, &dset));
   VkDescriptorImageInfo dii = {smp, src, VK_IMAGE_LAYOUT_GENERAL};
   VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii};
   UpdateDescriptorSets(dev, 1, &wr, 0, NULL);

   VkShaderModule vs = load_spv("tex.vert.spv"), fs = load_spv("tex.frag.spv");
   if (!vs || !fs) FAIL("tex shader modules (use --spv-dir)");
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .size = 16};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   VkPipelineLayout layout;
   CK(CreatePipelineLayout(dev, &pli, NULL, &layout));
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
   VkVertexInputBindingDescription vbd[2] = {{0, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 16, VK_VERTEX_INPUT_RATE_VERTEX}};
   VkVertexInputAttributeDescription vad[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};
   VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 2,
                                               .pVertexBindingDescriptions = vbd, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = vad};
   VkPipelineInputAssemblyStateCreateInfo ias = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, IW, IH, 0, 1};
   VkRect2D sc = {{0, 0}, {IW, IH}};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp,
                                            .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
                                                .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
   VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vis,
                                      .pInputAssemblyState = &ias, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                      .pColorBlendState = &cbs, .layout = layout, .renderPass = rp};
   VkPipeline pipe;
   CK(CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
   /* full-screen quad, 6 vertices: positions at 0, uv (= colour attribute xy) at 128 */
   static const float pos[24] = {-1, -1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, -1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1};
   static const float uv[24] = {0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1, 0, 1};
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
   VkBuffer vb;
   CK(CreateBuffer(dev, &bi, NULL, &vb));
   VkMemoryRequirements bmr;
   GetBufferMemoryRequirements(dev, vb, &bmr);
   VkMemoryAllocateInfo ba = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.size,
                              .memoryTypeIndex = (uint32_t)memtype(bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   VkDeviceMemory bm;
   CK(AllocateMemory(dev, &ba, NULL, &bm));
   CK(BindBufferMemory(dev, vb, bm, 0));
   uint8_t *bp;
   CK(MapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, (void **)&bp));
   memcpy(bp, pos, sizeof pos);
   memcpy(bp + 128, uv, sizeof uv);

   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool pool;
   CK(CreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cb;
   CK(AllocateCommandBuffers(dev, &cai, &cb));
   VkCommandBufferBeginInfo bgi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CK(BeginCommandBuffer(cb, &bgi));
   VkClearValue cv = {.color = {{0, 0, 0, 0}}};
   VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                               .renderArea = {{0, 0}, {IW, IH}}, .clearValueCount = 1, .pClearValues = &cv};
   CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   VkBuffer vbs[2] = {vb, vb};
   VkDeviceSize offs[2] = {0, 128};
   CmdBindVertexBuffers(cb, 0, 2, vbs, offs);
   const float rot[4] = {1, 0, 0, 0};
   CmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &dset, 0, NULL);
   CmdDraw(cb, 6, 1, 0, 0);
   CmdEndRenderPass(cb);
   CK(EndCommandBuffer(cb));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   CK(QueueSubmit(queue, 1, &si, fence));
   VkResult w = WaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
   if (w != VK_SUCCESS) FAIL("sample pass WaitForFences = %d (hang or DEVICE_LOST)", (int)w);
   return 1;
}

/* one frame; fills r. Everything is created per run so the two cases are independent. */
static int
run_case(struct run *r)
{
   VkFence fence;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CK(CreateFence(dev, &fi, NULL, &fence));
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                           .extent = {IW, IH, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                           .tiling = r->afbc ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_LINEAR,
                           .usage = (r->afbc ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                             : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) |
                                    (r->sample ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage img;
   CK(CreateImage(dev, &ii, NULL, &img));
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, img, &mr);
   int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   if (mt < 0) FAIL("no host visible memory type for the image");
   r->img_size = mr.size;
   r->alloc = mr.size + GUARD;
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r->alloc, .memoryTypeIndex = (uint32_t)mt};
   VkDeviceMemory mem;
   CK(AllocateMemory(dev, &ai, NULL, &mem));
   CK(MapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&r->map));
   memset(r->map, 0xA5, r->alloc);   /* sentinel everywhere: the driver must overwrite only what it owns; AFBC headers are zeroed at bind */
   CK(BindImageMemory(dev, img, mem, 0));
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView view;
   CK(CreateImageView(dev, &vi, NULL, &view));
   if (!r->afbc) {
      VkImageSubresource sr = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
      VkSubresourceLayout sl;
      GetImageSubresourceLayout(dev, img, &sr, &sl);
      r->pitch = (uint32_t)sl.rowPitch;
      r->map += sl.offset;
   }

   VkAttachmentDescription ad = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
                                 .subpassCount = 1, .pSubpasses = &sp};
   VkRenderPass rp;
   CK(CreateRenderPass(dev, &rpi, NULL, &rp));
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
                                  .pAttachments = &view, .width = IW, .height = IH, .layers = 1};
   VkFramebuffer fb;
   CK(CreateFramebuffer(dev, &fbi, NULL, &fb));

   VkPipeline pipe = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkBuffer vb[2] = {0, 0};
   if (r->draw) {
      VkShaderModule vs = load_spv("rot.vert.spv"), fs = load_spv("var.frag.spv");
      if (!vs || !fs) FAIL("shader modules (use --spv-dir)");
      VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .size = 16};
      VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
      CK(CreatePipelineLayout(dev, &pli, NULL, &layout));
      VkPipelineShaderStageCreateInfo st[2] = {
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
      VkVertexInputBindingDescription vbd[2] = {{0, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 16, VK_VERTEX_INPUT_RATE_VERTEX}};
      VkVertexInputAttributeDescription vad[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};
      VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                  .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = vbd,
                                                  .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = vad};
      VkPipelineInputAssemblyStateCreateInfo ias = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
      VkViewport vp = {0, 0, IW, IH, 0, 1};
      VkRect2D sc = {{0, 0}, {IW, IH}};
      VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp,
                                               .scissorCount = 1, .pScissors = &sc};
      VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                   .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
      VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                 .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
      VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
      VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
      VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                         .pVertexInputState = &vis, .pInputAssemblyState = &ias, .pViewportState = &vps,
                                         .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs,
                                         .layout = layout, .renderPass = rp};
      CK(CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
      /* one buffer holds both streams: positions at 0, colours at 64 */
      static const float pos[12] = {-.5f, -.5f, 0, 1, .5f, -.5f, 0, 1, 0, .5f, 0, 1};
      static const float col[12] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};   /* solid red triangle */
      VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
      CK(CreateBuffer(dev, &bi, NULL, &vb[0]));
      VkMemoryRequirements bmr;
      GetBufferMemoryRequirements(dev, vb[0], &bmr);
      VkMemoryAllocateInfo ba = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.size,
                                 .memoryTypeIndex = (uint32_t)memtype(bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
      VkDeviceMemory bm;
      CK(AllocateMemory(dev, &ba, NULL, &bm));
      CK(BindBufferMemory(dev, vb[0], bm, 0));
      uint8_t *bp;
      CK(MapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, (void **)&bp));
      memcpy(bp, pos, sizeof pos);
      memcpy(bp + 64, col, sizeof col);
      vb[1] = vb[0];
   }

   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool pool;
   CK(CreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cb;
   CK(AllocateCommandBuffers(dev, &cai, &cb));
   VkCommandBufferBeginInfo bgi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CK(BeginCommandBuffer(cb, &bgi));
   VkClearValue cv = {.color = {{0.25f, 0.5f, 0.75f, 1.0f}}};
   VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                               .renderArea = {{0, 0}, {IW, IH}}, .clearValueCount = 1, .pClearValues = &cv};
   CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
   if (r->draw) {
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      VkBuffer vbs[2] = {vb[0], vb[1]};
      VkDeviceSize offs[2] = {0, 64};
      CmdBindVertexBuffers(cb, 0, 2, vbs, offs);
      const float rot[4] = {1, 0, 0, 0};
      CmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot);
      CmdDraw(cb, 3, 1, 0, 0);
   }
   CmdEndRenderPass(cb);
   CK(EndCommandBuffer(cb));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   CK(QueueSubmit(queue, 1, &si, fence));
   VkResult w = WaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
   if (w != VK_SUCCESS) FAIL("WaitForFences = %d (hang or DEVICE_LOST)", (int)w);
   if (r->sample) return sample_phase(r, view);
   return 1;
}

static int
check_linear(const struct run *r)
{
   if (g_fake) return 1;
   const uint8_t *c = r->map + 4 * 4 + 4 * r->pitch;                 /* (4,4): clear colour */
   const uint8_t *t = r->map + (IW / 2) * 4 + (IH / 2) * r->pitch;   /* centre: inside the triangle */
   const uint8_t want_c[4] = {64, 128, 191, 255};                    /* 0.25 0.5 0.75 1 */
   for (int i = 0; i < 4; i++)
      if (abs(c[i] - want_c[i]) > 1) FAIL("clear pixel %d,%d,%d,%d", c[0], c[1], c[2], c[3]);
   if (r->draw && !(t[0] > 250 && t[1] < 4 && t[2] < 4)) FAIL("centre pixel %d,%d,%d,%d is not the triangle red", t[0], t[1], t[2], t[3]);
   if (!r->draw && abs(t[0] - 64) > 1) FAIL("clear-only centre pixel %d", t[0]);
   return 1;
}

/* The sampled copy of the frame must be exactly the clear colour or the triangle red everywhere, and the triangle (base 64 x height 64 px = 2048 px) present. */
static int
check_sample(const struct run *r)
{
   if (g_fake) return 1;
   const uint8_t wc[4] = {64, 128, 191, 255};
   unsigned nc = 0, nr = 0, bad = 0;
   for (int y = 0; y < IH; y++)
      for (int x = 0; x < IW; x++) {
         const uint8_t *p = r->tmap + (size_t)y * r->tpitch + (size_t)x * 4;
         if (abs(p[0] - wc[0]) <= 1 && abs(p[1] - wc[1]) <= 1 && abs(p[2] - wc[2]) <= 1 && p[3] == 255) nc++;
         else if (p[0] > 250 && p[1] < 4 && p[2] < 4 && p[3] == 255) nr++;
         else if (bad++ < 4) printf("{\"bad_pixel\":[%d,%d],\"rgba\":[%d,%d,%d,%d]}\n", x, y, p[0], p[1], p[2], p[3]);
      }
   printf("{\"sample\":{\"clear\":%u,\"red\":%u,\"other\":%u}}\n", nc, nr, bad);
   if (bad) FAIL("sampled image has %u pixels that are neither clear nor triangle colour (decode wrong)", bad);
   if (nr < 1900 || nr > 2200) FAIL("triangle pixel count %u, want about 2048", nr);
   return 1;
}

/* AFBC mode: what the host can verify without a decoder. The layout is the driver's own (same pure header, 16x16 superblocks, 32 bpp, arch 13). */
static int
check_afbc(struct run *r, const char *name)
{
   struct mali_afbc_image_layout al;
   if (mali_afbc_image_layout(13, MALI_AFBC_SB_16x16, 32, IW, IH, 1, 1, &al)) FAIL("host layout");
   if (r->img_size < al.size) FAIL("driver image size %llu < AFBC layout %llu (image is not AFBC)", (unsigned long long)r->img_size, (unsigned long long)al.size);
   const struct mali_afbc_layout *L = &al.level[0];
   const uint32_t sx = IW / L->sb_w, sy = IH / L->sb_h;
   r->nhdr = sx * sy;
   r->hdr = malloc((size_t)r->nhdr * 16);
   r->nonzero = 0;
   uint32_t inrange = 0;
   for (uint32_t y = 0; y < sy; y++)
      for (uint32_t x = 0; x < sx; x++) {
         const uint8_t *h = r->map + al.level_offset[0] + (uint64_t)y * L->header_row_stride + (uint64_t)x * 16;
         memcpy(r->hdr + (size_t)(y * sx + x) * 16, h, 16);
         int nz = 0;
         for (int i = 0; i < 16; i++) nz |= h[i];
         r->nonzero += nz != 0;
         uint32_t bp;
         memcpy(&bp, h, 4);
         inrange += bp < r->alloc;   /* informational: the body pointer layout is verified against real data by stage 6 */
      }
   printf("{\"case\":\"%s\",\"image_bytes\":%llu,\"header_bytes\":%llu,\"headers\":%u,\"nonzero\":%u,\"word0_below_alloc\":%u,\"hdr0\":\"", name,
          (unsigned long long)r->img_size, (unsigned long long)L->header_size, r->nhdr, r->nonzero, inrange);
   for (int i = 0; i < 16; i++) printf("%02x", r->hdr[i]);
   printf("\",\"hdr_mid\":\"");
   for (int i = 0; i < 16; i++) printf("%02x", r->hdr[(size_t)(r->nhdr / 2 + sx / 2) * 16 + i]);
   printf("\"}\n");
   if (g_fake) {
      if (r->nonzero) FAIL("%s: fake backend but %u headers non-zero (bind-time init did not zero them)", name, r->nonzero);
   } else if (r->nonzero != r->nhdr) FAIL("%s: %u of %u superblock headers still zero: the GPU did not write them", name, r->nhdr - r->nonzero, r->nhdr);
   for (uint64_t i = r->img_size; i < r->alloc; i++)
      if (r->map[i] != 0xA5) FAIL("%s: guard byte %llu past the image overwritten (0x%02x)", name, (unsigned long long)(i - r->img_size), r->map[i]);
   return 1;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL, *only = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else if (!strcmp(argv[i], "--case") && i + 1 < argc) only = argv[++i];
      else { fprintf(stderr, "usage: t_afbc_hw --direct icd.so [--spv-dir dir] [--case clear|draw|sample]\n"); return 2; }
   }
   if (!icd) { fprintf(stderr, "--direct required\n"); return 2; }
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("{\"fail\":\"dlopen: %s\"}\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { printf("{\"fail\":\"no vk_icdGetInstanceProcAddr\"}\n"); return 1; }
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   if (!CreateInstance || CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices EPD = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceMemoryProperties GMP = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkGetPhysicalDeviceProperties GPP = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
   PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr GDPA = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   VkResult er = EPD(inst, &cnt, &pd);
   if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || cnt < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   VkPhysicalDeviceProperties pp;
   GPP(pd, &pp);
   GMP(pd, &memprops);
   const char *be = getenv("MALI_BACKEND");
   g_fake = be && !strcmp(be, "fake");
   const char *env = getenv("MALI_AFBC");
   const int afbc = env && env[0] == '1' && !env[1];
   printf("{\"device\":\"%s\",\"mali_afbc\":%d}\n", pp.deviceName, afbc);
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GDPA(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   DEV_FNS(LOAD)
   GetDeviceQueue(dev, 0, 0, &queue);

   int ok = 1;
   struct run rc = {.afbc = afbc, .draw = 0}, rd = {.afbc = afbc, .draw = 1};
   if (!only || !strcmp(only, "clear")) {
      ok &= run_case(&rc);
      if (ok) ok &= afbc ? check_afbc(&rc, "clear") : check_linear(&rc);
   }
   if (ok && (!only || !strcmp(only, "draw"))) {
      ok &= run_case(&rd);
      if (ok) ok &= afbc ? check_afbc(&rd, "draw") : check_linear(&rd);
   }
   if (ok && (!only || !strcmp(only, "sample"))) {
      struct run rs = {.afbc = afbc, .draw = 1, .sample = 1};
      ok &= run_case(&rs);
      if (ok) ok &= afbc ? check_afbc(&rs, "sample-src") : check_linear(&rs);
      if (ok) ok &= check_sample(&rs);
   }
   if (ok && afbc && !g_fake && rc.hdr && rd.hdr) {
      /* the triangle covers superblocks the clear-only frame left solid: some header must differ */
      int diff = memcmp(rc.hdr, rd.hdr, (size_t)rc.nhdr * 16) != 0;
      printf("{\"headers_differ_clear_vs_draw\":%s}\n", diff ? "true" : "false");
      if (!diff) { printf("{\"fail\":\"draw produced identical headers to the clear-only frame\"}\n"); ok = 0; }
   }
   printf("{\"afbc_hw\":\"%s\",\"mode\":\"%s\"}\n", ok ? "PASS" : "FAIL", afbc ? "afbc" : "linear-reference");
   return ok ? 0 : 1;
}
