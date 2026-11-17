/* SPDX-License-Identifier: MIT */
/* Fake backend: no kernel, no GPU. Describes one of the two target GPUs so the front end can be
 * built and exercised on the laptop. MALI_FAKE_GPU=v11 (default) or v13. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "mali_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int
fake_probe(struct mali_gpu_info *out)
{
   const char *which = getenv("MALI_FAKE_GPU");

   memset(out, 0, sizeof(*out));
   out->fake = true;
   out->heap_bytes = 2ull << 30;

   if (which == NULL || strcmp(which, "v11") == 0) {
      /* CMF Phone 1: Mali-G615 MC2, arch 11.8, kbase uAPI 1.21 */
      out->gpu_id = 0xB8030000u;
      out->core_count = 2;
      out->core_id_range = 2;
      out->tls_threads = 2048;
      out->uapi_major = 1;
      out->uapi_minor = 21;
      strcpy(out->name, "Mali-G615 MC2");
   } else if (strcmp(which, "v13") == 0) {
      /* Poco X8 Pro Max: Mali-G925 MC11, arch 13.8, kbase uAPI 1.30 */
      out->gpu_id = 0xD8000000u;
      out->core_count = 11;
      out->core_id_range = 11;
      out->tls_threads = 2048;
      out->uapi_major = 1;
      out->uapi_minor = 30;
      strcpy(out->name, "Mali-G925 MC11");
   } else {
      return -ENODEV;
   }

   out->arch_major = out->gpu_id >> 28;
   out->arch_minor = (out->gpu_id >> 24) & 0xf;
   return 0;
}

/* The fake "GPU" retires every submission at once: the fence is an eventfd that is already readable.
 * MALI_FAULT_INJECT=n makes the n-th submission fail (device lost). MALI_FAKE_SLOW_US=n delays the
 * signal by n us (a helper thread writes the eventfd) so that waits really block. */
struct mali_kdev {
   pthread_mutex_t lock;
   unsigned long submits, fault_at;
   unsigned slow_us;
   unsigned long cs_submits, cs_instr;
   uint64_t next_va;
   bool lost;
};

static int
fake_device_create(const struct mali_gpu_info *gpu, struct mali_kdev **out)
{
   struct mali_kdev *k = calloc(1, sizeof(*k));
   if (!k)
      return -ENOMEM;
   pthread_mutex_init(&k->lock, NULL);
   const char *f = getenv("MALI_FAULT_INJECT"), *sl = getenv("MALI_FAKE_SLOW_US");
   k->fault_at = f ? strtoul(f, NULL, 10) : 0;
   k->slow_us = sl ? (unsigned)strtoul(sl, NULL, 10) : 0;
   k->next_va = 0x5f00000000ull;
   *out = k;
   return 0;
}

static void
fake_device_destroy(struct mali_kdev *k)
{
   if (!k)
      return;
   pthread_mutex_destroy(&k->lock);
   free(k);
}

static int
fake_map(struct mali_kdev *k, int fd, size_t size, uint32_t flags, struct mali_bo *out)
{
   void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (p == MAP_FAILED)
      return -errno;
   pthread_mutex_lock(&k->lock);
   uint64_t va = k->next_va;
   k->next_va += size + 4096;
   pthread_mutex_unlock(&k->lock);
   *out = (struct mali_bo){.gpu_va = va, .cpu = p, .size = size, .flags = flags, .dmabuf_fd = fd};
   return 0;
}

static int
fake_bo_alloc(struct mali_kdev *k, size_t size, uint32_t flags, struct mali_bo *out)
{
   size = (size + 4095) & ~(size_t)4095;
   if (size > (1ull << 32))
      return -ENOMEM;
   int fd = (int)syscall(SYS_memfd_create, "mali-fake-bo", 1u /* MFD_CLOEXEC */); /* no libc wrapper below API 30 */
   if (fd < 0)
      return -errno;
   if (ftruncate(fd, size)) {
      int e = errno;
      close(fd);
      return -e;
   }
   int r = fake_map(k, fd, size, flags, out);
   if (r) {
      close(fd);
      return r;
   }
   if (!(flags & MALI_BO_EXPORT)) {
      close(fd);
      out->dmabuf_fd = -1;
   }
   return 0;
}

static int
fake_bo_import(struct mali_kdev *k, int fd, size_t size, uint32_t flags, struct mali_bo *out)
{
   size = (size + 4095) & ~(size_t)4095;
   struct stat st;
   if (fstat(fd, &st) || (size_t)st.st_size < size)
      return -EINVAL;
   int d = fcntl(fd, F_DUPFD_CLOEXEC, 3);
   if (d < 0)
      return -errno;
   int r = fake_map(k, d, size, flags | MALI_BO_EXPORT, out);
   if (r)
      close(d);
   return r;
}

