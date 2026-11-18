/* SPDX-License-Identifier: MIT */
/* Thread-safety test: concurrent entry points that Vulkan allows from several threads. Meant to run under TSan
 * (MALI_SAN=thread MALI_TARGET=native-san ./build.sh, then tests/build-t_threads.sh). Fake backend only.
 *   t_threads --direct <icd.so> [--iters N] [--only alloc|cmdpool|submit|descpool|fence]
 * Rules respected: objects are externally synchronised only where the spec says so (queue, pool, command buffer). */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NT 4
static int g_fail, g_iters = 200;
static const char *g_only;
#define CASE_ON(n) (!g_only || !strcmp(g_only, n))
#define CK(c)                                                                                      \
   do { if (!(c)) { printf("{\"fail\":\"%s:%d\",\"expr\":\"%s\"}\n", __FILE__, __LINE__, #c); __atomic_store_n(&g_fail, 1, __ATOMIC_SEQ_CST); return NULL; } } while (0)

static VkDevice dev;
static VkQueue queues[2];
static uint32_t nqueues;
static VkPhysicalDeviceMemoryProperties memprops;

#define FNS(X) X(AllocateMemory) X(FreeMemory) X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool) \
   X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) \
   X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) \
   X(GetFenceStatus) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) \
   X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(FreeDescriptorSets) X(ResetDescriptorPool) \
   X(UpdateDescriptorSets) X(CreateBuffer) X(DestroyBuffer) X(GetDeviceQueue) X(CmdFillBuffer) \
   X(CreateSemaphore) X(DestroySemaphore) X(GetBufferMemoryRequirements) X(BindBufferMemory)
#define DECL(n) static PFN_vk##n n;
FNS(DECL)

static void run(void *(*fn)(void *), const char *name)
{
   pthread_t th[NT];
   long ids[NT];
   for (long i = 0; i < NT; i++) { ids[i] = i; pthread_create(&th[i], NULL, fn, (void *)ids[i]); }
   for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);
   printf("{\"test\":\"%s\",\"ok\":%s}\n", name, g_fail ? "false" : "true");
}

static void *t_alloc(void *a)
{
   (void)a;
   for (int i = 0; i < g_iters; i++) {
      VkDeviceMemory m[4];
      for (int j = 0; j < 4; j++) {
         VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 65536 << (j & 1), .memoryTypeIndex = (i + j) & 1};
         CK(AllocateMemory(dev, &ai, NULL, &m[j]) == VK_SUCCESS);
      }
      for (int j = 0; j < 4; j++) FreeMemory(dev, m[j], NULL);
   }
   return NULL;
}

static void *t_cmdpool(void *a)
{
   (void)a;
   VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool p;
   CK(CreateCommandPool(dev, &pi, NULL, &p) == VK_SUCCESS);
   for (int i = 0; i < g_iters; i++) {
      VkCommandBuffer cb[3];
      VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = p, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3};
      CK(AllocateCommandBuffers(dev, &ai, cb) == VK_SUCCESS);
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      for (int j = 0; j < 3; j++) { CK(BeginCommandBuffer(cb[j], &bi) == VK_SUCCESS); CK(EndCommandBuffer(cb[j]) == VK_SUCCESS); }
      ResetCommandBuffer(cb[0], 0);
      if (i & 1) FreeCommandBuffers(dev, p, 3, cb); else ResetCommandPool(dev, p, 0);
   }
   DestroyCommandPool(dev, p, NULL);
   return NULL;
}

static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER; /* queue is externally synchronised */
static void *t_submit(void *a)
{
   long id = (long)a;
   VkQueue q = queues[id % nqueues];
   pthread_mutex_t *mu = nqueues > 1 ? NULL : &qmu;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence f;
   VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool p;
   VkCommandBuffer cb;
   CK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS);
   CK(CreateCommandPool(dev, &pi, NULL, &p) == VK_SUCCESS);
   VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = p, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   CK(AllocateCommandBuffers(dev, &ai, &cb) == VK_SUCCESS);
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   CK(BeginCommandBuffer(cb, &bi) == VK_SUCCESS);
   CK(EndCommandBuffer(cb) == VK_SUCCESS);
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   for (int i = 0; i < g_iters; i++) {
      if (mu) pthread_mutex_lock(mu);
      VkResult r = QueueSubmit(q, 1, &si, f);
      if (mu) pthread_mutex_unlock(mu);
      CK(r == VK_SUCCESS);
      CK(WaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull) == VK_SUCCESS);
      CK(ResetFences(dev, 1, &f) == VK_SUCCESS);
   }
   DestroyCommandPool(dev, p, NULL);
   DestroyFence(dev, f, NULL);
   return NULL;
}

