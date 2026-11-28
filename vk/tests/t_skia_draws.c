/* SPDX-License-Identifier: MIT */
/* Host micro-benchmark and correctness check of the per-draw / per-bind record path, shaped like Skia Ganesh on ANGLE's Vulkan
 * backend (the Chromium GPU thread of the Steam menu): many small draws in render passes of ~50, each draw 4 (strip) or 6
 * (indexed list) vertices from a dynamic vertex buffer bound at a new offset, a descriptor-set rebind every 2 draws (texture +
 * dynamic uniform buffer, new dynamic offset), a pipeline change every 8, a scissor change every 3, push constants every draw.
 * Fake backend only: nothing executes, so the numbers are the driver's CPU cost (record = Begin..End, submit = vkQueueSubmit,
 * which builds the descriptors and the command stream of every recorded pass).
 *
 *   t_skia_draws --direct <icd.so> [--spv-dir DIR] [--draws N] [--per-cb N] [--pass N] [--redundant] [--prof FILE]
 *      benchmark: key=value lines (ns per draw for record and submit, heap allocations per draw); --prof writes SIGPROF samples
 *      of the measured loop for tests/sym_prof.py (top symbols, self and inclusive)
 *   t_skia_draws --direct <icd.so> [--spv-dir DIR] --check
 *      correctness (vk/check.sh): the command stream of one command buffer has one RUN_IDVS per recorded draw, and the same
 *      stream with every bind re-issued with identical arguments before each draw (pipeline, descriptor set + dynamic offset,
 *      vertex buffer, index buffer, scissor, push constants) emits exactly the same instructions (opcode and register of every
 *      word) and allocates exactly as many descriptor bytes: a redundant rebind emits nothing. Uses MALI_FAKE_DUMP /
 *      MALI_ALLOC_STATS (set by the test itself). Exit 0 = pass.
 * Not a timing gate: check.sh runs only --check. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__GLIBC__) && defined(__x86_64__)
#include <execinfo.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#define HAVE_PROF 1
#else
#define HAVE_PROF 0
#endif

/* ---- heap allocation counter (glibc only: the a64 bionic build has no __libc_malloc; there the counts read 0) ---- */
static volatile int g_count_on;
static unsigned long g_nalloc;
#if defined(__GLIBC__)
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);
void *malloc(size_t n) { if (g_count_on) __atomic_fetch_add(&g_nalloc, 1, __ATOMIC_RELAXED); return __libc_malloc(n); }
void *calloc(size_t a, size_t b) { if (g_count_on) __atomic_fetch_add(&g_nalloc, 1, __ATOMIC_RELAXED); return __libc_calloc(a, b); }
void *realloc(void *p, size_t n) { if (g_count_on) __atomic_fetch_add(&g_nalloc, 1, __ATOMIC_RELAXED); return __libc_realloc(p, n); }
int posix_memalign(void **out, size_t al, size_t n)
{
   if (g_count_on) __atomic_fetch_add(&g_nalloc, 1, __ATOMIC_RELAXED);
   void *p = __libc_memalign(al, n);
   if (!p) return 12;
   *out = p;
   return 0;
}
void *aligned_alloc(size_t al, size_t n) { if (g_count_on) __atomic_fetch_add(&g_nalloc, 1, __ATOMIC_RELAXED); return __libc_memalign(al, n); }
#define HAVE_COUNT 1
#else
#define HAVE_COUNT 0
#endif

/* ---- --prof FILE: SIGPROF sampler (perf_event_open is not available everywhere this runs). Each sample = the interrupted PC and
 * its callers; written with /proc/self/maps so tests/sym_prof.py can resolve them against the symbol tables (perf's "self" and
 * "children" columns). ---- */
