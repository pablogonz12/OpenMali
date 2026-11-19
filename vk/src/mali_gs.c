/* SPDX-License-Identifier: MIT */
/* GS support, parts 1-3: what the GS declares (a SPIR-V module is enough), the record/index layout, the
 * passthrough vertex shader that turns records back into a draw, the body-to-compute-kernel lowering, and the
 * pipeline/draw plan. Wired through vkCreateGraphicsPipelines and vkCmdDraw under MALI_GS_EXPERIMENTAL=1
 * (vk/GEOMETRY-SHADERS.md); nothing here advertises the feature. */
#include "compiler/glsl_types.h"
#include "compiler/spirv/nir_spirv.h"
#include "panfrost/compiler/pan_compiler.h"
#include "nir.h"
#include "nir_builder.h"

#include "mali_compile.h"
#include "mali_descriptor_lower.h"
#include "mali_gs.h"

#include "util/bitscan.h"
#include "util/bitset.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- layout ---- */

/* PanVK's gpu_prerast slot maths (042-geometry-shaders-gpu-prerast.patch:554): one 16-byte slot per written
 * output, one record per emitted vertex; 2 indices per slot (a triangle strip pair), padded to 4. */
void
mali_gs_layout(struct mali_gs_info *gs, uint64_t arena_bytes)
{
   (void)arena_bytes;
   gs->record_stride = gs->record_stride ? gs->record_stride
                                         : (uint32_t)((gs->noutputs * 16 + 15) & ~15u);
   if (!gs->record_stride)
      gs->record_stride = 16;
   gs->index_stride = (2 * gs->vertices_out + 3) & ~3u;
   if (!gs->index_stride)
      gs->index_stride = 4;
}

uint32_t
mali_gs_cap_slots(const struct mali_gs_info *gs, uint64_t arena_bytes)
{
   /* Half the arena for records, half for indices (PanVK splits its arena the same way). */
   uint64_t rec = (arena_bytes / 2) / ((uint64_t)gs->record_stride * (gs->vertices_out ? gs->vertices_out : 1));
   uint64_t idx = (arena_bytes / 2) / ((uint64_t)gs->index_stride * 4);
   uint64_t cap = rec < idx ? rec : idx;
   return cap > UINT32_MAX ? UINT32_MAX : (uint32_t)cap;
}

/* ---- SPIR-V -> GS facts ---- */

/* glsl_type singletons are refcounted process-wide; mali_compile.c owns them for the binary path, and the GS
 * entry points can be called on their own (tests, pipeline creation order). */
static pthread_once_t g_gs_once = PTHREAD_ONCE_INIT;

static void gs_init_once(void) { glsl_type_singleton_init_or_ref(); }

static nir_shader *
gs_frontend_stage(const struct mali_compile_opts *o, mesa_shader_stage stage, char *err, size_t errsz)
{
   const nir_shader_compiler_options *opts = pan_get_nir_shader_compiler_options(o->arch, stage, false);
   struct spirv_to_nir_options so = {
      .environment = NIR_SPIRV_VULKAN,
      .ubo_addr_format = o->layout ? MALI_DESC_BUFFER_ADDR_FORMAT : nir_address_format_32bit_index_offset,
      .ssbo_addr_format = o->layout ? MALI_DESC_BUFFER_ADDR_FORMAT : nir_address_format_64bit_global_32bit_offset,
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ubo_alignment = 16, .min_ssbo_alignment = 16,
      .push_const_addr_format = nir_address_format_32bit_offset,
   };
   /* No specialization in the analyze path: the GS limits (vertices_out, invocations, ...) are execution modes,
    * not spec constants, and DXVK only specializes the body. */
   struct nir_spirv_specialization spec = {0};
   struct nir_spirv_specialization_entry se[32];
   if (o->nspec && stage == MESA_SHADER_VERTEX) {   /* the VS body is specialized like the graphics VS (mali_compile.c) */
      for (unsigned i = 0; i < o->nspec && i < 32; i++)
         se[spec.num_entries++] = (struct nir_spirv_specialization_entry){.id = o->spec[i].id, .size = o->spec[i].size,
                                                                          .data = (uint8_t *)o->spec_data + o->spec[i].offset};
      spec.entries = se;
   }
   nir_shader *nir = spirv_to_nir(o->spirv, o->spirv_words, spec.num_entries ? &spec : NULL, stage,
                                  o->entry ? o->entry : "main", &so, opts);
   if (!nir) {
      snprintf(err, errsz, "spirv_to_nir(%s) failed", stage == MESA_SHADER_VERTEX ? "vertex" : "geometry");
      return NULL;
   }
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp | nir_var_shader_out);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_system_values);
   pan_preprocess_nir(nir, o->arch);
   return nir;
}

