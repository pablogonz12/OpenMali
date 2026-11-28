/* SPDX-License-Identifier: MIT */
/* Unit test of the suballocator (vk/src/mali_suballoc.c) over a mock backend whose BOs are malloc'd and whose GPU progress
 * (submitted / retired serials) the test drives by hand. Built plain and with ASan+UBSan (tests/build-t_suballoc.sh).
 * Cases: alignment and zero fill, one pool per flag set, the use-after-free guard (a range freed while a submission is in
 * flight is not handed out until that submission retired), dedicated BOs (large / over-aligned / uncuttable flags) with
 * deferred free, the high-water reclaim, the MALI_BO_SUBALLOC=0 path (one BO per allocation), and four threads at once. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mali_suballoc.h"

static int g_fail;
#define CHECK(c, ...)                                                                                                    \
   do {                                                                                                                  \
      if (!(c)) {                                                                                                        \
         printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #c);                                                             \
         printf(__VA_ARGS__);                                                                                            \
         printf("\n");                                                                                                   \
         g_fail = 1;                                                                                                     \
         return 1;                                                                                                       \
      }                                                                                                                  \
   } while (0)

/* ---- mock backend ---- */
#define MAXBO 4096
static struct { uint64_t va; size_t size; uint32_t flags; void *cpu; int live; } g_bo[MAXBO];
static unsigned g_nbo, g_live, g_allocs, g_frees;
static uint64_t g_next_va = 0x100000000ull;
static pthread_mutex_t g_mlock = PTHREAD_MUTEX_INITIALIZER;

static int
m_alloc(struct mali_kdev *k, size_t size, uint32_t flags, struct mali_bo *out)
{
   size = (size + 4095) & ~(size_t)4095;
   void *p = aligned_alloc(4096, size);
   if (!p)
      return -12;
   memset(p, 0, size);
   pthread_mutex_lock(&g_mlock);
   if (g_nbo == MAXBO) {
      pthread_mutex_unlock(&g_mlock);
      free(p);
      return -12;
   }
   unsigned i = g_nbo++;
   g_bo[i].va = g_next_va;
   g_bo[i].size = size;
   g_bo[i].flags = flags;
   g_bo[i].cpu = p;
   g_bo[i].live = 1;
   g_next_va += size + 4096;
   g_live++;
   g_allocs++;
   *out = (struct mali_bo){.gpu_va = g_bo[i].va, .cpu = p, .size = size, .flags = flags, .dmabuf_fd = -1, .priv = &g_bo[i]};
   pthread_mutex_unlock(&g_mlock);
   return 0;
}

static void
m_free(struct mali_kdev *k, struct mali_bo *bo)
{
   pthread_mutex_lock(&g_mlock);
   for (unsigned i = 0; i < g_nbo; i++)
      if (g_bo[i].live && g_bo[i].cpu == bo->cpu) {
         g_bo[i].live = 0;
         free(g_bo[i].cpu);
         g_live--;
         g_frees++;
         break;
      }
   pthread_mutex_unlock(&g_mlock);
   bo->cpu = NULL;
}

static const struct mali_backend_ops g_be = {.name = "mock", .bo_alloc = m_alloc, .bo_free = m_free};

/* flags of the BO holding va */
static int
bo_flags_of(uint64_t va, uint32_t *flags)
{
   for (unsigned i = 0; i < g_nbo; i++)
      if (g_bo[i].live && va >= g_bo[i].va && va < g_bo[i].va + g_bo[i].size) {
         *flags = g_bo[i].flags;
         return 1;
      }
   return 0;
}

static uint64_t g_sub, g_ret;   /* the "GPU" */
static void
progress(void *u, uint64_t *s, uint64_t *r)
{
   *s = __atomic_load_n(&g_sub, __ATOMIC_RELAXED);
   *r = __atomic_load_n(&g_ret, __ATOMIC_RELAXED);
}

