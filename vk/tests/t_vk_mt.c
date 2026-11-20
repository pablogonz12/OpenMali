/* SPDX-License-Identifier: MIT */
/* Multithreaded stress test (DXVK-like driving pattern) on the ICD, meant for the fake backend and for ThreadSanitizer / ASan runs.
 * Loader-free: dlopens the ICD. Opt-in: check.sh runs it only with MALI_MT_STRESS=1 (sanitizer builds are the point).
 *   t_vk_mt --direct <icd.so> [--spv-dir dir] [--iters N] [--pipe-threads N] [--rec-threads M]
 * Threads (one queue; the single submit thread and the present thread take a mutex around vkQueueSubmit, as the spec requires):
 *   pipe   N  shader module + layout + compute pipeline against one shared pipeline cache, vkGetPipelineCacheData, vkMergePipelineCaches
 *   rec    M  each with its own command pool: record fill + dispatch, hand over to the submit thread, reset the pool when it is done
 *   submit 1  submits the recorded buffers with a fence and waits
 *   present 1 frame submit signalling a semaphore, then a present-style submit waiting on it (what wsi does; no surface on the host)
 *   res    2  buffer + memory create/bind/vkMapMemory/write/unmap/free, image create/destroy (mapped concurrently from several threads)
 *   query  1  query pool create / vkGetQueryPoolResults / destroy
 *   desc   1  descriptor set layout, pool alloc / update / free sets, reset pool
 * Fails on any Vulkan error. The races themselves are reported by the sanitizer runtime. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FNS \
   F(DestroyDevice) F(GetDeviceQueue) F(AllocateMemory) F(FreeMemory) F(MapMemory) F(UnmapMemory) \
   F(CreateFence) F(DestroyFence) F(WaitForFences) F(ResetFences) F(QueueSubmit) \
   F(CreateSemaphore) F(DestroySemaphore) \
   F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements) F(BindBufferMemory) \
   F(CreateImage) F(DestroyImage) F(GetImageMemoryRequirements) \
   F(CreateCommandPool) F(DestroyCommandPool) F(ResetCommandPool) F(AllocateCommandBuffers) F(FreeCommandBuffers) \
   F(BeginCommandBuffer) F(EndCommandBuffer) F(CmdFillBuffer) F(CmdBindPipeline) F(CmdDispatch) F(CmdPushConstants) \
   F(CreateShaderModule) F(DestroyShaderModule) F(CreatePipelineLayout) F(DestroyPipelineLayout) \
   F(CreateComputePipelines) F(DestroyPipeline) \
   F(CreatePipelineCache) F(DestroyPipelineCache) F(GetPipelineCacheData) F(MergePipelineCaches) \
   F(CreateQueryPool) F(DestroyQueryPool) F(GetQueryPoolResults) \
   F(CreateDescriptorSetLayout) F(DestroyDescriptorSetLayout) F(CreateDescriptorPool) F(DestroyDescriptorPool) \
   F(AllocateDescriptorSets) F(FreeDescriptorSets) F(ResetDescriptorPool) F(UpdateDescriptorSets)

static struct {
   PFN_vkGetDeviceProcAddr gdpa;
#define F(n) PFN_vk##n n;
   FNS
#undef F
} f;

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static VkPipelineCache g_cache;
static uint32_t *g_code;
static size_t g_code_size;
static int g_iters = 200, g_nipe = 3, g_nrec = 3;
static atomic_int g_fail, g_stop;
static atomic_int g_nrec_done;

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("{\"fail\":\"%s\",\"line\":%d,\"vk\":%d}\n", #x, __LINE__, (int)r_); atomic_store(&g_fail, 1); atomic_store(&g_stop, 1); return NULL; } } while (0)

static int
memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

static VkResult
submit_locked(const VkSubmitInfo *si, VkFence fe)
{
   pthread_mutex_lock(&qmu);
   VkResult r = f.QueueSubmit(queue, 1, si, fe);
   pthread_mutex_unlock(&qmu);
   return r;
}

static void *
pipe_thread(void *arg)
{
   (void)arg;
   VkPipelineCache mine;
   VkPipelineCacheCreateInfo pci0 = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
   CK(f.CreatePipelineCache(dev, &pci0, NULL, &mine));
   for (int i = 0; i < g_iters && !atomic_load(&g_stop); i++) {
      VkShaderModule sm;
      VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = g_code_size, .pCode = g_code};
      CK(f.CreateShaderModule(dev, &smi, NULL, &sm));
      VkPipelineLayout pl;
      VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
      VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
      CK(f.CreatePipelineLayout(dev, &pli, NULL, &pl));
      VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
      VkPipeline p;
      CK(f.CreateComputePipelines(dev, (i & 1) ? g_cache : mine, 1, &ci, NULL, &p));
      size_t sz = 0;
      CK(f.GetPipelineCacheData(dev, g_cache, &sz, NULL));
      if (sz) {
         void *buf = malloc(sz);
         VkResult r = f.GetPipelineCacheData(dev, g_cache, &sz, buf);
         free(buf);
         if (r != VK_SUCCESS && r != VK_INCOMPLETE) { printf("{\"fail\":\"GetPipelineCacheData\",\"vk\":%d}\n", (int)r); atomic_store(&g_fail, 1); return NULL; }
      }
      if (i % 4 == 0)
         CK(f.MergePipelineCaches(dev, g_cache, 1, &mine));
      f.DestroyPipeline(dev, p, NULL);
      f.DestroyPipelineLayout(dev, pl, NULL);
      f.DestroyShaderModule(dev, sm, NULL);
   }
   f.DestroyPipelineCache(dev, mine, NULL);
   return NULL;
}

/* rec -> submit hand-over: state 0 free, 1 recorded, 2 submitted+done */
struct slot { VkCommandPool pool; VkCommandBuffer cb; VkBuffer buf; int state; };
/* The hand-over uses a pthread mutex, not C11 atomics: the test binary is not built with -fsanitize=thread, so TSan only sees the
 * synchronisation it intercepts in libc (a mutex), and an atomic hand-off would be reported as a race on the driver's own data. */
