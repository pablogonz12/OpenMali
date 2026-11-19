/* SPDX-License-Identifier: MIT */
/* Hardware probe for robust buffer access (robustBufferAccess2 semantics) behind the default-off lowering (nir_lower_robust_access + size queries).
 * robustBufferAccess2 is NOT advertised: the ICD enables the lowering for the device when MALI_TEST_FORCE_ROBUST=1 (mali_device.c).
 *
 *   MALI_TEST_FORCE_ROBUST=1 t_vk_robust --direct <icd.so> [--spv-dir dir] [--only ssbo|vertex|wild]
 *
 * Checks (each prints one JSON line, exit 0 = all pass):
 *   ssbo   out-of-bounds SSBO / UBO reads return 0, OOB writes and atomics are discarded (canaries in the same VkBuffer past the descriptor range stay
 *          intact), in-bounds accesses still work, .length() of a small / full range is right
 *   vertex a draw of 16 vertices from an 8-word vertex buffer: vertices 8..15 fetch 0 (not the canaries stored after the buffer)
 *   wild   far OOB indexes (1 GiB, 0xFFFFFFF0 words) neither fault nor hang and read 0
 * MALI_TEST_FORCE_ROBUST unset or on the fake backend: skipped (pass). MALI_TEST_FORCE_ROBUST=0: control run (shader without .length(): OpArrayLength aborts the backend when the lowering is off), prints what the unchecked
 * driver returns for the near-OOB cases, never fails on them and skips "wild" (it could fault the GPU).
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail, g_fake, g_control;
static const char *g_only, *g_spv = "shader";

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
/* a robustness expectation: a failure in the control run is only reported */
#define EXPECT(c, ...)                                                                             \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("{\"%s\":\"%s:%d\",\"expr\":\"%s\",\"detail\":\"", g_control ? "observed" : "fail", __FILE__, __LINE__, #c); \
         printf(__VA_ARGS__);                                                                      \
         printf("\"}\n");                                                                          \
         if (!g_control) { g_fail = 1; bad++; }                                                    \
      }                                                                                            \
   } while (0)

#define DEV_FNS(X)                                                                                 \
   X(DestroyDevice) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory)  \
   X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(QueueSubmit) X(DeviceWaitIdle)  \
   X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory)            \
   X(CreateShaderModule) X(DestroyShaderModule) X(CreatePipelineLayout) X(DestroyPipelineLayout)   \
   X(CreateComputePipelines) X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateCommandPool)    \
   X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)       \
   X(CmdBindPipeline) X(CmdPushConstants) X(CmdDispatch) X(CmdDraw) X(CmdBindVertexBuffers)        \
   X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdCopyImageToBuffer)                              \
   X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory)                 \
   X(CreateImageView) X(DestroyImageView) X(CreateRenderPass) X(DestroyRenderPass)                 \
   X(CreateFramebuffer) X(DestroyFramebuffer)                                                      \
   X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool)             \
   X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CmdBindDescriptorSets)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;

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

struct buf { VkBuffer b; VkDeviceMemory m; void *p; size_t size, memsize; };

/* A VkBuffer of `size` bytes on a `memsize`-byte allocation (memsize >= the required size); the whole allocation is filled with `fill`. */
static int
mkbuf(struct buf *o, size_t size, size_t memsize, VkBufferUsageFlags usage, uint32_t fill)
{
   o->size = size;
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   CHECK(CreateBuffer(dev, &bi, NULL, &o->b) == VK_SUCCESS, "CreateBuffer");
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, o->b, &mr);
   int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   CHECK(mt >= 0, "no host visible type");
   if (memsize < mr.size) memsize = mr.size;
   o->memsize = memsize;
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = memsize, .memoryTypeIndex = mt};
   CHECK(AllocateMemory(dev, &ai, NULL, &o->m) == VK_SUCCESS, "AllocateMemory");
   CHECK(BindBufferMemory(dev, o->b, o->m, 0) == VK_SUCCESS, "BindBufferMemory");
   CHECK(MapMemory(dev, o->m, 0, VK_WHOLE_SIZE, 0, &o->p) == VK_SUCCESS, "MapMemory");
   uint32_t *w = o->p;
   for (size_t i = 0; i < memsize / 4; i++) w[i] = fill;
   return 1;
}

static void
rmbuf(struct buf *o)
{
   if (o->b) DestroyBuffer(dev, o->b, NULL);
   if (o->m) FreeMemory(dev, o->m, NULL);
}

