/* SPDX-License-Identifier: MIT */
/* M8: command buffers (Vulkan layer over mali_cs_emit.c). See mali_cmd.h. Unsupported things do not crash: they mark the command
 * buffer with VK_ERROR_FEATURE_NOT_PRESENT (vkEndCommandBuffer returns it) and are documented in R3-PLAN.md "M8":
 * (indexed draws: implemented in M9 rest, v11 INDEX_BUFFER r54 / SIZE r39 from genxml v11, index type PRIMITIVE_FLAGS[10:8])
 *   - colour loadOp LOAD, more than one colour attachment, depth-only render passes (the FBD needs a colour RT)
 *   - vkCmdClearAttachments that does not cover the whole render area before the first draw; vkCmdClear*Image except RGBA8 colour
 *   - dynamic descriptor offsets, instance-rate vertex attributes, non-triangle-list topologies (M6 resolver decides) */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mali_cmd.h"
#include "mali_descriptor.h"
#include "mali_meta_vk.h"
#include "vk_buffer.h"
#include "vk_pipeline_layout.h"
#include "vk_alloc.h"
#include "vk_graphics_state.h"
#include "util/bitset.h"

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

static void
unsupported(struct mali_cmd_buffer *cmd, const char *what)
{
   (void)what;
   vk_command_buffer_set_error(&cmd->vk, VK_ERROR_FEATURE_NOT_PRESENT);
}

static void
reset_state(struct mali_cmd_buffer *cmd)
{
   for (unsigned i = 0; i < cmd->nframes; i++)
      mali_frame_finish(&cmd->frames[i]);
   free(cmd->frames);
   cmd->frames = NULL;
   cmd->nframes = cmd->capframes = 0;
   mali_frame_finish(&cmd->cur);
   memset(&cmd->cur, 0, sizeof cmd->cur);
   cmd->in_render = false;
   memset(&cmd->gfx, 0, sizeof cmd->gfx);
   cmd->have_gfx = false;
   cmd->cs = NULL;
   memset(cmd->push_cs, 0, sizeof cmd->push_cs);
   memset(cmd->vb_va, 0, sizeof cmd->vb_va);
   memset(cmd->vb_size, 0, sizeof cmd->vb_size);
   memset(cmd->push, 0, sizeof cmd->push);
   cmd->nsets = 0;
   cmd->ndyn = 0;
   cmd->occ_va = 0;
   cmd->occ_mode = 0;
   free(cmd->def_ops);
   cmd->def_ops = NULL;
   cmd->ndef = cmd->capdef = 0;
   mali_meta_cmd_forget(cmd);
}

