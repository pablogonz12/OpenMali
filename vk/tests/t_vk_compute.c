/* SPDX-License-Identifier: MIT */
/* M11 test: compute through the ICD (SAXPY, shared-memory prefix sum, SSBO atomics, indirect dispatch) (shader modules, graphics pipeline,
 * render pass, framebuffer, command buffer, queue submit, fence, host readback). Loader-free: dlopens the
 * ICD like t_queue --direct. One JSON line per case; exit 0 = all pass.
 *
 *   t_vk_compute --direct <icd.so> [--loop N] [--only saxpy|saxpy_ind|scan|atomic|atomic256|atomic_ind]
 *             [--spv-dir dir]
 * Pixel checks are skipped on the fake backend (MALI_BACKEND=fake): it renders nothing.
 * --loop N repeats the triangle case N times; --loop-reset reuses one command buffer via
 * vkResetCommandPool instead of allocating a fresh one each time.
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 256
#define H 256

static int g_fail, g_fake, g_loop = 1;
static unsigned g_groups;   /* --groups N overrides the atomic/scan group count */
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

#define DEV_FNS(X)                                                                                 \
   X(DestroyDevice) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory)  \
   X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(QueueSubmit) X(DeviceWaitIdle)  \
   X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory)            \
   X(CreateShaderModule) X(DestroyShaderModule) X(CreatePipelineLayout) X(DestroyPipelineLayout)   \
   X(CreateComputePipelines) X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool)         \
   X(ResetCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)         \
   X(CmdBindPipeline) X(CmdPushConstants) X(CmdDispatch) X(CmdDispatchIndirect)                    \
   X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool)             \
   X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CmdBindDescriptorSets)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;

__attribute__((unused)) static double
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
   if (!f) { printf("{\"fail\":\"open %s\"}\n", path); return VK_NULL_HANDLE; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *buf = malloc(n);
   if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return VK_NULL_HANDLE; }
   fclose(f);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = buf};
   VkShaderModule m = VK_NULL_HANDLE;
   if (CreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS)
      m = VK_NULL_HANDLE;
   free(buf);
   return m;
}


struct buf { VkBuffer b; VkDeviceMemory m; void *p; size_t size; };

static int
mkbuf(struct buf *o, size_t size, VkBufferUsageFlags usage)
{
   o->size = size;
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   CHECK(CreateBuffer(dev, &bi, NULL, &o->b) == VK_SUCCESS, "CreateBuffer");
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, o->b, &mr);
   int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   CHECK(mt >= 0, "no host visible type");
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
   CHECK(AllocateMemory(dev, &ai, NULL, &o->m) == VK_SUCCESS, "AllocateMemory");
   CHECK(BindBufferMemory(dev, o->b, o->m, 0) == VK_SUCCESS, "BindBufferMemory");
   CHECK(MapMemory(dev, o->m, 0, VK_WHOLE_SIZE, 0, &o->p) == VK_SUCCESS, "MapMemory");
   return 1;
}

static void
rmbuf(struct buf *o)
{
   if (o->b) DestroyBuffer(dev, o->b, NULL);
   if (o->m) FreeMemory(dev, o->m, NULL);
}

enum { K_SAXPY, K_SCAN, K_ATOMIC };

