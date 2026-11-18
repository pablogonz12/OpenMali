/* SPDX-License-Identifier: MIT */
/* VK_KHR_maintenance5 entry points that need no command-buffer internals.
 * The runtime's vk_common_CmdBindIndexBuffer2KHR forwards to CmdBindIndexBuffer3KHR (not implemented here), so the 2-form is wired straight to
 * mali_CmdBindIndexBuffer. The bound size is not tracked separately: ib_bytes stays "to the end of the buffer", which only matters for
 * out-of-range indices (undefined behaviour; robustBufferAccess is off). Module-less pipeline stages: mali_pipeline.c (stage_spirv). */
#include "mali_entrypoints.h"
#include "vk_command_buffer.h"

VKAPI_ATTR void VKAPI_CALL
mali_CmdBindIndexBuffer2KHR(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkIndexType indexType)
{
   (void)size;
   mali_CmdBindIndexBuffer(cb, buffer, offset, indexType);
}
