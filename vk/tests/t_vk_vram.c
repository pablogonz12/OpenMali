/* SPDX-License-Identifier: MIT */
/* Device-wide commit budget (mali_memory.c mem_reserve/mem_release, mali_private.h mem_cap).
 *   t_vk_vram --direct <icd.so>
 * Sets MALI_VRAM_CAP_MB=8 in this process before the physical device is created, so the cap under test is the
 * one the driver reads. kbase pins every byte it is given (libkbase/src/bo.c:45), therefore the budget has to be
 * enforced before anything is allocated:
 *   - an allocation that fits the cap succeeds, one that would exceed it is refused with
 *     VK_ERROR_OUT_OF_DEVICE_MEMORY (no abort, no partial allocation),
 *   - the refusal does not consume budget: the next allocation that still fits succeeds,
 *   - freeing returns the bytes to the budget, so the same large allocation succeeds afterwards.
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

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
   if (!icd) { printf("usage: t_vk_vram --direct <icd.so>\n"); return 2; }

   setenv("MALI_VRAM_CAP_MB", "8", 1);   /* read by mali_physical_device.c */

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { printf("no vk_icdGetInstanceProcAddr\n"); return 1; }

   VkInstance inst;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("FAIL CreateInstance\n"); return 1; }

#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceMemoryProperties); I(CreateDevice); I(GetDeviceProcAddr);
   I(DestroyInstance);
   VkPhysicalDevice pd; uint32_t n = 1;
   if (EnumeratePhysicalDevices(inst, &n, &pd) != VK_SUCCESS || n != 1) { printf("FAIL enumerate\n"); return 1; }

   VkPhysicalDeviceMemoryProperties mp;
   GetPhysicalDeviceMemoryProperties(pd, &mp);
   int t = -1;
   for (uint32_t i = 0; i < mp.memoryTypeCount && t < 0; i++)
      if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) t = (int)i;
   CHECK(t >= 0, "no host-visible memory type");
   if (t < 0) return 1;

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   VkDevice dev;
   if (CreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { printf("FAIL CreateDevice\n"); return 1; }
#define D(n) PFN_vk##n n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n)
   D(AllocateMemory); D(FreeMemory); D(DestroyDevice);

   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .memoryTypeIndex = (uint32_t)t};
   VkDeviceMemory small = VK_NULL_HANDLE, big = VK_NULL_HANDLE;

   /* 4 MiB of an 8 MiB cap. */
   mai.allocationSize = 4 * MIB;
   CHECK(AllocateMemory(dev, &mai, NULL, &small) == VK_SUCCESS, "4 MiB within an 8 MiB cap");

   /* 8 MiB would make 12: refused, cleanly. */
   mai.allocationSize = 8 * MIB;
   VkResult r = AllocateMemory(dev, &mai, NULL, &big);
   CHECK(r == VK_ERROR_OUT_OF_DEVICE_MEMORY, "8 MiB over budget: got %d", (int)r);
   CHECK(big == VK_NULL_HANDLE, "refused allocation must not hand out a handle");

   /* The refusal reserved nothing: another 4 MiB still fits exactly. */
   mai.allocationSize = 4 * MIB;
   VkDeviceMemory second = VK_NULL_HANDLE;
   CHECK(AllocateMemory(dev, &mai, NULL, &second) == VK_SUCCESS, "refusal must not consume budget");

   /* Anything above the cap is refused even on an empty budget. */
   CHECK(AllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                     .allocationSize = 9 * MIB, .memoryTypeIndex = (uint32_t)t},
                        NULL, &big) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "9 MiB alone is over the 8 MiB cap");
   CHECK(big == VK_NULL_HANDLE, "over-cap allocation must not hand out a handle");

   /* Freeing returns the bytes: the 8 MiB allocation now fits. */
   FreeMemory(dev, small, NULL);
   FreeMemory(dev, second, NULL);
   mai.allocationSize = 8 * MIB;
   CHECK(AllocateMemory(dev, &mai, NULL, &big) == VK_SUCCESS, "8 MiB after freeing 8 MiB");
   if (big != VK_NULL_HANDLE) FreeMemory(dev, big, NULL);

   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   printf("t_vk_vram %s\n", g_fail ? "FAILED" : "ok: 8 MiB cap enforced, refusals reserve nothing, frees release");
   return g_fail;
}