static nir_shader *
gs_frontend(const struct mali_compile_opts *o, char *err, size_t errsz)
{
   return gs_frontend_stage(o, MESA_SHADER_GEOMETRY, err, errsz);
}

/* Components the record slot must hold. Structs (gl_PerVertex is split by nir_split_per_member_structs on the
 * VS path, not here) only expose their first member's size; the GS kernel stores what the store_output
 * intrinsics write, and mali_gs.h's 16-byte slot holds every legal size. */
static unsigned
var_ncomps(const nir_variable *var)
{
   switch (var->data.location) {
   case VARYING_SLOT_PSIZ:
   case VARYING_SLOT_LAYER:
   case VARYING_SLOT_VIEWPORT:
   case VARYING_SLOT_PRIMITIVE_ID:
      return 1;
   default:
      break;
   }
   const struct glsl_type *t = var->type;
   if (glsl_type_is_array(t))
      t = glsl_get_array_element(t);
   if (glsl_type_is_struct_or_ifc(t) && glsl_get_length(t))
      t = glsl_get_struct_field(t, 0);
   unsigned n = glsl_type_is_vector(t) || glsl_type_is_scalar(t) ? glsl_get_vector_elements(t) : 4;
   return n ? (n > 4 ? 4 : n) : 4;
}

int
mali_gs_analyze(const struct mali_compile_opts *o, struct mali_gs_info *out, char *err, size_t errsz)
{
   if (!o || !o->spirv || o->spirv_words < 5 || !out) {
      snprintf(err, errsz, "bad arguments");
      return -1;
   }
   pthread_once(&g_gs_once, gs_init_once);
   char e[256] = "";
   nir_shader *nir = gs_frontend(o, e, sizeof e);
   if (!nir) {
      snprintf(err, errsz, "%s", e);
      return -1;
   }

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   nir_assign_io_var_locations(nir, nir_var_shader_out);

   memset(out, 0, sizeof *out);
   out->outputs_written = nir->info.outputs_written;
   out->vertices_out = nir->info.gs.vertices_out;
   out->invocations = nir->info.gs.invocations ? nir->info.gs.invocations : 1;
   out->input_prim = nir->info.gs.input_primitive;
   out->output_prim = nir->info.gs.output_primitive;

   nir_foreach_shader_out_variable(var, nir) {
      if (var->data.location < 0 || out->noutputs >= MALI_GS_MAX_OUTPUTS)
         continue;
      struct mali_gs_output *s = &out->output[out->noutputs++];
      s->location = var->data.location;
      s->ncomps = var_ncomps(var);
      s->alu_type = var->data.location == VARYING_SLOT_PSIZ ? 0 : nir_type_float32;
   }

   mali_gs_layout(out, 0);
   if (!out->vertices_out) {
      snprintf(err, errsz, "geometry shader declares max_vertices = 0");
      ralloc_free(nir);
      return -1;
   }
   ralloc_free(nir);
   return 0;
}

/* ---- passthrough vertex shader ---- */

struct nir_shader *
mali_gs_passthrough_nir(const struct nir_shader_compiler_options *opts,
                        const struct mali_gs_info *gs, unsigned pc_off)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "mali_gs_passthrough");
   /* struct mali_gs_params.records is the second 64-bit field: pc_off + 8. */
   nir_def *lo = nir_load_push_constant(&b, 2, 32, nir_imm_int(&b, pc_off + 8));
   nir_def *base = nir_pack_64_2x32(&b, lo);
   nir_def *off = nir_imul_imm(&b, nir_load_vertex_id(&b), gs->record_stride);
   nir_def *record = nir_iadd(&b, base, nir_u2u64(&b, off));

   unsigned idx = 0, slot = 0;
   for (unsigned i = 0; i < gs->noutputs; i++) {
      const struct mali_gs_output *s = &gs->output[i];
      if (s->location == VARYING_SLOT_VIEWPORT)
         continue;   /* no multi-viewport: the slot is dropped, as in PanVK */
      nir_def *value = nir_load_global(&b, s->ncomps ? s->ncomps : 4, 32,
                                       nir_iadd_imm(&b, record, slot * 16), .align_mul = 4);
      nir_store_output(&b, value, nir_imm_int(&b, 0), .base = idx++,
                       .write_mask = (1u << (s->ncomps ? s->ncomps : 4)) - 1,
                       .src_type = s->alu_type ? s->alu_type : nir_type_float32,
                       .io_semantics = {.location = s->location, .num_slots = 1});
      slot++;
   }

   b.shader->info.outputs_written = gs->outputs_written & ~VARYING_BIT_VIEWPORT;
   b.shader->num_outputs = idx;
   return b.shader;
}

