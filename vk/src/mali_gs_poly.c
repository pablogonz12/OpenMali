/* SPDX-License-Identifier: MIT */
/* GS lane 2, step (ii) — the geometry stage as programs our backend can run.
 *
 * Step (i) (mali_poly.c) proved that Mesa's poly_nir_lower_gs() runs on a real GS module inside our compile path and
 * that its four outputs validate. This file takes the next step and nothing more: it hands those four programs to our
 * own backend (mali_compile_nir) and reports what came out — sizes, workgroup sizes, and the first thing that refused.
 * The draw path (step (iii): arena, count/prefix-sum dispatches, the rast draw) is *not* here yet, so no pipeline can
 * pass through this function: mali_pipeline.c keeps refusing a non-pass-through GS, with the compile's verdict in the
 * reason. That is deliberate — a refused pipeline is safe, a pipeline that draws nothing is not.
 *
 * Why the four programs need no SPIR-V of their own: poly splits one module into `main` (compute, the GS body run once
 * per input primitive), `count` (compute, output counts when the count is data-dependent), `copy` (vertex, the rast
 * program that pulls the arena) and `pre_gs` (vertex, the input-side patch-up that writes the arena). They are NIR, and
 * mali_compile_nir() is the seam that compiles NIR (mali_compile.c). The stage flip is KosmicKrisp's
 * (kk_shader.c:1014-1017): set info.stage, clear the union member the old stage used, drop xfb_info.
 */
#include "compiler/glsl_types.h"
#include "compiler/shader_enums.h"
#include "compiler/spirv/nir_spirv.h"
#include "panfrost/compiler/pan_compiler.h"
#include "poly/nir/poly_nir.h"
#include "nir.h"

#include "mali_compile.h"
#include "mali_gs_poly.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gs_diag { char msg[256]; };

static void
gs_diag_cb(void *priv, enum nir_spirv_debug_level level, size_t offset, const char *message)
{
   struct gs_diag *d = priv;
   (void)offset;
   if (level == NIR_SPIRV_DEBUG_LEVEL_ERROR)
      snprintf(d->msg, sizeof d->msg, "%.200s", message);
}

static void
one_line(char *s)
{
   for (char *p = s; *p; p++)
      if (*p == '\n' || *p == '\r' || *p == '"') *p = (*p == '"') ? '\'' : ' ';
}

/* poly hands a GS four programs that read driver-supplied table addresses (nir_intrinsics.py:1590-1603): the geometry
 * param block, the VS-outputs mask, the vertex param block and the index size. Our descriptor lowering knows only
 * load_tess_param_buffer_poly (mali_descriptor_lower.c:311-332), so any other one of them reaches the Valhall backend as
 * an intrinsic it has never seen — and Mesa's own divergence analysis aborts the *process* on it
 * (nir_divergence_analysis.c:1157, measured: "Unhandled intrinsic" from load_geometry_param_buffer_poly). That abort is
 * the reason this check is here: a GS pipeline must be refused with a name, never crash the driver, because
 * MALI_EXPERIMENTAL_GS=1 or a future advertise makes this path reachable from an application.
 * The helper that removes this blocker is shared with the tess lane (#388, lower_poly_sysvals): it lowers exactly these
 * four sysvals to the table descriptors we already write. When it lands, delete this check and step (iii) records the
 * draw. */
static const char *
unlowered_poly_sysval(nir_shader *nir, const char *kind)
{
   static const struct { nir_intrinsic_op op; const char *name; } K[] = {
      /* The four sysvals lower_poly_sysvals (mali_descriptor_lower.c, from the tess lane) now lowers are *not* in this
       * list: geometry/vertex param buffer and vs_outputs are handled, and a program that still shows one means the
       * lowering did not reach it, which the compile below reports with the backend's own message. */
      { nir_intrinsic_load_index_size_poly, "load_index_size_poly" },
      { nir_intrinsic_load_input_topology_poly, "load_input_topology_poly" },
      { nir_intrinsic_load_ro_sink_address_poly, "load_ro_sink_address_poly" },
   };
   (void)kind;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_op op = nir_instr_as_intrinsic(instr)->intrinsic;
            for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++)
               if (K[i].op == op)
                  return K[i].name;
         }
      }
   }
   return NULL;
}

