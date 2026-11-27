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
   /* Do NOT remove the instruction here: a module can load gl_TessCoord more than once (glslang emits one load per
    * component read) and removing during the intrinsics walk made the second load survive, so the classification
    * silently kept whatever the *old* value folded to. Rewriting the uses is enough: the load has no side effects and
    * nir_opt_dce deletes it. */
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

/* One store in the folded body, in deref form: the output varying, and the per-vertex input varying + control point its
 * value was loaded from (-1 = the value is not a plain load of a patch input). */
struct dstore { gl_varying_slot out_loc, in_loc; int ctrl; unsigned out_npath, npath; };

static int
deref_slot(nir_deref_instr *d, unsigned *npath, int *ctrl, unsigned *first_index)
{
   *ctrl = -1;
   *npath = 0;
   bool seen_array = false;
   for (nir_deref_instr *p = d; p; p = nir_deref_instr_parent(p)) {
      if (p->deref_type == nir_deref_type_array) {
         if (!seen_array && nir_src_is_const(p->arr.index)) {
            /* the first (innermost to the variable) array index is the patch control point */
            *ctrl = (int)nir_src_as_uint(p->arr.index);
            seen_array = true;
            if (first_index && *first_index == 0)
               *first_index = 1;
         } else if (nir_src_is_const(p->arr.index)) {
            (*npath)++;
         } else {
            return 0;   /* a non-constant element index: not a copy of one element */
         }
      } else if (p->deref_type == nir_deref_type_struct) {
         (*npath)++;
      }
      if (!nir_deref_instr_parent(p))
         return 1;
   }
   return 1;
}

static unsigned
collect_deref_stores(nir_shader *nir, struct dstore *out, unsigned max)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   unsigned n = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *st = nir_instr_as_intrinsic(instr);
         if (st->intrinsic != nir_intrinsic_store_deref || n >= max)
            continue;
         nir_deref_instr *od = nir_src_as_deref(st->src[0]);
         if (!od)
            continue;
         nir_variable *ovar = nir_deref_instr_get_variable(od);
         if (!ovar)
            continue;
         struct dstore s = {.out_loc = ovar->data.location, .in_loc = 0, .ctrl = -1, .out_npath = 0, .npath = 0};
         {
            int c = -1; unsigned np = 0; unsigned fi = 0;
            if (!deref_slot(od, &np, &c, &fi))
               continue;      /* a non-constant output index: refused (ctrl stays -1) */
            s.out_npath = np;
         }
         /* the value: a plain load_deref of a patch input, or nothing */
         if (st->src[1].ssa && nir_def_instr(st->src[1].ssa)->type == nir_instr_type_intrinsic) {
            nir_intrinsic_instr *ld = nir_instr_as_intrinsic(nir_def_instr(st->src[1].ssa));
            if (ld->intrinsic == nir_intrinsic_load_deref) {
               nir_deref_instr *id = nir_src_as_deref(ld->src[0]);
               nir_variable *ivar = id ? nir_deref_instr_get_variable(id) : NULL;
               if (ivar) {
                  int ctrl = -1; unsigned np = 0; unsigned fi = 0;
                  if (deref_slot(id, &np, &ctrl, &fi)) {
                     s.in_loc = ivar->data.location;
                     s.ctrl = ctrl;
                     s.npath = np;
                  }
               }
            }
         }
         out[n++] = s;
      }
   }
   return n;
}

void
mali_tess_header(uint8_t hdr[MALI_TESS_HDR_SIZE], uint64_t blob, uint64_t vblob, uint64_t vs_outputs)
{
   memset(hdr, 0, MALI_TESS_HDR_SIZE);
   memcpy(hdr + MALI_TESS_HDR_BLOB, &blob, sizeof blob);
   memcpy(hdr + MALI_TESS_HDR_VBLOB, &vblob, sizeof vblob);
   memcpy(hdr + MALI_TESS_HDR_VSOUT, &vs_outputs, sizeof vs_outputs);
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
      /* No nir_lower_io_passes here on purpose: in the *deref* form the control point is still an explicit constant array
       * index (deref_array &gl_in[1]), while after IO lowering it moves into the varying base and src[1] reads 0 — which is
       * exactly the trap that made e1/e2 look like e0. The folded body is inspected as it stands. */
      struct dstore st[64];
      const unsigned n = collect_deref_stores(clone, st, 64);
      unsigned seen_pos = 0;
      const char *bad = NULL;
      for (unsigned s = 0; s < n && !bad; s++) {
         if (getenv("MALI_TESS_DBG"))
            fprintf(stderr, "mali_tess: tes e%u store loc=%u -> in loc=%u ctrl=%d npath=%u\n", i,
                    (unsigned)st[s].out_loc, (unsigned)st[s].in_loc, st[s].ctrl, st[s].npath);
         if (st[s].ctrl < 0) bad = "an output that is not a copy of a patch input";
         else if (st[s].in_loc != st[s].out_loc || st[s].npath != st[s].out_npath) bad = "an output copied from a different varying than the one it writes";
         else if (st[s].ctrl != (int)i) bad = "an output copied from the wrong patch vertex";
         if (st[s].out_loc == VARYING_SLOT_POS && !bad) seen_pos = 1;
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