static pthread_mutex_t hmu = PTHREAD_MUTEX_INITIALIZER;
static int get_state(struct slot *s) { pthread_mutex_lock(&hmu); int v = s->state; pthread_mutex_unlock(&hmu); return v; }
static void set_state(struct slot *s, int v) { pthread_mutex_lock(&hmu); s->state = v; pthread_mutex_unlock(&hmu); }
static struct slot *g_slots;

static void *
rec_thread(void *arg)
{
   struct slot *s = arg;
   VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CK(f.CreateCommandPool(dev, &pi, NULL, &s->pool));
   VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   CK(f.AllocateCommandBuffers(dev, &ai, &s->cb));
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   CK(f.CreateBuffer(dev, &bci, NULL, &s->buf));
   VkMemoryRequirements mr;
   f.GetBufferMemoryRequirements(dev, s->buf, &mr);
   VkDeviceMemory mem;
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = (uint32_t)memtype(mr.memoryTypeBits, 0)};
   CK(f.AllocateMemory(dev, &mai, NULL, &mem));
   CK(f.BindBufferMemory(dev, s->buf, mem, 0));
   for (int i = 0; i < g_iters && !atomic_load(&g_stop); i++) {
      while (get_state(s) == 1 && !atomic_load(&g_stop))
         usleep(50);
      if (get_state(s) == 2) {
         CK(f.ResetCommandPool(dev, s->pool, 0));
         set_state(s, 0);
      }
      if (i && (i & 3) == 0) {   /* churn the command buffers: free + allocate grows and shrinks the driver's per-command-buffer tables */
         f.FreeCommandBuffers(dev, s->pool, 1, &s->cb);
         CK(f.AllocateCommandBuffers(dev, &ai, &s->cb));
      }
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CK(f.BeginCommandBuffer(s->cb, &bi));
      f.CmdFillBuffer(s->cb, s->buf, 0, 4096, (uint32_t)i);
      CK(f.EndCommandBuffer(s->cb));
      set_state(s, 1);
   }
   while (get_state(s) == 1 && !atomic_load(&g_stop))
      usleep(50);
   atomic_fetch_add(&g_nrec_done, 1);
   f.DestroyBuffer(dev, s->buf, NULL);
   f.FreeMemory(dev, mem, NULL);
   f.DestroyCommandPool(dev, s->pool, NULL);
   return NULL;
}

