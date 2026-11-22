/* SPDX-License-Identifier: MIT */
/* M9: graphics pipelines (VS + FS compiled by mali_compile against the pipeline layout, binaries in one executable BO), the
 * resolvers that give mali_cmd.c its pipeline / image view targets, and the small entrypoints a render-pass client needs that no
 * runtime default covers (barriers are no-ops: every submission is serialised by the queue). Shader modules, pipeline cache,
 * render pass, framebuffer: the runtime's vk_common_* (render pass 1.x is emulated through vkCmdBeginRendering, mali_cmd.c). */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "mali_cmd.h"
#include "mali_soft.h"
#include "mali_cs_emit.h"
#include "mali_entrypoints.h"
#include "mali_image.h"
#include "vk_enum_to_str.h"
#include "mali_msaa.h"
#include "mali_gs.h"
#include "mali_pipeline_layout.h"
#include "vk_alloc.h"
#include "vk_graphics_state.h"
#include "vk_pipeline_layout.h"
#include "vk_shader_module.h"
#include "util/bitscan.h"
#include "vk_util.h"
#include <pthread.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <stdlib.h>

struct mali_pipeline {
   struct vk_object_base base;
   struct mali_device *dev;
   struct vk_dynamic_graphics_state dynamic;   /* R4: static state values + which states are static (bound with the pipeline) */
   struct mali_gfx_state gfx;     /* vs/fs point into this object; vs_va/fs_va into code */
   struct mali_compiled vs, fs, cs, blend;   /* cs: compute pipelines (is_compute); the binary is at code + 0 */
   struct mali_compiled vs_plain;            /* XFB programs: the same VS without the XFB lowering (mali_tf.h) */
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

/* SPIR-V of a stage: a shader module, or (VK_KHR_maintenance5) a VkShaderModuleCreateInfo chained to the stage with module == NULL. */
static bool
stage_spirv(const VkPipelineShaderStageCreateInfo *s, const uint32_t **code, uint32_t *words)
{
   const struct vk_shader_module *m = vk_shader_module_from_handle(s->module);
   if (m) {
      *code = (const uint32_t *)m->data;
      *words = m->size / 4;
      return true;
   }
   const VkShaderModuleCreateInfo *mci = vk_find_struct_const(s->pNext, SHADER_MODULE_CREATE_INFO);
   if (!mci || !mci->pCode)
      return false;
   *code = mci->pCode;
   *words = mci->codeSize / 4;
   return true;
}

/* VkSpecializationInfo -> mali_compile_opts (entries copied into `buf`, MALI_SPEC_MAX of them). Entries outside dataSize are dropped. */
#define MALI_SPEC_MAX 32
static void
set_spec(struct mali_compile_opts *o, struct mali_spec_entry *buf, const VkSpecializationInfo *si)
{
   o->nspec = 0;
   o->spec = NULL;
   o->spec_data = NULL;
   if (!si || !si->mapEntryCount || !si->pData)
      return;
   unsigned n = 0;
   for (uint32_t i = 0; i < si->mapEntryCount && n < MALI_SPEC_MAX; i++) {
      const VkSpecializationMapEntry *e = &si->pMapEntries[i];
      if (e->size == 0 || e->offset + e->size > si->dataSize)
         continue;
      buf[n++] = (struct mali_spec_entry){.id = e->constantID, .offset = e->offset, .size = (uint32_t)e->size};
   }
   o->nspec = n;
   o->spec = n ? buf : NULL;
   o->spec_data = si->pData;
}


/* Empty fragment shader (void main() {}): a pipeline without a fragment stage that is not rasterizer-discarded still has depth/stencil
 * effects (dEQP pipeline.monolithic.{stencil,depth}.nocolor: no FS, no colour attachment), so its draws run with this shader. */
static const uint32_t null_fs_spv[] = {
   0x07230203, 0x00010000, 0, 5, 0,
   0x00020011, 1,                                  /* OpCapability Shader */
   0x0003000e, 0, 1,                               /* OpMemoryModel Logical GLSL450 */
   0x0005000f, 4, 1, 0x6e69616d, 0,                /* OpEntryPoint Fragment %1 "main" */
   0x00030010, 1, 7,                               /* OpExecutionMode %1 OriginUpperLeft */
   0x00020013, 2,                                  /* %2 = OpTypeVoid */
   0x00030021, 3, 2,                               /* %3 = OpTypeFunction %2 */
   0x00050036, 2, 1, 0, 3,                         /* %1 = OpFunction %2 None %3 */
   0x000200f8, 4,                                  /* %4 = OpLabel */
   0x000100fd,                                     /* OpReturn */
   0x00010038,                                     /* OpFunctionEnd */
};

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
   struct mali_spec_entry specbuf[MALI_SPEC_MAX];

   if (!p)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   p->dev = dev;

