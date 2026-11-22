/* SPDX-License-Identifier: MIT */
/* t_cs_barrier — T2 step 3's blocker, on the host (notes/tess-design.md § T2): does a compute shader with `barrier()` and
 * shared memory compile, and does our compiler tell the emitter what it needs?
 *
 * White-box (linked statically with the driver objects, like t_gs_lower), so it calls mali_compile() directly and reads
 * the metadata the CS emitter uses:
 *
 *   * `wls_size` — the per-workgroup local storage the shader needs (Mesa's `pan_shader_info::wls_size`,
 *     pan_compiler.h:431). Zero here would mean the barrier allocation in mali_cs_emit.c:837-859 is never taken; for a
 *     64-uint shared array it must cover 256 B.
 *   * `allow_merge` — the CS descriptor's "workgroups may be merged" bit (mali_cs_emit.c:914 writes it into CR_WG_SIZE
 *     bit 31). A shader with shared memory or a workgroup barrier must NOT set it: Mesa's own rule is
 *     valhall_can_merge_workgroups() (bifrost_nir.c:517 — `shared_size != 0` or a `nir_intrinsic_barrier` with
 *     SCOPE_WORKGROUP returns false), and PanVK feeds exactly that into its descriptor (panvk_vX_shader.c:1598, with the
 *     divergence options re-picked when merging is allowed, :1605-1611).
 *   * `bin_size`, `work_reg_count`, `local_size[0]` — the program exists and the workgroup width reached the backend.
 *
 * The control is the same check on a shader without shared memory (saxpy.comp.spv): `wls_size == 0` and
 * `allow_merge == 1`, i.e. the two values above are not constants and the merge bit is not simply off everywhere.
 *
 * What this does NOT prove: that the hardware honours the barrier. That needs a device run of `t_vk_compute --only
 * barrier` (the same shader, 64-wide, dispatched from the real ICD).
 *   t_cs_barrier [--spv-dir DIR] [--arch 11|13] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mali_compile.h"
#include "mali_pipeline_layout.h"

static const char *g_spv = "shader";
static unsigned g_arch = 11;
static int g_fail;

static uint32_t *
read_spv(const char *name, uint32_t *nwords)
{
   char path[512];
   snprintf(path, sizeof path, "%s/%s", g_spv, name);
   FILE *f = fopen(path, "rb");
   if (!f) { printf("{\"test\":\"cs_barrier\",\"ok\":false,\"fail\":\"open %s\"}\n", path); return NULL; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (n <= 0 || (n % 4)) { fclose(f); printf("{\"test\":\"cs_barrier\",\"ok\":false,\"fail\":\"%s is not SPIR-V\"}\n", path); return NULL; }
   uint32_t *buf = malloc((size_t)n);
   if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); printf("{\"test\":\"cs_barrier\",\"ok\":false,\"fail\":\"read %s\"}\n", path); return NULL; }
   fclose(f);
   *nwords = (uint32_t)(n / 4);
   return buf;
}

/* Compile one module and return the metadata the CS path consumes. */
static int
compile_one(const char *file, struct mali_compiled *out, unsigned *words_used)
{
   uint32_t nwords = 0;
   uint32_t *spirv = read_spv(file, &nwords);
   if (!spirv) return 0;
   char err[256] = "";
   /* Two storage buffers at set 0, bindings 0 and 1 — the layout the shader's SSBOs need; without one the backend hits
    * `vulkan_resource_index` and asserts (bifrost_compile.c:2158), which is a test bug, not a driver one. */
   /* slots 0 and 1: one 32-byte descriptor each, in binding order */
   static const struct mali_lbinding d0[2] = {
      {.type = MALI_DT_STORAGE_BUFFER, .count = 1, .slot = 0, .stride = 1, .imm_off = MALI_NO_IMM_SAMPLER},
      {.type = MALI_DT_STORAGE_BUFFER, .count = 1, .slot = 1, .stride = 1, .imm_off = MALI_NO_IMM_SAMPLER},
   };
   struct mali_lset set0 = {.nbindings = 2, .slot_count = 2, .b = d0};
   struct mali_compile_layout lay = {.nsets = 1, .push_size = 8};   /* saxpy.comp's { float a; uint n; } */
   lay.set[0] = set0;
   struct mali_compile_opts o = {.arch = g_arch, .stage = MALI_STAGE_COMPUTE, .spirv = spirv, .spirv_words = nwords,
                                 .layout = &lay,
                                 .cache_dir = "" /* never the disk cache: this checks the compiler, not a stored .bin */};
   memset(out, 0, sizeof *out);
   const int rc = mali_compile(&o, out, err, sizeof err);
   free(spirv);
   if (words_used) *words_used = nwords;
   if (rc) {
      printf("{\"test\":\"cs_barrier\",\"file\":\"%s\",\"arch\":%u,\"ok\":false,\"error\":\"%s\"}\n", file, g_arch, err);
      return 0;
   }
   return 1;
}

int
main(int argc, char **argv)
{
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else if (!strcmp(argv[i], "--arch") && i + 1 < argc) g_arch = (unsigned)strtoul(argv[++i], NULL, 10);
      else { printf("usage: t_cs_barrier [--spv-dir DIR] [--arch 11|13]\n"); return 2; }
   }

   /* 1. the barrier + shared-memory shader: compiles, needs WLS, and must not claim mergeable workgroups */
   struct mali_compiled b;
   if (!compile_one("barrier.comp.spv", &b, NULL)) return 1;
   const char *why = NULL;
   if (b.local_size[0] != 64) why = "local_size_x is not 64";
   else if (b.wls_size < 256) why = "wls_size does not cover the 64-uint shared array";
   else if (b.allow_merge) why = "a shader with a workgroup barrier claims mergeable workgroups";
   else if (b.bin_size == 0) why = "no program was produced";
   printf("{\"test\":\"cs_barrier\",\"file\":\"barrier.comp.spv\",\"arch\":%u,\"local_size\":[%u,%u,%u],\"wls_size\":%u,"
          "\"allow_merge\":%u,\"bin_size\":%u,\"work_regs\":%u",
          g_arch, b.local_size[0], b.local_size[1], b.local_size[2], b.wls_size, b.allow_merge, b.bin_size, b.work_reg_count);
   if (why) { printf(",\"ok\":false,\"why\":\"%s\"}\n", why); mali_compiled_free(&b); return 1; }
   printf(",\"ok\":true}\n");
   mali_compiled_free(&b);

   /* 2. the control: no shared memory, no barrier — zero WLS and the merge bit set */
   struct mali_compiled s;
   if (!compile_one("saxpy.comp.spv", &s, NULL)) return 1;
   const char *cwhy = NULL;
   if (s.wls_size != 0) cwhy = "a shader without shared memory asked for WLS";
   else if (!s.allow_merge) cwhy = "a shader without a barrier did not get the merge bit";
   printf("{\"test\":\"cs_barrier\",\"file\":\"saxpy.comp.spv\",\"arch\":%u,\"wls_size\":%u,\"allow_merge\":%u",
          g_arch, s.wls_size, s.allow_merge);
   if (cwhy) { printf(",\"ok\":false,\"why\":\"%s\"}\n", cwhy); mali_compiled_free(&s); return 1; }
   printf(",\"ok\":true}\n");
   mali_compiled_free(&s);

   printf("{\"test\":\"cs_barrier\",\"arch\":%u,\"ok\":true}\n", g_arch);
   (void)g_fail;
   return 0;
}
