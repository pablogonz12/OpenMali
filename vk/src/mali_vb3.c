/* SPDX-License-Identifier: MIT */
/* vkCmdBindVertexBuffers2 (core 1.3 / EXT_extended_dynamic_state) lands on vkCmdBindVertexBuffers3KHR in this runtime
 * (vk_common_CmdBindVertexBuffers2EXT builds VkBindVertexBuffer3InfoKHR and calls it); without this entry point a 1.3 client such as zink
 * (which always uses the 2-form) jumped through NULL on its first draw. The binding itself is the same as mali_CmdBindVertexBuffers: address +
 * size per slot.
 * NOT handled: pBindingInfos[].setStride (dynamic VERTEX_INPUT_BINDING_STRIDE). The strides still come from the pipeline's attribute
 * descriptors (mali_pipeline.c / mali_cs_emit.c, other worker's files): patches/zink-dynamic-vertex-stride.NOTE.md. */
#include "mali_cmd.h"
#include "mali_entrypoints.h"

VKAPI_ATTR void VKAPI_CALL
mali_CmdBindVertexBuffers3KHR(VkCommandBuffer h, uint32_t first, uint32_t n, const VkBindVertexBuffer3InfoKHR *info)
{
   struct mali_cmd_buffer *cmd = mali_cmd_buffer_from_vk(vk_command_buffer_from_handle(h));
   for (uint32_t i = 0; i < n; i++) {
      unsigned b = first + i;
      if (b >= MALI_MAX_VB) return;   /* same limit as mali_CmdBindVertexBuffers; its "unsupported" marking is local to mali_cmd.c */
      uint64_t sz = info[i].addressRange.size;
      cmd->vb_va[b] = info[i].addressRange.address;
      cmd->vb_size[b] = info[i].addressRange.address ? (uint32_t)(sz > 0xffffffffu ? 0xffffffffu : sz) : 0;
   }
}
