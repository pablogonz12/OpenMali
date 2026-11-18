/* SPDX-License-Identifier: MIT */
/* Fault-injection sweep (offline, fake backend): every create/allocate entrypoint is run with the n-th host allocation failing
 * (counting VkAllocationCallbacks) and, separately, with the n-th BO allocation failing (MALI_FAKE_FAIL_BO=n, the stand-in for a
 * failing GPU_MEM_ALLOC ioctl), for n = 1.. until the call no longer reaches the n-th allocation. For every n:
 *   - a failing call returns VK_ERROR_OUT_OF_HOST_MEMORY (host fault) / OUT_OF_DEVICE_MEMORY or OUT_OF_HOST_MEMORY (BO fault)
 *     (vkCreateDevice: or INITIALIZATION_FAILED) and writes VK_NULL_HANDLE to its output handle (checked where the spec says so);
 *   - after the scenario (success or failure) every callback allocation is freed again and no fd / mapping is left behind.
 * Driver-internal libc malloc() is not routed through the callbacks; the BO sweep (fds + /proc/self/maps) covers the GPU memory.
 *   t_fault --direct <icd.so> --spv <comp.spv>      (needs MALI_BACKEND=fake MALI_FAKE_GPU=v11|v13). Exit 0 = pass.
 * Mutation check: --self-test makes the allocator leak one block on the n=2 failure and must report a leak (exit 0 when it does). */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dirent.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
static const char *g_spv;
static int g_selftest;

#define FAIL(...) do { g_fail = 1; printf("{\"fail\":\"%s:%d\",\"detail\":\"", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\"}\n"); } while (0)

/* ---- counting / failing allocator ---- */
struct blk { void *orig; size_t size; };
static long g_live, g_count, g_fail_at;
static int g_leak_one;   /* self-test: skip one free */

static void *
a_alloc(void *u, size_t size, size_t align, VkSystemAllocationScope s)
{
   (void)u; (void)s;
   if (g_fail_at && ++g_count == g_fail_at)
      return NULL;
   if (!g_fail_at)
      g_count++;
   if (align < sizeof(struct blk)) align = sizeof(struct blk);
   char *o = malloc(size + align + sizeof(struct blk));
   if (!o)
      return NULL;
   uintptr_t p = ((uintptr_t)o + sizeof(struct blk) + align - 1) & ~(uintptr_t)(align - 1);
   struct blk *b = (struct blk *)p - 1;
   b->orig = o;
   b->size = size;
   g_live++;
   return (void *)p;
}

static void
a_free(void *u, void *p)
{
   (void)u;
   if (!p)
      return;
   if (g_leak_one) {   /* self-test only */
      g_leak_one = 0;
      return;
   }
   free(((struct blk *)p - 1)->orig);
   g_live--;
}

static void *
a_realloc(void *u, void *p, size_t size, size_t align, VkSystemAllocationScope s)
{
   if (!p)
      return a_alloc(u, size, align, s);
   if (!size) {
      a_free(u, p);
      return NULL;
   }
   void *n = a_alloc(u, size, align, s);
   if (!n)
      return NULL;   /* the old block stays valid */
   size_t old = ((struct blk *)p - 1)->size;
   memcpy(n, p, old < size ? old : size);
   a_free(u, p);
   return n;
}

static const VkAllocationCallbacks g_cb = {.pfnAllocation = a_alloc, .pfnReallocation = a_realloc, .pfnFree = a_free};
#define CB (&g_cb)


/* ---- libc malloc interposer (the executable's malloc wins over libc's for the dlopen'ed driver): while armed, the n-th malloc-family
 * call fails with NULL, and every block handed out while armed is tracked until freed (the leak count). ---- */
extern void *__libc_malloc(size_t), *__libc_calloc(size_t, size_t), *__libc_realloc(void *, size_t), *__libc_memalign(size_t, size_t);
extern void __libc_free(void *);
#define TAB (1u << 16)
static void *g_tab[TAB];
static void *g_bt[TAB][8];   /* allocation site (return addresses) of each tracked block, for the leak report */
static int g_in_bt;
static unsigned long g_seq, g_seq_tab[TAB], g_report_from;   /* allocation order: a leak report lists only blocks newer than the baseline */
static long g_libc_live, g_libc_count, g_libc_fail_at;
static int g_armed;