static int
all_zero(const void *p, size_t n)
{
   const uint8_t *b = p;
   for (size_t i = 0; i < n; i++)
      if (b[i])
         return 0;
   return 1;
}

static int
t_basic(void)
{
   struct mali_suballoc *sa = mali_suballoc_create(&g_be, NULL, true, progress, NULL);
   CHECK(sa, "create");
   unsigned a0 = g_allocs;
   void *p[64];
   uint64_t va[64];
   for (unsigned i = 0; i < 64; i++) {
      size_t sz = 1 + i * 37;
      p[i] = mali_suballoc_alloc(sa, MALI_SITE_META, MALI_BO_COH, sz, 64, &va[i]);
      CHECK(p[i] && (va[i] & 63) == 0 && ((uintptr_t)p[i] & 63) == 0, "alloc %u aligned", i);
      CHECK(all_zero(p[i], sz), "fresh range %u zero", i);
      memset(p[i], 0xab, sz);
      size_t off;
      struct mali_bo *bo = mali_suballoc_bo_of(sa, p[i], &off);
      CHECK(bo && bo->gpu_va + off == va[i] && (uint8_t *)bo->cpu + off == (uint8_t *)p[i], "bo_of %u", i);
      uint32_t fl;
      CHECK(bo_flags_of(va[i], &fl) && fl == MALI_BO_COH, "range %u lives in a COH BO", i);
   }
   for (unsigned i = 0; i < 64; i++)
      for (unsigned j = i + 1; j < 64; j++)
         CHECK(va[i] + 1 + i * 37 <= va[j] || va[j] + 1 + j * 37 <= va[i], "ranges %u/%u overlap", i, j);
   CHECK(g_allocs - a0 == 1, "64 small ranges from one chunk: %u BOs", g_allocs - a0);
   /* a second flag set never shares a chunk */
   uint64_t vb;
   void *q = mali_suballoc_alloc(sa, MALI_SITE_VIEW, 0, 32, 64, &vb);
   uint32_t fl;
   CHECK(q && bo_flags_of(vb, &fl) && fl == 0 && g_allocs - a0 == 2, "flags 0 pool: own chunk");
   void *x = mali_suballoc_alloc(sa, MALI_SITE_INTERNAL, MALI_BO_EXEC, 100, 64, &vb);
   CHECK(x && bo_flags_of(vb, &fl) && fl == MALI_BO_EXEC && g_allocs - a0 == 3, "exec pool: own chunk");
   /* reuse after retire is zeroed */
   g_sub = 5;
   g_ret = 5;
   mali_suballoc_free(sa, p[3]);
   uint64_t v2;
   void *r = mali_suballoc_alloc(sa, MALI_SITE_META, MALI_BO_COH, 1 + 3 * 37, 64, &v2);
   CHECK(r == p[3] && v2 == va[3], "retired range reused");
   CHECK(all_zero(r, 1 + 3 * 37), "reused range zeroed");
   /* double free is ignored, not corrupting */
   mali_suballoc_free(sa, p[5]);
   mali_suballoc_free(sa, p[5]);
   void *s1 = mali_suballoc_alloc(sa, MALI_SITE_META, MALI_BO_COH, 1 + 5 * 37, 64, &v2);
   void *s2 = mali_suballoc_alloc(sa, MALI_SITE_META, MALI_BO_COH, 1 + 5 * 37, 64, &v2);
   CHECK(s1 == p[5] && s2 != p[5], "double free hands the range out once");
   mali_suballoc_destroy(sa);
   CHECK(g_live == 0, "destroy frees every chunk (%u live)", g_live);
   printf("ok    basic\n");
   return 0;
}

