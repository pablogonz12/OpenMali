/* SPDX-License-Identifier: MIT */
/* M13 event / barrier entrypoints of mali_event_vk.c (prototypes for -Wmissing-prototypes). */
#ifndef MALI_EVENT_VK_H
#define MALI_EVENT_VK_H
#include <vulkan/vulkan_core.h>
VKAPI_ATTR VkResult VKAPI_CALL mali_CreateEvent(VkDevice, const VkEventCreateInfo *, const VkAllocationCallbacks *, VkEvent *);
VKAPI_ATTR void VKAPI_CALL mali_DestroyEvent(VkDevice, VkEvent, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL mali_GetEventStatus(VkDevice, VkEvent);
VKAPI_ATTR VkResult VKAPI_CALL mali_SetEvent(VkDevice, VkEvent);
VKAPI_ATTR VkResult VKAPI_CALL mali_ResetEvent(VkDevice, VkEvent);
VKAPI_ATTR void VKAPI_CALL mali_CmdSetEvent2(VkCommandBuffer, VkEvent, const VkDependencyInfo *);
VKAPI_ATTR void VKAPI_CALL mali_CmdResetEvent2(VkCommandBuffer, VkEvent, VkPipelineStageFlags2);
VKAPI_ATTR void VKAPI_CALL mali_CmdWaitEvents2(VkCommandBuffer, uint32_t, const VkEvent *, const VkDependencyInfo *);
#endif