static void
track(void *p)
{
   if (!p || !g_armed)
      return;
   unsigned h = (unsigned)(((uintptr_t)p >> 4) * 2654435761u) & (TAB - 1);
   while (g_tab[h] && g_tab[h] != (void *)1)
      h = (h + 1) & (TAB - 1);
   g_tab[h] = p;
   g_seq_tab[h] = ++g_seq;
   g_libc_live++;
   if (!g_in_bt) {
      g_in_bt = 1;
      memset(g_bt[h], 0, sizeof g_bt[h]);
      backtrace(g_bt[h], 8);
      g_in_bt = 0;
   }
}

static void
untrack(void *p)
{
   if (!p || !g_libc_live)
      return;
   unsigned h = (unsigned)(((uintptr_t)p >> 4) * 2654435761u) & (TAB - 1);
   for (unsigned i = 0; i < TAB && g_tab[h]; i++, h = (h + 1) & (TAB - 1))
      if (g_tab[h] == p) {
         g_tab[h] = (void *)1;   /* tombstone */
         g_libc_live--;
         return;
      }
}

/* print the allocation sites of the tracked blocks as "<object>+0xoff" (resolve with addr2line -f -e <object>) */
static void
report_live(void)
{
   int shown = 0;
   for (unsigned h = 0; h < TAB && shown < 12; h++) {
      if (!g_tab[h] || g_tab[h] == (void *)1 || g_seq_tab[h] <= g_report_from)
         continue;
      shown++;
      printf("{\"leak_site\":[");
      for (int i = 0; i < 8 && g_bt[h][i]; i++) {
         Dl_info di;
         if (dladdr(g_bt[h][i], &di) && di.dli_fname)
            printf("%s\"%s+0x%lx\"", i ? "," : "", strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
                   (unsigned long)((char *)g_bt[h][i] - (char *)di.dli_fbase));
      }
      printf("]}\n");
   }
}

static int
libc_fault(void)
{
   if (!g_armed)
      return 0;
   g_libc_count++;
   return g_libc_fail_at && g_libc_count == g_libc_fail_at;
}

void *malloc(size_t n) { if (libc_fault()) return NULL; void *p = __libc_malloc(n); track(p); return p; }
void *calloc(size_t a, size_t b) { if (libc_fault()) return NULL; void *p = __libc_calloc(a, b); track(p); return p; }
void *realloc(void *o, size_t n) { if (n && libc_fault()) return NULL; untrack(o); void *p = __libc_realloc(o, n); if (!p && n) track(o); else track(p); return p; }
void *memalign(size_t al, size_t n) { if (libc_fault()) return NULL; void *p = __libc_memalign(al, n); track(p); return p; }
void *aligned_alloc(size_t al, size_t n) { return memalign(al, n); }
int posix_memalign(void **o, size_t al, size_t n) { void *p = memalign(al, n); if (!p) return 12; *o = p; return 0; }
void free(void *p) { untrack(p); __libc_free(p); }

/* ---- driver entrypoints ---- */
static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static PFN_vkGetDeviceProcAddr gdpa;
#define FNS(X) \
   X(CreateBuffer) X(DestroyBuffer) X(CreateImage) X(DestroyImage) X(CreateImageView) X(DestroyImageView) X(CreateSampler) \
   X(DestroySampler) X(AllocateMemory) X(FreeMemory) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
   X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
   X(CreateShaderModule) X(DestroyShaderModule) X(CreateComputePipelines) X(DestroyPipeline) X(CreateCommandPool) \
   X(DestroyCommandPool) X(AllocateCommandBuffers) X(CreateFence) X(DestroyFence) X(CreateSemaphore) X(DestroySemaphore) \
   X(CreateEvent) X(DestroyEvent) X(CreateQueryPool) X(DestroyQueryPool) X(CreateRenderPass) X(DestroyRenderPass) \
   X(CreateFramebuffer) X(DestroyFramebuffer) X(DestroyDevice) X(GetDeviceQueue) X(BeginCommandBuffer) X(EndCommandBuffer) \
   X(CmdFillBuffer) X(QueueSubmit) X(WaitForFences) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateBufferView) \
   X(DestroyBufferView) X(CreateGraphicsPipelines) X(QueueWaitIdle)
#define X(n) static PFN_vk##n p_##n __attribute__((unused));
FNS(X)
#undef X

