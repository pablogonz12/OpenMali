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

/* Every tess level is the constant 1.0 (see the file comment): both slots are TCS outputs in Vulkan, so this is where the
 * levels come from. A level written from a value (or above 1.0) would subdivide the patch. */
static int
check_levels(nir_shader *tcs, char *why, size_t whysz)
{
   unsigned outer_mask = 0, inner_mask = 0;
   nir_function_impl *impl = nir_shader_get_entrypoint(tcs);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *st = nir_instr_as_intrinsic(instr);
         /* the levels are *patch* outputs: after IO lowering they are plain store_output, not per-vertex ones */
         if (st->intrinsic != nir_intrinsic_store_output && st->intrinsic != nir_intrinsic_store_per_vertex_output)
            continue;
         const gl_varying_slot loc = nir_intrinsic_io_semantics(st).location;
         const bool outer = loc == VARYING_SLOT_TESS_LEVEL_OUTER;
         if (!outer && loc != VARYING_SLOT_TESS_LEVEL_INNER)
            continue;
         if (!nir_src_is_const(st->src[0]))
            return refuse(why, whysz, "gl_TessLevel%s is written from a value, not the constant 1.0", outer ? "Outer" : "Inner");
         const nir_const_value *c = nir_src_as_const_value(st->src[0]);
         for (unsigned i = 0; i < st->src[0].ssa->num_components; i++)
            if (c[i].f32 != 1.0f)
               return refuse(why, whysz, "gl_TessLevel%s[%u] is %g, not 1.0", outer ? "Outer" : "Inner", i, c[i].f32);
         /* IO lowering may split an array store into scalar ones: what matters is that every component 0..3 / 0..1 is
          * written, each with the constant 1.0. */
         const unsigned comp = nir_intrinsic_component(st);
         for (unsigned k = 0; k < st->src[0].ssa->num_components; k++) {
            if (outer) outer_mask |= 1u << (comp + k);
            else inner_mask |= 1u << (comp + k);
         }
      }
   }
   if (outer_mask != 0xf)
      return refuse(why, whysz, "gl_TessLevelOuter is not written as 4 components of 1.0 (mask 0x%x)", outer_mask);
   if (inner_mask != 0x3)
      return refuse(why, whysz, "gl_TessLevelInner is not written as 2 components of 1.0 (mask 0x%x)", inner_mask);
   return 1;
}

/* gl_InvocationID, however the preprocessing left it: an intrinsic, or a constant per invocation later. For a TCS the
 * invocation id *is* the control point, so this is the canonical "same control point" answer. */
static bool
is_invocation_id(nir_src s)
{
   if (!s.ssa)
      return false;
   nir_instr *instr = nir_def_instr(s.ssa);
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   return nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_invocation_id;
}

/* gl_InvocationID -> the constant invocation, so an identity TCS folds to constant per-control-point copies before the
 * preprocessing runs (the same trick as the TES coord: evaluate, do not pattern-match). */
static bool
replace_invocation(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_invocation_id)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def_rewrite_uses(&intr->def, nir_imm_int(b, (int)(intptr_t)data));
   nir_instr_remove(&intr->instr);
   return true;
}

/* The TCS must be the identity patch: at every invocation i, every per-vertex output is the per-vertex input of the *same*
 * varying from control point i. Anything else (a TCS that moves a vertex, adds a varying or reorders control points)
 * changes the stream the VS wrote, and needs T2/T3. */