/* ---- GS body -> compute kernel ---- */

static unsigned
gs_type_size(const struct glsl_type *t, bool bindless)
{
   return glsl_count_attribute_slots(t, false);
}

static unsigned
gs_verts_per_prim(unsigned prim)
{
   switch (prim) {
   case MESA_PRIM_POINTS: return 1;
   case MESA_PRIM_LINES: return 2;
   case MESA_PRIM_TRIANGLES: return 3;
   default: return 0;
   }
}

struct gs_lower {
   nir_builder b;
   const struct mali_gs_info *gs;
   uint64_t vs_outputs;
   unsigned flags;
   nir_def *prim, *slot, *vs_records, *rec_base, *idx_base;
   nir_variable *vcount, *ipos;
   nir_variable *shadow[MALI_GS_MAX_OUTPUTS];
};

static int
gs_out_index(const struct mali_gs_info *gs, unsigned loc)
{
   for (unsigned i = 0; i < gs->noutputs; i++)
      if (gs->output[i].location == loc)
         return (int)i;
   return -1;
}

static nir_def *
gs_load_param64(nir_builder *b, unsigned pc_off, unsigned field)
{
   return nir_pack_64_2x32(b, nir_load_push_constant(b, 2, 32, nir_imm_int(b, pc_off + field)));
}

static void
gs_lower_emit(struct gs_lower *L, nir_instr *instr)
{
   nir_builder *b = &L->b;
   const struct mali_gs_info *gs = L->gs;
   b->cursor = nir_before_instr(instr);
   nir_def *vc = nir_load_var(b, L->vcount);
   nir_def *ip = nir_load_var(b, L->ipos);
   /* Truncation: EmitVertex past max_vertices emits nothing (it must not spill into the next slot). */
   nir_def *fits = nir_ult_imm(b, vc, gs->vertices_out);
   if (L->flags & MALI_GS_LOWER_TEST_NO_TRUNC)
      fits = nir_imm_true(b);
   nir_push_if(b, fits);
   {
      nir_def *rec = nir_iadd(b, L->rec_base, nir_u2u64(b, nir_imul_imm(b, vc, gs->record_stride)));
      unsigned slot = 0;
      for (unsigned i = 0; i < gs->noutputs; i++) {
         const struct mali_gs_output *s = &gs->output[i];
         if (s->location == VARYING_SLOT_VIEWPORT)
            continue;
         unsigned n = s->ncomps ? s->ncomps : 4;
         nir_def *v = nir_load_var(b, L->shadow[i]);
         nir_store_global(b, nir_trim_vector(b, v, n), nir_iadd_imm(b, rec, slot * 16),
                          .write_mask = (1u << n) - 1, .align_mul = 4);
         slot++;
      }
      /* Record index of this vertex, appended at the current index position. */
      nir_def *ridx = nir_iadd(b, nir_imul_imm(b, L->slot, gs->vertices_out), vc);
      nir_push_if(b, nir_ult_imm(b, ip, gs->index_stride));
      {
         nir_def *a = nir_iadd(b, L->idx_base, nir_u2u64(b, nir_imul_imm(b, ip, 4)));
         nir_store_global(b, ridx, a, .write_mask = 1, .align_mul = 4);
      }
      nir_pop_if(b, NULL);
      nir_store_var(b, L->vcount, nir_iadd_imm(b, vc, 1), 1);
      nir_store_var(b, L->ipos, nir_iadd_imm(b, ip, 1), 1);
   }
   nir_pop_if(b, NULL);
   nir_instr_remove(instr);
}

static void
gs_lower_end_primitive(struct gs_lower *L, nir_instr *instr)
{
   nir_builder *b = &L->b;
   b->cursor = nir_before_instr(instr);
   /* The slot already holds the restart value: just step past it (never beyond the slot). */
   nir_def *ip = nir_load_var(b, L->ipos);
   nir_def *adv = nir_umin(b, nir_iadd_imm(b, ip, 1), nir_imm_int(b, L->gs->index_stride));
   nir_store_var(b, L->ipos, adv, 1);
   nir_instr_remove(instr);
}