/* One case: kind, shader, local size, dispatch (direct or indirect), repeated g_loop times. */
static int
run_case(const char *name, int kind, const char *spv, unsigned local, unsigned groups, int indirect)
{
   int rc = 0;
   if (g_groups && kind != K_SAXPY) groups = g_groups;
   const unsigned N = kind == K_SAXPY ? 100000 : groups * local;
   const unsigned nbind = kind == K_ATOMIC ? 1 : 2;
   struct buf b[2] = {{0}}, ind = {0};
   VkShaderModule sm = load_spv(spv);
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
   VkDescriptorPool dp = VK_NULL_HANDLE;
   VkDescriptorSet ds;
   VkPipelineLayout pl = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;
   VkCommandBuffer cb;
   VkFence f = VK_NULL_HANDLE;
   CHECK(sm, "load %s", spv);

   VkDescriptorSetLayoutBinding lb[2];
   for (unsigned i = 0; i < nbind; i++)
      lb[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
                                             .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nbind, .pBindings = lb};
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nbind};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "dpool");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   CHECK(AllocateDescriptorSets(dev, &dai, &ds) == VK_SUCCESS, "alloc set");
   VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
                                     .pushConstantRangeCount = kind == K_SAXPY, .pPushConstantRanges = &pcr};
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &pl) == VK_SUCCESS, "layout");
   VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
   VkResult pr = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipe);
   CHECK(pr == VK_SUCCESS, "CreateComputePipelines = %d", pr);

   size_t sz0 = kind == K_ATOMIC ? 4096 : (size_t)N * 4;
   CHECK(mkbuf(&b[0], sz0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT), "buf0");
   if (nbind > 1) CHECK(mkbuf(&b[1], (size_t)N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT), "buf1");
   CHECK(mkbuf(&ind, 256, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT), "indirect buf");
   VkDescriptorBufferInfo bi[2] = {{b[0].b, 0, VK_WHOLE_SIZE}, {b[1].b, 0, VK_WHOLE_SIZE}};
   VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = nbind,
                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = bi};
   UpdateDescriptorSets(dev, 1, &w, 0, NULL);

   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CHECK(CreateCommandPool(dev, &cpi, NULL, &pool) == VK_SUCCESS, "pool");
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   CHECK(AllocateCommandBuffers(dev, &cai, &cb) == VK_SUCCESS, "cb");
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");

   for (int it = 0; it < g_loop; it++) {
      float *x = NULL, *y = NULL;
      if (kind == K_SAXPY) {
         x = b[0].p; y = b[1].p;
         for (unsigned i = 0; i < N; i++) { x[i] = (float)(i % 97) * 0.5f; y[i] = (float)(i % 13); }
      } else if (kind == K_SCAN) {
         uint32_t *s = b[0].p;
         for (unsigned i = 0; i < N; i++) s[i] = (i * 2654435761u >> 20) & 15;
         memset(b[1].p, 0xee, (size_t)N * 4);
      } else {
         memset(b[0].p, 0, 4096);
      }
      uint32_t *iw = ind.p;
      iw[0] = groups; iw[1] = 1; iw[2] = 1;

      VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CHECK(BeginCommandBuffer(cb, &cbi) == VK_SUCCESS, "begin");
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
      CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
      if (kind == K_SAXPY) {
         struct { float a; uint32_t n; } pc = {2.0f, N};
         CmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &pc);
      }
      if (indirect) CmdDispatchIndirect(cb, ind.b, 0);
      else CmdDispatch(cb, groups, 1, 1);
      VkResult er = EndCommandBuffer(cb);
      CHECK(er == VK_SUCCESS, "end = %d", er);
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
      CHECK(ResetFences(dev, 1, &f) == VK_SUCCESS, "reset fence");
      CHECK(QueueSubmit(queue, 1, &si, f) == VK_SUCCESS, "submit");
      VkResult wr = WaitForFences(dev, 1, &f, VK_TRUE, 20000000000ull);
      CHECK(wr == VK_SUCCESS, "wait = %d", wr);

      if (!g_fake) {
         if (kind == K_SAXPY) {
            unsigned bad = 0, first = 0;
            for (unsigned i = 0; i < N; i++) {
               float e = 2.0f * ((float)(i % 97) * 0.5f) + (float)(i % 13);
               if (y[i] != e) { if (!bad) first = i; bad++; }
            }
            CHECK(bad == 0, "saxpy: %u of %u wrong, first at %u: got %f want %f", bad, N, first, y[first],
                  2.0f * ((float)(first % 97) * 0.5f) + (float)(first % 13));
         } else if (kind == K_SCAN) {
            uint32_t *s = b[0].p, *d = b[1].p;
            unsigned bad = 0, first = 0;
            for (unsigned g = 0; g < groups; g++) {
               uint32_t acc = 0;
               for (unsigned l = 0; l < local; l++) {
                  acc += s[g * local + l];
                  if (d[g * local + l] != acc) { if (!bad) first = g * 256 + l; bad++; }
               }
            }
            CHECK(bad == 0, "scan: %u of %u wrong, first at %u: got %u", bad, N, first, d[first]);
         } else {
            uint32_t *c = b[0].p;
            uint64_t tot = (uint64_t)groups * local;
            uint32_t want_sum = (uint32_t)(tot * (tot - 1) / 2);
            CHECK(c[0] == tot && c[1] == want_sum, "atomic: count %u (want %llu) sum %u (want %u)", c[0], (unsigned long long)tot, c[1], want_sum);
         }
      }
      if (it + 1 < g_loop)
         CHECK(ResetCommandPool(dev, pool, 0) == VK_SUCCESS, "reset pool");
   }
   printf("{\"test\":\"%s\",\"groups\":%u,\"n\":%d,\"ok\":true%s}\n", name, groups, g_loop, g_fake ? ",\"skipped\":\"results: fake backend\"" : "");
   DestroyFence(dev, f, NULL);
   DestroyCommandPool(dev, pool, NULL);
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, pl, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   DestroyShaderModule(dev, sm, NULL);
   rmbuf(&b[0]); rmbuf(&b[1]); rmbuf(&ind);
   rc = 1;
   return rc;
}

