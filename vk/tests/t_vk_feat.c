/* SPDX-License-Identifier: MIT */
/* R4-B2: behaviour tests for the DXVK-required core features, one test per feature (the honesty rule: a feature is advertised only
 * with a passing test here or in t_vk_draw). Loader-free like t_vk_draw. One JSON line per case; exit 0 = all pass.
 *
 *   t_vk_feat --direct <icd.so> [--spv-dir dir] [--only firstinst|idx32|aniso|mirror|cubearray]
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
static VkBool32 g_mirror;

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
   X(CmdPipelineBarrier)

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
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
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
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   /* enable exactly what is advertised (everything advertised is under test somewhere) */
   VkPhysicalDeviceVulkan12Features e12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .samplerMirrorClampToEdge = f12.samplerMirrorClampToEdge,
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

   DeviceWaitIdle(dev);
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   dlclose(lib);
   printf("{\"pass\":%s}\n", (ok && !g_fail) ? "true" : "false");
   return (ok && !g_fail) ? 0 : 1;
}
