/* SPDX-License-Identifier: MIT */
/* Offline tests of the GS front half (mali_gs.c): what a GS module declares, the record/index layout, and the
 * passthrough vertex shader. No GPU, no pipeline: see docs/gs-design.md for the remaining stages. */
#include "../src/mali_compile.h"
#include "../src/mali_gs.h"

#include "nir.h"
#include "panfrost/compiler/pan_compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const char *g_dir = "driver/vk/shader";

struct spv { uint32_t *w; size_t n; };

static struct spv
load(const char *name)
{
   char p[512];
   snprintf(p, sizeof p, "%s/%s", g_dir, name);
   struct spv s = {0};
   FILE *f = fopen(p, "rb");
   if (!f) { printf("  cannot open %s\n", p); g_fail++; return s; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   s.w = malloc(n);
   if (fread(s.w, 1, n, f) != (size_t)n) { g_fail++; free(s.w); s.w = NULL; }
   else s.n = n / 4;
   fclose(f);
   return s;
}

static void
analyze(const char *what, unsigned arch, const struct spv *s, struct mali_gs_info *gs)
{
   struct mali_compile_opts o = { .arch = arch, .stage = MALI_STAGE_COMPUTE, .spirv = s->w, .spirv_words = s->n };
   char err[256] = "";
   int r = mali_gs_analyze(&o, gs, err, sizeof err);
   CHECK(r == 0, "%s (arch %u): analyze failed: %s", what, arch, err);
}

static int
output_index(const struct mali_gs_info *gs, unsigned location)
{
   for (unsigned i = 0; i < gs->noutputs; i++)
      if (gs->output[i].location == location)
         return (int)i;
   return -1;
}

static void
test_basic(unsigned arch)
{
   struct spv s = load("gs/basic.geom.spv");
   if (!s.w)
      return;
   struct mali_gs_info gs;
   analyze("basic", arch, &s, &gs);
   CHECK(gs.vertices_out == 3, "basic: vertices_out %u != 3", gs.vertices_out);
   CHECK(gs.invocations == 1, "basic: invocations %u != 1", gs.invocations);
   CHECK(gs.input_prim == MESA_PRIM_TRIANGLES, "basic: input prim %u != triangles", gs.input_prim);
   CHECK(gs.output_prim == MESA_PRIM_TRIANGLE_STRIP, "basic: output prim %u != triangle strip", gs.output_prim);
   CHECK(gs.noutputs == 2, "basic: %u outputs != 2", gs.noutputs);
   CHECK(gs.outputs_written & VARYING_BIT_POS, "basic: no gl_Position output");
   CHECK(output_index(&gs, VARYING_SLOT_VAR0) >= 0, "basic: out_col missing");
   int pos = output_index(&gs, VARYING_SLOT_POS);
   CHECK(pos >= 0 && gs.output[pos].ncomps == 4, "basic: gl_Position must take a 4-component slot");
   int col = output_index(&gs, VARYING_SLOT_VAR0);
   CHECK(col >= 0 && gs.output[col].ncomps == 3, "basic: out_col must take 3 components");
   printf("  basic v%u: %u outputs, max_vertices %u, invocations %u, in %u -> out %u, record %u B, index %u u32\n",
          arch, gs.noutputs, gs.vertices_out, gs.invocations, gs.input_prim, gs.output_prim,
          gs.record_stride, gs.index_stride);
   CHECK(gs.record_stride == 32, "basic: record stride %u != 32 (2 slots x 16)", gs.record_stride);
   CHECK(gs.index_stride == 8, "basic: index stride %u != 8 (2 * 3 padded)", gs.index_stride);
   free(s.w);
}

static void
test_points(unsigned arch)
{
   struct spv s = load("gs/points.geom.spv");
   if (!s.w)
      return;
   struct mali_gs_info gs;
   analyze("points", arch, &s, &gs);
   CHECK(gs.vertices_out == 4, "points: vertices_out %u != 4", gs.vertices_out);
   CHECK(gs.input_prim == MESA_PRIM_POINTS, "points: input prim %u != points", gs.input_prim);
   CHECK(gs.output_prim == MESA_PRIM_TRIANGLE_STRIP, "points: output prim %u != triangle strip", gs.output_prim);
   CHECK(gs.record_stride == 32, "points: record stride %u != 32", gs.record_stride);
   CHECK(gs.index_stride == 8, "points: index stride %u != 8", gs.index_stride);
   free(s.w);
}

static void
test_layout(void)
{
   struct mali_gs_info gs = { .noutputs = 2, .vertices_out = 3, .invocations = 1 };
   mali_gs_layout(&gs, 0);
   const uint64_t arena = 256 * 1024;
   unsigned cap = mali_gs_cap_slots(&gs, arena);
   CHECK(cap > 0, "layout: no slots in a 256 KiB arena");
   /* records and indices must both fit: cap * (stride + index_stride * 4) <= arena */
   uint64_t need = (uint64_t)cap * (gs.record_stride * (uint64_t)gs.vertices_out +
                                    4ull * gs.index_stride);
   CHECK(need <= arena, "layout: %u slots need %llu bytes of a %llu-byte arena", cap,
         (unsigned long long)need, (unsigned long long)arena);
}

/* The passthrough VS must read the record base from the params block (push constant at pc_off + 8, the second
 * 64-bit field of struct mali_gs_params) and reproduce exactly the GS's outputs. */
static void
test_passthrough(unsigned arch)
{
   struct spv s = load("gs/basic.geom.spv");
   if (!s.w)
      return;
   struct mali_gs_info gs;
   analyze("basic-pt", arch, &s, &gs);
   const unsigned pc_off = 64;   /* whatever the driver appends after the app's push range */
   const struct nir_shader_compiler_options *opts =
      pan_get_nir_shader_compiler_options(arch, MESA_SHADER_VERTEX, false);
   nir_shader *vs = mali_gs_passthrough_nir(opts, &gs, pc_off);
   CHECK(vs != NULL, "passthrough: builder returned NULL");
   if (!vs) { free(s.w); return; }
   CHECK(vs->info.stage == MESA_SHADER_VERTEX, "passthrough: stage");
   nir_shader_gather_info(vs, nir_shader_get_entrypoint(vs));
   CHECK(vs->info.outputs_written == (gs.outputs_written & ~VARYING_BIT_VIEWPORT),
         "passthrough: outputs 0x%llx != GS outputs 0x%llx",
         (unsigned long long)vs->info.outputs_written, (unsigned long long)gs.outputs_written);
   CHECK(vs->num_outputs == gs.noutputs, "passthrough: %u outputs != %u", vs->num_outputs, gs.noutputs);

   bool base_ok = false;
   nir_foreach_block(block, nir_shader_get_entrypoint(vs))
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_load_push_constant)
            continue;
         if (nir_src_is_const(intr->src[0]) && nir_src_as_uint(intr->src[0]) == pc_off + 8)
            base_ok = true;
      }
   CHECK(base_ok, "passthrough: no load of struct mali_gs_params.records at pc_off + 8");
   ralloc_free(vs);
   free(s.w);
}

int
main(int argc, char **argv)
{
   if (argc > 1)
      g_dir = argv[1];
   printf("t_gs: %s\n", g_dir);
   for (unsigned arch = 11; arch <= 13; arch += 2) {
      test_basic(arch);
      test_points(arch);
      test_passthrough(arch);
   }
   test_layout();
   if (g_fail)
      printf("FAILED (%d)\n", g_fail);
   else
      printf("OK\n");
   return g_fail ? 1 : 0;
}