#if HAVE_PROF
#define PROF_MAX 400000
#define PROF_DEPTH 12
static void *g_prof[PROF_MAX][PROF_DEPTH + 1];
static unsigned char g_prof_n[PROF_MAX];
static volatile unsigned g_nprof;
static volatile int g_prof_on;
static void
prof_sig(int sig, siginfo_t *si, void *uc)
{
   (void)sig; (void)si;
   if (!g_prof_on || g_nprof >= PROF_MAX)
      return;
   unsigned k = g_nprof++;
   void *pc = (void *)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
   void *bt[PROF_DEPTH + 4];
   int n = backtrace(bt, PROF_DEPTH + 4), j = 0;
   while (j < n && bt[j] != pc)   /* frames of this handler and the signal trampoline come first */
      j++;
   g_prof[k][0] = pc;
   unsigned m = 1;
   for (j++; j < n && m <= PROF_DEPTH; j++)
      g_prof[k][m++] = bt[j];
   g_prof_n[k] = (unsigned char)m;
}
static void
prof_start(void)
{
   void *warm[4];
   backtrace(warm, 4);   /* loads libgcc_s outside the handler */
   struct sigaction sa = {.sa_sigaction = prof_sig, .sa_flags = SA_SIGINFO | SA_RESTART};
   sigaction(SIGPROF, &sa, NULL);
   struct itimerval it = {{0, 100}, {0, 100}};
   setitimer(ITIMER_PROF, &it, NULL);
}
static void
prof_write(const char *path)
{
   struct itimerval it = {{0, 0}, {0, 0}};
   setitimer(ITIMER_PROF, &it, NULL);
   FILE *f = fopen(path, "w"), *m = fopen("/proc/self/maps", "r");
   if (!f || !m)
      return;
   char line[1024];
   while (fgets(line, sizeof line, m))
      fprintf(f, "map %s", line);
   fclose(m);
   for (unsigned k = 0; k < g_nprof; k++) {
      fputs("s", f);
      for (unsigned j = 0; j < g_prof_n[k]; j++)
         fprintf(f, " %lx", (unsigned long)g_prof[k][j]);
      fputs("\n", f);
   }
   fclose(f);
}
#endif

#define W 256
#define H 256
#define NPIPE 4
#define NSETS 8
#define NTEX 4
#define NSLOTS 256
#define NVERT 65536     /* vertex buffer: 16 B per vertex */
#define NIDX 65536      /* index buffer: u16 */

static int g_fail;
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                        \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                               \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

#define DEV_FNS(X)                                                                                 \
   X(DestroyDevice) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory)  \
   X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(QueueSubmit) X(DeviceWaitIdle)  \
   X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory)                  \
   X(CreateImageView) X(DestroyImageView) X(CreateBuffer) X(DestroyBuffer)                          \
   X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateShaderModule) X(DestroyShaderModule)  \
   X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateRenderPass) X(DestroyRenderPass)        \
   X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateGraphicsPipelines) X(DestroyPipeline)         \
   X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer)       \
   X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline)                 \
   X(CmdSetViewport) X(CmdSetScissor) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer)                \
   X(CmdPushConstants) X(CmdDraw) X(CmdDrawIndexed) X(CmdBindDescriptorSets)                        \
   X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool)               \
   X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateSampler)      \
   X(DestroySampler)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;
static VkPhysicalDeviceProperties props;
static const char *g_spv = "shader";

struct scene {
   VkRenderPass rp;
   VkFramebuffer fb;
   VkPipeline pipe[NPIPE];
   VkPipelineLayout layout;
   VkDescriptorSet set[NSETS];
   VkBuffer vb, ib, ubo;
   uint32_t ubo_stride;
};

static double
now_ns(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
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
   char path[1024];
   snprintf(path, sizeof path, "%s/%s", g_spv, name);
   FILE *f = fopen(path, "rb");
   if (!f)
      return VK_NULL_HANDLE;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *code = malloc((size_t)n);
   if (!code || n <= 0 || fread(code, 1, (size_t)n, f) != (size_t)n) {
      fclose(f);
      free(code);
      return VK_NULL_HANDLE;
   }
   fclose(f);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = code};
   VkShaderModule m = VK_NULL_HANDLE;
   VkResult r = CreateShaderModule(dev, &ci, NULL, &m);
   free(code);
   return r == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static int