static void
gs_lower_store_output(struct gs_lower *L, nir_intrinsic_instr *intr)
{
   nir_builder *b = &L->b;
   b->cursor = nir_before_instr(&intr->instr);
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   unsigned loc = sem.location + (nir_src_is_const(intr->src[1]) ? nir_src_as_uint(intr->src[1]) : 0);
   int i = gs_out_index(L->gs, loc);
   if (i >= 0) {
      unsigned comp = nir_intrinsic_component(intr), mask = nir_intrinsic_write_mask(intr);
      nir_def *v = intr->src[0].ssa, *c[4];
      for (unsigned k = 0; k < 4; k++)
         c[k] = (k >= comp && k - comp < v->num_components) ? nir_channel(b, v, k - comp) : nir_undef(b, 1, 32);
      nir_store_var(b, L->shadow[i], nir_vec(b, c, 4), (mask << comp) & 0xf);
   }
   nir_instr_remove(&intr->instr);
}

static void
gs_lower_load_input(struct gs_lower *L, nir_intrinsic_instr *intr)
{
   nir_builder *b = &L->b;
   b->cursor = nir_before_instr(&intr->instr);
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   unsigned loc = sem.location + (nir_src_is_const(intr->src[1]) ? nir_src_as_uint(intr->src[1]) : 0);
   unsigned n = intr->def.num_components;
   nir_def *res;
   if (!(L->vs_outputs & BITFIELD64_BIT(loc))) {
      res = nir_imm_zero(b, n, 32);
   } else {
      unsigned vs_stride = util_bitcount64(L->vs_outputs) * 16;
      unsigned slot = util_bitcount64(L->vs_outputs & BITFIELD64_MASK(loc));
      unsigned vpp = gs_verts_per_prim(L->gs->input_prim);
      nir_def *vtx = nir_iadd(b, nir_imul_imm(b, L->prim, vpp), intr->src[0].ssa);
      nir_def *a = nir_iadd(b, L->vs_records, nir_u2u64(b, nir_imul_imm(b, vtx, vs_stride)));
      a = nir_iadd_imm(b, a, slot * 16 + nir_intrinsic_component(intr) * 4);
      res = nir_load_global(b, n, 32, a, .align_mul = 4);
   }
   nir_def_replace(&intr->def, res);
}

