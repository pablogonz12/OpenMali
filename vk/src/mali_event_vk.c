/* SPDX-License-Identifier: MIT */
/* M13, Vulkan layer of events and barriers.
 *   Event       one u32 word in zeroed, CPU-mapped, uncached device memory (mali_meta device seam), so host and CS see the same word.
 *   CmdSetEvent2/ResetEvent2/WaitEvents2   ops frames (mali_event.c: SYNC_SET32 / SYNC_WAIT32), pushed with mali_cmd_push_ops
 *                                          (deferred to after the render pass frame when recorded inside one).
 *   CmdPipelineBarrier2 (mali_pipeline.c)  no-op: frames are serialised, each one flushes L2/LSC before it signals the retire counter and the
 *                                          queue waits for it, so every barrier is already satisfied (R3-PLAN.md M13). Layouts are no-ops.
 * Also: the IndexedEXT query entrypoints the runtime's vkCmdBeginQuery/EndQuery wrappers dispatch to (index 0 only). */
#include <stdatomic.h>

#include "mali_cmd.h"
#include "mali_event_vk.h"
#include "mali_meta.h"
#include "mali_query.h"
#include "mali_queue.h"
#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_object.h"

struct mali_event {
   struct vk_object_base base;
   volatile uint32_t *cpu;
   uint64_t va;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(mali_event, base, VkEvent, VK_OBJECT_TYPE_EVENT)

#define CMD(h) struct mali_cmd_buffer *cmd = mali_cmd_buffer_from_vk(vk_command_buffer_from_handle(h))

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateEvent(VkDevice _dev, const VkEventCreateInfo *ci, const VkAllocationCallbacks *alloc, VkEvent *out)
{
   struct vk_device *dev = vk_device_from_handle(_dev);
   struct mali_event *e = vk_object_zalloc(dev, alloc, sizeof *e, VK_OBJECT_TYPE_EVENT);
   if (!e)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   e->cpu = mali_hw_meta_alloc(container_of(dev, struct mali_device, vk), 64, 64, &e->va);
   if (!e->cpu) {
      vk_object_free(dev, alloc, e);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   __atomic_store_n(e->cpu, 0, __ATOMIC_RELEASE);   /* device memory is recycled, not zeroed */
   *out = mali_event_to_handle(e);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyEvent(VkDevice _dev, VkEvent h, const VkAllocationCallbacks *alloc)
{
   struct vk_device *dev = vk_device_from_handle(_dev);
   struct mali_event *e = mali_event_from_handle(h);
   if (!e)
      return;
   mali_hw_meta_free(container_of(dev, struct mali_device, vk), (void *)e->cpu);
   vk_object_free(dev, alloc, e);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_GetEventStatus(VkDevice _dev, VkEvent h)
{
   struct mali_event *e = mali_event_from_handle(h);
   return __atomic_load_n(e->cpu, __ATOMIC_ACQUIRE) ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_SetEvent(VkDevice _dev, VkEvent h)
{
   __atomic_store_n(mali_event_from_handle(h)->cpu, 1, __ATOMIC_RELEASE);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_ResetEvent(VkDevice _dev, VkEvent h)
{
   __atomic_store_n(mali_event_from_handle(h)->cpu, 0, __ATOMIC_RELEASE);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdSetEvent2(VkCommandBuffer h, VkEvent ev, const VkDependencyInfo *dep)
{
   CMD(h);
   struct mali_op op = { .kind = MALI_OP_AVAIL, .va = mali_event_from_handle(ev)->va, .val = 1 };
   mali_cmd_push_ops(cmd, &op, 1);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdResetEvent2(VkCommandBuffer h, VkEvent ev, VkPipelineStageFlags2 stage)
{
   CMD(h);
   struct mali_op op = { .kind = MALI_OP_AVAIL, .va = mali_event_from_handle(ev)->va, .val = 0 };
   mali_cmd_push_ops(cmd, &op, 1);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdWaitEvents2(VkCommandBuffer h, uint32_t n, const VkEvent *evs, const VkDependencyInfo *deps)
{
   CMD(h);
   for (uint32_t i = 0; i < n; i++) {
      struct mali_op op = { .kind = MALI_OP_EVENT_WAIT, .va = mali_event_from_handle(evs[i])->va };
      mali_cmd_push_ops(cmd, &op, 1);
   }
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdBeginQueryIndexedEXT(VkCommandBuffer h, VkQueryPool pool, uint32_t q, VkQueryControlFlags flags, uint32_t index)
{
   if (index) {
      CMD(h);
      mali_cmd_unsupported(cmd, "begin indexed query with nonzero index");
      return;
   }
   mali_CmdBeginQuery(h, pool, q, flags);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdEndQueryIndexedEXT(VkCommandBuffer h, VkQueryPool pool, uint32_t q, uint32_t index)
{
   if (index) {
      CMD(h);
      mali_cmd_unsupported(cmd, "end indexed query with nonzero index");
      return;
   }
   mali_CmdEndQuery(h, pool, q);
}
