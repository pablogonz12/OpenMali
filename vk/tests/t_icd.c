/* SPDX-License-Identifier: MIT */
/* M1 test: instance -> physical device -> device -> queue, JSON lines on stdout.
 *
 *   t_icd                      through the system Vulkan loader (set VK_DRIVER_FILES to our manifest)
 *   t_icd --direct <icd.so>    no loader: dlopen the ICD and use vk_icdGetInstanceProcAddr
 *                              (this is how the driver is driven from `adb shell` on bionic)
 *   --expect-none              pass only if zero devices are enumerated
 *   --expect-name <substr>     pass only if a device name contains <substr>
 *
 * Needs only the Vulkan headers and libdl. Exit 0 = pass.
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAIL(...)                                                                                  \
   do {                                                                                            \
      printf("{\"test\":\"t_icd\",\"pass\":false,\"error\":\"");                                   \
      printf(__VA_ARGS__);                                                                         \
      printf("\"}\n");                                                                             \
      return 1;                                                                                    \
   } while (0)

/* Number of open fds of this process (any kind). The kbase fd is a plain open(/dev/mali0), but under
 * proot readlink does not always show that path, so the leak check compares total counts too. */
static int
count_all_fds(void)
{
   int n = 0;
   DIR *d = opendir("/proc/self/fd");

   if (!d)
      return -1;
   while (readdir(d))
      n++;
   closedir(d);
   return n;
}

/* Number of open fds of this process that point at a kbase node (/dev/mali*). */
static int
count_mali_fds(void)
{
   int n = 0;
   DIR *d = opendir("/proc/self/fd");
   struct dirent *e;
   char path[320], tgt[256];

   if (!d)
      return -1;
   while ((e = readdir(d))) {
      snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
      ssize_t l = readlink(path, tgt, sizeof(tgt) - 1);
      if (l > 0) {
         tgt[l] = 0;
         if (strstr(tgt, "/dev/mali"))
            n++;
      }
   }
   closedir(d);
   return n;
}

