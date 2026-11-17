/* SPDX-License-Identifier: MIT */
/* R4-B2: behaviour tests for the DXVK-required core features, one test per feature (the honesty rule: a feature is advertised only
 * with a passing test here or in t_vk_draw). Loader-free like t_vk_draw. One JSON line per case; exit 0 = all pass.
 *
 *   t_vk_feat --direct <icd.so> [--spv-dir dir] [--only firstinst|idx32|aniso|mirror|cubearray|wide|storfmt|bda|didx]
 *
 * A case whose feature is not advertised prints "skipped" (so it can be run against the driver before the bit is flipped:
 * the flip is made, the test run, and reverted if it fails). Pixels are checked only on hardware (fake backend renders nothing).
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 256
#define H 256

static int g_fail, g_fake;
static const char *g_only, *g_spv = "shader";
static VkPhysicalDeviceFeatures g_feat;
static VkPhysicalDeviceProperties g_props;
static PFN_vkGetImageSubresourceLayout g_gsl;
static PFN_vkResetFences g_rf;
static VkBool32 g_mirror, g_bda, g_didx;
static VkPrimitiveTopology g_topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
static float g_lw = 1.0f;

#define CASE_ON(n) (!g_only || !strcmp(g_only, n))
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("{\"fail\":\"%s:%d\",\"expr\":\"%s\",\"detail\":\"", __FILE__, __LINE__, #c);     \
         printf(__VA_ARGS__);                                                                      \
         printf("\"}\n");                                                                          \
         g_fail = 1;                                                                               \
         return 0;                                                                                 \
      }                                                                                            \
   } while (0)

#define DEV_FNS(X)                                                                                                    \
   X(DestroyDevice) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(CreateFence)    \
   X(DestroyFence) X(WaitForFences) X(QueueSubmit) X(DeviceWaitIdle) X(CreateImage) X(DestroyImage)                  \
   X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateBuffer)          \
   X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateShaderModule) X(DestroyShaderModule) \
   X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer)   \
   X(DestroyFramebuffer) X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool)   \
   X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass)    \
   X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor) X(CmdPushConstants) X(CmdDraw) X(CmdDrawIndexed)            \
   X(CmdBindIndexBuffer) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect) X(CreateDescriptorSetLayout)                  \
   X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets)         \
   X(UpdateDescriptorSets) X(CreateSampler) X(DestroySampler) X(CmdBindDescriptorSets) X(CmdCopyBufferToImage)      \
   X(CmdPipelineBarrier) X(CreateComputePipelines) X(CmdDispatch) X(CmdCopyImageToBuffer) X(GetBufferDeviceAddress)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;

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
   uint32_t *buf = malloc(n);
   if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return VK_NULL_HANDLE; }
   fclose(f);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = buf};
   VkShaderModule m = VK_NULL_HANDLE;
   if (CreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
   free(buf);
   return m;
}

/* host visible buffer with mapped pointer */
struct hbuf { VkBuffer b; VkDeviceMemory m; uint8_t *p; };

static int
hbuf_create(struct hbuf *h, VkDeviceSize size, VkBufferUsageFlags usage)
{
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
   CHECK(CreateBuffer(dev, &bi, NULL, &h->b) == VK_SUCCESS, "buffer");
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, h->b, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   CHECK(AllocateMemory(dev, &ai, NULL, &h->m) == VK_SUCCESS, "buffer memory");
   CHECK(BindBufferMemory(dev, h->b, h->m, 0) == VK_SUCCESS, "bind buffer");
   CHECK(MapMemory(dev, h->m, 0, VK_WHOLE_SIZE, 0, (void **)&h->p) == VK_SUCCESS, "map buffer");
   return 1;
}

static void
hbuf_destroy(struct hbuf *h)
{
   if (h->b) DestroyBuffer(dev, h->b, NULL);
   if (h->m) FreeMemory(dev, h->m, NULL);
}

/* Render target: linear RGBA8 256x256, host readable. */
struct tgt {
   VkImage img; VkDeviceMemory mem; VkImageView view; VkRenderPass rp; VkFramebuffer fb;
   uint8_t *px; uint32_t pitch;
   VkCommandPool pool; VkFence fence;
};

static int
tgt_create(struct tgt *t)
{
   memset(t, 0, sizeof *t);
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                           .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
                           .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   CHECK(CreateImage(dev, &ii, NULL, &t->img) == VK_SUCCESS, "colour image");
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, t->img, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   CHECK(AllocateMemory(dev, &ai, NULL, &t->mem) == VK_SUCCESS, "colour mem");
   CHECK(BindImageMemory(dev, t->img, t->mem, 0) == VK_SUCCESS, "bind colour");
   CHECK(MapMemory(dev, t->mem, 0, VK_WHOLE_SIZE, 0, (void **)&t->px) == VK_SUCCESS, "map colour");
   VkImageSubresource sr = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubresourceLayout sl;
   g_gsl(dev, t->img, &sr, &sl);
   t->pitch = (uint32_t)sl.rowPitch;
   t->px += sl.offset;
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CHECK(CreateImageView(dev, &vi, NULL, &t->view) == VK_SUCCESS, "colour view");
   VkAttachmentDescription ad = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
                                 .subpassCount = 1, .pSubpasses = &sp};
   CHECK(CreateRenderPass(dev, &rpi, NULL, &t->rp) == VK_SUCCESS, "render pass");
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = t->rp, .attachmentCount = 1,
                                  .pAttachments = &t->view, .width = W, .height = H, .layers = 1};
   CHECK(CreateFramebuffer(dev, &fbi, NULL, &t->fb) == VK_SUCCESS, "framebuffer");
   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CHECK(CreateCommandPool(dev, &pci, NULL, &t->pool) == VK_SUCCESS, "pool");
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &t->fence) == VK_SUCCESS, "fence");
   return 1;
}

