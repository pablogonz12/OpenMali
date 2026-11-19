/* SPDX-License-Identifier: MIT */
/* Mutation fuzzer for the SPIR-V ingest (mali_compile): offline, no GPU.
 * Usage: fuzz_spirv shader_dir [iterations=2000] [seed=1] [repro_dir]
 * Every case runs in a forked child with a 20 s alarm; a signal or hang is a finding (input saved to repro_dir).
 * Exit 0 = no crash/hang. Rejected modules (compile error) are fine. */
#include "mali_compile.h"
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 11); }

static uint32_t *
load(const char *p, size_t *n)
{
   FILE *f = fopen(p, "rb");
   if (!f) return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *w = malloc(sz + 16);
   if (fread(w, 1, sz, f) != (size_t)sz) { free(w); fclose(f); return NULL; }
   fclose(f);
   *n = sz / 4;
   return w;
}

static size_t
mutate(uint32_t *w, size_t n)
{
   int k = 1 + rnd() % 3;
   while (k--) {
      if (n < 6) break;
      size_t i = 5 + rnd() % (n - 5);
      switch (rnd() % 7) {
      case 0: w[i] ^= 1u << (rnd() % 32); break;
      case 1: w[i] = rnd(); break;
      case 2: w[i] = (rnd() & 1) ? 0 : 0xffffffffu; break;
      case 3: w[i] = rnd() % 64; break;                         /* small ids / counts */
      case 4: n = i; break;                                     /* truncate */
      case 5: if (i + 1 < n) { memmove(w + i, w + i + 1, (n - i - 1) * 4); n--; } break;   /* delete a word */
      case 6: w[i] = (w[i] & 0xffff) | ((rnd() % 40) << 16); break;   /* word count of an instruction header */
      }
   }
   if (rnd() % 16 == 0) w[3] = rnd() % 4096;   /* id bound */
   return n;
}

static int
run_case(const uint32_t *w, size_t n, unsigned arch, enum mali_stage st)
{
   pid_t p = fork();
   if (p == 0) {
      alarm(20);
      struct mali_compile_opts o = { .arch = arch, .stage = st, .spirv = w, .spirv_words = n, .cache_dir = "" };
      struct mali_compiled c = {0};
      char err[256];
      if (mali_compile(&o, &c, err, sizeof err) == 0) mali_compiled_free(&c);
      _exit(0);
   }
   int status;
   waitpid(p, &status, 0);
   if (WIFSIGNALED(status)) return WTERMSIG(status);
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc < 2) return 2;
   if (strlen(argv[1]) > 4 && !strcmp(argv[1] + strlen(argv[1]) - 4, ".spv")) {   /* replay one file on every arch/stage */
      size_t n;
      uint32_t *w = load(argv[1], &n);
      int bad = 0;
      if (!w) { fprintf(stderr, "fuzz_spirv: cannot load %s\n", argv[1]); return 2; }
      for (unsigned a = 11; a <= 13; a += 2)
         for (int st = 0; st <= MALI_STAGE_COMPUTE; st++) {
            int sig = run_case(w, n, a, st);
            if (sig) { printf("replay %s arch=%u stage=%d: signal %d\n", argv[1], a, st, sig); bad++; }
         }
      free(w);
      return bad != 0;
   }
   int iters = argc > 2 ? atoi(argv[2]) : 2000;
   rs = (argc > 3 ? strtoull(argv[3], 0, 0) : 1) * 0x9E3779B97F4A7C15ull | 1;
   const char *rep = argc > 4 ? argv[4] : "/tmp";
   char *names[256]; int nn = 0;
   DIR *d = opendir(argv[1]);
   struct dirent *e;
   while (d && (e = readdir(d)) && nn < 256) {
      size_t l = strlen(e->d_name);
      if (l > 8 && !strcmp(e->d_name + l - 4, ".spv")) names[nn++] = strdup(e->d_name);
   }
   if (nn == 0) { fprintf(stderr, "fuzz_spirv: no .spv files in %s\n", argv[1]); return 2; }
   int bad = 0;
   for (int it = 0; it < iters; it++) {
      char p[600];
      snprintf(p, sizeof p, "%s/%s", argv[1], names[rnd() % nn]);
      size_t n;
      uint32_t *w = load(p, &n);
      if (!w) continue;
      enum mali_stage st = strstr(p, ".vert") ? MALI_STAGE_VERTEX : strstr(p, ".frag") ? MALI_STAGE_FRAGMENT : MALI_STAGE_COMPUTE;
      n = mutate(w, n);
      unsigned arch = (rnd() & 1) ? 13 : 11;
      int sig = run_case(w, n, arch, st);
      if (sig) {
         char o[700];
         snprintf(o, sizeof o, "%s/fuzz-crash-%d-%d.spv", rep, (int)getpid(), it);
         FILE *f = fopen(o, "wb");
         if (f) { fwrite(w, 4, n, f); fclose(f); }
         printf("CRASH sig=%d arch=%u stage=%d base=%s -> %s\n", sig, arch, st, p, o);
         bad++;
      }
      free(w);
   }
   printf("fuzz_spirv: %d cases, %d crashes\n", iters, bad);
   return bad != 0;
}
