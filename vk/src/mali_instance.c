/* SPDX-License-Identifier: MIT */
#include "mali_private.h"

#include "mali_entrypoints.h"
#include "vk_alloc.h"
#include "vk_util.h"

static const struct vk_instance_extension_table mali_instance_extensions = {
   .KHR_get_physical_device_properties2 = true,
   .KHR_external_memory_capabilities = true,
   .KHR_external_semaphore_capabilities = true,
   .KHR_external_fence_capabilities = true,
   .EXT_debug_report = true,
   /* M14: surfaces come from wsi_common (Wayland, XCB, Xlib: whatever Mesa was configured with) */
   .KHR_surface = true,
   .KHR_get_surface_capabilities2 = true,
   .EXT_surface_maintenance1 = true,   /* companion of swapchain_maintenance1 (wsi_common) */
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   .KHR_wayland_surface = true,
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
   .KHR_xcb_surface = true,    /* X11 WSI: wsi_common_x11 (DRI3/Present with the shm fallback), see STEAM-GAPS.md */
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
   .KHR_xlib_surface = true,
#endif
   .EXT_debug_utils = true,
};

VKAPI_ATTR VkResult VKAPI_CALL
mali_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
   uint32_t version_override = vk_get_version_override();

   *pApiVersion = version_override ? version_override : MALI_INSTANCE_API_VERSION;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_EnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount,
                                          VkExtensionProperties *pProperties)
{
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(&mali_instance_extensions, pPropertyCount,
                                                     pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
   struct mali_instance *instance;
   VkResult result;

   pAllocator = pAllocator ? pAllocator : vk_default_allocator();
   instance = vk_zalloc(pAllocator, sizeof(*instance), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_instance_dispatch_table dispatch_table;
   vk_instance_dispatch_table_from_entrypoints(&dispatch_table, &mali_instance_entrypoints, true);
   vk_instance_dispatch_table_from_entrypoints(&dispatch_table, &wsi_instance_entrypoints, false);

   result = vk_instance_init(&instance->vk, &mali_instance_extensions, &dispatch_table,
                             pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, instance);
      return vk_error(NULL, result);
   }

   instance->vk.physical_devices.enumerate = mali_enumerate_physical_devices;
   instance->vk.physical_devices.destroy = mali_destroy_physical_device;

   *pInstance = mali_instance_to_handle(instance);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyInstance(VkInstance _instance, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(mali_instance, instance, _instance);

   if (!instance)
      return;

   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
mali_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
   VK_FROM_HANDLE(mali_instance, instance, _instance);

   return vk_instance_get_proc_addr(instance ? &instance->vk : NULL, &mali_instance_entrypoints,
                                    pName);
}

/* Loader entry point. vk_icdNegotiateLoaderICDInterfaceVersion and
 * vk_icdGetPhysicalDeviceProcAddr come from the common runtime (vk_instance.c). */
PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return mali_GetInstanceProcAddr(instance, pName);
}