static void *
submit_thread(void *arg)
{
   (void)arg;
   VkFence fe;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CK(f.CreateFence(dev, &fi, NULL, &fe));
   while (!atomic_load(&g_stop) && atomic_load(&g_nrec_done) < g_nrec) {
      int any = 0;
      for (int k = 0; k < g_nrec; k++) {
         struct slot *s = &g_slots[k];
         if (get_state(s) != 1)
            continue;
         any = 1;
         VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s->cb};
         CK(submit_locked(&si, fe));
         CK(f.WaitForFences(dev, 1, &fe, VK_TRUE, 10ull * 1000000000));
         CK(f.ResetFences(dev, 1, &fe));
         set_state(s, 2);
      }
      if (!any)
         usleep(50);
   }
   f.DestroyFence(dev, fe, NULL);
   return NULL;
}

static void *
present_thread(void *arg)
{
   (void)arg;
   VkSemaphore sem;
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   CK(f.CreateSemaphore(dev, &sci, NULL, &sem));
   VkFence fe;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CK(f.CreateFence(dev, &fi, NULL, &fe));
   for (int i = 0; i < g_iters && !atomic_load(&g_stop); i++) {
      VkSubmitInfo frame = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1, .pSignalSemaphores = &sem};
      CK(submit_locked(&frame, VK_NULL_HANDLE));
      VkPipelineStageFlags w = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkSubmitInfo pres = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem, .pWaitDstStageMask = &w};
      CK(submit_locked(&pres, fe));
      CK(f.WaitForFences(dev, 1, &fe, VK_TRUE, 10ull * 1000000000));
      CK(f.ResetFences(dev, 1, &fe));
   }
   f.DestroyFence(dev, fe, NULL);
   f.DestroySemaphore(dev, sem, NULL);
   return NULL;
}

static void *
res_thread(void *arg)
{
   (void)arg;
   for (int i = 0; i < g_iters && !atomic_load(&g_stop); i++) {
      VkBuffer b;
      VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096 << (i & 3),
                                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
      CK(f.CreateBuffer(dev, &bci, NULL, &b));
      VkMemoryRequirements mr;
      f.GetBufferMemoryRequirements(dev, b, &mr);
      int t = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      VkDeviceMemory m;
      VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)(t < 0 ? 0 : t)};
      CK(f.AllocateMemory(dev, &mai, NULL, &m));
      CK(f.BindBufferMemory(dev, b, m, 0));
      void *p;
      CK(f.MapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &p));
      memset(p, i, 64);
      f.UnmapMemory(dev, m);
      f.DestroyBuffer(dev, b, NULL);
      f.FreeMemory(dev, m, NULL);

      VkImage im;
      VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                               .extent = {16u << (i & 1), 16, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                               .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
      CK(f.CreateImage(dev, &ici, NULL, &im));
      f.GetImageMemoryRequirements(dev, im, &mr);
      f.DestroyImage(dev, im, NULL);
   }
   return NULL;
}

static void *
query_thread(void *arg)
{
   (void)arg;
   for (int i = 0; i < g_iters && !atomic_load(&g_stop); i++) {
      VkQueryPool qp;
      VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 8 + (i & 7)};
      CK(f.CreateQueryPool(dev, &qi, NULL, &qp));
      uint64_t res[16];
      VkResult r = f.GetQueryPoolResults(dev, qp, 0, 1, sizeof res, res, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_PARTIAL_BIT);
      if (r != VK_SUCCESS && r != VK_NOT_READY) { printf("{\"fail\":\"GetQueryPoolResults\",\"vk\":%d}\n", (int)r); atomic_store(&g_fail, 1); return NULL; }
      f.DestroyQueryPool(dev, qp, NULL);
   }
   return NULL;
}