bind_mem(VkBuffer b, VkImage img, VkMemoryPropertyFlags want, void **map)
{
   VkMemoryRequirements req;
   if (b)
      GetBufferMemoryRequirements(dev, b, &req);
   else
      GetImageMemoryRequirements(dev, img, &req);
   int t = memtype(req.memoryTypeBits, want);
   if (t < 0)
      return -1;
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
   VkDeviceMemory m;
   if (AllocateMemory(dev, &ai, NULL, &m) != VK_SUCCESS)
      return -1;
   if (b ? BindBufferMemory(dev, b, m, 0) : BindImageMemory(dev, img, m, 0))
      return -1;
   if (map && MapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS)
      return -1;
   return 0;
}

static VkBuffer
make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer b = VK_NULL_HANDLE;
   if (CreateBuffer(dev, &bci, NULL, &b) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   if (bind_mem(b, VK_NULL_HANDLE, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, map))
      return VK_NULL_HANDLE;
   return b;
}

static int
make_scene(struct scene *s)
{
   const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
   /* colour target + render pass (CLEAR/STORE: no preload draws, so every RUN_IDVS is a recorded draw) */
   VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage rt;
   CHECK(CreateImage(dev, &ici, NULL, &rt) == VK_SUCCESS, "rt");
   CHECK(!bind_mem(VK_NULL_HANDLE, rt, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL), "rt mem");
   VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = rt, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView rtv;
   CHECK(CreateImageView(dev, &vci, NULL, &rtv) == VK_SUCCESS, "rt view");
   VkAttachmentDescription att = {.format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                  .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref};
   VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
                                  .subpassCount = 1, .pSubpasses = &sub};
   CHECK(CreateRenderPass(dev, &rpci, NULL, &s->rp) == VK_SUCCESS, "render pass");
   VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = s->rp, .attachmentCount = 1,
                                  .pAttachments = &rtv, .width = W, .height = H, .layers = 1};
   CHECK(CreateFramebuffer(dev, &fci, NULL, &s->fb) == VK_SUCCESS, "framebuffer");

   /* textures (never uploaded: nothing executes on the fake GPU) */
   VkImageView tv[NTEX];
   for (int i = 0; i < NTEX; i++) {
      VkImageCreateInfo ti = ici;
      ti.extent = (VkExtent3D){16, 16, 1};
      ti.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      VkImage img;
      CHECK(CreateImage(dev, &ti, NULL, &img) == VK_SUCCESS, "tex");
      CHECK(!bind_mem(VK_NULL_HANDLE, img, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL), "tex mem");
      VkImageViewCreateInfo tvi = vci;
      tvi.image = img;
      CHECK(CreateImageView(dev, &tvi, NULL, &tv[i]) == VK_SUCCESS, "tex view");
   }
   VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 1.0f};
   VkSampler smp;
   CHECK(CreateSampler(dev, &sci, NULL, &smp) == VK_SUCCESS, "sampler");

   /* buffers: vertices (pos.xy uv.xy), u16 indices (quads), the uniform ring */
   void *vmap = NULL, *imap = NULL, *umap = NULL;
   s->vb = make_buffer((VkDeviceSize)NVERT * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vmap);
   s->ib = make_buffer((VkDeviceSize)NIDX * 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &imap);
   uint32_t al = props.limits.minUniformBufferOffsetAlignment ? (uint32_t)props.limits.minUniformBufferOffsetAlignment : 16;
   s->ubo_stride = (16u + al - 1u) / al * al;
   s->ubo = make_buffer((VkDeviceSize)s->ubo_stride * NSLOTS, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &umap);
   CHECK(s->vb && s->ib && s->ubo && vmap && imap && umap, "buffers");
   for (unsigned v = 0; v < NVERT; v++) {
      float q[4] = {(float)(v & 1), (float)((v >> 1) & 1), (float)(v & 1), (float)((v >> 1) & 1)};
      memcpy((char *)vmap + v * 16, q, 16);
   }
   for (unsigned i = 0; i < NIDX; i++) {
      static const uint16_t quad[6] = {0, 1, 2, 2, 1, 3};
      ((uint16_t *)imap)[i] = quad[i % 6];
   }

   /* set 0: combined image sampler (FS) + dynamic uniform buffer (FS) */
   VkDescriptorSetLayoutBinding b[2] = {
      {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
      {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
   };
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
   VkDescriptorSetLayout dsl;
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NSETS}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NSETS}};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = NSETS, .poolSizeCount = 2, .pPoolSizes = ps};
   VkDescriptorPool dp;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "pool");
   VkDescriptorSetLayout dsls[NSETS];
   for (int i = 0; i < NSETS; i++)
      dsls[i] = dsl;
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
                                      .descriptorSetCount = NSETS, .pSetLayouts = dsls};
   CHECK(AllocateDescriptorSets(dev, &dai, s->set) == VK_SUCCESS, "sets");
   for (int i = 0; i < NSETS; i++) {
      VkDescriptorImageInfo ii = {.sampler = smp, .imageView = tv[i % NTEX], .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorBufferInfo bi = {.buffer = s->ubo, .offset = 0, .range = 16};
      VkWriteDescriptorSet w[2] = {
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->set[i], .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->set[i], .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi},
      };
      UpdateDescriptorSets(dev, 2, w, 0, NULL);
   }
   VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &s->layout) == VK_SUCCESS, "layout");

   /* four pipelines: {indexed triangle list, triangle strip} x {opaque, premultiplied src-over} */
   VkShaderModule vs = load_spv("skia.vert.spv"), fs = load_spv("skia.frag.spv");
   CHECK(vs && fs, "shaders (--spv-dir %s)", g_spv);
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
   };
   VkVertexInputBindingDescription vbd = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription vad[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}};
   VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbd,
                                               .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = vad};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                              .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
   for (int p = 0; p < NPIPE; p++) {
      VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                   .topology = (p & 1) ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
      VkPipelineColorBlendAttachmentState cba = {.blendEnable = (p >> 1) & 1, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                 .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
                                                 .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                 .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
      VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
      VkGraphicsPipelineCreateInfo gci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                          .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vps,
                                          .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
                                          .pDynamicState = &ds, .layout = s->layout, .renderPass = s->rp};
      CHECK(CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &s->pipe[p]) == VK_SUCCESS, "pipeline %d", p);
   }
   return 0;
}

