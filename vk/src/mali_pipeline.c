/* SPDX-License-Identifier: MIT */
/* M9: graphics pipelines (VS + FS compiled by mali_compile against the pipeline layout, binaries in one executable BO), the
 * resolvers that give mali_cmd.c its pipeline / image view targets, and the small entrypoints a render-pass client needs that no
 * runtime default covers (barriers are no-ops: every submission is serialised by the queue). Shader modules, pipeline cache,
 * render pass, framebuffer: the runtime's vk_common_* (render pass 1.x is emulated through vkCmdBeginRendering, mali_cmd.c). */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "mali_cmd.h"
#include "mali_cs_emit.h"
#include "mali_entrypoints.h"
#include "mali_image.h"
#include "mali_msaa.h"
#include "mali_pipeline_layout.h"
#include "vk_alloc.h"
#include "vk_graphics_state.h"
#include "vk_pipeline_layout.h"
#include "vk_shader_module.h"
#include "util/bitscan.h"
#include "vk_util.h"

struct mali_pipeline {
   struct vk_object_base base;
   struct mali_device *dev;
   struct vk_dynamic_graphics_state dynamic;   /* R4: static state values + which states are static (bound with the pipeline) */
   struct mali_gfx_state gfx;     /* vs/fs point into this object; vs_va/fs_va into code */
   struct mali_compiled vs, fs, cs, blend;   /* cs: compute pipelines (is_compute); the binary is at code + 0 */
   bool is_compute;
   struct mali_bo code;
   bool have_code;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(mali_pipeline, base, VkPipeline, VK_OBJECT_TYPE_PIPELINE)

#define BIN_ALIGN 256u

static void
pipeline_destroy(struct mali_device *dev, struct mali_pipeline *p, const VkAllocationCallbacks *alloc)
{
   if (p->have_code)
      mali_device_pdev(dev)->backend->bo_free(dev->kdev, &p->code);
   mali_compiled_free(&p->vs);
   mali_compiled_free(&p->fs);
   mali_compiled_free(&p->cs);
   mali_compiled_free(&p->blend);
   vk_object_free(&dev->vk, alloc, p);
}

static const struct vk_shader_module *
find_module(const VkPipelineShaderStageCreateInfo *s)
{
   return vk_shader_module_from_handle(s->module);
}

static VkResult
create_one(struct mali_device *dev, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipeline *out)
{
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   const unsigned arch = pdev->gpu.arch_major >= 12 ? 13 : pdev->gpu.arch_major == 10 ? 10 : 11;
   struct mali_pipeline *p = vk_object_zalloc(&dev->vk, alloc, sizeof *p, VK_OBJECT_TYPE_PIPELINE);
   struct vk_graphics_pipeline_all_state *all = NULL;
   struct vk_graphics_pipeline_state st = {0};
   void *state_mem = NULL;
   VkResult res = VK_ERROR_OUT_OF_HOST_MEMORY;
   char err[256];

   if (!p)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   p->dev = dev;

   const VkPipelineShaderStageCreateInfo *vss = NULL, *fss = NULL;
   for (uint32_t i = 0; i < ci->stageCount; i++) {
      if (ci->pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT)
         vss = &ci->pStages[i];
      else if (ci->pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
         fss = &ci->pStages[i];
      else {
         res = VK_ERROR_FEATURE_NOT_PRESENT; if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: unsupported pipeline state at %s:%d\n", __FILE__, __LINE__);   /* tessellation / geometry: not supported */
         goto fail;
      }
   }
   if (!vss || !fss || !find_module(vss) || !find_module(fss)) {
      res = VK_ERROR_FEATURE_NOT_PRESENT; if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: unsupported pipeline state at %s:%d\n", __FILE__, __LINE__);   /* VS-only (rasterizer discard) and module-less (maintenance5) stages: later */
      goto fail;
   }

   all = vk_zalloc(&dev->vk.alloc, sizeof *all, 8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!all)
      goto fail;
   res = vk_graphics_pipeline_state_fill(&dev->vk, &st, ci, NULL, NULL, 0, all, NULL, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND, &state_mem);
   if (res != VK_SUCCESS)
      goto fail;
   if (getenv("MALI_PIPE_DBG"))
      fprintf(stderr, "mali_vk: state vi=%p ia=%p vp=%p rs=%p ms=%p ds=%p cb=%p dyn=%lx\n", (void *)st.vi, (void *)st.ia,
              (void *)st.vp, (void *)st.rs, (void *)st.ms, (void *)st.ds, (void *)st.cb, (unsigned long)st.dynamic[0]);

   struct mali_flat_layout flat;
   VK_FROM_HANDLE(vk_pipeline_layout, layout, ci->layout);
   if (layout && (res = mali_pipeline_layout_flatten(layout, &flat)) != VK_SUCCESS)
      goto fail;

   struct mali_compile_opts o = {.arch = arch, .layout = layout ? &flat.c : NULL};
   const struct vk_shader_module *m = find_module(vss);
   o.stage = MALI_STAGE_VERTEX;
   o.strip_psiz = ci->pInputAssemblyState && ci->pInputAssemblyState->topology != VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
   o.spirv = (const uint32_t *)m->data;
   o.spirv_words = m->size / 4;
   o.entry = vss->pName;
   if (mali_compile(&o, &p->vs, err, sizeof err)) {
      fprintf(stderr, "mali_vk: VS compile failed: %s\n", err);
      res = VK_ERROR_INVALID_SHADER_NV;
      goto fail;
   }
   m = find_module(fss);
   o.stage = MALI_STAGE_FRAGMENT;
   o.strip_psiz = 0;
   o.spirv = (const uint32_t *)m->data;
   o.spirv_words = m->size / 4;
   o.entry = fss->pName;
   /* MSAA (MALI_EXPERIMENTAL_MSAA): 1 or 4 samples; sampleRateShading when minSampleShading * samples > 1 */
   unsigned samples = st.ms && st.ms->rasterization_samples ? st.ms->rasterization_samples : 1;
   if (samples != 1 && (samples != 4 || !mali_experimental_msaa() || BITSET_TEST(st.dynamic, MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES))) {
      res = VK_ERROR_FEATURE_NOT_PRESENT;
      goto fail;
   }
   o.sample_shading = samples > 1 && st.ms && st.ms->sample_shading_enable && st.ms->min_sample_shading * (float)samples > 1.0f;
   o.link_vs = &p->vs.layout;
   o.clip_distances = p->vs.vs_clip_distances;
   if (mali_compile(&o, &p->fs, err, sizeof err)) {
      fprintf(stderr, "mali_vk: FS compile failed: %s\n", err);
      res = VK_ERROR_INVALID_SHADER_NV;
      goto fail;
   }

   /* dualSrcBlend: SRC1 factors (VkBlendFactor 15..18) on attachment 0 need a blend shader (fixed function cannot read SRC1) */
   bool has_blend = false;
   if (st.cb && st.cb->attachment_count > 0 && !(BITSET_TEST(st.dynamic, MESA_VK_DYNAMIC_CB_BLEND_EQUATIONS))) {
      const struct vk_color_blend_attachment_state *a = &st.cb->attachments[0];
      const unsigned f[4] = {a->src_color_blend_factor, a->dst_color_blend_factor, a->src_alpha_blend_factor, a->dst_alpha_blend_factor};
      for (unsigned i = 0; i < 4; i++)
         if (f[i] >= VK_BLEND_FACTOR_SRC1_COLOR && f[i] <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA)
            has_blend = true;
      /* equations fixed function cannot express (MIN/MAX, SRC_ALPHA_SATURATE, unsupported factor pairs) also go through the blend shader */
      if (!has_blend && a->blend_enable) {
         struct mali_fixed fx = {.blend_en = true, .src_c = f[0], .dst_c = f[1], .src_a = f[2], .dst_a = f[3],
                                 .op_c = a->color_blend_op, .op_a = a->alpha_blend_op};
         uint32_t eq; unsigned cm;
         if (mali_blend_equation(&fx, &eq, &cm))
            has_blend = true;
      }
      if (has_blend) {
         if (!st.rp || st.rp->color_attachment_count < 1 || st.rp->color_attachment_formats[0] == VK_FORMAT_UNDEFINED ||
             BITSET_TEST(st.dynamic, MESA_VK_DYNAMIC_CB_WRITE_MASKS)) {
            res = VK_ERROR_FEATURE_NOT_PRESENT;
            goto fail;
         }
         struct mali_blend_opts bo = {
            .arch = arch, .vk_format = st.rp->color_attachment_formats[0],
            .samples = st.ms && st.ms->rasterization_samples ? st.ms->rasterization_samples : 1,
            .rgb_op = a->color_blend_op, .alpha_op = a->alpha_blend_op,
            .rgb_src = a->src_color_blend_factor, .rgb_dst = a->dst_color_blend_factor,
            .alpha_src = a->src_alpha_blend_factor, .alpha_dst = a->dst_alpha_blend_factor,
            .color_mask = a->write_mask,
            .src0_type = p->fs.fs_blend0_type, .src1_type = p->fs.fs_blend_src1_type,
         };
         bool uses_src1 = false;
         for (unsigned i = 0; i < 4; i++)
            if (f[i] >= VK_BLEND_FACTOR_SRC1_COLOR && f[i] <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA)
               uses_src1 = true;
         if (!bo.src0_type || (uses_src1 && !bo.src1_type)) {   /* the FS does not write a second output: SRC1 would read garbage */
            res = VK_ERROR_FEATURE_NOT_PRESENT;
            goto fail;
         }
         if (mali_compile_blend(&bo, &p->blend, err, sizeof err)) {
            fprintf(stderr, "mali_vk: blend shader compile failed: %s\n", err);
            res = VK_ERROR_INVALID_SHADER_NV;
            goto fail;
         }
      }
   }

   /* code BO: VS binary (its varying secondary shader follows inside it), FS binary, 256-byte aligned; blend shader after it, 16-aligned */
   size_t fs_off = (p->vs.bin_size + BIN_ALIGN - 1) & ~(size_t)(BIN_ALIGN - 1);
   size_t bl_off = (fs_off + p->fs.bin_size + 15) & ~(size_t)15;
   size_t code_sz = has_blend ? bl_off + p->blend.bin_size : fs_off + p->fs.bin_size;
   if (mali_device_pdev(dev)->backend->bo_alloc(dev->kdev, code_sz, MALI_BO_EXEC, &p->code)) {
      res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto fail;
   }
   p->have_code = true;
   memcpy(p->code.cpu, p->vs.bin, p->vs.bin_size);
   memcpy((char *)p->code.cpu + fs_off, p->fs.bin, p->fs.bin_size);
   if (has_blend)
      memcpy((char *)p->code.cpu + bl_off, p->blend.bin, p->blend.bin_size);

   struct mali_gfx_state *g = &p->gfx;
   if (has_blend)
      g->blend_va = p->code.gpu_va + bl_off;
   g->vs = &p->vs;
   g->fs = &p->fs;
   g->samples = samples;
   g->per_sample = samples > 1 && (o.sample_shading || p->fs.fs_sample_shading);
   g->vs_va = p->code.gpu_va;
   g->fs_va = p->code.gpu_va + fs_off;
   g->push_base[0] = 40;
   g->push_base[1] = arch == 13 ? 24 : 0;   /* v13: FAU words 4,5 hold the blend descriptor */
   g->depth_func = VK_COMPARE_OP_ALWAYS;
   if (st.ds) {
      g->depth_test = st.ds->depth.test_enable;
      g->depth_write = st.ds->depth.write_enable;
      g->depth_func = st.ds->depth.compare_op;
   }
   if (ci->pInputAssemblyState) {
      switch (ci->pInputAssemblyState->topology) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: g->prim = 1; break;
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: g->prim = 2; break;
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: g->prim = 4; break;
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: g->prim = 8; break;
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: g->prim = 10; break;
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: g->prim = 12; break;
      default:
         res = VK_ERROR_FEATURE_NOT_PRESENT; if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: unsupported topology %d\n", ci->pInputAssemblyState->topology);
         goto fail;
      }
      g->prim_restart = ci->pInputAssemblyState->primitiveRestartEnable;
      /* fillModeNonSolid: Mali has no polygon mode. POINT = the vertices as a point list; LINE = triangle edges as an indexed line list built at frame build */
      if (st.rs && g->prim >= 8 && st.rs->polygon_mode == VK_POLYGON_MODE_POINT) {
         g->poly_mode = 2; g->prim = 1; g->prim_restart = false;
      } else if (st.rs && g->prim >= 8 && st.rs->polygon_mode == VK_POLYGON_MODE_LINE) {
         g->poly_mode = 1; g->poly_prim = g->prim; g->prim = 2;
      }
   }
   if (st.vi) {
      uint32_t v = st.vi->attributes_valid;
      while (v) {
         unsigned loc = u_bit_scan(&v);
         const struct vk_vertex_attribute_state *a = &st.vi->attributes[loc];
         struct mali_fmt f;
         if (g->nattr != loc || loc >= MALI_MAX_ATTR || !mali_format_get(arch, a->format, &f) || !mali_format_attr_word0(&f) ||
             !(st.vi->bindings_valid & (1u << a->binding)) ||
             (st.vi->bindings[a->binding].input_rate && st.vi->bindings[a->binding].divisor > 1)) {
            res = VK_ERROR_FEATURE_NOT_PRESENT; if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: unsupported pipeline state at %s:%d\n", __FILE__, __LINE__);   /* sparse locations, instance rate, non-vertex format: not supported */
            goto fail;
         }
         const struct vk_vertex_binding_state *vb = &st.vi->bindings[a->binding];
         const bool inst = vb->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE;
         /* instance rate: HW has no zero divisor, panvk uses stride 0 */
         g->attr[g->nattr++] = (struct mali_attr){f.hw, a->binding, a->offset, inst && vb->divisor == 0 ? 0 : vb->stride, inst};
      }
   }
   vk_dynamic_graphics_state_init(&p->dynamic);
   vk_dynamic_graphics_state_fill(&p->dynamic, &st);
   vk_free(&dev->vk.alloc, state_mem);
   vk_free(&dev->vk.alloc, all);
   *out = mali_pipeline_to_handle(p);
   return VK_SUCCESS;

fail:
   vk_free(&dev->vk.alloc, state_mem);
   vk_free(&dev->vk.alloc, all);
   pipeline_destroy(dev, p, alloc);
   return res;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                             const VkAllocationCallbacks *alloc, VkPipeline *out)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   VkResult result = VK_SUCCESS;
   uint32_t i;

