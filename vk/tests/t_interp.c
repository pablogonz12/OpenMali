/* SPDX-License-Identifier: MIT */
/* Interpolation qualifier sweep: smooth / noperspective / flat x plain / centroid / sample, each with and without
 * `invariant` on the vertex output, drawn as one triangle whose per-vertex colour (v0 red, v1 green, v2 blue) and
 * per-vertex clip w (1, 2, 3) make every mode a different picture at every interior pixel, and drawn a second time
 * with the index order reversed so `flat` must take the other provoking vertex. Every case is compared pixel by
 * pixel against a CPU reference (screen-space barycentrics; perspective-correct for smooth, affine for
 * noperspective, provoking vertex for flat). The provoking vertex of a triangle list is the last vertex (Vulkan's
 * default: VK_EXT_provoking_vertex's provokingVertexLast is not enabled here, and t_vk_feat/ provoking covers the
 * FIRST/LAST switch), so the reversed draw must read v0 red.
 *
 * Usage: t_interp --direct <icd.so> [--spv-dir dir] [--only <mode>] [-v]
 *        MALI_BACKEND=fake: pipelines are created and drawn, pixels are skipped ("skipped":"pixels: fake backend").
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 128
#define H 128
/* A channel off by more than this many 8-bit steps is wrong. The modes differ by tens of steps at the centre
 * (perspective-correct (139,70,46) vs affine (85,85,85)), so the tolerance cannot hide a wrong mode; it covers
 * 8-bit rounding and interpolation precision only. */
#define TOL 4
#define NVERT 3

struct vert { float p[2], w, c[3]; };

static const struct vert g_verts[NVERT] = {
   {{-0.9f, -0.9f}, 1.0f, {1, 0, 0}},   /* v0 red */
   {{ 0.9f, -0.9f}, 2.0f, {0, 1, 0}},   /* v1 green */
   {{ 0.0f,  0.9f}, 3.0f, {0, 0, 1}},   /* v2 blue */
};

static const char *g_spv = "shader", *g_only;
static int g_verbose, g_fail;
static struct { const char *mode, *decor; } g_modes[] = {
   {"smooth", "plain"}, {"smooth", "centroid"}, {"smooth", "sample"},
   {"noperspective", "plain"}, {"noperspective", "centroid"}, {"noperspective", "sample"},
   {"flat", "plain"}, {"flat", "centroid"}, {"flat", "sample"},
};
#define NMODES (sizeof g_modes / sizeof g_modes[0])

#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                        \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                               \
      }                                                                                            \
   } while (0)

#define DEV_FNS(X)                                                                                 \
   X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(CreateImageView) X(DestroyImageView) \
   X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) \
   X(UnmapMemory) X(InvalidateMappedMemoryRanges) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) \
   X(GetImageSubresourceLayout) X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) \
   X(DestroyFramebuffer) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
   X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass) \
   X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor) X(CmdBindVertexBuffers) \
   X(CmdBindIndexBuffer) X(CmdDrawIndexed) X(CreateShaderModule) X(DestroyShaderModule) \
   X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(DestroyPipeline) \
   X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(GetDeviceQueue) X(QueueSubmit) \
   X(DeviceWaitIdle)
#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
#undef DECL
static PFN_vkDestroyDevice DestroyDevice_;
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;
static int g_fake;

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
   VkResult r = CreateShaderModule(dev, &ci, NULL, &m);
   free(buf);
   if (r != VK_SUCCESS) { printf("{\"fail\":\"vkCreateShaderModule %s = %d\"}\n", name, r); return VK_NULL_HANDLE; }
   return m;
}

/* Render target: linear RGBA8, host-readable through its own mapping (the recipe of t_vk_feat/t_vk_draw). */
static struct {
   VkImage img; VkDeviceMemory mem; VkImageView view; VkRenderPass rp; VkFramebuffer fb;
   uint8_t *px; uint32_t pitch; int coherent; VkDeviceSize size;
} T;

