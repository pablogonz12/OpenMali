/* SPDX-License-Identifier: MIT */
/* Own implementation (design follows the v9+ branch of the PanVK lowering, which is the proven resource model). */
#include "mali_descriptor_lower.h"

#include "nir_builder.h"
#include "panfrost/compiler/pan_compiler.h"

#include <stdio.h>

struct ctx {
   const struct mali_compile_layout *l;
   char *err;
   size_t errsz;
   bool failed;
};

static const struct mali_lbinding *
find(struct ctx *c, unsigned set, unsigned binding)
{
   if (set < c->l->nsets && binding < c->l->set[set].nbindings && c->l->set[set].b[binding].type != MALI_DT_NONE)
      return &c->l->set[set].b[binding];
   if (!c->failed && c->err)
      snprintf(c->err, c->errsz, "shader uses set %u binding %u, not in the pipeline layout", set, binding);
   c->failed = true;
   return NULL;
}

static uint32_t
first_handle(const struct ctx *c, unsigned set, const struct mali_lbinding *b)
{
   if (!b->stride)
      return pan_res_handle(MALI_DYN_TABLE, MALI_DYN_BUF_START + c->l->dyn_start[set] + b->dyn_idx);
   return pan_res_handle(set + 1, b->slot);
}

/* deref chain -> (set, binding, constant index, ssa index) */
static bool
deref_binding(nir_deref_instr *d, unsigned *set, unsigned *binding, uint32_t *imm, nir_def **ssa)
{
   *imm = 0;
   *ssa = NULL;
   if (d->deref_type == nir_deref_type_array) {
      if (nir_src_is_const(d->arr.index)) *imm = nir_src_as_uint(d->arr.index);
      else *ssa = d->arr.index.ssa;
      d = nir_deref_instr_parent(d);
   }
   if (d->deref_type != nir_deref_type_var) return false;
   *set = d->var->data.descriptor_set;
   *binding = d->var->data.binding;
   return true;
}

/* handle of a texture (sampler = false) or sampler (true) deref */
static nir_def *
deref_handle(nir_builder *b, struct ctx *c, nir_deref_instr *deref, bool sampler, unsigned plane)
{
   unsigned set, binding;
   uint32_t imm;
   nir_def *ssa;
   if (!deref_binding(deref, &set, &binding, &imm, &ssa)) {
      c->failed = true;
      if (c->err) snprintf(c->err, c->errsz, "unsupported texture/sampler deref");
      return nir_imm_int(b, 0);
   }
   const struct mali_lbinding *bd = find(c, set, binding);
   if (!bd) return nir_imm_int(b, 0);
   unsigned sub = (sampler && bd->type == MALI_DT_COMBINED_IMAGE_SAMPLER) ? (bd->ycbcr_planes ? bd->ycbcr_planes : 1) : 0;
   if (!sampler && bd->ycbcr_planes) sub = plane;   /* YCbCr element: [plane 0 texture, plane 1 texture, sampler] */
   unsigned stride = bd->stride ? bd->stride : 1;
   nir_def *h = nir_imm_int(b, first_handle(c, set, bd) + sub + imm * stride);
   if (ssa) h = nir_iadd(b, h, nir_imul_imm(b, ssa, stride));
   return h;
}

