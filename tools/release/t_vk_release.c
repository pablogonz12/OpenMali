/* SPDX-License-Identifier: MIT
 * Release probe: report what an installed guest driver really exposes.
 *   t_vk_release --direct <icd.so>
 * Loader-free (dlopen the ICD, vk_icdGetInstanceProcAddr). One line per fact:
 *   driver=<name>|<info>          (driverInfo is everything after the first '|')
 *   device=<deviceName>
 *   instance_ext=<extension>
 *   device_ext=<extension>
 * Used by tools/release/verify-guest.sh; exit 0 when the probe ran.
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
   if (!icd) { printf("usage: t_vk_release --direct <icd.so>\n"); return 2; }

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { fprintf(stderr, "no vk_icdGetInstanceProcAddr\n"); return 1; }

   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   PFN_vkEnumerateInstanceExtensionProperties EIEP =
      (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
   if (!CreateInstance || !EIEP) { fprintf(stderr, "missing instance entry points\n"); return 1; }

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "mali-release-verify",
                            .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst = VK_NULL_HANDLE;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS || !inst) {
      fprintf(stderr, "vkCreateInstance failed\n");
      return 1;
   }

   uint32_t n = 0;
   EIEP(NULL, &n, NULL);
   VkExtensionProperties *iep = n ? calloc(n, sizeof(*iep)) : NULL;
   if (iep) EIEP(NULL, &n, iep);
   for (uint32_t i = 0; i < n; i++) printf("instance_ext=%s\n", iep[i].extensionName);
   free(iep);

   PFN_vkEnumeratePhysicalDevices EPD = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceProperties2 GPP2 =
      (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
   PFN_vkGetPhysicalDeviceProperties GPP = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
   PFN_vkEnumerateDeviceExtensionProperties EDEP =
      (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
   PFN_vkDestroyInstance DI = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");

   uint32_t npd = 0;
   if (EPD) EPD(inst, &npd, NULL);
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   if (EPD && npd) { npd = 1; EPD(inst, &npd, &pd); }

   if (pd != VK_NULL_HANDLE) {
      VkPhysicalDeviceProperties props;
      VkPhysicalDeviceDriverProperties drv;
      memset(&props, 0, sizeof props);
      memset(&drv, 0, sizeof drv);
      drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
      if (GPP2) {
         VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
         GPP2(pd, &p2);
         props = p2.properties;
      } else if (GPP) {
         GPP(pd, &props);
      }
      printf("driver=%s|%s\n", drv.driverName, drv.driverInfo);
      printf("device=%s\n", props.deviceName);

      if (EDEP) {
         uint32_t ne = 0;
         EDEP(pd, NULL, &ne, NULL);
         VkExtensionProperties *eps = ne ? calloc(ne, sizeof(*eps)) : NULL;
         if (eps) EDEP(pd, NULL, &ne, eps);
         for (uint32_t i = 0; i < ne; i++) printf("device_ext=%s\n", eps[i].extensionName);
         free(eps);
      }
   } else {
      fprintf(stderr, "no physical device\n");
   }

   if (DI) DI(inst, NULL);
   return 0;
}