int
main(int argc, char **argv)
{
   const char *direct = NULL, *expect_name = NULL;
   int expect_none = 0;

   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc)
         direct = argv[++i];
      else if (!strcmp(argv[i], "--expect-name") && i + 1 < argc)
         expect_name = argv[++i];
      else if (!strcmp(argv[i], "--expect-none"))
         expect_none = 1;
      else {
         fprintf(stderr, "usage: t_icd [--direct icd.so] [--expect-none] [--expect-name s]\n");
         return 2;
      }
   }

   int all_start = count_all_fds();
   void *lib = dlopen(direct ? direct : "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!lib)
      FAIL("dlopen: %s", dlerror());

   PFN_vkGetInstanceProcAddr gipa =
      (PFN_vkGetInstanceProcAddr)dlsym(lib, direct ? "vk_icdGetInstanceProcAddr" : "vkGetInstanceProcAddr");
   if (!gipa)
      FAIL("no GetInstanceProcAddr symbol");

   if (direct) {
      /* What a loader does first. */
      VkResult (*negotiate)(uint32_t *) =
         (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
      uint32_t icd_version = 5;
      if (!negotiate || negotiate(&icd_version) != VK_SUCCESS)
         FAIL("vk_icdNegotiateLoaderICDInterfaceVersion failed");
      printf("{\"icd_interface_version\":%u}\n", icd_version);
   }

#define GLOBAL(name) PFN_vk##name name = (PFN_vk##name)gipa(NULL, "vk" #name)
#define INST(name)   PFN_vk##name name = (PFN_vk##name)gipa(instance, "vk" #name)

   GLOBAL(EnumerateInstanceVersion);
   GLOBAL(EnumerateInstanceExtensionProperties);
   GLOBAL(CreateInstance);
   if (!CreateInstance || !EnumerateInstanceExtensionProperties)
      FAIL("global entry points missing");

   uint32_t inst_version = VK_API_VERSION_1_0;
   if (EnumerateInstanceVersion && EnumerateInstanceVersion(&inst_version) != VK_SUCCESS)
      FAIL("vkEnumerateInstanceVersion failed");

   uint32_t ext_count = 0;
   if (EnumerateInstanceExtensionProperties(NULL, &ext_count, NULL) != VK_SUCCESS)
      FAIL("vkEnumerateInstanceExtensionProperties failed");

   printf("{\"mode\":\"%s\",\"instance_version\":\"%u.%u.%u\",\"instance_extensions\":%u}\n",
          direct ? "direct" : "loader", VK_API_VERSION_MAJOR(inst_version),
          VK_API_VERSION_MINOR(inst_version), VK_API_VERSION_PATCH(inst_version), ext_count);

   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "t_icd",
      .apiVersion = VK_API_VERSION_1_0,
   };
   const VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance = VK_NULL_HANDLE;
   VkResult r = CreateInstance(&ici, NULL, &instance);
   if (r != VK_SUCCESS)
      FAIL("vkCreateInstance = %d", r);

   INST(DestroyInstance);
   INST(EnumeratePhysicalDevices);
   INST(GetPhysicalDeviceProperties);
   INST(GetPhysicalDeviceFeatures);
   INST(GetPhysicalDeviceQueueFamilyProperties);
   INST(GetPhysicalDeviceMemoryProperties);
   INST(GetPhysicalDeviceFormatProperties);
   INST(EnumerateDeviceExtensionProperties);
   INST(CreateDevice);
   INST(GetDeviceProcAddr);
   if (!DestroyInstance || !EnumeratePhysicalDevices || !GetPhysicalDeviceProperties ||
       !GetPhysicalDeviceFeatures || !GetPhysicalDeviceQueueFamilyProperties ||
       !GetPhysicalDeviceMemoryProperties || !GetPhysicalDeviceFormatProperties ||
       !EnumerateDeviceExtensionProperties || !CreateDevice || !GetDeviceProcAddr)
      FAIL("instance entry points missing");

   VkPhysicalDevice pdevs[8];
   uint32_t pdev_count = 8;
   r = EnumeratePhysicalDevices(instance, &pdev_count, pdevs);
   /* With a loader and no device at all, the loader itself reports an error. */
   if (r != VK_SUCCESS && r != VK_INCOMPLETE)
      pdev_count = 0;
   printf("{\"enumerate_result\":%d,\"physical_devices\":%u}\n", r, pdev_count);

   int name_ok = expect_name == NULL, devices_ok = 0;

   for (uint32_t i = 0; i < pdev_count; i++) {
      VkPhysicalDeviceProperties props;
      VkPhysicalDeviceFeatures feats;
      VkPhysicalDeviceMemoryProperties mem;
      VkFormatProperties fmt;
      VkQueueFamilyProperties qf[4];
      uint32_t qf_count = 4, dev_ext_count = 0;

      GetPhysicalDeviceProperties(pdevs[i], &props);
      GetPhysicalDeviceFeatures(pdevs[i], &feats);
      GetPhysicalDeviceMemoryProperties(pdevs[i], &mem);
      GetPhysicalDeviceQueueFamilyProperties(pdevs[i], &qf_count, qf);
      GetPhysicalDeviceFormatProperties(pdevs[i], VK_FORMAT_R8G8B8A8_UNORM, &fmt);
      if (EnumerateDeviceExtensionProperties(pdevs[i], NULL, &dev_ext_count, NULL) != VK_SUCCESS)
         FAIL("vkEnumerateDeviceExtensionProperties failed");

      printf("{\"device\":%u,\"name\":\"%s\",\"api\":\"%u.%u.%u\",\"vendor\":\"0x%04x\","
             "\"device_id\":\"0x%04x\",\"type\":%d,\"queue_families\":%u,\"queue_flags\":%u,"
             "\"mem_types\":%u,\"heap0_mib\":%llu,\"rgba8_optimal\":\"0x%x\",\"device_extensions\":%u,"
             "\"max_image_2d\":%u}\n",
             i, props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
             VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
             props.vendorID, props.deviceID, props.deviceType, qf_count,
             qf_count ? qf[0].queueFlags : 0, mem.memoryTypeCount,
             (unsigned long long)(mem.memoryHeaps[0].size >> 20), fmt.optimalTilingFeatures,
             dev_ext_count, props.limits.maxImageDimension2D);

      if (expect_name && strstr(props.deviceName, expect_name))
         name_ok = 1;
      else if (expect_name)
         continue; /* some other driver's device (loader mode): do not create a device on it */

      if (qf_count < 1 || !(qf[0].queueFlags & VK_QUEUE_GRAPHICS_BIT) || mem.memoryTypeCount < 1)
         FAIL("device %u: no graphics queue family or no memory type", i);

      const float prio = 1.0f;
      const VkDeviceQueueCreateInfo qci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = 0,
         .queueCount = 1,
         .pQueuePriorities = &prio,
      };
      const VkDeviceCreateInfo dci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos = &qci,
      };
      VkDevice device = VK_NULL_HANDLE;
      r = CreateDevice(pdevs[i], &dci, NULL, &device);
      if (r != VK_SUCCESS)
         FAIL("vkCreateDevice = %d", r);

      PFN_vkGetDeviceQueue GetDeviceQueue =
         (PFN_vkGetDeviceQueue)GetDeviceProcAddr(device, "vkGetDeviceQueue");
      PFN_vkDestroyDevice DestroyDevice =
         (PFN_vkDestroyDevice)GetDeviceProcAddr(device, "vkDestroyDevice");
      if (!GetDeviceQueue || !DestroyDevice)
         FAIL("device entry points missing");

      VkQueue queue = VK_NULL_HANDLE;
      GetDeviceQueue(device, 0, 0, &queue);
      printf("{\"device\":%u,\"create_device\":true,\"queue\":%s,\"mali_fds_live\":%d}\n", i,
             queue ? "true" : "false", count_mali_fds());
      if (!queue)
         FAIL("vkGetDeviceQueue returned NULL");

      DestroyDevice(device, NULL);
      devices_ok++;
   }

   int fds_after_device = count_mali_fds();
   int all_after_device = count_all_fds();
   DestroyInstance(instance, NULL);
   int fds_end = count_mali_fds();
   int all_end = count_all_fds();

   int pass = expect_none ? pdev_count == 0 : (devices_ok > 0 && name_ok);
   if (fds_after_device != 0 || fds_end != 0 || all_after_device > all_start || all_end > all_start)
      pass = 0; /* leak: a kbase fd outlived vkDestroyDevice or vkDestroyInstance */
   printf("{\"test\":\"t_icd\",\"pass\":%s,\"devices_ok\":%d,\"mali_fds_after_destroy_device\":%d,"
          "\"mali_fds_end\":%d,\"fds_start\":%d,\"fds_after_destroy_device\":%d,\"fds_end\":%d}\n",
          pass ? "true" : "false", devices_ok, fds_after_device, fds_end, all_start, all_after_device, all_end);
   return pass ? 0 : 1;
}
