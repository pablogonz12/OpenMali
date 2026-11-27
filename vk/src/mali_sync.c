/* SPDX-License-Identifier: MIT */
/* vk_sync type over sync_files from the kbase KCPU queue. The runtime's vk_sync_timeline (emulated
 * timeline semaphores) uses this as its point type. */
#include "mali_queue.h"
#include "mali_wait_debug.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef MALI_GIT_SHA
#define MALI_GIT_SHA "unknown"
#endif
static pthread_once_t wait_once = PTHREAD_ONCE_INIT;
static FILE *wait_file;
static pthread_mutex_t wait_file_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t wait_submits, wait_presents, wait_active, wait_last_hb;

static void
wait_init(void)
{
   const char *env = getenv("MALI_WAIT_DBG");
   if (!env)
      return;
   char comm[16] = "?";
   prctl(PR_GET_NAME, comm, 0, 0, 0);
   if (!strcmp(env, "1")) {
      wait_file = stderr;
   } else if (env[0] == '/') {
      size_t len = strlen(env) + 64;
      char *path = malloc(len);
      if (!path)
         return;
      snprintf(path, len, "%s-%s-%ld.log", env, comm, (long)getpid());
      int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
      free(path);
      if (fd >= 0) {
         wait_file = fdopen(fd, "a");
         if (!wait_file)
            close(fd);
      }
      if (wait_file)
         setvbuf(wait_file, NULL, _IOLBF, 0);
   }
   if (wait_file)
      fprintf(wait_file, "mali_wait: startup comm=%s pid=%ld mali-open %s\n",
              comm, (long)getpid(), MALI_GIT_SHA);
}

int
mali_wait_debug(void)
{
   pthread_once(&wait_once, wait_init);
   return wait_file != NULL;
}

void
mali_wait_event(const char *fmt, ...)
{
   if (!mali_wait_debug())
      return;
   va_list ap;
   va_start(ap, fmt);
   pthread_mutex_lock(&wait_file_lock);
   fputs("mali_wait: ", wait_file);
   vfprintf(wait_file, fmt, ap);
   fputc('\n', wait_file);
   pthread_mutex_unlock(&wait_file_lock);
   va_end(ap);
}

void
mali_wait_result(const char *function, int result, const char *fmt, ...)
{
   if (!result || !mali_wait_debug())
      return;
   static struct { const char *function; int result; uint64_t count; } counts[128];
   pthread_mutex_lock(&wait_file_lock);
   unsigned i;
   for (i = 0; i < 128 && counts[i].function; i++)
      if (counts[i].result == result && !strcmp(counts[i].function, function))
         break;
   if (i < 128) {
      counts[i].function = function;
      counts[i].result = result;
      uint64_t count = ++counts[i].count;
      fprintf(wait_file, "mali_wait: result function=%s result=%d count=%" PRIu64, function, result, count);
      if (count <= 20 && fmt) {
         va_list ap;
         va_start(ap, fmt);
         fputc(' ', wait_file);
         vfprintf(wait_file, fmt, ap);
         va_end(ap);
      }
      fputc('\n', wait_file);
   }
   pthread_mutex_unlock(&wait_file_lock);
}

void
mali_wait_log(const char *what, const void *sync, const char *state, uint64_t ms, uint64_t value)
{
   if (!mali_wait_debug())
      return;
   char comm[16] = "?";
   prctl(PR_GET_NAME, comm, 0, 0, 0);
   pthread_mutex_lock(&wait_file_lock);
   fprintf(wait_file, "mali_wait: %s %p %s %" PRIu64 " ms comm=%s tid=%ld value=%" PRIu64 "\n",
           what, sync, state, ms, comm, (long)syscall(SYS_gettid), value);
   pthread_mutex_unlock(&wait_file_lock);
}

void
mali_wait_count(int change)
{
   if (mali_wait_debug()) {
      if (change > 0)
         atomic_fetch_add(&wait_active, 1);
      else
         atomic_fetch_sub(&wait_active, 1);
   }
}

