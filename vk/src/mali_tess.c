/* SPDX-License-Identifier: MIT */
/* The T1 classifier, in NIR (see mali_tess.h for the accepted shape and why it is folded rather than matched).
 *
 * Two checks, both on preprocessed NIR:
 *   1. the vertex shader's tess levels are the constants 1.0 — otherwise the patch would be subdivided and the vertex
 *      stream the VS writes would not be the one the tessellator would produce;
 *   2. for each of the three output vertices i, the TES evaluated at gl_TessCoord = e_i stores, for every output, the
 *      patch input of the same varying and component at control point i. That is checked after Mesa folds the constant
 *      into the body, so any arithmetic the shader does is evaluated rather than guessed. */
#include "compiler/glsl_types.h"
#include "compiler/shader_enums.h"
#include "nir.h"
#include "nir_builder.h"

#include "mali_poly.h"
#include "mali_tcs.h"
#include "mali_tess.h"

#include <stdarg.h>
#include <stdio.h>

static int
refuse(char *why, size_t whysz, const char *fmt, ...)
{
   if (!why || !whysz)
      return 0;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(why, whysz, fmt, ap);
   va_end(ap);
   if (!why[0])
      snprintf(why, whysz, "refused");
   return 0;
}

/* One store_output the TES performed at output vertex i, and the patch input it was copied from (if any). */
struct store {
   gl_varying_slot location;
   unsigned base, component, ncomp, bits;
   int from_vertex;   /* -1 = not a per-vertex input load (i.e. computed) */
   int same;          /* 1 = the load's varying tuple equals the store's */
};

/* Replaces every gl_TessCoord load with the constant coord, so the next optimisation passes fold the TES body. */
struct coord_key { float x, y, z; };
static bool
replace_coord(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (getenv("MALI_TESS_DBG"))
      fprintf(stderr, "mali_tess: coord-intr %u (want %u) value=(%g,%g,%g)\n", (unsigned)intr->intrinsic,
              (unsigned)nir_intrinsic_load_tess_coord, ((const struct coord_key *)data)->x,
              ((const struct coord_key *)data)->y, ((const struct coord_key *)data)->z);
   if (intr->intrinsic != nir_intrinsic_load_tess_coord)
      return false;
   const struct coord_key *k = data;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def_rewrite_uses(&intr->def, nir_imm_vec3(b, k->x, k->y, k->z));
   nir_instr_remove(&intr->instr);
   return true;
}

/* Collects the entry point's stores into `out` (up to max), recording the patch input each value came from. */
static unsigned
collect_stores(nir_shader *nir, struct store *out, unsigned max)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   unsigned n = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *st = nir_instr_as_intrinsic(instr);
         if (st->intrinsic != nir_intrinsic_store_output || n >= max)
            continue;
         struct store s = {
            .location = nir_intrinsic_io_semantics(st).location,
            .base = nir_intrinsic_base(st),
            .component = nir_intrinsic_component(st),
            .ncomp = st->src[0].ssa->num_components,
            .bits = st->src[0].ssa->bit_size,
            .from_vertex = -1,
         };
         nir_src *v = &st->src[0];
         /* the folded select leaves movs behind: a copy of a copy is still a copy */
         for (int guard = 0; guard < 8 && v->ssa; guard++) {
            nir_instr *in = nir_def_instr(v->ssa);
            if (in->type != nir_instr_type_alu)
               break;
            nir_alu_instr *alu = nir_instr_as_alu(in);
            if (getenv("MALI_TESS_DBG"))
               fprintf(stderr, "mali_tess: value alu op=%u\n", (unsigned)alu->op);
            if (alu->op != nir_op_mov || !alu->src[0].src.ssa)
               break;
            *v = alu->src[0].src;
         }
         if (v->ssa && nir_def_instr(v->ssa)->type == nir_instr_type_intrinsic) {
            nir_intrinsic_instr *ld = nir_instr_as_intrinsic(nir_def_instr(v->ssa));
            if (ld->intrinsic == nir_intrinsic_load_per_vertex_input) {
               if (nir_src_is_const(ld->src[1]))
                  s.from_vertex = (int)nir_src_as_uint(ld->src[1]);
               s.same = nir_intrinsic_io_semantics(ld).location == s.location &&
                        nir_intrinsic_base(ld) == s.base &&
                        nir_intrinsic_component(ld) == s.component &&
                        ld->def.num_components == s.ncomp && ld->def.bit_size == s.bits;
            }
         }
         out[n++] = s;
      }
   }
   return n;
}

/* The control shader is checked from its words (`mali_tcs.c`): the generic IO lowering leaves a gl_out store with a
 * temp-variable vertex index and the matching gl_in load with a constant one, so no per-invocation identity is provable
 * from the lowered NIR (this file's first attempt, PR #385). The TES below is still folded in NIR because there the
 * constant actually propagates — and because that is what the note asks for. */
static int
check_tcs(const uint32_t *words, uint32_t nwords, const char *entry, char *why, size_t whysz)
{
   return mali_tcs_passthrough(words, nwords, entry, why, whysz);
}

