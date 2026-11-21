/* SPDX-License-Identifier: MIT */
/* Host test of the VRAM commit budget against the kbase BO pool (libkbase fake kernel, real VkDevice objects).
 *
 * The budget (pdev->mem_used / mem_cap, MALI_VRAM_CAP_MB) is charged when a VkDeviceMemory is reserved and released
 * when the memory is really gone. A HOSTLAZY BO the kbase pool keeps after vkFreeMemory is still committed in kbase,
 * so its charge must stay with the retained BO and be released exactly once: when the pool hands it out again, when the
 * pool evicts it, or when the device destroys the pool. Releasing it in vkFreeMemory as well would underflow mem_used
 * (the heap usage would wrap); never releasing it would refuse allocations that fit in RAM.
 *
 * Cases (heapUsage[0] is the observable, VK_EXT_memory_budget):
 *   (a) allocate -> free: the pool keeps the BO, the budget is still charged exactly once
 *   (b) re-allocate from the pool: the charge moves to the new memory, never doubles
 *   (c) the charge is released exactly once when the pool really frees the BO (eviction under pressure)
 *   (d) a request over the cap with nothing to reclaim fails OUT_OF_DEVICE_MEMORY and leaves the budget unchanged
 *   (e) DR-1: a second VkDevice on the same physical device can reclaim the first device's pooled charge
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);

static int g_fail;
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("FAIL  ");                                                                         \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                               \
         return 0;                                                                                 \
      }                                                                                            \
      printf("ok    ");                                                                            \
      printf(__VA_ARGS__);                                                                         \
      printf("\n");                                                                                \
   } while (0)

#define MIB (1024u * 1024u)

static VkInstance inst;
static VkPhysicalDevice pd;
static PFN_vkGetDeviceProcAddr gdpa;
static PFN_vkGetPhysicalDeviceMemoryProperties2 gdmp2;

typedef struct dev {
   VkDevice d;
   PFN_vkAllocateMemory AllocateMemory;
   PFN_vkFreeMemory FreeMemory;
   PFN_vkDestroyDevice DestroyDevice;
} vkdev_t;

static uint64_t
usage(void)
{
   VkPhysicalDeviceMemoryBudgetPropertiesEXT bud = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
   VkPhysicalDeviceMemoryProperties2 mp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bud};
   gdmp2(pd, &mp);
   return bud.heapUsage[0];
}

static uint64_t
budget(void)
{
   VkPhysicalDeviceMemoryBudgetPropertiesEXT bud = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
   VkPhysicalDeviceMemoryProperties2 mp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bud};
   gdmp2(pd, &mp);
   return bud.heapBudget[0];
}

static int
open_device(vkdev_t *o)
{
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                  .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qci};
   VkDevice d;
   PFN_vkCreateDevice cdev = (PFN_vkCreateDevice)vk_icdGetInstanceProcAddr(inst, "vkCreateDevice");
   if (cdev(pd, &dci, NULL, &d) != VK_SUCCESS)
      return 0;
   o->d = d;
   o->AllocateMemory = (PFN_vkAllocateMemory)gdpa(d, "vkAllocateMemory");
   o->FreeMemory = (PFN_vkFreeMemory)gdpa(d, "vkFreeMemory");
   o->DestroyDevice = (PFN_vkDestroyDevice)gdpa(d, "vkDestroyDevice");
   return o->AllocateMemory && o->FreeMemory && o->DestroyDevice;
}

static VkDeviceMemory
alloc_mem(vkdev_t *v, uint64_t size)
{
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size, .memoryTypeIndex = 0};
   VkDeviceMemory m = VK_NULL_HANDLE;
   VkResult r = v->AllocateMemory(v->d, &ai, NULL, &m);
   if (r != VK_SUCCESS)
      return VK_NULL_HANDLE;
   return m;
}

/* Two allocators on two devices of the same budget race the reclaim path (mem_reserve -> pool purge, DR-1's shared
 * pool list + the atomic mem_used). Nothing may hang (timed join is the watchdog) or corrupt the accounting. */
typedef struct { vkdev_t *v; unsigned n; uint64_t size; } alloc_thr;

static void *
alloc_worker(void *a)
{
   alloc_thr *t = a;
   for (unsigned i = 0; i < t->n; i++) {
      VkDeviceMemory m = alloc_mem(t->v, t->size);
      if (m)
         t->v->FreeMemory(t->v->d, m, NULL);
   }
   return NULL;
}