static void
fake_bo_free(struct mali_kdev *k, struct mali_bo *bo)
{
   if (bo->cpu)
      munmap(bo->cpu, bo->size);
   if (bo->dmabuf_fd >= 0)
      close(bo->dmabuf_fd);
   bo->cpu = NULL;
   bo->dmabuf_fd = -1;
}

static void
fake_bo_cache(struct mali_kdev *k, const struct mali_bo *bo, size_t off, size_t len, bool invalidate)
{
   __sync_synchronize();
}

static int
fake_bo_export(struct mali_kdev *k, const struct mali_bo *bo)
{
   if (bo->dmabuf_fd < 0)
      return -EINVAL;
   int fd = fcntl(bo->dmabuf_fd, F_DUPFD_CLOEXEC, 3);
   return fd < 0 ? -errno : fd;
}

static void *
slow_signal(void *arg)
{
   uintptr_t v = (uintptr_t)arg;
   int fd = (int)(v & 0xffffffff);
   usleep((unsigned)(v >> 32));
   uint64_t one = 1;
   if (write(fd, &one, sizeof(one)) < 0) {}
   close(fd);
   return NULL;
}

static int
fake_submit_signal(struct mali_kdev *k, int *fence_fd)
{
   pthread_mutex_lock(&k->lock);
   bool bad = k->lost || (k->fault_at && k->submits + 1 == k->fault_at);
   if (bad)
      k->lost = true;
   else
      k->submits++;
   unsigned slow = k->slow_us;
   pthread_mutex_unlock(&k->lock);
   if (bad)
      return -EIO;

   int fd = eventfd(slow ? 0 : 1, EFD_CLOEXEC | EFD_NONBLOCK);
   if (fd < 0)
      return -errno;
   if (slow) {
      pthread_t t;
      int d = dup(fd);
      if (pthread_create(&t, NULL, slow_signal, (void *)((uintptr_t)d | ((uintptr_t)slow << 32)))) {
         uint64_t one = 1;
         if (write(fd, &one, sizeof(one)) < 0) {}
         close(d);
      } else {
         pthread_detach(t);
      }
   }
   *fence_fd = fd;
   return 0;
}

static int
fake_fence_wait(struct mali_kdev *k, int fence_fd, int timeout_ms)
{
   struct pollfd pf = {fence_fd, POLLIN, 0};
   for (;;) {
      int r = poll(&pf, 1, timeout_ms);
      if (r < 0 && errno == EINTR)
         continue;
      if (r < 0)
         return -errno;
      if (r == 0)
         return -ETIMEDOUT;
      return (pf.revents & POLLERR) ? -EIO : 0;
   }
}

static bool
fake_device_lost(struct mali_kdev *k)
{
   return k && k->lost;
}

/* M9: the fake GPU accepts and ignores frame command streams (nothing renders); counters are never advanced. */
static int
fake_submit_cs(struct mali_kdev *k, const uint64_t *cs, unsigned n)
{
   pthread_mutex_lock(&k->lock);
   bool lost = k->lost;
   k->cs_submits++;
   k->cs_instr += n;
   pthread_mutex_unlock(&k->lock);
   return lost ? -EIO : 0;
}

static int
fake_wait_counter(struct mali_kdev *k, const volatile uint64_t *c, uint64_t target, int timeout_ms)
{
   return 0;
}

static int
fake_heap_create(struct mali_kdev *k, uint32_t chunk, uint32_t initial, uint32_t max, struct mali_heap *out)
{
   *out = (struct mali_heap){.ctx_va = 0xf0000000ull, .first_chunk_va = 0xf1000000ull, .chunk_size = chunk};
   return 0;
}

static void
fake_heap_destroy(struct mali_kdev *k, struct mali_heap *heap)
{
}

const struct mali_backend_ops mali_backend_fake = {
   .name = "fake",
   .probe = fake_probe,
   .device_create = fake_device_create,
   .device_destroy = fake_device_destroy,
   .bo_alloc = fake_bo_alloc,
   .bo_import = fake_bo_import,
   .bo_free = fake_bo_free,
   .bo_cache = fake_bo_cache,
   .bo_export = fake_bo_export,
   .submit_signal = fake_submit_signal,
   .fence_wait = fake_fence_wait,
   .device_lost = fake_device_lost,
   .submit_cs = fake_submit_cs,
   .wait_counter = fake_wait_counter,
   .heap_create = fake_heap_create,
   .heap_destroy = fake_heap_destroy,
};

const struct mali_backend_ops *
mali_backend_select(void)
{
   const char *sel = getenv("MALI_BACKEND");

   if (sel != NULL && strcmp(sel, "fake") == 0)
      return &mali_backend_fake;

   return &mali_backend_kbase;
}
