/* SPDX-License-Identifier: MIT */
/* Device-wide commit budget (mali_memory.c mem_reserve/mem_release, mali_private.h mem_cap).
 *   t_vk_vram --direct <icd.so>
 * Sets MALI_VRAM_CAP_MB in this process before the physical device is created, so the cap under test is the one
 * the driver reads (a fresh instance re-reads it). kbase pins every byte it is given (libkbase/src/bo.c:45),
 * therefore the budget has to be enforced before anything is allocated:
 *   - an allocation that fits the cap succeeds, one that would exceed it is refused with
 *     VK_ERROR_OUT_OF_DEVICE_MEMORY (no abort, no partial allocation),
 *   - the refusal does not consume budget: the next allocation that still fits succeeds,
 *   - freeing returns the bytes to the budget, so the same large allocation succeeds afterwards,
 *   - the charge is the page-rounded size the backend commits, not the byte count asked for: 2049 one-byte
 *     allocations do not fit an 8 MiB cap (2048 x 4096 B do),
 *   - both memory types (uncached and CPU-cached) are charged, so mixing them cannot commit twice the cap,
 *   - MALI_VRAM_CAP_MB=auto (or any other non-numeric value) is ignored: the default cap stays, it is not 0.
 * Exit 0 = pass. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                        \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                               \
      }                                                                                            \
   } while (0)

#define MIB (1024u * 1024u)

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static uint32_t mt_uncached, mt_cached;
static PFN_vkAllocateMemory pAllocateMemory;
static PFN_vkFreeMemory pFreeMemory;
static PFN_vkDestroyDevice pDestroyDevice;
static PFN_vkDestroyInstance pDestroyInstance;

static int
open_device(void)
{
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("FAIL CreateInstance\n"); return 1; }

#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceMemoryProperties); I(CreateDevice); I(GetDeviceProcAddr);
   I(DestroyInstance);

   uint32_t n = 1;
   if (EnumeratePhysicalDevices(inst, &n, &pd) != VK_SUCCESS || n != 1) { printf("FAIL enumerate\n"); return 1; }

   VkPhysicalDeviceMemoryProperties mp;
   GetPhysicalDeviceMemoryProperties(pd, &mp);
   mt_uncached = mt_cached = UINT32_MAX;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      if (!(mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         continue;
      if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
         if (mt_cached == UINT32_MAX) mt_cached = i;
      } else if (mt_uncached == UINT32_MAX) {
         mt_uncached = i;
      }
   }
   CHECK(mt_uncached != UINT32_MAX, "no host-visible uncached memory type");
   if (mt_uncached == UINT32_MAX) return 1;

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   if (CreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { printf("FAIL CreateDevice\n"); return 1; }
#define D(n) PFN_vk##n n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n)
   D(AllocateMemory); D(FreeMemory); D(DestroyDevice);
   pAllocateMemory = AllocateMemory;
   pFreeMemory = FreeMemory;
   pDestroyDevice = DestroyDevice;
   pDestroyInstance = DestroyInstance;
   return 0;
}

static void
close_device(void)
{
   pDestroyDevice(dev, NULL);
   pDestroyInstance(inst, NULL);
}

static VkResult
alloc(uint32_t type, VkDeviceSize size, VkDeviceMemory *out)
{
   *out = VK_NULL_HANDLE;
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = size, .memoryTypeIndex = type};
   return pAllocateMemory(dev, &mai, NULL, out);
}

/* MALI_VRAM_CAP_MB=8: 4 MiB fits, 8 MiB more does not, a refusal reserves nothing, freeing returns the bytes. */
static void
test_cap_8mib(void)
{
   VkDeviceMemory small = VK_NULL_HANDLE, big = VK_NULL_HANDLE, second = VK_NULL_HANDLE;

   CHECK(alloc(mt_uncached, 4 * MIB, &small) == VK_SUCCESS, "4 MiB within an 8 MiB cap");

   VkResult r = alloc(mt_uncached, 8 * MIB, &big);
   CHECK(r == VK_ERROR_OUT_OF_DEVICE_MEMORY, "8 MiB over budget: got %d", (int)r);
   CHECK(big == VK_NULL_HANDLE, "refused allocation must not hand out a handle");

   /* The refusal reserved nothing: another 4 MiB still fits exactly. */
   CHECK(alloc(mt_uncached, 4 * MIB, &second) == VK_SUCCESS, "refusal must not consume budget");

   /* Anything above the cap is refused even on an empty budget. */
   CHECK(alloc(mt_uncached, 9 * MIB, &big) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "9 MiB alone is over the 8 MiB cap");
   CHECK(big == VK_NULL_HANDLE, "over-cap allocation must not hand out a handle");

   /* Freeing returns the bytes: the 8 MiB allocation now fits. */
   pFreeMemory(dev, small, NULL);
   pFreeMemory(dev, second, NULL);
   small = second = VK_NULL_HANDLE;
   CHECK(alloc(mt_uncached, 8 * MIB, &big) == VK_SUCCESS, "8 MiB after freeing 8 MiB");
   if (big != VK_NULL_HANDLE) pFreeMemory(dev, big, NULL);
}