/* Compute with a dynamic storage-buffer descriptor: src is bound with a dynamic offset (256 bytes = 64 words) and dst[i] must be src[64 + i] + 1.
 * Before the fix record_dispatch dropped the dynamic descriptors and the shader read an uninitialised resource table (deqp dynamic_offset.compute). */
static int
run_dyn(void)
{
   const unsigned N = 64;
   struct buf src = {0}, dst = {0};
   VkShaderModule sm = load_spv("dynbuf.comp.spv");
   CHECK(sm, "load dynbuf.comp.spv");
   VkDescriptorSetLayoutBinding lb[2] = {
      {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
      {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
   VkDescriptorSetLayout dsl;
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb};
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps};
   VkDescriptorPool dp;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "dpool");
   VkDescriptorSet ds;
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   CHECK(AllocateDescriptorSets(dev, &dai, &ds) == VK_SUCCESS, "alloc set");
   VkPipelineLayout pl;
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &pl) == VK_SUCCESS, "layout");
   VkPipeline pipe;
   VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
   CHECK(CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipe) == VK_SUCCESS, "CreateComputePipelines");
   CHECK(mkbuf(&src, 2 * N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT), "src");
   CHECK(mkbuf(&dst, N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT), "dst");
   uint32_t *s = src.p, *d = dst.p;
   for (unsigned i = 0; i < 2 * N; i++) s[i] = 1000 * (i / N) + i;
   memset(d, 0xee, N * 4);
   VkDescriptorBufferInfo bi[2] = {{src.b, 0, 2 * N * 4 / 2}, {dst.b, 0, VK_WHOLE_SIZE}};
   VkWriteDescriptorSet w[2] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &bi[0]},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1]}};
   UpdateDescriptorSets(dev, 2, w, 0, NULL);
   VkCommandPool pool;
   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CHECK(CreateCommandPool(dev, &cpi, NULL, &pool) == VK_SUCCESS, "pool");
   VkCommandBuffer cb;
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   CHECK(AllocateCommandBuffers(dev, &cai, &cb) == VK_SUCCESS, "cb");
   VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(BeginCommandBuffer(cb, &cbi) == VK_SUCCESS, "begin");
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   uint32_t dynoff = N * 4;
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 1, &dynoff);
   CmdDispatch(cb, 1, 1, 1);
   CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   CHECK(QueueSubmit(queue, 1, &si, f) == VK_SUCCESS, "submit");
   CHECK(WaitForFences(dev, 1, &f, VK_TRUE, 20000000000ull) == VK_SUCCESS, "wait");
   if (!g_fake) {
      unsigned bad = 0, first = 0;
      for (unsigned i = 0; i < N; i++)
         if (d[i] != s[N + i] + 1) { if (!bad) first = i; bad++; }
      CHECK(bad == 0, "dyn: %u of %u wrong, first at %u: got %u want %u", bad, N, first, d[first], s[N + first] + 1);
   }
   printf("{\"test\":\"dyn\",\"ok\":true%s}\n", g_fake ? ",\"skipped\":\"results: fake backend\"" : "");
   DestroyFence(dev, f, NULL);
   DestroyCommandPool(dev, pool, NULL);
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, pl, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   DestroyShaderModule(dev, sm, NULL);
   rmbuf(&src); rmbuf(&dst);
   return 1;
}