int
mali_tess_passthrough(unsigned arch, const uint32_t *tcs_words, uint32_t ntcs, const char *tcs_entry,
                      const uint32_t *tes_words, uint32_t ntes, const char *tes_entry, char *why, size_t whysz)
{
   if (why && whysz)
      why[0] = 0;
   if (!tcs_words || !tes_words)
      return refuse(why, whysz, "no SPIR-V in the tessellation stages");

   char err[192] = "";
   nir_shader *tes = mali_poly_nir_from_spirv(arch, tes_words, ntes, MESA_SHADER_TESS_EVAL, tes_entry, err, sizeof err);
   if (!tes)
      return refuse(why, whysz, "tessellation evaluation shader: %s", err[0] ? err : "does not parse");
   if (tes->info.tess._primitive_mode != TESS_PRIMITIVE_TRIANGLES) {
      refuse(why, whysz, "only triangles patches are accepted (this TES declares mode %u)", tes->info.tess._primitive_mode);
      ralloc_free(tes);
      return 0;
   }
   if (tes->info.tess.spacing != TESS_SPACING_EQUAL) {
      refuse(why, whysz, "only equal_spacing is accepted (this TES declares spacing %u)", tes->info.tess.spacing);
      ralloc_free(tes);
      return 0;
   }
   if (tes->info.tess.point_mode) {
      refuse(why, whysz, "point_mode TES needs the tessellator (lane T2)");
      ralloc_free(tes);
      return 0;
   }

   if (!check_tcs(tcs_words, ntcs, tcs_entry, why, whysz))
      return 0;

   /* Three output vertices: at each, the TES body must be the identity copy of the matching patch vertex. */
   static const struct coord_key coords[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
   int rc = 0;
   for (unsigned i = 0; i < 3; i++) {
      char err2[192] = "";
      nir_shader *clone = mali_poly_nir_from_spirv(arch, tes_words, ntes, MESA_SHADER_TESS_EVAL, tes_entry, err2, sizeof err2);
      if (!clone) {
         refuse(why, whysz, "tessellation evaluation shader: %s", err2[0] ? err2 : "does not parse");
         goto done;
      }
      /* gl_TessCoord is still a builtin *variable* load at this point (spirv_to_nir emits the intrinsic only when the
       * system values are lowered), so lower them first — otherwise there is no intrinsic to replace and every clone
       * silently keeps the shader's default path (which reads control point 0 and looked like a pass at e0). */
      NIR_PASS(_, clone, nir_lower_system_values);
      struct coord_key k = coords[i];
      NIR_PASS(_, clone, nir_shader_intrinsics_pass, replace_coord, nir_metadata_control_flow, &k);
      /* glslang keeps `vec3 c = gl_TessCoord` in a local variable: without SSA the coordinate is a load_deref and nothing
       * folds, which is how e0 "passed" while e1 and e2 still read control point 0. */
      NIR_PASS(_, clone, nir_lower_vars_to_ssa);
      /* Fold the constant through the body, then lower IO so the copy shows up as store_output <- load_per_vertex_input. */
      bool progress;
      do {
         progress = false;
         NIR_PASS(progress, clone, nir_opt_constant_folding);
         NIR_PASS(progress, clone, nir_opt_algebraic);
         NIR_PASS(progress, clone, nir_opt_copy_prop);
         NIR_PASS(progress, clone, nir_opt_dce);
         NIR_PASS(progress, clone, nir_opt_dead_cf);
         NIR_PASS(progress, clone, nir_opt_peephole_select, &(nir_opt_peephole_select_options){.limit = 0, .discard_ok = true});
         NIR_PASS(progress, clone, nir_opt_if,
                  nir_opt_if_optimize_phi_true_false | nir_opt_if_avoid_64bit_phis);
      } while (progress);
      if (getenv("MALI_TESS_NIR")) {
         fprintf(stderr, "--- TES at e%u after the fold ---\n", i);
         nir_print_shader(clone, stderr);
      }
      mali_poly_preprocess(clone);

      struct store st[64];
      const unsigned n = collect_stores(clone, st, 64);
      unsigned seen_pos = 0;
      const char *bad = NULL;
      for (unsigned s = 0; s < n && !bad; s++) {
         if (st[s].from_vertex < 0) bad = "an output that is not a copy of a patch input";
         else if (!st[s].same) bad = "an output copied from a different varying than the one it writes";
         else if (st[s].from_vertex != (int)i) bad = "an output copied from the wrong patch vertex";
         if (st[s].location == VARYING_SLOT_POS && !bad) seen_pos = 1;
      }
      if (getenv("MALI_TESS_DBG"))
         for (unsigned k = 0; k < n; k++)
            fprintf(stderr, "mali_tess: tes e%u store loc=%u base=%u comp=%u ncomp=%u from=%d same=%d\n", i,
                    (unsigned)st[k].location, st[k].base, st[k].component, st[k].ncomp, st[k].from_vertex, st[k].same);
      if (!bad && !n) bad = "no outputs at all";
      if (!bad && !seen_pos) bad = "no gl_Position";
      ralloc_free(clone);
      if (bad) {
         refuse(why, whysz, "%s (at gl_TessCoord = e%u)", bad, i);
         goto done;
      }
   }
   rc = 1;
done:
   ralloc_free(tes);
   return rc;
}