/* The charge is the page-rounded size: 2048 one-byte allocations fill an 8 MiB cap exactly, the 2049th does not. */
static void
test_page_rounding(void)
{
   enum { N = 2049 };
   static VkDeviceMemory m[N];
   int ok = 0, refused_at = -1;

   for (int i = 0; i < N; i++) {
      VkResult r = alloc(mt_uncached, 1, &m[i]);
      if (r == VK_SUCCESS) { ok++; continue; }
      CHECK(r == VK_ERROR_OUT_OF_DEVICE_MEMORY, "2049 x 1 B: unexpected result %d at #%d", (int)r, i);
      refused_at = i;
      break;
   }
   CHECK(ok == 2048, "2049 x 1 B (4096 B committed each) against an 8 MiB cap: %d taken, want 2048", ok);
   CHECK(refused_at == 2048, "the 2049th one-byte allocation must be refused (refused at #%d)", refused_at);
   for (int i = 0; i < ok; i++)
      pFreeMemory(dev, m[i], NULL);

   /* All of it comes back. */
   VkDeviceMemory big = VK_NULL_HANDLE;
   CHECK(alloc(mt_uncached, 8 * MIB, &big) == VK_SUCCESS, "8 MiB fits again after the 2048 one-byte frees");
   if (big != VK_NULL_HANDLE) pFreeMemory(dev, big, NULL);
}

/* Both memory types are charged: 4 MiB uncached + 4 MiB cached fill the cap, a third allocation cannot pass. */
static void
test_mixed_types(void)
{
   if (mt_cached == UINT32_MAX) {
      printf("skip  mixed memory types: the driver advertises no HOST_CACHED type\n");
      return;
   }
   VkDeviceMemory a = VK_NULL_HANDLE, b = VK_NULL_HANDLE, c = VK_NULL_HANDLE;
   CHECK(alloc(mt_uncached, 4 * MIB, &a) == VK_SUCCESS, "4 MiB uncached");
   CHECK(alloc(mt_cached, 4 * MIB, &b) == VK_SUCCESS, "4 MiB cached alongside it");
   CHECK(alloc(mt_uncached, 1, &c) == VK_ERROR_OUT_OF_DEVICE_MEMORY,
         "cached and uncached are both charged: nothing more fits an 8 MiB cap");
   CHECK(c == VK_NULL_HANDLE, "refused mixed-type allocation must not hand out a handle");
   /* Freeing the cached one gives exactly its size back: 8 MiB cached now fits. */
   pFreeMemory(dev, b, NULL);
   CHECK(alloc(mt_cached, 8 * MIB, &b) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "8 MiB cached with 4 MiB uncached live");
   pFreeMemory(dev, a, NULL);
   a = VK_NULL_HANDLE;
   if (c != VK_NULL_HANDLE) pFreeMemory(dev, c, NULL);
   CHECK(alloc(mt_cached, 8 * MIB, &b) == VK_SUCCESS, "8 MiB cached after freeing the uncached 4 MiB");
   if (b != VK_NULL_HANDLE) pFreeMemory(dev, b, NULL);
}

/* MALI_VRAM_CAP_MB is only a positive whole number of MiB; anything else must keep the default cap, never 0. */
static void
test_bad_override(const char *value)
{
   setenv("MALI_VRAM_CAP_MB", value, 1);
   if (open_device()) {
      /* A setup failure is a test failure: returning silently here reported success (g_fail stayed 0). */
      g_fail = 1;
      return;
   }
   VkDeviceMemory m = VK_NULL_HANDLE;
   CHECK(alloc(mt_uncached, 64 * MIB, &m) == VK_SUCCESS,
         "MALI_VRAM_CAP_MB=%s: the default cap must stay (a zero cap refuses even 64 MiB)", value);
   if (m != VK_NULL_HANDLE) pFreeMemory(dev, m, NULL);
   close_device();
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
   if (!icd) { printf("usage: t_vk_vram --direct <icd.so>\n"); return 2; }

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { printf("no vk_icdGetInstanceProcAddr\n"); return 1; }

   setenv("MALI_VRAM_CAP_MB", "8", 1);   /* read by mali_physical_device.c */
   if (open_device())
      return 1;
   test_cap_8mib();
   test_page_rounding();
   test_mixed_types();
   close_device();

   /* Each is read when the physical device is created, so each gets its own instance. */
   test_bad_override("auto");
   test_bad_override("12abc");

   printf("t_vk_vram %s\n", g_fail ? "FAILED" : "ok: 8 MiB cap enforced on page-rounded sizes for both memory "
                                                     "types, refusals reserve nothing, frees release, "
                                                     "MALI_VRAM_CAP_MB=auto keeps the default cap");
   return g_fail;
}
