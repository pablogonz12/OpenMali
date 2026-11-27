/* SPDX-License-Identifier: MIT */
/* MALI_RP_STATS: per-render-pass contents of the colour attachments. See mali_rpstats.h for the policy and the line. */
#include "mali_rpstats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mali_wait_debug.h"

static const char *const kind_name[] = {"render", "clear", "copy", "blit", "resolve"};

static struct mali_rpstats *g_current;

void
mali_rpstats_set_current(struct mali_rpstats *s)
{
   g_current = s;
}

struct mali_rpstats *
mali_rpstats_get(void)
{
   return g_current;
}

/* This process asked for it: MALI_STATS_COMM is a substring of /proc/self/comm, unmatched = every process. */
static bool
comm_allows(void)
{
   const char *want = getenv("MALI_STATS_COMM");
   if (!want || !want[0])
      return true;
   char comm[32] = "";
   FILE *f = fopen("/proc/self/comm", "r");
   if (!f)
      return true;
   if (!fgets(comm, sizeof comm, f))
      comm[0] = '\0';
   fclose(f);
   size_t n = strlen(comm);
   while (n && (comm[n - 1] == '\n' || comm[n - 1] == ' '))
      comm[--n] = '\0';
   return strstr(comm, want) != NULL;
}

void
mali_rpstats_init(struct mali_rpstats *s, bool (*lock_live)(const void *), void (*unlock)(void))
{
   memset(s, 0, sizeof *s);
   pthread_mutex_init(&s->lock, NULL);
   const char *e = getenv("MALI_RP_STATS");
   /* The lines go to the MALI_WAIT_DBG sink (one file per process); without one there is nowhere to put them. */
   s->on = e && e[0] && strcmp(e, "0") && mali_wait_debug() && comm_allows();
   const char *m = getenv("MALI_RP_STATS_MAX");
   s->max_lines = m && m[0] ? (uint32_t)strtoul(m, NULL, 10) : MALI_RP_DEFAULT_MAX;
   if (!s->max_lines)
      s->max_lines = MALI_RP_DEFAULT_MAX;
   s->lock_live = lock_live;
   s->unlock = unlock;
}

void
mali_rpstats_finish(struct mali_rpstats *s)
{
   if (s->on && (s->nlogged || s->ndropped))
      mali_wait_line("rp stats total logged=%u dropped=%u pending=%u", s->nlogged, s->ndropped, s->npending);
   s->on = 0;
   pthread_mutex_destroy(&s->lock);
}

bool
mali_rpstats_on(struct mali_rpstats *s)
{
   return s->on != 0;
}

void
mali_rpstats_pass(struct mali_rpstats *s, const struct mali_rp_pass *p)
{
   if (!s->on)
      return;
   /* Copies the whole record (attachments included): the command buffer may be reset before the submit. */
   for (unsigned i = 0; i < p->natt && i < MALI_RP_MAX_ATT; i++)
      if (p->att[i].bo && !p->att[i].bo->cpu) {
         ((struct mali_rp_pass *)p)->att[i].bo = NULL;   /* not mappable: sampled as "no attachment" */
         ((struct mali_rp_pass *)p)->att[i].owner = NULL;
      }
   pthread_mutex_lock(&s->lock);
   if (s->nlogged + s->npending >= s->max_lines && s->npending < MALI_RP_MAX_PASS) {
      s->ndropped++;
      pthread_mutex_unlock(&s->lock);
      return;
   }
   if (s->npending < MALI_RP_MAX_PASS)
      s->pass[s->npending++] = *p;
   else
      s->ndropped++;
   pthread_mutex_unlock(&s->lock);
}