static void
tgt_destroy(struct tgt *t)
{
   DeviceWaitIdle(dev);
   if (t->fence) DestroyFence(dev, t->fence, NULL);
   if (t->pool) DestroyCommandPool(dev, t->pool, NULL);
   if (t->fb) DestroyFramebuffer(dev, t->fb, NULL);
   if (t->rp) DestroyRenderPass(dev, t->rp, NULL);
   if (t->view) DestroyImageView(dev, t->view, NULL);
   if (t->img) DestroyImage(dev, t->img, NULL);
   if (t->mem) FreeMemory(dev, t->mem, NULL);
}

/* allocate a primary command buffer, begin */
static VkCommandBuffer
cb_begin(struct tgt *t)
{
   VkCommandBuffer cb;
   VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = t->pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   if (AllocateCommandBuffers(dev, &ai, &cb) != VK_SUCCESS) return NULL;
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   if (BeginCommandBuffer(cb, &bi) != VK_SUCCESS) return NULL;
   return cb;
}

static int
cb_submit(struct tgt *t, VkCommandBuffer cb)
{
   CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   VkResult r = QueueSubmit(queue, 1, &si, t->fence);
   CHECK(r == VK_SUCCESS, "submit = %d", r);
   r = WaitForFences(dev, 1, &t->fence, VK_TRUE, 20000000000ull);
   CHECK(r == VK_SUCCESS, "wait = %d", r);
   g_rf(dev, 1, &t->fence);
   return 1;
}

/* graphics pipeline: VS+FS, triangle list, no vertex input, optional set layout / push constant size, no depth, opaque */
static int
make_pipe(struct tgt *t, const char *vs, const char *fs, VkDescriptorSetLayout dsl, uint32_t pc_size, VkPipelineLayout *lay, VkPipeline *pipe)
{
   VkShaderModule v = load_spv(vs), f = load_spv(fs);
   CHECK(v && f, "shader modules %s %s", vs, fs);
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = pc_size};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = dsl ? 1 : 0, .pSetLayouts = &dsl,
                                     .pushConstantRangeCount = pc_size ? 1 : 0, .pPushConstantRanges = &pcr};
   CHECK(CreatePipelineLayout(dev, &pli, NULL, lay) == VK_SUCCESS, "pipeline layout");
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = v, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = f, .pName = "main"}};
   VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ias = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = g_topo};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = g_lw};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                              .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &cba};
   VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo dys = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
   VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                      .pVertexInputState = &vis, .pInputAssemblyState = &ias, .pViewportState = &vps,
                                      .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs,
                                      .pDynamicState = &dys, .layout = *lay, .renderPass = t->rp};
   VkResult r = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, pipe);
   DestroyShaderModule(dev, v, NULL);
   DestroyShaderModule(dev, f, NULL);
   CHECK(r == VK_SUCCESS, "graphics pipeline = %d", r);
   return 1;
}

static void
begin_pass(VkCommandBuffer cb, struct tgt *t, VkPipeline pipe)
{
   VkClearValue cv = {.color = {{0, 0, 0, 0}}};
   VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = t->rp, .framebuffer = t->fb,
                               .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &cv};
   CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   VkViewport vp = {0, 0, W, H, 0.0f, 1.0f};
   VkRect2D sc = {{0, 0}, {W, H}};
   CmdSetViewport(cb, 0, 1, &vp);
   CmdSetScissor(cb, 0, 1, &sc);
}

static const uint8_t *
pix(struct tgt *t, int x, int y)
{
   return t->px + y * t->pitch + x * 4;
}

static uint32_t
coverage(struct tgt *t)
{
   uint32_t n = 0;
   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) n += pix(t, x, y)[3] != 0;
   return n;
}

static int
near(const uint8_t *p, int r, int g, int b, int a, int tol)
{
   return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol && abs(p[3] - a) <= tol;
}

/* ---------------------------------------------------------------- firstinst / idx32 */
static int
red_at(struct tgt *t, int x, int y)
{
   return pix(t, x, y)[0] > 200 && pix(t, x, y)[3] == 255;
}

