/* SPDX-License-Identifier: MIT */
/* M8: command buffers (Vulkan layer over mali_cs_emit.c). See mali_cmd.h. Unsupported things do not crash: they mark the command
 * buffer with VK_ERROR_FEATURE_NOT_PRESENT (vkEndCommandBuffer returns it) and are documented in R3-PLAN.md "M8":
 * (indexed draws: implemented in M9 rest, v11 INDEX_BUFFER r54 / SIZE r39 from genxml v11, index type PRIMITIVE_FLAGS[10:8])
 *   - colour loadOp LOAD, more than one colour attachment, depth-only render passes (the FBD needs a colour RT)
 *   - vkCmdClearAttachments that does not cover the whole render area before the first draw; vkCmdClear*Image except RGBA8 colour
 *   - dynamic descriptor offsets, non-triangle-list topologies (M6 resolver decides) */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "mali_cmd.h"
#include "kbase.h"
#include "mali_descriptor.h"
#include "mali_meta_vk.h"
#include "mali_image.h"
#include "mali_queue.h"
#include "vk_buffer.h"
#include "vk_enum_to_str.h"
#include "vk_pipeline_layout.h"
#include "vk_alloc.h"
#include "vk_graphics_state.h"
#include "util/bitset.h"
#include "util/u_math.h"
#include "util/format/u_format.h"
#include "vk_format.h"

static struct mali_cmd_res_ops g_res;
static struct mali_hw g_hw;
static bool g_hw_set;

void
mali_cmd_set_res(const struct mali_cmd_res_ops *ops)
{
   if (ops)
      g_res = *ops;
   else
      memset(&g_res, 0, sizeof g_res);
}

void
mali_cmd_set_hw(const struct mali_hw *hw)
{
   g_hw_set = hw != NULL;
   if (hw)
      g_hw = *hw;
}

#define CMD(h) struct mali_cmd_buffer *cmd = mali_cmd_buffer_from_vk(vk_command_buffer_from_handle(h))

void
mali_cmd_unsupported(struct mali_cmd_buffer *cmd, const char *what)
{
   /* Reasons may be temporary buffers. Recording is concurrent. */
   static char *seen[256];
   static unsigned count;
   static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
   bool first = true;
   pthread_mutex_lock(&mutex);
   for (unsigned i = 0; i < count; i++)
      if (!strcmp(seen[i], what)) {
         first = false;
         break;
      }
   if (first && count < sizeof(seen) / sizeof(seen[0])) {
      char *copy = strdup(what);
      if (copy)
         seen[count++] = copy;
      /* On allocation failure, log this occurrence and retry next time. */
   }
   pthread_mutex_unlock(&mutex);
   if (first)
      fprintf(stderr, "mali_cmd: unsupported %s\n", what);
   vk_command_buffer_set_error(&cmd->vk, VK_ERROR_FEATURE_NOT_PRESENT);
}

#define unsupported mali_cmd_unsupported

/* Legal-but-unimplemented state: render the closest legal thing, log ONE line, do not poison the command buffer.
 * PanVK's only vk_command_buffer_set_error is OOM (panvk_vX_cmd_desc_state.c:82); illegal state is UNREACHABLE and
 * unimplemented-but-legal state is emitted best-effort or the draw is skipped. One refused call used to make
 * vkEndCommandBuffer return VK_ERROR_FEATURE_NOT_PRESENT, which zink turns into GL_CONTEXT_LOST: Chromium then
 * reports "Disabling GPU acceleration: Disabled/RuntimeDetect" and Steam's CEF compositor loses the GPU for the
 * whole session (cef-gpu-cmf.md). Three classes (audit-refusals.md "Sticky-error policy"):
 *   - corrupting (wrong shader/pipeline, no index buffer, a clear we cannot express): unsupported(), sticky;
 *   - degradable (extra colour attachments, a clear of an attachment we do not render, MSAA->1x, exotic blend):
 *     degrade(), log once, continue with the closest legal behaviour;
 *   - app bugs (draw outside a pass, End inside one): unsupported(), sticky - the layers report them anyway. */
static void
degrade(struct mali_cmd_buffer *cmd, const char *what)
{
   /* Recording may run on several threads (separate command pools), so the lookup-and-insert is under one lock; the log line
    * is printed outside it (stderr is slow) but only by the thread that inserted the entry */
   static const char *seen[16];
   static unsigned nseen;
   static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
   bool first = false;
   pthread_mutex_lock(&m);
   {
      bool found = false;
      for (unsigned i = 0; i < nseen; i++)
         if (seen[i] == what || (seen[i] && !strcmp(seen[i], what)))
            found = true;
      if (!found && nseen < sizeof seen / sizeof seen[0]) { seen[nseen++] = what; first = true; }
   }
   pthread_mutex_unlock(&m);
   if (first && getenv("MALI_PIPE_DBG"))
      fprintf(stderr, "mali_cmd: degraded: %s (the frame continues)\n", what);
   (void)cmd;
}

/* M15: record a GPU write against this command buffer so the platform layer only CSYNCs BOs that changed
 * (vk/MEMORY.md). Over-subscribing is harmless; missing a write is stale data, so every write path calls this. */
void
mali_cmd_mark_written(struct mali_cmd_buffer *cmd, struct mali_memory *mem)
{
   if (!cmd || !mem)
      return;
   for (unsigned i = 0; i < cmd->nwrote; i++)
      if (cmd->wrote[i] == mem)
         return;
   if (cmd->nwrote < MALI_CMD_WROTE_MAX) {
      cmd->wrote[cmd->nwrote++] = mem;
   } else {
      cmd->wrote_full = true;   /* submit asks for one full sweep instead of guessing */
   }
}

/* Implicit sync, consumer side (mali_isync.h): an imported dma-buf this command buffer reads must have its
 * producer's fences waited before the submit. Only imported ones are recorded (a BO this driver allocated is in
 * order with its own submits), so the list stays short whatever the frame binds. */
void
mali_cmd_mark_read(struct mali_cmd_buffer *cmd, struct mali_memory *mem)
{
   if (!mem || mem->isync != MALI_ISYNC_BO_IMPORTED)
      return;
   for (unsigned i = 0; i < cmd->nread; i++)
      if (cmd->read[i] == mem)
         return;
   if (cmd->nread < MALI_CMD_READ_MAX)
      cmd->read[cmd->nread++] = mem;
   else
      cmd->read_full = true;
}

/* Implicit sync, producer side (mali_isync.h): the buffer this submit gives away. A reset or a freed memory must not
 * leave a stale mark behind, hence the guard at collect time (mali_queue.c) and the filter here. */
void
mali_cmd_mark_handoff(struct mali_cmd_buffer *cmd, struct mali_memory *mem)
{
   if (!mem || (mem->isync != MALI_ISYNC_BO_EXPORTED && mem->isync != MALI_ISYNC_BO_IMPORTED))
      return;
   for (unsigned i = 0; i < cmd->nhandoff; i++)
      if (cmd->handoff[i] == mem)
         return;
   if (cmd->nhandoff < MALI_CMD_HANDOFF_MAX)
      cmd->handoff[cmd->nhandoff++] = mem;
   else
      cmd->handoff_full = true;
}

void
mali_cmd_mark_handoff_image(struct mali_cmd_buffer *cmd, VkImage i)
{
   if (g_res.image_mem)
      mali_cmd_mark_handoff(cmd, g_res.image_mem(i));
}

void
mali_cmd_mark_buffer_handoff(struct mali_cmd_buffer *cmd, VkBuffer b)
{
   if (g_res.buffer_mem)
      mali_cmd_mark_handoff(cmd, g_res.buffer_mem(b));
}

void
mali_cmd_mark_image_read(struct mali_cmd_buffer *cmd, VkImage i)
{
   if (g_res.image_mem)
      mali_cmd_mark_read(cmd, g_res.image_mem(i));
}

void
mali_cmd_mark_buffer_read(struct mali_cmd_buffer *cmd, VkBuffer b)
{
   if (g_res.buffer_mem)
      mali_cmd_mark_read(cmd, g_res.buffer_mem(b));
}

void
mali_cmd_mark_image_view_read(struct mali_cmd_buffer *cmd, VkImageView v)
{
   if (g_res.image_view_mem)
      mali_cmd_mark_read(cmd, g_res.image_view_mem(v));
}

void
mali_cmd_mark_buffer_written(struct mali_cmd_buffer *cmd, VkBuffer b)
{
   if (cmd && g_res.buffer_mem)
      mali_cmd_mark_written(cmd, g_res.buffer_mem(b));
}

void
mali_cmd_mark_image_written(struct mali_cmd_buffer *cmd, VkImage i)
{
   if (cmd && g_res.image_mem)
      mali_cmd_mark_written(cmd, g_res.image_mem(i));
}

static void
reset_state(struct mali_cmd_buffer *cmd)
{
   for (unsigned i = 0; i < cmd->nframes; i++)
      mali_frame_finish(&cmd->frames[i]);
   free(cmd->frames);
   cmd->frames = NULL;
   cmd->nframes = cmd->capframes = 0;
   for (unsigned i = 0; i < cmd->ndef_frames; i++)
      mali_frame_finish(&cmd->def_frames[i]);
   free(cmd->def_frames);
   cmd->def_frames = NULL;
   cmd->ndef_frames = cmd->capdef_frames = 0;
   mali_frame_finish(&cmd->cur);
   memset(&cmd->cur, 0, sizeof cmd->cur);
   cmd->in_render = false;
   memset(&cmd->gfx, 0, sizeof cmd->gfx);
   cmd->have_gfx = false;
   cmd->cs = NULL;
   memset(cmd->push_cs, 0, sizeof cmd->push_cs);
   memset(cmd->vb_va, 0, sizeof cmd->vb_va);
   memset(cmd->vb_size, 0, sizeof cmd->vb_size);
   memset(cmd->vb_stride, 0, sizeof cmd->vb_stride);
   memset(cmd->push, 0, sizeof cmd->push);
   /* the TF range is recording state like the bindings above: a reset buffer that kept the old handles would patch a
    * freed buffer's address into the next draw (mali_cmd.c: the XFB patch below), so it goes with the push block. */
   memset(&cmd->tf, 0, sizeof cmd->tf);
   cmd->nsets = 0;
   cmd->ndyn = 0;
   cmd->occ_va = 0;
   cmd->occ_mode = 0;
   cmd->dirty = MALI_D_ALL;
   free(cmd->def_ops);
   cmd->def_ops = NULL;
   cmd->ndef = cmd->capdef = 0;
   cmd->nwrote = 0;
   cmd->wrote_full = false;
   /* implicit sync: the read marks are per recording too. A reset buffer that kept them could wait (or, worse, name)
    * an imported dma-buf that its owner has freed since. */
   cmd->nread = 0;
   cmd->read_full = false;
   cmd->nhandoff = 0;
   cmd->handoff_full = false;
   mali_meta_cmd_forget(cmd);
}

/* ---- vk_command_buffer_ops ---- */
static VkResult
mali_cmd_create(struct vk_command_pool *pool, VkCommandBufferLevel level, struct vk_command_buffer **out)
{
   struct mali_cmd_buffer *cmd = vk_zalloc(&pool->alloc, sizeof *cmd, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct vk_command_buffer_init_params ip = { .pool = pool, .level = level, .ops = &mali_cmd_ops,
                                               .needs_cmd_queue = level == VK_COMMAND_BUFFER_LEVEL_SECONDARY };
   VkResult r = vk_command_buffer_init_with_params(&cmd->vk, &ip);
   if (r != VK_SUCCESS) {
      vk_free(&pool->alloc, cmd);
      return r;
   }
   *out = &cmd->vk;
   return VK_SUCCESS;
}

static void
mali_cmd_reset(struct vk_command_buffer *vk, VkCommandBufferResetFlags flags)
{
   reset_state(mali_cmd_buffer_from_vk(vk));
   vk_command_buffer_reset(vk);
}

static void
mali_cmd_destroy(struct vk_command_buffer *vk)
{
   struct mali_cmd_buffer *cmd = mali_cmd_buffer_from_vk(vk);
   struct vk_command_pool *pool = vk->pool;
   reset_state(cmd);
   vk_command_buffer_finish(vk);
   vk_free(&pool->alloc, cmd);
}

const struct vk_command_buffer_ops mali_cmd_ops = {
   .create = mali_cmd_create,
   .reset = mali_cmd_reset,
   .destroy = mali_cmd_destroy,
};

VKAPI_ATTR VkResult VKAPI_CALL
mali_BeginCommandBuffer(VkCommandBuffer h, const VkCommandBufferBeginInfo *bi)
{
   CMD(h);
   vk_command_buffer_begin(&cmd->vk, bi);   /* calls ops->reset (-> reset_state) when it was recorded before */
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_EndCommandBuffer(VkCommandBuffer h)
{
   CMD(h);
   if (cmd->in_render)
      unsupported(cmd, "command buffer ended inside a rendering");
   return vk_command_buffer_end(&cmd->vk);   /* returns the sticky error set by unsupported() */
}

/* ---- binding ---- */
VKAPI_ATTR void VKAPI_CALL
mali_CmdBindPipeline(VkCommandBuffer h, VkPipelineBindPoint bp, VkPipeline p)
{
   CMD(h);
   if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) {
      if (!g_res.compute || g_res.compute(p, &cmd->cs, &cmd->cs_va)) {
         cmd->cs = NULL;
         unsupported(cmd, "compute pipeline");
      }
      return;
   }
   if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS) {
      unsupported(cmd, "bind point");
      return;
   }
   struct mali_gfx_state ng;
   memset(&ng, 0, sizeof ng);
   if (!g_res.pipeline || g_res.pipeline(p, &ng)) {
      cmd->have_gfx = false;
      cmd->dirty |= MALI_D_PIPE;
      unsupported(cmd, "pipeline");
      return;
   }
   if (!ng.push_base[0])
      ng.push_base[0] = 40;   /* compile defaults: VS push block at FAU byte 40, FS at 0 */
   if (memcmp(&ng, &cmd->gfx, sizeof ng)) {   /* rebinding an equal pipeline is not a change */
      cmd->gfx = ng;
      cmd->dirty |= MALI_D_PIPE;
   }
   if (g_res.pipeline_dyn) {
      const struct vk_dynamic_graphics_state *pd;
      if (!g_res.pipeline_dyn(p, &pd))
         vk_cmd_set_dynamic_graphics_state(&cmd->vk, pd);
   }
   cmd->have_gfx = true;
}

