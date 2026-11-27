/* SPDX-License-Identifier: MIT */
/* Targeted implicit sync for dma-bufs shared outside a swapchain. See mali_isync.h for the policy and the evidence. */
#include "mali_isync.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void
mali_isync_dev_init(struct mali_isync_dev *d)
{
   pthread_mutex_init(&d->lock, NULL);
   /* Read at device creation: MALI_IMPLICIT_SYNC=0 turns the whole mechanism off, and the plan functions are pure
    * (they never touch the environment). */
   const char *e = getenv("MALI_IMPLICIT_SYNC");
   d->enabled = !(e && !strcmp(e, "0"));
   d->import_ok = -1;
   d->export_ok = -1;
   d->stat_fence = d->stat_cpu_wait = d->stat_wait = d->stat_err = 0;
}

void
mali_isync_dev_finish(struct mali_isync_dev *d)
{
   const char *e = getenv("MALI_ISYNC_DBG");
   if (e && *e && strcmp(e, "0") && (d->stat_fence || d->stat_cpu_wait || d->stat_wait || d->stat_err))
      fprintf(stderr, "PROF/isync fence=%u cpu_wait=%u wait=%u err=%u import=%d export=%d\n",
              d->stat_fence, d->stat_cpu_wait, d->stat_wait, d->stat_err, d->import_ok, d->export_ok);
   pthread_mutex_destroy(&d->lock);
}

/* MALI_IMPLICIT_SYNC, read once at device creation (mali_isync_dev_init). */
bool
mali_isync_enabled(struct mali_isync_dev *d)
{
   pthread_mutex_lock(&d->lock);
   bool on = d->enabled != 0;
   pthread_mutex_unlock(&d->lock);
   return on;
}

/* Kernel import probe: -1 (not probed yet) is optimistic - the first ioctl decides and latches ENOTTY. */
enum mali_isync_action
mali_isync_plan_producer(const struct mali_isync_dev *d, unsigned nfds, bool unlisted)
{
   if (d->enabled == 0)
      return MALI_ISYNC_NONE;
   if (!nfds && !unlisted)
      return MALI_ISYNC_NONE;
   /* A buffer this submit writes but cannot name (wrote_full): the fence cannot be attached to it, so the CPU
    * wait is the only way to make its consumer see the finished frame. */
   if (unlisted)
      return MALI_ISYNC_CPU_WAIT;
   if (d->import_ok == 0)
      return MALI_ISYNC_CPU_WAIT;
   return MALI_ISYNC_FENCE;
}

enum mali_isync_action
mali_isync_plan_consumer(const struct mali_isync_dev *d, unsigned nfds)
{
   if (d->enabled == 0 || !nfds || d->export_ok == 0)
      return MALI_ISYNC_NONE;
   return MALI_ISYNC_FENCE;
}

enum mali_isync_action
mali_isync_producer(struct mali_isync_dev *d, const struct mali_backend_ops *be, struct mali_kdev *kdev,
                    const int *fds, unsigned nfds, bool unlisted, int fence_fd)
{
   enum mali_isync_action a;

   pthread_mutex_lock(&d->lock);
   a = mali_isync_plan_producer(d, nfds, unlisted);
   if (a == MALI_ISYNC_FENCE) {
      for (unsigned i = 0; i < nfds; i++) {
         int r = be->dmabuf_import_sync_file(kdev, fds[i], MALI_DMA_BUF_SYNC_WRITE, fence_fd);
         if (r == -ENOTTY) {
            /* 5.10 (Poco X4): the ioctl does not exist, so no implicit fence can be attached. The CPU wait below
             * is the only sync there is; latch it so later submits skip the failing ioctl. */
            d->import_ok = 0;
            a = MALI_ISYNC_CPU_WAIT;
            break;
         }
         if (r != 0)
            d->stat_err++;
         else
            d->stat_fence++;
      }
      if (d->import_ok < 0)
         d->import_ok = a == MALI_ISYNC_FENCE;
   }
   if (a == MALI_ISYNC_CPU_WAIT) {
      d->stat_cpu_wait++;
      if (fence_fd >= 0) {
         /* The submission is in flight; the consumer must not read the buffer before it retires. */
         pthread_mutex_unlock(&d->lock);
         (void)be->fence_wait(kdev, fence_fd, -1);
         return a;
      }
   }
   pthread_mutex_unlock(&d->lock);
   return a;
}

void
mali_isync_consumer(struct mali_isync_dev *d, const struct mali_backend_ops *be, struct mali_kdev *kdev,
                    const int *fds, unsigned nfds)
{
   pthread_mutex_lock(&d->lock);
   if (mali_isync_plan_consumer(d, nfds) == MALI_ISYNC_NONE) {
      pthread_mutex_unlock(&d->lock);
      return;
   }
   pthread_mutex_unlock(&d->lock);

   for (unsigned i = 0; i < nfds; i++) {
      int sf = -1;
      int r = be->dmabuf_export_sync_file(kdev, fds[i], MALI_DMA_BUF_SYNC_READ, &sf);
      pthread_mutex_lock(&d->lock);
      if (r == -ENOTTY) {
         d->export_ok = 0;
         pthread_mutex_unlock(&d->lock);
         return;
      }
      if (r != 0)
         d->stat_err++;
      else
         d->stat_wait++;
      if (d->export_ok < 0 && r == 0)
         d->export_ok = 1;
      pthread_mutex_unlock(&d->lock);
      if (r == 0 && sf >= 0) {
         /* No KCPU fence wait for a foreign fence in a submit: wait it on the CPU (poll) before submitting. */
         (void)be->fence_wait(kdev, sf, -1);
         close(sf);
      }
   }
}