   for (i = 0; i < n; i++) {
      VkResult r = create_one(dev, &ci[i], alloc, &out[i]);
      if (r != VK_SUCCESS) {
         result = r;
         out[i] = VK_NULL_HANDLE;
         if (ci[i].flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)
            break;
      }
   }
   for (i++; i < n; i++)
      out[i] = VK_NULL_HANDLE;
   return result;
}

static VkResult
create_compute_one(struct mali_device *dev, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipeline *out)
{
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   const unsigned arch = pdev->gpu.arch_major >= 12 ? 13 : pdev->gpu.arch_major == 10 ? 10 : 11;
   struct mali_pipeline *p;
   struct mali_flat_layout flat;
   VkResult res = VK_ERROR_OUT_OF_HOST_MEMORY;
   char err[256];
   const struct vk_shader_module *m = find_module(&ci->stage);

   if (!m || ci->stage.stage != VK_SHADER_STAGE_COMPUTE_BIT)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   p = vk_object_zalloc(&dev->vk, alloc, sizeof *p, VK_OBJECT_TYPE_PIPELINE);
   if (!p)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   p->dev = dev;
   p->is_compute = true;
   VK_FROM_HANDLE(vk_pipeline_layout, layout, ci->layout);
   if (layout && (res = mali_pipeline_layout_flatten(layout, &flat)) != VK_SUCCESS)
      goto fail;
   struct mali_compile_opts o = {.arch = arch, .layout = layout ? &flat.c : NULL, .stage = MALI_STAGE_COMPUTE};
   o.spirv = (const uint32_t *)m->data;
   o.spirv_words = m->size / 4;
   o.entry = ci->stage.pName;
   if (mali_compile(&o, &p->cs, err, sizeof err)) {
      fprintf(stderr, "mali_vk: CS compile failed: %s\n", err);
      res = VK_ERROR_INVALID_SHADER_NV;
      goto fail;
   }
   if (getenv("MALI_PIPE_DBG"))
      fprintf(stderr, "mali_vk: compute local %ux%ux%u tls %u wls %u fau %u bin %u\n", p->cs.local_size[0], p->cs.local_size[1],
              p->cs.local_size[2], p->cs.tls_size, p->cs.wls_size, p->cs.fau_count, (unsigned)p->cs.bin_size);
   if (p->cs.local_size[0] * p->cs.local_size[1] * p->cs.local_size[2] > 1024) {   /* tls_size > 0 (register spill, e.g. int64 divide) is supported by the compute emit (mali_cs_emit.c) */
      res = VK_ERROR_FEATURE_NOT_PRESENT;
      goto fail;
   }
   if (mali_device_pdev(dev)->backend->bo_alloc(dev->kdev, p->cs.bin_size, MALI_BO_EXEC, &p->code)) {
      res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto fail;
   }
   p->have_code = true;
   memcpy(p->code.cpu, p->cs.bin, p->cs.bin_size);
   *out = mali_pipeline_to_handle(p);
   return VK_SUCCESS;
fail:
   pipeline_destroy(dev, p, alloc);
   return res;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateComputePipelines(VkDevice d, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *ci,
                            const VkAllocationCallbacks *alloc, VkPipeline *out)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   VkResult result = VK_SUCCESS;
   uint32_t i;

   for (i = 0; i < n; i++) {
      VkResult r = create_compute_one(dev, &ci[i], alloc, &out[i]);
      if (r != VK_SUCCESS) {
         result = r;
         out[i] = VK_NULL_HANDLE;
         if (ci[i].flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)
            break;
      }
   }
   for (i++; i < n; i++)
      out[i] = VK_NULL_HANDLE;
   return result;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyPipeline(VkDevice d, VkPipeline pipeline, const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   VK_FROM_HANDLE(mali_pipeline, p, pipeline);
   if (p)
      pipeline_destroy(dev, p, alloc);
}

/* Every submission is serialised by the queue (a frame is built and retired before the next one), so barriers have nothing to do. */
VKAPI_ATTR void VKAPI_CALL
mali_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *info)
{
}