/* The use-after-free guard: a range freed while a submission is in flight is not handed out until it retired. */
static int
t_inflight(void)
{
   struct mali_suballoc *sa = mali_suballoc_create(&g_be, NULL, true, progress, NULL);
   g_sub = 10;
   g_ret = 9;   /* submission 10 in flight */
   uint64_t va, vb;
   void *a = mali_suballoc_alloc(sa, MALI_SITE_VIEW, 0, 96, 64, &va);
   CHECK(a, "alloc");
   memset(a, 0x5a, 96);
   mali_suballoc_free(sa, a);   /* tag 10 */
   for (int i = 0; i < 100; i++) {
      void *b = mali_suballoc_alloc(sa, MALI_SITE_VIEW, 0, 96, 64, &vb);
      CHECK(b && b != a && vb != va, "iteration %d: range freed during submission 10 handed out before it retired", i);
   }
   CHECK(((uint8_t *)a)[0] == 0x5a, "the in-flight range is untouched (not zeroed for a new owner)");
   g_ret = 10;
   void *c = mali_suballoc_alloc(sa, MALI_SITE_VIEW, 0, 96, 64, &vb);
   CHECK(c == a, "reused once submission 10 retired");
   /* dedicated BOs: the free is deferred the same way */
   g_sub = 20;
   unsigned l0 = g_live;
   void *big = mali_suballoc_alloc(sa, MALI_SITE_DPOOL, 0, 300 << 10, 64, &vb);
   CHECK(big && g_live == l0 + 1, "large range: dedicated BO");
   mali_suballoc_free(sa, big);
   CHECK(g_live == l0 + 1, "dedicated BO kept while submission 20 is in flight");
   g_ret = 20;
   void *tiny = mali_suballoc_alloc(sa, MALI_SITE_VIEW, 0, 8, 64, &vb);   /* any call reaps */
   CHECK(tiny && g_live == l0, "dedicated BO freed after retire");
   /* uncuttable flags and over-alignment are dedicated too */
   void *ev = mali_suballoc_alloc(sa, MALI_SITE_QEV, MALI_BO_EVENT, 64, 64, &vb);
   void *al = mali_suballoc_alloc(sa, MALI_SITE_QEV, 0, 64, 4096, &va);
   CHECK(ev && al && (va & 4095) == 0 && g_live == l0 + 2, "EVENT / 4 KiB-aligned: dedicated");
   mali_suballoc_destroy(sa);
   CHECK(g_live == 0, "destroy");
   printf("ok    in-flight guard\n");
   return 0;
}

/* High water: a pool with more than 4 MiB of free blocks gives empty, retired chunks back; the steady state allocates nothing. */
static int
t_reclaim(void)
{
   struct mali_suballoc *sa = mali_suballoc_create(&g_be, NULL, true, progress, NULL);
   g_sub = g_ret = 0;
   enum { N = 3000 };
   static void *p[N];
   uint64_t va;
   for (unsigned i = 0; i < N; i++)
      CHECK(p[i] = mali_suballoc_alloc(sa, MALI_SITE_DPOOL, 0, 4096, 64, &va), "alloc %u", i);
   unsigned peak;
   mali_suballoc_usage(sa, &peak, NULL, NULL);
   CHECK(peak >= 23, "12 MiB of 4 KiB ranges: %u chunks", peak);
   g_sub = 1;   /* freed during submission 1 */
   for (unsigned i = 0; i < N; i++)
      mali_suballoc_free(sa, p[i]);
   unsigned n1;
   mali_suballoc_usage(sa, &n1, NULL, NULL);
   CHECK(n1 == peak, "nothing reclaimed while the frees are in flight (%u of %u)", n1, peak);
   g_ret = 1;
   mali_suballoc_free(sa, mali_suballoc_alloc(sa, MALI_SITE_DPOOL, 0, 4096, 64, &va));
   unsigned n2;
   mali_suballoc_usage(sa, &n2, NULL, NULL);
   CHECK(n2 < peak && n2 >= 8, "reclaimed down to the high water: %u of %u chunks", n2, peak);
   unsigned a0 = g_allocs;
   for (int r = 0; r < 50; r++) {
      for (unsigned i = 0; i < 500; i++)
         CHECK(p[i] = mali_suballoc_alloc(sa, MALI_SITE_DPOOL, 0, 4096, 64, &va), "steady alloc");
      g_sub++;
      for (unsigned i = 0; i < 500; i++)
         mali_suballoc_free(sa, p[i]);
      g_ret = g_sub;
   }
   CHECK(g_allocs == a0, "steady state: %u BO allocations", g_allocs - a0);
   mali_suballoc_destroy(sa);
   CHECK(g_live == 0, "destroy");
   printf("ok    reclaim + steady state\n");
   return 0;
}

