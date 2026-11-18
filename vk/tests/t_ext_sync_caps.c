/* SPDX-License-Identifier: MIT */
/* External sync capability gate for the WSI explicit-sync work (docs: /vk/EXPLICIT-SYNC.md).
 *   t_ext_sync_caps --direct <icd.so>
 * Pins what Mesa wsi_common keys on (wsi_common.c wsi_device_init + wsi_device_supports_explicit_sync):
 *   - binary semaphores and fences are importable+exportable as SYNC_FD (implicit-sync present path: sync_file into dma-buf);
 *   - timeline semaphores are NOT exportable as OPAQUE_FD yet, so wsi_device_supports_explicit_sync() is false and the
 *     Wayland WSI never binds wp_linux_drm_syncobj_manager_v1. Whoever adds a DRM-syncobj-backed timeline type flips the
 *     EXPLICIT_SYNC_READY expectation below together with the implementation.
 * Exit 0 = pass. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#ifndef EXPLICIT_SYNC_READY
#define EXPLICIT_SYNC_READY 0
#endif
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d %s: ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

int
main(int argc, char **argv)
{
   if (argc < 3 || strcmp(argv[1], "--direct")) { printf("usage: t_ext_sync_caps --direct <icd.so>\n"); return 2; }
   void *lib = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst = NULL;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("FAIL instance\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices Enum = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceExternalSemaphoreProperties GS =
      (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)gipa(inst, "vkGetPhysicalDeviceExternalSemaphoreProperties");
   PFN_vkGetPhysicalDeviceExternalFenceProperties GF =
      (PFN_vkGetPhysicalDeviceExternalFenceProperties)gipa(inst, "vkGetPhysicalDeviceExternalFenceProperties");
   uint32_t n = 1;
   VkPhysicalDevice pd;
   CHECK(Enum(inst, &n, &pd) == VK_SUCCESS && n == 1, "enumerate");

   const VkExternalSemaphoreFeatureFlags both = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
   VkPhysicalDeviceExternalSemaphoreInfo si = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
                                               .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
   VkExternalSemaphoreProperties sp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
   GS(pd, &si, &sp);
   printf("binary SYNC_FD semaphore features=0x%x\n", sp.externalSemaphoreFeatures);
   CHECK((sp.externalSemaphoreFeatures & both) == both, "binary SYNC_FD semaphore must be import+export (implicit-sync present)");

   VkPhysicalDeviceExternalFenceInfo fi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
                                           .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
   VkExternalFenceProperties fp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
   GF(pd, &fi, &fp);
   printf("SYNC_FD fence features=0x%x\n", fp.externalFenceFeatures);
   CHECK((fp.externalFenceFeatures & (VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT)) ==
            (VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT),
         "SYNC_FD fence must be import+export");

   /* timeline + OPAQUE_FD: the wsi_device_supports_explicit_sync() key. */
   VkSemaphoreTypeCreateInfo tt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
   si.pNext = &tt;
   si.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
   memset(&sp, 0, sizeof sp);
   sp.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
   GS(pd, &si, &sp);
   int exp = !!(sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT);
   printf("timeline OPAQUE_FD exportable=%d (explicit sync %s)\n", exp, exp ? "ON" : "off");
   CHECK(exp == EXPLICIT_SYNC_READY, "timeline OPAQUE_FD export changed: update EXPLICIT_SYNC_READY with the implementation");

   printf(g_fail ? "t_ext_sync_caps: %d FAIL\n" : "t_ext_sync_caps: PASS\n", g_fail);
   return g_fail ? 1 : 0;
}