static int
submit_wait(VkCommandPool pool, VkCommandBuffer cb, const char *what)
{
   (void)pool;
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   CHECK(QueueSubmit(queue, 1, &si, f) == VK_SUCCESS, "%s: submit", what);
   VkResult wr = WaitForFences(dev, 1, &f, VK_TRUE, 20000000000ull);
   CHECK(wr == VK_SUCCESS, "%s: wait = %d (GPU fault or hang)", what, wr);
   DestroyFence(dev, f, NULL);
   return 1;
}

#define CANARY_A 0x77770000u
#define CANARY_U 0x66660000u
#define CANARY_C 0x55550000u
#define BUFSZ 4096

/* ssbo/ubo/length probe; wild = 1 adds the far OOB accesses (and is the only thing the run does). */
static int
run_compute(int wild)
{
   int bad = 0;
   struct buf A = {0}, R = {0}, U = {0}, C = {0};
   VkShaderModule sm = load_spv(g_control ? "robust_nolen.comp.spv" : "robust.comp.spv");
   CHECK(sm, "load robust.comp.spv");
   VkDescriptorSetLayoutBinding lb[4];
   for (unsigned i = 0; i < 4; i++)
      lb[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayout dsl;
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = lb};
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
   VkDescriptorPool dp;
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps};
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "dpool");
   VkDescriptorSet ds;
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   CHECK(AllocateDescriptorSets(dev, &dai, &ds) == VK_SUCCESS, "alloc set");
   VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
   VkPipelineLayout pl;
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &pl) == VK_SUCCESS, "layout");
   VkPipeline pipe;
   VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
   VkResult pr = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipe);
   CHECK(pr == VK_SUCCESS, "CreateComputePipelines = %d", pr);

   CHECK(mkbuf(&A, BUFSZ, BUFSZ, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, CANARY_A), "A");
   CHECK(mkbuf(&R, BUFSZ, BUFSZ, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0xDEADBEEFu), "R");
   CHECK(mkbuf(&U, 256, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, CANARY_U), "U");
   CHECK(mkbuf(&C, BUFSZ, BUFSZ, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, CANARY_C), "C");
   uint32_t *a = A.p, *r = R.p, *u = U.p, *c = C.p;
   for (unsigned i = 0; i < 16; i++) a[i] = 0;
   for (unsigned i = 0; i < 24; i++) a[16 + i] = CANARY_A + i;
   a[0] = 111; a[2] = 112;
   for (unsigned i = 0; i < 64; i++) u[i] = 0;
   u[4] = 222;   /* std140: u[1].x is word 4 */
   c[0] = 0; c[1] = 555;
   VkDescriptorBufferInfo bi[4] = {{A.b, 0, 64}, {R.b, 0, VK_WHOLE_SIZE}, {U.b, 0, 32}, {C.b, 0, 8}};   /* ranges end well before the buffers do */
   VkWriteDescriptorSet w[3] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[0]},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[2]},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[3]}};
   UpdateDescriptorSets(dev, 3, w, 0, NULL);

   VkCommandPool pool;
   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CHECK(CreateCommandPool(dev, &cpi, NULL, &pool) == VK_SUCCESS, "pool");
   VkCommandBuffer cb;
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   CHECK(AllocateCommandBuffers(dev, &cai, &cb) == VK_SUCCESS, "cb");
   VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(BeginCommandBuffer(cb, &cbi) == VK_SUCCESS, "begin");
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   uint32_t pc[2] = {0, wild};
   CmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
   CmdDispatch(cb, 1, 1, 1);
   CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");
   fflush(stdout);
   if (!submit_wait(pool, cb, wild ? "wild" : "ssbo")) return 0;

   if (!g_fake) {
      for (unsigned i = 0; i < 8; i++) EXPECT(r[i] == 0, "OOB SSBO read a[%u] = 0x%x (want 0)", 16 + i, r[i]);
      EXPECT(r[8] == 111, "in-bounds SSBO read = %u (want 111)", r[8]);
      for (unsigned i = 0; i < 4; i++) EXPECT(r[10 + i] == 0, "OOB UBO read u[%u] = 0x%x (want 0)", 4 + i, r[10 + i]);
      EXPECT(r[14] == 222, "in-bounds UBO read = %u (want 222)", r[14]);
      if (!g_control) {
      EXPECT(r[15] == 16, "a.length() = %u (want 16)", r[15]);
      EXPECT(r[16] == 2, "c.length() = %u (want 2)", r[16]);
      }
      EXPECT(r[17] == 0, "OOB atomicAdd returned 0x%x (want 0)", r[17]);
      EXPECT(r[18] == 112, "in-bounds read a[2] = %u (want 112)", r[18]);
      EXPECT(r[19] == 555, "in-bounds C read = %u (want 555)", r[19]);
      for (unsigned i = 0; i < 24; i++) EXPECT(a[16 + i] == CANARY_A + i, "OOB write/atomic reached memory: a[%u] = 0x%x (canary 0x%x)", 16 + i, a[16 + i], CANARY_A + i);
      EXPECT(c[2] == CANARY_C, "OOB write to C reached memory: c[2] = 0x%x", c[2]);
      EXPECT(a[3] == 444, "in-bounds write lost: a[3] = %u (want 444)", a[3]);
      if (wild) {
         EXPECT(r[20] == 0, "far OOB read (1 GiB) = 0x%x (want 0)", r[20]);
         EXPECT(r[21] == 0, "far OOB read (index 0xFFFFFFF0) = 0x%x (want 0)", r[21]);
         EXPECT(r[22] == 0, "far OOB atomic returned 0x%x (want 0)", r[22]);
         EXPECT(r[23] == 7, "wild shader did not run to the end (r[23] = 0x%x)", r[23]);
      }
   }
   printf("{\"test\":\"%s\",\"ok\":%s,\"mismatches\":%d%s}\n", wild ? "wild" : "ssbo", bad ? "false" : "true", bad, g_fake ? ",\"skipped\":\"results: fake backend\"" : "");
   DestroyCommandPool(dev, pool, NULL);
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, pl, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   DestroyShaderModule(dev, sm, NULL);
   rmbuf(&A); rmbuf(&R); rmbuf(&U); rmbuf(&C);
   return 1;
}