static int
tgt_create(void)
{
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                           .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
                           .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   CHECK(CreateImage(dev, &ii, NULL, &T.img) == VK_SUCCESS, "colour image");
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, T.img, &mr);
   T.size = mr.size;
   int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   CHECK(mt >= 0, "no host-visible memory type for the colour image");
   T.coherent = (memprops.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt};
   CHECK(AllocateMemory(dev, &ai, NULL, &T.mem) == VK_SUCCESS, "colour memory");
   CHECK(BindImageMemory(dev, T.img, T.mem, 0) == VK_SUCCESS, "bind colour");
   void *map = NULL;
   CHECK(MapMemory(dev, T.mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS, "map colour");
   VkImageSubresource sr = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubresourceLayout sl;
   GetImageSubresourceLayout(dev, T.img, &sr, &sl);
   T.pitch = (uint32_t)sl.rowPitch;
   T.px = (uint8_t *)map + sl.offset;
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = T.img,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CHECK(CreateImageView(dev, &vi, NULL, &T.view) == VK_SUCCESS, "colour view");
   VkAttachmentDescription ad = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
                                 .subpassCount = 1, .pSubpasses = &sp};
   CHECK(CreateRenderPass(dev, &rpi, NULL, &T.rp) == VK_SUCCESS, "render pass");
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = T.rp, .attachmentCount = 1,
                                  .pAttachments = &T.view, .width = W, .height = H, .layers = 1};
   CHECK(CreateFramebuffer(dev, &fbi, NULL, &T.fb) == VK_SUCCESS, "framebuffer");
   return 1;
}

static VkBuffer g_vbo, g_ibo;
static VkDeviceMemory g_vmem, g_imem;

static void
buf_upload(VkBuffer *b, VkDeviceMemory *m, const void *data, VkDeviceSize size, VkBufferUsageFlags usage)
{
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
   CHECK(CreateBuffer(dev, &bi, NULL, b) == VK_SUCCESS, "buffer");
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, *b, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = (uint32_t)memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   CHECK(AllocateMemory(dev, &ai, NULL, m) == VK_SUCCESS, "buffer memory");
   CHECK(BindBufferMemory(dev, *b, *m, 0) == VK_SUCCESS, "bind buffer");
   void *p = NULL;
   CHECK(MapMemory(dev, *m, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS, "map buffer");
   memcpy(p, data, (size_t)size);
   UnmapMemory(dev, *m);
}

/* one pipeline for a shader pair */
static VkPipeline
make_pipe(const char *vs, const char *fs, VkPipelineLayout *lay)
{
   VkShaderModule v = load_spv(vs), f = load_spv(fs);
   if (!v || !f) return VK_NULL_HANDLE;
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   CHECK(CreatePipelineLayout(dev, &pli, NULL, lay) == VK_SUCCESS, "pipeline layout");
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = v, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = f, .pName = "main"}};
   VkVertexInputBindingDescription vb = {.binding = 0, .stride = sizeof(struct vert), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription va[3] = {
      {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(struct vert, p)},
      {.location = 1, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(struct vert, w)},
      {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(struct vert, c)}};
   VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
                                               .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va};
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
                                      .pDynamicState = &dys, .layout = *lay, .renderPass = T.rp};
   VkPipeline pipe = VK_NULL_HANDLE;
   VkResult r = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe);
   DestroyShaderModule(dev, v, NULL);
   DestroyShaderModule(dev, f, NULL);
   if (r != VK_SUCCESS) { printf("{\"fail\":\"vkCreateGraphicsPipelines %s %s = %d\"}\n", vs, fs, r); return VK_NULL_HANDLE; }
   return pipe;
}

static VkFence g_fence;
static VkCommandPool g_pool;

/* One draw: clear the whole target to black, draw the triangle with the given index order.
 * rev=1 draws 2,1,0: the same triangle (cull is off) with v0 as the provoking vertex. */
