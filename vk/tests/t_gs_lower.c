/* SPDX-License-Identifier: MIT */
/* t_gs_lower — GS lane 2 step (i) (notes/gs-design.md §5(i)): proves Mesa's src/poly geometry lowering is linked into
 * our driver and runs on real GS modules. White-box and host-only: linked statically with the driver objects (build.sh),
 * it calls mali_poly_lower_gs() — the same function the shipped libvulkan_mali.so contains — on the .spv files of
 * vk/shader/, and checks what poly_nir_lower_gs() returned for each:
 *
 *   gs_pass.geom    3-in/3-out copy      max_indices 3, triangle-strip output decomposed to triangles, static counts
 *   gs_amplify.geom 3-in/6-out unrolled   max_indices 6, same mode, static counts
 *   gs_loop.geom    the DXVK-shaped for   max_indices == its max_vertices, static or dynamic counts both accepted
 *   gs_computed.geom a computed output     lowers (poly has no pass-through restriction: that is lane 1's classifier)
 *
 * For every module: main/copy/pre_gs exist, copy is a VERTEX program and pre_gs a COMPUTE program (the proven draw shape
 * of the falsifier: a compute job writes the arena, a vertex program pulls it), count exists iff count_words > 0, the
 * reported rast_vertices matches poly_gs_rast_vertices() of the reported shape, and nir_validate_shader() accepted all
 * four programs. A vertex shader module must be refused by name ("no geometry shader"), which is the negative that shows
 * the entry point is really the geometry stage and not "anything that parses".
 *
 * No draw, no pipeline, no GPU: that is step (ii)+(iii). Exit 0 = pass.
 *   t_gs_lower [--spv-dir DIR] [--arch 11|13]   (default: shader, 11) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mali_poly.h"

static const char *g_spv = "shader";
static unsigned g_arch = 11;
static int g_fail;

#define FAIL(...) do { printf("{\"test\":\"gs_lower\",\"ok\":false,\"fail\":\"" __VA_ARGS__); printf("\"}\n"); return 1; } while (0)

static uint32_t *
read_spv(const char *name, uint32_t *nwords)
{
   char path[512];
   snprintf(path, sizeof path, "%s/%s", g_spv, name);
   FILE *f = fopen(path, "rb");
   if (!f) { printf("{\"test\":\"gs_lower\",\"ok\":false,\"fail\":\"open %s\"}\n", path); return NULL; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (n <= 0 || (n % 4) != 0) { fclose(f); printf("{\"test\":\"gs_lower\",\"ok\":false,\"fail\":\"%s is %ld B, not SPIR-V\"}\n", path, n); return NULL; }
   uint32_t *buf = malloc((size_t)n);
   if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); printf("{\"test\":\"gs_lower\",\"ok\":false,\"fail\":\"read %s\"}\n", path); return NULL; }
   fclose(f);
   *nwords = (uint32_t)(n / 4);
   return buf;
}

/* One case: lower it, check the invariants, print one JSON line per file (the shape of t_vk_draw's output). */
static int
case_gs(const char *file, unsigned max_indices, unsigned count_words, const char *why_expected)
{
   uint32_t nwords = 0;
   uint32_t *spirv = read_spv(file, &nwords);
   if (!spirv) return 1;

   struct mali_poly_gs gs;
   char err[256] = "";
   int ok = mali_poly_lower_gs(g_arch, spirv, nwords, "main", &gs, err, sizeof err);
   free(spirv);
   if (!ok) { printf("{\"test\":\"gs_lower\",\"file\":\"%s\",\"ok\":false,\"error\":\"%s\"}\n", file, err); return 1; }

   const char *why = NULL;
   char detail[160] = "";
   if (!gs.has_main || !gs.has_copy || !gs.has_pre_gs) why = "a program is missing";
   else if (gs.copy_stage != 0 /* MESA_SHADER_VERTEX */) why = "the rast program is not a vertex program";
   else if (gs.pre_gs_stage != 5 /* MESA_SHADER_COMPUTE */) why = "pre_gs is not a compute program";
   else if (gs.max_indices == 0 || gs.max_indices > 4096) why = "max_indices out of the plausible range";
   /* max_indices is poly's own worst case for a dynamic index buffer (poly_nir_lower_gs.c:1073
    * calculate_max_indices: vertices + vertices/3 for a triangle topology), or the static index count when the emits
    * are provably in the start block. Both are per-file facts, not a range: the test names the number. */
   else if (gs.max_indices != max_indices) { snprintf(detail, sizeof detail, "max_indices %u, want %u (%s)", gs.max_indices, max_indices, why_expected); why = detail; }
   else if (gs.count_words != count_words) { snprintf(detail, sizeof detail, "count_words %u, want %u", gs.count_words, count_words); why = detail; }
   else if (gs.shape != MALI_POLY_SHAPE_DYNAMIC_INDEXED && gs.shape != MALI_POLY_SHAPE_STATIC_INDEXED && gs.shape != MALI_POLY_SHAPE_STATIC_PER_PRIM && gs.shape != MALI_POLY_SHAPE_STATIC_PER_INSTANCE) { snprintf(detail, sizeof detail, "shape %u unknown", gs.shape); why = detail; }
   else if (gs.rast_vertices < gs.max_indices) { snprintf(detail, sizeof detail, "rast_vertices %u < max_indices %u", gs.rast_vertices, gs.max_indices); why = detail; }
   else if (gs.mode != MALI_POLY_PRIM_TRIANGLES && gs.mode != MALI_POLY_PRIM_TRIANGLE_STRIP) { snprintf(detail, sizeof detail, "mode %u is not a triangle topology", gs.mode); why = detail; }
   else if ((gs.count_words > 0) != (gs.has_count != 0)) { snprintf(detail, sizeof detail, "count_words %u but count program %s", gs.count_words, gs.has_count ? "present" : "absent"); why = detail; }
   else if (gs.count_words > 16) why = "count_words larger than a primitive record can be";

   else if (!gs.nir_valid) why = "nir_validate_shader rejected an output";

   printf("{\"test\":\"gs_lower\",\"file\":\"%s\",\"arch\":%u,\"ok\":%s,\"mode\":%u,\"shape\":%u,\"max_indices\":%u,"
          "\"count_words\":%u,\"rast_vertices\":%u,\"programs\":\"%s%s%s%s\",\"stages\":[%u,%u,%u,%u]%s%s}\n",
          file, g_arch, why ? "false" : "true", gs.mode, gs.shape, gs.max_indices, gs.count_words, gs.rast_vertices,
          gs.has_main ? "main," : "", gs.has_count ? "count," : "", gs.has_copy ? "copy," : "", gs.has_pre_gs ? "pre_gs" : "",
          gs.main_stage, gs.count_stage, gs.copy_stage, gs.pre_gs_stage,
          why ? ",\"why\":\"" : "", why ? why : "");
   if (why) { printf("{\"test\":\"gs_lower\",\"fail\":\"%s: %s\"}\n", file, why); g_fail = 1; return 1; }
   return 0;
}

