/* SPDX-License-Identifier: MIT */
/* VkDeviceMemory / instance edge cases on the fake backend (mali_memory.c, mali_device.c, mali_instance.c error paths).
 *   t_vk_mem --direct <icd.so>
 * Checks: a layer-name extension query is LAYER_NOT_PRESENT; map twice and map past the end fail with MEMORY_MAP_FAILED and the
 * failed second map does not unmap the first; map after unmap works and offset maps are offset from the base; the cached memory
 * type accepts flush/invalidate (explicit size, WHOLE_SIZE, unaligned range); an out-of-range memory type and an oversized
 * allocation are OUT_OF_DEVICE_MEMORY; the 4096-allocation cap returns TOO_MANY_OBJECTS and frees make room again;
 * vkGetDeviceMemoryCommitment is 0 (no lazily allocated type); the opaque capture address queries are 0 (no
 * bufferDeviceAddressCaptureReplay). Exit 0 = pass. */
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
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                       \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                               \
      }                                                                                            \
   } while (0)

#define MAXALLOC 4096 /* MALI_MAX_ALLOCS */

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
   if (!icd) { printf("usage: t_vk_mem --direct <icd.so>\n"); return 2; }
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { printf("no vk_icdGetInstanceProcAddr\n"); return 1; }

   PFN_vkEnumerateInstanceExtensionProperties EIEP =
      (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
   uint32_t cnt = 0;
   CHECK(EIEP("VK_LAYER_does_not_exist", &cnt, NULL) == VK_ERROR_LAYER_NOT_PRESENT, "layer query");
   CHECK(EIEP(NULL, &cnt, NULL) == VK_SUCCESS, "plain instance ext query");

   VkInstance inst;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("FAIL CreateInstance\n"); return 1; }
