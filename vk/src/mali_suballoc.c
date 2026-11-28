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
