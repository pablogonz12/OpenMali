/* SPDX-License-Identifier: MIT */
/* XFB emulation (docs/xfb-emulation.md): the real vertex-shader XFB variant is compiled for v11/v13 and inspected; the
 * capture plan (counts, counters, overflow, pause/resume, strips, instances) and the data layout are checked against a CPU
 * reference of the same shader. The GPU run of the variant is NOT covered here: the fake backend executes no shader code. */
#include "../src/mali_compile.h"
#include "../src/mali_backend.h"
#include "../src/mali_xfb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail;
/* mali_backend_fake.c's mali_backend_select() names both backends; this test links only the fake one, and never calls it. */
const struct mali_backend_ops mali_backend_kbase;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const char *g_dir = "driver/vk/shader";

static uint32_t *
load(const char *name, size_t *words)
{
   char p[512];
   snprintf(p, sizeof p, "%s/%s", g_dir, name);
   FILE *f = fopen(p, "rb");
   if (!f) { printf("  cannot open %s\n", p); g_fail++; return NULL; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *w = malloc(n);
   if (fread(w, 1, n, f) != (size_t)n) { g_fail++; free(w); fclose(f); return NULL; }
   fclose(f);
   *words = n / 4;
   return w;
}

/* The CPU twin of shader/xfb/basic.vert */
struct vsctx { float scale[4]; const float (*pos)[4]; const float (*col)[4]; };
static void
vs_basic(void *u, uint32_t vid, uint32_t iid, float *rec)
{
   const struct vsctx *c = u;
   (void)iid;
   for (int i = 0; i < 4; i++) rec[i] = c->pos[vid][i] * c->scale[i];
   for (int i = 0; i < 4; i++) rec[4 + i] = c->col[vid][i];
   rec[4] += (float)vid;
}

static void
test_compile(unsigned arch)
{
   size_t n;
   uint32_t *w = load("xfb/basic.vert.spv", &n);
   if (!w) return;
   struct mali_compile_opts base = { .arch = arch, .stage = MALI_STAGE_VERTEX, .spirv = w, .spirv_words = n, .cache_dir = "" };
   struct mali_compiled plain = {0}, x = {0};
   char err[256] = "";
   CHECK(mali_compile(&base, &plain, err, sizeof err) == 0, "v%u plain: %s", arch, err);
   struct mali_compile_opts xo = base;
   xo.xfb = 1; xo.xfb_params_off = 64; xo.fau_reserved = 28;
   err[0] = 0;
   int r = mali_compile(&xo, &x, err, sizeof err);
   CHECK(r == 0, "v%u xfb variant: %s", arch, err);
   if (r == 0) {
      CHECK(x.writes_global, "v%u xfb variant has no global stores", arch);
      CHECK(!plain.writes_global, "v%u plain VS must not store globally", arch);
      CHECK(x.attribute_count == 2, "v%u xfb variant reads %u attributes != 2", arch, x.attribute_count);
      CHECK(!x.vs_secondary_enable, "v%u xfb variant must not be IDVS (secondary shader enabled)", arch);
      printf("  v%u: plain VS %u B (+secondary %s), xfb variant %u B, regs %u -> %u\n", arch, plain.bin_size,
             plain.vs_secondary_enable ? "yes" : "no", x.bin_size, plain.work_reg_count, x.work_reg_count);
   }
   mali_compiled_free(&plain); mali_compiled_free(&x);
   free(w);
}

static void
test_plan(void)
{
   struct mali_xfb_buffer b = { .size = 1024, .stride = 32 };
   struct mali_xfb_plan p;
   /* triangle list, 6 vertices = 2 prims, whole buffer */
   CHECK(mali_xfb_plan(MALI_XFB_TRIANGLE_LIST, 6, 1, false, &b, 1, &p), "plan list");
   CHECK(p.prims_needed == 2 && p.prims_written == 2 && p.out_vertices == 6 && p.counter[0] == 6 * 32,
         "list: need %llu wrote %llu verts %llu counter %llu", (unsigned long long)p.prims_needed,
         (unsigned long long)p.prims_written, (unsigned long long)p.out_vertices, (unsigned long long)p.counter[0]);
   /* overflow: room for 4 vertices only -> 1 whole triangle, needed still 2 */
   b.size = 4 * 32;
   CHECK(mali_xfb_plan(MALI_XFB_TRIANGLE_LIST, 6, 1, false, &b, 1, &p) && p.prims_needed == 2 && p.prims_written == 1 &&
         p.counter[0] == 3 * 32, "overflow: written %llu counter %llu", (unsigned long long)p.prims_written, (unsigned long long)p.counter[0]);
   /* pause/resume: second draw continues from the counter, 7 slots total */
   b.size = 7 * 32; b.start = p.counter[0];
   CHECK(mali_xfb_plan(MALI_XFB_TRIANGLE_LIST, 6, 1, false, &b, 1, &p) && p.prims_written == 1 && p.counter[0] == 6 * 32,
         "resume: written %llu counter %llu", (unsigned long long)p.prims_written, (unsigned long long)p.counter[0]);
   /* strip: n-2 triangles, recorded as lists (3 per prim) */
   b.size = 4096; b.start = 0;
   CHECK(mali_xfb_plan(MALI_XFB_TRIANGLE_STRIP, 6, 1, false, &b, 1, &p) && p.prims_needed == 4 && p.out_vertices == 12, "strip");
   CHECK(mali_xfb_input_vertex(MALI_XFB_TRIANGLE_STRIP, 0, 0) == 0 && mali_xfb_input_vertex(MALI_XFB_TRIANGLE_STRIP, 1, 0) == 2 &&
         mali_xfb_input_vertex(MALI_XFB_TRIANGLE_STRIP, 1, 1) == 1 && mali_xfb_input_vertex(MALI_XFB_TRIANGLE_STRIP, 1, 2) == 3, "strip order");
   CHECK(mali_xfb_input_vertex(MALI_XFB_TRIANGLE_FAN, 1, 0) == 0 && mali_xfb_input_vertex(MALI_XFB_TRIANGLE_FAN, 1, 1) == 2 &&
         mali_xfb_input_vertex(MALI_XFB_TRIANGLE_FAN, 1, 2) == 3, "fan order");
   /* instances: out_per_instance is per instance, total multiplies */
   CHECK(mali_xfb_plan(MALI_XFB_TRIANGLE_LIST, 6, 3, false, &b, 1, &p) && p.out_per_instance == 6 && p.out_vertices == 18, "instances");
   /* two buffers: the smaller one limits */
   struct mali_xfb_buffer bb[2] = { { .size = 4096, .stride = 16 }, { .size = 5 * 8, .stride = 8 } };
   CHECK(mali_xfb_plan(MALI_XFB_POINT_LIST, 10, 1, false, bb, 2, &p) && p.prims_written == 5 && p.counter[0] == 80 && p.counter[1] == 40, "two buffers");
   /* refusals: restart, adjacency, unaligned stride, nothing captured */
   CHECK(!mali_xfb_plan(MALI_XFB_TRIANGLE_LIST, 6, 1, true, &b, 1, &p), "restart refused");
   CHECK(!mali_xfb_plan(6, 6, 1, false, &b, 1, &p), "adjacency refused");
   b.stride = 6;
   CHECK(!mali_xfb_plan(MALI_XFB_TRIANGLE_LIST, 6, 1, false, &b, 1, &p), "stride 6 refused");
   b.stride = 0;
   CHECK(!mali_xfb_plan(MALI_XFB_TRIANGLE_LIST, 6, 1, false, &b, 1, &p), "no stride refused");
}

static void
test_data(void)
{
   static const float pos[3][4] = { {0, 0, 0, 1}, {1, 0, 0, 1}, {0, 1, 0, 1} };
   static const float col[3][4] = { {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1} };
   struct vsctx c = { .scale = {2, 2, 1, 1}, .pos = pos, .col = col };
   struct mali_xfb_buffer b = { .size = 3 * 32, .stride = 32 };
   struct mali_xfb_plan p;
   CHECK(mali_xfb_plan(MALI_XFB_TRIANGLE_LIST, 3, 1, false, &b, 1, &p), "plan");
   uint8_t buf[3 * 32 + 32];
   memset(buf, 0xee, sizeof buf);
   uint64_t ran = mali_xfb_run_cpu(MALI_XFB_TRIANGLE_LIST, 3, 1, &p, 32, buf, vs_basic, &c);
   CHECK(ran == 3 && p.prims_written == 1 && p.counter[0] == 96, "ran %llu counter %llu", (unsigned long long)ran, (unsigned long long)p.counter[0]);
   float *f = (float *)buf;
   CHECK(f[8] == 2.f && f[9] == 0.f && f[10] == 0.f && f[11] == 1.f, "vertex 1 position");
   CHECK(f[12] == 0.f + 1.f && f[13] == 1.f && f[15] == 1.f, "vertex 1 varying %g %g %g", f[12], f[13], f[15]);
   CHECK(f[16] == 0.f && f[17] == 2.f, "vertex 2 position");
   CHECK(f[20] == 2.f && f[22] == 1.f, "vertex 2 varying (vid added to x)");
   for (unsigned i = 96; i < sizeof buf; i++) CHECK(buf[i] == 0xee, "wrote past the recorded count at byte %u", i);
}

/* The same capture through the fake backend's memory path: the XFB buffer is a VkDeviceMemory-style BO, the CPU twin writes
 * the record the XFB variant stores, and the bytes and the recorded vertex count are read back through the backend. */
static void
test_fake_backend(void)
{
   const struct mali_backend_ops *b = &mali_backend_fake;
   struct mali_gpu_info gi = { .arch_major = 11, .uapi_major = 1, .uapi_minor = 21, .core_count = 2 };
   struct mali_kdev *k = NULL;
   CHECK(b->device_create(&gi, &k) == 0 && k, "fake: device_create");
   if (!k) return;
   struct mali_bo bo = { .dmabuf_fd = -1 };
   struct mali_xfb_buffer bd = { .size = 3 * 32, .stride = 32 };
   struct mali_xfb_plan p;
   CHECK(mali_xfb_plan(MALI_XFB_TRIANGLE_LIST, 3, 1, false, &bd, 1, &p), "fake: plan");
   CHECK(b->bo_alloc(k, (size_t)bd.size, 0, &bo) == 0 && bo.cpu, "fake: bo_alloc");
   if (!bo.cpu) { b->device_destroy(k); return; }
   static const float pos[3][4] = { {0, 0, 0, 1}, {1, 0, 0, 1}, {0, 1, 0, 1} };
   static const float col[3][4] = { {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1} };
   struct vsctx c = { .scale = {2, 2, 1, 1}, .pos = pos, .col = col };
   memset(bo.cpu, 0xee, bd.size);
   uint64_t ran = mali_xfb_run_cpu(MALI_XFB_TRIANGLE_LIST, 3, 1, &p, 32, bo.cpu, vs_basic, &c);
   /* recorded vertex count: the plan's out_vertices, and the byte position it implies */
   CHECK(ran == p.out_vertices && ran == 3, "fake: recorded %llu vertices, plan %llu", (unsigned long long)ran,
         (unsigned long long)p.out_vertices);
   CHECK(p.counter[0] == ran * bd.stride, "fake: recorded count %llu vertices = %llu bytes", (unsigned long long)ran,
         (unsigned long long)p.counter[0]);
   void *rd = b->va_cpu(k, bo.gpu_va, bd.size);
   CHECK(rd == bo.cpu, "fake: va_cpu(gpu_va) is not the BO mapping");
   const float *f = rd ? (const float *)rd : (const float *)bo.cpu;
   CHECK(f[0] == 0.f && f[1] == 0.f && f[2] == 0.f && f[3] == 1.f, "fake: vertex 0 position");
   CHECK(f[4] == 1.f && f[5] == 0.f && f[6] == 0.f && f[7] == 1.f, "fake: vertex 0 varying");
   CHECK(f[8] == 2.f && f[12] == 1.f && f[16] == 0.f && f[17] == 2.f && f[20] == 2.f, "fake: vertices 1/2");
   const uint8_t *u8 = (const uint8_t *)f;
   for (unsigned i = (unsigned)p.counter[0]; i < bd.size; i++)
      CHECK(u8[i] == 0xee, "fake: wrote past the recorded count at byte %u", i);
   b->bo_free(k, &bo);
   b->device_destroy(k);
   printf("  fake backend: %llu vertices recorded, %llu bytes, counter %llu\n", (unsigned long long)ran,
          (unsigned long long)(ran * bd.stride), (unsigned long long)p.counter[0]);
}

/* Cost model input: the CPU twin per captured vertex, against the VS-only cost of a plain draw. */
static void
bench(void)
{
   enum { N = 3 * 100000 };
   float (*pos)[4] = malloc(N * sizeof *pos), (*col)[4] = malloc(N * sizeof *col);
   for (int i = 0; i < N; i++) for (int k = 0; k < 4; k++) { pos[i][k] = (float)(i % 7); col[i][k] = (float)(i % 5); }
   struct vsctx c = { .scale = {2, 2, 1, 1}, .pos = (const float (*)[4])pos, .col = (const float (*)[4])col };
   struct mali_xfb_buffer b = { .size = (uint64_t)N * 32, .stride = 32 };
   struct mali_xfb_plan p;
   mali_xfb_plan(MALI_XFB_TRIANGLE_LIST, N, 1, false, &b, 1, &p);
   uint8_t *buf = malloc((size_t)N * 32);
   float rec[64], sink = 0;
   struct timespec a, z;
   double best_x = 1e9, best_v = 1e9;
   for (int rep = 0; rep < 7; rep++) {
      clock_gettime(CLOCK_MONOTONIC, &a);
      mali_xfb_run_cpu(MALI_XFB_TRIANGLE_LIST, N, 1, &p, 32, buf, vs_basic, &c);
      clock_gettime(CLOCK_MONOTONIC, &z);
      double x = ((z.tv_sec - a.tv_sec) * 1e9 + (z.tv_nsec - a.tv_nsec)) / N;
      if (x < best_x) best_x = x;
      clock_gettime(CLOCK_MONOTONIC, &a);
      for (int i = 0; i < N; i++) { vs_basic(&c, i, 0, rec); sink += rec[0] + rec[5]; }
      clock_gettime(CLOCK_MONOTONIC, &z);
      double v = ((z.tv_sec - a.tv_sec) * 1e9 + (z.tv_nsec - a.tv_nsec)) / N;
      if (v < best_v) best_v = v;
   }
   printf("  cpu twin: plain VS %.1f ns/vertex, VS + xfb record %.1f ns/vertex (x%.2f) [%g]\n", best_v, best_x, best_x / best_v, sink);
   free(pos); free(col); free(buf);
}

int
main(int argc, char **argv)
{
   if (argc > 1 && argv[1][0] != '-') g_dir = argv[1];
   test_plan();
   test_data();
   test_fake_backend();
   test_compile(11);
   test_compile(13);
   bench();
   printf(g_fail ? "FAILED %d\n" : "ok t_xfb\n", g_fail);
   return g_fail != 0;
}
