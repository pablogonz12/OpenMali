/* SPDX-License-Identifier: MIT */
/* Query API edge cases through the ICD (loader-free, fake backend): the host-side result packing and the rejection paths of
 * mali_query.c / mali_query_core.c that the happy-path tests (t_vk_feat, t_vk_meta) never reach.
 *   t_vk_qerr --direct <icd.so>
 * 1. vkCreateQueryPool: occlusion + timestamp work, PIPELINE_STATISTICS is VK_ERROR_FEATURE_NOT_PRESENT;
 * 2. vkGetQueryPoolResults on never-written queries: VK_NOT_READY (32-bit), PARTIAL|64BIT is VK_SUCCESS with zero data,
 *    WITH_AVAILABILITY writes a 0 availability word next to each value;
 * 3. malformed requests (range past the pool, stride too small / unaligned, data buffer too small) fail with VK_ERROR_UNKNOWN
 *    and leave the caller's buffer untouched;
 * 4. command-buffer misuse (BeginQuery/EndQuery on a timestamp pool, WriteTimestamp2 on an occlusion pool or past the end) makes
 *    vkEndCommandBuffer fail, while the matching correct calls succeed;
 * 5. vkResetQueryPool resets in range. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                       \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                               \
      }                                                                                            \
   } while (0)

#define DEV_FNS(X)                                                                                 \
   X(DestroyDevice) X(GetDeviceQueue) X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) \
   X(ResetQueryPool) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)           \
   X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginQuery) X(CmdEndQuery) X(CmdWriteTimestamp2)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)

static VkDevice dev;
static VkCommandBuffer cb;

static VkQueryPool
mkpool(VkQueryType t, uint32_t n)
{
   VkQueryPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = t, .queryCount = n};
   VkQueryPool p = VK_NULL_HANDLE;
   VkResult r = CreateQueryPool(dev, &ci, NULL, &p);
   CHECK(r == VK_SUCCESS, "create pool type %d -> %d", t, r);
   return p;
}

/* record `f`, return vkEndCommandBuffer's result */
#define RECORD(body, out)                                                                          \
   do {                                                                                            \
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};       \
      CHECK(BeginCommandBuffer(cb, &bi) == VK_SUCCESS, "begin");                                   \
      body;                                                                                        \
      out = EndCommandBuffer(cb);                                                                  \
   } while (0)

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc)
         icd = argv[++i];
   if (!icd) { fprintf(stderr, "usage: t_vk_qerr --direct icd.so\n"); return 2; }

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("icd entry\n"); return 1; }

   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("CreateInstance\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices EPD = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr GDPA = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   PFN_vkDestroyInstance DestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   VkResult r = EPD(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) { printf("no device\n"); return 1; }

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .synchronization2 = VK_TRUE};
   VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13, .hostQueryReset = VK_TRUE};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f12, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("CreateDevice\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GDPA(dev, "vk" #n); if (!n) { printf("missing vk" #n "\n"); return 1; }
   DEV_FNS(LOAD)

   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool cp;
   CHECK(CreateCommandPool(dev, &pci, NULL, &cp) == VK_SUCCESS, "cmd pool");
   VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   CHECK(AllocateCommandBuffers(dev, &cbi, &cb) == VK_SUCCESS, "alloc cb");

   /* 1. create */
   VkQueryPoolCreateInfo bad = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS, .queryCount = 1,
                                .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT};
   VkQueryPool none = VK_NULL_HANDLE;
   r = CreateQueryPool(dev, &bad, NULL, &none);
   CHECK(r == VK_ERROR_FEATURE_NOT_PRESENT, "pipeline-statistics pool -> %d", r);
   CHECK(none == VK_NULL_HANDLE, "no handle on failure");

   VkQueryPool occ = mkpool(VK_QUERY_TYPE_OCCLUSION, 4), ts = mkpool(VK_QUERY_TYPE_TIMESTAMP, 4);

   /* 2. results of never-written queries */
   uint32_t u32[8];
   uint64_t u64[8];
   memset(u32, 0xAA, sizeof u32);
   r = GetQueryPoolResults(dev, occ, 0, 4, sizeof u32, u32, 4, 0);
   CHECK(r == VK_NOT_READY, "unwritten 32-bit -> %d", r);
   memset(u64, 0xAA, sizeof u64);
   r = GetQueryPoolResults(dev, occ, 0, 4, sizeof u64, u64, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_PARTIAL_BIT);
   CHECK(r == VK_SUCCESS, "partial|64 -> %d", r);
   for (int i = 0; i < 4; i++)
      CHECK(u64[i] == 0, "partial value[%d] = 0x%llx (pool memory is zeroed)", i, (unsigned long long)u64[i]);
   CHECK(u64[4] == 0xAAAAAAAAAAAAAAAAull, "wrote past the 4 requested queries");
   memset(u64, 0xAA, sizeof u64);
   r = GetQueryPoolResults(dev, ts, 1, 2, sizeof u64, u64, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
   CHECK(r == VK_NOT_READY, "avail, unwritten -> %d", r);
   CHECK(u64[1] == 0 && u64[3] == 0, "availability words = 0 (got %llx %llx)", (unsigned long long)u64[1], (unsigned long long)u64[3]);
   CHECK(u64[4] == 0xAAAAAAAAAAAAAAAAull, "range [1,3) wrote too much");
   memset(u32, 0xAA, sizeof u32);
   r = GetQueryPoolResults(dev, ts, 0, 2, sizeof u32, u32, 8, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_PARTIAL_BIT);
   CHECK(r == VK_SUCCESS, "partial+avail 32-bit -> %d", r);
   CHECK(u32[1] == 0 && u32[3] == 0 && u32[0] == 0 && u32[2] == 0, "32-bit partial words %x %x %x %x", u32[0], u32[1], u32[2], u32[3]);

   /* 3. malformed requests: error and the buffer is untouched */
   struct { uint32_t first, count; size_t size; uint64_t stride; VkQueryResultFlags fl; const char *what; } badq[] = {
      {3, 2, sizeof u32, 4, 0, "range past the pool"},
      {5, 0, sizeof u32, 4, 0, "first past the pool"},
      {0, 2, sizeof u32, 2, 0, "stride < element"},
      {0, 2, sizeof u32, 6, 0, "stride not a multiple of the element size"},
      {0, 2, sizeof u64, 12, VK_QUERY_RESULT_64_BIT, "64-bit stride not 8-aligned"},
      {0, 4, 8, 4, 0, "data buffer too small"},
      {0, 2, 8, 8, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, "buffer too small for value+availability"},
   };
   for (unsigned i = 0; i < sizeof badq / sizeof *badq; i++) {
      memset(u32, 0xAA, sizeof u32);
      r = GetQueryPoolResults(dev, occ, badq[i].first, badq[i].count, badq[i].size, u32, badq[i].stride, badq[i].fl);
      CHECK(r == VK_ERROR_UNKNOWN, "%s -> %d", badq[i].what, r);
      for (int k = 0; k < 8; k++)
         CHECK(u32[k] == 0xAAAAAAAAu, "%s: buffer written (word %d)", badq[i].what, k);
   }
   /* zero queries is fine (even when first == count of the pool) */
   r = GetQueryPoolResults(dev, occ, 4, 0, 0, u32, 4, 0);
   CHECK(r == VK_SUCCESS, "empty range at the end -> %d", r);

   /* 4. command-buffer misuse is reported at vkEndCommandBuffer, the right calls are not */
   VkResult e;
   RECORD((CmdBeginQuery(cb, occ, 0, 0), CmdEndQuery(cb, occ, 0)), e);
   CHECK(e == VK_SUCCESS, "occlusion begin/end -> %d", e);
   RECORD(CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ts, 3), e);
   CHECK(e == VK_SUCCESS, "timestamp write -> %d", e);
   RECORD(CmdBeginQuery(cb, ts, 0, 0), e);
   CHECK(e == VK_ERROR_FEATURE_NOT_PRESENT, "BeginQuery on a timestamp pool -> %d", e);
   RECORD(CmdBeginQuery(cb, occ, 4, 0), e);
   CHECK(e == VK_ERROR_FEATURE_NOT_PRESENT, "BeginQuery past the pool -> %d", e);
   RECORD(CmdEndQuery(cb, ts, 0), e);
   CHECK(e == VK_ERROR_FEATURE_NOT_PRESENT, "EndQuery on a timestamp pool -> %d", e);
   RECORD(CmdEndQuery(cb, occ, 4), e);
   CHECK(e == VK_ERROR_FEATURE_NOT_PRESENT, "EndQuery past the pool -> %d", e);
   RECORD(CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, occ, 0), e);
   CHECK(e == VK_ERROR_FEATURE_NOT_PRESENT, "WriteTimestamp2 on an occlusion pool -> %d", e);
   RECORD(CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ts, 4), e);
   CHECK(e == VK_ERROR_FEATURE_NOT_PRESENT, "WriteTimestamp2 past the pool -> %d", e);

   /* 5. host reset */
   ResetQueryPool(dev, occ, 0, 4);
   r = GetQueryPoolResults(dev, occ, 0, 4, sizeof u32, u32, 4, 0);
   CHECK(r == VK_NOT_READY, "after reset -> %d", r);

   DestroyQueryPool(dev, occ, NULL);
   DestroyQueryPool(dev, ts, NULL);
   DestroyCommandPool(dev, cp, NULL);
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   dlclose(lib);
   printf(g_fail ? "FAIL\n" : "ok\n");
   return g_fail;
}