#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceMemoryProperties); I(CreateDevice); I(GetDeviceProcAddr); I(DestroyInstance);
   VkPhysicalDevice pd; uint32_t n = 1;
   if (EnumeratePhysicalDevices(inst, &n, &pd) != VK_SUCCESS || n != 1) { printf("FAIL enumerate\n"); return 1; }
   VkPhysicalDeviceMemoryProperties mp;
   GetPhysicalDeviceMemoryProperties(pd, &mp);
   int t_coh = -1, t_cached = -1;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
      if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && t_coh < 0) t_coh = i;
      if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) && t_cached < 0) t_cached = i;
   }
   CHECK(t_coh >= 0 && t_cached >= 0, "memory types coherent=%d cached=%d", t_coh, t_cached);
   if (t_coh < 0 || t_cached < 0) return 1;

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   VkDevice dev;
   if (CreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { printf("FAIL CreateDevice\n"); return 1; }
#define D(n) PFN_vk##n n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n)
   D(AllocateMemory); D(FreeMemory); D(MapMemory); D(UnmapMemory); D(FlushMappedMemoryRanges); D(InvalidateMappedMemoryRanges);
   D(GetDeviceMemoryCommitment); D(GetDeviceMemoryOpaqueCaptureAddress); D(DestroyDevice);

   /* ---- map state machine ---- */
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 65536, .memoryTypeIndex = t_coh};
   VkDeviceMemory m;
   CHECK(AllocateMemory(dev, &mai, NULL, &m) == VK_SUCCESS, "alloc coherent");
   uint8_t *p = NULL, *q = NULL;
   CHECK(MapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&p) == VK_SUCCESS && p, "map");
   CHECK(MapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&q) == VK_ERROR_MEMORY_MAP_FAILED, "second map must fail");
   p[0] = 0x5a; p[65535] = 0xa5;
   UnmapMemory(dev, m);
   CHECK(MapMemory(dev, m, 65536, VK_WHOLE_SIZE, 0, (void **)&q) == VK_ERROR_MEMORY_MAP_FAILED, "offset == size must fail");
   CHECK(MapMemory(dev, m, 4096, 4096, 0, (void **)&q) == VK_SUCCESS && q == p + 4096, "offset map is base+offset (%p vs %p)", (void *)q, (void *)p);
   UnmapMemory(dev, m);
   CHECK(MapMemory(dev, m, 0, 16, 0, (void **)&q) == VK_SUCCESS && q == p && q[0] == 0x5a && q[65535] == 0xa5,
         "map after unmap keeps contents");
   UnmapMemory(dev, m);
   CHECK(MapMemory(dev, VK_NULL_HANDLE, 0, 16, 0, (void **)&q) == VK_SUCCESS && q == NULL, "map of VK_NULL_HANDLE gives NULL");
   uint64_t committed = 123;
   GetDeviceMemoryCommitment(dev, m, &committed);
   CHECK(committed == 0, "commitment %llu", (unsigned long long)committed);
   VkDeviceMemoryOpaqueCaptureAddressInfo oci = {.sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_OPAQUE_CAPTURE_ADDRESS_INFO, .memory = m};
   CHECK(GetDeviceMemoryOpaqueCaptureAddress(dev, &oci) == 0, "opaque capture address");
   FreeMemory(dev, m, NULL);

   /* ---- cached type: flush/invalidate are legal and keep the data ---- */
   mai.memoryTypeIndex = t_cached;
   VkDeviceMemory c;
   CHECK(AllocateMemory(dev, &mai, NULL, &c) == VK_SUCCESS, "alloc cached");
   CHECK(MapMemory(dev, c, 0, VK_WHOLE_SIZE, 0, (void **)&p) == VK_SUCCESS, "map cached");
   memset(p, 0x77, 65536);
   VkMappedMemoryRange r[3] = {
      {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = c, .offset = 0, .size = 128},
      {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = c, .offset = 64, .size = VK_WHOLE_SIZE},
      {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = c, .offset = 65536 - 64, .size = 64},
   };
   CHECK(FlushMappedMemoryRanges(dev, 3, r) == VK_SUCCESS, "flush");
   CHECK(InvalidateMappedMemoryRanges(dev, 3, r) == VK_SUCCESS, "invalidate");
   CHECK(p[0] == 0x77 && p[65535] == 0x77, "data after flush/invalidate");
   UnmapMemory(dev, c);
   FreeMemory(dev, c, NULL);

   /* ---- allocation limits ---- */
   VkDeviceMemory bad = VK_NULL_HANDLE;
   mai.memoryTypeIndex = 2; /* MALI_MEM_TYPE_COUNT */
   CHECK(AllocateMemory(dev, &mai, NULL, &bad) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "memory type out of range");
   mai.memoryTypeIndex = t_coh; mai.allocationSize = mp.memoryHeaps[0].size + 1;
   CHECK(AllocateMemory(dev, &mai, NULL, &bad) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "alloc larger than the heap");

   mai.allocationSize = 64;
   VkDeviceMemory *all = calloc(MAXALLOC + 1, sizeof *all);
   int got = 0;
   for (; got < MAXALLOC + 1; got++) {
      VkResult res = AllocateMemory(dev, &mai, NULL, &all[got]);
      if (res != VK_SUCCESS) { CHECK(res == VK_ERROR_TOO_MANY_OBJECTS, "cap result %d", res); break; }
   }
   CHECK(got == MAXALLOC, "allocation cap at %d, want %d", got, MAXALLOC);
   if (got > 0) {
      FreeMemory(dev, all[--got], NULL);
      VkDeviceMemory again;
      CHECK(AllocateMemory(dev, &mai, NULL, &again) == VK_SUCCESS, "a free makes room under the cap");
      if (again) all[got++] = again;
   }
   for (int i = 0; i < got; i++) FreeMemory(dev, all[i], NULL);
   free(all);

   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   printf(g_fail ? "FAILED\n" : "OK\n");
   return g_fail;
}
