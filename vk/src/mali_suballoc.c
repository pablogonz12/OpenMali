/* SPDX-License-Identifier: MIT */
/* BO statistics by allocation site and the per-device suballocator for small driver-internal GPU memory (mali_suballoc.h). */
#include "mali_suballoc.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- statistics ---- */
static struct mali_bo_stats g_st;   /* updated with __atomic builtins */
static _Thread_local unsigned char t_site;   /* MALI_SITE_INTERNAL = 0 */

static const char *const g_site_name[MALI_SITE_N] = {
   [MALI_SITE_INTERNAL] = "internal", [MALI_SITE_META] = "meta", [MALI_SITE_VIEW] = "view", [MALI_SITE_DPOOL] = "dpool",
   [MALI_SITE_RES] = "res", [MALI_SITE_QEV] = "qev", [MALI_SITE_USER] = "user", [MALI_SITE_CHUNK] = "chunk",
};

const char *
mali_bo_site_name(enum mali_bo_site s)
{
   return s < MALI_SITE_N ? g_site_name[s] : "?";
}

#define ADD(f, v) __atomic_add_fetch(&(f), (v), __ATOMIC_RELAXED)
#define LD(f) __atomic_load_n(&(f), __ATOMIC_RELAXED)

void
mali_bo_stats_get(struct mali_bo_stats *out)
{
   uint64_t *d = (uint64_t *)out, *s = (uint64_t *)&g_st;
   for (size_t i = 0; i < sizeof *out / sizeof(uint64_t); i++)
      d[i] = LD(s[i]);
}

static int g_stats_env = -1;   /* MALI_BO_STATS, read once */

static bool
stats_on(void)
{
   int e = __atomic_load_n(&g_stats_env, __ATOMIC_RELAXED);
   if (e < 0) {
      const char *v = getenv("MALI_BO_STATS");
      e = v && v[0] && v[0] != '0';
      __atomic_store_n(&g_stats_env, e, __ATOMIC_RELAXED);
   }
   return e;
}

void
mali_bo_stats_note(bool alloc, size_t size, uint64_t ns)
{
   unsigned s = t_site < MALI_SITE_N ? t_site : MALI_SITE_INTERNAL;
   if (alloc) {
      ADD(g_st.allocs[s], 1);
      ADD(g_st.alloc_bytes[s], size);
      ADD(g_st.alloc_ns, ns);
   } else {
      ADD(g_st.frees[s], 1);
      ADD(g_st.free_bytes[s], size);
      ADD(g_st.free_ns, ns);
   }
   mali_bo_stats_tick();
}

void
mali_bo_stats_note_site(enum mali_bo_site site, bool alloc, size_t size, uint64_t ns)
{
   unsigned char old = t_site;
   t_site = (unsigned char)site;
   mali_bo_stats_note(alloc, size, ns);
   t_site = old;
}

/* One line per second: per site "allocs/frees in this second, live BOs, live KiB", then the time spent in the backend and
 * the suballocator's range traffic. Printed by whichever thread allocates or suballocates first after the second ends. */
void
mali_bo_stats_tick(void)
{
   if (!stats_on())
      return;
   static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
   static struct mali_bo_stats last;
   static uint64_t last_ns;
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
   uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
   if (now - __atomic_load_n(&last_ns, __ATOMIC_RELAXED) < 1000000000ull || pthread_mutex_trylock(&lock))
      return;
   if (now - last_ns >= 1000000000ull) {
      struct mali_bo_stats c;
      mali_bo_stats_get(&c);
      char line[1024];
      int n = snprintf(line, sizeof line, "mali_bo_stats:");
      for (unsigned i = 0; i < MALI_SITE_N && n < (int)sizeof line; i++)
         n += snprintf(line + n, sizeof line - n, " %s %llu/%llu live %lld %lldK", g_site_name[i],
                       (unsigned long long)(c.allocs[i] - last.allocs[i]), (unsigned long long)(c.frees[i] - last.frees[i]),
                       (long long)(c.allocs[i] - c.frees[i]), (long long)(c.alloc_bytes[i] - c.free_bytes[i]) >> 10);
      if (n < (int)sizeof line)
         n += snprintf(line + n, sizeof line - n, " | alloc %lluus free %lluus | sub %llu/%llu reused %llu fresh %llu ded %llu reclaimed %llu\n",
                       (unsigned long long)(c.alloc_ns - last.alloc_ns) / 1000, (unsigned long long)(c.free_ns - last.free_ns) / 1000,
                       (unsigned long long)(c.sub_allocs - last.sub_allocs), (unsigned long long)(c.sub_frees - last.sub_frees),
                       (unsigned long long)(c.sub_reused - last.sub_reused), (unsigned long long)(c.sub_fresh - last.sub_fresh),
                       (unsigned long long)(c.sub_dedicated - last.sub_dedicated),
                       (unsigned long long)(c.sub_reclaimed - last.sub_reclaimed));
      fputs(line, stderr);
      last = c;
      __atomic_store_n(&last_ns, now, __ATOMIC_RELAXED);
   }
   pthread_mutex_unlock(&lock);
}