/* 398: an application-provided allocator may only be called from the thread that issued the command, so a batch with
 * VkAllocationCallbacks must not run on workers. These record the calling thread. */
static pthread_t g_main_thread;
static int g_alloc_calls, g_alloc_outside;
static void *test_alloc(void *u, size_t size, size_t align, VkSystemAllocationScope sc)
{
   (void)u; (void)align; (void)sc;
   if (!pthread_equal(pthread_self(), g_main_thread))
      __atomic_add_fetch(&g_alloc_outside, 1, __ATOMIC_RELAXED);
   __atomic_add_fetch(&g_alloc_calls, 1, __ATOMIC_RELAXED);
   return malloc(size);
}
static void *test_realloc(void *u, void *p, size_t size, size_t align, VkSystemAllocationScope sc)
{
   (void)u; (void)align; (void)sc;
   if (!pthread_equal(pthread_self(), g_main_thread))
      __atomic_add_fetch(&g_alloc_outside, 1, __ATOMIC_RELAXED);
   __atomic_add_fetch(&g_alloc_calls, 1, __ATOMIC_RELAXED);
   return realloc(p, size);
}
static void test_free(void *u, void *p) { (void)u; free(p); }
static void test_internal(void *u, size_t size, VkInternalAllocationType t, VkSystemAllocationScope sc) { (void)u; (void)size; (void)t; (void)sc; }
static const VkAllocationCallbacks test_cb = { .pfnAllocation = test_alloc, .pfnReallocation = test_realloc,
                                               .pfnFree = test_free, .pfnInternalAllocation = test_internal,
                                               .pfnInternalFree = test_internal };

/* Hitch: a batch of N pipelines in ONE vkCreateComputePipelines call must be compiled in parallel (MALI_PIPE_THREADS, default 4), so the
 * call's latency (what the render thread sees) is well under the sum of the same compiles done one at a time. Run with
 * MALI_SHADER_CACHE= (disabled) so every create really compiles. Reports max single-create latency and the batch latency. */
