/* SPDX-License-Identifier: MIT */
/* GS lane 2, step (i): Mesa's src/poly geometry-shader lowering as a linked library.
 *
 * The library is Mesa's, unmodified (vk/poly.sh builds every .c in src/poly/nir plus the CL half Mesa generates from
 * src/poly/cl/{geometry,tessellation}.cl with its own mesa_clc). This file is the whole of our side: SPIR-V -> NIR with
 * the options mali_compile() uses, then one call to poly_nir_lower_gs(), then a report of what came back. No NIR pass of
 * ours is in between, so what the reports measure is poly's behaviour and not ours.
 *
 * Why the lowering is reached from here at all: this object file is part of the driver build (vk/build.sh SRCS) and links
 * the archive into libvulkan_mali.so, so t_gs_lower proves the pair links and runs in the shipped configuration, not just
 * in a test binary. What it does *not* yet prove is that the code survives the final link: nothing calls mali_poly_lower_gs()
 * from the draw path before step (ii), so -Wl,--gc-sections (build.sh:145) drops it from the .so (the archive's FILE symbol
 * is in the binary, the function is not). Step (ii) turns these four programs into panfrost code (pre_gs/main/count as
 * compute, copy as our vertex program) and step (iii) records the draw. */
#include "compiler/glsl_types.h"
#include "compiler/spirv/nir_spirv.h"
#include "panfrost/compiler/pan_compiler.h"
#include "poly/nir/poly_nir.h"
#include "nir.h"

#include "mali_poly.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The public mirrors in mali_poly.h are only honest if they are these Mesa values: a renumbering in Mesa fails the build
 * here instead of silently mis-sizing a rast draw in mali_cmd.c (step iii). */
_Static_assert((int)MALI_POLY_PRIM_POINTS == (int)MESA_PRIM_POINTS, "mali_poly.h mirror: points");
_Static_assert((int)MALI_POLY_PRIM_LINES == (int)MESA_PRIM_LINES, "mali_poly.h mirror: lines");
_Static_assert((int)MALI_POLY_PRIM_TRIANGLES == (int)MESA_PRIM_TRIANGLES, "mali_poly.h mirror: triangles");
_Static_assert((int)MALI_POLY_SHAPE_DYNAMIC_INDEXED == (int)POLY_GS_SHAPE_DYNAMIC_INDEXED, "mali_poly.h mirror: dynamic indexed");
_Static_assert((int)MALI_POLY_SHAPE_STATIC_INDEXED == (int)POLY_GS_SHAPE_STATIC_INDEXED, "mali_poly.h mirror: static indexed");
_Static_assert((int)MALI_POLY_SHAPE_STATIC_PER_PRIM == (int)POLY_GS_SHAPE_STATIC_PER_PRIM, "mali_poly.h mirror: static per prim");
_Static_assert((int)MALI_POLY_SHAPE_STATIC_PER_INSTANCE == (int)POLY_GS_SHAPE_STATIC_PER_INSTANCE, "mali_poly.h mirror: per instance");

/* nir_validate's message sink and spirv_to_nir's: one buffer, because a bad module must fail with a name, not abort. */
struct poly_diag { char msg[256]; };
static void
poly_diag_cb(void *priv, enum nir_spirv_debug_level level, size_t offset, const char *message)
{
   struct poly_diag *d = priv;
   (void)offset;
   if (level == NIR_SPIRV_DEBUG_LEVEL_ERROR) snprintf(d->msg, sizeof d->msg, "%.200s", message);
}

const char *
mali_poly_provenance(void)
{
   /* Not a build id (the archive is per target and untracked): the note names the commit and the numbers. */
   return "mesa/src/poly (poly_nir_lower_gs, unmodified; vk/poly.sh)";
}

static void
fill_from_info(struct mali_poly_gs *out, const struct poly_gs_info *info)
{
   out->mode = (uint32_t)info->mode;
   out->shape = (uint32_t)info->shape;
   out->count_words = info->count_words;
   out->max_indices = info->max_indices;
   out->xfb = info->xfb != 0;
   out->prefix_sum = info->prefix_sum != 0;
   out->multistream = info->multistream != 0;
   /* One input primitive, one instance: the smallest shape a rast draw can take, i.e. the index count of a single GS
    * invocation. poly_gs_rast_vertices() is poly's own arithmetic (src/poly/geometry.h:53), quoted not reimplemented. */
   out->rast_vertices = poly_gs_rast_vertices(info->shape, info->max_indices, 1, 1);
}