struct nir_shader *
mali_gs_kernel_nir(const struct mali_compile_opts *o, const struct mali_gs_info *gs, uint64_t vs_outputs,
                   unsigned pc_off, unsigned flags, char *err, size_t errsz)
{
   if (!gs_verts_per_prim(gs->input_prim)) {
      snprintf(err, errsz, "GS input primitive %u not supported by the kernel lowering yet", gs->input_prim);
      return NULL;
   }
   if (gs->invocations != 1) {
      snprintf(err, errsz, "GS invocations > 1 not supported by the kernel lowering yet");
      return NULL;
   }
   pthread_once(&g_gs_once, gs_init_once);
   nir_shader *nir = gs_frontend(o, err, errsz);
   if (!nir)
      return NULL;
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   nir_assign_io_var_locations(nir, nir_var_shader_out);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out, gs_type_size, (nir_lower_io_options)0);

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   struct gs_lower L = { .gs = gs, .vs_outputs = vs_outputs, .flags = flags };
   L.b = nir_builder_at(nir_before_impl(impl));
   nir_builder *b = &L.b;

   for (unsigned i = 0; i < gs->noutputs; i++)
      L.shadow[i] = nir_local_variable_create(impl, glsl_vec4_type(), "gs_out");
   L.vcount = nir_local_variable_create(impl, glsl_uint_type(), "vcount");
   L.ipos = nir_local_variable_create(impl, glsl_uint_type(), "ipos");

   nir_def *gid = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   L.prim = gid;
   L.slot = gid;   /* invocations == 1: slot == primitive */
   nir_def *vcnt = nir_load_push_constant(b, 1, 32, nir_imm_int(b, pc_off + 32));
   nir_def *cap = nir_load_push_constant(b, 1, 32, nir_imm_int(b, pc_off + 44));
   nir_def *nprims = nir_udiv_imm(b, vcnt, gs_verts_per_prim(gs->input_prim));
   nir_def *oob = nir_ior(b, nir_uge(b, gid, nprims), nir_uge(b, gid, cap));
   nir_push_if(b, oob);
   nir_jump(b, nir_jump_return);
   nir_pop_if(b, NULL);

   L.vs_records = gs_load_param64(b, pc_off, 0);
   nir_def *records = gs_load_param64(b, pc_off, 8), *indices = gs_load_param64(b, pc_off, 16);
   L.rec_base = nir_iadd(b, records, nir_u2u64(b, nir_imul_imm(b, gid, gs->vertices_out * gs->record_stride)));
   L.idx_base = nir_iadd(b, indices, nir_u2u64(b, nir_imul_imm(b, gid, gs->index_stride * 4)));
   nir_store_var(b, L.vcount, nir_imm_int(b, 0), 1);
   nir_store_var(b, L.ipos, nir_imm_int(b, 0), 1);
   /* Every unwritten index slot is a restart. */
   for (unsigned i = 0; i < gs->index_stride; i += 4)
      nir_store_global(b, nir_imm_ivec4(b, -1, -1, -1, -1), nir_iadd_imm(b, L.idx_base, i * 4),
                       .write_mask = 0xf, .align_mul = 4);

   /* Collect first: the rewrites split blocks. */
   nir_instr **list = NULL;
   unsigned n = 0, cap_n = 0;
   nir_foreach_block(block, impl)
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         switch (nir_instr_as_intrinsic(instr)->intrinsic) {
         case nir_intrinsic_emit_vertex:
         case nir_intrinsic_end_primitive:
         case nir_intrinsic_store_output:
         case nir_intrinsic_load_per_vertex_input:
         case nir_intrinsic_load_primitive_id:
         case nir_intrinsic_load_invocation_id:
            if (n == cap_n)
               list = realloc(list, (cap_n = cap_n ? cap_n * 2 : 64) * sizeof *list);
            list[n++] = instr;
            break;
         default:
            break;
         }
      }
   for (unsigned i = 0; i < n; i++) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(list[i]);
      switch (intr->intrinsic) {
      case nir_intrinsic_emit_vertex: gs_lower_emit(&L, list[i]); break;
      case nir_intrinsic_end_primitive: gs_lower_end_primitive(&L, list[i]); break;
      case nir_intrinsic_store_output: gs_lower_store_output(&L, intr); break;
      case nir_intrinsic_load_per_vertex_input: gs_lower_load_input(&L, intr); break;
      case nir_intrinsic_load_primitive_id:
         b->cursor = nir_before_instr(list[i]);
         nir_def_replace(&intr->def, L.prim);
         break;
      default:   /* invocation id: always 0 with one invocation */
         b->cursor = nir_before_instr(list[i]);
         nir_def_replace(&intr->def, nir_imm_int(b, 0));
         break;
      }
   }
   free(list);

   nir->info.stage = MESA_SHADER_COMPUTE;
   memset(&nir->info.cs, 0, sizeof nir->info.cs);
   nir->info.workgroup_size[0] = nir->info.workgroup_size[1] = nir->info.workgroup_size[2] = 1;
   nir->info.inputs_read = nir->info.outputs_written = 0;
   nir->num_inputs = nir->num_outputs = 0;
   nir_foreach_variable_with_modes_safe(var, nir, nir_var_shader_in | nir_var_shader_out)
      exec_node_remove(&var->node);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_opt_cse);
   nir_validate_shader(nir, "mali_gs_kernel_nir");
   return nir;
}


/* ---- app VS -> record-writing compute kernel ---- */

struct vsk {
   nir_builder b;
   const struct mali_gs_vs_attr *attr;
   unsigned nattr, pc_off;
   uint64_t vs_outputs;
   nir_def *gid, *rec, *first_vertex;
   bool bad;
};

static nir_def *
vsk_vb(struct vsk *K, unsigned binding)
{
   return nir_pack_64_2x32(&K->b, nir_load_push_constant(&K->b, 2, 32, nir_imm_int(&K->b, K->pc_off + 64 + 8 * binding)));
}