int
mali_bo_alloc_site(const struct mali_backend_ops *be, struct mali_kdev *k, enum mali_bo_site site, size_t size,
                   uint32_t flags, struct mali_bo *out)
{
   unsigned char old = t_site;
   t_site = (unsigned char)site;
   int r = be->bo_alloc(k, size, flags, out);
   t_site = old;
   return r;
}

void
mali_bo_free_site(const struct mali_backend_ops *be, struct mali_kdev *k, enum mali_bo_site site, struct mali_bo *bo)
{
   unsigned char old = t_site;
   t_site = (unsigned char)site;
   be->bo_free(k, bo);
   t_site = old;
}

/* ---- suballocator ----
 * One pool per distinct bo_alloc flag set (never mixed: a range has exactly the GPU access, coherency, executable and
 * zero-fill properties of a BO of its own with those flags). A pool owns chunk BOs of CHUNK_BYTES, carved front to back
 * (bump) into power-of-two blocks of 64 B .. MAX_BLOCK; a request rounds up to its block class. A freed block goes to
 * its class's FIFO with tag = the device's submission serial at the time of the free, and is handed out again only once
 * the GPU has retired that serial: a submission still in flight when the range was freed can never see it reused
 * (vk/MEMORY.md "Suballocated driver memory"). Requests over MAX_BLOCK (or aligned beyond 64 B) get a dedicated BO
 * whose free is deferred the same way. Chunks go back to the kernel at destroy, or when a pool holds more than
 * HIGH_WATER bytes of free blocks and one of its chunks is empty and fully retired.
 *
 * Off (MALI_BO_SUBALLOC=0): one BO per allocation, 64-byte header in front of the range (the pre-suballocator code). */
#define GRAN 64u
#define NCLS 11u                        /* 64 B .. 64 KiB */
#define MAX_BLOCK (GRAN << (NCLS - 1))
#define CHUNK_BYTES (512u << 10)
#define HIGH_WATER (4u << 20)
#define LEGACY_HDR 64u
_Static_assert(sizeof(struct mali_bo) <= LEGACY_HDR, "legacy header");

struct sa_chunk {
   struct mali_bo bo;
   unsigned pool;
   unsigned live;          /* blocks handed out */
   uint64_t max_tag;       /* highest free tag of a block of this chunk */
   uint8_t cls[CHUNK_BYTES / GRAN];   /* per granule: 1 + class of a live block starting here, else 0 */
};

struct sa_ent { struct sa_chunk *c; uint32_t off; uint64_t tag; };
struct sa_fifo { struct sa_ent *e; unsigned head, n, cap; };   /* ring */

struct sa_pool {
   uint32_t flags;
   enum mali_bo_site site;     /* first user (chunk allocations are tagged CHUNK; this is only for the debug print) */
   struct sa_chunk *cur;       /* bump chunk */
   size_t bump;
   struct sa_fifo fl[NCLS];
   size_t free_bytes;
   unsigned nchunks;
   bool reclaim;               /* an empty chunk may be reclaimable */
};

struct sa_big { void *cpu; struct mali_bo bo; uint64_t tag; bool freed; };

struct mali_suballoc {
   const struct mali_backend_ops *be;
   struct mali_kdev *k;
   bool on;
   mali_suballoc_progress_fn progress;
   void *user;
   pthread_mutex_t lock;
   struct sa_pool *pools;
   unsigned npools;
   struct sa_chunk **chunks;   /* sorted by cpu address */
   unsigned nchunks, capchunks;
   struct sa_big *big;         /* dedicated BOs (live and deferred frees), unsorted */
   unsigned nbig, capbig, nbig_freed;
   size_t live_bytes;
};