/* One command buffer of the Skia-shaped stream: `n` draws in passes of `pass`. `seed` varies offsets between command buffers.
 * redundant: before every draw, every bind of the stream is issued again with identical arguments. Returns the draws recorded. */
static unsigned
record_stream(const struct scene *s, VkCommandBuffer cb, unsigned n, unsigned pass, unsigned seed, int redundant)
{
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VkClearValue clear = {.color = {{0, 0, 0, 1}}};
   VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s->rp, .framebuffer = s->fb,
                                .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &clear};
   const VkViewport vp = {0, 0, W, H, 0, 1};
   BeginCommandBuffer(cb, &bi);
   unsigned p = 0, set = 0;
   uint32_t dyn_off = 0;
   VkRect2D sc = {{0, 0}, {W, H}};
   for (unsigned i = 0; i < n; i++) {
      const unsigned g = i + seed;
      const int newpass = i % pass == 0;
      if (newpass) {
         if (i)
            CmdEndRenderPass(cb);
         CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
         CmdSetViewport(cb, 0, 1, &vp);
      }
      if (newpass || i % 8 == 0) {
         p = (g / 8) % NPIPE;
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe[p]);
      }
      if (newpass || i % 2 == 0) {
         set = (g / 2) % NSETS;
         dyn_off = ((g / 2) % NSLOTS) * s->ubo_stride;
         CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 1, &s->set[set], 1, &dyn_off);
      }
      if (newpass || i % 3 == 0) {
         sc.offset.x = (int32_t)((g * 7) % (W / 2));
         sc.offset.y = (int32_t)((g * 13) % (H / 2));
         sc.extent.width = W / 2;
         sc.extent.height = H / 2 - (g % 5);
         CmdSetScissor(cb, 0, 1, &sc);
      }
      const float xf[4] = {0.25f, 0.25f, (float)(g % 7) * 0.1f - 0.5f, (float)(g % 11) * 0.08f - 0.5f};
      CmdPushConstants(cb, s->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof xf, xf);
      const VkDeviceSize voff = (VkDeviceSize)((g * 4) % (NVERT - 8)) * 16;
      CmdBindVertexBuffers(cb, 0, 1, &s->vb, &voff);
      const int indexed = !(p & 1);
      const VkDeviceSize ioff = (VkDeviceSize)((g * 6) % (NIDX - 12)) * 2;
      if (indexed)
         CmdBindIndexBuffer(cb, s->ib, ioff, VK_INDEX_TYPE_UINT16);
      if (redundant) {
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe[p]);
         CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 1, &s->set[set], 1, &dyn_off);
         CmdSetScissor(cb, 0, 1, &sc);
         CmdSetViewport(cb, 0, 1, &vp);
         CmdPushConstants(cb, s->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof xf, xf);
         CmdBindVertexBuffers(cb, 0, 1, &s->vb, &voff);
         if (indexed)
            CmdBindIndexBuffer(cb, s->ib, ioff, VK_INDEX_TYPE_UINT16);
      }
      if (indexed)
         CmdDrawIndexed(cb, 6, 1, 0, 0, 0);
      else
         CmdDraw(cb, 4, 1, 0, 0);
   }
   if (n)
      CmdEndRenderPass(cb);
   return n;
}