static bool
lower_tex(nir_builder *b, nir_tex_instr *tex, struct ctx *c)
{
   int ti = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   int si = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
   if (ti < 0) return false;
   b->cursor = nir_before_instr(&tex->instr);
   int pi = nir_tex_instr_src_index(tex, nir_tex_src_plane);
   unsigned plane = 0;
   if (pi >= 0) {
      if (!nir_src_is_const(tex->src[pi].src)) {
         c->failed = true;
         if (c->err) snprintf(c->err, c->errsz, "non-constant YCbCr plane");
         return false;
      }
      plane = nir_src_as_uint(tex->src[pi].src);
   }
   nir_deref_instr *tderef = nir_src_as_deref(tex->src[ti].src);
   nir_def *th = deref_handle(b, c, tderef, false, plane);
   nir_def *sh;
   if (si >= 0) {
      sh = deref_handle(b, c, nir_src_as_deref(tex->src[si].src), true, 0);
   } else {
      /* A texelFetch (txf/txf_ms) of a combined image sampler has a texture deref and no sampler deref. Fetching
       * through the all-zero dummy sampler entry (table 0) returns 0 on the phone for every texel; the combined
       * image sampler already owns a real sampler at slot + stride - 1, so use that instead. */
      unsigned tset, tbinding;
      uint32_t timm;
      nir_def *tssa;
      const struct mali_lbinding *tbd = NULL;
      if (deref_binding(tderef, &tset, &tbinding, &timm, &tssa))
         tbd = find(c, tset, tbinding);
      if (tbd && tbd->type == MALI_DT_COMBINED_IMAGE_SAMPLER)
         sh = deref_handle(b, c, tderef, true, 0);
      else
         sh = nir_imm_int(b, pan_res_handle(0, MALI_DUMMY_SAMPLER_IDX));
   }
   if (si >= 0) nir_tex_instr_remove_src(tex, si);
   if (pi >= 0) nir_tex_instr_remove_src(tex, nir_tex_instr_src_index(tex, nir_tex_src_plane));
   nir_tex_instr_remove_src(tex, nir_tex_instr_src_index(tex, nir_tex_src_texture_deref));
   nir_tex_instr_add_src(tex, nir_tex_src_texture_handle, th);
   nir_tex_instr_add_src(tex, nir_tex_src_sampler_handle, sh);
   tex->texture_index = 0;
   tex->sampler_index = 0;
   return true;
}

static bool
lower_intr(nir_builder *b, nir_intrinsic_instr *i, struct ctx *c)
{
   b->cursor = nir_before_instr(&i->instr);
   switch (i->intrinsic) {
   case nir_intrinsic_vulkan_resource_index: {
      unsigned set = nir_intrinsic_desc_set(i), binding = nir_intrinsic_binding(i);
      const struct mali_lbinding *bd = find(c, set, binding);
      if (!bd) {
         nir_def_replace(&i->def, nir_imm_ivec3(b, 0, 0, 0));
         return true;
      }
      nir_def *r = nir_vec3(b, nir_imm_int(b, first_handle(c, set, bd)), i->src[0].ssa, nir_imm_int(b, bd->count - 1));
      nir_def_replace(&i->def, r);
      return true;
   }
   case nir_intrinsic_vulkan_resource_reindex: {
      nir_def *o = i->src[0].ssa;
      nir_def *r = nir_vec3(b, nir_channel(b, o, 0), nir_iadd(b, nir_channel(b, o, 1), i->src[1].ssa), nir_channel(b, o, 2));
      nir_def_replace(&i->def, r);
      return true;
   }
   case nir_intrinsic_load_vulkan_descriptor: {
      nir_def *o = i->src[0].ssa;
      nir_def *idx = nir_umin(b, nir_channel(b, o, 1), nir_channel(b, o, 2));   /* robust: clamp the array index */
      nir_def_replace(&i->def, nir_vec3(b, nir_channel(b, o, 0), idx, nir_imm_int(b, 0)));
      return true;
   }
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples: {
      nir_def *h = deref_handle(b, c, nir_src_as_deref(i->src[0]), false, 0);
      nir_rewrite_image_intrinsic(i, h, nir_image_intrinsic_type_default);
      return true;
   }
   default:
      return false;
   }
}

static bool
lower_instr(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type == nir_instr_type_tex)
      return lower_tex(b, nir_instr_as_tex(instr), data);
   if (instr->type == nir_instr_type_intrinsic)
      return lower_intr(b, nir_instr_as_intrinsic(instr), data);
   return false;
}

/* After explicit IO the buffer index is vec2 (first handle, array index): the Valhall backend wants one scalar handle. */
static bool
pack_buf_idx(nir_builder *b, nir_intrinsic_instr *i, void *data)
{
   unsigned src;
   switch (i->intrinsic) {
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
   case nir_intrinsic_get_ssbo_size:
   case nir_intrinsic_get_ubo_size: src = 0; break;
   case nir_intrinsic_store_ssbo: src = 1; break;
   default: return false;
   }
   nir_def *idx = i->src[src].ssa;
   if (idx->num_components == 1)
      return false;
   b->cursor = nir_before_instr(&i->instr);
   nir_src_rewrite(&i->src[src], nir_iadd(b, nir_channel(b, idx, 0), nir_channel(b, idx, 1)));
   return true;
}