static int
test_firstinst(void)
{
   if (!g_feat.drawIndirectFirstInstance) { printf("{\"test\":\"firstinst\",\"skipped\":\"drawIndirectFirstInstance not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(&t, "inst.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe)) return 0;
   struct hbuf ib;
   if (!hbuf_create(&ib, 256, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)) return 0;
   /* byte 0: u32 indices {0,1,2}; byte 16: DrawIndirect {3,1,0,firstInstance 0}, {3,1,0,2}; byte 48: {3,2,0,3} (instances 3,4);
    * byte 80: DrawIndexedIndirect {3,1,0,0,firstInstance 1} */
   static const uint32_t idx[3] = {0, 1, 2};
   static const uint32_t di[12] = {3, 1, 0, 0, 3, 1, 0, 2, 3, 2, 0, 3};
   static const uint32_t dx[5] = {3, 1, 0, 0, 1};
   memcpy(ib.p, idx, sizeof idx);
   memcpy(ib.p + 16, di, sizeof di);
   memcpy(ib.p + 80, dx, sizeof dx);
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   CmdBindIndexBuffer(cb, ib.b, 0, VK_INDEX_TYPE_UINT32);
   CmdDrawIndirect(cb, ib.b, 16, 1, 16);                 /* A: firstInstance 0 -> x -1..-0.5 */
   CmdDrawIndirect(cb, ib.b, 32, 1, 16);                 /* B: firstInstance 2 -> x 0..0.5 */
   CmdDrawIndirect(cb, ib.b, 48, 1, 16);                 /* D: instances 3,4 -> x 0.5..1 (4 is off screen) */
   CmdDrawIndexedIndirect(cb, ib.b, 80, 1, 20);          /* C: firstInstance 1 -> x -0.5..0 */
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      uint32_t cov = coverage(&t);
      CHECK(abs((int)cov - 8192) <= 8192 / 50, "coverage %u expected 8192", cov);
      /* centroids of A, C, B, D (y = 117 in image rows: NDC y -0.083) */
      static const int cx[4] = {21, 85, 149, 213};
      for (int i = 0; i < 4; i++)
         CHECK(red_at(&t, cx[i], 117), "triangle %d missing at (%d,117): %u,%u,%u,%u", i, cx[i], pix(&t, cx[i], 117)[0],
               pix(&t, cx[i], 117)[1], pix(&t, cx[i], 117)[2], pix(&t, cx[i], 117)[3]);
      printf("{\"test\":\"firstinst\",\"coverage\":%u,\"ok\":true}\n", cov);
   } else {
      printf("{\"test\":\"firstinst\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   hbuf_destroy(&ib);
   tgt_destroy(&t);
   return 1;
}

static int
test_idx32(void)
{
   if (!g_feat.fullDrawIndexUint32) { printf("{\"test\":\"idx32\",\"skipped\":\"fullDrawIndexUint32 not advertised\",\"ok\":true}\n"); return 1; }
   CHECK(g_props.limits.maxDrawIndexedIndexValue == 0xffffffffu, "maxDrawIndexedIndexValue = %u", g_props.limits.maxDrawIndexedIndexValue);
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(&t, "inst.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe)) return 0;
   struct hbuf ib;
   if (!hbuf_create(&ib, 256, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return 0;
   uint32_t idx[6] = {0x01000000u, 0x01000001u, 0x01000002u, 0x7ffffff0u, 0x7ffffff1u, 0x7ffffff2u};
   memcpy(ib.p, idx, sizeof idx);
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   CmdBindIndexBuffer(cb, ib.b, 0, VK_INDEX_TYPE_UINT32);
   CmdDrawIndexed(cb, 3, 1, 0, -0x01000000, 0);          /* indices above 2^24: vertex 0..2, x -1..-0.5 */
   CmdDrawIndexed(cb, 3, 1, 3, -0x7ffffff0, 2);          /* indices near 2^31, firstInstance 2: x 0..0.5 */
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      uint32_t cov = coverage(&t);
      CHECK(abs((int)cov - 4096) <= 4096 / 50, "coverage %u expected 4096", cov);
      CHECK(red_at(&t, 21, 117), "first triangle missing");
      CHECK(red_at(&t, 149, 117), "second triangle missing");
      printf("{\"test\":\"idx32\",\"coverage\":%u,\"ok\":true}\n", cov);
   } else {
      printf("{\"test\":\"idx32\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   hbuf_destroy(&ib);
   tgt_destroy(&t);
   return 1;
}

/* ---------------------------------------------------------------- textured cases */
struct tex {
   VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler smp;
   VkDescriptorSetLayout dsl; VkDescriptorPool pool; VkDescriptorSet set;
};

/* Create an optimal sampled image, upload `regions` from `data`, make a view / sampler / descriptor set (combined sampler, fragment). */
static int
tex_create(struct tgt *t, struct tex *x, const VkImageCreateInfo *ici, VkImageViewType vt, uint32_t layers, const void *data, size_t size,
           const VkBufferImageCopy *regions, uint32_t nreg, const VkSamplerCreateInfo *sci)
{
   memset(x, 0, sizeof *x);
   CHECK(CreateImage(dev, ici, NULL, &x->img) == VK_SUCCESS, "texture image");
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, x->img, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, 0)};
   CHECK(AllocateMemory(dev, &ai, NULL, &x->mem) == VK_SUCCESS, "texture memory");
   CHECK(BindImageMemory(dev, x->img, x->mem, 0) == VK_SUCCESS, "bind texture");
   struct hbuf st;
   if (!hbuf_create(&st, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return 0;
   memcpy(st.p, data, size);
   VkCommandBuffer cb = cb_begin(t);
   CHECK(cb, "cmdbuf");
   VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, ici->mipLevels, 0, ici->arrayLayers};
   VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = x->img, .subresourceRange = rng};
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
   CmdCopyBufferToImage(cb, st.b, x->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nreg, regions);
   VkImageMemoryBarrier b2 = b1;
   b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
   if (!cb_submit(t, cb)) return 0;
   hbuf_destroy(&st);
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = x->img, .viewType = vt,
                               .format = ici->format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, ici->mipLevels, 0, layers}};
   CHECK(CreateImageView(dev, &vi, NULL, &x->view) == VK_SUCCESS, "texture view");
   CHECK(CreateSampler(dev, sci, NULL, &x->smp) == VK_SUCCESS, "sampler");
   VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
                                      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb};
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &x->dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &x->pool) == VK_SUCCESS, "dpool");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = x->pool,
                                      .descriptorSetCount = 1, .pSetLayouts = &x->dsl};
   CHECK(AllocateDescriptorSets(dev, &dai, &x->set) == VK_SUCCESS, "alloc set");
   VkDescriptorImageInfo ii = {x->smp, x->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = x->set, .dstBinding = 0, .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
   UpdateDescriptorSets(dev, 1, &w, 0, NULL);
   return 1;
}