/* ---- resolvers for mali_cmd.c ---- */
static int
res_pipeline(VkPipeline h, struct mali_gfx_state *out)
{
   struct mali_pipeline *p = mali_pipeline_from_handle(h);
   if (!p || p->is_compute)
      return -EINVAL;
   *out = p->gfx;
   return 0;
}

static int
res_pipeline_dyn(VkPipeline h, const struct vk_dynamic_graphics_state **out)
{
   struct mali_pipeline *p = mali_pipeline_from_handle(h);
   if (!p || p->is_compute)
      return -EINVAL;
   *out = &p->dynamic;
   return 0;
}

static int
res_compute(VkPipeline h, const struct mali_compiled **c, uint64_t *va)
{
   struct mali_pipeline *p = mali_pipeline_from_handle(h);
   if (!p || !p->is_compute)
      return -EINVAL;
   *c = &p->cs;
   *va = p->code.gpu_va;
   return 0;
}

static int
target_of(const struct mali_image *img, VkImageAspectFlags aspect, unsigned level, unsigned layer, struct mali_cmd_target *out)
{
   if (!img->va || level >= img->lay.d.levels || img->lay.d.tiled)
      return -EINVAL;
   int zs = 0;
   bool has_d = true;   /* the format has a depth plane we can attach */
   out->rt = NULL;
   out->is_zs = false;
   out->has_s = false;
   switch (img->vk.format) {
   case VK_FORMAT_R8G8B8A8_UNORM: break;
   /* D+S images are two planes (depth, S8): the depth plane is the ZS target, the S8 plane the S target */
   case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: zs = 1; break;
   case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT: zs = 2; break;
   case VK_FORMAT_D16_UNORM: case VK_FORMAT_D16_UNORM_S8_UINT: zs = 3; break;
   case VK_FORMAT_S8_UINT: has_d = false; break;
   default: {
      bool ok;
      out->rt = mali_rtfmt_lookup(img->vk.format, &ok);
      if (!ok)
         return -EINVAL;
      break;
   }
   }
   const bool st_fmt = img->vk.format == VK_FORMAT_S8_UINT || img->vk.format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                       img->vk.format == VK_FORMAT_D24_UNORM_S8_UINT || img->vk.format == VK_FORMAT_D16_UNORM_S8_UINT;
   const bool want_d = zs && has_d && (aspect & VK_IMAGE_ASPECT_DEPTH_BIT);
   const bool want_s = st_fmt && (aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
   if (!zs && !st_fmt && aspect != VK_IMAGE_ASPECT_COLOR_BIT)
      return -EINVAL;
   if ((zs || st_fmt) && !want_d && !want_s)
      return -EINVAL;
   const struct mali_plane_layout *pl = mali_image_plane_for_aspect(&img->lay, want_d ? VK_IMAGE_ASPECT_DEPTH_BIT : want_s ? VK_IMAGE_ASPECT_STENCIL_BIT : aspect);
   if (!pl)
      return -EINVAL;
   out->width = MAX2(img->lay.d.extent.width >> level, 1u);
   out->height = MAX2(img->lay.d.extent.height >> level, 1u);
   out->zs_format = want_d ? zs : 0;
   out->va = img->va + mali_image_subresource_offset(pl, level, layer);
   out->row_stride = pl->slice[level].row_stride;
   out->is_zs = zs || st_fmt;
   out->samples = img->lay.d.samples;
   out->surface_stride = (uint32_t)pl->slice[level].surface_stride;
   if (want_s) {
      const struct mali_plane_layout *sp = mali_image_plane_for_aspect(&img->lay, VK_IMAGE_ASPECT_STENCIL_BIT);
      if (!sp)
         return -EINVAL;
      out->has_s = true;
      out->s_va = img->va + mali_image_subresource_offset(sp, level, layer);
      out->s_row_stride = sp->slice[level].row_stride;
      if ((out->s_va & 63) || (out->s_row_stride & 15))
         return -EINVAL;
   }
   if (!want_d && want_s) {   /* stencil-only view: no ZS target, the primary va is unused */
      out->va = out->s_va;
      out->row_stride = out->s_row_stride;
   }
   return (out->va & 63) || (out->row_stride & 15) ? -EINVAL : 0;
}

static int
res_image_view(VkImageView h, struct mali_cmd_target *out)
{
   struct mali_image_view *v = mali_image_view_from_handle(h);
   if (!v)
      return -EINVAL;
   struct mali_image *img = container_of(v->vk.image, struct mali_image, vk);
   VkImageAspectFlags a = v->vk.aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? (v->vk.aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) : VK_IMAGE_ASPECT_COLOR_BIT;
   return target_of(img, a, v->vk.base_mip_level, v->vk.base_array_layer, out);
}

static int
res_image(VkImage h, struct mali_cmd_target *out)
{
   struct mali_image *img = mali_image_from_handle(h);
   if (!img)
      return -EINVAL;
   int r = target_of(img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, out);
   if (!r)
      out->multi = img->lay.d.levels != 1 || img->lay.d.layers != 1 || img->lay.d.type != VK_IMAGE_TYPE_2D || img->lay.d.samples != 1;
   return r;
}

static int
res_shader(const struct vk_shader *s, const struct mali_compiled **c, uint64_t *va)
{
   return -ENOTSUP;   /* shader objects: not exposed */
}

const struct mali_cmd_res_ops mali_cmd_res = {res_pipeline, res_image_view, res_image, res_shader, res_compute, res_pipeline_dyn};
