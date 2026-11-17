/* kdecode: run Mesa pandecode (CSF interpreter + descriptor decode + Valhall disasm) over a memory snapshot.
 * Usage: kdecode <snap.mem> <kick_no> <arch 11|13> <cs_va hex> <size_bytes> [ring|chunk]
 * Env KD_ZERO=va_hex:len maps a zero region (PanVK exec BO at 0x800000000000 is not in the trace).
 * snap.mem = records { u64 magic 0x4d454d53, u64 kick, u64 va, u64 len, u64 prot, bytes } (driver/tools/ioctl_trace.c;
 * fake2mem.py converts a KB_FAKE_DUMP). "chunk" = decode a called chunk directly (no ring), same interpreter. */
#include "panfrost/lib/wrap.h"
#include "panfrost/compiler/bifrost/valhall/disassemble.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void dis(FILE *fp, const void *code, size_t sz, uint64_t gpu_id, bool verbose)
{
   (void)gpu_id; (void)verbose;
   if (sz > 1024) sz = 1024;
   disassemble_valhall(fp, code, sz, true);
}

int main(int argc, char **argv)
{
   if (argc < 6) { fprintf(stderr, "usage\n"); return 2; }
   FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
   unsigned long long kick = strtoull(argv[2], 0, 0);
   unsigned arch = atoi(argv[3]);
   uint64_t va = strtoull(argv[4], 0, 16); uint32_t size = strtoul(argv[5], 0, 0);
   uint64_t gpu_id = arch == 13 ? 0xd8000000ull : 0xb8030000ull;
   struct pandecode_context *ctx = pandecode_create_context(true);
   pandecode_set_disassemble(ctx, dis);
   uint64_t h[5];
   while (fread(h, 8, 5, f) == 5) {
      if (h[0] != 0x4d454d53ull) { fprintf(stderr, "bad magic\n"); return 1; }
      void *p = malloc(h[3]);
      if (fread(p, 1, h[3], f) != h[3]) return 1;
      if (h[1] == kick) pandecode_inject_mmap(ctx, h[2], p, h[3], NULL); else free(p);
   }
   const char *z = getenv("KD_ZERO");   /* KD_ZERO=va_hex:len : map zeros (e.g. PanVK's exec BO, never CPU-mapped, so not in the trace) */
   if (z) { uint64_t zva = strtoull(z, 0, 16); size_t zl = strtoul(strchr(z, ':') + 1, 0, 0); pandecode_inject_mmap(ctx, zva, calloc(1, zl), zl, NULL); }
   static uint32_t regs[256];
   pandecode_interpret_cs(ctx, va, size, gpu_id, regs);
   fflush(stderr);
   return 0;
}