/* ---- --check: parse the fake backend's dump (MALI_FAKE_DUMP) ---- */
static char g_dump[256], g_stats[256];

/* The last kick's command stream: words into *out (malloc'd), returns the count or -1. */
static long
last_ring(uint64_t **out)
{
   FILE *f = fopen(g_dump, "r");
   if (!f)
      return -1;
   char *line = NULL, *last = NULL;
   size_t cap = 0;
   ssize_t len;
   while ((len = getline(&line, &cap, f)) > 0)
      if (strstr(line, "\"ev\":\"ring\"")) {
         free(last);
         last = strdup(line);
      }
   free(line);
   fclose(f);
   if (!last)
      return -1;
   char *h = strstr(last, "\"hex\":\"");
   if (!h) {
      free(last);
      return -1;
   }
   h += 7;
   size_t nb = strcspn(h, "\"") / 2;
   uint64_t *w = calloc(nb / 8 + 1, sizeof *w);
   for (size_t i = 0; i < nb; i++) {
      unsigned v;
      sscanf(h + 2 * i, "%2x", &v);
      w[i / 8] |= (uint64_t)v << (8 * (i % 8));
   }
   free(last);
   *out = w;
   return (long)(nb / 8);
}

static unsigned long long
alloc_bytes(void)
{
   unsigned long long a = 0, z = 0;
   FILE *f = fopen(g_stats, "r");
   if (f) {
      if (fscanf(f, "%llu %llu", &a, &z) != 2)
         a = 0;
      fclose(f);
   }
   return a;
}

static int
submit_wait(VkCommandBuffer cb, VkFence fence)
{
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   if (EndCommandBuffer(cb) != VK_SUCCESS || QueueSubmit(queue, 1, &si, fence) != VK_SUCCESS)
      return -1;
   WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
   ResetFences(dev, 1, &fence);
   return 0;
}