static void
vsk_load_input(struct vsk *K, nir_intrinsic_instr *intr)
{
   nir_builder *b = &K->b;
   b->cursor = nir_before_instr(&intr->instr);
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   unsigned loc = sem.location + (nir_src_is_const(intr->src[0]) ? nir_src_as_uint(intr->src[0]) : 0);
   const struct mali_gs_vs_attr *a = NULL;
   for (unsigned i = 0; i < K->nattr; i++)
      if (loc >= VERT_ATTRIB_GENERIC0 && K->attr[i].location == loc - VERT_ATTRIB_GENERIC0)
         a = &K->attr[i];
   unsigned n = intr->def.num_components, c0 = nir_intrinsic_component(intr);
   if (intr->def.bit_size != 32) {
      K->bad = true;
      return;
   }
   nir_def *comp[4];
   nir_def *vtx = nir_u2u64(b, nir_iadd(b, K->gid, K->first_vertex));
   for (unsigned k = 0; k < n; k++) {
      unsigned c = c0 + k;
      if (a && c < a->ncomps) {
         nir_def *addr = nir_iadd(b, vsk_vb(K, a->binding), nir_imul_imm(b, vtx, a->stride));
         addr = nir_iadd_imm(b, addr, a->offset + c * 4);
         comp[k] = nir_load_global(b, 1, 32, addr, .align_mul = 4);
      } else {
         comp[k] = nir_imm_floatN_t(b, c == 3 ? 1.0 : 0.0, 32);   /* missing components: (0, 0, 0, 1) */
      }
   }
   nir_def_replace(&intr->def, nir_vec(b, comp, n));
}

static void
vsk_store_output(struct vsk *K, nir_intrinsic_instr *intr)
{
   nir_builder *b = &K->b;
   b->cursor = nir_before_instr(&intr->instr);
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   unsigned loc = sem.location + (nir_src_is_const(intr->src[1]) ? nir_src_as_uint(intr->src[1]) : 0);
   if ((K->vs_outputs & BITFIELD64_BIT(loc)) && intr->src[0].ssa->bit_size == 32) {
      unsigned slot = util_bitcount64(K->vs_outputs & BITFIELD64_MASK(loc)), comp = nir_intrinsic_component(intr);
      unsigned mask = nir_intrinsic_write_mask(intr);
      u_foreach_bit(k, mask) {
         nir_def *addr = nir_iadd_imm(b, K->rec, slot * 16 + (comp + k) * 4);
         nir_store_global(b, nir_channel(b, intr->src[0].ssa, k), addr, .write_mask = 1, .align_mul = 4);
      }
   }
   nir_instr_remove(&intr->instr);
}