static int
run_batch(void)
{
   static const struct { const char *spv; unsigned nbind; } K[4] = {{"int64div.comp.spv", 2}, {"etc2.comp.spv", 2}, {"int64div.comp.spv", 2}, {"etc2.comp.spv", 2}};
   enum { N = 8 };
   VkComputePipelineCreateInfo ci[N];
   VkPipelineLayout pl[N];
   VkDescriptorSetLayout dsl[N];
   VkPipeline pb[N], ps[N];
   for (int i = 0; i < N; i++) {
      VkShaderModule sm = load_spv(K[i % 4].spv);
      CHECK(sm, "load");
      VkDescriptorSetLayoutBinding lb[2];
      for (unsigned j = 0; j < K[i % 4].nbind; j++)
         lb[j] = (VkDescriptorSetLayoutBinding){.binding = j, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
      VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = K[i % 4].nbind, .pBindings = lb};
      CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl[i]) == VK_SUCCESS, "dsl");
      VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 48};
      VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl[i],
                                        .pushConstantRangeCount = i % 2 == 1, .pPushConstantRanges = &pcr};
      CHECK(CreatePipelineLayout(dev, &pli, NULL, &pl[i]) == VK_SUCCESS, "layout");
      ci[i] = (VkComputePipelineCreateInfo){.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl[i],
         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
   }
   double t0 = now_us();
   VkResult r = CreateComputePipelines(dev, VK_NULL_HANDLE, N, ci, NULL, pb);
   double batch = now_us() - t0;
   CHECK(r == VK_SUCCESS, "batch = %d", r);
   for (int i = 0; i < N; i++)
      CHECK(pb[i] != VK_NULL_HANDLE, "batch[%d] null", i);
   double seq = 0, mx = 0;
   for (int i = 0; i < N; i++) {
      t0 = now_us();
      CHECK(CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci[i], NULL, &ps[i]) == VK_SUCCESS, "single %d", i);
      double d = now_us() - t0;
      seq += d;
      if (d > mx) mx = d;
   }
   const char *e = getenv("MALI_PIPE_THREADS");
   int par = !(e && atoi(e) <= 1) && sysconf(_SC_NPROCESSORS_ONLN) >= 2;
   printf("{\"case\":\"batch\",\"n\":%d,\"batch_us\":%.0f,\"seq_us\":%.0f,\"max_single_us\":%.0f,\"parallel\":%d}\n", N, batch, seq, mx, par);
   if (getenv("MALI_SHADER_CACHE") && !*getenv("MALI_SHADER_CACHE") && par)
      CHECK(batch < seq * 0.75, "batch %.0f us not faster than sequential %.0f us", batch, seq);
   for (int i = 0; i < N; i++) { DestroyPipeline(dev, pb[i], NULL); DestroyPipeline(dev, ps[i], NULL); }

   /* 398: the same batch with an application allocator stays on this thread (sequential create). */
   g_main_thread = pthread_self();
   g_alloc_calls = g_alloc_outside = 0;
   VkPipeline pa[N];
   VkResult ra = CreateComputePipelines(dev, VK_NULL_HANDLE, N, ci, &test_cb, pa);
   CHECK(ra == VK_SUCCESS, "batch with app allocator = %d", ra);
   CHECK(g_alloc_calls > 0, "app allocator was never called (%d)", g_alloc_calls);
   CHECK(g_alloc_outside == 0, "%d of %d app-allocator calls ran on a worker thread", g_alloc_outside, g_alloc_calls);
   for (int i = 0; i < N; i++)
      if (pa[i] != VK_NULL_HANDLE)
         DestroyPipeline(dev, pa[i], &test_cb);
   return 1;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--loop") && i + 1 < argc) g_loop = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--groups") && i + 1 < argc) g_groups = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else { fprintf(stderr, "usage: t_vk_compute --direct icd.so [--loop N] [--only case] [--spv-dir d]\n"); return 2; }
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
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   const char *iext[] = {"VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities",
                         "VK_KHR_external_semaphore_capabilities", "VK_KHR_external_fence_capabilities"};
   ici.enabledExtensionCount = 4;
   ici.ppEnabledExtensionNames = iext;
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
   printf("{\"device\":\"%s\"}\n", pp.deviceName);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GDPA(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   DEV_FNS(LOAD)
   GetDeviceQueue(dev, 0, 0, &queue);

   int ok = 1;
   if (CASE_ON("dyn")) ok &= run_dyn();
   if (ok && g_only && !strcmp(g_only, "batch")) ok &= run_batch();
   if (ok && CASE_ON("saxpy")) ok &= run_case("saxpy", K_SAXPY, "saxpy.comp.spv", 64, (100000 + 63) / 64, 0);
   if (ok && CASE_ON("saxpy_ind")) ok &= run_case("saxpy_ind", K_SAXPY, "saxpy.comp.spv", 64, (100000 + 63) / 64, 1);
   if (ok && CASE_ON("atomic")) ok &= run_case("atomic", K_ATOMIC, "atomic.comp.spv", 64, 100, 0);
   if (ok && CASE_ON("atomic256")) ok &= run_case("atomic256", K_ATOMIC, "atomic256.comp.spv", 256, 37, 0);
   if (ok && CASE_ON("atomic_ind")) ok &= run_case("atomic_ind", K_ATOMIC, "atomic.comp.spv", 64, 100, 1);
   if (ok && CASE_ON("scan")) ok &= run_case("scan", K_SCAN, "scan.comp.spv", 256, 64, 0);
   /* GL 4.3 asks for 1024 invocations per workgroup: one 1024-wide group doing a shared-memory prefix sum with barriers
    * (4 KiB of shared memory), the same shape as the 256-wide `scan` that passes today. */
   if (ok && CASE_ON("scan1024")) ok &= run_case("scan1024", K_SCAN, "scan1024.comp.spv", 1024, 4, 0);

   DeviceWaitIdle(dev);
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   dlclose(lib);
   printf("{\"pass\":%s}\n", (ok && !g_fail) ? "true" : "false");
   return (ok && !g_fail) ? 0 : 1;
}
