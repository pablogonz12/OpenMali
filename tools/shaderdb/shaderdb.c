/* SPDX-License-Identifier: MIT */
/* shaderdb: offline mali_compile() statistics over a SPIR-V corpus.
 * usage: shaderdb [-a 11,13] FILE.spv...   Stage comes from the SPIR-V entry point. TSV on stdout (header first):
 *   file arch stage status instr regs spills wls fau imm bytes time_us
 * instr = bin_size / 8 (Valhall instructions are 64 bit), regs = work_reg_count, spills = tls_size bytes (TLS is only used for spills/arrays).
 * Disk cache off; the process is single-threaded so time_us is stable enough to compare (median of MALI_SHADERDB_REPS, default 3). */
#include "../../vk/src/mali_compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static int
stage_of(const uint32_t *w, size_t n)
{
   for (size_t i = 5; i < n;) {
      unsigned wc = w[i] >> 16, op = w[i] & 0xffff;
      if (!wc || i + wc > n) break;
      if (op == 15) { switch (w[i + 1]) { case 0: return MALI_STAGE_VERTEX; case 4: return MALI_STAGE_FRAGMENT; case 5: return MALI_STAGE_COMPUTE; default: return -1; } }
      i += wc;
   }
   return -1;
}


/* Minimal SPIR-V reflection: build a descriptor layout (sets/bindings/types/array counts) so descriptor-using shaders compile.
 * Slots are packed per set; push constants are given the full 128 bytes. */
struct tinfo { uint16_t op; const uint32_t *w; };
struct layout_store { struct mali_compile_layout lay; struct mali_lbinding b[MALI_MAX_SETS][32]; };

static unsigned
const_u32(struct tinfo *t, uint32_t bound, uint32_t id)
{
   return id < bound && t[id].op == 43 ? t[id].w[3] : 1;
}

static int
build_layout(const uint32_t *w, size_t n, struct layout_store *L)
{
   uint32_t bound = w[3];
   if (bound > (1u << 22)) return 0;
   struct tinfo *t = calloc(bound, sizeof *t);
   uint8_t *set = calloc(bound, 1), *bind = calloc(bound, 1), *has = calloc(bound, 1), *bufblock = calloc(bound, 1);
   memset(L, 0, sizeof *L);
   for (size_t i = 5; i < n;) {
      unsigned wc = w[i] >> 16, op = w[i] & 0xffff;
      if (!wc || i + wc > n) break;
      const uint32_t *p = w + i;
      switch (op) {
      case 71: /* OpDecorate */
         if (p[1] < bound) {
            if (p[2] == 34) { set[p[1]] = p[3]; has[p[1]] |= 1; }
            else if (p[2] == 33) { bind[p[1]] = p[3]; has[p[1]] |= 2; }
            else if (p[2] == 3) bufblock[p[1]] = 1;
         }
         break;
      case 25: case 26: case 27: case 28: case 29: case 30: case 32: /* types: result id at p[1] */
         if (p[1] < bound) { t[p[1]].op = op; t[p[1]].w = p; }
         break;
      }
      if ((op == 59 || op == 43) && p[2] < bound) { t[p[2]].op = 59; t[p[2]].w = p; }
      i += wc;
   }
   unsigned nsets = 0, slot[MALI_MAX_SETS] = {0}, nb[MALI_MAX_SETS] = {0};
   for (uint32_t id = 0; id < bound; id++) {
      if (t[id].op != 59 || (has[id] & 3) != 3) continue;
      const uint32_t *v = t[id].w;
      uint32_t sc = v[3], ty = v[1];
      if (ty >= bound || t[ty].op != 32) continue;
      uint32_t cur = t[ty].w[3];            /* pointee */
      unsigned count = 1;
      while (cur < bound && (t[cur].op == 28 || t[cur].op == 29)) {
         count *= t[cur].op == 28 ? const_u32(t, bound, t[cur].w[3]) : 16;
         cur = t[cur].w[2];
      }
      if (cur >= bound) continue;
      unsigned type;
      if (sc == 0) {   /* UniformConstant */
         if (t[cur].op == 27) type = MALI_DT_COMBINED_IMAGE_SAMPLER;
         else if (t[cur].op == 26) type = MALI_DT_SAMPLER;
         else if (t[cur].op == 25) {
            unsigned dim = t[cur].w[3], sampled = t[cur].w[7];
            type = dim == 5 ? (sampled == 2 ? MALI_DT_STORAGE_TEXEL_BUFFER : MALI_DT_UNIFORM_TEXEL_BUFFER)
                            : dim == 6 ? MALI_DT_INPUT_ATTACHMENT : (sampled == 2 ? MALI_DT_STORAGE_IMAGE : MALI_DT_SAMPLED_IMAGE);
         } else continue;
      } else if (sc == 2) type = bufblock[cur] ? MALI_DT_STORAGE_BUFFER : MALI_DT_UNIFORM_BUFFER;
      else if (sc == 12) type = MALI_DT_STORAGE_BUFFER;
      else continue;
      unsigned s = set[id], b = bind[id];
      if (s >= MALI_MAX_SETS || b >= 32) continue;
      if (!L->b[s][b].count) {
         unsigned stride = mali_desc_stride(type);
         L->b[s][b] = (struct mali_lbinding){ type, count, slot[s], stride, 0, MALI_NO_IMM_SAMPLER };
         slot[s] += count * stride;
      }
      if (b + 1 > nb[s]) nb[s] = b + 1;
      if (s + 1 > nsets) nsets = s + 1;
   }
   for (unsigned s = 0; s < MALI_MAX_SETS; s++)
      for (unsigned b = 0; b < 32; b++) if (!L->b[s][b].count) L->b[s][b].type = MALI_DT_NONE;
   L->lay.nsets = nsets;
   for (unsigned s = 0; s < nsets; s++) L->lay.set[s] = (struct mali_lset){ nb[s], slot[s], 0, L->b[s] };
   L->lay.push_size = MALI_MAX_PUSH_BYTES;
   free(t); free(set); free(bind); free(has); free(bufblock);
   return 1;
}