static int
draw(VkPipeline pipe, VkPipelineLayout lay, int rev)
{
   VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cb = VK_NULL_HANDLE;
   CHECK(AllocateCommandBuffers(dev, &cbi, &cb) == VK_SUCCESS, "command buffer");
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(BeginCommandBuffer(cb, &bi) == VK_SUCCESS, "begin");
   VkClearValue clr = {.color = {{0, 0, 0, 1}}};
   VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = T.rp, .framebuffer = T.fb,
                                .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &clr};
   CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   VkViewport vp = {0, 0, W, H, 0, 1};
   VkRect2D sc = {{0, 0}, {W, H}};
   CmdSetViewport(cb, 0, 1, &vp);
   CmdSetScissor(cb, 0, 1, &sc);
   VkDeviceSize off = 0;
   CmdBindVertexBuffers(cb, 0, 1, &g_vbo, &off);
   CmdBindIndexBuffer(cb, g_ibo, (VkDeviceSize)(rev ? 3 : 0) * sizeof(uint16_t), VK_INDEX_TYPE_UINT16);
   CmdDrawIndexed(cb, 3, 1, 0, 0, 0);
   CmdEndRenderPass(cb);
   CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   CHECK(QueueSubmit(queue, 1, &si, g_fence) == VK_SUCCESS, "submit");
   CHECK(WaitForFences(dev, 1, &g_fence, VK_TRUE, 20000000000ull) == VK_SUCCESS, "wait");
   ResetFences(dev, 1, &g_fence);
   FreeCommandBuffers(dev, g_pool, 1, &cb);
   DeviceWaitIdle(dev);
   if (!T.coherent) {
      VkMappedMemoryRange mr = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = T.mem, .offset = 0, .size = VK_WHOLE_SIZE};
      InvalidateMappedMemoryRanges(dev, 1, &mr);
   }
   return 1;
}

static const uint8_t *
px(int x, int y) { return T.px + (size_t)y * T.pitch + (size_t)x * 4; }

/* screen-space barycentrics of the triangle in NDC (perspective divide already applied) */
static void
bary(double x, double y, double l[3])
{
   const double ax = g_verts[0].p[0], ay = g_verts[0].p[1];
   const double bx = g_verts[1].p[0], by = g_verts[1].p[1];
   const double cx = g_verts[2].p[0], cy = g_verts[2].p[1];
   double d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
   l[0] = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / d;
   l[1] = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / d;
   l[2] = 1.0 - l[0] - l[1];
}

/* CPU reference: colour at NDC (x, y) for a mode */
static void
ref_colour(const char *mode, const double l[3], int prov, double rgb[3])
{
   if (!strcmp(mode, "flat")) {
      for (int c = 0; c < 3; c++) rgb[c] = g_verts[prov].c[c];
      return;
   }
   double w[NVERT], sum = 0.0;
   for (int i = 0; i < NVERT; i++) { w[i] = l[i]; sum += l[i]; }
   if (!strcmp(mode, "smooth")) {
      /* perspective-correct: interpolate c/w and 1/w in screen space, then divide */
      for (int i = 0; i < NVERT; i++) w[i] = l[i] / g_verts[i].w;
      sum = w[0] + w[1] + w[2];
   }
   for (int c = 0; c < 3; c++) {
      double v = 0.0;
      for (int i = 0; i < NVERT; i++) v += w[i] * g_verts[i].c[c];
      rgb[c] = v / sum;
   }
}

/* pixel whose centre is NDC (x, y) */
static void
ndc_to_px(double x, double y, int *px_, int *py_)
{
   *px_ = (int)floor((x * 0.5 + 0.5) * W);
   *py_ = (int)floor((y * 0.5 + 0.5) * H);
}

