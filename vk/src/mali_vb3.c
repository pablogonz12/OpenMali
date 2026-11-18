/* SPDX-License-Identifier: MIT */
/* vkCmdBindVertexBuffers2 (core 1.3 / EXT_extended_dynamic_state) lands on vkCmdBindVertexBuffers3KHR in this runtime
 * (vk_common_CmdBindVertexBuffers2EXT builds VkBindVertexBuffer3InfoKHR and calls it); without this entry point a 1.3 client such as zink
 * (which always uses the 2-form) jumped through NULL on its first draw. The binding itself is the same as mali_CmdBindVertexBuffers: address +
 * size per slot.
 * setStride: the stride is stored per binding (cmd->vb_stride) and replaces the pipeline's stride when the pipeline has
 * VERTEX_INPUT_BINDING_STRIDE dynamic (gfx.dyn_stride; mali_cs_emit.c). */
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
      uint64_t nva = info[i].addressRange.address;
      uint32_t nst = info[i].setStride ? (uint32_t)info[i].addressRange.stride : 0;   /* dynamic VERTEX_INPUT_BINDING_STRIDE */
      uint32_t nsz = nva ? (uint32_t)(sz > 0xffffffffu ? 0xffffffffu : sz) : 0;
      if (cmd->vb_va[b] != nva || cmd->vb_size[b] != nsz || cmd->vb_stride[b] != nst) {
         cmd->vb_va[b] = nva; cmd->vb_size[b] = nsz; cmd->vb_stride[b] = nst;
         cmd->dirty |= MALI_D_BIND;   /* the stride is part of the BIND block (shared driver set) */
      }
   }
}