static int
check_tcs_identity(nir_shader *tcs_raw, char *why, size_t whysz)
{
   unsigned n = 0;
   for (unsigned i = 0; i < 3; i++) {
   nir_shader *tcs = nir_shader_clone(NULL, tcs_raw);
   NIR_PASS(_, tcs, nir_shader_intrinsics_pass, replace_invocation, nir_metadata_control_flow, (void *)(intptr_t)i);
   mali_poly_preprocess(tcs);
   nir_function_impl *impl = nir_shader_get_entrypoint(tcs);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *st = nir_instr_as_intrinsic(instr);
         if (st->intrinsic != nir_intrinsic_store_per_vertex_output)
            continue;
         const gl_varying_slot loc = nir_intrinsic_io_semantics(st).location;
         if (loc == VARYING_SLOT_TESS_LEVEL_OUTER || loc == VARYING_SLOT_TESS_LEVEL_INNER)
            continue;   /* the level slots are T1's own business (check_levels) */
         n++;
         if (getenv("MALI_TESS_DBG"))
            fprintf(stderr, "mali_tess: store loc=%u base=%u comp=%u ncomp=%u vert=%d\n", (unsigned)loc,
                    nir_intrinsic_base(st), nir_intrinsic_component(st), st->src[0].ssa->num_components,
                    nir_src_is_const(st->src[1]) ? (int)nir_src_as_uint(st->src[1]) : -1);
         nir_intrinsic_instr *ld = NULL;
         if (st->src[0].ssa && nir_def_instr(st->src[0].ssa)->type == nir_instr_type_intrinsic) {
            nir_intrinsic_instr *cand = nir_instr_as_intrinsic(nir_def_instr(st->src[0].ssa));
            if (cand->intrinsic == nir_intrinsic_load_per_vertex_input)
               ld = cand;
         }
         if (!ld)
            return refuse(why, whysz, "a tessellation control output that is not a copy of the patch input");
         if (nir_intrinsic_io_semantics(ld).location != loc || nir_intrinsic_base(ld) != nir_intrinsic_base(st) ||
             nir_intrinsic_component(ld) != nir_intrinsic_component(st) ||
             ld->def.num_components != st->src[0].ssa->num_components || ld->def.bit_size != st->src[0].ssa->bit_size)
            return refuse(why, whysz, "a tessellation control output copied from a different varying than the one it writes");
         /* the same control point: the same index value, whether a loop variable or a constant */
         const bool same_index = ld->src[1].ssa == st->src[1].ssa ||
            (nir_src_is_const(ld->src[1]) && nir_src_is_const(st->src[1]) &&
             nir_src_as_uint(ld->src[1]) == nir_src_as_uint(st->src[1])) ||
            (is_invocation_id(ld->src[1]) && is_invocation_id(st->src[1]));
         if (getenv("MALI_TESS_DBG")) {
            nir_instr *a = nir_def_instr(ld->src[1].ssa), *b = nir_def_instr(st->src[1].ssa);
            fprintf(stderr, "mali_tess: load loc=%u base=%u comp=%u ncomp=%u ; idx load type=%d op=%d const=%d ; store type=%d op=%d const=%d\n",
                    (unsigned)nir_intrinsic_io_semantics(ld).location, nir_intrinsic_base(ld), nir_intrinsic_component(ld), ld->def.num_components,
                    (int)a->type, a->type == nir_instr_type_intrinsic ? (int)nir_instr_as_intrinsic(a)->intrinsic : -1,
                    (int)(nir_src_is_const(ld->src[1]) ? nir_src_as_uint(ld->src[1]) : 9999),
                    (int)b->type, b->type == nir_instr_type_intrinsic ? (int)nir_instr_as_intrinsic(b)->intrinsic : -1,
                    (int)(nir_src_is_const(st->src[1]) ? nir_src_as_uint(st->src[1]) : 9999));
         }
         const bool at_this_invocation = (nir_src_is_const(st->src[1]) && nir_src_as_uint(st->src[1]) == i);
         if (!same_index || !at_this_invocation) {
            if (getenv("MALI_TESS_DBG"))
               fprintf(stderr, "mali_tess: invocation %u: same=%d at=%d\n", i, same_index, at_this_invocation);
            ralloc_free(tcs);
            return refuse(why, whysz, "a tessellation control output that is not the identity patch at invocation %u", i);
         }
      }
   }
   ralloc_free(tcs);
   }
   if (getenv("MALI_TESS_DBG"))
      fprintf(stderr, "mali_tess: tcs per-vertex stores = %u\n", n);
   if (!n)
      return refuse(why, whysz, "the tessellation control shader writes no per-vertex output");
   return 1;
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

   nir_shader *tcs = mali_poly_nir_from_spirv(arch, tcs_words, ntcs, MESA_SHADER_TESS_CTRL, tcs_entry, err, sizeof err);
   if (!tcs)
      return refuse(why, whysz, "tessellation control shader: %s", err[0] ? err : "does not parse");
   if (tcs->info.tess.tcs_vertices_out != 3) {
      refuse(why, whysz, "the tessellation control shader declares %u output vertices, T1 accepts 3",
             tcs->info.tess.tcs_vertices_out);
      ralloc_free(tcs);
      return 0;
   }
   /* the levels are checked on a preprocessed copy, the body on one clone per invocation (the fold must happen before the
    * preprocessing, or the invocation id is already gone) */
   nir_shader *levels = nir_shader_clone(NULL, tcs);
   mali_poly_preprocess(levels);
   const int levels_ok = check_levels(levels, why, whysz);
   ralloc_free(levels);
   if (!levels_ok || !check_tcs_identity(tcs, why, whysz)) {
      ralloc_free(tcs);
      return 0;
   }
   ralloc_free(tcs);

   /* Three output vertices: at each, the TES body must be the identity copy of the matching patch vertex. */
   static const struct coord_key coords[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
   int rc = 0;
   for (unsigned i = 0; i < 3; i++) {
      nir_shader *clone = nir_shader_clone(NULL, tes);
      struct coord_key k = coords[i];
      NIR_PASS(_, clone, nir_shader_intrinsics_pass, replace_coord, nir_metadata_control_flow, &k);
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
