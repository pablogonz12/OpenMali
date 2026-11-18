/* SPDX-License-Identifier: MIT */
/* vk_sync type over sync_files from the kbase KCPU queue. The runtime's vk_sync_timeline (emulated
 * timeline semaphores) uses this as its point type. */
#include "mali_queue.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

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

   for (;;) {
      pthread_mutex_lock(&dev->sync_lock);
      if (s->signaled && s->fd < 0) {
         pthread_mutex_unlock(&dev->sync_lock);
         return VK_SUCCESS;
      }
      int fd = s->fd >= 0 ? dup(s->fd) : -1;
      uint64_t now = now_ns();
      uint64_t left = abs_timeout_ns > now ? abs_timeout_ns - now : 0;

      if (s->fd < 0) {
         /* Not submitted yet (queue thread has not got to it): wait for it to get a fence. */
         if (left == 0) {
            pthread_mutex_unlock(&dev->sync_lock);
            return VK_TIMEOUT;
         }
         if (abs_timeout_ns == UINT64_MAX) {
            pthread_cond_wait(&dev->sync_cv, &dev->sync_lock);
         } else {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t t = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec + left;
            ts.tv_sec = t / 1000000000ull;
            ts.tv_nsec = t % 1000000000ull;
            pthread_cond_timedwait(&dev->sync_cv, &dev->sync_lock, &ts);
         }
         pthread_mutex_unlock(&dev->sync_lock);
         continue;
      }
      pthread_mutex_unlock(&dev->sync_lock);
      if (fd < 0)
         return vk_error(vk, VK_ERROR_OUT_OF_HOST_MEMORY);

      /* Slices of 100 ms so that a reset/move/destroy on another thread and a device loss are noticed. */
      uint64_t ms = left / 1000000ull;
      int slice = ms > 100 ? 100 : (int)ms;
      int r = be->fence_wait(dev->kdev, fd, slice);
      close(fd);
      if (r == 0)
         return VK_SUCCESS;
      if (r == -EIO)
         return vk_device_set_lost(vk, "GPU fault while waiting on a fence");
      if (r != -ETIMEDOUT)
         return vk_errorf(vk, VK_ERROR_UNKNOWN, "fence wait failed: %d", r);
      if (vk_device_is_lost(vk))
         return VK_ERROR_DEVICE_LOST;
      if (now_ns() >= abs_timeout_ns)
         return VK_TIMEOUT;
   }
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
   if (s->fd >= 0) {
      d = fcntl(s->fd, F_DUPFD_CLOEXEC, 3);
      if (d < 0) {
         pthread_mutex_unlock(&dev->sync_lock);
         return vk_error(vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }
   pthread_mutex_unlock(&dev->sync_lock);
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