static void *
desc_thread(void *arg)
{
   (void)arg;
   VkDescriptorSetLayoutBinding bnd = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &bnd};
   VkDescriptorSetLayout dl;
   CK(f.CreateDescriptorSetLayout(dev, &li, NULL, &dl));
   VkBuffer b;
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
   CK(f.CreateBuffer(dev, &bci, NULL, &b));
   for (int i = 0; i < g_iters && !atomic_load(&g_stop); i++) {
      VkDescriptorPool dp;
      VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8};
      VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                       .maxSets = 8, .poolSizeCount = 1, .pPoolSizes = &ps};
      CK(f.CreateDescriptorPool(dev, &pi, NULL, &dp));
      VkDescriptorSet sets[4];
      VkDescriptorSetLayout ls[4] = {dl, dl, dl, dl};
      VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 4, .pSetLayouts = ls};
      CK(f.AllocateDescriptorSets(dev, &ai, sets));
      VkDescriptorBufferInfo bi = {b, 0, 4096};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0], .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
      f.UpdateDescriptorSets(dev, 1, &w, 0, NULL);
      CK(f.FreeDescriptorSets(dev, dp, 2, sets));
      if (i & 1)
         CK(f.ResetDescriptorPool(dev, dp, 0));
      f.DestroyDescriptorPool(dev, dp, NULL);
   }
   f.DestroyBuffer(dev, b, NULL);
   f.DestroyDescriptorSetLayout(dev, dl, NULL);
   return NULL;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL, *spv = "shader";
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) spv = argv[++i];
      else if (!strcmp(argv[i], "--iters") && i + 1 < argc) g_iters = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--pipe-threads") && i + 1 < argc) g_nipe = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--rec-threads") && i + 1 < argc) g_nrec = atoi(argv[++i]);
   }
   if (!icd || g_nrec < 1 || g_nipe < 1) { fprintf(stderr, "usage: t_vk_mt --direct icd.so [--spv-dir d] [--iters N]\n"); return 2; }
   char path[600];
   snprintf(path, sizeof path, "%s/comp.comp.spv", spv);
   FILE *fp = fopen(path, "rb");
   if (!fp) { printf("{\"fail\":\"open %s\"}\n", path); return 1; }
   static uint32_t code[8192];
   g_code_size = fread(code, 1, sizeof code, fp);
   fclose(fp);
   g_code = code;

   void *h = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
   if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   if (ci(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
   uint32_t n = 1;
   if (((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &phys) < 0 || n < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties"))(phys, &memprops);
   VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1};
   float pr = 1;
   q.pQueuePriorities = &pr;
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q};
   if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(phys, &dci, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
   f.gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define F(nm) f.nm = (PFN_vk##nm)f.gdpa(dev, "vk" #nm); if (!f.nm) { printf("{\"fail\":\"missing vk" #nm "\"}\n"); return 1; }
   FNS
#undef F
   f.GetDeviceQueue(dev, 0, 0, &queue);
   VkPipelineCacheCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
   if (f.CreatePipelineCache(dev, &pci, NULL, &g_cache) != VK_SUCCESS) return 1;

   g_slots = calloc(g_nrec, sizeof *g_slots);
   int nt = g_nipe + g_nrec + 6;
   pthread_t *t = calloc(nt, sizeof *t);
   int k = 0;
   for (int i = 0; i < g_nipe; i++) pthread_create(&t[k++], NULL, pipe_thread, NULL);
   for (int i = 0; i < g_nrec; i++) pthread_create(&t[k++], NULL, rec_thread, &g_slots[i]);
   pthread_create(&t[k++], NULL, submit_thread, NULL);
   pthread_create(&t[k++], NULL, present_thread, NULL);
   pthread_create(&t[k++], NULL, res_thread, NULL);
   pthread_create(&t[k++], NULL, res_thread, NULL);
   pthread_create(&t[k++], NULL, query_thread, NULL);
   pthread_create(&t[k++], NULL, desc_thread, NULL);
   for (int i = 0; i < k; i++) pthread_join(t[i], NULL);
   f.DestroyPipelineCache(dev, g_cache, NULL);
   f.DestroyDevice(dev, NULL);
   printf("{\"mt\":{\"iters\":%d,\"pipe_threads\":%d,\"rec_threads\":%d},\"pass\":%s}\n", g_iters, g_nipe, g_nrec, atomic_load(&g_fail) ? "false" : "true");
   return atomic_load(&g_fail) ? 1 : 0;
}