   const VkPipelineShaderStageCreateInfo *vss = NULL, *fss = NULL, *gss = NULL;
   unsigned gs_prims = 0;   /* lane 1: the input primitive of an accepted pass-through GS (0 = no geometry stage) */
   for (uint32_t i = 0; i < ci->stageCount; i++) {
      /* requiredSubgroupSizeStages is COMPUTE only: a graphics stage may not chain a requested size. */
      const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *rss =
         vk_find_struct_const(ci->pStages[i].pNext, PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO);
      if (rss && rss->requiredSubgroupSize) {
         fprintf(stderr, "mali_vk: requiredSubgroupSize %u on a non-compute stage\n", rss->requiredSubgroupSize);
         res = VK_ERROR_FEATURE_NOT_PRESENT;
         goto fail;
      }
      if (ci->pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT)
         vss = &ci->pStages[i];
      else if (ci->pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
         fss = &ci->pStages[i];
      else if (ci->pStages[i].stage == VK_SHADER_STAGE_GEOMETRY_BIT)
         gss = &ci->pStages[i];   /* accepted below only when it is a provable pass-through (mali_gs.c) */
      else {
         res = VK_ERROR_FEATURE_NOT_PRESENT; if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: unsupported pipeline state at %s:%d\n", __FILE__, __LINE__);   /* tessellation / geometry: not supported */
         goto fail;
      }
   }
   const uint32_t *vcode = NULL, *fcode = NULL;
   uint32_t vwords = 0, fwords = 0;
   if (!vss || !stage_spirv(vss, &vcode, &vwords) || (fss && !stage_spirv(fss, &fcode, &fwords))) {
      res = VK_ERROR_FEATURE_NOT_PRESENT; if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: unsupported pipeline state at %s:%d\n", __FILE__, __LINE__);   /* VS-only (rasterizer discard) pipelines: later */
      goto fail;
   }

   /* Geometry shaders, lane 1 of notes/gs-design.md §4: a *provable* pass-through GS (1-in/1-out, in order, one stream,
    * every output a copy of the matching input vertex — mali_gs.c) is compiled into the vertex program by not existing:
    * the stream it would emit is exactly the one the vertex shader above already produces, so the VS is compiled
    * unchanged and the draw below keeps its topology, its vertex count and its pipeline state, which is why the pass-through
    * draw record *is* the existing one (mali_cmd.c). Zero jobs, zero dispatches, zero bytes. Any other geometry shader is
    * refused here with a named reason, before any state is built: it needs lane 2's compute pre-pass. The feature bit stays
    * false (mali_physical_device.c), so an application cannot reach this path at all; MALI_EXPERIMENTAL_GS=1 (mali_gs.h) is
    * the development gate the gs_* tests use, and it never advertises anything. */
   if (gss) {
      const uint32_t *gcode = NULL;
      uint32_t gwords = 0;
      char why[192] = "no SPIR-V in the geometry stage";
      if (!mali_experimental_gs()) {
         res = VK_ERROR_FEATURE_NOT_PRESENT;
         if (getenv("MALI_PIPE_DBG"))
            fprintf(stderr, "mali_vk: geometry shader refused: geometryShader is not advertised (MALI_EXPERIMENTAL_GS=1 is the development gate)\n");
         goto fail;
      }
      if (!stage_spirv(gss, &gcode, &gwords) ||
          !mali_gs_passthrough(gcode, gwords, gss->pName, &gs_prims, why, sizeof why)) {
         res = VK_ERROR_FEATURE_NOT_PRESENT;
         if (getenv("MALI_PIPE_DBG"))
            fprintf(stderr, "mali_vk: geometry shader refused: %s\n", why);
         goto fail;
      }
      if (getenv("MALI_PIPE_DBG"))
         fprintf(stderr, "mali_vk: geometry shader accepted as a pass-through (%u-in/1-out, compiled as the vertex program)\n",
                 gs_prims);
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
   /* nir_lower_robust_access runs for robustness2 only (it costs a size load per access); its get_ssbo_size/get_ubo_size
    * queries are lowered to a read of the descriptor's size word (mali_descriptor_lower.c lower_size_query), so the
    * features it backs can be advertised on their own. robustBufferAccess alone is still advertised without it (Vulkan
    * 1.0 makes it mandatory, zink needs it): the dynamic array-index clamp (mali_descriptor_lower.c:129) plus the
    * descriptor bounds, not full byte-range clamping. */
   flat.c.robust_access = dev->vk.enabled_features.robustBufferAccess2 || dev->vk.enabled_features.robustImageAccess2;

   /* VK_EXT_transform_feedback: four u64 buffer addresses go after the application's push constants, so an XFB
    * program's push range grows by 32 B (the app never sees it; the shader's range check and the FAU reservation
    * both need the grown size). Only for programs that declare XFB, so other pipelines keep their FAU layout. */
   const unsigned app_push = layout ? flat.c.push_size : 0;
   const int xfb = mali_spirv_has_xfb(vcode, vwords);
   if (xfb) {
      if (app_push + 32 > MALI_PUSH_MAX) {   /* the app already uses the whole block: no room for the addresses */
         fprintf(stderr, "mali_vk: transform feedback needs 32 B of push constants after the application's %u\n", app_push);
         res = VK_ERROR_FEATURE_NOT_PRESENT;
         goto fail;
      }
      if (layout)
         flat.c.push_size = app_push + 32;
   }
   struct mali_compile_opts o = {.arch = arch, .layout = layout ? &flat.c : NULL};
   o.xfb_pc = app_push;
   o.xfb = xfb;
   o.stage = MALI_STAGE_VERTEX;
   o.strip_psiz = ci->pInputAssemblyState && ci->pInputAssemblyState->topology != VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
   o.spirv = vcode;
   o.spirv_words = vwords;
   o.entry = vss->pName;
   /* The VS code depends on the FS's interpolants (noperspective mask): hand it the FS module that will be linked.
    * fss == NULL only when the pipeline has no fragment stage (rasterizerDiscardEnable), which also writes no
    * interpolants; the mask is then 0. There is no path in this driver that compiles the stages in separate
    * invocations (no VK_EXT_shader_object, no VK_KHR_pipeline_library: they are not in the device's extension list),
    * so every VS compile that can have interpolants has its FS here. */
   o.fs_spirv = fss ? fcode : NULL;
   o.fs_spirv_words = fss ? fwords : 0;
   o.fs_entry = fss ? fss->pName : NULL;
   set_spec(&o, specbuf, vss->pSpecializationInfo);
   int vcr = mali_compile(&o, &p->vs, err, sizeof err);
   if (vcr) {
      fprintf(stderr, "mali_vk: VS compile failed: %s\n", err);
      res = vcr == MALI_COMPILE_UNSUPPORTED ? VK_ERROR_FEATURE_NOT_PRESENT : VK_ERROR_INVALID_SHADER_NV;
      goto fail;
   }
   if (xfb) {
      /* The same vertex shader without the XFB lowering: every draw outside a Begin/End range runs this one
       * (mali_cmd.c), so a shader with XFB outputs stores nothing while capture is not active. Compiled once per
       * pipeline with the same push layout (only the lowering differs), so the pipeline cache key does not change. */
      o.xfb = 0;
      o.xfb_pc = 0;
      if (mali_compile(&o, &p->vs_plain, err, sizeof err)) {
         fprintf(stderr, "mali_vk: VS compile (non-XFB variant) failed: %s\n", err);
         res = VK_ERROR_INVALID_SHADER_NV;
         goto fail;
      }
      o.xfb = 1;
      o.xfb_pc = app_push;
   }
   /* no fragment stage (rasterizerDiscardEnable pipelines): nothing to compile, every draw of the pipeline is skipped (vertex-stage side effects are not run) */
   o.stage = MALI_STAGE_FRAGMENT;
   o.strip_psiz = 0;
   o.spirv = fss ? fcode : null_fs_spv;
   o.spirv_words = fss ? fwords : (uint32_t)(sizeof null_fs_spv / 4);
   o.entry = fss ? fss->pName : "main";
   set_spec(&o, specbuf, fss ? fss->pSpecializationInfo : NULL);
   /* MSAA (MALI_EXPERIMENTAL_MSAA): 1 or 4 samples; sampleRateShading when minSampleShading * samples > 1 */
   unsigned samples = st.ms && st.ms->rasterization_samples ? st.ms->rasterization_samples : 1;
   if (samples != 1 && (samples != 4 || !mali_experimental_msaa() || BITSET_TEST(st.dynamic, MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES))) {
      res = VK_ERROR_FEATURE_NOT_PRESENT;
      goto fail;
   }
   o.sample_shading = samples > 1 && st.ms && st.ms->sample_shading_enable && st.ms->min_sample_shading * (float)samples > 1.0f;
   o.link_vs = &p->vs.layout;
   o.clip_distances = p->vs.vs_clip_distances;
   const bool rs_discard = st.rs && st.rs->rasterizer_discard_enable;
   if ((fss || !rs_discard) && mali_compile(&o, &p->fs, err, sizeof err)) {
      fprintf(stderr, "mali_vk: FS compile failed: %s\n", err);
      res = VK_ERROR_INVALID_SHADER_NV;
      goto fail;
   }

   /* dualSrcBlend: SRC1 factors (VkBlendFactor 15..18) on attachment 0 need a blend shader (fixed function cannot read SRC1) */
   bool has_blend = false;
   if (fss && st.cb && st.cb->attachment_count > 0 && !(BITSET_TEST(st.dynamic, MESA_VK_DYNAMIC_CB_BLEND_EQUATIONS))) {
      const struct vk_color_blend_attachment_state *a = &st.cb->attachments[0];
      const unsigned f[4] = {a->src_color_blend_factor, a->dst_color_blend_factor, a->src_alpha_blend_factor, a->dst_alpha_blend_factor};
      for (unsigned i = 0; i < 4; i++)
         if (f[i] >= VK_BLEND_FACTOR_SRC1_COLOR && f[i] <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA)
            has_blend = true;
      /* logicOp (static state; zink uses it statically as extendedDynamicState2LogicOp is not advertised): colour logic op in the blend shader */
      /* a logic op can only change the output when colour output 0 is written and attachment 0 is writable; otherwise the pipeline is
       * accepted without a blend shader and the draw-time check (gfx.logic_inert) lets it through */
      /* a write-masked RT0 makes the logic op inert only when no OTHER attachment writes: the later attachments keep their
       * own blend state and would silently skip the logic operation (the blend shader writes RT0 alone) */
      bool other_writes = false;
      if (st.rp && st.rp->color_attachment_count > 1) {
         other_writes = BITSET_TEST(st.dynamic, MESA_VK_DYNAMIC_CB_WRITE_MASKS);
         for (unsigned i = 1; !other_writes && i < st.rp->color_attachment_count && i < 4; i++)
            /* the shader must write that output too: a depth-only FS with two bound attachments writes no colour, so
             * the logic op is still inert there and the pipeline must stay accepted */
            other_writes = st.rp->color_attachment_formats[i] != VK_FORMAT_UNDEFINED && (a[i].write_mask & 0xf) != 0 &&
                           (!p->fs.fs_colour_mask || (p->fs.fs_colour_mask & (1u << i)));
      }
      const bool rt0_inert = !p->fs.fs_blend0_type || !st.rp || st.rp->color_attachment_count < 1 ||
                             st.rp->color_attachment_formats[0] == VK_FORMAT_UNDEFINED ||
                             (!BITSET_TEST(st.dynamic, MESA_VK_DYNAMIC_CB_WRITE_MASKS) && !(a->write_mask & 0xf));
      /* a later writer keeps the logic op live: the blend shader writes RT0 alone, so it must be requested and then
       * refused at draw time, never silently dropped */
      const bool logic_inert = rt0_inert && !other_writes;
      const bool logic = st.cb->logic_op_enable && !logic_inert;
      p->gfx.logic_inert = st.cb->logic_op_enable && logic_inert;
      if (logic)
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
            .logic_op_enable = logic, .logic_op = st.cb->logic_op,
         };
         bool uses_src1 = false;
         for (unsigned i = 0; i < 4; i++)
            if (f[i] >= VK_BLEND_FACTOR_SRC1_COLOR && f[i] <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA)
               uses_src1 = true;
         if (!bo.src0_type || (uses_src1 && !logic && !bo.src1_type)) {   /* the FS does not write a second output: SRC1 would read garbage */
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
   size_t pl_off = (fs_off + p->fs.bin_size + BIN_ALIGN - 1) & ~(size_t)(BIN_ALIGN - 1);   /* non-XFB VS variant of an XFB program */
   size_t bl_off = (pl_off + p->vs_plain.bin_size + 15) & ~(size_t)15;
   size_t code_sz = has_blend ? bl_off + p->blend.bin_size : pl_off + p->vs_plain.bin_size;
   if (mali_device_pdev(dev)->backend->bo_alloc(dev->kdev, code_sz, MALI_BO_EXEC, &p->code)) {
      res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto fail;
   }
   p->have_code = true;
   memcpy(p->code.cpu, p->vs.bin, p->vs.bin_size);
   if (p->fs.bin_size)
      memcpy((char *)p->code.cpu + fs_off, p->fs.bin, p->fs.bin_size);
   if (p->vs_plain.bin_size)
      memcpy((char *)p->code.cpu + pl_off, p->vs_plain.bin, p->vs_plain.bin_size);
   if (has_blend)
      memcpy((char *)p->code.cpu + bl_off, p->blend.bin, p->blend.bin_size);

   struct mali_gfx_state *g = &p->gfx;
   if (has_blend)
      g->blend_va = p->code.gpu_va + bl_off;
   g->vs = &p->vs;
   g->fs = &p->fs;
   g->samples = samples;
   g->dyn_stride = BITSET_TEST(st.dynamic, MESA_VK_DYNAMIC_VI_BINDING_STRIDES);
   g->discard = rs_discard;   /* FS-less pipelines draw with the null FS (depth/stencil effects) */
   g->per_sample = samples > 1 && (o.sample_shading || p->fs.fs_sample_shading);
   g->vs_va = p->code.gpu_va;
   g->fs_va = p->code.gpu_va + fs_off;
   g->xfb_pc = app_push;   /* where mali_cmd.c stores the XFB buffer addresses in the push block */
   g->xfb = xfb;           /* xfb_pc == 0 is a valid offset, so the flag is what says the area exists */
   if (p->vs_plain.bin_size) {
      g->vs_plain = &p->vs_plain;          /* drawn whenever no Begin/End range is open (mali_cmd.c) */
      g->vs_plain_va = p->code.gpu_va + pl_off;
      if (getenv("MALI_TF_DBG"))
         fprintf(stderr, "mali_tf: pipeline has a second, non-XFB vertex program at 0x%" PRIx64 " (XFB one at 0x%" PRIx64 ")\n",
                 g->vs_plain_va, g->vs_va);
   }
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
      /* A pass-through GS keeps the input topology: the vertex stream is unchanged. That is only a faithful draw for a
       * LIST topology — a strip's gl_in order is the vertex order, so re-emitting those vertices as fresh strips reverses
       * the winding of every odd triangle (and a fan's first vertex is not in gl_in at all), so strip/fan inputs are
       * refused with a name instead of misrendering. */
      if (gs_prims) {
         const unsigned want = gs_prims == MALI_GS_POINTS ? 1 : gs_prims == MALI_GS_LINES ? 2 : 8;
         /* the topology the draw will use is not knowable when the pipeline takes it dynamically, and a POINT/LINE
          * polygon rewrite turns one triangle into several primitives (the per-vertex flags would then be split) */
         const bool dyn_prim = BITSET_TEST(st.dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY);
         /* a dynamic polygon mode is a rewrite too: mali_cmd.c applies POINT/LINE from the dynamic state at draw time */
         const bool dyn_poly = BITSET_TEST(st.dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE);
         const bool poly_rewrite = dyn_poly || (st.rs && st.rs->polygon_mode != VK_POLYGON_MODE_FILL);
         if (dyn_prim || poly_rewrite) {
            res = VK_ERROR_FEATURE_NOT_PRESENT;
            if (getenv("MALI_PIPE_DBG"))
               fprintf(stderr, "mali_vk: geometry shader refused: pass-through GS needs a static list topology and polygonMode FILL (dynamic topology %u, polygonMode %d)\n",
                       (unsigned)dyn_prim, st.rs ? (int)st.rs->polygon_mode : (int)VK_POLYGON_MODE_FILL);
            goto fail;
         }
         if (g->poly_mode || g->prim != want) {
            res = VK_ERROR_FEATURE_NOT_PRESENT;
            if (getenv("MALI_PIPE_DBG"))
               fprintf(stderr, "mali_vk: geometry shader refused: pass-through GS needs a %s list topology (pipeline has prim %u, polygonMode rewrite %u)\n",
                       gs_prims == MALI_GS_POINTS ? "point" : gs_prims == MALI_GS_LINES ? "line" : "triangle", g->prim, g->poly_mode);
            goto fail;
         }
      }
      g->prim_restart = ci->pInputAssemblyState->primitiveRestartEnable;
      /* fillModeNonSolid: Mali has no polygon mode. POINT = the vertices as a point list; LINE = triangle edges as an indexed line list built at frame build */
      if (st.rs && g->prim >= 8 && st.rs->polygon_mode == VK_POLYGON_MODE_POINT) {
         g->poly_mode = 2; g->poly_prim = g->prim; g->prim = 1;   /* prim_restart stays: the rewrite uses it to split strips */
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
         if (g->nattr > loc || loc >= MALI_MAX_ATTR || !mali_format_get(arch, a->format, &f) || !mali_format_attr_word0(&f) ||
             !(st.vi->bindings_valid & (1u << a->binding))) {
            res = VK_ERROR_FEATURE_NOT_PRESENT; if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: unsupported pipeline state at %s:%d\n", __FILE__, __LINE__);   /* sparse locations, non-vertex format: not supported */
            goto fail;
         }
         const struct vk_vertex_binding_state *vb = &st.vi->bindings[a->binding];
         const bool inst = vb->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE;
         /* instance rate: HW has no zero divisor, panvk uses stride 0 (divisor 1). divisor > 1: 1D POT / NPOT divisor attribute */
         const struct mali_attr cur = {f.hw, a->binding, a->offset, inst && vb->divisor == 0 ? 0 : vb->stride, inst, inst && vb->divisor == 0,
                                       inst && vb->divisor > 1 ? vb->divisor : 1, (int)a->format};
         /* sparse locations (dEQP vertex_input layout_skip): the shader reads attribute slot = location, so the skipped slots are filled with a copy
          * of this attribute (a valid binding; the shader never reads them) */
         while (g->nattr < loc)
            g->attr[g->nattr++] = cur;
         g->attr[g->nattr++] = cur;
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

/* maintenance5: VkPipelineCreateFlags2CreateInfo::flags replaces ci.flags when chained */
static bool
early_return(VkPipelineCreateFlags flags, const void *pNext)
{
   const VkPipelineCreateFlags2CreateInfoKHR *f2 = vk_find_struct_const(pNext, PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR);
   if (f2)
      return (f2->flags & VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR) != 0;
   return (flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT) != 0;
}


/* Batched vkCreate*Pipelines: each create is independent (mali_compile is thread-safe, the shader cache is locked), so a batch of n > 1 is
 * compiled by up to MALI_PIPE_THREADS workers (env, default 4, 1 = sequential). Results are per index; EARLY_RETURN_ON_FAILURE keeps its
 * meaning (first failing index wins, later pipelines are destroyed again). */
struct batch {
   struct mali_device *dev;
   const void *ci;
   size_t stride;
   bool compute;
   const VkAllocationCallbacks *alloc;
   VkPipeline *out;
   VkResult *res;
   uint32_t n;
   atomic_uint next;
};

/* The Vulkan memory chapter: an implementation must only call an application-provided allocator from the thread that
 * issued the command. create_one/create_compute_one allocate the pipeline object through `alloc` and set up temporary
 * state through dev->vk.alloc, so batching on workers is only allowed when both allocators are the driver's default.
 * With an application allocator the batch runs sequentially on the caller's thread (same results, no extra latency for
 * the usual n = 1..few case, and no thread-local allocator can be used from a worker). */
static bool
alloc_is_default(const struct mali_device *dev, const VkAllocationCallbacks *alloc)
{
   if (alloc)
      return false;
   const VkAllocationCallbacks *d = vk_default_allocator();
   return dev->vk.alloc.pfnAllocation == d->pfnAllocation && dev->vk.alloc.pfnFree == d->pfnFree;
}

static VkResult create_one(struct mali_device *, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
static VkResult create_compute_one(struct mali_device *, const VkComputePipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);

static void *
batch_worker(void *arg)
{
   struct batch *b = arg;
   for (uint32_t i; (i = atomic_fetch_add(&b->next, 1)) < b->n;) {
      const char *ci = (const char *)b->ci + i * b->stride;
      b->res[i] = b->compute ? create_compute_one(b->dev, (const void *)ci, b->alloc, &b->out[i])
                             : create_one(b->dev, (const void *)ci, b->alloc, &b->out[i]);
      if (b->res[i] != VK_SUCCESS)
         b->out[i] = VK_NULL_HANDLE;
   }
   return NULL;
}

static unsigned
pipe_threads(uint32_t n)
{
   const char *e = getenv("MALI_PIPE_THREADS");
   unsigned t = e ? (unsigned)atoi(e) : 4;
   return t > n ? n : t;
}

static VkResult
create_batch(struct mali_device *dev, bool compute, const void *ci, size_t stride, uint32_t n, const VkAllocationCallbacks *alloc, VkPipeline *out)
{
   VkResult result = VK_SUCCESS;
   /* Spec: when the command fails and creates no pipeline, every pPipelines element is VK_NULL_HANDLE. This path can
    * fail before a single worker runs (the calloc below), so null every entry up front; workers overwrite only the
    * indices they successfully created, and the failure scan below relies on the rest being null already. */
   for (uint32_t i = 0; i < n; i++)
      out[i] = VK_NULL_HANDLE;
   VkResult *res = calloc(n ? n : 1, sizeof *res);
   if (!res)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct batch b = {dev, ci, stride, compute, alloc, out, res, n, 0};
   unsigned nt = pipe_threads(n);
   pthread_t th[8];
   unsigned started = 0;
   if (nt > 8)
      nt = 8;
   for (unsigned k = 1; k < nt; k++)
      if (pthread_create(&th[started], NULL, batch_worker, &b) == 0)
         started++;
   batch_worker(&b);
   for (unsigned k = 0; k < started; k++)
      pthread_join(th[k], NULL);
   bool stop = false;
   for (uint32_t i = 0; i < n; i++) {
      if (stop) {
         if (out[i] != VK_NULL_HANDLE) {
            VK_FROM_HANDLE(mali_pipeline, p, out[i]);
            pipeline_destroy(dev, p, alloc);
         }
         out[i] = VK_NULL_HANDLE;
         continue;
      }
      if (res[i] != VK_SUCCESS) {
         result = res[i];
         const VkPipelineCreateFlags fl = *(const VkPipelineCreateFlags *)((const char *)ci + i * stride + offsetof(VkGraphicsPipelineCreateInfo, flags));
         const void *pn = *(const void *const *)((const char *)ci + i * stride + offsetof(VkGraphicsPipelineCreateInfo, pNext));
         if (early_return(fl, pn))
            stop = true;
      }
   }
   free(res);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                             const VkAllocationCallbacks *alloc, VkPipeline *out)
{
   VK_FROM_HANDLE(mali_device, dev, d);
   VkResult result = VK_SUCCESS;
   uint32_t i;

   if (n > 1 && pipe_threads(n) > 1 && alloc_is_default(dev, alloc))
      return create_batch(dev, false, ci, sizeof *ci, n, alloc, out);
   for (i = 0; i < n; i++) {
      VkResult r = create_one(dev, &ci[i], alloc, &out[i]);
      if (r != VK_SUCCESS) {
         result = r;
         out[i] = VK_NULL_HANDLE;
         if (early_return(ci[i].flags, ci[i].pNext))
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
   const uint32_t *ccode = NULL;
   uint32_t cwords = 0;

   if (!stage_spirv(&ci->stage, &ccode, &cwords) || ci->stage.stage != VK_SHADER_STAGE_COMPUTE_BIT)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   /* subgroupSizeControl: we advertise min == max == the hardware warp (16). A different requiredSubgroupSize is a spec
    * violation (VUID-requiredSubgroupSize-02414) that we must not turn into 16-wide code behind the shader's back: that would
    * corrupt every ballot/shuffle/quad/arithmetic lowering. Refuse it (and never compile it below). */
   const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *rss =
      vk_find_struct_const(ci->stage.pNext, PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO);
   const unsigned hw_subgroup_size = mali_subgroup_size_for_arch(arch);
   if (rss && rss->requiredSubgroupSize && rss->requiredSubgroupSize != hw_subgroup_size) {
      fprintf(stderr, "mali_vk: requiredSubgroupSize %u refused: the warp is %u\n", rss->requiredSubgroupSize, hw_subgroup_size);
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   p = vk_object_zalloc(&dev->vk, alloc, sizeof *p, VK_OBJECT_TYPE_PIPELINE);
   if (!p)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   p->dev = dev;
   p->is_compute = true;
   VK_FROM_HANDLE(vk_pipeline_layout, layout, ci->layout);
   if (layout && (res = mali_pipeline_layout_flatten(layout, &flat)) != VK_SUCCESS)
      goto fail;
   /* nir_lower_robust_access runs for robustness2 only (it costs a size load per access); its get_ssbo_size/get_ubo_size
    * queries are lowered to a read of the descriptor's size word (mali_descriptor_lower.c lower_size_query), so the
    * features it backs can be advertised on their own. robustBufferAccess alone is still advertised without it (Vulkan
    * 1.0 makes it mandatory, zink needs it): the dynamic array-index clamp (mali_descriptor_lower.c:129) plus the
    * descriptor bounds, not full byte-range clamping. */
   flat.c.robust_access = dev->vk.enabled_features.robustBufferAccess2 || dev->vk.enabled_features.robustImageAccess2;
   struct mali_compile_opts o = {.arch = arch, .layout = layout ? &flat.c : NULL, .stage = MALI_STAGE_COMPUTE};
   /* Compute defaults to 8 reserved FAU words (32 B of push constants); a larger push block is copied in from word 0, so reserve it or
      Bifrost asserts (bi_emit_load_push_constant). Never shrink below the default: that would move the immediates of existing shaders. */
   if (layout && (flat.c.push_size + 3) / 4 > 8)
      o.fau_reserved = (flat.c.push_size + 3) / 4;
   o.spirv = ccode;
   o.spirv_words = cwords;
   o.entry = ci->stage.pName;
   o.subgroup_size = rss ? rss->requiredSubgroupSize : 0;   /* 0 or == hw_subgroup_size; mali_compile refuses the rest too */
   struct mali_spec_entry specbuf[MALI_SPEC_MAX];
   set_spec(&o, specbuf, ci->stage.pSpecializationInfo);
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
   if (mali_device_pdev(dev)->backend->bo_alloc(dev->kdev, p->cs.bin_size ? p->cs.bin_size : 64, MALI_BO_EXEC, &p->code)) {   /* an empty shader (void main(){}) compiles to 0 bytes: a code BO of its own (dispatches of it are skipped) */
      res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto fail;
   }
   p->have_code = true;
   if (p->cs.bin_size)
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

   if (n > 1 && pipe_threads(n) > 1 && alloc_is_default(dev, alloc))
      return create_batch(dev, true, ci, sizeof *ci, n, alloc, out);
   for (i = 0; i < n; i++) {
      VkResult r = create_compute_one(dev, &ci[i], alloc, &out[i]);
      if (r != VK_SUCCESS) {
         result = r;
         out[i] = VK_NULL_HANDLE;
         if (early_return(ci[i].flags, ci[i].pNext))
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
   if (p) {
      /* the software rasteriser keys its CPU programs on these addresses, which are about to be reusable */
      mali_soft_retire(&p->vs, &p->fs);
      pipeline_destroy(dev, p, alloc);
   }
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
target_of(const struct mali_image *img, VkImageAspectFlags aspect, unsigned level, unsigned layer, bool view_is_3d,
          struct mali_cmd_target *out)
{
   if (!img->va || level >= img->lay.d.levels || img->lay.d.tiled) {
      fprintf(stderr, "mali_vk: render target rejected: %s va=%llx level %u/%u tiled=%d\n",
              vk_Format_to_str(img->vk.format), (unsigned long long)img->va, level, img->lay.d.levels,
              (int)img->lay.d.tiled);
      return -EINVAL;
   }
   const bool image_3d = img->vk.image_type == VK_IMAGE_TYPE_3D;
   if (image_3d && view_is_3d)
      return -EINVAL;   /* render to a whole 3D image: hangs the GPU (dEQP render_to_image.3d.mipmap, also with a1r5g5b5; level 0 too).
                         * A 2D / 2D-array view of one slice is a different case and is accepted below: the slice is just a 2D surface. */
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
      if (!ok) {
         fprintf(stderr, "mali_vk: render target format %s is not renderable (%d)\n",
                 vk_Format_to_str(img->vk.format), (int)img->vk.format);
         return -EINVAL;
      }
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
   /* A 2D view of a 3D image names a slice, which is a plain linear surface at base + layer * surface_stride.
    * A 3D view is refused above; the image-level resolver keeps refusing 3D images for copies/clears. */
   if (image_3d) {
      out->va = img->va + mali_image_subresource_offset(pl, level, 0) +
                (uint64_t)layer * pl->slice[level].surface_stride;
   } else {
      out->va = img->va + mali_image_subresource_offset(pl, level, layer);
   }
   out->row_stride = pl->slice[level].row_stride;
   out->is_zs = zs || st_fmt;
   out->samples = img->lay.d.samples;
   out->surface_stride = image_3d ? 0 : (uint32_t)pl->slice[level].surface_stride;   /* a 2D view of a slice is one surface */
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
   if (((out->va & 63) || (out->row_stride & 15)) && getenv("MALI_PIPE_DBG"))
      fprintf(stderr, "mali_vk: render target not aligned: format %d va&63=%u row_stride=%u %ux%u samples %u\n", (int)img->vk.format,
              (unsigned)(out->va & 63), (unsigned)out->row_stride, out->width, out->height, out->samples);
   return (out->va & 63) || (out->row_stride & 15) ? -EINVAL : 0;
}

static int
res_image_view(VkImageView h, struct mali_cmd_target *out)
{
   struct mali_image_view *v = mali_image_view_from_handle(h);
   if (!v) {
      if (getenv("MALI_PIPE_DBG"))
         fprintf(stderr, "mali_vk: image view handle is null\n");
      return -EINVAL;
   }
   struct mali_image *img = container_of(v->vk.image, struct mali_image, vk);
   VkImageAspectFlags a = v->vk.aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? (v->vk.aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) : VK_IMAGE_ASPECT_COLOR_BIT;
   int r = target_of(img, a, v->vk.base_mip_level, v->vk.base_array_layer,
                     v->vk.view_type == VK_IMAGE_VIEW_TYPE_3D, out);
   /* VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT: an SRGB view of a UNORM image (zink paints the window that way: notes/apilog.md,
    * vfmt=50 over ifmt=44) or the reverse renders with the VIEW's encoding, and target_of() above resolved the image's.
    * PanVK takes the colour attachment format from the image view (panvk_vX_cmd_draw.c:159,171; the blend conversion
    * format at :794 comes from that same VkFormat), and its texture descriptor likewise (panvk_vX_image_view.c:344). */
   if (!r && !out->is_zs && v->vk.view_format && v->vk.view_format != img->vk.format) {
      bool ok;
      const struct mali_rtfmt *vrt = mali_rtfmt_lookup(v->vk.view_format, &ok);
      if (!ok) {
         fprintf(stderr, "mali_vk: render target view format %s is not renderable (%d)\n",
                 vk_Format_to_str(v->vk.view_format), (int)v->vk.view_format);
         return -EINVAL;
      }
      out->rt = vrt;   /* also what the LOAD preload's conversion words (mali_cmd.c) and the blend format word use */
   }
   if (r)
      fprintf(stderr, "mali_vk: image view rejected rc=%d: format %s (%d) image type %s view type %d aspects 0x%x "
              "view aspect 0x%x usage 0x%x samples %u level %u/%u layer %u/%u\n", r, vk_Format_to_str(img->vk.format),
              (int)img->vk.format, img->vk.image_type == VK_IMAGE_TYPE_3D ? "3D" :
              img->vk.image_type == VK_IMAGE_TYPE_2D ? "2D" : "1D", (int)v->vk.view_type, (unsigned)v->vk.aspects,
              (unsigned)a, (unsigned)img->vk.usage, img->lay.d.samples, v->vk.base_mip_level, img->lay.d.levels,
              v->vk.base_array_layer, img->lay.d.layers);
   return r;
}

static int
res_image(VkImage h, struct mali_cmd_target *out)
{
   struct mali_image *img = mali_image_from_handle(h);
   if (!img)
      return -EINVAL;
   int r = target_of(img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, img->vk.image_type == VK_IMAGE_TYPE_3D, out);
   if (!r)
      out->multi = img->lay.d.levels != 1 || img->lay.d.layers != 1 || img->lay.d.type != VK_IMAGE_TYPE_2D || img->lay.d.samples != 1;
   return r;
}

static int
res_shader(const struct vk_shader *s, const struct mali_compiled **c, uint64_t *va)
{
   return -ENOTSUP;   /* shader objects: not exposed */
}

static struct mali_memory *
res_buffer_mem(VkBuffer b)
{
   struct mali_buffer *mb = mali_buffer_from_handle(b);
   return mb ? mb->mem : NULL;
}

static struct mali_memory *
res_image_mem(VkImage i)
{
   struct mali_image *mi = mali_image_from_handle(i);
   return mi ? mi->mem : NULL;
}

static struct mali_memory *
res_image_view_mem(VkImageView v)
{
   struct mali_image_view *iv = mali_image_view_from_handle(v);
   return iv && iv->vk.image ? container_of(iv->vk.image, struct mali_image, vk)->mem : NULL;
}

const struct mali_cmd_res_ops mali_cmd_res = {res_pipeline, res_image_view, res_image, res_shader, res_compute, res_pipeline_dyn,
                                              res_buffer_mem, res_image_mem, res_image_view_mem};

/* Desktop software rasteriser (mali_soft.c): the compiled shader pointers the frame record carries, so a test can key its CPU
 * program on the pipeline exactly the way the rasteriser will look it up. */
void
mali_soft_pipeline_keys(VkPipeline h, const struct mali_compiled **vs, const struct mali_compiled **fs)
{
   struct mali_pipeline *p = mali_pipeline_from_handle(h);
   *vs = p ? &p->vs : NULL;
   *fs = p ? &p->fs : NULL;
}
