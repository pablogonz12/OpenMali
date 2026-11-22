/* SPDX-License-Identifier: MIT. Offline: compile a fragment SPIR-V that reads a combined image sampler at set 2
 * binding 128 (the frame's layout, notes/blitreplay.md) and check the resource handles the lowering gives the
 * texture instruction. Offline, no GPU: the failing-first test for the texelFetch path (notes/txf-fix.md).
 *
 * A NIR txf carries a texture deref and no sampler deref. The lowering then has to choose the sampler handle itself:
 * pan_res_handle(0, MALI_DUMMY_SAMPLER_IDX) (the all-zero driver-set entry) or the sampler half of the same combined
 * image sampler (the binding's slot + stride - 1). This test pins which one a txf gets, and that a plain texture()
 * gets the real sampler.
 *
 * Usage: BIFROST_MESA_DEBUG=shaders t_txf <arch> <file.spv> [--print]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/mali_compile.h"

/* the packed texture handle the Valhall backend emits: (tex_table<<27)|(tex_index<<16)|(samp_table<<11)|samp_index
 * (third_party/mesa/src/panfrost/compiler/pan_nir_lower_tex.c va_pack_tex_handle) */
static unsigned
find_tex_handle(const struct mali_compiled *c, uint32_t *out)
{
   unsigned n = 0;
   for (unsigned i = 0; i < c->nimm; i++) {
      const uint32_t w = c->imm[i].value;
      /* tex table 3 (set 2 + 1), texture index 0, a plausible sampler table and index */
      if (((w >> 27) & 0xf) == 3 && ((w >> 16) & 0x7ff) == 0 && ((w >> 11) & 0xf) <= 3 && (w & 0x7ff) < 32) {
         out[n++] = w;
      }
   }
   return n;
}

int
main(int argc, char **argv)
{
   int pr = argc > 3 && !strcmp(argv[3], "--print");
   if (argc < 3) return 2;
   FILE *f = fopen(argv[2], "rb");
   if (!f) { perror(argv[2]); return 2; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *spv = malloc(n);
   if (fread(spv, 1, n, f) != (size_t)n) return 2;
   fclose(f);

   /* the frame's layout: set 0 and 1 unused, set 2 = one combined image sampler, binding 128, slot 0, stride 2
    * (notes/blitreplay.md, notes/bindless-mech.md) */
   /* the table is dense, indexed by binding number: 129 entries, binding 128 used */
   static struct mali_lbinding b[129];
   b[128] = (struct mali_lbinding){ MALI_DT_COMBINED_IMAGE_SAMPLER, 1, 0, 2, 0, MALI_NO_IMM_SAMPLER, 0 };
   struct mali_compile_layout lay;
   memset(&lay, 0, sizeof lay);
   lay.nsets = 3;
   lay.set[2] = (struct mali_lset){ 129, 1, 0, b };
   lay.push_size = 0;
   struct mali_compile_opts o = { 0 };
   struct mali_compiled c;
   char err[512] = "";
   o.arch = atoi(argv[1]);
   o.stage = MALI_STAGE_FRAGMENT;
   o.spirv = spv;
   o.spirv_words = n / 4;
   o.layout = &lay;
   o.cache_dir = "";
   int r = mali_compile(&o, &c, err, sizeof err);
   printf("rc %d %s regs %u bin %u\n", r, err, c.work_reg_count, c.bin_size);
   if (r) return r;

   uint32_t h[8];
   unsigned nh = find_tex_handle(&c, h);
   if (pr) {
      for (unsigned i = 0; i < c.nimm; i++) printf("imm word %u = 0x%08x\n", c.imm[i].word, c.imm[i].value);
      printf("tex handles: %u\n", nh);
   }
   for (unsigned i = 0; i < nh; i++)
      printf("tex handle 0x%08x: table %u index %u sampler table %u index %u\n", h[i], (h[i] >> 27) & 0xf,
             (h[i] >> 16) & 0x7ff, (h[i] >> 11) & 0xf, h[i] & 0x7ff);

   /* A texelFetch of the combined image sampler must use that binding's sampler half (slot + stride - 1 = 1), not
    * the all-zero dummy at table 0 entry 16: the dummy descriptor makes the hardware fetch return 0 on the phone
    * (notes/txf-fix.md). A texture() carries its own sampler deref and must keep the same real handle. */
   int ok = 1;
   if (!nh) {
      printf("FAIL no packed texture handle in the compiled shader\n");
      ok = 0;
   }
   for (unsigned i = 0; i < nh; i++) {
      const unsigned st = (h[i] >> 11) & 0xf, si = h[i] & 0x7ff;
      if (st != 3 || si != 1) {
         printf("FAIL texture instruction uses sampler handle table %u index %u, want table 3 index 1 "
                "(the combined image sampler's own sampler)\n", st, si);
         ok = 0;
      }
   }
   printf("%s\n", ok ? "ok txf handle" : "FAILED");
   return ok ? 0 : 1;
}