bool
mali_suballoc_env_on(void)
{
   const char *e = getenv("MALI_BO_SUBALLOC");
   return !(e && e[0] == '0');
}

bool
mali_suballoc_is_on(const struct mali_suballoc *sa)
{
   return sa && sa->on;
}

struct mali_suballoc *
mali_suballoc_create(const struct mali_backend_ops *be, struct mali_kdev *k, bool on, mali_suballoc_progress_fn progress, void *user)
{
   struct mali_suballoc *sa = calloc(1, sizeof *sa);
   if (!sa)
      return NULL;
   sa->be = be;
   sa->k = k;
   sa->on = on;
   sa->progress = progress;
   sa->user = user;
   pthread_mutex_init(&sa->lock, NULL);
   return sa;
}

static uint64_t
retired_now(struct mali_suballoc *sa, uint64_t *submitted)
{
   uint64_t s = 0, r = UINT64_MAX;
   if (sa->progress)
      sa->progress(sa->user, &s, &r);
   if (submitted)
      *submitted = s;
   return r;
}

static void
note_sub(struct mali_suballoc *sa, uint64_t va, size_t size, uint32_t flags, int live)
{
   if (sa->be->bo_note_sub)
      sa->be->bo_note_sub(sa->k, va, size, flags, live);
}

static unsigned
cls_of(size_t size)
{
   unsigned c = 0;
   while ((GRAN << c) < size)
      c++;
   return c;
}

static struct sa_chunk *
chunk_find(struct mali_suballoc *sa, const void *cpu)
{
   unsigned lo = 0, hi = sa->nchunks;
   const uint8_t *p = cpu;
   while (lo < hi) {
      unsigned mid = (lo + hi) / 2;
      struct sa_chunk *c = sa->chunks[mid];
      const uint8_t *b = c->bo.cpu;
      if (p < b)
         hi = mid;
      else if (p >= b + CHUNK_BYTES)
         lo = mid + 1;
      else
         return c;
   }
   return NULL;
}

static bool
chunk_insert(struct mali_suballoc *sa, struct sa_chunk *c)
{
   if (sa->nchunks == sa->capchunks) {
      unsigned nc = sa->capchunks ? sa->capchunks * 2 : 16;
      struct sa_chunk **n = realloc(sa->chunks, nc * sizeof *n);
      if (!n)
         return false;
      sa->chunks = n;
      sa->capchunks = nc;
   }
   unsigned i = sa->nchunks;
   while (i && (const uint8_t *)sa->chunks[i - 1]->bo.cpu > (const uint8_t *)c->bo.cpu) {
      sa->chunks[i] = sa->chunks[i - 1];
      i--;
   }
   sa->chunks[i] = c;
   sa->nchunks++;
   return true;
}

static void
chunk_remove(struct mali_suballoc *sa, struct sa_chunk *c)
{
   for (unsigned i = 0; i < sa->nchunks; i++)
      if (sa->chunks[i] == c) {
         memmove(&sa->chunks[i], &sa->chunks[i + 1], (sa->nchunks - i - 1) * sizeof sa->chunks[0]);
         sa->nchunks--;
         return;
      }
}

static struct sa_pool *
pool_get(struct mali_suballoc *sa, uint32_t flags, enum mali_bo_site site, unsigned *idx)
{
   for (unsigned i = 0; i < sa->npools; i++)
      if (sa->pools[i].flags == flags) {
         *idx = i;
         return &sa->pools[i];
      }
   struct sa_pool *n = realloc(sa->pools, (sa->npools + 1) * sizeof *n);
   if (!n)
      return NULL;
   sa->pools = n;
   memset(&n[sa->npools], 0, sizeof n[0]);
   n[sa->npools].flags = flags;
   n[sa->npools].site = site;
   *idx = sa->npools;
   return &n[sa->npools++];
}

static bool
fifo_push(struct sa_fifo *f, struct sa_ent e)
{
   if (f->n == f->cap) {
      unsigned nc = f->cap ? f->cap * 2 : 64;
      struct sa_ent *n = malloc(nc * sizeof *n);
      if (!n)
         return false;
      for (unsigned i = 0; i < f->n; i++)
         n[i] = f->e[(f->head + i) % f->cap];
      free(f->e);
      f->e = n;
      f->cap = nc;
      f->head = 0;
   }
   f->e[(f->head + f->n) % f->cap] = e;
   f->n++;
   return true;
}

