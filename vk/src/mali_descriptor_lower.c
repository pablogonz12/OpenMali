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
   uint32_t stride = bd->stride ? bd->stride : 1;
   nir_def *h = nir_imm_int(b, first_handle(c, set, bd) + sub + imm * stride);
   if (ssa) {
      /* Dynamic index into a descriptor array. PARTIALLY_BOUND: an index that is not less than the binding's element count must
       * read as a null descriptor (D3D11/HLSL OOB semantics DXVK relies on), not the last element and not memory past the set. */
      nir_def *off = nir_imul_imm(b, ssa, stride);
      if (bd->bflags & MALI_DESC_BINDING_PARTIALLY_BOUND) {
         nir_def *in_range = nir_ult(b, ssa, nir_imm_int(b, bd->count));
         nir_def *addr = nir_iadd(b, h, off);
         h = nir_bcsel(b, in_range, addr, nir_imm_int(b, pan_res_handle(0, MALI_NULL_DESC_IDX)));
      } else {
         h = nir_iadd(b, h, off);
      }
   } else if (imm >= bd->count && (bd->bflags & MALI_DESC_BINDING_PARTIALLY_BOUND)) {
      h = nir_imm_int(b, pan_res_handle(0, MALI_NULL_DESC_IDX));
   }
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
   /* Loads are NOT given to nir_lower_robust_access: for them it only rewrites the offset to 0 (lower_buffer_load ->
    * rewrite_offset), which returns the buffer's first element - legal for robustBufferAccess's undefined-value rule, wrong for
    * robustness2, where an out-of-bounds read must give zero (CTS zero-pads the reference data to
    * robust{Uniform,Storage}BufferAccessSizeAlignment, vktRobustnessExtsTests.cpp:1049-1057). lower_load_bounds below does them. */
   switch (i->intrinsic) {   /* atomics/original image intrinsics only: stores are bounded by lower_store_bounds */
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
/* robustness2 buffer loads: zero when [offset, offset + access_size) leaves the descriptor's size, otherwise the load. Same
 * nir_if shape the store/atomic half gets from Mesa (predicate_offset -> wrap_in_if, nir_lower_robust_access.c:95-114), so it is
 * a construct the backend already turns into predication on the hardware (the store/atomic discard works on the G610). The size
 * is the get_ssbo_size/get_ubo_size that lower_size_query turns into a read of descriptor dw1; run this first so it does. */
static bool
lower_load_bounds(nir_builder *b, nir_intrinsic_instr *i, void *d)
{
   if (i->intrinsic != nir_intrinsic_load_ssbo && i->intrinsic != nir_intrinsic_load_ubo)
      return false;
   b->cursor = nir_before_instr(&i->instr);
   nir_def *h = i->src[0].ssa;
   if (h->num_components != 1)
      h = nir_iadd(b, nir_channel(b, h, 0), nir_channel(b, h, 1));
   nir_def *size = i->intrinsic == nir_intrinsic_load_ubo ? nir_get_ubo_size(b, 32, h) : nir_get_ssbo_size(b, 32, h);
   const unsigned access = i->def.num_components * i->def.bit_size / 8;
   nir_def *off = i->src[1].ssa;
   if (off->bit_size < 32)
      off = nir_u2u32(b, off);
   nir_def *max_off = nir_iadd_imm(b, off, access - 1);
   /* `off + access - 1` is a 32-bit sum and can wrap (offset 0xfffffff4, access 4 -> 3, which would pass the size
    * test and run the very load this lowering exists to drop): the wrapped end is below the start, so require both */
   nir_def *valid = nir_iand(b, nir_ult(b, max_off, size), nir_uge(b, max_off, off));
   nir_def *zero = nir_imm_zero(b, i->def.num_components, i->def.bit_size);
   nir_push_if(b, valid);
   nir_intrinsic_instr *cl = nir_instr_as_intrinsic(nir_instr_clone(b->shader, &i->instr));
   nir_builder_instr_insert(b, &cl->instr);
   nir_def *res = &cl->def;
   nir_pop_if(b, NULL);
   nir_def_rewrite_uses(&i->def, nir_if_phi(b, res, zero));
   nir_instr_remove(&i->instr);
   return true;
}

/* robustness2 buffer stores: Mesa's pass accepts the store when "offset + access_size - 1 < size", and that sum is 32-bit.
 * With scalarBlockLayout a vec4 may sit at a 4-byte-aligned offset, so offset 0xfffffff4 + 15 wraps to 3, the store passes
 * the test and writes past the descriptor. Same shape as lower_load_bounds, with the wrap made impossible. */
static bool
lower_store_bounds(nir_builder *b, nir_intrinsic_instr *i, void *d)
{
   if (i->intrinsic != nir_intrinsic_store_ssbo)
      return false;
   b->cursor = nir_before_instr(&i->instr);
   nir_def *h = i->src[1].ssa;
   if (h->num_components != 1)
      h = nir_iadd(b, nir_channel(b, h, 0), nir_channel(b, h, 1));
   nir_def *size = nir_get_ssbo_size(b, 32, h);
   /* only the components the write mask actually writes: a masked store may start at a nonzero component */
   const unsigned wmask = nir_intrinsic_write_mask(i);
   const unsigned comp0 = wmask ? (unsigned)__builtin_ctz(wmask) : 0;
   /* the span is lowest..highest written component: a sparse mask (0x5) leaves a gap that still occupies bytes, so
    * counting the enabled components would underestimate what the store touches */
   const unsigned hi = wmask ? 31u - (unsigned)__builtin_clz(wmask) : nir_src_num_components(i->src[0]) - 1;
   const unsigned esz = nir_src_bit_size(i->src[0]) / 8;
   const unsigned access = (hi - comp0 + 1) * esz;
   nir_def *off = i->src[2].ssa;
   if (off->bit_size < 32)
      off = nir_u2u32(b, off);
   nir_def *off0 = off;
   if (comp0)
      off = nir_iadd_imm(b, off, comp0 * esz);
   nir_def *max_off = nir_iadd_imm(b, off, access - 1);
   /* three wraps to reject: the component offset, the end offset, and an end below the start */
   nir_def *valid = nir_iand(b, nir_ult(b, max_off, size),
                             nir_iand(b, nir_uge(b, max_off, off), nir_uge(b, off, off0)));
   nir_push_if(b, valid);
   nir_intrinsic_instr *cl = nir_instr_as_intrinsic(nir_instr_clone(b->shader, &i->instr));
   nir_builder_instr_insert(b, &cl->instr);
   nir_pop_if(b, NULL);
   nir_instr_remove(&i->instr);
   return true;
}

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
   if (layout->robust_access) {
      /* this pass inserts an if and new blocks, so block indices and dominance are stale afterwards: Mesa passes
       * nir_metadata_none for its own robustness pass (nir_lower_robust_access.c:335) */
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_load_bounds, nir_metadata_none, NULL);
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_store_bounds, nir_metadata_none, NULL);
   }
   /* get_ssbo_size/get_ubo_size exist in SPIR-V whether or not robust access is on, and Valhall has no such
    * instruction at all: the size query has to be lowered for every shader, not only the robust ones */
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_size_query, nir_metadata_control_flow, NULL);
   return true;
}