/* Compile one poly output into `out`. `kind` names it in the message ("main", "count", "copy", "pre_gs"). */
static int
compile_one(const char *kind, nir_shader *nir, unsigned arch, const struct mali_compile_opts *base, struct mali_compiled *out,
            char *err, size_t errsz)
{
   if (!nir)
      return 0;
   const char *want = unlowered_poly_sysval(nir, kind);
   if (want) {
      snprintf(err, errsz, "poly's %s program uses %s, which our descriptor lowering does not lower yet "
                           "(lower_poly_sysvals is shared with the tess lane, #388): refusing instead of aborting in "
                           "Mesa's divergence analysis", kind, want);
      return -1;
   }
   /* tcs-compute-pipeline.md steps 6 and 8 applied to poly's programs: every memory mode a program still declares with
    * a struct type has to be made explicit before the backend (nir_validate.c:1822 validate_var_decl,
    * "glsl_type_is_scalar", is the assert that fires otherwise). do_compile's compute branch does this for
    * nir_var_mem_shared and its tail for nir_var_mem_global; poly's programs carry the arena in *both* forms (the
    * geometry/vs-output blobs are global, the count buffer's staging is shared), so the pair is run here, shared first
    * (step 6) then global (step 8), before anything else touches the program. */
   NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared, glsl_get_natural_size_align_bytes);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared, nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_global, glsl_get_natural_size_align_bytes);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global, nir_address_format_64bit_global);
   if (getenv("MALI_PIPE_DBG"))
      fprintf(stderr, "mali_vk: GS step (ii): compiling poly's %s program (%s, workgroup %u,%u,%u)\n", kind,
              nir->info.stage == MESA_SHADER_COMPUTE ? "compute" : "vertex",
              nir->info.workgroup_size[0], nir->info.workgroup_size[1], nir->info.workgroup_size[2]);
   struct mali_compile_opts o = *base;
   switch (nir->info.stage) {
   case MESA_SHADER_COMPUTE: o.stage = MALI_STAGE_COMPUTE; break;
   case MESA_SHADER_VERTEX: o.stage = MALI_STAGE_VERTEX; break;
   default:
      snprintf(err, errsz, "poly's %s program has stage %u, which this driver cannot run", kind, (unsigned)nir->info.stage);
      return -1;
   }
   o.spirv = NULL;   /* no module of its own: the backend is entered through mali_compile_nir */
   o.spirv_words = 0;
   char e[256] = "";
   int r = mali_compile_nir(&o, nir, out, e, sizeof e);
   if (r)
      snprintf(err, errsz, "poly's %s program (%s) did not compile: %.150s", kind,
               nir->info.stage == MESA_SHADER_COMPUTE ? "compute" : "vertex", e);
   return r;
}