static long
count_dir(const char *p)
{
   DIR *d = opendir(p);
   long n = 0;
   if (!d)
      return -1;
   while (readdir(d))
      n++;
   closedir(d);
   return n;
}

static long
count_maps(void)
{
   FILE *fp = fopen("/proc/self/maps", "r");
   char line[512];
   long n = 0;
   while (fp && fgets(line, sizeof line, fp))
      n++;
   if (fp)
      fclose(fp);
   return n;
}

static void
load_dev_fns(void)
{
#define X(n) p_##n = (PFN_vk##n)gdpa(dev, "vk" #n);
   FNS(X)
#undef X
}

/* ---- scenarios: build the objects, destroy them again, return the first failing VkResult ---- */
#define TRY(e) do { r = (e); if (r != VK_SUCCESS) goto out; } while (0)
#define NH(h) do { if ((h) != VK_NULL_HANDLE) { FAIL("%s: a failed call left a non-null output handle", scn_name); (h) = VK_NULL_HANDLE; } } while (0)
static const char *scn_name;

static VkResult
scn_instance(void)
{
   VkInstance i = VK_NULL_HANDLE;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkResult r = ((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, CB, &i);
   if (r != VK_SUCCESS) {
      NH(i);
      return r;
   }
   VkPhysicalDevice pd[2];
   uint32_t n = 2;
   r = ((PFN_vkEnumeratePhysicalDevices)gipa(i, "vkEnumeratePhysicalDevices"))(i, &n, pd);
   if (r == VK_INCOMPLETE)
      r = VK_SUCCESS;
   ((PFN_vkDestroyInstance)gipa(i, "vkDestroyInstance"))(i, CB);
   return r;
}

/* instance with the counting allocator, device + one queue submit, destroy */
static VkResult
scn_device(void)
{
   VkDevice d = VK_NULL_HANDLE;
   float pr = 1;
   VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &pr};
   VkDeviceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q};
   VkResult r = ((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(phys, &ci, CB, &d);
   if (r != VK_SUCCESS) {
      NH(d);
      return r;
   }
   VkQueue qq;
   p_GetDeviceQueue(d, 0, 0, &qq);
   VkFence fence;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   r = p_CreateFence(d, &fi, CB, &fence);
   if (r == VK_SUCCESS) {
      r = p_QueueSubmit(qq, 0, NULL, fence);
      if (r == VK_SUCCESS)
         r = p_WaitForFences(d, 1, &fence, VK_TRUE, 10ull * 1000000000);
      p_DestroyFence(d, fence, CB);
   }
   p_DestroyDevice(d, CB);
   return r;
}

static VkResult
scn_memory(void)
{
   VkDeviceMemory m = VK_NULL_HANDLE, m2 = VK_NULL_HANDLE;
   VkResult r;
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 65536, .memoryTypeIndex = 0};
   r = p_AllocateMemory(dev, &ai, CB, &m);
   if (r != VK_SUCCESS) {
      NH(m);
      return r;
   }
   ai.allocationSize = 4096;
   r = p_AllocateMemory(dev, &ai, CB, &m2);
   if (r != VK_SUCCESS)
      NH(m2);
   p_FreeMemory(dev, m2, CB);
   p_FreeMemory(dev, m, CB);
   return r;
}

static VkResult
scn_buffer_image(void)
{
   VkBuffer b = VK_NULL_HANDLE;
   VkBufferView bv = VK_NULL_HANDLE;
   VkImage im = VK_NULL_HANDLE;
   VkImageView iv = VK_NULL_HANDLE;
   VkSampler sm = VK_NULL_HANDLE;
   VkResult r;
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT};
   TRY(p_CreateBuffer(dev, &bci, CB, &b));
   VkBufferViewCreateInfo bvi = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = b, .format = VK_FORMAT_R8G8B8A8_UNORM, .range = VK_WHOLE_SIZE};
   TRY(p_CreateBufferView(dev, &bvi, CB, &bv));
   VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {16, 16, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   TRY(p_CreateImage(dev, &ici, CB, &im));
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = VK_FORMAT_R8G8B8A8_UNORM,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   TRY(p_CreateImageView(dev, &vi, CB, &iv));
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                             .maxLod = 1};
   TRY(p_CreateSampler(dev, &si, CB, &sm));