/* Vertex-buffer OOB: 8 valid vertices (values 100..107) in a 32-byte VkBuffer followed by canaries in the same allocation, 16 drawn. */
static int
run_vertex(void)
{
   int bad = 0;
   enum { NV = 16, NVALID = 8 };
   struct buf V = {0}, RB = {0};
   VkShaderModule vs = load_spv("robust.vert.spv"), fs = load_spv("robust.frag.spv");
   CHECK(vs && fs, "load robust.vert/frag");
   CHECK(mkbuf(&V, NVALID * 4, BUFSZ, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0x77770000u), "vertex buf");
   for (unsigned i = 0; i < NVALID; i++) ((uint32_t *)V.p)[i] = 100 + i;
   for (unsigned i = NVALID; i < 64; i++) ((uint32_t *)V.p)[i] = 0x77770000u + i;
   CHECK(mkbuf(&RB, NV * 4, NV * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0xAAAAAAAAu), "readback buf");

   VkImage img;
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32_UINT, .extent = {NV, 1, 1},
      .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   CHECK(CreateImage(dev, &ii, NULL, &img) == VK_SUCCESS, "CreateImage");
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, img, &mr);
   int mt = memtype(mr.memoryTypeBits, 0);
   CHECK(mt >= 0, "image mem type");
   VkDeviceMemory im;
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
   CHECK(AllocateMemory(dev, &mai, NULL, &im) == VK_SUCCESS, "image mem");
   CHECK(BindImageMemory(dev, img, im, 0) == VK_SUCCESS, "bind image");
   VkImageView iv;
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32_UINT,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CHECK(CreateImageView(dev, &vi, NULL, &iv) == VK_SUCCESS, "view");

   VkAttachmentDescription ad = {.format = VK_FORMAT_R32_UINT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
   VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sd = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
   VkSubpassDependency dep = {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
   VkRenderPass rp;
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd,
      .dependencyCount = 1, .pDependencies = &dep};
   CHECK(CreateRenderPass(dev, &rpi, NULL, &rp) == VK_SUCCESS, "render pass");
   VkFramebuffer fb;
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &iv, .width = NV, .height = 1, .layers = 1};
   CHECK(CreateFramebuffer(dev, &fbi, NULL, &fb) == VK_SUCCESS, "framebuffer");

   VkPipelineLayout pl;
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &pl) == VK_SUCCESS, "layout");
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
   VkVertexInputBindingDescription vb = {0, 4, VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription va = {0, 0, VK_FORMAT_R32_UINT, 0};
   VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
      .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &va};
   VkPipelineInputAssemblyStateCreateInfo ias = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
   VkViewport vp = {0, 0, NV, 1, 0, 1};
   VkRect2D sc = {{0, 0}, {NV, 1}};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
   VkGraphicsPipelineCreateInfo gpi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vis,
      .pInputAssemblyState = &ias, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs, .layout = pl, .renderPass = rp};
   VkPipeline pipe;
   VkResult pr = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe);
   CHECK(pr == VK_SUCCESS, "CreateGraphicsPipelines = %d", pr);

   VkCommandPool pool;
   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CHECK(CreateCommandPool(dev, &cpi, NULL, &pool) == VK_SUCCESS, "pool");
   VkCommandBuffer cb;
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   CHECK(AllocateCommandBuffers(dev, &cai, &cb) == VK_SUCCESS, "cb");
   VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(BeginCommandBuffer(cb, &cbi) == VK_SUCCESS, "begin");
   VkClearValue cv = {.color.uint32 = {0xDEADBEEFu, 0, 0, 0}};
   VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {NV, 1}}, .clearValueCount = 1, .pClearValues = &cv};
   CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   VkDeviceSize off = 0;
   CmdBindVertexBuffers(cb, 0, 1, &V.b, &off);
   CmdDraw(cb, NV, 1, 0, 0);
   CmdEndRenderPass(cb);
   VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {NV, 1, 1}};
   CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, RB.b, 1, &bc);
   CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");
   fflush(stdout);
   if (!submit_wait(pool, cb, "vertex")) return 0;

   if (!g_fake) {
      uint32_t *px = RB.p;
      for (unsigned i = 0; i < NVALID; i++) EXPECT(px[i] == 100 + i, "valid vertex %u fetched %u (want %u)", i, px[i], 100 + i);
      for (unsigned i = NVALID; i < NV; i++) EXPECT(px[i] == 0, "OOB vertex %u fetched 0x%x (want 0)", i, px[i]);
   }
   printf("{\"test\":\"vertex\",\"ok\":%s,\"mismatches\":%d%s}\n", bad ? "false" : "true", bad, g_fake ? ",\"skipped\":\"results: fake backend\"" : "");
   DestroyCommandPool(dev, pool, NULL);
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, pl, NULL);
   DestroyFramebuffer(dev, fb, NULL);
   DestroyRenderPass(dev, rp, NULL);
   DestroyImageView(dev, iv, NULL);
   DestroyImage(dev, img, NULL);
   FreeMemory(dev, im, NULL);
   DestroyShaderModule(dev, vs, NULL);
   DestroyShaderModule(dev, fs, NULL);
   rmbuf(&V); rmbuf(&RB);
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
      else { fprintf(stderr, "usage: MALI_TEST_FORCE_ROBUST=1 t_vk_robust --direct icd.so [--only ssbo|vertex|wild] [--spv-dir d]\n"); return 2; }
   }
   if (!icd) { fprintf(stderr, "--direct required\n"); return 2; }
   const char *be = getenv("MALI_BACKEND");
   g_fake = be && !strcmp(be, "fake");
   const char *fr = getenv("MALI_TEST_FORCE_ROBUST");
   if (!fr || (fr[0] != '1' && fr[0] != '0' && fr[0] != 'f')) {
      printf("{\"test\":\"robust\",\"ok\":true,\"skipped\":\"MALI_TEST_FORCE_ROBUST not set (=1 forces robust lowering, =f enables the advertised robustBufferAccess feature, =0 control run)\"}\n{\"pass\":true}\n");
      return 0;
   }
   g_control = fr[0] == '0';

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("{\"fail\":\"dlopen: %s\"}\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("{\"fail\":\"icd entry\"}\n"); return 1; }

   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices EPD = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceMemoryProperties GMP = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkGetPhysicalDeviceProperties GPP = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
   PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr GDPA = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   PFN_vkDestroyInstance DestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   VkResult r = EPD(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   VkPhysicalDeviceProperties pp;
   GPP(pd, &pp);
   GMP(pd, &memprops);
   printf("{\"device\":\"%s\",\"force_robust\":\"%s\"}\n", pp.deviceName, fr);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkPhysicalDeviceFeatures feats = {.robustBufferAccess = VK_TRUE};   /* =f: the real core feature (advertised), no env hook in the ICD */
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .pEnabledFeatures = fr[0] == 'f' ? &feats : NULL};
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GDPA(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   DEV_FNS(LOAD)
   GetDeviceQueue(dev, 0, 0, &queue);

   int ok = 1;
   if (CASE_ON("ssbo")) ok &= run_compute(0);
   if (ok && CASE_ON("vertex")) ok &= run_vertex();
   /* last: a fault here must not hide the results above */
   if (ok && CASE_ON("wild") && !g_control) ok &= run_compute(1);

   DeviceWaitIdle(dev);
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   dlclose(lib);
   printf("{\"pass\":%s}\n", (ok && !g_fail) ? "true" : "false");
   return (ok && !g_fail) ? 0 : 1;
}
