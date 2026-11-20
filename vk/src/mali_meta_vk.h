/* SPDX-License-Identifier: MIT */
/* M12 Vulkan entrypoints of mali_meta_vk.c (prototypes; the generated entrypoint header has no mali_ prototypes for these). */
#ifndef MALI_META_VK_H
#define MALI_META_VK_H
#include <vulkan/vulkan_core.h>
struct mali_cmd_buffer;
struct mali_meta_list;
void mali_meta_vk_emit(struct mali_cmd_buffer *cmd, struct mali_meta_list *l, int plan_err);
/* MALI_SOFT (desktop harness): run one materialised meta dispatch on the host through mali_meta_ref_run, so host tests check the
 * pixels a blit/copy/clear really wrote. False when the dispatch is not a built-in meta job (BC decode) or a VA is unmapped. */
bool mali_meta_vk_host_run(const struct mali_dispatch *d, void *(*va_cpu)(void *u, uint64_t va, size_t size), void *u);
#include "mali_meta.h"
struct mali_draw;
int mali_meta_vk_preload(struct mali_cmd_buffer *cmd, enum mali_preload_kind kind, const struct mali_preload_src *src, struct mali_draw *d);
void mali_meta_cmd_forget(const void *cmd);
VKAPI_ATTR void VKAPI_CALL mali_CmdCopyBuffer2(VkCommandBuffer, const VkCopyBufferInfo2 *);
VKAPI_ATTR void VKAPI_CALL mali_CmdCopyImage2(VkCommandBuffer, const VkCopyImageInfo2 *);
VKAPI_ATTR void VKAPI_CALL mali_CmdCopyBufferToImage2(VkCommandBuffer, const VkCopyBufferToImageInfo2 *);
VKAPI_ATTR void VKAPI_CALL mali_CmdCopyImageToBuffer2(VkCommandBuffer, const VkCopyImageToBufferInfo2 *);
VKAPI_ATTR void VKAPI_CALL mali_CmdFillBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t);
VKAPI_ATTR void VKAPI_CALL mali_CmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void *);
VKAPI_ATTR void VKAPI_CALL mali_CmdBlitImage2(VkCommandBuffer, const VkBlitImageInfo2 *);
VKAPI_ATTR void VKAPI_CALL mali_CmdResolveImage2(VkCommandBuffer, const VkResolveImageInfo2 *);
VKAPI_ATTR void VKAPI_CALL mali_meta_CmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t, const VkImageSubresourceRange *);
VKAPI_ATTR void VKAPI_CALL mali_meta_CmdClearDepthStencilImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearDepthStencilValue *, uint32_t, const VkImageSubresourceRange *);
#endif
