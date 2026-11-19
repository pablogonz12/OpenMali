/* SPDX-License-Identifier: MIT */
/* GS support, part 1: what the GS declares (a SPIR-V module is enough), the record/index layout, and the
 * passthrough vertex shader that turns records back into a draw. The body-to-compute-kernel lowering and the
 * pipeline/draw wiring are the follow-up PRs; nothing here advertises the feature. */
#include "compiler/glsl_types.h"
#include "compiler/spirv/nir_spirv.h"
#include "panfrost/compiler/pan_compiler.h"
#include "nir.h"
#include "nir_builder.h"

#include "mali_compile.h"
#include "mali_descriptor_lower.h"
#include "mali_gs.h"

#include <pthread.h>
#include <stdio.h>
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
gs_frontend(const struct mali_compile_opts *o, char *err, size_t errsz)
{
   const nir_shader_compiler_options *opts =
      pan_get_nir_shader_compiler_options(o->arch, MESA_SHADER_GEOMETRY, false);
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
   nir_shader *nir = spirv_to_nir(o->spirv, o->spirv_words, NULL, MESA_SHADER_GEOMETRY,
                                  o->entry ? o->entry : "main", &so, opts);
   if (!nir) {
      snprintf(err, errsz, "spirv_to_nir(geometry) failed");
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