int
mali_gs_poly_compile(unsigned arch, const uint32_t *spirv, uint32_t nwords, const char *entry,
                     const struct mali_compile_opts *base, struct mali_gs_programs *out, char *err, size_t errsz)
{
   memset(out, 0, sizeof *out);
   struct gs_diag diag = {{0}};
   nir_shader_compiler_options nopts = *pan_get_nir_shader_compiler_options(arch, MESA_SHADER_GEOMETRY, false);
   nopts.lower_mediump_io = NULL;
   struct spirv_to_nir_options so = {
      .environment = NIR_SPIRV_VULKAN,
      .debug = {.func = gs_diag_cb, .private_data = &diag},
      .skip_os_break_in_debug_build = true,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_64bit_global_32bit_offset,
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ubo_alignment = 16, .min_ssbo_alignment = 16,
      .push_const_addr_format = nir_address_format_32bit_offset,
   };
   nir_shader *gs = spirv_to_nir(spirv, nwords, NULL, MESA_SHADER_GEOMETRY, entry ? entry : "main", &so, &nopts);
   if (!gs) {
      snprintf(err, errsz, "spirv_to_nir: %.200s", diag.msg[0] ? diag.msg : "does not parse");
      one_line(err);
      return -1;
   }
   /* poly's precondition, in the order mali_poly.c runs it (mali_poly.c:120-150): the same passes our VS/FS path runs,
    * then the generic IO lowering that sets info.io_lowered. */
   NIR_PASS(_, gs, nir_lower_variable_initializers, nir_var_function_temp | nir_var_shader_temp | nir_var_shader_out);
   NIR_PASS(_, gs, nir_lower_returns);
   NIR_PASS(_, gs, nir_inline_functions);
   nir_remove_non_entrypoints(gs);
   NIR_PASS(_, gs, nir_opt_copy_prop);
   NIR_PASS(_, gs, nir_lower_global_vars_to_local);
   NIR_PASS(_, gs, nir_lower_vars_to_ssa);
   NIR_PASS(_, gs, nir_lower_io_vars_to_temporaries, nir_shader_get_entrypoint(gs), nir_var_shader_out);
   NIR_PASS(_, gs, nir_split_var_copies);
   NIR_PASS(_, gs, nir_lower_system_values);
   nir_lower_io_passes(gs, false);
   nir_shader_gather_info(gs, nir_shader_get_entrypoint(gs));   /* poly consumes outputs_written / inputs_read */

   nir_shader *count = NULL, *copy = NULL, *pre_gs = NULL;
   struct poly_gs_info info;
   if (!poly_nir_lower_gs(gs, &count, &copy, &pre_gs, &info)) {
      snprintf(err, errsz, "poly_nir_lower_gs refused this shader");
      ralloc_free(gs);
      return -1;
   }
   out->info.ok = 1;
   out->info.mode = (uint32_t)info.mode;
   out->info.shape = (uint32_t)info.shape;
   out->info.count_words = info.count_words;
   out->info.max_indices = info.max_indices;
   out->has_count = count != NULL;
   out->has_copy = copy != NULL;
   out->has_pre_gs = pre_gs != NULL;

   /* The stage flip, after poly (it reads info.tess/geometry): the compute programs become compute *now*, so the
    * backend's compute branch (workgroup decomposition, explicit shared IO) sees them. The union member the old stage
    * used is cleared, as KosmicKrisp does. */
   if (count) {
      count->info.stage = MESA_SHADER_COMPUTE;
      memset(&count->info.cs, 0, sizeof count->info.cs);
      count->xfb_info = NULL;
      count->info.workgroup_size[0] = 1;
      count->info.workgroup_size[1] = 1;
      count->info.workgroup_size[2] = 1;
   }
   gs->info.stage = MESA_SHADER_COMPUTE;
   memset(&gs->info.cs, 0, sizeof gs->info.cs);
   gs->xfb_info = NULL;
   if (out->info.shape == MALI_POLY_SHAPE_DYNAMIC_INDEXED) {
      /* one invocation per input primitive, then a prefix sum over the count buffer (step (iii) runs the second
       * dispatch); poly reports the workgroup size it wants when it needs one. */
      gs->info.workgroup_size[0] = 1;
      gs->info.workgroup_size[1] = 1;
      gs->info.workgroup_size[2] = 1;
   }

   int rc = 0;
   if ((rc = compile_one("main", gs, arch, base, &out->main, err, errsz)) ||
       (rc = compile_one("count", count, arch, base, &out->count, err, errsz)) ||
       (rc = compile_one("copy", copy, arch, base, &out->copy, err, errsz)) ||
       (rc = compile_one("pre_gs", pre_gs, arch, base, &out->pre_gs, err, errsz)))
      goto out;   /* the shader that failed is freed by mali_compile_nir (ownership passed); the rest here */
   out->info.has_main = out->main.bin_size != 0;
   out->info.has_count = out->count.bin_size != 0;
   out->info.has_copy = out->copy.bin_size != 0;
   out->info.has_pre_gs = out->pre_gs.bin_size != 0;
out:
   /* mali_compile_nir took ownership of the one it was given; the others still belong to us. A program that failed is
    * owned by the backend, so it must not be freed here: the four are freed by the caller through mali_gs_programs_free
    * for the ones it owns. */
   if (!out->count.bin_size && !rc) ralloc_free(count);
   if (!out->copy.bin_size && !rc) ralloc_free(copy);
   if (!out->pre_gs.bin_size && !rc) ralloc_free(pre_gs);
   if (!out->main.bin_size && !rc) ralloc_free(gs);
   return rc;
}

void
mali_gs_programs_free(struct mali_gs_programs *p)
{
   if (!p)
      return;
   /* mali_compiled_free is idempotent for a zeroed struct (bin NULL / size 0). */
   mali_compiled_free(&p->main);
   mali_compiled_free(&p->count);
   mali_compiled_free(&p->copy);
   mali_compiled_free(&p->pre_gs);
   memset(p, 0, sizeof *p);
}

const char *
mali_gs_programs_report(const struct mali_gs_programs *p, char *buf, size_t n)
{
   snprintf(buf, n, "poly: shape %u mode %u max_indices %u count_words %u; main %u B, count %u B, copy %u B, pre_gs %u B",
            p->info.shape, p->info.mode, p->info.max_indices, p->info.count_words,
            p->main.bin_size, p->count.bin_size, p->copy.bin_size, p->pre_gs.bin_size);
   return buf;
}