static int
run_case(int mi, int inv, int rev)
{
   char vs[64], fs[64];
   const char *mode = g_modes[mi].mode, *decor = g_modes[mi].decor;
   snprintf(vs, sizeof vs, "interp_%s_%s%s.vert.spv", mode, decor, inv ? "_inv" : "");
   snprintf(fs, sizeof fs, "interp_%s_%s.frag.spv", mode, decor);
   VkPipelineLayout lay = VK_NULL_HANDLE;
   VkPipeline pipe = make_pipe(vs, fs, &lay);
   if (!pipe) { g_fail = 1; return 0; }
   if (g_fake) {
      /* The fake backend runs the whole path (SPIR-V compile, varying setup, pipeline record) but rasterises nothing. */
      printf("{\"test\":\"interp\",\"case\":\"%s/%s%s\",\"prov\":\"%s\",\"skipped\":\"pixels: fake backend (pipeline created)\",\"ok\":true}\n",
             mode, decor, inv ? "/invariant" : "", rev ? "v2" : "v0");
      DestroyPipeline(dev, pipe, NULL);
      DestroyPipelineLayout(dev, lay, NULL);
      return 1;
   }
   draw(pipe, lay, rev);

   int prov = rev ? 2 : 0;   /* no provokingVertexMode struct: FIRST vertex (Vulkan default), i.e. the first index */
   /* pending buffer of the previous case must not be read: the draw above cleared the target */
   int bad = 0, n = 0, maxerr = 0, firstx = -1, firsty = -1;
   uint8_t firstgot[4] = {0};
   for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
         double nx = (x + 0.5) * 2.0 / W - 1.0, ny = (y + 0.5) * 2.0 / H - 1.0, l[3];
         bary(nx, ny, l);
         if (l[0] < 0.02 || l[1] < 0.02 || l[2] < 0.02) continue;   /* >2 px from every edge */
         double want[3], got[3];
         ref_colour(mode, l, prov, want);
         const uint8_t *p = px(x, y);
         for (int c = 0; c < 3; c++) got[c] = p[c] / 255.0;
         n++;
         for (int c = 0; c < 3; c++) {
            int e = (int)lround(fabs(got[c] - want[c]) * 255.0);
            if (e > maxerr) maxerr = e;
            if (e > TOL) {
               bad++;
               if (firstx < 0) { firstx = x; firsty = y; memcpy(firstgot, p, 4); }
               break;
            }
         }
         if (l[0] < 0.02 || l[1] < 0.02 || l[2] < 0.02) continue;
      }
   }
   /* centre and one point inside each vertex, for the notes' table */
   double pts[4][2] = {{0.0, -0.9 * 0.1}, {0, 0}, {0, 0}, {0, 0}};
   double gc[3] = {(g_verts[0].p[0] + g_verts[1].p[0] + g_verts[2].p[0]) / 3.0,
                   (g_verts[0].p[1] + g_verts[1].p[1] + g_verts[2].p[1]) / 3.0, 0};
   for (int i = 0; i < NVERT; i++) {
      pts[i + 1][0] = g_verts[i].p[0] + 0.3 * (gc[0] - g_verts[i].p[0]);
      pts[i + 1][1] = g_verts[i].p[1] + 0.3 * (gc[1] - g_verts[i].p[1]);
   }
   pts[0][0] = gc[0]; pts[0][1] = gc[1];
   printf("{\"test\":\"interp\",\"case\":\"%s/%s%s\",\"prov\":\"%s\",\"centre\":[", mode, decor,
          inv ? "/invariant" : "", rev ? "v2" : "v0");
   double want[3], l[3];
   for (int i = 0; i < 4; i++) {
      int xx, yy;
      ndc_to_px(pts[i][0], pts[i][1], &xx, &yy);
      const uint8_t *p = px(xx, yy);
      if (i) printf(",");
      printf("[%u,%u,%u]", p[0], p[1], p[2]);
   }
   printf("],\"want\":[");
   for (int i = 0; i < 4; i++) {
      int xx, yy;
      ndc_to_px(pts[i][0], pts[i][1], &xx, &yy);
      (void)xx; (void)yy;
      bary(pts[i][0], pts[i][1], l);
      ref_colour(mode, l, prov, want);
      if (i) printf(",");
      printf("[%d,%d,%d]", (int)lround(want[0] * 255), (int)lround(want[1] * 255), (int)lround(want[2] * 255));
   }
   int ok = bad == 0;
   printf("],\"bad\":%d,\"n\":%d,\"maxerr\":%d,\"ok\":%s}\n", bad, n, maxerr, ok ? "true" : "false");
   if (g_verbose)
      printf("  case %s/%s%s prov=%s: %d of %d pixels off by >%d (max %d)%s\n", mode, decor,
             inv ? "/invariant" : "", rev ? "v2" : "v0", bad, n, TOL, maxerr,
             firstx >= 0 ? "" : "");
   if (!ok) {
      printf("  first wrong pixel (%d,%d) got %u,%u,%u\n", firstx, firsty, firstgot[0], firstgot[1], firstgot[2]);
      g_fail = 1;
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   return ok;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
      else if (!strcmp(argv[i], "-v")) g_verbose = 1;
      else { printf("usage: t_interp --direct <icd.so> [--spv-dir dir] [--only mode] [-v]\n"); return 2; }
   }
   if (!icd) { printf("usage: t_interp --direct <icd.so> [--spv-dir dir] [--only mode] [-v]\n"); return 2; }
   const char *be = getenv("MALI_BACKEND");
   g_fake = be && !strcmp(be, "fake");

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("{\"fail\":\"dlopen: %s\"}\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { printf("{\"fail\":\"no vk_icdGetInstanceProcAddr\"}\n"); return 1; }
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "interp-sweep", .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst = VK_NULL_HANDLE;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"vkCreateInstance\"}\n"); return 1; }
#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceProperties); I(GetPhysicalDeviceFeatures);
   I(GetPhysicalDeviceMemoryProperties); I(CreateDevice); I(GetDeviceProcAddr);
   uint32_t nd = 1;
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   if (EnumeratePhysicalDevices(inst, &nd, &pd) != VK_SUCCESS || nd < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   VkPhysicalDeviceProperties pr;
   GetPhysicalDeviceProperties(pd, &pr);
   printf("{\"device\":\"%s\"}\n", pr.deviceName);
   VkPhysicalDeviceFeatures feat;
   GetPhysicalDeviceFeatures(pd, &feat);
   GetPhysicalDeviceMemoryProperties(pd, &memprops);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   /* `sample` inputs carry OpCapability SampleRateShading, so the device must have the feature enabled. */
   VkPhysicalDeviceFeatures want = {.sampleRateShading = feat.sampleRateShading};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                             .pEnabledFeatures = &want};
   if (CreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"vkCreateDevice\"}\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   DEV_FNS(LOAD)
#undef LOAD
   DestroyDevice_ = (PFN_vkDestroyDevice)GetDeviceProcAddr(dev, "vkDestroyDevice");
   GetDeviceQueue(dev, 0, 0, &queue);

   if (!tgt_create()) return 1;
   buf_upload(&g_vbo, &g_vmem, g_verts, sizeof g_verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
   uint16_t idx[6] = {0, 1, 2, 2, 1, 0};
   buf_upload(&g_ibo, &g_imem, idx, sizeof idx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CreateCommandPool(dev, &pci, NULL, &g_pool);
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CreateFence(dev, &fi, NULL, &g_fence);

   int ncase = 0, nfail = 0;
   for (unsigned mi = 0; mi < NMODES; mi++) {
      if (g_only && strcmp(g_only, g_modes[mi].mode)) continue;
      if (!strcmp(g_modes[mi].decor, "sample") && !feat.sampleRateShading) {
         printf("{\"test\":\"interp\",\"case\":\"%s/%s\",\"skipped\":\"sampleRateShading not supported by this device\",\"ok\":true}\n",
                g_modes[mi].mode, g_modes[mi].decor);
         continue;
      }
      for (int inv = 0; inv < 2; inv++) {
         for (int rev = 0; rev < 2; rev++) {
            ncase++;
            if (!run_case((int)mi, inv, rev)) nfail++;
         }
      }
   }
   printf("{\"summary\":\"interp\",\"cases\":%d,\"fail\":%d,\"fake\":%s,\"ok\":%s}\n", ncase, nfail,
          g_fake ? "true" : "false", (g_fail || nfail) ? "false" : "true");
   if (DestroyDevice_) DestroyDevice_(dev, NULL);
   return (g_fail || nfail) ? 1 : 0;
}