static void
tex_destroy(struct tex *x)
{
   if (x->pool) DestroyDescriptorPool(dev, x->pool, NULL);
   if (x->dsl) DestroyDescriptorSetLayout(dev, x->dsl, NULL);
   if (x->smp) DestroySampler(dev, x->smp, NULL);
   if (x->view) DestroyImageView(dev, x->view, NULL);
   if (x->img) DestroyImage(dev, x->img, NULL);
   if (x->mem) FreeMemory(dev, x->mem, NULL);
}

/* one full-screen quad draw sampling tex with fragment shader fs, push constants uv = pos01 * s.xy + s.zw */
static int
quad_draw(struct tgt *t, struct tex *x, const char *fs, const float s[4])
{
   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(t, "quad.vert.spv", fs, x->dsl, 16, &lay, &pipe)) return 0;
   VkCommandBuffer cb = cb_begin(t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, t, pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 1, &x->set, 0, NULL);
   CmdPushConstants(cb, lay, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, s);
   CmdDraw(cb, 6, 1, 0, 0);
   CmdEndRenderPass(cb);
   int ok = cb_submit(t, cb);
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   return ok;
}

static VkImageCreateInfo
img2d(VkFormat f, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, VkImageCreateFlags flags)
{
   return (VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                              .extent = {w, h, 1}, .mipLevels = mips, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
}

/* Anisotropy: 64x64 texture, 7 mips each a different flat colour (mip 0 red, mip 1 green). The quad's uv derivative is 0.25 texel/px in
 * u and 1.5 in v: without anisotropy the LOD follows the long axis (log2 1.5 = 0.58 -> mip 1, green); with maxAnisotropy 16 the LOD
 * follows long/ratio (1.5/6 = 0.25 -> clamped to 0 -> mip 0, red). */
static int
aniso_run(struct tgt *t, int enable, uint8_t out[4])
{
   static const uint8_t mc[7][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255},
                                    {0, 255, 255, 255}, {255, 0, 255, 255}, {255, 255, 255, 255}};
   uint8_t *data = malloc(64 * 64 * 4 * 2);
   VkBufferImageCopy reg[7];
   size_t off = 0;
   for (int l = 0; l < 7; l++) {
      uint32_t d = 64u >> l;
      for (uint32_t i = 0; i < d * d; i++) memcpy(data + off + i * 4, mc[l], 4);
      reg[l] = (VkBufferImageCopy){.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, 1}, .imageExtent = {d, d, 1}};
      off += d * d * 4;
   }
   VkImageCreateInfo ici = img2d(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 7, 1, 0);
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                             .anisotropyEnable = enable, .maxAnisotropy = enable ? 16.0f : 1.0f, .maxLod = 7.0f};
   struct tex x;
   int ok = tex_create(t, &x, &ici, VK_IMAGE_VIEW_TYPE_2D, 1, data, off, reg, 7, &si);
   free(data);
   if (ok) {
      const float s[4] = {1.0f, 6.0f, 0, 0};
      ok = quad_draw(t, &x, "tex.frag.spv", s);
      if (ok && !g_fake) memcpy(out, pix(t, 128, 128), 4);
   }
   tex_destroy(&x);
   return ok;
}