/* nonzero / mean / alpha_nonzero of one BO through its CPU mapping. Returns false when there is nothing to read. */
static bool
sample_bo(struct mali_rpstats *s, const struct mali_backend_ops *be, struct mali_kdev *kdev, struct mali_bo *bo,
          const void *owner, uint32_t bpp, double *nonzero, double *mean, double *alpha)
{
   if (!bo || !bo->cpu || !bo->size)
      return false;
   if (s->lock_live && !s->lock_live(owner))
      return false;   /* the memory is gone: nothing to sample */
   if (!bo->cpu || !bo->size) {
      if (s->unlock)
         s->unlock();
      return false;
   }
   /* The GPU wrote this BO: the same device->CPU sync the driver does before any CPU read. */
   if (be) {
      if (be->bo_host_touch)
         be->bo_host_touch(kdev, bo);
      if (be->bo_cache)
         be->bo_cache(kdev, bo, 0, bo->size, true);
   }
   /* Every 16th PIXEL, all of its bytes: one byte per 64 was blind to a pixel whose blue/alpha is set but whose
    * first byte is 0 (the X4 surface read nonzero=0.0 alpha_nonzero=100 on a phone that paints). */
   const uint8_t *p = (const uint8_t *)bo->cpu;
   unsigned long long nz = 0, sum = 0, an = 0, anz = 0;
   size_t n = 0;
   const size_t px = bpp ? bpp : 1;
   for (size_t i = 0; i + px <= bo->size; i += 16 * px) {
      for (size_t c = 0; c < px && c < 4; c++) {
         sum += p[i + c];
         nz += p[i + c] != 0;
         n++;
      }
      if (px == 4) {
         anz += p[i + 3] != 0;
         an++;
      }
   }
   if (s->unlock)
      s->unlock();
   *nonzero = n ? 100.0 * (double)nz / (double)n : 0.0;
   *mean = n ? (double)sum / (double)n : 0.0;
   *alpha = an ? 100.0 * (double)anz / (double)an : -1.0;
   return true;
}

void
mali_rpstats_flush(struct mali_rpstats *s, const struct mali_backend_ops *be, struct mali_kdev *kdev)
{
   if (!s->on)
      return;
   pthread_mutex_lock(&s->lock);
   struct mali_rp_pass local[MALI_RP_MAX_PASS];
   const unsigned n = s->npending;
   memcpy(local, s->pass, sizeof(local[0]) * n);
   s->npending = 0;
   pthread_mutex_unlock(&s->lock);

   for (unsigned i = 0; i < n; i++) {
      const struct mali_rp_pass *p = &local[i];
      char nz[96] = "-", mn[96] = "-", al[96] = "-";
      size_t o1 = 0, o2 = 0, o3 = 0;
      for (unsigned a = 0; a < p->natt && a < MALI_RP_MAX_ATT; a++) {
         double rn = 0, rm = 0, ra = 0;
         if (!sample_bo(s, be, kdev, p->att[a].bo, p->att[a].owner, p->att[a].bpp, &rn, &rm, &ra))
            continue;
         o1 += (size_t)snprintf(nz + o1, sizeof nz - o1, "%s%.1f", o1 ? "," : "", rn);
         o2 += (size_t)snprintf(mn + o2, sizeof mn - o2, "%s%.2f", o2 ? "," : "", rm);
         if (ra >= 0)
            o3 += (size_t)snprintf(al + o3, sizeof al - o3, "%s%.1f", o3 ? "," : "", ra);
      }
      const char *rk = "none";
      if (p->resolve_mode == 1)
         rk = "1x";
      else if (p->resolve_mode == 2)
         rk = "msaa";
      char pipes[512] = "";
      size_t op = 0;
      for (unsigned q = 0; q < p->npipes && q < MALI_RP_MAX_PIPE; q++) {
         const struct mali_rp_pipe *pi = &p->pipe[q];
         op += (size_t)snprintf(pipes + op, sizeof pipes - op,
                                " p%u=%08x:%08x blend=%x wmask=%x depth=%u/%u/%u samples=%u prim=%u tls=%u regs=%u "
                                "tex=%u ubo=%u",
                                q, pi->vsh, pi->fsh, pi->blend, pi->wmask, pi->dtest, pi->dwrite, pi->dfunc,
                                pi->samples, pi->prim, pi->tls, pi->regs, pi->tex, pi->ubo);
      }
      pthread_mutex_lock(&s->lock);
      s->nlogged++;
      pthread_mutex_unlock(&s->lock);
      mali_wait_line("rp n=%u kind=%s extent=%ux%u layers=%u samples=%u fmt=%u load=%u store=%u resolve=%s,%u "
                     "draws=%u nonzero=%s mean=%s alpha_nonzero=%s pipes=%u%s descs=sampled:%u ubos:%u imports:%u "
                     "sets:%u writes:%u",
                     i + 1, kind_name[p->kind & 7], p->w, p->h, p->layers, p->samples, p->fmt, p->load, p->store,
                     rk, p->resolve_fmt, p->draws, nz, mn, al, p->npipes, pipes, p->desc_sampled, p->desc_ubo,
                     p->desc_imports, p->desc_sets, p->desc_writes);
   }
}