static int
t_off(void)
{
   struct mali_suballoc *sa = mali_suballoc_create(&g_be, NULL, false, progress, NULL);
   unsigned a0 = g_allocs, f0 = g_frees;
   void *p[10];
   uint64_t va;
   for (int i = 0; i < 10; i++) {
      CHECK(p[i] = mali_suballoc_alloc(sa, MALI_SITE_VIEW, 0, 32, 64, &va), "alloc");
      CHECK(all_zero(p[i], 32) && (va & 63) == 0, "zero, aligned");
   }
   CHECK(g_allocs - a0 == 10, "off: one BO per allocation");
   g_sub = 100;
   g_ret = 0;
   for (int i = 0; i < 10; i++)
      mali_suballoc_free_site(sa, MALI_SITE_VIEW, p[i]);
   CHECK(g_frees - f0 == 10, "off: freed at once, as before");
   mali_suballoc_destroy(sa);
   printf("ok    MALI_BO_SUBALLOC=0 path\n");
   return 0;
}

static struct mali_suballoc *g_tsa;
static void *
worker(void *arg)
{
   uintptr_t id = (uintptr_t)arg;
   void *p[64];
   uint64_t va[64];
   for (int r = 0; r < 300; r++) {
      for (int i = 0; i < 64; i++) {
         size_t sz = 16 + (size_t)((i * 131 + r + id) % 3000);
         p[i] = mali_suballoc_alloc(g_tsa, (enum mali_bo_site)(1 + id % 3), id & 1 ? MALI_BO_COH : 0, sz, 64, &va[i]);
         if (!p[i])
            return (void *)1;
         if (!all_zero(p[i], sz))
            return (void *)2;
         memset(p[i], (int)(id + 1), sz);
      }
      for (int i = 0; i < 64; i++) {
         if (((uint8_t *)p[i])[0] != (uint8_t)(id + 1))
            return (void *)3;   /* someone else got the range */
         mali_suballoc_free(g_tsa, p[i]);
      }
      __atomic_add_fetch(&g_sub, 1, __ATOMIC_RELAXED);
      __atomic_store_n(&g_ret, __atomic_load_n(&g_sub, __ATOMIC_RELAXED) - 1, __ATOMIC_RELAXED);
   }
   return NULL;
}

static int
t_threads(void)
{
   g_sub = g_ret = 0;
   g_tsa = mali_suballoc_create(&g_be, NULL, true, progress, NULL);
   pthread_t t[4];
   for (uintptr_t i = 0; i < 4; i++)
      pthread_create(&t[i], NULL, worker, (void *)i);
   int bad = 0;
   for (int i = 0; i < 4; i++) {
      void *r;
      pthread_join(t[i], &r);
      if (r)
         bad = (int)(uintptr_t)r;
   }
   CHECK(!bad, "worker failed (%d)", bad);
   mali_suballoc_destroy(g_tsa);
   CHECK(g_live == 0, "destroy");
   printf("ok    4 threads\n");
   return 0;
}

int
main(void)
{
   if (t_basic() || t_inflight() || t_reclaim() || t_off() || t_threads())
      return 1;
   struct mali_bo_stats s;
   mali_bo_stats_get(&s);
   printf("ok    t_suballoc (sub allocs %llu reused %llu reclaimed %llu)\n", (unsigned long long)s.sub_allocs,
          (unsigned long long)s.sub_reused, (unsigned long long)s.sub_reclaimed);
   return g_fail;
}