static int
test_aniso(void)
{
   if (!g_feat.samplerAnisotropy) { printf("{\"test\":\"aniso\",\"skipped\":\"samplerAnisotropy not advertised\",\"ok\":true}\n"); return 1; }
   CHECK(g_props.limits.maxSamplerAnisotropy == 16.0f, "maxSamplerAnisotropy = %g", g_props.limits.maxSamplerAnisotropy);
   uint8_t a[4] = {0}, b[4] = {0};
   for (int e = 0; e < 2; e++) {
      struct tgt t;
      if (!tgt_create(&t)) return 0;
      int ok = aniso_run(&t, e, e ? b : a);
      tgt_destroy(&t);
      if (!ok) return 0;
   }
   if (g_fake) { printf("{\"test\":\"aniso\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n"); return 1; }
   CHECK(near(a, 0, 255, 0, 255, 8), "no anisotropy: %u,%u,%u,%u expected mip 1 (green)", a[0], a[1], a[2], a[3]);
   CHECK(near(b, 255, 0, 0, 255, 8), "anisotropy 16: %u,%u,%u,%u expected mip 0 (red)", b[0], b[1], b[2], b[3]);
   printf("{\"test\":\"aniso\",\"off\":[%u,%u,%u,%u],\"on\":[%u,%u,%u,%u],\"ok\":true}\n", a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
   return 1;
}

/* Mirror clamp to edge: 4x1 texture R,G,B,Y; u runs -1..1 across the quad, nearest filter: coordinate -> clamp(|u|, 0, 1). */
static int
test_mirror(void)
{
   if (!g_mirror) { printf("{\"test\":\"mirror\",\"skipped\":\"samplerMirrorClampToEdge not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   static const uint8_t texel[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};
   VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 1, 1}};
   VkImageCreateInfo ici = img2d(VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 1, 1, 0);
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                             .addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, .maxLod = 0.25f};
   struct tex x;
   if (!tex_create(&t, &x, &ici, VK_IMAGE_VIEW_TYPE_2D, 1, texel, sizeof texel, &reg, 1, &si)) return 0;
   const float s[4] = {2.0f, 0.0f, -1.0f, 0.5f};
   if (!quad_draw(&t, &x, "tex.frag.spv", s)) return 0;
   if (!g_fake) {
      /* x -> u = (x+.5)/128 - 1 ; texel = floor(clamp(|u|,0,1) * 4) */
      /* x -> u = (x+.5)/128 - 1 ; texel = floor(clamp(|u|,0,1) * 4); skip positions near a texel boundary */
      int n = 0, mirrored = 0;
      for (int x = 4; x < W; x += 6) {
         double u = (x + 0.5) / 128.0 - 1.0, a = fabs(u) > 1.0 ? 1.0 : fabs(u), ft = a * 4.0;
         if (fabs(ft - floor(ft + 0.5)) < 0.15) continue;
         int tx = (int)ft > 3 ? 3 : (int)ft;
         const uint8_t *e = texel[tx];
         const uint8_t *p = pix(&t, x, 128);
         CHECK(near(p, e[0], e[1], e[2], e[3], 8), "x=%d (u=%.3f) got %u,%u,%u,%u expected texel %d", x, u, p[0], p[1], p[2], p[3], tx);
         n++;
         mirrored += u < 0;
      }
      CHECK(n > 20 && mirrored > 8, "probes %d mirrored %d", n, mirrored);
      printf("{\"test\":\"mirror\",\"probes\":%d,\"ok\":true}\n", n);
   } else {
      printf("{\"test\":\"mirror\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   tex_destroy(&x);
   tgt_destroy(&t);
   return 1;
}

/* Cube array: 2 cubes (12 layers) of 4x4 flat faces, layer L colour (10+20L, 255-20L, 77). Four quadrants sample L = 0, 3, 6, 10. */
static int
test_cubearray(void)
{
   if (!g_feat.imageCubeArray) { printf("{\"test\":\"cubearray\",\"skipped\":\"imageCubeArray not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   uint8_t data[12 * 4 * 4 * 4];
   for (int l = 0; l < 12; l++)
      for (int i = 0; i < 16; i++) {
         uint8_t *p = data + (l * 16 + i) * 4;
         p[0] = 10 + 20 * l; p[1] = 255 - 20 * l; p[2] = 77; p[3] = 255;
      }
   VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 12}, .imageExtent = {4, 4, 1}};
   VkImageCreateInfo ici = img2d(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 12, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
   struct tex x;
   if (!tex_create(&t, &x, &ici, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 12, data, sizeof data, &reg, 1, &si)) return 0;
   const float s[4] = {1.0f, 1.0f, 0, 0};
   if (!quad_draw(&t, &x, "cubearr.frag.spv", s)) return 0;
   if (!g_fake) {
      static const struct { int x, y, l; } probe[] = {{64, 64, 0}, {192, 64, 3}, {64, 192, 6}, {192, 192, 10}};
      for (unsigned i = 0; i < 4; i++) {
         int l = probe[i].l;
         const uint8_t *p = pix(&t, probe[i].x, probe[i].y);
         CHECK(near(p, 10 + 20 * l, 255 - 20 * l, 77, 255, 6), "quadrant %u got %u,%u,%u,%u expected layer %d", i, p[0], p[1], p[2], p[3], l);
      }
      printf("{\"test\":\"cubearray\",\"ok\":true}\n");
   } else {
      printf("{\"test\":\"cubearray\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   tex_destroy(&x);
   tgt_destroy(&t);
   return 1;
}

/* wideLines: horizontal line list, width 5 -> about 5 rows covered at x=128, and width 1 -> about 1 */
static int
line_rows(float w)
{
   struct tgt t;
   if (!tgt_create(&t)) return -1;
   g_topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
   g_lw = w;
   VkPipelineLayout lay;
   VkPipeline pipe;
   int ok = make_pipe(&t, "line.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
   g_topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   g_lw = 1.0f;
   if (!ok) return -1;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   CmdDraw(cb, 2, 1, 0, 0);
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return -1;
   int rows = 0;
   if (!g_fake)
      for (int y = 0; y < H; y++) rows += pix(&t, 128, y)[3] != 0;
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   tgt_destroy(&t);
   return rows;
}

static int
test_wide(void)
{
   if (!g_feat.wideLines) { printf("{\"test\":\"wide\",\"skipped\":\"wideLines not advertised\",\"ok\":true}\n"); return 1; }
   int r1 = line_rows(1.0f), r5 = line_rows(5.0f);
   if (r1 < 0 || r5 < 0) return 0;
   if (!g_fake) {
      CHECK(r1 >= 1 && r1 <= 2, "width 1 covers %d rows", r1);
      CHECK(r5 >= 4 && r5 <= 6, "width 5 covers %d rows", r5);
      printf("{\"test\":\"wide\",\"rows1\":%d,\"rows5\":%d,\"ok\":true}\n", r1, r5);
   } else {
      printf("{\"test\":\"wide\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   return 1;
}

/* shaderStorageImageExtendedFormats: write a known texel through imageStore into every spec-listed extended format, copy out, compare bytes. */
struct sfmt { const char *q; VkFormat f; int n, bits; char kind; };  /* kind: U unorm, S snorm, u uint, i sint, F float; special: n=0 packed */
static const struct sfmt sfmts[] = {
   {"r8", 9, 1, 8, 'U'}, {"r8_snorm", 10, 1, 8, 'S'}, {"r8ui", 13, 1, 8, 'u'}, {"r8i", 14, 1, 8, 'i'},
   {"rg8", 16, 2, 8, 'U'}, {"rg8_snorm", 17, 2, 8, 'S'}, {"rg8ui", 20, 2, 8, 'u'}, {"rg8i", 21, 2, 8, 'i'},
   {"rgb10_a2", 64, 0, 0, 'U'}, {"rgb10_a2ui", 68, 0, 0, 'u'},
   {"r16", 70, 1, 16, 'U'}, {"r16_snorm", 71, 1, 16, 'S'}, {"r16ui", 74, 1, 16, 'u'}, {"r16i", 75, 1, 16, 'i'}, {"r16f", 76, 1, 16, 'F'},
   {"rg16", 77, 2, 16, 'U'}, {"rg16_snorm", 78, 2, 16, 'S'}, {"rg16ui", 81, 2, 16, 'u'}, {"rg16i", 82, 2, 16, 'i'}, {"rg16f", 83, 2, 16, 'F'},
   {"rgba16", 91, 4, 16, 'U'}, {"rgba16_snorm", 92, 4, 16, 'S'}, {"rgba16ui", 95, 4, 16, 'u'}, {"rgba16i", 96, 4, 16, 'i'}, {"rgba16f", 97, 4, 16, 'F'},
   {"rg32ui", 101, 2, 32, 'u'}, {"rg32i", 102, 2, 32, 'i'}, {"rg32f", 103, 2, 32, 'F'},
   {"r11f_g11f_b10f", 122, 0, 0, 'F'},
};

/* expected little-endian texel into out[], returns size */
static int
sfmt_expect(const struct sfmt *f, uint8_t *out)
{
   if (!f->n) {
      uint32_t v = !strcmp(f->q, "rgb10_a2") ? 0xFFF003FFu : !strcmp(f->q, "rgb10_a2ui") ? (1u | 2u << 10 | 3u << 20 | 1u << 30) : 0x780003C0u;
      memcpy(out, &v, 4);
      return 4;
   }
   int bytes = f->bits / 8;
   for (int c = 0; c < f->n; c++) {
      uint64_t v;
      switch (f->kind) {
      case 'U': v = (c == 1) ? 0 : (f->bits == 32 ? 0xffffffffu : (1ull << f->bits) - 1); break;
      case 'S': v = (c == 1) ? 0 : (1ull << (f->bits - 1)) - 1; break;
      case 'u': v = (uint64_t[]){1, 2, 3, 1}[c]; break;
      case 'i': v = ((uint64_t[]){(uint64_t)-1, 2, (uint64_t)-3, 1}[c]) & (f->bits == 32 ? 0xffffffffu : (1ull << f->bits) - 1); break;
      default: v = (c == 1) ? 0 : (f->bits == 16 ? 0x3C00 : 0x3F800000u); break;
      }
      memcpy(out + c * bytes, &v, bytes);   /* little endian host */
   }
   return f->n * bytes;
}

static int
storfmt_one(struct tgt *t, const struct sfmt *f)
{
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f->f, .extent = {4, 4, 1},
                           .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage img;
   VkDeviceMemory mem;
   VkMemoryRequirements mr;
   if (CreateImage(dev, &ii, NULL, &img) != VK_SUCCESS) { printf("{\"storfmt\":\"%s\",\"fail\":\"CreateImage\"}\n", f->q); return 0; }
   GetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, 0)};
   CHECK(AllocateMemory(dev, &ai, NULL, &mem) == VK_SUCCESS, "mem");
   CHECK(BindImageMemory(dev, img, mem, 0) == VK_SUCCESS, "bind");
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f->f,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView view;
   CHECK(CreateImageView(dev, &vi, NULL, &view) == VK_SUCCESS, "view");
   VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
   VkDescriptorSetLayout dsl;
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool dp;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "dp");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet ds;
   CHECK(AllocateDescriptorSets(dev, &dai, &ds) == VK_SUCCESS, "ds");
   VkDescriptorImageInfo dii = {.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
   VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dii};
   UpdateDescriptorSets(dev, 1, &w, 0, NULL);
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
   VkPipelineLayout lay;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &lay) == VK_SUCCESS, "layout");
   char name[64];
   snprintf(name, sizeof name, "stor_%s.comp.spv", f->q);
   VkShaderModule m = load_spv(name);
   CHECK(m, "shader %s", name);
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = lay,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
   VkPipeline pipe;
   VkResult r = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
   DestroyShaderModule(dev, m, NULL);
   if (r != VK_SUCCESS) { printf("{\"storfmt\":\"%s\",\"fail\":\"pipeline %d\"}\n", f->q, r); g_fail++; return 0; }
   struct hbuf ob;
   if (!hbuf_create(&ob, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return 0;
   memset(ob.p, 0xA5, 4096);
   VkCommandBuffer cb = cb_begin(t);
   CHECK(cb, "cmdbuf");
   VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, &ds, 0, NULL);
   CmdDispatch(cb, 1, 1, 1);
   ib.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
   ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   ib.oldLayout = ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
   VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
   CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, ob.b, 1, &reg);
   if (!cb_submit(t, cb)) return 0;
   int ok = 1;
   if (!g_fake) {
      uint8_t exp[16];
      int tb = sfmt_expect(f, exp);
      for (int i = 0; i < 16 && ok; i++)
         if (memcmp(ob.p + i * tb, exp, tb)) {
            ok = 0;
            printf("{\"storfmt\":\"%s\",\"fail\":\"texel %d\",\"got\":\"", f->q, i);
            for (int k = 0; k < tb; k++) printf("%02x", ob.p[i * tb + k]);
            printf("\",\"want\":\"");
            for (int k = 0; k < tb; k++) printf("%02x", exp[k]);
            printf("\"}\n");
         }
   }
   if (!ok) g_fail++;
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   DestroyImageView(dev, view, NULL);
   DestroyImage(dev, img, NULL);
   FreeMemory(dev, mem, NULL);
   hbuf_destroy(&ob);
   return ok;
}

static int
test_storfmt(void)
{
   if (!g_feat.shaderStorageImageExtendedFormats) { printf("{\"test\":\"storfmt\",\"skipped\":\"shaderStorageImageExtendedFormats not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   int pass = 0, n = (int)(sizeof sfmts / sizeof *sfmts);
   for (int i = 0; i < n; i++) pass += storfmt_one(&t, &sfmts[i]);
   printf("{\"test\":\"storfmt\",\"formats\":%d,\"passed\":%d,\"ok\":%s}\n", n, g_fake ? n : pass, pass == n ? "true" : "false");
   tgt_destroy(&t);
   return pass == n;
}

/* bufferDeviceAddress: push a 64-bit pointer (buffer address + 256) to a compute shader that writes through it. */
static int
test_bda(void)
{
   if (!g_bda) { printf("{\"test\":\"bda\",\"skipped\":\"bufferDeviceAddress not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
   VkBuffer buf;
   CHECK(CreateBuffer(dev, &bi, NULL, &buf) == VK_SUCCESS, "buffer");
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, buf, &mr);
   VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &fi, .allocationSize = mr.size,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   VkDeviceMemory mem;
   CHECK(AllocateMemory(dev, &ai, NULL, &mem) == VK_SUCCESS, "alloc");
   CHECK(BindBufferMemory(dev, buf, mem, 0) == VK_SUCCESS, "bind");
   uint32_t *p;
   CHECK(MapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p) == VK_SUCCESS, "map");
   memset(p, 0xEE, 4096);
   VkBufferDeviceAddressInfo da = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf};
   uint64_t addr = GetBufferDeviceAddress(dev, &da);
   CHECK(addr && !(addr & 3), "address 0x%llx", (unsigned long long)addr);
   CHECK(GetBufferDeviceAddress(dev, &da) == addr, "address stable");

   struct { uint64_t addr; uint32_t base; uint32_t pad; } pc = {addr + 256, 1000, 0};
   VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   VkPipelineLayout lay;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &lay) == VK_SUCCESS, "layout");
   VkShaderModule m = load_spv("bda.comp.spv");
   CHECK(m, "shader");
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = lay,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
   VkPipeline pipe;
   VkResult r = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
   DestroyShaderModule(dev, m, NULL);
   CHECK(r == VK_SUCCESS, "compute pipeline = %d", r);
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   CmdPushConstants(cb, lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &pc);
   CmdDispatch(cb, 2, 1, 1);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      for (int i = 0; i < 128; i++)
         CHECK(p[64 + i] == 1000u + 3u * i, "elem %d = %u expected %u", i, p[64 + i], 1000u + 3u * i);
      CHECK(p[63] == 0xEEEEEEEEu && p[192] == 0xEEEEEEEEu, "stray write: p[63]=%x p[192]=%x", p[63], p[192]);
      printf("{\"test\":\"bda\",\"addr\":\"0x%llx\",\"ok\":true}\n", (unsigned long long)addr);
   } else {
      printf("{\"test\":\"bda\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyBuffer(dev, buf, NULL);
   FreeMemory(dev, mem, NULL);
   tgt_destroy(&t);
   return 1;
}

/* descriptor indexing (sampled images): a PARTIALLY_BOUND | UPDATE_AFTER_BIND | UPDATE_UNUSED_WHILE_PENDING array of 8 sampled
 * images with only 0..3 (and 5) bound, indexed non-uniformly (per pixel of a 2x2 quad) through a runtime array in the shader.
 * Slot 2 is rewritten AFTER the descriptor set was bound in the command buffer and before submit. */
static int
test_didx(void)
{
   if (!g_didx) { printf("{\"test\":\"didx\",\"skipped\":\"descriptor indexing not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   static const uint8_t col[6][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}, {0, 255, 255, 255}, {255, 0, 255, 255}};
   struct tex x[6];
   VkImageCreateInfo ici = img2d(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 0);
   VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}};
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                             .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
   for (int i = 0; i < 6; i++)
      if (!tex_create(&t, &x[i], &ici, VK_IMAGE_VIEW_TYPE_2D, 1, col[i], 4, &reg, 1, &si)) return 0;

   VkDescriptorBindingFlags bf[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
                                     VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT};
   VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                      .bindingCount = 2, .pBindingFlags = bf};
   VkDescriptorSetLayoutBinding lb[2] = {{.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 8, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                         {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bfi,
                                          .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, .bindingCount = 2, .pBindings = lb};
   VkDescriptorSetLayout dsl;
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                                     .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps};
   VkDescriptorPool dp;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "dpool");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet set;
   CHECK(AllocateDescriptorSets(dev, &dai, &set) == VK_SUCCESS, "alloc set");
   VkDescriptorImageInfo ii[5];
   for (int i = 0; i < 4; i++) ii[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, x[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   ii[4] = (VkDescriptorImageInfo){VK_NULL_HANDLE, x[5].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};   /* slot 5 */
   VkDescriptorImageInfo smpi = {x[0].smp, VK_NULL_HANDLE, 0};
   VkWriteDescriptorSet w[3] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .dstArrayElement = 0, .descriptorCount = 4,
       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .dstArrayElement = 5, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[4]},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &smpi}};
   UpdateDescriptorSets(dev, 3, w, 0, NULL);

   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(&t, "quad.vert.spv", "didx.frag.spv", dsl, 16, &lay, &pipe)) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 1, &set, 0, NULL);
   const float sc[4] = {1, 1, 0, 0};
   CmdPushConstants(cb, lay, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, sc);
   CmdDraw(cb, 6, 1, 0, 0);
   CmdEndRenderPass(cb);
   /* update after bind: slot 2 (blue) -> image 4 (cyan), while the set is bound in a recorded, not yet submitted command buffer */
   VkDescriptorImageInfo upd = {VK_NULL_HANDLE, x[4].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet wu = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .dstArrayElement = 2, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &upd};
   UpdateDescriptorSets(dev, 1, &wu, 0, NULL);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      static const int expect[4] = {0, 1, 4, 3};   /* index = (x&1) + 2*(y&1) */
      int bad = 0;
      for (int y = 8; y < 24; y++)
         for (int xx = 8; xx < 24; xx++) {
            int e = expect[(xx & 1) + 2 * (y & 1)];
            if (!near(pix(&t, xx, y), col[e][0], col[e][1], col[e][2], 255, 2)) bad++;
         }
      if (bad) for (int k = 0; k < 4; k++) printf("{\"quad\":%d,\"rgba\":[%u,%u,%u,%u]}\n", k, pix(&t, 8 + (k & 1), 8 + (k >> 1))[0], pix(&t, 8 + (k & 1), 8 + (k >> 1))[1], pix(&t, 8 + (k & 1), 8 + (k >> 1))[2], pix(&t, 8 + (k & 1), 8 + (k >> 1))[3]);
      CHECK(bad == 0, "%d of 256 probed pixels have the wrong colour (first: %u,%u,%u,%u)", bad, pix(&t, 8, 8)[0], pix(&t, 8, 8)[1], pix(&t, 8, 8)[2], pix(&t, 8, 8)[3]);
      const uint8_t *q = pix(&t, 200, 201);
      CHECK(near(q, col[expect[(200 & 1) + 2 * (201 & 1)]][0], col[expect[(200 & 1) + 2 * (201 & 1)]][1], col[expect[(200 & 1) + 2 * (201 & 1)]][2], 255, 2), "far pixel wrong");
      printf("{\"test\":\"didx\",\"bad\":%d,\"ok\":true}\n", bad);
   } else {
      printf("{\"test\":\"didx\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   for (int i = 0; i < 6; i++) tex_destroy(&x[i]);
   tgt_destroy(&t);
   return 1;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else { fprintf(stderr, "usage: t_vk_feat --direct icd.so [--spv-dir dir] [--only case]\n"); return 2; }
   }
   if (!icd) { fprintf(stderr, "--direct required\n"); return 2; }
   const char *be = getenv("MALI_BACKEND");
   g_fake = be && !strcmp(be, "fake");

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("{\"fail\":\"dlopen: %s\"}\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("{\"fail\":\"icd entry\"}\n"); return 1; }
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceMemoryProperties); I(GetPhysicalDeviceProperties);
   I(GetPhysicalDeviceFeatures2); I(CreateDevice); I(GetDeviceProcAddr); I(DestroyInstance);
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   VkResult r = EnumeratePhysicalDevices(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   GetPhysicalDeviceProperties(pd, &g_props);
   GetPhysicalDeviceMemoryProperties(pd, &memprops);
   printf("{\"device\":\"%s\"}\n", g_props.deviceName);

   VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
   VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
   GetPhysicalDeviceFeatures2(pd, &f2);
   g_feat = f2.features;
   g_mirror = f12.samplerMirrorClampToEdge;
   g_bda = f12.bufferDeviceAddress;
   g_didx = f12.runtimeDescriptorArray && f12.shaderSampledImageArrayNonUniformIndexing && f12.descriptorBindingPartiallyBound &&
            f12.descriptorBindingSampledImageUpdateAfterBind && f12.descriptorBindingUpdateUnusedWhilePending;
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   /* enable exactly what is advertised (everything advertised is under test somewhere) */
   VkPhysicalDeviceVulkan12Features e12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .samplerMirrorClampToEdge = f12.samplerMirrorClampToEdge, .bufferDeviceAddress = f12.bufferDeviceAddress, .runtimeDescriptorArray = f12.runtimeDescriptorArray,
      .shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing, .descriptorBindingPartiallyBound = f12.descriptorBindingPartiallyBound,
      .descriptorBindingSampledImageUpdateAfterBind = f12.descriptorBindingSampledImageUpdateAfterBind, .descriptorBindingUpdateUnusedWhilePending = f12.descriptorBindingUpdateUnusedWhilePending,
                                           .timelineSemaphore = f12.timelineSemaphore};
   VkPhysicalDeviceFeatures2 e2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e12, .features = g_feat};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &e2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   DEV_FNS(LOAD)
   g_gsl = (PFN_vkGetImageSubresourceLayout)GetDeviceProcAddr(dev, "vkGetImageSubresourceLayout");
   g_rf = (PFN_vkResetFences)GetDeviceProcAddr(dev, "vkResetFences");
   GetDeviceQueue(dev, 0, 0, &queue);

   int ok = 1;
   if (ok && CASE_ON("firstinst")) ok &= test_firstinst();
   if (ok && CASE_ON("idx32")) ok &= test_idx32();
   if (ok && CASE_ON("aniso")) ok &= test_aniso();
   if (ok && CASE_ON("mirror")) ok &= test_mirror();
   if (ok && CASE_ON("cubearray")) ok &= test_cubearray();
   if (ok && CASE_ON("wide")) ok &= test_wide();
   if (ok && CASE_ON("storfmt")) ok &= test_storfmt();
   if (ok && CASE_ON("bda")) ok &= test_bda();
   if (ok && CASE_ON("didx")) ok &= test_didx();

   DeviceWaitIdle(dev);
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   dlclose(lib);
   printf("{\"pass\":%s}\n", (ok && !g_fail) ? "true" : "false");
   return (ok && !g_fail) ? 0 : 1;
}