out:
   p_DestroySampler(dev, sm, CB);
   p_DestroyImageView(dev, iv, CB);
   p_DestroyImage(dev, im, CB);
   p_DestroyBufferView(dev, bv, CB);
   p_DestroyBuffer(dev, b, CB);
   return r;
}

static VkResult
scn_descriptors(void)
{
   VkDescriptorSetLayout l = VK_NULL_HANDLE;
   VkDescriptorPool pool = VK_NULL_HANDLE;
   VkDescriptorSet set[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   VkPipelineLayout pl = VK_NULL_HANDLE;
   VkResult r;
   VkDescriptorSetLayoutBinding bind[2] = {
      {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL},
      {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL}};
   VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bind};
   TRY(p_CreateDescriptorSetLayout(dev, &lci, CB, &l));
   VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}};
   VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 2, .pPoolSizes = ps};
   TRY(p_CreateDescriptorPool(dev, &pci, CB, &pool));
   VkDescriptorSetLayout ls[2] = {l, l};
   VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 2, .pSetLayouts = ls};
   r = p_AllocateDescriptorSets(dev, &ai, set);
   if (r != VK_SUCCESS) {
      /* spec: on failure every pSets entry is VK_NULL_HANDLE and the pool is unchanged */
      NH(set[0]);
      NH(set[1]);
      goto out;
   }
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &l};
   TRY(p_CreatePipelineLayout(dev, &plci, CB, &pl));
out:
   p_DestroyPipelineLayout(dev, pl, CB);
   p_DestroyDescriptorPool(dev, pool, CB);   /* frees the sets */
   p_DestroyDescriptorSetLayout(dev, l, CB);
   return r;
}

static uint32_t *g_spv_words;
static size_t g_spv_bytes;

static VkResult
scn_pipeline(void)
{
   VkShaderModule sm = VK_NULL_HANDLE;
   VkPipelineLayout pl = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   VkResult r;
   VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = g_spv_bytes, .pCode = g_spv_words};
   TRY(p_CreateShaderModule(dev, &smi, CB, &sm));
   VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
   TRY(p_CreatePipelineLayout(dev, &plci, CB, &pl));
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = sm, .pName = "main"}};
   r = p_CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, CB, &pipe);
   if (r != VK_SUCCESS)
      NH(pipe);
out:
   p_DestroyPipeline(dev, pipe, CB);
   p_DestroyPipelineLayout(dev, pl, CB);
   p_DestroyShaderModule(dev, sm, CB);
   return r;
}

static VkResult
scn_cmd_sync(void)
{
   VkCommandPool pool = VK_NULL_HANDLE;
   VkCommandBuffer cb[2] = {NULL, NULL};
   VkFence fence = VK_NULL_HANDLE;
   VkSemaphore sem = VK_NULL_HANDLE;
   VkEvent ev = VK_NULL_HANDLE;
   VkQueryPool qp = VK_NULL_HANDLE;
   VkResult r;
   VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   TRY(p_CreateCommandPool(dev, &pi, CB, &pool));
   VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
   r = p_AllocateCommandBuffers(dev, &ai, cb);
   if (r != VK_SUCCESS) {
      if (cb[0] || cb[1])
         FAIL("%s: AllocateCommandBuffers failed but left non-null handles", scn_name);
      goto out;
   }
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   TRY(p_CreateFence(dev, &fi, CB, &fence));
   VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   TRY(p_CreateSemaphore(dev, &semi, CB, &sem));
   VkEventCreateInfo evi = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
   TRY(p_CreateEvent(dev, &evi, CB, &ev));
   VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 8};
   TRY(p_CreateQueryPool(dev, &qi, CB, &qp));
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   TRY(p_BeginCommandBuffer(cb[0], &bi));
   TRY(p_EndCommandBuffer(cb[0]));
out:
   p_DestroyQueryPool(dev, qp, CB);
   p_DestroyEvent(dev, ev, CB);
   p_DestroySemaphore(dev, sem, CB);
   p_DestroyFence(dev, fence, CB);
   p_DestroyCommandPool(dev, pool, CB);   /* frees the buffers */
   return r;
}

