/* SPDX-License-Identifier: MIT */
/* M13 query pool Vulkan entrypoints (prototypes for -Wmissing-prototypes; the generated header only has the ones the runtime lacks). */
#ifndef MALI_QUERY_H
#define MALI_QUERY_H
#include <vulkan/vulkan_core.h>
VKAPI_ATTR VkResult VKAPI_CALL mali_CreateQueryPool(VkDevice, const VkQueryPoolCreateInfo *, const VkAllocationCallbacks *, VkQueryPool *);
VKAPI_ATTR void VKAPI_CALL mali_DestroyQueryPool(VkDevice, VkQueryPool, const VkAllocationCallbacks *);
VKAPI_ATTR void VKAPI_CALL mali_ResetQueryPool(VkDevice, VkQueryPool, uint32_t, uint32_t);
VKAPI_ATTR VkResult VKAPI_CALL mali_GetQueryPoolResults(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t, void *, VkDeviceSize, VkQueryResultFlags);
VKAPI_ATTR void VKAPI_CALL mali_CmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL mali_CmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags);
VKAPI_ATTR void VKAPI_CALL mali_CmdWriteTimestamp2(VkCommandBuffer, VkPipelineStageFlags2, VkQueryPool, uint32_t);
VKAPI_ATTR void VKAPI_CALL mali_CmdBeginQuery(VkCommandBuffer, VkQueryPool, uint32_t, VkQueryControlFlags);
VKAPI_ATTR void VKAPI_CALL mali_CmdEndQuery(VkCommandBuffer, VkQueryPool, uint32_t);
VKAPI_ATTR void VKAPI_CALL mali_CmdBeginQueryIndexedEXT(VkCommandBuffer, VkQueryPool, uint32_t, VkQueryControlFlags, uint32_t);
VKAPI_ATTR void VKAPI_CALL mali_CmdEndQueryIndexedEXT(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t);
#endif
