/* SPDX-License-Identifier: MIT. Offline: compile a compute SPIR-V (set0: b0 combined sampler, b1 SSBO, push 20 B) with mali_compile for v11/v13.
 * Usage: BIFROST_MESA_DEBUG=shaders t_txf_cc <arch> <file.spv> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/mali_compile.h"
int main(int argc, char **argv)
{
   if (argc < 3) return 2;
   FILE *f = fopen(argv[2], "rb"); if (!f) return 2;
   fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
   uint32_t *spv = malloc(n); if (fread(spv, 1, n, f) != (size_t)n) return 2;
   struct mali_lbinding b[2] = { { MALI_DT_COMBINED_IMAGE_SAMPLER, 1, 0, 2, 0, MALI_NO_IMM_SAMPLER, 0 }, { MALI_DT_STORAGE_BUFFER, 1, 2, 1, 0, MALI_NO_IMM_SAMPLER, 0 } };
   struct mali_compile_layout lay; memset(&lay, 0, sizeof lay);
   lay.nsets = 1; lay.set[0] = (struct mali_lset){ 2, 3, 0, b }; lay.push_size = 20;
   struct mali_compile_opts o = { 0 }; struct mali_compiled c; char err[512] = "";
   o.arch = atoi(argv[1]); o.stage = MALI_STAGE_COMPUTE; o.spirv = spv; o.spirv_words = n / 4; o.layout = &lay; o.fau_reserved = 12; o.cache_dir = "";
   int r = mali_compile(&o, &c, err, sizeof err);
   printf("rc %d %s regs %u bin %u tls %u push %u fau %u/%u nimm %u\n", r, err, c.work_reg_count, c.bin_size, c.tls_size, c.push_const_bytes, c.fau_count, c.fau_reserved, c.nimm);
   for (unsigned i = 0; i < c.nimm; i++) printf("imm word %u = 0x%x\n", c.imm[i].word, c.imm[i].value);
   return r;
}