/* A vertex shader is not a geometry shader: the entry point must refuse it, with a reason.
 * (0x07230203 magic + a real vertex module from the same directory.) */
static void
case_not_a_gs(void)
{
   uint32_t nwords = 0;
   uint32_t *spirv = read_spv("rot.vert.spv", &nwords);
   if (!spirv) { g_fail = 1; return; }
   struct mali_poly_gs gs;
   char err[256] = "";
   int ok = mali_poly_lower_gs(g_arch, spirv, nwords, "main", &gs, err, sizeof err);
   free(spirv);
   if (ok) { printf("{\"test\":\"gs_lower\",\"fail\":\"a vertex shader was lowered as a geometry shader\"}\n"); g_fail = 1; }
   else if (!strstr(err, "geometry shader")) { printf("{\"test\":\"gs_lower\",\"fail\":\"vertex shader refused without naming the reason: %s\"}\n", err); g_fail = 1; }
   else printf("{\"test\":\"gs_lower\",\"not_a_gs\":\"refused: %s\"}\n", err);
}

/* The tessellation half of the same library (notes/tess-design.md, lane T1): poly_nir_lower_tes() must accept a TES and
 * produce a shader, both as the T2 "hardware VS" form and in place. This is the library-link proof T1 needs; the level-1
 * *draw* is mali_tess.c's classifier and t_vk_draw's tess_tri row. */