static VkResult
scn_render_pass(void)
{
   VkRenderPass rp = VK_NULL_HANDLE;
   VkFramebuffer fb = VK_NULL_HANDLE;
   VkImage im = VK_NULL_HANDLE;
   VkImageView iv = VK_NULL_HANDLE;
   VkResult r;
   VkAttachmentDescription ad = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
                                 .subpassCount = 1, .pSubpasses = &sp};
   TRY(p_CreateRenderPass(dev, &rpi, CB, &rp));
   VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {16, 16, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   TRY(p_CreateImage(dev, &ici, CB, &im));
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   TRY(p_CreateImageView(dev, &vi, CB, &iv));
   VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &iv,
                                  .width = 16, .height = 16, .layers = 1};
   TRY(p_CreateFramebuffer(dev, &fci, CB, &fb));
out:
   p_DestroyFramebuffer(dev, fb, CB);
   p_DestroyImageView(dev, iv, CB);
   p_DestroyImage(dev, im, CB);
   p_DestroyRenderPass(dev, rp, CB);
   return r;
}

/* record + submit + wait: allocation failures inside QueueSubmit / WaitForFences must surface as an error, never crash or leak */
static VkResult
scn_submit(void)
{
   VkCommandPool pool = VK_NULL_HANDLE;
   VkCommandBuffer cb = VK_NULL_HANDLE;
   VkBuffer b = VK_NULL_HANDLE;
   VkDeviceMemory m = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   VkResult r;
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   TRY(p_CreateBuffer(dev, &bci, CB, &b));
   VkMemoryRequirements mr;
   p_GetBufferMemoryRequirements(dev, b, &mr);
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mr.memoryTypeBits)};
   TRY(p_AllocateMemory(dev, &mai, CB, &m));
   TRY(p_BindBufferMemory(dev, b, m, 0));
   VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   TRY(p_CreateCommandPool(dev, &pi, CB, &pool));
   VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   TRY(p_AllocateCommandBuffers(dev, &ai, &cb));
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   TRY(p_CreateFence(dev, &fi, CB, &fence));
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   TRY(p_BeginCommandBuffer(cb, &bi));
   p_CmdFillBuffer(cb, b, 0, 4096, 0x01020304);
   TRY(p_EndCommandBuffer(cb));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   TRY(p_QueueSubmit(queue, 1, &si, fence));
   TRY(p_WaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000000000));
out:
   /* a failed submit may still have queued work: drain before freeing what it uses */
   p_QueueWaitIdle(queue);
   p_DestroyFence(dev, fence, CB);
   p_DestroyCommandPool(dev, pool, CB);
   p_DestroyBuffer(dev, b, CB);
   p_FreeMemory(dev, m, CB);
   return r;
}

static char *g_vert_spv, *g_frag_spv;
static uint32_t *
load_spv(const char *path, size_t *bytes)
{
   FILE *fp = fopen(path, "rb");
   if (!fp)
      return NULL;
   fseek(fp, 0, SEEK_END);
   long n = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   uint32_t *w = n > 0 ? malloc((size_t)n) : NULL;
   if (!w || fread(w, 1, (size_t)n, fp) != (size_t)n) {
      free(w);
      fclose(fp);
      return NULL;
   }
   fclose(fp);
   *bytes = (size_t)n;
   return w;
}

static uint32_t *g_vs_words, *g_fs_words;
static size_t g_vs_bytes, g_fs_bytes;

static VkResult
scn_gfx_pipeline(void)
{
   VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
   VkPipelineLayout pl = VK_NULL_HANDLE;
   VkRenderPass rp = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   VkResult r;
   if (!g_vs_words || !g_fs_words)
      return VK_SUCCESS;
   VkShaderModuleCreateInfo vsi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = g_vs_bytes, .pCode = g_vs_words};
   VkShaderModuleCreateInfo fsi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = g_fs_bytes, .pCode = g_fs_words};
   TRY(p_CreateShaderModule(dev, &vsi, CB, &vs));
   TRY(p_CreateShaderModule(dev, &fsi, CB, &fs));
   VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
   TRY(p_CreatePipelineLayout(dev, &plci, CB, &pl));
   VkAttachmentDescription ad = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
                                 .subpassCount = 1, .pSubpasses = &sp};
   TRY(p_CreateRenderPass(dev, &rpi, CB, &rp));
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
   VkVertexInputBindingDescription vb = {0, 32, VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16}};
   VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1,
                                               .pVertexBindingDescriptions = &vb, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = va};
   VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, 16, 16, 0, 1};
   VkRect2D sc = {{0, 0}, {16, 16}};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp,
                                            .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
                                                .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
   VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                      .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
                                      .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl, .renderPass = rp};
   r = p_CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, CB, &pipe);
   if (r != VK_SUCCESS)
      NH(pipe);