void
mali_cmd_bind_shaders(struct vk_command_buffer *vk, uint32_t stage_count, const mesa_shader_stage *stages,
                      struct vk_shader *const *shaders)
{
   struct mali_cmd_buffer *cmd = mali_cmd_buffer_from_vk(vk);
   for (uint32_t i = 0; i < stage_count; i++) {
      const struct mali_compiled *c = NULL;
      uint64_t va = 0;
      if (!shaders[i])
         continue;   /* unbind: keep the other stage */
      if (!g_res.shader || g_res.shader(shaders[i], &c, &va)) {
         unsupported(cmd, "shader");
         return;
      }
      if (stages[i] == MESA_SHADER_VERTEX) {
         cmd->gfx.vs = c;
         cmd->gfx.vs_va = va;
         cmd->gfx.vs_plain = NULL;   /* a dynamically bound shader has no compiled non-XFB variant */
         cmd->gfx.vs_plain_va = 0;
      } else if (stages[i] == MESA_SHADER_FRAGMENT) {
         cmd->gfx.fs = c;
         cmd->gfx.fs_va = va;
      } else {
         unsupported(cmd, "stage");
         return;
      }
   }
   if (!cmd->gfx.push_base[0])
      cmd->gfx.push_base[0] = 40;
   cmd->have_gfx = cmd->gfx.vs && cmd->gfx.fs;
   cmd->dirty |= MALI_D_PIPE;
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdBindDescriptorSets(VkCommandBuffer h, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first, uint32_t n,
                           const VkDescriptorSet *sets, uint32_t ndyn, const uint32_t *dyn)
{
   CMD(h);
   VK_FROM_HANDLE(vk_pipeline_layout, pl, layout);
   uint32_t used = 0;
   for (uint32_t i = 0; i < n; i++) {
      unsigned s = first + i;
      if (s >= MALI_CMD_SETS) {
         unsupported(cmd, "set index");
         return;
      }
      struct mali_dset *ds = mali_dset_from_handle(sets[i]);
      /* M15: storage descriptors of a bound set can write their memory */
      for (unsigned k = 0; ds && k < ds->nwrote; k++)
         mali_cmd_mark_written(cmd, ds->wrote[k]);
      if (ds && ds->wrote_full)
         cmd->wrote_full = true;
      /* Implicit sync: the imported dma-bufs this set samples (mali_isync.h). */
      for (unsigned k = 0; ds && k < ds->nread; k++)
         mali_cmd_mark_read(cmd, ds->read[k]);
      if (ds && ds->read_full)
         cmd->read_full = true;
      if (ds->layout && ds->layout->dyn_count) {
         /* dynamic buffers: this set's dyn_start = dynamic descriptors of the lower sets of the pipeline layout (matches mali_pipeline_layout_flatten) */
         unsigned start = 0;
         for (unsigned j = 0; j < s && pl && j < pl->set_count; j++)
            if (pl->set_layouts[j])
               start += ((const struct mali_dsl *)pl->set_layouts[j])->dyn_count;
         for (unsigned k = 0; k < ds->layout->dyn_count; k++) {
            if (start + k >= MALI_MAX_DYN || used >= ndyn) {
               unsupported(cmd, "dynamic descriptor offsets (count)");
               return;
            }
            struct mali_hw_desc hd;
            mali_dyn_buf_pack(ds->arch, &hd, &ds->dyn[k], dyn[used++]);
            if (memcmp(cmd->dyn_desc[start + k], hd.w, sizeof hd.w)) {
               memcpy(cmd->dyn_desc[start + k], hd.w, sizeof hd.w);
               cmd->dirty |= MALI_D_DYNBUF;
            }
            if (start + k + 1 > cmd->ndyn) {
               cmd->ndyn = start + k + 1;
               cmd->dirty |= MALI_D_BIND;
            }
         }
      }
      if (cmd->set_va[s] != ds->va || cmd->set_bytes[s] != ds->bytes) {
         cmd->set_va[s] = ds->va;
         cmd->set_bytes[s] = ds->bytes;
         cmd->dirty |= MALI_D_BIND;
      }
      if (s + 1 > cmd->nsets) {
         cmd->nsets = s + 1;
         cmd->dirty |= MALI_D_BIND;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdBindVertexBuffers(VkCommandBuffer h, uint32_t first, uint32_t n, const VkBuffer *bufs, const VkDeviceSize *offs)
{
   CMD(h);
   for (uint32_t i = 0; i < n; i++) {
      unsigned b = first + i;
      if (b >= MALI_MAX_VB) {
         unsupported(cmd, "vertex binding");
         return;
      }
      struct vk_buffer *buf = vk_buffer_from_handle(bufs[i]);
      mali_cmd_mark_buffer_read(cmd, bufs[i]);   /* implicit sync: an imported vertex buffer is a read (mali_isync.h) */
      uint64_t nva = buf ? vk_buffer_address(buf, offs[i]) : 0;
      uint32_t nsz = buf ? (uint32_t)(buf->size - offs[i]) : 0;
      if (cmd->vb_va[b] != nva || cmd->vb_size[b] != nsz) {
         cmd->vb_va[b] = nva;
         cmd->vb_size[b] = nsz;   /* the dynamic stride (BindVertexBuffers2) is kept: a plain bind does not set it */
         cmd->dirty |= MALI_D_BIND;
      }
   }
}

static const uint8_t *buffer_cpu(VkBuffer b, VkDeviceSize off, VkDeviceSize need);


VKAPI_ATTR void VKAPI_CALL
mali_CmdBindIndexBuffer(VkCommandBuffer h, VkBuffer b, VkDeviceSize off, VkIndexType t)
{
   CMD(h);
   mali_cmd_mark_buffer_read(cmd, b);   /* implicit sync: an imported index buffer is a read (mali_isync.h) */
   struct vk_buffer *buf = vk_buffer_from_handle(b);
   if (!buf) {
      cmd->ib_va = 0;
      cmd->ib_cpu = NULL;
      cmd->ib_bytes = 0;
      cmd->ib_size = 0;
      return;
   }
   if (t != VK_INDEX_TYPE_UINT16 && t != VK_INDEX_TYPE_UINT32 && t != VK_INDEX_TYPE_UINT8_KHR) {
      unsupported(cmd, "index type");
      return;
   }
   cmd->ib_va = vk_buffer_address(buf, off);
   cmd->ib_bytes = (uint32_t)(buf->size - off);
   cmd->ib_cpu = buffer_cpu(b, off, 0);
   cmd->ib_size = t == VK_INDEX_TYPE_UINT32 ? 4 : t == VK_INDEX_TYPE_UINT16 ? 2 : 1;
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdPushConstants(VkCommandBuffer h, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t off, uint32_t size,
                      const void *data)
{
   CMD(h);
   if (off + size > MALI_PUSH_MAX) {
      unsupported(cmd, "push constant range");
      return;
   }
   /* Per-stage view: bytes written with stageFlags without a stage are invisible to that stage (Vulkan push constant semantics) */
   if ((stages & VK_SHADER_STAGE_VERTEX_BIT) && memcmp(cmd->push[0] + off, data, size)) {
      memcpy(cmd->push[0] + off, data, size);
      cmd->dirty |= MALI_D_PUSH;
   }
   if ((stages & VK_SHADER_STAGE_FRAGMENT_BIT) && memcmp(cmd->push[1] + off, data, size)) {
      memcpy(cmd->push[1] + off, data, size);
      cmd->dirty |= MALI_D_PUSH;
   }
   if (stages & VK_SHADER_STAGE_COMPUTE_BIT)
      memcpy(cmd->push_cs + off, data, size);
}

/* VK_KHR_maintenance6: the "2" forms carry stage flags instead of a bind point. They forward to the 1.0 entry points (the runtime's
 * vk_common_* wrappers go the other way, but we override the 1.0 ones). A null layout (pNext layout info) needs push descriptors /
 * descriptor buffers, which are not advertised. */
VKAPI_ATTR void VKAPI_CALL
mali_CmdBindDescriptorSets2KHR(VkCommandBuffer h, const VkBindDescriptorSetsInfoKHR *info)
{
   CMD(h);
   if (info->layout == VK_NULL_HANDLE) {
      unsupported(cmd, "BindDescriptorSets2 without a layout");
      return;
   }
   if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
      mali_CmdBindDescriptorSets(h, VK_PIPELINE_BIND_POINT_COMPUTE, info->layout, info->firstSet, info->descriptorSetCount,
                                 info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets);
   if (info->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS)
      mali_CmdBindDescriptorSets(h, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, info->firstSet, info->descriptorSetCount,
                                 info->pDescriptorSets, info->dynamicOffsetCount, info->pDynamicOffsets);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdPushConstants2KHR(VkCommandBuffer h, const VkPushConstantsInfoKHR *info)
{
   mali_CmdPushConstants(h, info->layout, info->stageFlags, info->offset, info->size, info->pValues);
}

/* ---- draws ---- */
static bool
snapshot_draw(struct mali_cmd_buffer *cmd, struct mali_rec *d, uint32_t count, uint32_t inst, uint32_t first, uint32_t first_inst)
{
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   const int32_t *a = cmd->cur.area;
   struct mali_frame *fr = &cmd->cur;
   /* VK_EXT_provoking_vertex: one mode per render pass (provokingVertexModePerPipeline = false); the latest draw's mode applies to the frame */
   fr->provoking_last = dyn->rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
   const struct mali_rec *pv = fr->ndraws ? &fr->draws[fr->ndraws - 1] : NULL;
   /* dirty mask: setter bits plus the runtime's dynamic-state dirty bits (viewport and scissor apart from everything else) */
   uint32_t m = cmd->dirty;
   cmd->dirty = 0;
   if (!BITSET_IS_EMPTY(dyn->dirty)) {
      BITSET_DECLARE(t, MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX);
      memcpy(t, dyn->dirty, sizeof t);
      if (BITSET_TEST(t, MESA_VK_DYNAMIC_VP_VIEWPORTS) || BITSET_TEST(t, MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT))
         m |= MALI_D_VP;
      if (BITSET_TEST(t, MESA_VK_DYNAMIC_VP_SCISSORS) || BITSET_TEST(t, MESA_VK_DYNAMIC_VP_SCISSOR_COUNT))
         m |= MALI_D_SC;
      BITSET_CLEAR(t, MESA_VK_DYNAMIC_VP_VIEWPORTS);
      BITSET_CLEAR(t, MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT);
      BITSET_CLEAR(t, MESA_VK_DYNAMIC_VP_SCISSORS);
      BITSET_CLEAR(t, MESA_VK_DYNAMIC_VP_SCISSOR_COUNT);
      if (!BITSET_IS_EMPTY(t))
         m |= MALI_D_FX;
      vk_dynamic_graphics_state_clear_dirty((struct vk_dynamic_graphics_state *)dyn);
   }
   if (!pv)
      m = MALI_D_ALL;
   *d = (struct mali_rec){
      .dirty = m,
      .pipe = pv ? pv->pipe : NULL, .bind = pv ? pv->bind : NULL, .push = pv ? pv->push : NULL, .dyn = pv ? pv->dyn : NULL,
      .occ_va = cmd->occ_va, .occ_mode = cmd->occ_mode,
      .count = count, .instance_count = inst, .first_vertex = first, .first_instance = first_inst,
   };
   if (m & (MALI_D_BIND | MALI_D_DYNBUF)) {
      struct mali_bdyn *bd = mali_frame_blk(fr, sizeof *bd + cmd->ndyn * sizeof bd->d[0]);
      if (!bd)
         return false;
      bd->n = cmd->ndyn;
      memcpy(bd->d, cmd->dyn_desc, cmd->ndyn * sizeof bd->d[0]);
      d->dyn = bd;
   }
   if (m & MALI_D_BIND) {
      struct mali_bbind *bb = mali_frame_blk(fr, sizeof *bb);
      if (!bb)
         return false;
      memcpy(bb->vb_va, cmd->vb_va, sizeof bb->vb_va);
      memcpy(bb->vb_size, cmd->vb_size, sizeof bb->vb_size);
      memcpy(bb->vb_stride, cmd->vb_stride, sizeof bb->vb_stride);
      bb->nsets = cmd->nsets;
      memcpy(bb->set_va, cmd->set_va, sizeof bb->set_va);
      memcpy(bb->set_bytes, cmd->set_bytes, sizeof bb->set_bytes);
      bb->ndyn = cmd->ndyn;
      d->bind = bb;
   }
   /* VK_EXT_transform_feedback: the pipeline reserves 32 B after the app's push constants for the four XFB buffer
    * addresses; they change per draw (the buffers can be rebound between draws), so patch them into this draw's
    * push block. */
   const bool xfb = cmd->have_gfx && cmd->gfx.xfb;
   const unsigned xfb_pc = xfb ? cmd->gfx.xfb_pc : 0;
   /* No range open: run the pipeline's second, non-XFB vertex program instead of lowering the stores with zero bases.
    * The push block is still republished, so a draw that reuses the previous record cannot inherit a live range's
    * addresses (the block itself is zero there: mali_CmdPushConstants cannot reach past the application's range). */
   const bool xfb_off = xfb && !cmd->tf.active && cmd->gfx.vs_plain;
   if (xfb && !xfb_off && xfb_pc + 32 > MALI_PUSH_MAX)
      return false;
   /* Both the push block and the pipeline block are republished for every XFB draw: which of the two vertex programs
    * runs changes with the range, so inheriting the previous draw's blocks would run the wrong one. */
   if (xfb)
      m |= MALI_D_PIPE | MALI_D_PUSH;
   if (m & MALI_D_PUSH) {
      struct mali_bpush *bu = mali_frame_blk(fr, sizeof *bu);
      if (!bu)
         return false;
      memcpy(bu->push, cmd->push, sizeof bu->push);
      if (xfb && cmd->tf.active)
         for (unsigned k = 0; k < 4; k++) {
            const uint64_t va = k < cmd->tf.nbufs && cmd->tf.buf[k]
                                   ? vk_buffer_address(vk_buffer_from_handle(cmd->tf.buf[k]), cmd->tf.off[k]) : 0;
            memcpy(bu->push[0] + xfb_pc + k * 8, &va, sizeof va);
         }
      /* MALI_TF_DBG=1 (off by default): the device-side half of a transform-feedback diagnosis, next to the
       * compile-side line in mali_compile.c - what the draw believes the four buffer bases are. */
      if (getenv("MALI_TF_DBG") && xfb && cmd->tf.active) {
         uint64_t q[4];
         memcpy(q, bu->push[0] + xfb_pc, sizeof q);
         fprintf(stderr, "mali_tf: draw xfb_pc %u bases %llx %llx %llx %llx\n", xfb_pc,
                 (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3]);
      }
      d->push = bu;
   }
   if (m & (MALI_D_PIPE | MALI_D_FX | MALI_D_VP | MALI_D_SC)) {
      struct mali_bpipe *bp = mali_frame_blk(fr, sizeof *bp);
      if (!bp)
         return false;
      d->pipe = bp;
      bp->gfx = cmd->gfx;
      if (xfb_off) {   /* no range open: the non-XFB program (mali_pipeline.c), so nothing is stored */
         bp->gfx.vs = bp->gfx.vs_plain;
         bp->gfx.vs_va = bp->gfx.vs_plain_va;
      }
      if (xfb && getenv("MALI_TF_DBG"))
         fprintf(stderr, "mali_tf: draw uses the %s vertex program at 0x%" PRIx64 "\n", xfb_off ? "plain" : "XFB", bp->gfx.vs_va);
      if (BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_VP_VIEWPORTS) && dyn->vp.viewport_count) {
         const VkViewport *v = &dyn->vp.viewports[0];
         bp->vp[0] = v->x; bp->vp[1] = v->y; bp->vp[2] = v->width; bp->vp[3] = v->height; bp->vp[4] = v->minDepth; bp->vp[5] = v->maxDepth;
      } else {
         bp->vp[0] = (float)a[0]; bp->vp[1] = (float)a[1]; bp->vp[2] = (float)a[2]; bp->vp[3] = (float)a[3];
         bp->vp[4] = 0.0f; bp->vp[5] = 1.0f;
      }
      if (BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_VP_SCISSORS) && dyn->vp.scissor_count) {
         const VkRect2D *s = &dyn->vp.scissors[0];
         bp->sc[0] = s->offset.x; bp->sc[1] = s->offset.y; bp->sc[2] = (int32_t)s->extent.width; bp->sc[3] = (int32_t)s->extent.height;
      } else {
         memcpy(bp->sc, a, sizeof bp->sc);
      }
      /* R4: effective fixed-function state (pipeline statics + vkCmdSet*), see mali_CmdBindPipeline */
      struct mali_fixed *x = &bp->fx;
      x->set = true;
      x->cull = (dyn->rs.cull_mode & VK_CULL_MODE_FRONT_BIT ? 1 : 0) | (dyn->rs.cull_mode & VK_CULL_MODE_BACK_BIT ? 2 : 0);
      x->front_ccw = dyn->rs.front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE;
      x->depth_clamp = dyn->rs.depth_clamp_enable;
      x->depth_clip = vk_rasterization_state_depth_clip_enable(&dyn->rs);
      x->bias_en = dyn->rs.depth_bias.enable;
      x->bias_const = dyn->rs.depth_bias.constant_factor;
      x->bias_slope = dyn->rs.depth_bias.slope_factor;
      x->bias_clamp = dyn->rs.depth_bias.clamp;
      x->line_width = dyn->rs.line.width;
      x->st_test = dyn->ds.stencil.test_enable;
      {
         const struct vk_stencil_test_face_state *fs[2] = { &dyn->ds.stencil.front, &dyn->ds.stencil.back };
         for (int k = 0; k < 2; k++) {
            struct mali_st_face *o = k ? &x->st_b : &x->st_f;
            o->cmp = fs[k]->op.compare; o->fail = fs[k]->op.fail; o->dfail = fs[k]->op.depth_fail; o->pass = fs[k]->op.pass;
            o->wmask = fs[k]->write_mask; o->vmask = fs[k]->compare_mask; o->ref = fs[k]->reference;
         }
      }
      if (BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE))
         bp->gfx.depth_test = dyn->ds.depth.test_enable;
      if (BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE))
         bp->gfx.depth_write = dyn->ds.depth.write_enable;
      if (BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_DS_DEPTH_COMPARE_OP))
         bp->gfx.depth_func = dyn->ds.depth.compare_op;
      if (BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY)) {
         switch (dyn->ia.primitive_topology) {
         case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: bp->gfx.prim = 1; break;
         case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: bp->gfx.prim = 2; break;
         case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: bp->gfx.prim = 4; break;
         case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: bp->gfx.prim = 8; break;
         case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: bp->gfx.prim = 10; break;
         case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: bp->gfx.prim = 12; break;
         default: unsupported(cmd, "dynamic topology");
         }
      }
      if (BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_IA_PRIMITIVE_RESTART_ENABLE))
         bp->gfx.prim_restart = dyn->ia.primitive_restart_enable;
      /* fillModeNonSolid: polygonMode (pipeline state or VK_EXT_extended_dynamic_state3 vkCmdSetPolygonModeEXT; dyn->set covers both) needs the
       * API topology above. LINE = triangle edges (prim 2, rewritten to an indexed line list at frame build), POINT = the vertices as a point list */
      if (BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_RS_POLYGON_MODE) && BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY)) {
         const bool tri = bp->gfx.prim >= 8;
         bp->gfx.poly_mode = 0;
         bp->gfx.poly_prim = 0;
         switch (dyn->rs.polygon_mode) {
         case VK_POLYGON_MODE_FILL: break;
         case VK_POLYGON_MODE_LINE:   /* a non-triangle topology: polygonMode does not apply */
            if (tri) { bp->gfx.poly_mode = 1; bp->gfx.poly_prim = bp->gfx.prim; bp->gfx.prim = 2; }
            break;
         case VK_POLYGON_MODE_POINT:
            if (tri) { bp->gfx.poly_mode = 2; bp->gfx.poly_prim = bp->gfx.prim; bp->gfx.prim = 1; }   /* prim_restart stays: the rewrite uses it to split strips */
            break;
         default: unsupported(cmd, "polygon mode");
         }
      }
      /* the colour attachment this frame renders: index 0 unless the pass's leading slots are null (then slot 1, ...), so
       * the blend and colour-write state must be read for that slot, not for slot 0 the view may not exist for */
      const unsigned cs = cmd->cur.has_colour ? (cmd->cur.colour_slot & 7u) : 0u;
      const struct vk_color_blend_attachment_state *cb = &dyn->cb.attachments[cs];
      x->blend_en = BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_CB_BLEND_ENABLES) && cb->blend_enable;
      x->wmask = BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_CB_WRITE_MASKS) ? cb->write_mask : 0xf;
      if (BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_CB_COLOR_WRITE_ENABLES) && !(dyn->cb.color_write_enables & (1u << cs)))
         x->wmask = 0;
      x->src_c = cb->src_color_blend_factor; x->dst_c = cb->dst_color_blend_factor;
      x->src_a = cb->src_alpha_blend_factor; x->dst_a = cb->dst_alpha_blend_factor;
      x->op_c = cb->color_blend_op; x->op_a = cb->alpha_blend_op;
      memcpy(x->bconst, dyn->cb.blend_constants, sizeof x->bconst);
      /* attachments 1..3 have their own blend state (independentBlend is advertised). Only the CPU rasteriser draws
       * them today — a frame with more than one colour attachment is refused on the hardware path — so recording it
       * here cannot change the command stream; it is what the renderer that does draw them reads. */
      for (unsigned r = 1; r < 4; r++) {
         const struct vk_color_blend_attachment_state *cr = &dyn->cb.attachments[r];
         struct mali_fx_att *o = &x->xatt[r - 1];
         o->en = BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_CB_BLEND_ENABLES) && cr->blend_enable;
         o->wmask = BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_CB_WRITE_MASKS) ? cr->write_mask : 0xf;
         if (BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_CB_COLOR_WRITE_ENABLES) && !(dyn->cb.color_write_enables & (1u << r)))
            o->wmask = 0;
         o->src_c = cr->src_color_blend_factor; o->dst_c = cr->dst_color_blend_factor;
         o->src_a = cr->src_alpha_blend_factor; o->dst_a = cr->dst_alpha_blend_factor;
         o->op_c = cr->color_blend_op; o->op_a = cr->alpha_blend_op;
         memcpy(o->bconst, dyn->cb.blend_constants, sizeof o->bconst);
      }
      /* vk_graphics_state keeps the previous pass' logic_op_enable when a pipeline without colour blending is bound:
       * a depth-only pass must ignore it, not fail the command buffer on retained state */
      if (dyn->cb.logic_op_enable && cmd->cur.has_colour) {
         if (bp->gfx.blend_va)
            x->blend_en = true;   /* the pipeline's blend shader does the logic op (mali_pipeline.c); it reads the destination */
         else if (!bp->gfx.logic_inert)
            unsupported(cmd, "dynamic logic op");
      }
      if (x->blend_en && !bp->gfx.blend_va) {
         uint32_t eq; unsigned cm;
         if (mali_blend_equation(x, &eq, &cm))
            unsupported(cmd, "blend equation (fixed function)");
      }
   }
   return true;
}