static struct sa_chunk *
chunk_new(struct mali_suballoc *sa, struct sa_pool *p, unsigned pidx)
{
   struct sa_chunk *c = calloc(1, sizeof *c);
   if (!c)
      return NULL;
   if (mali_bo_alloc_site(sa->be, sa->k, MALI_SITE_CHUNK, CHUNK_BYTES, p->flags, &c->bo) != 0) {
      free(c);
      return NULL;
   }
   if (c->bo.size < CHUNK_BYTES || !chunk_insert(sa, c)) {
      mali_bo_free_site(sa->be, sa->k, MALI_SITE_CHUNK, &c->bo);
      free(c);
      return NULL;
   }
   c->pool = pidx;
   p->nchunks++;
   note_sub(sa, c->bo.gpu_va, 0, p->flags, 2);   /* fake dumps: the chunk is a host of ranges, not a BO of its own */
   return c;
}

static void
chunk_release(struct mali_suballoc *sa, struct sa_chunk *c)
{
   chunk_remove(sa, c);
   mali_bo_free_site(sa->be, sa->k, MALI_SITE_CHUNK, &c->bo);
   free(c);
}

/* Drops empty, fully retired chunks of a pool that holds more than HIGH_WATER bytes of free blocks (not its bump chunk). */
static void
pool_reclaim(struct mali_suballoc *sa, struct sa_pool *p, uint64_t retired)
{
   if (!p->reclaim || p->free_bytes <= HIGH_WATER)
      return;
   p->reclaim = false;
   for (unsigned i = 0; i < sa->nchunks && p->free_bytes > HIGH_WATER;) {
      struct sa_chunk *c = sa->chunks[i];
      if (&sa->pools[c->pool] != p || c->live || c == p->cur) {
         i++;
         continue;
      }
      if (c->max_tag > retired) {
         p->reclaim = true;   /* try again once the GPU caught up */
         i++;
         continue;
      }
      for (unsigned k = 0; k < NCLS; k++) {   /* take its blocks out of the FIFOs, keeping their order */
         struct sa_fifo *f = &p->fl[k];
         unsigned w = 0;
         for (unsigned j = 0; j < f->n; j++) {
            struct sa_ent e = f->e[(f->head + j) % f->cap];
            if (e.c == c)
               p->free_bytes -= GRAN << k;
            else
               f->e[(f->head + w++) % f->cap] = e;
         }
         f->n = w;
      }
      p->nchunks--;
      chunk_release(sa, c);   /* removes chunks[i]: do not advance */
      ADD(g_st.sub_reclaimed, 1);
   }
}

static void
big_reap(struct mali_suballoc *sa, uint64_t retired)
{
   if (!sa->nbig_freed)
      return;
   unsigned w = 0;
   for (unsigned i = 0; i < sa->nbig; i++) {
      struct sa_big *b = &sa->big[i];
      if (b->freed && b->tag <= retired) {
         mali_bo_free_site(sa->be, sa->k, MALI_SITE_CHUNK, &b->bo);
         sa->nbig_freed--;
      } else
         sa->big[w++] = *b;
   }
   sa->nbig = w;
}

static void *
legacy_alloc(struct mali_suballoc *sa, enum mali_bo_site site, uint32_t flags, size_t size, uint64_t *va)
{
   struct mali_bo bo;
   if (mali_bo_alloc_site(sa->be, sa->k, site, size + LEGACY_HDR, flags, &bo) != 0)
      return NULL;
   memcpy(bo.cpu, &bo, sizeof bo);
   *va = bo.gpu_va + LEGACY_HDR;
   note_sub(sa, bo.gpu_va, 0, flags, 2);
   note_sub(sa, *va, size, flags, 1);
   return (char *)bo.cpu + LEGACY_HDR;
}

static void
legacy_free(struct mali_suballoc *sa, enum mali_bo_site site, void *cpu)
{
   struct mali_bo bo;
   memcpy(&bo, (char *)cpu - LEGACY_HDR, sizeof bo);
   note_sub(sa, bo.gpu_va + LEGACY_HDR, 0, 0, 0);
   mali_bo_free_site(sa->be, sa->k, site, &bo);
}

/* The site of a legacy range is needed at free time for the counters; on keeps no site per range (none is needed). */
static _Thread_local enum mali_bo_site t_free_site;