void
mali_wait_heartbeat(struct mali_device *dev, int submit, int present)
{
   if (!mali_wait_debug())
      return;
   if (submit) atomic_fetch_add(&wait_submits, 1);
   if (present) atomic_fetch_add(&wait_presents, 1);
   uint64_t now = mali_wait_now(), old = atomic_load(&wait_last_hb);
   if (now - old < 1000000000ull ||
       !atomic_compare_exchange_strong(&wait_last_hb, &old, now))
      return;
   uint64_t submitted = UINT64_MAX, done = UINT64_MAX;
   const struct mali_backend_ops *be = mali_device_pdev(dev)->backend;
   if (be->debug_seqnos)
      be->debug_seqnos(dev->kdev, &submitted, &done);
   pthread_mutex_lock(&wait_file_lock);
   fprintf(wait_file, "hb t=%" PRIu64 " submits=%" PRIu64 " presents=%" PRIu64
           " last_submit_seqno=%" PRIu64 " last_done_seqno=%" PRIu64 " waits_in_progress=%" PRIu64 "\n",
           now / 1000000, atomic_load(&wait_submits), atomic_load(&wait_presents),
           submitted, done, atomic_load(&wait_active));
   pthread_mutex_unlock(&wait_file_lock);
}

static uint64_t
now_ns(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static struct mali_device *
to_dev(struct vk_device *vk)
{
   return container_of(vk, struct mali_device, vk);
}

static struct mali_sync *
to_sync(struct vk_sync *s)
{
   return container_of(s, struct mali_sync, vk);
}

static VkResult
mali_sync_init(struct vk_device *vk, struct vk_sync *sync, uint64_t initial_value)
{
   struct mali_sync *s = to_sync(sync);

   s->fd = -1;
   s->owner = NULL;
   s->signaled = initial_value != 0;
   return VK_SUCCESS;
}

static void
mali_sync_finish(struct vk_device *vk, struct vk_sync *sync)
{
   struct mali_sync *s = to_sync(sync);

   if (s->fd >= 0)
      close(s->fd);
   s->fd = -1;
}

static VkResult
mali_sync_signal(struct vk_device *vk, struct vk_sync *sync, uint64_t value)
{
   struct mali_device *dev = to_dev(vk);
   struct mali_sync *s = to_sync(sync);

   pthread_mutex_lock(&dev->sync_lock);
   if (s->fd >= 0)
      close(s->fd);
   s->fd = -1;
   s->owner = NULL;
   s->signaled = true;
   pthread_cond_broadcast(&dev->sync_cv);
   pthread_mutex_unlock(&dev->sync_lock);
   return VK_SUCCESS;
}

static VkResult
mali_sync_reset(struct vk_device *vk, struct vk_sync *sync)
{
   struct mali_device *dev = to_dev(vk);
   struct mali_sync *s = to_sync(sync);

   pthread_mutex_lock(&dev->sync_lock);
   if (s->fd >= 0)
      close(s->fd);
   s->fd = -1;
   s->owner = NULL;
   s->signaled = false;
   pthread_mutex_unlock(&dev->sync_lock);
   return VK_SUCCESS;
}

static VkResult
mali_sync_move(struct vk_device *vk, struct vk_sync *dst, struct vk_sync *src)
{
   struct mali_device *dev = to_dev(vk);
   struct mali_sync *d = to_sync(dst), *s = to_sync(src);

   pthread_mutex_lock(&dev->sync_lock);
   if (d->fd >= 0)
      close(d->fd);
   d->fd = s->fd;
   d->signaled = s->signaled;
   d->owner = s->owner;
   s->owner = NULL;
   s->fd = -1;
   s->signaled = false;
   pthread_cond_broadcast(&dev->sync_cv);
   pthread_mutex_unlock(&dev->sync_lock);
   return VK_SUCCESS;
}

static VkResult
mali_sync_wait(struct vk_device *vk, struct vk_sync *sync, uint64_t wait_value,
               enum vk_sync_wait_flags wait_flags, uint64_t abs_timeout_ns)
{
   struct mali_device *dev = to_dev(vk);
   struct mali_sync *s = to_sync(sync);
   const struct mali_backend_ops *be = mali_device_pdev(dev)->backend;
   const bool dbg = mali_wait_debug();
   uint64_t start = dbg ? now_ns() : 0;
   bool logged_submit = false, logged_fence = false;
   VkResult result;
   if (dbg)
      mali_wait_count(1);

   for (;;) {
      pthread_mutex_lock(&dev->sync_lock);
      if (s->signaled && s->fd < 0) {
         pthread_mutex_unlock(&dev->sync_lock);
         result = VK_SUCCESS;
         goto done;
      }
      int fd = s->fd >= 0 ? dup(s->fd) : -1;
      uint64_t now = now_ns();
      uint64_t left = abs_timeout_ns > now ? abs_timeout_ns - now : 0;

      if (s->fd < 0) {
         /* Not submitted yet (queue thread has not got to it): wait for it to get a fence. */
         if (left == 0) {
            pthread_mutex_unlock(&dev->sync_lock);
            result = VK_TIMEOUT;
            goto done;
         }
         if (dbg && !logged_submit && now - start >= 2000000000ull) {
            mali_wait_log("sync_wait", s, "not submitted for", (now - start) / 1000000, wait_value);
            logged_submit = true;
         }
         uint64_t slice = left > 1000000000ull ? 1000000000ull : left;
         uint64_t t = now + slice;
         struct timespec ts = { .tv_sec = t / 1000000000ull, .tv_nsec = t % 1000000000ull };
         pthread_cond_timedwait(&dev->sync_cv, &dev->sync_lock, &ts);
         pthread_mutex_unlock(&dev->sync_lock);
         continue;
      }
      pthread_mutex_unlock(&dev->sync_lock);
      if (fd < 0)
         { result = vk_error(vk, VK_ERROR_OUT_OF_HOST_MEMORY); goto done; }
      if (left == 0) {
         /* A poll (vkGetFenceStatus, vkWaitForFences/AcquireNextImage with timeout 0): a pending fence answers at once from the fd and
          * never queues behind another thread's blocking wait in the backend (kbase serialises those in 20 ms slices). */
         struct pollfd pf = {fd, POLLIN, 0};
         if (poll(&pf, 1, 0) == 0) {
            close(fd);
            result = VK_TIMEOUT;
            goto done;
         }
      }

      /* Slices of 100 ms so that a reset/move/destroy on another thread and a device loss are noticed. */
      uint64_t ms = left / 1000000ull + (left % 1000000ull != 0);
      int slice = ms > 100 ? 100 : (int)ms;
      if (dbg && !logged_fence && now - start >= 2000000000ull) {
         mali_wait_log("fence_wait", s, "waiting for", (now - start) / 1000000, wait_value);
         logged_fence = true;
      }
      int r = be->fence_wait(dev->kdev, fd, slice);
      close(fd);
      if (r == 0)
         { result = VK_SUCCESS; goto done; }
      if (r == -EIO)
         { result = vk_device_set_lost(vk, "GPU fault while waiting on a fence"); goto done; }
      if (r != -ETIMEDOUT)
         { result = vk_errorf(vk, VK_ERROR_UNKNOWN, "fence wait failed: %d", r); goto done; }
      if (vk_device_is_lost(vk))
         { result = VK_ERROR_DEVICE_LOST; goto done; }
      if (now_ns() >= abs_timeout_ns)
         { result = VK_TIMEOUT; goto done; }
   }
done:
   if (logged_submit)
      mali_wait_log("sync_wait", s, "done after", (now_ns() - start) / 1000000, wait_value);
   if (logged_fence)
      mali_wait_log("fence_wait", s, "done after", (now_ns() - start) / 1000000, wait_value);
   if (dbg)
      mali_wait_count(-1);
   return result;
}

/* Attach a fence (caller keeps ownership of fd; -1 = signalled). */
static VkResult
mali_sync_import_sync_file(struct vk_device *vk, struct vk_sync *sync, int sync_file)
{
   struct mali_device *dev = to_dev(vk);
   struct mali_sync *s = to_sync(sync);
   int d = -1;

   if (sync_file >= 0) {
      d = fcntl(sync_file, F_DUPFD_CLOEXEC, 3);
      if (d < 0)
         return vk_error(vk, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   pthread_mutex_lock(&dev->sync_lock);
   if (s->fd >= 0)
      close(s->fd);
   s->fd = d;
   s->owner = NULL;
   s->signaled = d < 0;
   pthread_cond_broadcast(&dev->sync_cv);
   pthread_mutex_unlock(&dev->sync_lock);
   return VK_SUCCESS;
}

static VkResult
mali_sync_export_sync_file(struct vk_device *vk, struct vk_sync *sync, int *sync_file)
{
   struct mali_device *dev = to_dev(vk);
   struct mali_sync *s = to_sync(sync);
   int d = -1;

   pthread_mutex_lock(&dev->sync_lock);
   /* The signal operation may still be queued behind the runtime's submit thread: fd < 0 and not signalled means
    * "submitted but not yet handed to the driver" (a signal op that was never submitted is invalid usage, and the
    * driver cannot tell the two apart), not "already signalled". Exporting -1 then would let the compositor sample a
    * half-drawn buffer. Wait for the submission with no fixed deadline (dependencies may legitimately take long);
    * the predicate is rechecked under the lock on every wake and only device loss ends the wait early. */
   const bool dbg = mali_wait_debug();
   uint64_t start = dbg ? now_ns() : 0;
   bool logged = false;
   if (dbg)
      mali_wait_count(1);
   while (s->fd < 0 && !s->signaled) {
      if (vk_device_is_lost(vk) || mali_device_pdev(dev)->backend->device_lost(dev->kdev)) {
         pthread_mutex_unlock(&dev->sync_lock);
         if (logged)
            mali_wait_log("export_sync_file", s, "done after", (now_ns() - start) / 1000000, 0);
         if (dbg)
            mali_wait_count(-1);
         return VK_ERROR_DEVICE_LOST;
      }
      if (dbg && !logged && now_ns() - start >= 2000000000ull) {
         mali_wait_log("export_sync_file", s, "waiting for submit for", (now_ns() - start) / 1000000, 0);
         logged = true;
      }
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts); /* sync_cv is a CLOCK_MONOTONIC condvar (mali_queue.c) */
      ts.tv_nsec += 100 * 1000 * 1000;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&dev->sync_cv, &dev->sync_lock, &ts);
   }
   if (s->fd >= 0) {
      d = fcntl(s->fd, F_DUPFD_CLOEXEC, 3);
      if (d < 0) {
         pthread_mutex_unlock(&dev->sync_lock);
         if (dbg)
            mali_wait_count(-1);
         return vk_error(vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }
   pthread_mutex_unlock(&dev->sync_lock);
   if (logged)
      mali_wait_log("export_sync_file", s, "done after", (now_ns() - start) / 1000000, 0);
   if (dbg)
      mali_wait_count(-1);
   *sync_file = d; /* -1: already signalled */
   return VK_SUCCESS;
}

const struct vk_sync_type mali_sync_type = {
   .size = sizeof(struct mali_sync),
   .features = VK_SYNC_FEATURE_BINARY | VK_SYNC_FEATURE_GPU_WAIT | VK_SYNC_FEATURE_GPU_MULTI_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT | VK_SYNC_FEATURE_CPU_RESET | VK_SYNC_FEATURE_CPU_SIGNAL,
   .init = mali_sync_init,
   .finish = mali_sync_finish,
   .signal = mali_sync_signal,
   .reset = mali_sync_reset,
   .move = mali_sync_move,
   .wait = mali_sync_wait,
   .import_sync_file = mali_sync_import_sync_file,
   .export_sync_file = mali_sync_export_sync_file,
};

void
mali_sync_init_physical(struct mali_physical_device *pdev)
{
   pdev->timeline_type = vk_sync_timeline_get_type(&mali_sync_type);
   pdev->sync_types[0] = &mali_sync_type;
   pdev->sync_types[1] = &pdev->timeline_type.sync;
   pdev->sync_types[2] = NULL;
   pdev->vk.supported_sync_types = pdev->sync_types;
}