static bool
robust_filter(const nir_intrinsic_instr *i, const void *d)
{
   switch (i->intrinsic) {   /* descriptor-backed accesses only (not shared memory) */
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_store_ssbo:
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
   case nir_intrinsic_image_load:
   case nir_intrinsic_image_store:
   case nir_intrinsic_image_atomic:
   case nir_intrinsic_image_atomic_swap:
      return true;
   default:
      return false;
   }
}

/* robustness2 buffer bounds: nir_lower_robust_access asks the buffer for its size with get_ssbo_size/get_ubo_size, which the
 * Valhall backend does not have. The bound is dw1 of the 32-byte hardware Buffer descriptor, and the hardware exposes the
 * descriptor-table memory as resource "table 62" (index = table id, offset = bytes into that table), so the size is a UBO load
 * at slot * MALI_DESC_SLOT_SIZE + 4. Same lowering as PanVK's v9+ path
 * (third_party/mesa/src/panfrost/vulkan/panvk_vX_shader.c:607-630 valhall_lower_get_ssbo_size, citing pan_nir.h:63-73). A null
 * descriptor is the all-zero slot (mali_descriptor.c:423-430), so its size reads 0 and every access is discarded. */
static bool
lower_size_query(nir_builder *b, nir_intrinsic_instr *i, void *d)
{
   if (i->intrinsic != nir_intrinsic_get_ssbo_size && i->intrinsic != nir_intrinsic_get_ubo_size)
      return false;
   b->cursor = nir_before_instr(&i->instr);
   nir_def *h = i->src[0].ssa;
   if (h->num_components != 1)
      h = nir_iadd(b, nir_channel(b, h, 0), nir_channel(b, h, 1));
   nir_def *table = nir_ior_imm(b, nir_ushr_imm(b, h, 24), pan_res_handle(62, 0));
   nir_def *off = nir_iadd_imm(b, nir_imul_imm(b, nir_iand_imm(b, h, BITFIELD_MASK(24)), MALI_DESC_SLOT_SIZE), 4);
   nir_def *size = nir_load_ubo(b, 1, 32, table, off, .range = ~0u, .align_mul = MALI_DESC_SLOT_SIZE, .align_offset = 4);
   nir_def_replace(&i->def, size);
   return true;
}

bool
mali_nir_lower_descriptors(nir_shader *nir, const struct mali_compile_layout *layout, char *err, size_t errsz)
{
   struct ctx c = { .l = layout, .err = err, .errsz = errsz };
   NIR_PASS(_, nir, nir_shader_instructions_pass, lower_instr, nir_metadata_control_flow, &c);
   if (c.failed)
      return false;
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo, MALI_DESC_BUFFER_ADDR_FORMAT);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo, MALI_DESC_BUFFER_ADDR_FORMAT);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, pack_buf_idx, nir_metadata_control_flow, NULL);
   if (layout->robust_access)    /* needs scalar buffer handles (packed above) and the rewritten image intrinsics (lower_intr) */
      NIR_PASS(_, nir, nir_lower_robust_access, robust_filter, NULL);
   /* nonuniformEXT() indices: the hardware takes one resource handle per warp, so divergent handles become a waterfall loop
    * (same set of types as PanVK; runs after the UBO/SSBO IO lowering). */
   const enum nir_lower_non_uniform_access_type types =
      nir_lower_non_uniform_ubo_access | nir_lower_non_uniform_ssbo_access | nir_lower_non_uniform_texture_access |
      nir_lower_non_uniform_texture_query | nir_lower_non_uniform_image_access | nir_lower_non_uniform_image_query |
      nir_lower_non_uniform_get_ssbo_size | nir_lower_non_uniform_texture_offset_access;
   if (nir_has_non_uniform_access(nir, types)) {
      NIR_PASS(_, nir, nir_opt_non_uniform_access);
      struct nir_lower_non_uniform_access_options opts = { .types = types };
      NIR_PASS(_, nir, nir_lower_non_uniform_access, &opts);
   }
   /* after the waterfall: a divergent size query has become per-lane loads by now (PanVK runs it in the same place) */
   if (layout->robust_access)
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_size_query, nir_metadata_control_flow, NULL);
   return true;
}