static VkFence shared_fence;
static void *t_fence(void *a) /* one thread submits to a shared fence, others poll/wait it (allowed) */
{
   long id = (long)a;
   for (int i = 0; i < g_iters; i++) {
      if (id == 0) {
         VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
         CK(QueueSubmit(queues[0], 1, &si, shared_fence) == VK_SUCCESS);
         CK(QueueWaitIdle(queues[0]) == VK_SUCCESS);
         CK(WaitForFences(dev, 1, &shared_fence, VK_TRUE, 5000000000ull) == VK_SUCCESS);
         CK(ResetFences(dev, 1, &shared_fence) == VK_SUCCESS);
      } else {
         VkResult r = GetFenceStatus(dev, shared_fence);
         CK(r == VK_SUCCESS || r == VK_NOT_READY);
         WaitForFences(dev, 1, &shared_fence, VK_TRUE, 1000);
      }
   }
   return NULL;
}

static VkDescriptorSetLayout dsl;
static VkBuffer sbuf;
static void *t_descpool(void *a)
{
   (void)a;
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64};
   VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = 64, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool p;
   CK(CreateDescriptorPool(dev, &pi, NULL, &p) == VK_SUCCESS);
   for (int i = 0; i < g_iters; i++) {
      VkDescriptorSet s[8];
      VkDescriptorSetLayout l[8];
      for (int j = 0; j < 8; j++) l[j] = dsl;
      VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = p, .descriptorSetCount = 8, .pSetLayouts = l};
      CK(AllocateDescriptorSets(dev, &ai, s) == VK_SUCCESS);
      VkDescriptorBufferInfo bi = {sbuf, 0, VK_WHOLE_SIZE};
      for (int j = 0; j < 8; j++) {
         VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s[j], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
         UpdateDescriptorSets(dev, 1, &w, 0, NULL);
      }
      if (i & 1) CK(FreeDescriptorSets(dev, p, 8, s) == VK_SUCCESS); else ResetDescriptorPool(dev, p, 0);
   }
   DestroyDescriptorPool(dev, p, NULL);
   /* layouts/buffers/semaphores created and destroyed concurrently */
   for (int i = 0; i < g_iters; i++) {
      VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
      VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
      VkDescriptorSetLayout l;
      CK(CreateDescriptorSetLayout(dev, &li, NULL, &l) == VK_SUCCESS);
      DestroyDescriptorSetLayout(dev, l, NULL);
      VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkSemaphore sem;
      CK(CreateSemaphore(dev, &sci, NULL, &sem) == VK_SUCCESS);
      DestroySemaphore(dev, sem, NULL);
   }
   return NULL;
}

int main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--iters") && i + 1 < argc) g_iters = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
      else { fprintf(stderr, "usage: t_threads --direct icd.so [--iters N] [--only case]\n"); return 2; }
   }
   if (!icd) return 2;
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("{\"fail\":\"dlopen: %s\"}\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*neg)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !neg || neg(&iv)) return 1;
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst;
   if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst)) return 1;
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &cnt, &pd);
   ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &memprops);
   VkQueueFamilyProperties qf[8];
   uint32_t nqf = 8;
   ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"))(pd, &nqf, qf);
   nqueues = qf[0].queueCount > 2 ? 2 : qf[0].queueCount;
   float prio[2] = {1, 1};
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = nqueues, .pQueuePriorities = prio};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &di, NULL, &dev)) return 1;
   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define LOAD(n) n = (PFN_vk##n)gdpa(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   FNS(LOAD)
   for (uint32_t i = 0; i < nqueues; i++) GetDeviceQueue(dev, 0, i, &queues[i]);
   printf("{\"queues\":%u}\n", nqueues);

   VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
   VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (CreateDescriptorSetLayout(dev, &li, NULL, &dsl) || CreateBuffer(dev, &bci, NULL, &sbuf) || CreateFence(dev, &fi, NULL, &shared_fence)) return 1;
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, sbuf, &mr);
   VkDeviceMemory bm;
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = 0};
   if (AllocateMemory(dev, &mai, NULL, &bm) || BindBufferMemory(dev, sbuf, bm, 0)) return 1;

   if (CASE_ON("alloc")) run(t_alloc, "alloc");
   if (CASE_ON("cmdpool")) run(t_cmdpool, "cmdpool");
   if (CASE_ON("submit")) run(t_submit, "submit");
   if (CASE_ON("fence")) run(t_fence, "fence");
   if (CASE_ON("descpool")) run(t_descpool, "descpool");
   DeviceWaitIdle(dev);
   return g_fail;
}
