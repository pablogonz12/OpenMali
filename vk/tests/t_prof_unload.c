/* SPDX-License-Identifier: MIT */
/* The MALI_PROF sink must not leave anything behind that can run after the device is destroyed or the ICD is
 * unloaded. A Steam client probes Vulkan at start: it creates a device, destroys it and dlcloses the driver, and a
 * detached ticker thread of ours kept running inside the unloaded library and segfaulted the client
 * (cmf-1127-233734: "steam exited rc=139", one startup line, no counter line).
 *
 *   t_prof_unload --direct <icd.so> [--rounds N]
 *
 * Creates and destroys a device N times (2), dlcloses the ICD, then idles for 2 s. Any leftover thread would touch
 * unmapped code or freed state there. Passes only if every round got a device and the process is alive after the
 * idle: no driver entry point exists past the dlclose, so the sleeping phase is exactly the bug's window. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FAIL(...)                                                                                  \
   do {                                                                                            \
      printf("{\"test\":\"t_prof_unload\",\"pass\":false,\"error\":\"");                           \
      printf(__VA_ARGS__);                                                                         \
      printf("\"}\n");                                                                             \
      return 1;                                                                                    \
   } while (0)

#define STEP(what)                                                                                 \
   do {                                                                                            \
      fprintf(stderr, "probe_round: %s failed\n", what);                                           \
      return -1;                                                                                   \
   } while (0)

static int
probe_round(void *lib)
{
   /* What a loader does first; the ICD is not usable before it. */
   VkResult (*negotiate)(uint32_t *) =
      (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t icd_version = 5;
   if (!negotiate || negotiate(&icd_version) != VK_SUCCESS)
      STEP("vk_icdNegotiateLoaderICDInterfaceVersion");
   PFN_vkGetInstanceProcAddr gipa =
      (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa)
      STEP("vk_icdGetInstanceProcAddr");

#define GLOBAL(name) PFN_vk##name name = (PFN_vk##name)gipa(NULL, "vk" #name)
#define INST(name)   PFN_vk##name name = (PFN_vk##name)gipa(instance, "vk" #name)
   GLOBAL(EnumerateInstanceVersion);   /* the ICD wants this asked before the rest of the globals */
   GLOBAL(EnumerateInstanceExtensionProperties);
   GLOBAL(CreateInstance);
   {  /* asked in the loader's order, and only for its side effect on the ICD's global table */
      uint32_t v = 0;
      if (EnumerateInstanceVersion)
         (void)EnumerateInstanceVersion(&v);
      if (EnumerateInstanceExtensionProperties)
         (void)EnumerateInstanceExtensionProperties(NULL, &v, NULL);
   }
   if (!CreateInstance)
      STEP("global entry points");

   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                  .apiVersion = VK_API_VERSION_1_0};
   const VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                     .pApplicationInfo = &app};
   VkInstance instance = VK_NULL_HANDLE;
   if (CreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
      STEP("vkCreateInstance");

   /* Instance level, like the loader asks them: vk_icdGetInstanceProcAddr(NULL, ...) does not answer for these. */
   INST(EnumeratePhysicalDevices);
   INST(DestroyInstance);
   if (!EnumeratePhysicalDevices || !DestroyInstance)
      STEP("instance entry points");

   uint32_t n = 0;
   if (EnumeratePhysicalDevices(instance, &n, NULL) != VK_SUCCESS || n == 0) {
      DestroyInstance(instance, NULL);
      STEP("vkEnumeratePhysicalDevices");
   }
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   if (EnumeratePhysicalDevices(instance, &n, &pd) != VK_SUCCESS) {
      DestroyInstance(instance, NULL);
      STEP("vkEnumeratePhysicalDevices (count 1)");
   }

   INST(CreateDevice);
   INST(GetDeviceProcAddr);
   if (!CreateDevice || !GetDeviceProcAddr) {
      DestroyInstance(instance, NULL);
      STEP("device entry points");
   }
   static const float prio = 1.0f;
   const VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                        .queueFamilyIndex = 0, .queueCount = 1,
                                        .pQueuePriorities = &prio};
   const VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                   .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   VkDevice device = VK_NULL_HANDLE;
   VkResult cr = CreateDevice(pd, &dci, NULL, &device);
   if (cr != VK_SUCCESS || device == VK_NULL_HANDLE) {
      fprintf(stderr, "probe_round: vkCreateDevice = %d (device %p)\n", (int)cr, (void *)device);
      DestroyInstance(instance, NULL);
      return -1;
   }
   int ok = 0;
   PFN_vkDestroyDevice DestroyDevice =
      (PFN_vkDestroyDevice)GetDeviceProcAddr(device, "vkDestroyDevice");
   if (DestroyDevice) {
      DestroyDevice(device, NULL);
      ok = 1;
   } else {
      fprintf(stderr, "probe_round: no vkDestroyDevice\n");
   }
   DestroyInstance(instance, NULL);
   return ok ? 0 : -1;
#undef INST
#undef GLOBAL
}

int
main(int argc, char **argv)
{
   const char *direct = NULL;
   int rounds = 2;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc)
         direct = argv[++i];
      else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)
         rounds = atoi(argv[++i]);
      else {
         fprintf(stderr, "usage: t_prof_unload --direct icd.so [--rounds N]\n");
         return 2;
      }
   }
   if (!direct)
      FAIL("--direct required");

   for (int i = 0; i < rounds; i++) {
      void *lib = dlopen(direct, RTLD_NOW | RTLD_LOCAL);
      if (!lib)
         FAIL("dlopen: %s", dlerror());
      if (probe_round(lib) != 0)
         FAIL("round %d did not get a device", i);
      /* The client's own pattern: the driver goes away while the process lives on. */
      if (dlclose(lib) != 0)
         FAIL("dlclose: %s", dlerror());
   }

   /* Past the dlclose there is no driver code left to run; anything still running is a bug. */
   struct timespec idle = {.tv_sec = 2, .tv_nsec = 0};
   nanosleep(&idle, NULL);

   printf("{\"test\":\"t_prof_unload\",\"pass\":true,\"rounds\":%d,\"idle_s\":2}\n", rounds);
   return 0;
}