struct nir_shader *
mali_gs_vs_kernel_nir(const struct mali_compile_opts *o, const struct mali_gs_vs_attr *attr, unsigned nattr,
                      unsigned pc_off, uint64_t *vs_outputs, char *err, size_t errsz)
{
   pthread_once(&g_gs_once, gs_init_once);
   nir_shader *nir = gs_frontend_stage(o, MESA_SHADER_VERTEX, err, errsz);
   if (!nir)
      return NULL;
   NIR_PASS(_, nir, nir_split_per_member_structs);
   nir_foreach_shader_in_variable(var, nir)
      var->data.driver_location = var->data.location - VERT_ATTRIB_GENERIC0;
   nir_assign_io_var_locations(nir, nir_var_shader_out);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out, gs_type_size, (nir_lower_io_options)0);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   *vs_outputs = nir->info.outputs_written;

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   struct vsk K = { .attr = attr, .nattr = nattr, .pc_off = pc_off, .vs_outputs = nir->info.outputs_written };
   K.b = nir_builder_at(nir_before_impl(impl));
   nir_builder *b = &K.b;
   K.gid = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   K.first_vertex = nir_load_push_constant(b, 1, 32, nir_imm_int(b, pc_off + 60));
   nir_def *vcnt = nir_load_push_constant(b, 1, 32, nir_imm_int(b, pc_off + 32));
   nir_push_if(b, nir_uge(b, K.gid, vcnt));
   nir_jump(b, nir_jump_return);
   nir_pop_if(b, NULL);
   nir_def *records = nir_pack_64_2x32(b, nir_load_push_constant(b, 2, 32, nir_imm_int(b, pc_off + 0)));
   unsigned vs_stride = util_bitcount64(K.vs_outputs) * 16;
   K.rec = nir_iadd(b, records, nir_u2u64(b, nir_imul_imm(b, K.gid, vs_stride)));

   nir_instr **list = NULL;
   unsigned n = 0, cap_n = 0;
   nir_foreach_block(block, impl)
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         switch (nir_instr_as_intrinsic(instr)->intrinsic) {
         case nir_intrinsic_load_input:
         case nir_intrinsic_store_output:
         case nir_intrinsic_load_vertex_id:
         case nir_intrinsic_load_vertex_id_zero_base:
         case nir_intrinsic_load_first_vertex:
         case nir_intrinsic_load_base_vertex:
         case nir_intrinsic_load_instance_id:
         case nir_intrinsic_load_base_instance:
            if (n == cap_n)
               list = realloc(list, (cap_n = cap_n ? cap_n * 2 : 64) * sizeof *list);
            list[n++] = instr;
            break;
         default:
            break;
         }
      }
   for (unsigned i = 0; i < n; i++) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(list[i]);
      b->cursor = nir_before_instr(list[i]);
      switch (intr->intrinsic) {
      case nir_intrinsic_load_input: vsk_load_input(&K, intr); break;
      case nir_intrinsic_store_output: vsk_store_output(&K, intr); break;
      case nir_intrinsic_load_vertex_id: nir_def_replace(&intr->def, nir_iadd(b, K.gid, K.first_vertex)); break;
      case nir_intrinsic_load_vertex_id_zero_base: nir_def_replace(&intr->def, K.gid); break;
      case nir_intrinsic_load_first_vertex:
      case nir_intrinsic_load_base_vertex: nir_def_replace(&intr->def, K.first_vertex); break;
      default: nir_def_replace(&intr->def, nir_imm_int(b, 0)); break;   /* instance id / base instance: instance_count is 1 */
      }
   }
   free(list);
   if (K.bad) {
      snprintf(err, errsz, "VS input with a non-32-bit component type: not supported by the record kernel yet");
      ralloc_free(nir);
      return NULL;
   }

   nir->info.stage = MESA_SHADER_COMPUTE;
   memset(&nir->info.cs, 0, sizeof nir->info.cs);
   nir->info.workgroup_size[0] = 64;
   nir->info.workgroup_size[1] = nir->info.workgroup_size[2] = 1;
   nir->info.inputs_read = nir->info.outputs_written = 0;
   nir->num_inputs = nir->num_outputs = 0;
   nir_foreach_variable_with_modes_safe(var, nir, nir_var_shader_in | nir_var_shader_out)
      exec_node_remove(&var->node);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_opt_dce);
   nir_validate_shader(nir, "mali_gs_vs_kernel_nir");
   return nir;
}

/* ---- pipeline build ---- */

bool
mali_gs_enabled(void)
{
   const char *e = getenv("MALI_GS_EXPERIMENTAL");
   return e && *e && strcmp(e, "0");
}

uint32_t
mali_gs_prims(const struct mali_gs_info *gs, uint32_t vertex_count)
{
   unsigned vpp = gs_verts_per_prim(gs->input_prim);
   return vpp ? vertex_count / vpp : 0;
}

int
mali_gs_plan_draw(const struct mali_gs_pipe *p, uint32_t vertex_count, struct mali_gs_draw_plan *out)
{
   memset(out, 0, sizeof *out);
   out->vcount = vertex_count;
   out->prims = mali_gs_prims(&p->info, vertex_count);
   out->out_prim = p->out_prim;
   out->index_stride = p->info.index_stride;
   uint64_t rec = (uint64_t)out->prims * p->info.vertices_out * p->info.record_stride;
   uint64_t vsrec = (uint64_t)vertex_count * util_bitcount64(p->vs_outputs) * 16;
   uint64_t ind = (uint64_t)out->prims * p->info.index_stride * 4;
   uint64_t icount = (uint64_t)out->prims * p->info.index_stride;
   if (rec > UINT32_MAX || vsrec > UINT32_MAX || ind > UINT32_MAX || icount > UINT32_MAX)
      return -EINVAL;
   out->records_bytes = (uint32_t)rec;
   out->vs_records_bytes = (uint32_t)vsrec;
   out->indices_bytes = (uint32_t)ind;
   out->index_count = (uint32_t)icount;
   /* 64-thread workgroups: the VS-record kernel runs once per vertex, the GS kernel once per input primitive */
   out->vs_dispatch[0] = (vertex_count + 63) / 64;
   out->vs_dispatch[1] = out->vs_dispatch[2] = 1;
   out->dispatch[0] = (out->prims + 63) / 64;
   out->dispatch[1] = out->dispatch[2] = 1;
   out->empty = out->prims == 0;
   return 0;
}