/* rasterizer discard (pipeline state or dynamic) / pipeline without a fragment stage: the draw produces no fragments, so it is dropped */
static bool
discarded(const struct mali_cmd_buffer *cmd)
{
   return cmd->gfx.discard || cmd->vk.dynamic_graphics_state.rs.rasterizer_discard_enable;
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdDraw(VkCommandBuffer h, uint32_t vcount, uint32_t icount, uint32_t first, uint32_t first_inst)
{
   CMD(h);
   if (!cmd->in_render || !cmd->have_gfx) {
      unsupported(cmd, "draw outside a rendering with a pipeline");
      return;
   }
   if (discarded(cmd))
      return;
   if (!vcount || !icount)
      return;
   /* VK_EXT_transform_feedback: the lowered stores index with instance_id * num_vertices + vertex_id, and
    * num_vertices is not supplied to the shader yet (mali_compile.c replaces it with 0), so instanced XFB would
    * write every instance's vertices to the same slots. Refuse rather than write overlapping data silently. */
   if (cmd->tf.active && icount > 1) {
      unsupported(cmd, "transform feedback with instance_count > 1 (num_vertices is not wired; single-instance only)");
      return;
   }
   struct mali_rec *sl = mali_frame_draw_slot(&cmd->cur);   /* snapshot in place */
   if (!sl || !snapshot_draw(cmd, sl, vcount, icount, first, first_inst)) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   cmd->cur.ndraws++;
   mali_frame_note_rec(&cmd->cur, sl);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdDrawIndexed(VkCommandBuffer h, uint32_t icount, uint32_t inst, uint32_t first, int32_t voff, uint32_t first_inst)
{
   CMD(h);
   if (!cmd->in_render || !cmd->have_gfx || !cmd->ib_va) {
      unsupported(cmd, "indexed draw outside a rendering, without a pipeline or index buffer");
      return;
   }
   if (discarded(cmd))
      return;
   if (!icount || !inst)
      return;
   /* Same restriction as mali_CmdDraw: num_vertices is not wired (mali_compile.c replaces it with 0), so instanced
    * XFB would write every instance to the same slots (CodeRabbit on #313, vk/src/mali_cmd.c:633). */
   if (cmd->tf.active && inst > 1) {
      unsupported(cmd, "transform feedback with instance_count > 1 (num_vertices is not wired; single-instance only)");
      return;
   }
   struct mali_rec *sl = mali_frame_draw_slot(&cmd->cur);
   if (!sl || !snapshot_draw(cmd, sl, icount, inst, (uint32_t)voff, first_inst)) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   sl->indexed = true;
   sl->index_va = cmd->ib_va;
   sl->index_cpu = cmd->ib_cpu;
   sl->index_bytes = cmd->ib_bytes;
   sl->index_size = cmd->ib_size;
   sl->first_index = first;
   cmd->cur.ndraws++;
   mali_frame_note_rec(&cmd->cur, sl);
}

/* Indirect draws: the command array is either read through the CPU mapping when the frame is submitted (frames are serialised,
 * so every earlier write to the buffer has retired) and expanded into plain draws (expand_indirect), or - the default - handed
 * to the command stream as one record per command and loaded into the IDVS registers at execution time (emit_draw_regs), which
 * is the only way a descriptor a compute job wrote can drive the draw. */
static const uint8_t *
buffer_cpu(VkBuffer b, VkDeviceSize off, VkDeviceSize need)
{
   struct mali_buffer *mb = mali_buffer_from_handle(b);
   if (!mb || !mb->mem || !mb->mem->bo.cpu || off + need > mb->vk.size)
      return NULL;
   return (const uint8_t *)mb->mem->bo.cpu + mb->mem_off + off;
}

/* Commands are unrolled: one LOAD_MULTIPLE + RUN_IDVS per VkDraw*IndirectCommand, because libkbase has no CS control flow
 * (no WHILE/IF, kbase_cs.h). A drawCount above this therefore keeps the CPU expansion of every earlier release -- a real
 * multiDrawIndirect needs the CS loop PanVK uses (panvk_vX_cmd_draw.c:3353), which is also what DrawIndirectCount waits
 * for. 256 x ~15 words is a frame's worth of stream; the cases this exists for (DXVK, GPU-driven engines) issue 1..16. */
#define MALI_IND_UNROLL_MAX 256u

static void
record_indirect(VkCommandBuffer h, bool indexed, VkBuffer b, VkDeviceSize off, VkBuffer cb, VkDeviceSize coff, uint32_t n, uint32_t stride)
{
   CMD(h);
   struct mali_rec d;
   if (!cmd->in_render || !cmd->have_gfx || (indexed && !cmd->ib_va)) {
      unsupported(cmd, "indirect draw outside a rendering, without a pipeline or index buffer");
      return;
   }
   if (discarded(cmd))
      return;
   uint32_t sz = indexed ? 20 : 16;
   if (!stride)
      stride = sz;
   if (!n)
      return;
   /* The arguments are read by the CS at execution time (`va`) unless something needs them on the CPU: a count buffer (no
    * CS control flow yet), the software rasteriser, transform feedback's single-instance check, and polygonMode
    * LINE/POINT's index assembly. Those keep reading the CPU mapping, which is provably what they did before; every other
    * indirect draw is the one that must NOT read it, because the mapping still holds what the host wrote even after a
    * compute job rewrote the descriptor. The shaderDrawParameters sysvals are not in this list: the FAU words they read are
    * patched from the loaded registers by the command stream (mali_cs_emit.c emit_indirect_args). */
   /* cmd->gfx.poly_mode is the pipeline's STATIC polygon mode; with VK_EXT_extended_dynamic_state3 the record-time value
    * lives in the dynamic state, and LINE/POINT there also rewrites the draw into an index list at build time, which needs
    * the arguments on the CPU */
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   const bool dyn_poly = BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_RS_POLYGON_MODE) && dyn->rs.polygon_mode != VK_POLYGON_MODE_FILL;
   /* MALI_IND_CPU selects the CPU read as a control. Its two values are the two halves of the old behaviour, and only
    * "drain" is correct: the CPU reads the mapping at SUBMIT time (expand_indirect), so it sees a compute job's write only
    * if every earlier frame has retired first -- which the drain did, and which is exactly the serialization this lane
    * removes. MALI_IND_CPU=1 therefore means "read the mapping, do NOT drain": the stale read, i.e. the failing control
    * (nothing is drawn when a compute job in the same submission produced the command). MALI_IND_CPU=drain keeps the
    * historical pair (read + drain) for the A/B, where the picture must stay correct. */
   const char *ind_cpu_env = getenv("MALI_IND_CPU");
   const bool force_cpu = ind_cpu_env && *ind_cpu_env && strcmp(ind_cpu_env, "0") != 0;
   const bool cpu_drain = force_cpu && !strcmp(ind_cpu_env, "drain");
   const bool cpu_args = cb || g_hw.soft || cmd->tf.active || cmd->gfx.poly_mode || dyn_poly || n > MALI_IND_UNROLL_MAX || force_cpu;
   const uint8_t *p = NULL;
   const uint32_t *cp = NULL;
   uint64_t va = 0;
   if (cb && !(cp = (const uint32_t *)buffer_cpu(cb, coff, 4))) {
      unsupported(cmd, "indirect count buffer");
      return;
   }
   if (cpu_args)
      p = buffer_cpu(b, off, (VkDeviceSize)stride * (n - 1) + sz);
   else
      va = vk_buffer_address(vk_buffer_from_handle(b), off);
   if (!p && !va) {
      unsupported(cmd, "indirect buffer (no CPU mapping)");
      return;
   }
   if (!snapshot_draw(cmd, &d, 0, 0, 0, 0)) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   d.indexed = indexed;
   if (indexed) {
      d.index_va = cmd->ib_va;
      d.index_cpu = cmd->ib_cpu;
      d.index_bytes = cmd->ib_bytes;
      d.index_size = cmd->ib_size;
   }
   d.ind = p;
   d.ind_va = va;
   d.ind_count = cp;
   d.ind_n = n;
   d.ind_stride = stride;
   d.ind_nodrain = force_cpu && !cpu_drain;   /* the failing control: a CPU read with no ordering */
   if (getenv("MALI_PIPE_DBG"))
      fprintf(stderr, "mali_vk: indirect draw (n=%u, %s): arguments read on the %s%s\n", n, indexed ? "indexed" : "direct",
              va ? "GPU (command stream)" : "CPU (mapping at submit)",
              p && d.ind_nodrain ? ", NO drain: a compute job's write in this submission is invisible" : "");
   if (mali_frame_add_rec(&cmd->cur, &d))
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdDrawIndirect(VkCommandBuffer h, VkBuffer b, VkDeviceSize off, uint32_t n, uint32_t stride)
{
   record_indirect(h, false, b, off, VK_NULL_HANDLE, 0, n, stride);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdDrawIndexedIndirect(VkCommandBuffer h, VkBuffer b, VkDeviceSize off, uint32_t n, uint32_t stride)
{
   record_indirect(h, true, b, off, VK_NULL_HANDLE, 0, n, stride);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdDrawIndirectCount(VkCommandBuffer h, VkBuffer b, VkDeviceSize off, VkBuffer cb, VkDeviceSize coff, uint32_t max, uint32_t stride)
{
   record_indirect(h, false, b, off, cb, coff, max, stride);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdDrawIndexedIndirectCount(VkCommandBuffer h, VkBuffer b, VkDeviceSize off, VkBuffer cb, VkDeviceSize coff, uint32_t max, uint32_t stride)
{
   record_indirect(h, true, b, off, cb, coff, max, stride);
}

/* Replace the indirect draws of `f` by the plain draws their commands describe. Returns 0, or -ENOMEM. The result owns f->draws. */
static int
expand_indirect(struct mali_frame *f)
{
   struct mali_frame o = *f;
   o.draws = NULL;
   o.ndraws = o.cap = 0;
   uint32_t carry = 0;   /* dirty bits of draws that expand to nothing: the next draw's blocks inherit theirs, so it must renew those groups */
   for (unsigned i = 0; i < f->ndraws; i++) {
      const struct mali_rec *d = &f->draws[i];
      int rc = 0;
      if (!d->ind && !d->ind_va) {
         struct mali_rec e = *d;
         e.dirty |= carry;
         carry = 0;
         rc = mali_frame_add_rec(&o, &e);
      } else if (d->ind) {
         uint32_t n = d->ind_n;
         if (d->ind_count && *d->ind_count < n)
            n = *d->ind_count;
         for (uint32_t k = 0; k < n && !rc; k++) {
            struct mali_rec e = *d;
            if (k)
               e.dirty = 0;   /* same bound state as the first expanded draw (the per-draw parameters are compared at build) */

            const uint32_t *c = (const uint32_t *)(const void *)(d->ind + (size_t)k * d->ind_stride);
            e.ind = NULL;
            e.ind_count = NULL;
            e.count = c[0];
            e.draw_id = k;
            e.instance_count = c[1];
            if (d->indexed) {
               e.first_index = c[2];
               e.first_vertex = c[3];
               e.first_instance = c[4];
            } else {
               e.first_vertex = c[2];
               e.first_instance = c[3];
            }
            if (e.count && e.instance_count) {
               e.dirty |= carry;
               carry = 0;
               rc = mali_frame_add_rec(&o, &e);
            } else
               carry |= e.dirty;
         }
      } else {
         /* GPU-read descriptor: one record per command, each carrying its own VA; the count/instance/first_* words stay
          * unknown here and the command stream loads them (emit_draw_regs). Nothing is skipped as empty: the CPU cannot
          * see the values, and a skipped descriptor would leave the frame's dirty mask behind. */
         for (uint32_t k = 0; k < d->ind_n && !rc; k++) {
            struct mali_rec e = *d;
            if (k)
               e.dirty = 0;
            e.ind_va = d->ind_va + (uint64_t)k * d->ind_stride;
            e.draw_id = k;
            e.dirty |= carry;
            carry = 0;
            rc = mali_frame_add_rec(&o, &e);
         }
      }
      if (rc) {
         free(o.draws);
         return rc;
      }
   }
   *f = o;
   return 0;
}

#define push_frame mali_cmd_push_frame

/* ---- compute (M11): every dispatch is its own frame record, executed in order with the render passes ---- */
/* Only LOAD preserves the previous contents and needs a preload draw (colour, depth, stencil; MSAA and D24 depth are rejected for LOAD
 * only). VK_ATTACHMENT_LOAD_OP_NONE_KHR and DONT_CARE both leave the contents within the render area undefined, so neither may read
 * them: NONE is VK_KHR_load_store_op_none's explicit "do not load" and DONT_CARE is the same permission with the read allowed to be
 * skipped as a hint. The decision is per attachment (colour `load`, depth `zload`, stencil `sload`), mirroring the per-attachment store
 * flags (`colour.store`, `zs.store`, `st.store`): a pass may LOAD colour and DONT_CARE depth, or LOAD depth and DONT_CARE stencil.
 * With no load the FBD clears the tile to 0 (mali_render.c), which is legal for an undefined start.
 * STORE_OP_NONE leaves the memory untouched: for depth/stencil that is "not stored" (the ==STORE tests below), colour is always stored
 * by the FBD. */
static inline bool
is_load_op(VkAttachmentLoadOp op)
{
   return op == VK_ATTACHMENT_LOAD_OP_LOAD;
}

static void
record_dispatch(struct mali_cmd_buffer *cmd, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect_va)
{
   struct mali_frame f;
   if (cmd->in_render || !cmd->cs) {
      unsupported(cmd, "dispatch inside a rendering or without a compute pipeline");
      return;
   }
   if (!cmd->cs->bin_size)
      return;   /* an empty compute shader does nothing observable */
   memset(&f, 0, sizeof f);
   f.compute = true;
   f.disp.cs = cmd->cs;
   f.disp.cs_va = cmd->cs_va;
   memcpy(f.disp.push, cmd->push_cs, sizeof f.disp.push);
   f.disp.nsets = cmd->nsets;
   memcpy(f.disp.set_va, cmd->set_va, sizeof f.disp.set_va);
   memcpy(f.disp.set_bytes, cmd->set_bytes, sizeof f.disp.set_bytes);
   f.disp.ndyn = cmd->ndyn;
   memcpy(f.disp.dyn_desc, cmd->dyn_desc, sizeof f.disp.dyn_desc);
   f.disp.groups[0] = x;
   f.disp.groups[1] = y;
   f.disp.groups[2] = z;
   f.disp.indirect_va = indirect_va;
   push_frame(cmd, &f);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdDispatch(VkCommandBuffer h, uint32_t x, uint32_t y, uint32_t z)
{
   CMD(h);
   if (!x || !y || !z)
      return;
   record_dispatch(cmd, x, y, z, 0);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdDispatchIndirect(VkCommandBuffer h, VkBuffer b, VkDeviceSize off)
{
   CMD(h);
   struct vk_buffer *buf = vk_buffer_from_handle(b);
   if (!buf || (off & 3)) {
      unsupported(cmd, "indirect buffer");
      return;
   }
   record_dispatch(cmd, 0, 0, 0, vk_buffer_address(buf, off));
}

/* ---- VK_EXT_transform_feedback (stage 2: recording) ----
 * The streamout job does not exist yet: libkbase has RUN_IDVS/FINISH_TILING only (kbase_cs.h:111-154), so the
 * gallium route is nir_io_add_intrinsic_xfb_info + nir_lower_xfb_to_stores (pan_shader.c:124-136) with the base
 * addresses supplied as nir_load_xfb_address (nir_lower_xfb_to_stores.c:53), which our NIR backend does not lower
 * yet. Until it does, these commands record and validate the state (so a draw can report the real blocker) and
 * mark the XFB buffers as GPU-written, which they will be. */

VKAPI_ATTR void VKAPI_CALL
mali_CmdBindTransformFeedbackBuffersEXT(VkCommandBuffer h, uint32_t first, uint32_t count, const VkBuffer *buffers,
                                        const VkDeviceSize *offsets, const VkDeviceSize *sizes)
{
   CMD(h);
   /* The command is valid in both scopes (in and out of a render pass; VUID-vkCmdBindTransformFeedbackBuffersEXT-
    * renderpass does not exist), and only a rebind while capture is active is refused (CodeRabbit, #313). */
   if (cmd->tf.active) {
      unsupported(cmd, "vkCmdBindTransformFeedbackBuffersEXT while transform feedback is active");
      return;
   }
   if (!buffers || first + count > 4) {
      unsupported(cmd, "VK_EXT_transform_feedback: more than 4 XFB buffers");
      return;
   }
   for (uint32_t i = 0; i < count; i++) {
      const unsigned b = first + i;
      struct vk_buffer *vb = vk_buffer_from_handle(buffers[i]);
      if (!vb) {
         unsupported(cmd, "VK_EXT_transform_feedback: null XFB buffer");
         return;
      }
      cmd->tf.buf[b] = buffers[i];
      cmd->tf.off[b] = offsets ? offsets[i] : 0;
      cmd->tf.size[b] = sizes ? sizes[i] : 0;
      if (b + 1 > cmd->tf.nbufs)
         cmd->tf.nbufs = b + 1;
      /* the vertex stage stores the transformed vertices here: the fence wait for this submission must MEM_SYNC
       * it device->CPU (the dump/readback path in mali_cmd_submit already sweeps these marks). */
      mali_cmd_mark_buffer_written(cmd, buffers[i]);
   }
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdBeginTransformFeedbackEXT(VkCommandBuffer h, uint32_t first_counter_buffer, uint32_t counter_buffer_count,
                                  const VkBuffer *counter_buffers, const VkDeviceSize *counter_buffer_offsets)
{
   CMD(h);
   if (cmd->in_render) {
      unsupported(cmd, "vkCmdBeginTransformFeedbackEXT inside a render pass");
      return;
   }
   if (cmd->tf.active) {
      unsupported(cmd, "vkCmdBeginTransformFeedbackEXT while transform feedback is active");
      return;
   }
   if (counter_buffers && first_counter_buffer + counter_buffer_count > 4) {
      unsupported(cmd, "VK_EXT_transform_feedback: more than 4 counter buffers");
      return;
   }
   if (counter_buffers && counter_buffer_count) {
      /* The captured byte position is not saved (mali_compile.c has no counter write), so a counter would stay
       * stale and a pause/resume would overwrite records. Refuse loudly instead of marking it written
       * (CodeRabbit on #313, vk/src/mali_cmd.c:915/:941). */
      unsupported(cmd, "VK_EXT_transform_feedback: counter buffers are not implemented");
      return;
   }
   cmd->tf.active = true;
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdEndTransformFeedbackEXT(VkCommandBuffer h, uint32_t first_counter_buffer, uint32_t counter_buffer_count,
                                const VkBuffer *counter_buffers, const VkDeviceSize *counter_buffer_offsets)
{
   CMD(h);
   if (!cmd->tf.active) {
      unsupported(cmd, "vkCmdEndTransformFeedbackEXT without a matching Begin");
      return;
   }
   if (counter_buffers && counter_buffer_count) {
      unsupported(cmd, "VK_EXT_transform_feedback: counter buffers are not implemented");
      return;
   }
   cmd->tf.active = false;
}

/* ---- rendering ---- */
void
mali_cmd_push_frame(struct mali_cmd_buffer *cmd, const struct mali_frame *f)
{
   if (cmd->nframes == cmd->capframes) {
      unsigned nc = cmd->capframes ? cmd->capframes * 2 : 4;
      struct mali_frame *nf = realloc(cmd->frames, nc * sizeof *nf);
      if (!nf) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return;
      }
      cmd->frames = nf;
      cmd->capframes = nc;
   }
   cmd->frames[cmd->nframes++] = *f;
}

/* A meta frame recorded while cmd->in_render is set: keep it until the pass' own frame is pushed, then run it after
 * the pass. Vulkan orders commands inside a render pass in submission order with the rendering, and the CS ring is
 * in-order, so a compute job after the pass' frame is the definition of "after the pass". A failure only happens on
 * OOM (sticky error), never because of the pass. */
void
mali_cmd_push_frame_deferred(struct mali_cmd_buffer *cmd, const struct mali_frame *f)
{
   if (!cmd->in_render) {
      mali_cmd_push_frame(cmd, f);
      return;
   }
   if (cmd->ndef_frames == cmd->capdef_frames) {
      unsigned nc = cmd->capdef_frames ? cmd->capdef_frames * 2 : 4;
      struct mali_frame *nf = realloc(cmd->def_frames, nc * sizeof *nf);
      if (!nf) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return;
      }
      cmd->def_frames = nf;
      cmd->capdef_frames = nc;
   }
   cmd->def_frames[cmd->ndef_frames++] = *f;
}

/* colour loadOp LOAD of the RAW-tile-buffer formats (integer, SNORM, UNORM16, float32): preload.frag kinds 12..18 (kind | components << 12), bytes per texel */
static bool
raw_preload_kind(int fmt, unsigned *kind, unsigned *bpp, unsigned *nc)
{
   static const struct { int f; uint8_t k, bpp, nc; } t[] = {
      { VK_FORMAT_R8_UINT, 12, 1, 1 }, { VK_FORMAT_R8G8_UINT, 12, 2, 2 }, { VK_FORMAT_R8G8B8A8_UINT, 12, 4, 4 }, { VK_FORMAT_A8B8G8R8_UINT_PACK32, 12, 4, 4 },
      { VK_FORMAT_R16_UINT, 12, 2, 1 }, { VK_FORMAT_R16G16_UINT, 12, 4, 2 }, { VK_FORMAT_R16G16B16A16_UINT, 12, 8, 4 },
      { VK_FORMAT_R32_UINT, 12, 4, 1 }, { VK_FORMAT_R32G32_UINT, 12, 8, 2 }, { VK_FORMAT_R32G32B32A32_UINT, 12, 16, 4 },
      { VK_FORMAT_R8_SINT, 13, 1, 1 }, { VK_FORMAT_R8G8_SINT, 13, 2, 2 }, { VK_FORMAT_R8G8B8A8_SINT, 13, 4, 4 }, { VK_FORMAT_A8B8G8R8_SINT_PACK32, 13, 4, 4 },
      { VK_FORMAT_R16_SINT, 13, 2, 1 }, { VK_FORMAT_R16G16_SINT, 13, 4, 2 }, { VK_FORMAT_R16G16B16A16_SINT, 13, 8, 4 },
      { VK_FORMAT_R32_SINT, 13, 4, 1 }, { VK_FORMAT_R32G32_SINT, 13, 8, 2 }, { VK_FORMAT_R32G32B32A32_SINT, 13, 16, 4 },
      { VK_FORMAT_R8_SNORM, 14, 1, 1 }, { VK_FORMAT_R8G8_SNORM, 14, 2, 2 }, { VK_FORMAT_R8G8B8A8_SNORM, 14, 4, 4 }, { VK_FORMAT_A8B8G8R8_SNORM_PACK32, 14, 4, 4 },
      { VK_FORMAT_R16_SNORM, 14, 2, 1 }, { VK_FORMAT_R16G16_SNORM, 14, 4, 2 }, { VK_FORMAT_R16G16B16A16_SNORM, 14, 8, 4 },
      { VK_FORMAT_R16_UNORM, 15, 2, 1 }, { VK_FORMAT_R16G16_UNORM, 15, 4, 2 }, { VK_FORMAT_R16G16B16A16_UNORM, 15, 8, 4 },
      { VK_FORMAT_R32_SFLOAT, 16, 4, 1 }, { VK_FORMAT_R32G32_SFLOAT, 16, 8, 2 }, { VK_FORMAT_R32G32B32A32_SFLOAT, 16, 16, 4 },
      { VK_FORMAT_A2B10G10R10_UINT_PACK32, 17, 4, 4 }, { VK_FORMAT_A2B10G10R10_SINT_PACK32, 18, 4, 4 },
   };
   for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
      if (t[i].f == fmt) {
         *kind = t[i].k;
         *bpp = t[i].bpp;
         *nc = t[i].nc;
         return true;
      }
   return false;
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdBeginRendering(VkCommandBuffer h, const VkRenderingInfo *ri)
{
   CMD(h);
   struct mali_frame *f = &cmd->cur;
   struct mali_cmd_target t;
   memset(&t, 0, sizeof t);

   if (cmd->in_render || !g_res.image_view) {
      unsupported(cmd, "nested rendering / no image resolver");
      return;
   }
   memset(f, 0, sizeof *f);
   f->area[0] = ri->renderArea.offset.x;
   f->area[1] = ri->renderArea.offset.y;
   f->area[2] = (int32_t)ri->renderArea.extent.width;
   f->area[3] = (int32_t)ri->renderArea.extent.height;
   /* Count the non-null colour views only: a legal frame may name N slots and leave views null (zink always emits N),
    * and must not lose the whole command buffer for it. We render the first non-null one and drop any others with one
    * log line (real MRT is #358), never a sticky error. */
   bool no_col = true;
   uint32_t c0 = 0, ncol = 0;
   for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) {
      if (!ri->pColorAttachments[i].imageView)
         continue;
      if (no_col)
         c0 = i;
      no_col = false;
      ncol++;
   }
   if (ncol > 1 || (ncol == 1 && c0 != 0))
      degrade(cmd, "several colour attachment slots (rendering the first non-null one)");
   f->colour_slot = no_col ? 0u : c0;   /* CmdClearAttachments must target this slot, and only this one */
   const VkRenderingAttachmentInfo *ca = no_col ? NULL : &ri->pColorAttachments[c0];
   bool load = false;
   struct mali_preload_src psrc;
   memset(&psrc, 0, sizeof psrc);
   if (no_col) {
      f->width = (uint32_t)(f->area[0] + f->area[2]);
      f->height = (uint32_t)(f->area[1] + f->area[3]);
      f->samples = 1;
   }
   int ivr = no_col ? 0 : g_res.image_view(ca->imageView, &t);
   if (!no_col && (ivr || t.is_zs)) {
      if (getenv("MALI_PIPE_DBG"))
         fprintf(stderr, "mali_cmd: colour attachment view: resolver rc=%d is_zs=%d\n", ivr, t.is_zs);
      unsupported(cmd, "colour attachment view");
      return;
   }
   /* M15: the colour target is GPU-written in this frame */
   if (!no_col && g_res.image_view_mem) {
      mali_cmd_mark_written(cmd, g_res.image_view_mem(ca->imageView));
      /* Implicit sync, producer side, fallback rule: the frame's final store into a shared colour attachment is a
       * handoff even without a foreign-queue release barrier (mali_isync.h). */
      mali_cmd_mark_handoff(cmd, g_res.image_view_mem(ca->imageView));
   }
   if (!no_col)
      load = is_load_op(ca->loadOp);
   if (load) {
      /* LOAD = a preload draw reading the old contents (linear image) through a storage buffer; any render-target format */
      int fmt = t.rt ? t.rt->vk_format : VK_FORMAT_R8G8B8A8_UNORM;
      unsigned kind = 0, bpp = 4, srgb = 0;
      switch (fmt) {
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_A8B8G8R8_UNORM_PACK32: break;   /* same bytes as R8G8B8A8_UNORM */
      case VK_FORMAT_R8G8B8A8_SRGB:
      case VK_FORMAT_A8B8G8R8_SRGB_PACK32: srgb = 1; break;   /* same bytes as R8G8B8A8_SRGB */
      case VK_FORMAT_B8G8R8A8_UNORM: kind = 1; break;
      case VK_FORMAT_B8G8R8A8_SRGB: kind = 1; srgb = 1; break;
      case VK_FORMAT_R8_UNORM: bpp = 1; break;
      case VK_FORMAT_R8_SRGB: bpp = 1; srgb = 1; break;
      case VK_FORMAT_R8G8_UNORM: bpp = 2; break;
      case VK_FORMAT_R8G8_SRGB: bpp = 2; srgb = 1; break;
      case VK_FORMAT_R16_SFLOAT: kind = 2; bpp = 2; break;
      case VK_FORMAT_R16G16_SFLOAT: kind = 2; bpp = 4; break;
      case VK_FORMAT_R16G16B16A16_SFLOAT: kind = 2; bpp = 8; break;
      case VK_FORMAT_B10G11R11_UFLOAT_PACK32: kind = 3; break;
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32: kind = 4; break;
      case VK_FORMAT_A2R10G10B10_UNORM_PACK32: kind = 5; break;
      case VK_FORMAT_R5G6B5_UNORM_PACK16: kind = 6; bpp = 2; break;
      case VK_FORMAT_B5G6R5_UNORM_PACK16: kind = 7; bpp = 2; break;
      case VK_FORMAT_R4G4B4A4_UNORM_PACK16: kind = 8; bpp = 2; break;
      case VK_FORMAT_B4G4R4A4_UNORM_PACK16: kind = 9; bpp = 2; break;
      case VK_FORMAT_R5G5B5A1_UNORM_PACK16: kind = 10; bpp = 2; break;
      case VK_FORMAT_B5G5R5A1_UNORM_PACK16: kind = 11; bpp = 2; break;
      case VK_FORMAT_A1R5G5B5_UNORM_PACK16: kind = 10 | (1u << 18); bpp = 2; break;   /* A1R5G5B5: kind 10 with the alpha-rotate flag (preload.frag) */
      default: {
         unsigned rk, rbpp, rnc;
         if (!raw_preload_kind(fmt, &rk, &rbpp, &rnc)) {
            char why[128];
            snprintf(why, sizeof why, "colour loadOp LOAD of %s", vk_Format_to_str(fmt));
            unsupported(cmd, why);
            return;
         }
         kind = rk | (rnc << 12) | ((rk == 12 || rk == 17) ? 1u << 16 : (rk == 13 || rk == 18) ? 2u << 16 : 0);   /* bits 16..17: integer-typed colour output (uint / sint) */
         bpp = rbpp;
      }
      }
      psrc.arg = kind | (srgb ? 0x100u : 0u);
      psrc.bpp = bpp;
   }
   if (!no_col) {
   f->width = t.width;
   f->height = t.height;
   f->has_colour = true;
   psrc.va[0] = t.va;
   psrc.row[0] = t.row_stride;
   /* The preload reads its source as a storage buffer (pack_buffer(set, va, bytes), mali_meta.c:664) and the pipeline
    * has robustBufferAccess, so any read past `bytes` is clamped to zero. A 4x image is `samples` consecutive sample
    * planes (surface_stride each): the range must cover all of them, or planes 1..3 read as transparent black and the
    * resolve averages 1 of 4 samples (alpha 63 for every covered pixel). That was the LOAD histogram: alpha flattened
    * to a1_63 and 64 edge pixels at 0. */
   psrc.bytes[0] = t.row_stride * t.height * (t.samples > 1 ? t.samples : 1);
   f->colour.va = t.va;
   f->samples = t.samples > 1 ? t.samples : 1;
   f->colour.surface_stride = t.surface_stride;
   f->colour.rt = t.rt;
   f->colour.row_stride = t.row_stride;
   f->colour.clear = ca->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
   f->colour.store = ca->storeOp == VK_ATTACHMENT_STORE_OP_STORE;
   if (f->colour.clear)
      memcpy(f->colour.clear_color, ca->clearValue.color.float32, sizeof f->colour.clear_color);
   if (f->samples == 4) {
      /* MSAA resolve. Two designs, because the hardware writes the average OR the samples, never both:
       *  - storeOp DONT_CARE: the "Average" writeback resolves straight into the single-sample image, one pass,
       *    no sample data ever reaches memory (f->colour.resolve_va, mali_render.c:238-243);
       *  - storeOp STORE: the samples are written back (Layered, surface_stride) and the resolve runs as a
       *    separate pass after this frame (f->resolve_src/dst, mali_CmdEndRendering -> mali_CmdResolveImage2),
       *    which is what Vulkan requires: the attachment keeps its samples AND the resolve image is written. */
      if (ca->resolveMode != VK_RESOLVE_MODE_NONE) {
         struct mali_cmd_target rt;
         if (ca->resolveMode != VK_RESOLVE_MODE_AVERAGE_BIT || !ca->resolveImageView || g_res.image_view(ca->resolveImageView, &rt) ||
             rt.zs_format || rt.samples != 1 || rt.width != t.width || rt.height != t.height || rt.rt != t.rt) {
            unsupported(cmd, "MSAA colour resolve (AVERAGE into a matching single-sample view only)");
            return;
         }
         if (f->colour.store) {
            struct mali_image_view *cv = mali_image_view_from_handle(ca->imageView);
            struct mali_image_view *rv = mali_image_view_from_handle(ca->resolveImageView);
            if (!cv || !rv || !cv->vk.image || !rv->vk.image) {
               unsupported(cmd, "MSAA colour resolve (no image for the attachment or the resolve target)");
               return;
            }
            f->resolve_src = (uint64_t)(uintptr_t)cv->vk.image;
            f->resolve_dst = (uint64_t)(uintptr_t)rv->vk.image;
            f->resolve_level = cv->vk.base_mip_level;
            f->resolve_layer = cv->vk.base_array_layer;
            f->resolve_dst_level = rv->vk.base_mip_level;   /* the resolve image is its own view: mip/layer may differ */
            f->resolve_dst_layer = rv->vk.base_array_layer;
         } else {
            f->colour.resolve_va = rt.va;
            f->colour.resolve_row_stride = rt.row_stride;
         }
      }
      if (load) {
         /* per-sample preload: the fragment shader runs once per sample (gl_SampleID) and reads that sample's
          * plane of the 4x image, so the tile gets the exact old sample data (mali_meta_preload MSAA kind). */
         psrc.samples = 4;
         psrc.sample_stride = t.surface_stride;
      }
   } else if (ca->resolveMode != VK_RESOLVE_MODE_NONE) {
      struct mali_image_view *cv = mali_image_view_from_handle(ca->imageView);
      struct mali_image_view *rv = mali_image_view_from_handle(ca->resolveImageView);
      if (f->samples != 1 || ca->resolveMode != VK_RESOLVE_MODE_AVERAGE_BIT ||
          !cv || !rv || !cv->vk.image || !rv->vk.image || ri->layerCount != 1 || ri->viewMask ||
          cv->vk.base_mip_level >= cv->vk.image->mip_levels ||
          rv->vk.base_mip_level >= rv->vk.image->mip_levels) {
         unsupported(cmd, "resolve of a single-sampled attachment");
         return;
      }
      unsigned vb = util_format_get_blocksize(vk_format_to_pipe_format(rv->vk.view_format));
      unsigned ib = util_format_get_blocksize(vk_format_to_pipe_format(rv->vk.image->format));
      if (vb && ib && vb != ib) {
         /* An incompatible view cannot safely address the underlying image. */
      } else {
         f->resolve_src = (uint64_t)(uintptr_t)cv->vk.image;
         f->resolve_dst = (uint64_t)(uintptr_t)rv->vk.image;
         f->resolve_level = cv->vk.base_mip_level;
         f->resolve_layer = cv->vk.base_array_layer;
         f->resolve_dst_level = rv->vk.base_mip_level;
         f->resolve_dst_layer = rv->vk.base_array_layer;
         f->resolve_src_format = cv->vk.view_format;
         f->resolve_dst_format = rv->vk.view_format;
         f->resolve_1x = true;
         f->colour.store = true;
      }
   }
   }
   struct mali_preload_src zsrc;
   memset(&zsrc, 0, sizeof zsrc);
   bool zload = false, sload = false;
   if (ri->pDepthAttachment && ri->pDepthAttachment->imageView) {
      const VkRenderingAttachmentInfo *da = ri->pDepthAttachment;
      /* M15: GPU-written (late depth/stencil tests) */
      if (g_res.image_view_mem)
         mali_cmd_mark_written(cmd, g_res.image_view_mem(da->imageView));
      if (g_res.image_view(da->imageView, &t) || !t.zs_format) {
         unsupported(cmd, "depth attachment view");
         return;
      }
      if (is_load_op(da->loadOp)) {
         if (t.zs_format != 1 && t.zs_format != 2 && t.zs_format != 3) {
            char why[160];
            snprintf(why, sizeof why, "depth loadOp LOAD of %s (%u samples), supported: D32_SFLOAT, D24_UNORM_S8_UINT, "
                     "D16_UNORM",
                     t.zs_format == 1 ? "D32_SFLOAT" : t.zs_format == 2 ? "D24_UNORM_S8_UINT" :
                     t.zs_format == 3 ? "D16_UNORM" : "an unknown depth format", t.samples);
            unsupported(cmd, why);
            return;
         }
         zload = true;
         zsrc.va[0] = t.va;
         zsrc.row[0] = t.row_stride;
         /* every sample plane, not just sample 0: the per-sample variant reads gl_SampleID * surface_stride on */
          zsrc.bytes[0] = t.samples > 1 ? t.surface_stride * t.samples : t.row_stride * t.height;
         zsrc.arg = t.zs_format == 3 ? 1 : t.zs_format == 2 ? 2 : 0;   /* preload_zs.frag: 0 D32F, 1 D16, 2 D24 */
         /* multisampled: the per-sample variant reads sample gl_SampleID's plane (surface_stride bytes further on) */
         zsrc.samples = t.samples > 1 ? t.samples : 0;
         zsrc.sample_stride = t.samples > 1 ? t.surface_stride : 0;
      }
      f->has_zs = true;
      if (no_col) {
         f->width = t.width;
         f->height = t.height;
          f->samples = t.samples > 1 ? t.samples : 1;
      }
      if (da->resolveMode != VK_RESOLVE_MODE_NONE || (t.samples > 1) != (f->samples == 4)) {
         unsupported(cmd, "depth resolve / depth sample count differs from the colour attachment");
         return;
      }
      f->zs.va = t.va;
      f->zs.surface_stride = t.surface_stride;
      f->zs.row_stride = t.row_stride;
      f->zs.zs_format = t.zs_format;
      f->zs.clear = da->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
      f->zs.clear_depth = da->clearValue.depthStencil.depth;
      f->zs.store = da->storeOp == VK_ATTACHMENT_STORE_OP_STORE;
   }
   if (ri->pStencilAttachment && ri->pStencilAttachment->imageView) {
      if (g_res.image_view_mem)
         mali_cmd_mark_written(cmd, g_res.image_view_mem(ri->pStencilAttachment->imageView));
      const VkRenderingAttachmentInfo *sa = ri->pStencilAttachment;
      if (g_res.image_view(sa->imageView, &t) || !t.has_s) {
         unsupported(cmd, "stencil attachment view");
         return;
      }
      if (is_load_op(sa->loadOp)) {
         sload = true;
         zsrc.va[1] = t.s_va;
         zsrc.row[1] = t.s_row_stride;
          zsrc.bytes[1] = t.samples > 1 ? t.s_surface_stride * t.samples : t.s_row_stride * t.height;
         /* multisampled (the zink/CEF shape): each sample reads its own S8 plane before the pass' draws stencil-test
          * against it. Preloaded per sample like the multisampled colour LOAD; nothing here is sample-0 only. */
         zsrc.samples = t.samples > 1 ? t.samples : 0;
          zsrc.stencil_sample_stride = t.samples > 1 ? t.s_surface_stride : 0;
      }
      f->has_st = true;
      if (no_col) {
         f->width = t.width;
         f->height = t.height;
         if (!f->has_zs)
            f->samples = t.samples > 1 ? t.samples : 1;
      }
      if ((t.samples > 1) != (f->samples == 4) || sa->resolveMode != VK_RESOLVE_MODE_NONE) {
         unsupported(cmd, "stencil resolve / stencil sample count differs from other attachment");
         return;
      }
      f->st.va = t.s_va;
      f->st.row_stride = t.s_row_stride;
      f->st.surface_stride = t.s_surface_stride;
      f->st.clear = sa->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
      f->st.clear_stencil = (uint8_t)sa->clearValue.depthStencil.stencil;
      f->st.store = sa->storeOp == VK_ATTACHMENT_STORE_OP_STORE;
   }
   struct { bool on; enum mali_preload_kind kind; const struct mali_preload_src *src; const char *what; } pre[2] = {
      { load, MALI_PRELOAD_COLOUR, &psrc, "colour loadOp LOAD (preload shader)" },
      { zload || sload, zload && sload ? MALI_PRELOAD_ZS : zload ? MALI_PRELOAD_Z : MALI_PRELOAD_S, &zsrc, "depth/stencil loadOp LOAD (preload shader)" },
   };
   if (sload && !zload)
      zsrc.va[0] = zsrc.va[1], zsrc.bytes[0] = zsrc.bytes[1], zsrc.row[0] = zsrc.row[1];
   for (int pi = 0; pi < 2; pi++) {
      if (!pre[pi].on)
         continue;
      struct mali_draw d;
      if (mali_meta_vk_preload(cmd, pre[pi].kind, pre[pi].src, &d)) {
         unsupported(cmd, pre[pi].what);
         return;
      }
      d.dirty = MALI_D_ALL;
      for (int i = 0; i < 4; i++) d.sc[i] = f->area[i];
      d.vp[0] = (float)f->area[0]; d.vp[1] = (float)f->area[1]; d.vp[2] = (float)f->area[2]; d.vp[3] = (float)f->area[3]; d.vp[5] = 1.0f;
      if (mali_frame_add_draw(f, &d))
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   cmd->dirty = MALI_D_ALL;   /* the preload draws above are not snapshots of the bound state */
   cmd->in_render = true;
}

/* Region of the deferred MSAA resolve (mali_CmdEndRendering). Both sides keep their own subresource: the source is the
 * attachment view (level/layer recorded from it), the destination the resolve view, which may be mip 1 / layer 1 of a
 * larger image. Vulkan resolves the render area only, so the rect is that area — a whole-attachment rect would overwrite
 * the pixels outside it, which the app expects to keep. area[] zero (no render area recorded) = the whole attachment,
 * as elsewhere (mali_soft.c, mali_render.c). */
void
mali_resolve_region(const struct mali_frame *f, VkImageResolve2 *r)
{
   const int32_t *ar = f->area;
   const uint32_t x = ar[2] ? (uint32_t)ar[0] : 0, y = ar[3] ? (uint32_t)ar[1] : 0;
   memset(r, 0, sizeof *r);
   r->sType = VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2;
   r->srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, f->resolve_level, f->resolve_layer, 1};
   r->dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, f->resolve_dst_level, f->resolve_dst_layer, 1};
   r->srcOffset = (VkOffset3D){(int32_t)x, (int32_t)y, 0};
   r->dstOffset = r->srcOffset;
   r->extent = (VkExtent3D){ar[2] ? (uint32_t)ar[2] : f->width, ar[3] ? (uint32_t)ar[3] : f->height, 1};
}

bool
mali_resolve_1x_clamp(VkImageResolve2 *r, const int32_t area[4],
                      uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
   int64_t x = MAX2(area[0], 0), y = MAX2(area[1], 0);
   int64_t ex = MIN2((int64_t)area[0] + MAX2(area[2], 0), MIN2(sw, dw));
   int64_t ey = MIN2((int64_t)area[1] + MAX2(area[3], 0), MIN2(sh, dh));
   if (ex <= x || ey <= y)
      return false;
   r->srcOffset.x = r->dstOffset.x = (int32_t)x;
   r->srcOffset.y = r->dstOffset.y = (int32_t)y;
   r->extent.width = (uint32_t)(ex - x);
   r->extent.height = (uint32_t)(ey - y);
   return true;
}

void
mali_resolve_1x_slices(VkImageResolve2 *r, bool src3d, bool dst3d)
{
   if (src3d) {
      r->srcOffset.z = r->srcSubresource.baseArrayLayer;
      r->srcSubresource.baseArrayLayer = 0;
   }
   if (dst3d) {
      r->dstOffset.z = r->dstSubresource.baseArrayLayer;
      r->dstSubresource.baseArrayLayer = 0;
   }
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdEndRendering(VkCommandBuffer h)
{
   CMD(h);
   if (!cmd->in_render) {
      unsupported(cmd, "EndRendering without a valid Begin");
      mali_frame_finish(&cmd->cur);
      memset(&cmd->cur, 0, sizeof cmd->cur);
      return;
   }
   const uint64_t resolve_src = cmd->cur.resolve_src, resolve_dst = cmd->cur.resolve_dst;
   const bool resolve_1x = cmd->cur.resolve_1x;
   const VkFormat src_format = cmd->cur.resolve_src_format, dst_format = cmd->cur.resolve_dst_format;
   VkImageResolve2 rr;
   mali_resolve_region(&cmd->cur, &rr);
   int32_t area[4];
   memcpy(area, cmd->cur.area, sizeof area);
   push_frame(cmd, &cmd->cur);
   memset(&cmd->cur, 0, sizeof cmd->cur);   /* the draw list now belongs to the finished frame */
   cmd->in_render = false;
   if (resolve_src && resolve_dst && resolve_1x) {
      struct mali_image *src = (struct mali_image *)(uintptr_t)resolve_src;
      struct mali_image *dst = (struct mali_image *)(uintptr_t)resolve_dst;
      uint32_t sw = u_minify(src->vk.extent.width, rr.srcSubresource.mipLevel);
      uint32_t sh = u_minify(src->vk.extent.height, rr.srcSubresource.mipLevel);
      uint32_t dw = u_minify(dst->vk.extent.width, rr.dstSubresource.mipLevel);
      uint32_t dh = u_minify(dst->vk.extent.height, rr.dstSubresource.mipLevel);
      if (mali_resolve_1x_clamp(&rr, area, sw, sh, dw, dh)) {
         mali_resolve_1x_slices(&rr, src->vk.image_type == VK_IMAGE_TYPE_3D, dst->vk.image_type == VK_IMAGE_TYPE_3D);
         if (src_format == dst_format) {
            VkImageCopy2 r = {.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
               .srcSubresource = rr.srcSubresource, .srcOffset = rr.srcOffset,
               .dstSubresource = rr.dstSubresource, .dstOffset = rr.dstOffset, .extent = rr.extent};
            VkCopyImageInfo2 ci = {.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
               .srcImage = (VkImage)(uintptr_t)resolve_src, .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
               .dstImage = (VkImage)(uintptr_t)resolve_dst, .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
               .regionCount = 1, .pRegions = &r};
            mali_CmdCopyImage2(h, &ci);
         } else {
            VkOffset3D send = {rr.srcOffset.x + (int32_t)rr.extent.width,
                               rr.srcOffset.y + (int32_t)rr.extent.height, rr.srcOffset.z + 1};
            VkOffset3D dend = {rr.dstOffset.x + (int32_t)rr.extent.width,
                               rr.dstOffset.y + (int32_t)rr.extent.height, rr.dstOffset.z + 1};
            VkImageBlit2 r = {.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
               .srcSubresource = rr.srcSubresource, .srcOffsets = {rr.srcOffset, send},
               .dstSubresource = rr.dstSubresource, .dstOffsets = {rr.dstOffset, dend}};
            VkBlitImageInfo2 bi = {.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
               .srcImage = (VkImage)(uintptr_t)resolve_src, .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
               .dstImage = (VkImage)(uintptr_t)resolve_dst, .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
               .regionCount = 1, .pRegions = &r, .filter = VK_FILTER_NEAREST};
            mali_meta_blit_views(h, &bi, src_format, dst_format);
         }
      }
   } else if (resolve_src && resolve_dst) {
      /* MSAA storeOp STORE with a resolve: the samples are in memory now, resolve them in their own pass (the
       * hardware cannot write the average and the samples in one go). mali_CmdResolveImage2 plans a compute job
       * that is appended after the pass frame, which is "after the pass" on the in-order CS ring. */
      mali_resolve_region(&cmd->cur, &rr);
      VkResolveImageInfo2 ri = {.sType = VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2,
                                .srcImage = (VkImage)(uintptr_t)resolve_src,
                                .srcImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                .dstImage = (VkImage)(uintptr_t)resolve_dst,
                                .dstImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                .regionCount = 1, .pRegions = &rr};
      mali_CmdResolveImage2(h, &ri);
   }
   if (cmd->ndef) {                         /* M13: ops recorded inside the pass retire after it */
      struct mali_frame o;
      memset(&o, 0, sizeof o);
      o.ops = cmd->def_ops;
      o.nops = cmd->ndef;
      cmd->def_ops = NULL;
      cmd->ndef = cmd->capdef = 0;
      push_frame(cmd, &o);
   }
   for (unsigned i = 0; i < cmd->ndef_frames; i++) {   /* meta work recorded in the pass: after the pass, in order */
      push_frame(cmd, &cmd->def_frames[i]);
      mali_frame_finish(&cmd->def_frames[i]);
   }
   free(cmd->def_frames);
   cmd->def_frames = NULL;
   cmd->ndef_frames = cmd->capdef_frames = 0;
}

/* ---- M13 hooks ---- */
void
mali_cmd_push_ops(struct mali_cmd_buffer *cmd, const struct mali_op *ops, unsigned n)
{
   if (!n)
      return;
   if (cmd->in_render) {
      if (cmd->ndef + n > cmd->capdef) {
         unsigned nc = cmd->capdef ? cmd->capdef * 2 : 8;
         while (nc < cmd->ndef + n)
            nc *= 2;
         struct mali_op *no = realloc(cmd->def_ops, nc * sizeof *no);
         if (!no) {
            vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
         }
         cmd->def_ops = no;
         cmd->capdef = nc;
      }
      memcpy(cmd->def_ops + cmd->ndef, ops, n * sizeof *ops);
      cmd->ndef += n;
      return;
   }
   struct mali_frame f;
   memset(&f, 0, sizeof f);
   f.ops = malloc(n * sizeof *ops);
   if (!f.ops) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   memcpy(f.ops, ops, n * sizeof *ops);
   f.nops = n;
   push_frame(cmd, &f);
}

void
mali_cmd_set_occlusion(struct mali_cmd_buffer *cmd, uint64_t va, unsigned mode)
{
   cmd->occ_va = va;
   cmd->occ_mode = mode;
}

/* ---- clears ---- */
VKAPI_ATTR void VKAPI_CALL
mali_CmdClearAttachments(VkCommandBuffer h, uint32_t na, const VkClearAttachment *att, uint32_t nr, const VkClearRect *rects)
{
   CMD(h);
   struct mali_frame *f = &cmd->cur;
   if (!cmd->in_render) {
      unsupported(cmd, "CmdClearAttachments outside a rendering");
      return;
   }
   bool partial = false;
   for (uint32_t r = 0; r < nr; r++)
      if (rects[r].rect.offset.x != f->area[0] || rects[r].rect.offset.y != f->area[1] ||
          (int32_t)rects[r].rect.extent.width != f->area[2] || (int32_t)rects[r].rect.extent.height != f->area[3])
         partial = true;
   if (partial || f->ndraws) {
      /* after a draw, or a sub-rectangle: a constant-colour quad (preload.frag kind 19: the clear words are the format's raw shader output) per rect.
       * Single-sampled, layer 0; whole-area depth/stencil clears before the first draw keep the loadOp-patch path below. */
      /* depth/stencil: fixed function, no shader read. The constant-colour quad with colour writes masked; the depth value is the viewport
       * min=max depth (depth test ALWAYS + write, clip off), the stencil value a REPLACE-always reference. */
      for (uint32_t i = 0; i < na; i++) {
         VkImageAspectFlags a = att[i].aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
         if (!a)
            continue;
         bool zc = a & VK_IMAGE_ASPECT_DEPTH_BIT, sc = a & VK_IMAGE_ASPECT_STENCIL_BIT;
         if ((zc && !f->has_zs) || (sc && !f->has_st)) {
            /* only the aspect without a backing view has no effect: drop it and clear the other one */
            degrade(cmd, "CmdClearAttachments of a depth/stencil aspect this pass does not have");
            zc = zc && f->has_zs;
            sc = sc && f->has_st;
            if (!zc && !sc)
               continue;
         }
         if (f->samples != 1) {
            /* the depth/stencil quad writes one sample per fragment; at 4x that is not the clear Vulkan asks for */
            degrade(cmd, "CmdClearAttachments depth/stencil inside a multisampled pass");
            continue;
         }
         for (uint32_t r = 0; r < nr; r++) {
            if (rects[r].baseArrayLayer != 0 || rects[r].layerCount != 1) {
               unsupported(cmd, "CmdClearAttachments depth/stencil quad: layers");
               return;
            }
            struct mali_preload_src ps;
            memset(&ps, 0, sizeof ps);
            ps.arg = 19;
            struct mali_draw d;
            if (mali_meta_vk_preload(cmd, MALI_PRELOAD_COLOUR, &ps, &d)) {
               unsupported(cmd, "CmdClearAttachments depth/stencil quad (preload shader)");
               return;
            }
            d.dirty = MALI_D_ALL;
            d.fx.set = true;
            d.fx.front_ccw = true;
            d.fx.depth_clip = false;
            d.fx.wmask = 0;
            d.fx.src_c = d.fx.src_a = 1;
            float dv = att[i].clearValue.depthStencil.depth;
            d.gfx.depth_test = zc;
            d.gfx.depth_write = zc;
            if (sc) {
               d.fx.st_test = true;
               d.fx.st_f = d.fx.st_b = (struct mali_st_face){ 7 /* ALWAYS */, 2, 2, 2 /* REPLACE */, 0xff, 0xff,
                                                              (uint8_t)att[i].clearValue.depthStencil.stencil };
            }
            d.sc[0] = rects[r].rect.offset.x; d.sc[1] = rects[r].rect.offset.y;
            d.sc[2] = (int32_t)rects[r].rect.extent.width; d.sc[3] = (int32_t)rects[r].rect.extent.height;
            d.vp[0] = (float)f->area[0]; d.vp[1] = (float)f->area[1]; d.vp[2] = (float)f->area[2]; d.vp[3] = (float)f->area[3];
            d.vp[4] = d.vp[5] = zc ? dv : 0.0f;
            if (mali_frame_add_draw(f, &d)) {
               vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
               return;
            }
         }
      }
      bool has_c = false;
      for (uint32_t i = 0; i < na; i++)
         has_c |= !!(att[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT);
      if (!has_c) {
         cmd->dirty = MALI_D_ALL;
         return;
      }
      if (!f->has_colour) {
         degrade(cmd, "CmdClearAttachments colour quad without a colour attachment");
         /* the depth/stencil quads above were appended to the frame: the register state they were built from is no longer
          * valid for whatever draws follow, so forget it */
         cmd->dirty = MALI_D_ALL;
         return;
      }
      /* Multisampled passes included: the constant-colour quad is an ordinary draw into the tile, so every sample of
       * the covered pixels gets the clear colour, which is the clear (PanVK clears through its meta shaders the same
       * way: panvk_vX_meta_clear draws the rect, it does not special-case samples). */
      for (uint32_t i = 0; i < na; i++) {
         if (!(att[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT))
            continue;
         if (att[i].colorAttachment != f->colour_slot) {
            /* a clear names an attachment index, not "the one we render": a null slot has no view (no effect) and another
             * bound slot is not rendered by this pass - clearing the rendered RT for either would write the wrong image */
            degrade(cmd, "CmdClearAttachments of a colour attachment this pass does not render");
            continue;
         }
         for (uint32_t r = 0; r < nr; r++) {
            if (rects[r].baseArrayLayer != 0 || rects[r].layerCount != 1) {
               unsupported(cmd, "CmdClearAttachments quad: layers");
               return;
            }
            struct mali_preload_src ps;
            memset(&ps, 0, sizeof ps);
            ps.arg = 19 | (f->colour.rt && f->colour.rt->int_bits ? (f->colour.rt->is_signed ? 2u : 1u) << 16 : 0);   /* the clear words are the raw shader output: integer-typed for integer formats */
            /* the quad must be evaluated once per sample, or the clear only lands on sample 0: a constant colour needs no
             * per-sample plane, so the stride stays 0 (arg 19 reads the clear words, not memory) */
            ps.samples = f->samples > 1 ? f->samples : 0;
            memcpy(ps.clear, att[i].clearValue.color.uint32, sizeof ps.clear);
            struct mali_draw d;
            if (mali_meta_vk_preload(cmd, MALI_PRELOAD_COLOUR, &ps, &d)) {
               unsupported(cmd, "CmdClearAttachments quad (preload shader)");
               return;
            }
            d.dirty = MALI_D_ALL;
            d.sc[0] = rects[r].rect.offset.x; d.sc[1] = rects[r].rect.offset.y;
            d.sc[2] = (int32_t)rects[r].rect.extent.width; d.sc[3] = (int32_t)rects[r].rect.extent.height;
            d.vp[0] = (float)f->area[0]; d.vp[1] = (float)f->area[1]; d.vp[2] = (float)f->area[2]; d.vp[3] = (float)f->area[3]; d.vp[5] = 1.0f;
            if (mali_frame_add_draw(f, &d)) {
               vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
               return;
            }
         }
      }
      cmd->dirty = MALI_D_ALL;   /* the quads are not snapshots of the bound state */
      return;
   }
   for (uint32_t i = 0; i < na; i++) {
      if (att[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
         if (att[i].colorAttachment != 0) {
            degrade(cmd, "CmdClearAttachments of colour attachment > 0 (only RT0 is rendered)");
            continue;
         }
         f->colour.clear = true;   /* a loadOp CLEAR patch of the FBD */
         memcpy(f->colour.clear_color, att[i].clearValue.color.float32, sizeof f->colour.clear_color);
      }
      if (att[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         if (!f->has_zs) {
            unsupported(cmd, "depth clear without depth attachment");
            return;
         }
         f->zs.clear = true;
         f->zs.clear_depth = att[i].clearValue.depthStencil.depth;
      }
      if (att[i].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
         if (!f->has_st) {
            unsupported(cmd, "stencil clear without stencil attachment");
            return;
         }
         f->st.clear = true;
         f->st.clear_stencil = (uint8_t)att[i].clearValue.depthStencil.stencil;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdClearColorImage(VkCommandBuffer h, VkImage image, VkImageLayout layout, const VkClearColorValue *c, uint32_t nr,
                        const VkImageSubresourceRange *ranges)
{
   CMD(h);
   struct mali_cmd_target t;
   struct mali_frame f;
   mali_cmd_mark_image_written(cmd, image);   /* M15: GPU-written by this clear */
   /* Fast path (proven in M9): a render-target clear of a whole single-subresource RGBA8 image. Everything else: M12 compute clears. */
   memset(&t, 0, sizeof t);
   if (nr != 1 || ranges[0].baseMipLevel != 0 || ranges[0].baseArrayLayer != 0 || cmd->in_render || !g_res.image ||
       g_res.image(image, &t) || t.zs_format || t.multi) {
      mali_meta_CmdClearColorImage(h, image, layout, c, nr, ranges);
      return;
   }
   /* a clear-only frame: geometry-less FBD + RUN_FRAGMENT (libkbase t_clear) */
   memset(&f, 0, sizeof f);
   f.width = t.width;
   f.height = t.height;
   f.area[2] = (int32_t)t.width;
   f.area[3] = (int32_t)t.height;
   f.has_colour = true;
   f.colour.va = t.va;
   f.colour.rt = t.rt;
   f.colour.row_stride = t.row_stride;
   f.colour.clear = true;
   f.colour.store = true;
   memcpy(f.colour.clear_color, c->float32, sizeof f.colour.clear_color);
   push_frame(cmd, &f);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdClearDepthStencilImage(VkCommandBuffer h, VkImage image, VkImageLayout layout, const VkClearDepthStencilValue *v,
                               uint32_t nr, const VkImageSubresourceRange *ranges)
{
   mali_meta_CmdClearDepthStencilImage(h, image, layout, v, nr, ranges);
}

/* ---- submit ---- */
VkResult
mali_cmd_submit(struct vk_command_buffer *vk)
{
   struct mali_cmd_buffer *cmd = mali_cmd_buffer_from_vk(vk);
   if (!g_hw_set)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   uint64_t *all = NULL;
   unsigned nall = 0, capall = 0, counted = 0;
   VkResult res = VK_SUCCESS;
   if (g_hw.job_begin)
      g_hw.job_begin(g_hw.user);   /* once per command buffer: all its frames form one submission */
   /* M15: hand the GPU-write marks to the backend before the submission that performs them, so the fence wait
    * for it CSYNCs exactly these BOs. An unplaced write (wrote_full) asks for one full sweep. */
   if (g_hw.bo_wrote) {
      bool swept = cmd->wrote_full;
      for (unsigned i = 0; i < cmd->nwrote; i++) {
         /* A mark can name memory the app freed after recording (illegal, and still must not crash): the guard holds
          * the live VkDeviceMemory registry lock, so the BO cannot go away under bo_wrote. A dead mark falls back to
          * the full sweep, which is what an unlisted write does. */
         if (!mali_memory_guard(cmd->wrote[i])) {
            swept = true;
            continue;
         }
         g_hw.bo_wrote(g_hw.user, &cmd->wrote[i]->bo);
         mali_memory_unguard();
      }
      if (swept)
         g_hw.bo_wrote(g_hw.user, NULL);
   }
   for (unsigned i = 0; i < cmd->nframes; i++) {
      struct mali_frame fr = cmd->frames[i];
      /* any_ind / any_ind_cpu / poly_cpu are accumulated at record time (mali_frame_note_rec): no second pass over the frame's records */
      bool ind = fr.any_ind, ind_cpu = fr.any_ind_cpu, poly_cpu = fr.poly_cpu;
      if ((ind_cpu || poly_cpu) && g_hw.drain) {
         /* CPU-read indirect args and polygon-mode index contents: every earlier frame (also those of this command buffer) must have retired.
          * A GPU-read indirect draw is NOT in this set: the command stream loads it after the producing job, so a drain here would serialize
          * exactly the GPU-driven case this exists for. */
         if (nall) {
            kb_wmb();
            if (g_hw.submit_multi(g_hw.user, all, nall, counted)) {
               res = VK_ERROR_DEVICE_LOST;
               break;
            }
            nall = counted = 0;
         }
         g_hw.drain(g_hw.user);
         if (getenv("MALI_PIPE_DBG"))
            fprintf(stderr, "mali_vk: drained before frame %u/%u (CPU-read indirect arguments)\n", i, cmd->nframes);
      }
      if (ind && g_hw.soft) {
         /* MALI_SOFT=1: the rasteriser rejects an indirect draw on the frame record, and expansion below would
          * erase that origin before it ever sees it. Reject here, while the record still says "indirect". */
         res = VK_ERROR_FEATURE_NOT_PRESENT;
         break;
      }
      if (ind && expand_indirect(&fr)) {
         res = VK_ERROR_OUT_OF_HOST_MEMORY;
         break;
      }
      uint64_t *cs = NULL;
      unsigned n = 0;
      int rc = mali_frame_build(&g_hw, &fr, &cs, &n);
      if (ind)
         free(fr.draws);
      if (rc) {
         if (rc == -ENOMEM)
            res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         else if (rc == -ENOTSUP || rc == -EINVAL)
            res = VK_ERROR_FEATURE_NOT_PRESENT;
         else {
            if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: frame %u/%u submit failed: %d (draws %u ops %u)\n", i, cmd->nframes, rc, cmd->frames[i].ndraws, cmd->frames[i].nops);
            res = VK_ERROR_DEVICE_LOST;
         }
         break;
      }
      if (!n) {
         /* MALI_SOFT=1: the frame was rendered on the CPU inside mali_frame_build and produced no words. Appending
          * nothing to nothing would memcpy() to a NULL destination; there is nothing for submit_multi() to do. */
         free(cs);
         continue;
      }
      if (nall + n > capall) {
         capall = (nall + n) * 2;
         all = realloc(all, capall * sizeof *all);
         if (!all) { free(cs); return VK_ERROR_OUT_OF_HOST_MEMORY; }
      }
      memcpy(all + nall, cs, n * sizeof *all);
      nall += n;
      counted++;
      free(cs);
   }
   if (res == VK_SUCCESS && nall) {
      kb_wmb();
      int rc = g_hw.submit_multi(g_hw.user, all, nall, counted);
      if (rc)
         res = VK_ERROR_DEVICE_LOST;
   } else if (res != VK_SUCCESS && counted) {
      /* frames of this command buffer were built (arena consumed) but never submitted: nothing to retire */
   }
   free(all);
   return res;
}