void *
mali_suballoc_alloc(struct mali_suballoc *sa, enum mali_bo_site site, uint32_t flags, size_t size, size_t align, uint64_t *va)
{
   if (!sa)
      return NULL;
   if (!size)
      size = 1;
   if (!sa->on)
      return legacy_alloc(sa, site, flags, size, va);
   /* Memory whose CPU side is not plain write-combined + always synced (cached, exported, lazy, event) keeps one BO per
    * allocation: its per-BO sync state (cpu_dirty, host_lazy, dma-buf) does not split into ranges. */
   const bool chunkable = !(flags & (MALI_BO_CACHED | MALI_BO_EXPORT | MALI_BO_EVENT | MALI_BO_HOSTLAZY));
   void *ret = NULL;
   pthread_mutex_lock(&sa->lock);
   uint64_t retired = retired_now(sa, NULL);
   big_reap(sa, retired);
   if (!chunkable || size > MAX_BLOCK || align > GRAN) {
      if (sa->nbig == sa->capbig) {
         unsigned nc = sa->capbig ? sa->capbig * 2 : 16;
         struct sa_big *n = realloc(sa->big, nc * sizeof *n);
         if (!n)
            goto out;
         sa->big = n;
         sa->capbig = nc;
      }
      struct sa_big *b = &sa->big[sa->nbig];
      memset(b, 0, sizeof *b);
      if (mali_bo_alloc_site(sa->be, sa->k, site, size, flags, &b->bo) != 0)
         goto out;
      b->cpu = b->bo.cpu;
      sa->nbig++;
      *va = b->bo.gpu_va;
      ret = b->cpu;
      note_sub(sa, b->bo.gpu_va, 0, flags, 2);
      note_sub(sa, *va, size, flags, 1);
      ADD(g_st.sub_dedicated, 1);
      goto out;
   }
   unsigned pidx;
   struct sa_pool *p = pool_get(sa, flags, site, &pidx);
   if (!p)
      goto out;
   const unsigned k = cls_of(size);
   const size_t bsz = GRAN << k;
   struct sa_fifo *f = &p->fl[k];
   struct sa_chunk *c = NULL;
   uint32_t off = 0;
   if (f->n && f->e[f->head].tag <= retired) {   /* FIFO: tags never decrease, so the head is the oldest */
      struct sa_ent e = f->e[f->head];
      f->head = (f->head + 1) % f->cap;
      f->n--;
      p->free_bytes -= bsz;
      c = e.c;
      off = e.off;
      memset((uint8_t *)c->bo.cpu + off, 0, bsz);   /* zero-filled like a fresh BO */
      ADD(g_st.sub_reused, 1);
   } else {
      if (!p->cur || p->bump + bsz > CHUNK_BYTES) {
         /* the tail of the old bump chunk goes to the FIFOs as free blocks (never written, so already zero, tag 0) */
         if (p->cur)
            for (unsigned j = NCLS; j-- > 0;)
               while (p->bump + (GRAN << j) <= CHUNK_BYTES) {
                  if (!fifo_push(&p->fl[j], (struct sa_ent){p->cur, (uint32_t)p->bump, 0}))
                     break;
                  p->free_bytes += GRAN << j;
                  p->bump += GRAN << j;
               }
         struct sa_chunk *nc = chunk_new(sa, p, pidx);
         if (!nc)
            goto out;
         p = &sa->pools[pidx];
         p->cur = nc;
         p->bump = 0;
         f = &p->fl[k];
      }
      c = p->cur;
      off = (uint32_t)p->bump;
      p->bump += bsz;
      ADD(g_st.sub_fresh, 1);
   }
   c->cls[off / GRAN] = (uint8_t)(k + 1);
   c->live++;
   sa->live_bytes += bsz;
   *va = c->bo.gpu_va + off;
   ret = (uint8_t *)c->bo.cpu + off;
   note_sub(sa, *va, size, flags, 1);
out:
   if (ret)
      ADD(g_st.sub_allocs, 1);
   pthread_mutex_unlock(&sa->lock);
   mali_bo_stats_tick();
   return ret;
}