void
mali_gs_pad_indices(uint32_t *idx, uint32_t cap_slots, uint32_t index_stride)
{
   for (uint64_t i = 0, n = (uint64_t)cap_slots * index_stride; i < n; i++)
      idx[i] = 0xffffffffu;
}

void
mali_gs_pipe_free(struct mali_gs_pipe *p)
{
   mali_compiled_free(&p->vs_k);
   mali_compiled_free(&p->gs_k);
   mali_compiled_free(&p->pt);
}

int
mali_gs_pipe_build(const struct mali_compile_opts *ovs, const struct mali_compile_opts *ogs,
                   const struct mali_gs_vs_attr *attr, unsigned nattr, unsigned push_size, struct mali_gs_pipe *out,
                   char *err, size_t errsz)
{
   memset(out, 0, sizeof *out);
   struct mali_gs_info *gs = &out->info;
   if (mali_gs_analyze(ogs, gs, err, errsz))
      return -1;
   if (!gs_verts_per_prim(gs->input_prim)) {
      snprintf(err, errsz, "GS input primitive %u (adjacency / patches) not supported", gs->input_prim);
      return -1;
   }
   if (gs->invocations != 1) {
      snprintf(err, errsz, "GS invocations > 1 not supported");
      return -1;
   }
   switch (gs->output_prim) {
   case MESA_PRIM_POINTS: out->out_prim = 1; break;
   case MESA_PRIM_LINE_STRIP: out->out_prim = 4; break;
   case MESA_PRIM_TRIANGLE_STRIP: out->out_prim = 10; break;
   default:
      snprintf(err, errsz, "GS output primitive %u not supported", gs->output_prim);
      return -1;
   }
   if (nattr > MALI_GS_MAX_ATTR) {
      snprintf(err, errsz, "GS pipeline: %u vertex attributes > %u", nattr, MALI_GS_MAX_ATTR);
      return -1;
   }
   for (unsigned i = 0; i < nattr; i++) {
      if (attr[i].binding >= MALI_GS_MAX_VB) {
         snprintf(err, errsz, "GS pipeline: vertex binding %u >= %u", attr[i].binding, MALI_GS_MAX_VB);
         return -1;
      }
      out->attr[i] = attr[i];
   }
   out->nattr = nattr;
   out->pc_off = (push_size + 7) & ~7u;
   if (out->pc_off + sizeof(struct mali_gs_params) > 128) {   /* MALI_PUSH_MAX */
      snprintf(err, errsz, "GS pipeline: push constants (%u B) + params (%zu B) exceed the push block", push_size,
               sizeof(struct mali_gs_params));
      return -1;
   }

   /* the FAU words holding the params block must be reserved, and the shaders read it at pc_off from byte 0 */
   const unsigned fau_res = (out->pc_off + sizeof(struct mali_gs_params) + 3) / 4;
   struct mali_compile_opts k = *ovs;
   k.stage = MALI_STAGE_COMPUTE;
   k.entry = ovs->entry;
   k.fau_reserved = fau_res;
   k.push_base_set = 1;
   k.push_base = 0;
   k.strip_psiz = 0;
   struct nir_shader *nir = mali_gs_vs_kernel_nir(ovs, attr, nattr, out->pc_off, &out->vs_outputs, err, errsz);
   if (!nir || mali_compile_nir(&k, nir, &out->vs_k, err, errsz))
      goto fail;

   k = *ogs;
   k.stage = MALI_STAGE_COMPUTE;
   k.fau_reserved = fau_res;
   k.push_base_set = 1;
   k.push_base = 0;
   k.strip_psiz = 0;
   nir = mali_gs_kernel_nir(ogs, gs, out->vs_outputs, out->pc_off, 0, err, errsz);
   if (!nir || mali_compile_nir(&k, nir, &out->gs_k, err, errsz))
      goto fail;

   const nir_shader_compiler_options *vo = pan_get_nir_shader_compiler_options(ogs->arch, MESA_SHADER_VERTEX, false);
   nir = mali_gs_passthrough_nir(vo, gs, out->pc_off);
   k = *ogs;
   k.stage = MALI_STAGE_VERTEX;
   k.spirv = NULL;
   k.nspec = 0;
   k.fau_reserved = fau_res;
   k.push_base_set = 1;
   k.push_base = 0;
   k.strip_psiz = out->out_prim != 1;
   if (!nir || mali_compile_nir(&k, nir, &out->pt, err, errsz))
      goto fail;
   return 0;
fail:
   mali_gs_pipe_free(out);
   return -1;
}