out:
   p_DestroyPipeline(dev, pipe, CB);
   p_DestroyRenderPass(dev, rp, CB);
   p_DestroyPipelineLayout(dev, pl, CB);
   p_DestroyShaderModule(dev, fs, CB);
   p_DestroyShaderModule(dev, vs, CB);
   return r;
}

/* ---- sweep driver ---- */
enum { HOST, BO, LIBC };
static const char *const kname[] = {"host", "bo", "libc"};

static int
result_ok(int kind, VkResult r, int is_dev)
{
   if (r == VK_ERROR_OUT_OF_HOST_MEMORY)
      return 1;
   if (kind == BO && (r == VK_ERROR_OUT_OF_DEVICE_MEMORY || (is_dev && r == VK_ERROR_INITIALIZATION_FAILED)))
      return 1;
   return 0;
}

static int seq;

static void
sweep(const char *name, VkResult (*fn)(void), int kind, int is_dev)
{
   scn_name = name;
   const long fds0 = count_dir("/proc/self/fd"), maps0 = count_maps();
   if (kind == LIBC) {   /* warm-up: lazily created process-wide state must not count as a leak of the failing runs */
      for (int i = 0; i < 2; i++) {   /* twice: some statics are only touched on the second pass */
         g_armed = 1;
         VkResult w = fn();
         g_armed = 0;
         if (w != VK_SUCCESS)
            FAIL("%s: warm-up run failed (%d)", name, (int)w);
      }
   }
   long live0 = g_live, libc0 = g_libc_live;
   g_report_from = g_seq;
   int nfail = 0, last = 0;
   for (int n = 1; n <= 4000; n++) {
      char nb[32];
      g_count = 0;
      if (kind == HOST) {
         g_fail_at = n;
      } else if (kind == LIBC) {
         g_fail_at = 0;
         g_libc_count = 0;
         g_libc_fail_at = n;
         g_armed = 1;
      } else {
         g_fail_at = 0;
         snprintf(nb, sizeof nb, "%d.%d", n, ++seq);   /* the suffix restarts the fake's counter */
         setenv("MALI_FAKE_FAIL_BO", nb, 1);
      }
      if (g_selftest && n == 2)
         g_leak_one = 1;
      VkResult r = fn();
      g_armed = 0;
      g_libc_fail_at = 0;
      g_fail_at = 0;
      unsetenv("MALI_FAKE_FAIL_BO");
      if (g_leak_one) {   /* self-test: the failing call did not reach a free */
         g_leak_one = 0;
      }
      if (kind == LIBC && r == VK_SUCCESS && g_libc_live != libc0) {
         /* Mesa keeps lazily built process-wide caches that a faulted earlier run may have left half-initialised.
          * Only growth across a repeat of the same unfaulted run is a leak. */
         long after1 = g_libc_live;
         g_armed = 1;
         VkResult r2 = fn();
         g_armed = 0;
         if (r2 == VK_SUCCESS && g_libc_live == after1)
            libc0 = g_libc_live;
      }
      if (kind == LIBC && g_libc_live != libc0) {
         FAIL("%s n=%d (libc): %ld malloc block(s) leaked%s", name, n, g_libc_live - libc0, r == VK_SUCCESS ? " (success path)" : "");
         if (getenv("T_FAULT_LEAKS"))
            report_live();
         libc0 = g_libc_live;
         g_report_from = g_seq;
      }
      if (r == VK_SUCCESS) {
         last = n;   /* never reached the n-th allocation: the sweep is complete */
         if (g_live != live0)
            FAIL("%s n=%d (%s): success path leaked %ld allocation(s)", name, n, kname[kind], g_live - live0);
         break;
      }
      nfail++;
      if (!result_ok(kind, r, is_dev))
         FAIL("%s n=%d (%s): unexpected VkResult %d", name, n, kname[kind], (int)r);
      if (g_live != live0) {
         FAIL("%s n=%d (%s): %ld host allocation(s) leaked by the failed call", name, n, kname[kind], g_live - live0);
         live0 = g_live;   /* report each leak once */
      }
   }
   long fds = count_dir("/proc/self/fd"), maps = count_maps();
   if (fds != fds0)
      FAIL("%s (%s): %ld fd(s) leaked", name, kname[kind], fds - fds0);
   if (maps > maps0 + 2)   /* a few lines of allocator noise are fine, a leaked BO mapping per n is not */
      FAIL("%s (%s): %ld /proc/self/maps lines leaked", name, kname[kind], maps - maps0);
   if (!last)
      FAIL("%s (%s): sweep never reached a successful run (n limit)", name, kname[kind]);
   if (!nfail && kind != HOST)   /* HOST: some objects ignore pAllocator and use libc (covered by the libc sweep) */
      FAIL("%s (%s): no fault was ever injected", name, kname[kind]);
   printf("{\"sweep\":\"%s\",\"kind\":\"%s\",\"faults\":%d}\n", name, kname[kind], nfail);
}