int
main(void)
{
   /* A commit limit well under the pool's total capacity: pooled charges are what the tests see. */
   setenv("MALI_VRAM_CAP_MB", "8", 1);
   setenv("MALI_BO_POOL_MB", "8", 1);

   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) = vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance ci_fn = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   if (ci_fn(&ici, NULL, &inst) != VK_SUCCESS) { printf("FAIL  CreateInstance\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices enumpd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   gdmp2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)gipa(inst, "vkGetPhysicalDeviceMemoryProperties2");
   gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   if (!gdmp2 || !gdpa) { printf("FAIL  the ICD has no vkGetPhysicalDeviceMemoryProperties2/vkGetDeviceProcAddr\n"); return 1; }
   uint32_t npd = 1;
   if (enumpd(inst, &npd, &pd) != VK_SUCCESS || npd < 1) { printf("FAIL  no physical device\n"); return 1; }
   CHECK(budget() == 8ull * MIB, "MALI_VRAM_CAP_MB=8 caps heap 0 at 8 MiB (budget %llu)", (unsigned long long)budget());

   vkdev_t A;
   CHECK(open_device(&A), "CreateDevice A");
   CHECK(usage() == 0, "fresh device: nothing charged (%llu)", (unsigned long long)usage());

   /* (a) allocate -> free: the pool keeps the BO, the charge stays (the BO is still committed) */
   VkDeviceMemory m = alloc_mem(&A, MIB);
   CHECK(m, "alloc 1 MiB");
   CHECK(usage() == MIB, "(a) 1 MiB allocation charged once (%llu)", (unsigned long long)usage());
   A.FreeMemory(A.d, m, NULL);
   CHECK(usage() == MIB, "(a) freed into the pool: still charged exactly once (%llu, want %u)", (unsigned long long)usage(), MIB);

   /* (b) a pool hit moves the charge, it does not add one */
   m = alloc_mem(&A, MIB);
   CHECK(m, "re-alloc 1 MiB from the pool");
   CHECK(usage() == MIB, "(b) pool hit: no double charge (%llu, want %u)", (unsigned long long)usage(), MIB);
   A.FreeMemory(A.d, m, NULL);
   CHECK(usage() == MIB, "(b) freed again: still one charge (%llu)", (unsigned long long)usage());

   /* (c) the charge is released exactly once, when the pool really frees the BO (8 MiB does not fit with 1 charged) */
   m = alloc_mem(&A, 8 * MIB);
   CHECK(m, "(c) 8 MiB allocation reclaims the pooled 1 MiB");
   CHECK(usage() == 8ull * MIB, "(c) after eviction the budget is the live 8 MiB, released once (%llu)", (unsigned long long)usage());
   A.FreeMemory(A.d, m, NULL);
   CHECK(usage() == 8ull * MIB, "(c) the freed 8 MiB is pooled and charged (%llu)", (unsigned long long)usage());
   VkDeviceMemory small = alloc_mem(&A, MIB);
   CHECK(small, "(c) 1 MiB fits once the 8 MiB pooled BO is evicted");
   CHECK(usage() == MIB, "(c) eviction released the 8 MiB charge exactly once (%llu, want %u; a double release wraps)", (unsigned long long)usage(), MIB);
   A.FreeMemory(A.d, small, NULL);

   /* (d) over the cap with nothing to reclaim: refused, budget unchanged */
   VkDeviceMemory big = alloc_mem(&A, 8 * MIB);
   CHECK(big, "(d) 8 MiB live allocation");
   CHECK(usage() == 8ull * MIB, "(d) live 8 MiB charged (%llu)", (unsigned long long)usage());
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = MIB, .memoryTypeIndex = 0};
   VkDeviceMemory over = VK_NULL_HANDLE;
   VkResult r = A.AllocateMemory(A.d, &ai, NULL, &over);
   CHECK(r == VK_ERROR_OUT_OF_DEVICE_MEMORY, "(d) over the cap is refused with OUT_OF_DEVICE_MEMORY (%d)", r);
   CHECK(usage() == 8ull * MIB, "(d) a refused allocation leaves the budget unchanged (%llu)", (unsigned long long)usage());
   A.FreeMemory(A.d, big, NULL);

   /* (e) DR-1: the pool keeping a freed BO is charged to the physical device, so another VkDevice can reclaim it */
   VkDeviceMemory pool3[3];
   for (int i = 0; i < 3; i++) {
      pool3[i] = alloc_mem(&A, 2 * MIB);
      CHECK(pool3[i], "(e) device A alloc %d (2 MiB)", i);
   }
   for (int i = 0; i < 3; i++)
      A.FreeMemory(A.d, pool3[i], NULL);
   CHECK(usage() == 6ull * MIB, "(e) device A's pool holds 6 MiB of the shared 8 MiB budget (%llu)", (unsigned long long)usage());

   vkdev_t B;
   CHECK(open_device(&B), "(e) CreateDevice B on the same physical device");
   VkDeviceMemory b4 = alloc_mem(&B, 4 * MIB);
   CHECK(b4, "(e) device B allocates 4 MiB by reclaiming device A's pooled charge");
   /* Reclaim takes only what the request needs: 2 MiB of A's 6 MiB pooled charge goes, the other 4 MiB stays pooled. */
   CHECK(usage() == 8ull * MIB, "(e) B's 4 MiB live + 4 MiB still pooled on A = the 8 MiB cap (%llu)", (unsigned long long)usage());
   B.FreeMemory(B.d, b4, NULL);

   /* (f) both devices allocate and free at once while the pools are charged: the reclaim path and the shared budget
    * must stay consistent (two threads, timed join = watchdog). */
   alloc_thr t1 = {&A, 60, 512 * 1024}, t2 = {&B, 60, 512 * 1024};
   pthread_t th1, th2;
   pthread_create(&th1, NULL, alloc_worker, &t1);
   pthread_create(&th2, NULL, alloc_worker, &t2);
   struct timespec dl;
   clock_gettime(CLOCK_REALTIME, &dl);   /* pthread_timedjoin_np's clock */
   dl.tv_sec += 60;
   int j1 = pthread_timedjoin_np(th1, NULL, &dl), j2 = pthread_timedjoin_np(th2, NULL, &dl);
   CHECK(j1 == 0 && j2 == 0, "(f) two-device alloc/free stress finished (watchdog 60 s, joins %d/%d)", j1, j2);
   CHECK(usage() <= 8ull * MIB, "(f) the shared budget never exceeded the cap (%llu)", (unsigned long long)usage());

   B.DestroyDevice(B.d, NULL);
   A.DestroyDevice(A.d, NULL);
   CHECK(usage() == 0, "both devices destroyed: every pooled charge released, no underflow (%llu)", (unsigned long long)usage());

   PFN_vkDestroyInstance DestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
   if (DestroyInstance)
      DestroyInstance(inst, NULL);
   printf("{\"test\":\"t_budget\",\"pass\":%s}\n", g_fail ? "false" : "true");
   return g_fail != 0;
}
