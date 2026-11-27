/* SPDX-License-Identifier: MIT */
/* MALI_SHARED_STATS: per-stage dma-buf contents of the frame handoff. See mali_sstats.h for the lines and the policy. */
#include "mali_sstats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mali_wait_debug.h"

/* The line tag of each kind: shared_write / shared_read / present (the greppable names of the brief). */
static const char *const kind_name[] = {"shared_write", "shared_read", "present"};

void
mali_sstats_init(struct mali_sstats *s)
{
   memset(s, 0, sizeof *s);
   pthread_mutex_init(&s->lock, NULL);
   /* Read once at device creation. The lines go to the MALI_WAIT_DBG sink: without one there is nowhere to put them,
    * so MALI_SHARED_STATS alone is off (and the whole feature costs one int test per submit). */
   const char *e = getenv("MALI_SHARED_STATS");
   s->on = e && e[0] && strcmp(e, "0") && mali_wait_debug();
}

void
mali_sstats_finish(struct mali_sstats *s)
{
   if (s->on && (s->stat_lines || s->stat_nomap || s->stat_nofd || s->stat_noslot))
      mali_wait_line("shared stats total lines=%u nomap=%u nofd=%u unlisted=%u bos=%u", s->stat_lines, s->stat_nomap,
                     s->stat_nofd, s->stat_noslot, s->nslot);
   s->on = 0;
   pthread_mutex_destroy(&s->lock);
}

bool
mali_sstats_on(struct mali_sstats *s)
{
   return s->on != 0;
}

/* The submit counter: one per submitted VkQueueSubmit, advanced here so the producer, consumer and present lines of
 * one process can be read in order. */
static uint64_t
sstats_next_seq(struct mali_sstats *s)
{
   uint64_t v;
   pthread_mutex_lock(&s->lock);
   v = ++s->seq;
   pthread_mutex_unlock(&s->lock);
   return v;
}

uint64_t
mali_sstats_next(struct mali_sstats *s)
{
   return sstats_next_seq(s);
}

/* Rate limit: false = this note is dropped. Also the one place that allocates a slot, so a slot is never created for a
 * note that is not printed. */
static bool
sstats_allow(struct mali_sstats *s, int ino, enum mali_sstats_kind kind, unsigned *seen)
{
   bool emit = false;
   pthread_mutex_lock(&s->lock);
   struct mali_sstats_slot *sl = NULL;
   for (unsigned i = 0; i < s->nslot; i++)
      if (s->slot[i].ino == ino && s->slot[i].kind == (unsigned char)kind) {
         sl = &s->slot[i];
         break;
      }
   if (!sl) {
      if (s->nslot < MALI_SSTATS_SLOTS) {
         sl = &s->slot[s->nslot++];
         sl->ino = ino;
         sl->kind = (unsigned char)kind;
         sl->seen = 0;
         sl->emitted = 0;
      } else {
         s->stat_noslot++;
      }
   }
   if (sl) {
      *seen = ++sl->seen;
      emit = sl->seen <= MALI_SSTATS_FIRST || sl->seen % MALI_SSTATS_EVERY == 0;
      if (emit)
         sl->emitted++;
   }
   pthread_mutex_unlock(&s->lock);
   return emit;
}

void
mali_sstats_note(struct mali_sstats *s, const struct mali_backend_ops *be, struct mali_kdev *kdev,
                 const struct mali_bo *bo, enum mali_sstats_kind kind, uint64_t seq)
{
   if (!s->on || !bo || bo->size < MALI_SSTATS_MIN_SIZE)
      return;
   int ino = -1;
   if (bo->dmabuf_fd >= 0) {
      struct stat st;
      if (fstat(bo->dmabuf_fd, &st) == 0)
         ino = (int)st.st_ino;
   }
   if (ino < 0) {
      pthread_mutex_lock(&s->lock);
      s->stat_nofd++;
      pthread_mutex_unlock(&s->lock);
      return;
   }
   unsigned seen = 0;
   if (!sstats_allow(s, ino, kind, &seen))
      return;
   if (!bo->cpu) {
      pthread_mutex_lock(&s->lock);
      s->stat_nomap++;
      pthread_mutex_unlock(&s->lock);
      mali_wait_line("%s bo=%d size=%zu unmapped=1 seen=%u reason=no-cpu-mapping",
                     kind_name[kind & 3], ino, bo->size, seen);
      return;
   }

   /* The GPU wrote this BO: the same device->CPU sync the driver does before any CPU read (mali_wsi.c's
    * present_image_cpu_sync, mali_memory.c's invalidate). */
   if (be) {
      if (be->bo_host_touch)
         be->bo_host_touch(kdev, (struct mali_bo *)bo);
      if (be->bo_cache)
         be->bo_cache(kdev, (struct mali_bo *)bo, 0, bo->size, true);
   }

   const uint8_t *p = (const uint8_t *)bo->cpu;
   size_t n = 0;
   unsigned long long nz = 0, sum = 0;
   for (size_t i = 0; i < bo->size; i += MALI_SSTATS_STRIDE) {
      sum += p[i];
      nz += p[i] != 0;
      n++;
   }
   if (!n)
      return;
   pthread_mutex_lock(&s->lock);
   s->stat_lines++;
   pthread_mutex_unlock(&s->lock);
   mali_wait_line("%s bo=%d size=%zu nonzero=%.1f mean=%.2f seq=%llu samples=%zu seen=%u",
                  kind_name[kind & 3], ino, bo->size, 100.0 * (double)nz / (double)n, (double)sum / (double)n,
                  (unsigned long long)seq, n, seen);
}