/* ---- vk_command_buffer_ops ---- */
static VkResult
mali_cmd_create(struct vk_command_pool *pool, VkCommandBufferLevel level, struct vk_command_buffer **out)
{
   struct mali_cmd_buffer *cmd = vk_zalloc(&pool->alloc, sizeof *cmd, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkResult r = vk_command_buffer_init(pool, &cmd->vk, &mali_cmd_ops, level);
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
   if (!g_res.pipeline || g_res.pipeline(p, &cmd->gfx)) {
      cmd->have_gfx = false;
      unsupported(cmd, "pipeline");
      return;
   }
   if (!cmd->gfx.push_base[0])
      cmd->gfx.push_base[0] = 32;   /* compile defaults: VS push block at FAU byte 32, FS at 0 */
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
      } else if (stages[i] == MESA_SHADER_FRAGMENT) {
         cmd->gfx.fs = c;
         cmd->gfx.fs_va = va;
      } else {
         unsupported(cmd, "stage");
         return;
      }
   }
   if (!cmd->gfx.push_base[0])
      cmd->gfx.push_base[0] = 32;
   cmd->have_gfx = cmd->gfx.vs && cmd->gfx.fs;
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
            memcpy(cmd->dyn_desc[start + k], hd.w, sizeof hd.w);
            if (start + k + 1 > cmd->ndyn)
               cmd->ndyn = start + k + 1;
         }
      }
      cmd->set_va[s] = ds->va;
      cmd->set_bytes[s] = ds->bytes;
      if (s + 1 > cmd->nsets)
         cmd->nsets = s + 1;
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
      if (!buf) {
         cmd->vb_va[b] = 0;
         cmd->vb_size[b] = 0;
         continue;
      }
      cmd->vb_va[b] = vk_buffer_address(buf, offs[i]);
      cmd->vb_size[b] = (uint32_t)(buf->size - offs[i]);
   }
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdBindIndexBuffer(VkCommandBuffer h, VkBuffer b, VkDeviceSize off, VkIndexType t)
{
   CMD(h);
   struct vk_buffer *buf = vk_buffer_from_handle(b);
   if (!buf) {
      cmd->ib_va = 0;
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
   if (stages & VK_SHADER_STAGE_VERTEX_BIT)
      memcpy(cmd->push[0] + off, data, size);
   if (stages & VK_SHADER_STAGE_FRAGMENT_BIT)
      memcpy(cmd->push[1] + off, data, size);
   if (stages & VK_SHADER_STAGE_COMPUTE_BIT)
      memcpy(cmd->push_cs + off, data, size);
}

/* ---- draws ---- */
static void
snapshot_draw(struct mali_cmd_buffer *cmd, struct mali_draw *d, uint32_t count, uint32_t inst, uint32_t first, uint32_t first_inst)
{
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   const int32_t *a = cmd->cur.area;

   memset(d, 0, sizeof *d);
   d->gfx = cmd->gfx;
   memcpy(d->vb_va, cmd->vb_va, sizeof d->vb_va);
   memcpy(d->vb_size, cmd->vb_size, sizeof d->vb_size);
   memcpy(d->push, cmd->push, sizeof d->push);
   d->nsets = cmd->nsets;
   memcpy(d->set_va, cmd->set_va, sizeof d->set_va);
   memcpy(d->set_bytes, cmd->set_bytes, sizeof d->set_bytes);
   d->ndyn = cmd->ndyn;
   memcpy(d->dyn_desc, cmd->dyn_desc, sizeof d->dyn_desc);
   d->occ_va = cmd->occ_va;
   d->occ_mode = cmd->occ_mode;
   d->count = count;
   d->instance_count = inst;
   d->first_vertex = first;
   d->first_instance = first_inst;
   if (BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_VP_VIEWPORTS)) {
      const VkViewport *v = &dyn->vp.viewports[0];
      d->vp[0] = v->x; d->vp[1] = v->y; d->vp[2] = v->width; d->vp[3] = v->height; d->vp[4] = v->minDepth; d->vp[5] = v->maxDepth;
   } else {
      d->vp[0] = (float)a[0]; d->vp[1] = (float)a[1]; d->vp[2] = (float)a[2]; d->vp[3] = (float)a[3];
      d->vp[4] = 0.0f; d->vp[5] = 1.0f;
   }
   if (BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_VP_SCISSORS)) {
      const VkRect2D *s = &dyn->vp.scissors[0];
      d->sc[0] = s->offset.x; d->sc[1] = s->offset.y; d->sc[2] = (int32_t)s->extent.width; d->sc[3] = (int32_t)s->extent.height;
   } else {
      memcpy(d->sc, a, sizeof d->sc);
   }
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdDraw(VkCommandBuffer h, uint32_t vcount, uint32_t icount, uint32_t first, uint32_t first_inst)
{
   CMD(h);
   struct mali_draw d;
   if (!cmd->in_render || !cmd->have_gfx) {
      unsupported(cmd, "draw outside a rendering with a pipeline");
      return;
   }
   if (!vcount || !icount)
      return;
   snapshot_draw(cmd, &d, vcount, icount, first, first_inst);
   if (mali_frame_add_draw(&cmd->cur, &d))
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdDrawIndexed(VkCommandBuffer h, uint32_t icount, uint32_t inst, uint32_t first, int32_t voff, uint32_t first_inst)
{
   CMD(h);
   struct mali_draw d;
   if (!cmd->in_render || !cmd->have_gfx || !cmd->ib_va) {
      unsupported(cmd, "indexed draw outside a rendering, without a pipeline or index buffer");
      return;
   }
   if (!icount || !inst)
      return;
   snapshot_draw(cmd, &d, icount, inst, (uint32_t)voff, first_inst);
   d.indexed = true;
   d.index_va = cmd->ib_va;
   d.index_bytes = cmd->ib_bytes;
   d.index_size = cmd->ib_size;
   d.first_index = first;
   if (mali_frame_add_draw(&cmd->cur, &d))
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
}

#define push_frame mali_cmd_push_frame

/* ---- compute (M11): every dispatch is its own frame record, executed in order with the render passes ---- */
static void
record_dispatch(struct mali_cmd_buffer *cmd, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect_va)
{
   struct mali_frame f;
   if (cmd->in_render || !cmd->cs) {
      unsupported(cmd, "dispatch inside a rendering or without a compute pipeline");
      return;
   }
   memset(&f, 0, sizeof f);
   f.compute = true;
   f.disp.cs = cmd->cs;
   f.disp.cs_va = cmd->cs_va;
   memcpy(f.disp.push, cmd->push_cs, sizeof f.disp.push);
   f.disp.nsets = cmd->nsets;
   memcpy(f.disp.set_va, cmd->set_va, sizeof f.disp.set_va);
   memcpy(f.disp.set_bytes, cmd->set_bytes, sizeof f.disp.set_bytes);
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

VKAPI_ATTR void VKAPI_CALL
mali_CmdBeginRendering(VkCommandBuffer h, const VkRenderingInfo *ri)
{
   CMD(h);
   struct mali_frame *f = &cmd->cur;
   struct mali_cmd_target t;

   if (cmd->in_render || !g_res.image_view) {
      unsupported(cmd, "nested rendering / no image resolver");
      return;
   }
   memset(f, 0, sizeof *f);
   f->area[0] = ri->renderArea.offset.x;
   f->area[1] = ri->renderArea.offset.y;
   f->area[2] = (int32_t)ri->renderArea.extent.width;
   f->area[3] = (int32_t)ri->renderArea.extent.height;
   if (ri->colorAttachmentCount != 1 || !ri->pColorAttachments[0].imageView) {
      unsupported(cmd, "need exactly one colour attachment");
      return;
   }
   const VkRenderingAttachmentInfo *ca = &ri->pColorAttachments[0];
   if (ca->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
      unsupported(cmd, "colour loadOp LOAD");
      return;
   }
   if (g_res.image_view(ca->imageView, &t) || t.zs_format) {
      unsupported(cmd, "colour attachment view");
      return;
   }
   f->width = t.width;
   f->height = t.height;
   f->has_colour = true;
   f->colour.va = t.va;
   f->colour.row_stride = t.row_stride;
   f->colour.clear = ca->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
   f->colour.store = ca->storeOp == VK_ATTACHMENT_STORE_OP_STORE;
   if (f->colour.clear)
      memcpy(f->colour.clear_color, ca->clearValue.color.float32, sizeof f->colour.clear_color);
   if (ri->pDepthAttachment && ri->pDepthAttachment->imageView) {
      const VkRenderingAttachmentInfo *da = ri->pDepthAttachment;
      if (da->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
         unsupported(cmd, "depth loadOp LOAD");
         return;
      }
      if (g_res.image_view(da->imageView, &t) || !t.zs_format) {
         unsupported(cmd, "depth attachment view");
         return;
      }
      f->has_zs = true;
      f->zs.va = t.va;
      f->zs.row_stride = t.row_stride;
      f->zs.zs_format = t.zs_format;
      f->zs.clear = da->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
      f->zs.clear_depth = da->clearValue.depthStencil.depth;
      f->zs.store = da->storeOp == VK_ATTACHMENT_STORE_OP_STORE;
   }
   cmd->in_render = true;
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
   push_frame(cmd, &cmd->cur);
   memset(&cmd->cur, 0, sizeof cmd->cur);   /* the draw list now belongs to the finished frame */
   cmd->in_render = false;
   if (cmd->ndef) {                         /* M13: ops recorded inside the pass retire after it */
      struct mali_frame o;
      memset(&o, 0, sizeof o);
      o.ops = cmd->def_ops;
      o.nops = cmd->ndef;
      cmd->def_ops = NULL;
      cmd->ndef = cmd->capdef = 0;
      push_frame(cmd, &o);
   }
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
   if (!cmd->in_render || f->ndraws) {
      unsupported(cmd, "CmdClearAttachments after a draw (needs a quad draw)");
      return;
   }
   for (uint32_t r = 0; r < nr; r++)
      if (rects[r].rect.offset.x != f->area[0] || rects[r].rect.offset.y != f->area[1] ||
          (int32_t)rects[r].rect.extent.width != f->area[2] || (int32_t)rects[r].rect.extent.height != f->area[3]) {
         unsupported(cmd, "partial CmdClearAttachments");
         return;
      }
   for (uint32_t i = 0; i < na; i++) {
      if (att[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
         if (att[i].colorAttachment != 0) {
            unsupported(cmd, "colour attachment index");
            return;
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
   }
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdClearColorImage(VkCommandBuffer h, VkImage image, VkImageLayout layout, const VkClearColorValue *c, uint32_t nr,
                        const VkImageSubresourceRange *ranges)
{
   CMD(h);
   struct mali_cmd_target t;
   struct mali_frame f;
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
   for (unsigned i = 0; i < cmd->nframes; i++) {
      int rc = mali_frame_submit(&g_hw, &cmd->frames[i]);
      if (rc == -ENOMEM)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      if (rc == -ENOTSUP || rc == -EINVAL)
         return VK_ERROR_FEATURE_NOT_PRESENT;
      if (rc) {
         if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: frame %u/%u submit failed: %d (draws %u ops %u)\n", i, cmd->nframes, rc, cmd->frames[i].ndraws, cmd->frames[i].nops);
         return VK_ERROR_DEVICE_LOST;
      }
   }
   return VK_SUCCESS;
}