int
main(int argc, char **argv)
{
   unsigned archs[3] = { 11, 13, 0 }, na = 2;
   int a = 1;
   if (argc > 2 && !strcmp(argv[1], "-a")) {
      na = 0;
      for (char *s = strtok(argv[2], ","); s && na < 3; s = strtok(NULL, ",")) archs[na++] = atoi(s);
      a = 3;
   }
   const char *r = getenv("MALI_SHADERDB_REPS");
   int reps = r ? atoi(r) : 3;
   if (reps < 1 || reps > 15) reps = 3;
   puts("file\tarch\tstage\tstatus\tinstr\tregs\tspills\twls\tfau\timm\tbytes\ttime_us");
   for (; a < argc; a++) {
      FILE *f = fopen(argv[a], "rb");
      if (!f) { fprintf(stderr, "cannot open %s\n", argv[a]); continue; }
      fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
      uint32_t *w = malloc(sz + 4);
      size_t n = fread(w, 1, sz, f) / 4;
      fclose(f);
      int st = stage_of(w, n);
      const char *b = strrchr(argv[a], '/'); b = b ? b + 1 : argv[a];
      for (unsigned i = 0; i < na; i++) {
         if (st < 0) { printf("%s\t%u\t-\tNOSTAGE\t0\t0\t0\t0\t0\t0\t0\t0\n", b, archs[i]); continue; }
         struct mali_compile_opts o = { .arch = archs[i], .stage = st, .spirv = w, .spirv_words = n, .cache_dir = "" };
         struct layout_store *L = mmap(NULL, sizeof *L, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
         if (build_layout(w, n, L)) o.layout = &L->lay;
         o.fau_reserved = st == MALI_STAGE_COMPUTE ? 32 : 0;   /* compute: room for 128 B of push constants */
         /* each compile in a child process: asserts in the compiler become ERR rows instead of killing the run */
         struct { int rc; unsigned v[8]; double t; } *res = mmap(NULL, sizeof *res, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
         res->rc = -1;
         fflush(stdout);
         pid_t pid = fork();
         if (pid == 0) {
            if (!getenv("SHADERDB_V")) freopen("/dev/null", "w", stderr);
            alarm(getenv("SHADERDB_TIMEOUT") ? atoi(getenv("SHADERDB_TIMEOUT")) : 60);
            double t[15]; struct mali_compiled c; char err[256] = ""; int rc = 0, ok = 0;
            for (int k = 0; k < reps; k++) {
               double t0 = now();
               rc = mali_compile(&o, &c, err, sizeof err);
               t[k] = (now() - t0) * 1e6;
               if (rc) break;
               ok++;
               if (k + 1 < reps) mali_compiled_free(&c);
            }
            if (rc && getenv("SHADERDB_V")) fprintf(stderr, "%s v%u: %s\n", b, o.arch, err);
            res->rc = rc || !ok ? 1 : 0;
            if (!res->rc) {
               qsort(t, ok, sizeof t[0], cmpd);
               res->t = t[ok / 2];
               res->v[0] = c.bin_size / 8; res->v[1] = c.work_reg_count; res->v[2] = c.tls_size; res->v[3] = c.wls_size;
               res->v[4] = c.fau_count; res->v[5] = c.nimm; res->v[6] = c.bin_size;
            }
            _exit(0);
         }
         int wstat = 0;
         waitpid(pid, &wstat, 0);
         const char *sn = st == 0 ? "vert" : st == 1 ? "frag" : "comp";
         if (WIFSIGNALED(wstat)) printf("%s\t%u\t%s\tSIG%d\t0\t0\t0\t0\t0\t0\t0\t0\n", b, archs[i], sn, WTERMSIG(wstat));
         else if (WIFEXITED(wstat) && WEXITSTATUS(wstat) != 0) printf("%s\t%u\t%s\tEXIT%d\t0\t0\t0\t0\t0\t0\t0\t0\n", b, archs[i], sn, WEXITSTATUS(wstat));
         else if (res->rc != 0) printf("%s\t%u\t%s\tERR\t0\t0\t0\t0\t0\t0\t0\t0\n", b, archs[i], sn);
         else printf("%s\t%u\t%s\tOK\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%.0f\n", b, archs[i], sn, res->v[0], res->v[1], res->v[2], res->v[3], res->v[4], res->v[5], res->v[6], res->t);
         munmap(res, sizeof *res); munmap(L, sizeof *L);
      }
      free(w);
   }
   return 0;
}