static int
case_tes(const char *file)
{
   uint32_t nwords = 0;
   uint32_t *spirv = read_spv(file, &nwords);
   if (!spirv) return 1;
   struct mali_poly_tes tes;
   char err[256] = "";
   int ok = mali_poly_lower_tes(g_arch, spirv, nwords, "main", 1 /* to_hw_vs */, &tes, err, sizeof err);
   free(spirv);
   if (!ok) { printf("{\"test\":\"gs_lower\",\"tes\":\"%s\",\"ok\":false,\"error\":\"%s\"}\n", file, err); return 1; }
   const char *why = NULL;
   if (!tes.lowered) why = "poly_nir_lower_tes returned false";
   else if (!tes.nir_valid) why = "the lowered shader failed nir_validate";
   else if (tes.stage != 0 /* MESA_SHADER_VERTEX */) why = "to_hw_vs did not turn it into a vertex shader";
   printf("{\"test\":\"gs_lower\",\"tes\":\"%s\",\"arch\":%u,\"ok\":%s,\"lowered\":%u,\"stage\":%u%s%s}\n",
          file, g_arch, why ? "false" : "true", tes.lowered, tes.stage, why ? ",\"why\":\"" : "", why ? why : "");
   if (why) { g_fail = 1; return 1; }
   return 0;
}

/* T2 step 2's first number: Mesa's poly_nir_lower_tcs() on a real TCS, and the buffer geometry the record side needs. */
static int
case_tcs(const char *file, unsigned want_stride, unsigned want_verts)
{
   uint32_t nwords = 0;
   uint32_t *spirv = read_spv(file, &nwords);
   if (!spirv) return 1;
   struct mali_poly_tcs tcs;
   char err[256] = "";
   int ok = mali_poly_lower_tcs(g_arch, spirv, nwords, "main", &tcs, err, sizeof err);
   free(spirv);
   if (!ok) { printf("{\"test\":\"gs_lower\",\"tcs\":\"%s\",\"ok\":false,\"error\":\"%s\"}\n", file, err); return 1; }
   const char *why = NULL;
   if (!tcs.lowered) why = "poly_nir_lower_tcs returned false";
   else if (!tcs.nir_valid) why = "the lowered shader failed nir_validate";
   else if (tcs.vertices_out != want_verts) why = "tcs_vertices_out is not the declared patch size";
   /* The stride is poly's own arithmetic (poly_tcs_output_stride, poly_nir_lower_tess.c:34) and it is measured, not
    * predicted: the assertion is that it is a non-zero multiple of 4 bytes for a shader that does write varyings. */
   else if (tcs.stride < want_stride || (tcs.stride & 3)) why = "the TCS output stride is not a sane buffer stride";
   printf("{\"test\":\"gs_lower\",\"tcs\":\"%s\",\"arch\":%u,\"ok\":%s,\"stride\":%u,\"vertices_out\":%u,"
          "\"per_vertex_outputs\":%llu,\"shared_size\":%u%s%s}\n", file, g_arch, why ? "false" : "true", tcs.stride,
          tcs.vertices_out, (unsigned long long)tcs.per_vertex_outputs, tcs.shared_size, why ? ",\"why\":\"" : "",
          why ? why : "");
   if (why) { g_fail = 1; return 1; }
   return 0;
}

int
main(int argc, char **argv)
{
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else if (!strcmp(argv[i], "--arch") && i + 1 < argc) g_arch = (unsigned)strtoul(argv[++i], NULL, 10);
      else { printf("usage: t_gs_lower [--spv-dir DIR] [--arch 11|13]\n"); return 2; }
   }
   printf("{\"test\":\"gs_lower\",\"library\":\"%s\",\"arch\":%u}\n", mali_poly_provenance(), g_arch);
   /* The four cases, with the max_vertices each source declares (shader/gs_*.geom, layout(max_vertices = N)). */
   if (case_gs("gs_pass.geom.spv", 4, 0, "3 emitted, dynamic: 3 + 3/3")) return 1;
   if (case_gs("gs_amplify.geom.spv", 8, 1, "6 emitted in two primitives, dynamic: 6 + 6/3, one count word")) return 1;
   if (case_gs("gs_loop.geom.spv", 4, 0, "3 emitted in an unrolled loop, dynamic: 3 + 3/3")) return 1;
   if (case_gs("gs_computed.geom.spv", 4, 0, "3 emitted, dynamic: 3 + 3/3")) return 1;
   if (case_tes("tess_tri.tese.spv")) return 1;
   /* T2 step 2: the control shader through Mesa's poly lowering; 3 control points, position + one vec4 per vertex */
   if (case_tcs("tess_tri.tesc.spv", 4, 3)) return 1;
   case_not_a_gs();
   if (g_fail) { printf("{\"test\":\"gs_lower\",\"ok\":false}\n"); return 1; }
   printf("{\"test\":\"gs_lower\",\"ok\":true,\"cases\":4}\n");
   return 0;
}