int
mali_poly_lower_gs(unsigned arch, const uint32_t *spirv, uint32_t nwords, const char *entry,
                   struct mali_poly_gs *out, char *err, size_t errsz)
{
   if (out) memset(out, 0, sizeof *out);
   if (!out || !spirv || nwords < 5) {
      if (err && errsz) snprintf(err, errsz, "mali_poly_lower_gs: no SPIR-V");
      return 0;
   }
   if (arch != 11 && arch != 13 && arch != 10) {
      if (err && errsz) snprintf(err, errsz, "mali_poly_lower_gs: arch %u has no compiler options here", arch);
      return 0;
   }

   /* spirv_to_nir builds glsl types (mali_compile.c:64 refs the singleton once per process). Ref/unref around the call so
    * the entry point is safe whether or not a VkInstance exists yet. */
   glsl_type_singleton_init_or_ref();
   /* The same options object mali_compile() uses for a stage's NIR (pan_compiler.c:116 ignores the stage except for the
    * workgroup-merge flag), and the same descriptor-free address formats (mali_compile.c:463-475): a GS module without a
    * pipeline layout is parsed exactly like a CS/VS one. */
   const nir_shader_compiler_options *opts =
      pan_get_nir_shader_compiler_options(arch == 10 ? 10 : arch, MESA_SHADER_GEOMETRY, false);
   struct poly_diag diag = {{0}};
   struct spirv_to_nir_options so = {
      .environment = NIR_SPIRV_VULKAN,
      .debug = {.func = poly_diag_cb, .private_data = &diag},
      .skip_os_break_in_debug_build = true,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_64bit_global_32bit_offset,
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ubo_alignment = 16, .min_ssbo_alignment = 16,
      .push_const_addr_format = nir_address_format_32bit_offset,
   };
   nir_shader *gs = spirv_to_nir(spirv, nwords, NULL, MESA_SHADER_GEOMETRY, entry ? entry : "main", &so, opts);
   if (!gs) {
      if (err && errsz) snprintf(err, errsz, "spirv_to_nir: %s", diag.msg[0] ? diag.msg : "does not parse");
      glsl_type_singleton_decref();
      return 0;
   }
   if (gs->info.stage != MESA_SHADER_GEOMETRY) {
      if (err && errsz) snprintf(err, errsz, "no geometry shader in this module");
      ralloc_free(gs);
      glsl_type_singleton_decref();
      return 0;
   }

   const char *pp = getenv("MALI_POLY_PREPASS");
   if (pp && strstr(pp, "iopass")) nir_lower_io_passes(gs, false);
   if (pp && strstr(pp, "dce")) NIR_PASS(_, gs, nir_opt_dce);
   if (pp && strstr(pp, "returns")) NIR_PASS(_, gs, nir_lower_returns);
   if (pp && strstr(pp, "optif")) NIR_PASS(_, gs, nir_opt_if, nir_opt_if_optimize_phi_true_false | nir_opt_if_avoid_64bit_phis);
   if (getenv("MALI_POLY_DBG")) {
      nir_function_impl *impl = nir_shader_get_entrypoint(gs);
      nir_instr *last = nir_block_last_instr(nir_impl_last_block(impl));
      fprintf(stderr, "poly dbg: pre=%s tailtype=%d lastcf=%d ninstr=%u\n", pp ? pp : "-", last ? (int)last->type : -1,
              (int)nir_impl_last_block(impl)->cf_node.type, (unsigned)exec_list_length(&impl->body));
   }
   nir_shader *count = NULL, *copy = NULL, *pre_gs = NULL;
   struct poly_gs_info info;
   bool ok = poly_nir_lower_gs(gs, &count, &copy, &pre_gs, &info);
   out->ok = ok != 0;
   out->has_main = 1;
   out->has_count = count != NULL;
   out->has_copy = copy != NULL;
   out->has_pre_gs = pre_gs != NULL;
   out->main_stage = gs->info.stage;
   out->count_stage = count ? count->info.stage : 0;
   out->copy_stage = copy ? copy->info.stage : 0;
   out->pre_gs_stage = pre_gs ? pre_gs->info.stage : 0;
   fill_from_info(out, &info);
   if (ok) {
      /* The outputs are handed to the backend in step (ii), so this is where a lowering that produced invalid NIR has
       * to be caught, not inside a compile that would blame the application's shader. */
      out->nir_valid = 1;
      nir_shader *all[4] = {gs, count, copy, pre_gs};
      for (unsigned i = 0; i < 4; i++) {
         if (!all[i]) continue;
         nir_validate_shader(all[i], "poly output");
      }
   } else if (err && errsz) {
      snprintf(err, errsz, "poly_nir_lower_gs refused this shader");
   }
   ralloc_free(count); ralloc_free(copy); ralloc_free(pre_gs); ralloc_free(gs);
   glsl_type_singleton_decref();
   return out->ok;
}