static int
read_spv(const char *path)
{
   FILE *fp = fopen(path, "rb");
   if (!fp)
      return -1;
   fseek(fp, 0, SEEK_END);
   long n = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   g_spv_words = malloc((size_t)n);
   if (!g_spv_words || n <= 0 || fread(g_spv_words, 1, (size_t)n, fp) != (size_t)n) {
      fclose(fp);
      return -1;
   }
   fclose(fp);
   g_spv_bytes = (size_t)n;
   return 0;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--spv") && i + 1 < argc) g_spv = argv[++i];
      else if (!strcmp(argv[i], "--vert") && i + 1 < argc) g_vert_spv = argv[++i];
      else if (!strcmp(argv[i], "--frag") && i + 1 < argc) g_frag_spv = argv[++i];
      else if (!strcmp(argv[i], "--self-test")) g_selftest = 1;
   }
   if (!icd || !g_spv || read_spv(g_spv)) {
      fprintf(stderr, "usage: t_fault --direct icd.so --spv comp.spv [--self-test]\n");
      return 2;
   }
   if (g_vert_spv && g_frag_spv) {
      g_vs_words = load_spv(g_vert_spv, &g_vs_bytes);
      g_fs_words = load_spv(g_frag_spv, &g_fs_bytes);
      if (!g_vs_words || !g_fs_words) { fprintf(stderr, "cannot read --vert/--frag\n"); return 2; }
   }
   void *h = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
   if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");

   /* the instance scenario runs first, on its own instance, then the shared one (created without faults) */
   if (g_selftest) {
      /* mutation check: the allocator drops one free on the n=2 failure, so the sweep must flag a leak */
      sweep("instance", scn_instance, HOST, 0);
      return g_fail ? 0 : 1;
   }
   sweep("instance", scn_instance, HOST, 0);
   sweep("instance", scn_instance, LIBC, 0);

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
   uint32_t n = 1;
   if (((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &phys) < 0 || n < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");

   /* device functions need a device: load them from a throw-away one, then keep a long-lived device for the object scenarios */
   float pr = 1;
   VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &pr};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q};
   if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(phys, &dci, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
   load_dev_fns();
   p_GetDeviceQueue(dev, 0, 0, &queue);

   static const struct { const char *name; VkResult (*fn)(void); int is_dev; } sc[] = {
      {"device", scn_device, 1}, {"memory", scn_memory, 0}, {"buffer_image", scn_buffer_image, 0},
      {"descriptors", scn_descriptors, 0}, {"pipeline", scn_pipeline, 0}, {"cmd_sync", scn_cmd_sync, 0},
      {"render_pass", scn_render_pass, 0}, {"submit", scn_submit, 0}, {"gfx_pipeline", scn_gfx_pipeline, 0},
   };
   for (unsigned i = 0; i < sizeof sc / sizeof *sc; i++) {
      sweep(sc[i].name, sc[i].fn, HOST, sc[i].is_dev);
      sweep(sc[i].name, sc[i].fn, BO, sc[i].is_dev);
      sweep(sc[i].name, sc[i].fn, LIBC, sc[i].is_dev);
   }
   p_DestroyDevice(dev, NULL);
   ((PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance"))(inst, NULL);
   printf("{\"summary\":\"%s\"}\n", g_fail ? "FAIL" : "ok");
   return g_fail;
}