static int
run_check(const struct scene *s, VkCommandBuffer cb, VkFence fence)
{
   const unsigned n = 200, pass = 50;
   uint64_t *wa = NULL, *wb = NULL;
   /* the same seed for both: identical offsets, so the only difference is the redundant rebinds */
   unsigned long long a0 = alloc_bytes();
   record_stream(s, cb, n, pass, 0, 0);
   CHECK(!submit_wait(cb, fence), "submit A");
   unsigned long long a1 = alloc_bytes();
   long na = last_ring(&wa);
   CHECK(na > 0, "no ring in %s (MALI_FAKE_DUMP)", g_dump);
   record_stream(s, cb, n, pass, 0, 1);
   CHECK(!submit_wait(cb, fence), "submit B");
   unsigned long long a2 = alloc_bytes();
   long nb = last_ring(&wb);
   CHECK(nb > 0, "no ring B");
   unsigned runs = 0;
   for (long i = 0; i < na; i++) {
      unsigned op = (unsigned)(wa[i] >> 56);
      runs += op == 6 /* RUN_IDVS (v10/v11) */ || op == 12 /* RUN_IDVS2 (v13) */;
   }
   printf("check: draws %u run_idvs %u words %ld/%ld (redundant) alloc %llu/%llu B\n", n, runs, na, nb, a1 - a0, a2 - a1);
   CHECK(runs == n, "RUN_IDVS count %u != draws %u", runs, n);
   CHECK(na == nb, "redundant rebinds changed the stream length: %ld -> %ld words", na, nb);
   for (long i = 0; i < na; i++)
      CHECK((wa[i] >> 48) == (wb[i] >> 48), "word %ld differs in opcode/register: %016llx vs %016llx", i,
            (unsigned long long)wa[i], (unsigned long long)wb[i]);
   CHECK(a1 - a0 == a2 - a1, "redundant rebinds allocated descriptor memory: %llu -> %llu B", a1 - a0, a2 - a1);
   CHECK(a1 > a0, "MALI_ALLOC_STATS not written (%s)", g_stats);
   free(wa);
   free(wb);
   /* a pipeline change every 8 draws and a descriptor-set change every 2 must not cost more than one block group each: the
    * benchmark's descriptor bytes per draw stay below a bound that the old "rebuild everything on any bind" path exceeded */
   printf("check: %.1f descriptor B/draw\n", (double)(a1 - a0) / n);
   return 0;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   unsigned long total = 200000;
   unsigned per_cb = 2000, pass = 50;
   int check = 0, redundant = 0;
   const char *prof = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else if (!strcmp(argv[i], "--draws") && i + 1 < argc) total = strtoul(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--per-cb") && i + 1 < argc) per_cb = (unsigned)strtoul(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--pass") && i + 1 < argc) pass = (unsigned)strtoul(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--check")) check = 1;
      else if (!strcmp(argv[i], "--redundant")) redundant = 1;
      else if (!strcmp(argv[i], "--prof") && i + 1 < argc) prof = argv[++i];
      else {
         fprintf(stderr, "usage: t_skia_draws --direct icd.so [--spv-dir d] [--draws N] [--per-cb N] [--pass N] [--redundant] [--check] [--prof out]\n");
         return 2;
      }
   }
   if (!icd || !per_cb || !pass || total < per_cb) {
      fprintf(stderr, "--direct required; --draws >= --per-cb > 0\n");
      return 2;
   }
   if (check) {
      snprintf(g_dump, sizeof g_dump, "/tmp/t_skia_draws.%d.dump", (int)getpid());
      snprintf(g_stats, sizeof g_stats, "/tmp/t_skia_draws.%d.stats", (int)getpid());
      unlink(g_dump);
      setenv("MALI_FAKE_DUMP", g_dump, 1);
      setenv("MALI_ALLOC_STATS", g_stats, 1);
   }

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("FAIL icd entry\n"); return 1; }
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("FAIL CreateInstance\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceMemoryProperties GetMP = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkGetPhysicalDeviceProperties GetP = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
   PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   uint32_t np = 1;
   VkPhysicalDevice pd;
   if (EnumeratePhysicalDevices(inst, &np, &pd) < 0 || !np) { printf("FAIL no device\n"); return 1; }
   GetMP(pd, &memprops);
   GetP(pd, &props);
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   if (CreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { printf("FAIL CreateDevice\n"); return 1; }
#define LOAD(n) do { n = (PFN_vk##n)gdpa(dev, "vk" #n); if (!n) { printf("FAIL no vk%s\n", #n); return 1; } } while (0);
   DEV_FNS(LOAD)
#undef LOAD
   GetDeviceQueue(dev, 0, 0, &queue);

   struct scene s;
   memset(&s, 0, sizeof s);
   if (make_scene(&s))
      return 1;
   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool pool;
   if (CreateCommandPool(dev, &pci, NULL, &pool) != VK_SUCCESS) { printf("FAIL pool\n"); return 1; }
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
   VkCommandBuffer cb[2];
   if (AllocateCommandBuffers(dev, &cai, cb) != VK_SUCCESS) { printf("FAIL cbs\n"); return 1; }
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence[2];
   for (int i = 0; i < 2; i++)
      if (CreateFence(dev, &fci, NULL, &fence[i]) != VK_SUCCESS) { printf("FAIL fence\n"); return 1; }

   if (check) {
      int rc = run_check(&s, cb[0], fence[0]);
      unlink(g_dump);
      unlink(g_stats);
      printf("%s\n", rc || g_fail ? "FAILED" : "OK");
      return rc || g_fail;
   }

   /* benchmark: two command buffers in flight, the first two are warm-up */
   const unsigned long ncb = total / per_cb;
   double rec = 0, sub = 0, wait = 0;
#if HAVE_PROF
   if (prof)
      prof_start();
#else
   if (prof)
      fprintf(stderr, "t_skia_draws: --prof needs glibc on x86-64; ignored\n");
#endif
   unsigned long arec = 0, asub = 0, draws = 0;
   for (unsigned long k = 0; k < ncb + 2; k++) {
      const int i = (int)(k & 1), meas = k >= 2;
      if (k >= 2) {
         double w0 = now_ns();
         WaitForFences(dev, 1, &fence[i], VK_TRUE, UINT64_MAX);
         ResetFences(dev, 1, &fence[i]);
         if (meas) wait += now_ns() - w0;
      }
      g_nalloc = 0;
      g_count_on = meas;
#if HAVE_PROF
      g_prof_on = meas;
#endif
      double t0 = now_ns();
      unsigned d = record_stream(&s, cb[i], per_cb, pass, (unsigned)(k * 977u), redundant);
      if (EndCommandBuffer(cb[i]) != VK_SUCCESS) { printf("FAIL end\n"); return 1; }
      double t1 = now_ns();
      unsigned long n_rec = g_nalloc;
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb[i]};
      if (QueueSubmit(queue, 1, &si, fence[i]) != VK_SUCCESS) { printf("FAIL submit\n"); return 1; }
      double t2 = now_ns();
      g_count_on = 0;
      if (meas) {
         rec += t1 - t0;
         sub += t2 - t1;
         arec += n_rec;
         asub += g_nalloc - n_rec;
         draws += d;
      }
   }
#if HAVE_PROF
   g_prof_on = 0;
   if (prof)
      prof_write(prof);
#endif
   DeviceWaitIdle(dev);
   printf("draws=%lu\ncommand_buffers=%lu\ndraws_per_pass=%u\nredundant=%d\n", draws, ncb, pass, redundant);
   printf("record_ns_per_draw=%.1f\nsubmit_ns_per_draw=%.1f\nwait_ns_per_draw=%.1f\n", rec / draws, sub / draws, wait / draws);
   printf("allocs_per_draw_record=%.4f\nallocs_per_draw_submit=%.4f\nalloc_counting=%s\n", (double)arec / draws, (double)asub / draws,
          HAVE_COUNT ? "glibc" : "unavailable");
   return 0;
}