void
mali_suballoc_free(struct mali_suballoc *sa, void *cpu)
{
   if (!sa || !cpu)
      return;
   if (!sa->on) {
      legacy_free(sa, t_free_site, cpu);
      return;
   }
   pthread_mutex_lock(&sa->lock);
   uint64_t submitted;
   uint64_t retired = retired_now(sa, &submitted);
   struct sa_chunk *c = chunk_find(sa, cpu);
   if (c) {
      uint32_t off = (uint32_t)((const uint8_t *)cpu - (const uint8_t *)c->bo.cpu);
      unsigned k = off % GRAN ? 0 : c->cls[off / GRAN];
      if (!k) {
         fprintf(stderr, "mali_suballoc: free of %p: not a live range (double free?) - ignored\n", cpu);
         goto out;
      }
      k--;
      struct sa_pool *p = &sa->pools[c->pool];
      if (!fifo_push(&p->fl[k], (struct sa_ent){c, off, submitted}))
         goto out;   /* out of host memory: the block leaks inside its chunk, never reused early */
      c->cls[off / GRAN] = 0;
      c->live--;
      if (submitted > c->max_tag)
         c->max_tag = submitted;
      p->free_bytes += GRAN << k;
      sa->live_bytes -= GRAN << k;
      note_sub(sa, c->bo.gpu_va + off, 0, 0, 0);
      ADD(g_st.sub_frees, 1);
      if (!c->live && c != p->cur)
         p->reclaim = true;
      pool_reclaim(sa, p, retired);
      goto out;
   }
   for (unsigned i = 0; i < sa->nbig; i++)
      if (sa->big[i].cpu == cpu && !sa->big[i].freed) {
         sa->big[i].freed = true;
         sa->big[i].tag = submitted;
         sa->nbig_freed++;
         note_sub(sa, sa->big[i].bo.gpu_va, 0, 0, 0);
         ADD(g_st.sub_frees, 1);
         big_reap(sa, retired);
         goto out;
      }
   fprintf(stderr, "mali_suballoc: free of %p: unknown range - ignored\n", cpu);
out:
   pthread_mutex_unlock(&sa->lock);
}

/* The legacy path needs the site again at free time (the counters are per site); the converted callers pass it here. */
void
mali_suballoc_free_site(struct mali_suballoc *sa, enum mali_bo_site site, void *cpu)
{
   t_free_site = site;
   mali_suballoc_free(sa, cpu);
   t_free_site = MALI_SITE_INTERNAL;
}

struct mali_bo *
mali_suballoc_bo_of(struct mali_suballoc *sa, const void *cpu, size_t *off)
{
   if (!sa || !cpu || !sa->on)
      return NULL;   /* legacy: the BO is in the header in front of the range, but callers only need it when on */
   struct mali_bo *bo = NULL;
   pthread_mutex_lock(&sa->lock);
   struct sa_chunk *c = chunk_find(sa, cpu);
   if (c) {
      bo = &c->bo;
      if (off)
         *off = (size_t)((const uint8_t *)cpu - (const uint8_t *)c->bo.cpu);
   } else
      for (unsigned i = 0; i < sa->nbig; i++)
         if (sa->big[i].cpu == cpu) {
            bo = &sa->big[i].bo;
            if (off)
               *off = 0;
            break;
         }
   pthread_mutex_unlock(&sa->lock);
   return bo;
}

void
mali_suballoc_usage(struct mali_suballoc *sa, unsigned *chunks, size_t *chunk_bytes, size_t *live_bytes)
{
   pthread_mutex_lock(&sa->lock);
   if (chunks)
      *chunks = sa->nchunks;
   if (chunk_bytes)
      *chunk_bytes = (size_t)sa->nchunks * CHUNK_BYTES;
   if (live_bytes)
      *live_bytes = sa->live_bytes;
   pthread_mutex_unlock(&sa->lock);
}

void
mali_suballoc_destroy(struct mali_suballoc *sa)
{
   if (!sa)
      return;
   for (unsigned i = 0; i < sa->nbig; i++)
      mali_bo_free_site(sa->be, sa->k, MALI_SITE_CHUNK, &sa->big[i].bo);
   for (unsigned i = 0; i < sa->nchunks; i++) {
      mali_bo_free_site(sa->be, sa->k, MALI_SITE_CHUNK, &sa->chunks[i]->bo);
      free(sa->chunks[i]);
   }
   for (unsigned i = 0; i < sa->npools; i++)
      for (unsigned k = 0; k < NCLS; k++)
         free(sa->pools[i].fl[k].e);
   free(sa->pools);
   free(sa->chunks);
   free(sa->big);
   pthread_mutex_destroy(&sa->lock);
   free(sa);
}
