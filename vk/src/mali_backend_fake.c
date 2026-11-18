/* SPDX-License-Identifier: MIT */
/* Fake backend: no kernel, no GPU. Describes one of the two target GPUs so the front end can be
 * built and exercised on the laptop. MALI_FAKE_GPU=v11 (default), v10 or v13. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "mali_backend.h"
#include <time.h>

#include <errno.h>
#include <stdio.h>
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
   } else if (strcmp(which, "v10") == 0) {
      /* Poco X4 GT: Mali-G610 MC6 (Dimensity 8100), arch 10.8, CSF. Measured with probe/probe.c
       * (vk/DEVICE-poco-x4.md): gpu_id 0xA8670000, kbase r32p1 uAPI 1.18, shader_present 0x130013 (6 cores, non-contiguous). */
      out->gpu_id = 0xA8670000u;
      out->core_count = 6;
      out->core_id_range = 21;   /* highest set bit of shader_present 0x130013, as the kbase backend computes it */
      out->tls_threads = 2048;
      out->uapi_major = 1;
      out->uapi_minor = 18;
      strcpy(out->name, "Mali-G610 MC6");
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
   /* Offline checking (MALI_FAKE_DUMP=path|-): BO registry + decoded-stream dump, see tests/golden.py */
   struct fake_bo *bos;
   unsigned nbos, capbos, id, kicks;
};


/* ---- offline checking: BO registry, BO flag rules, command-stream dump (read by tests/golden.py) ----
 * VAs are a deterministic bump allocator and BOs are numbered in creation order, so a dump has no host pointers/timestamps. */
struct fake_bo {
   uint64_t va;
   size_t size;
   uint32_t flags;
   int kind;                  /* 0 = BO, 1 = tiler heap (no CPU mapping) */
   bool live;
   void *cpu;
   uint64_t *phash;           /* per 4 KiB page FNV-1a of what was last dumped */
};

static unsigned g_fake_ndev;
static FILE *g_dump_file;

static FILE *
dump_out(void)
{
   static bool tried;
   if (!tried) {
      tried = true;
      const char *p = getenv("MALI_FAKE_DUMP");
      if (p && *p)
         g_dump_file = strcmp(p, "-") ? fopen(p, "a") : stdout;
   }
   return g_dump_file;
}

/* Rules for BO flags (hardware found: wrong allocation flags broke Poco v13; the kbase backend picks the allocation type by
 * flag precedence EXPORT > EVENT > EXEC > plain, so a combination silently loses a flag). Returns a message or NULL. */
static const char *
fake_flag_rule(size_t size, uint32_t flags)
{
   const uint32_t known = MALI_BO_CACHED | MALI_BO_EXPORT | MALI_BO_EVENT | MALI_BO_EXEC | MALI_BO_HOSTLAZY | MALI_BO_HOT;
   if (flags & ~known)
      return "unknown MALI_BO_* bits";
   if (size == 0)
      return "zero-size BO";
   if ((flags & MALI_BO_EVENT) && (flags & (MALI_BO_CACHED | MALI_BO_EXPORT | MALI_BO_EXEC | MALI_BO_HOT | MALI_BO_HOSTLAZY)))
      return "EVENT BO (CQS words, kernel visible) must be plain uncached GPU memory: no CACHED/EXPORT/EXEC/HOT/HOSTLAZY";
   if ((flags & MALI_BO_EXEC) && (flags & (MALI_BO_CACHED | MALI_BO_EXPORT | MALI_BO_HOT | MALI_BO_HOSTLAZY)))
      return "EXEC BO (shader binaries) must not be CACHED/EXPORT/HOT/HOSTLAZY (GPU_RD|GPU_EX only)";
   if ((flags & MALI_BO_EXPORT) && (flags & MALI_BO_HOT))
      return "HOT (arena) BO cannot be a dma-buf export";
   if ((flags & MALI_BO_HOT) && (flags & MALI_BO_HOSTLAZY))
      return "HOT and HOSTLAZY are opposites (MEM_SYNC every submit vs never)";
   return NULL;
}

static uint64_t
page_hash(const uint8_t *p)
{
   uint64_t h = 1469598103934665603ull;
   for (unsigned i = 0; i < 4096; i += 8) {
      uint64_t w;
      memcpy(&w, p + i, 8);
      h = (h ^ w) * 1099511628211ull;
   }
   return h;
}

static struct fake_bo *
reg_bo(struct mali_kdev *k, uint64_t va, size_t size, uint32_t flags, int kind, void *cpu)
{
   if (k->nbos == k->capbos) {
      unsigned c = k->capbos ? k->capbos * 2 : 64;
      struct fake_bo *n = realloc(k->bos, c * sizeof(*n));
      if (!n)
         return NULL;
      k->bos = n;
      k->capbos = c;
   }
   struct fake_bo *b = &k->bos[k->nbos++];
   *b = (struct fake_bo){.va = va, .size = size, .flags = flags, .kind = kind, .live = true, .cpu = cpu};
   if (kind == 0) {
      static const uint8_t zero[4096];
      uint64_t zh = page_hash(zero);   /* all-zero pages are never dumped (a reader's Mem reads unmapped bytes as 0 anyway) */
      size_t np = size / 4096 ? size / 4096 : 1;
      b->phash = malloc(np * sizeof(uint64_t));
      for (size_t i = 0; b->phash && i < np; i++)
         b->phash[i] = zh;
   }
   return b;
}

static void
emit_hex(FILE *f, const uint8_t *p, size_t n)
{
   static const char hx[] = "0123456789abcdef";
   for (size_t i = 0; i < n; i++) {
      fputc(hx[p[i] >> 4], f);
      fputc(hx[p[i] & 15], f);
   }
}

/* One kick: live regions (numbered by creation order), the pages that changed since the last dump, then the stream. */
static void
dump_kick(struct mali_kdev *k, const uint64_t *cs, unsigned n)
{
   FILE *f = dump_out();
   if (!f)
      return;
   k->kicks++;
   fprintf(f, "{\"ev\":\"kick\",\"dev\":%u,\"n\":%u}\n", k->id, k->kicks);
   for (unsigned i = 0; i < k->nbos; i++) {
      struct fake_bo *b = &k->bos[i];
      fprintf(f, "{\"ev\":\"region\",\"dev\":%u,\"id\":%u,\"va\":\"0x%llx\",\"len\":%zu,\"kind\":%d,\"flags\":%u,\"live\":%d}\n", k->id, i,
              (unsigned long long)b->va, b->size, b->kind, b->flags, b->live);
      if (!b->live || b->kind != 0 || !b->cpu)
         continue;
      for (size_t o = 0; o < b->size; o += 4096) {
         const uint8_t *pg = (const uint8_t *)b->cpu + o;
         uint64_t h = page_hash(pg);
         if (!b->phash || h == b->phash[o / 4096])
            continue;
         b->phash[o / 4096] = h;
         fprintf(f, "{\"ev\":\"mem\",\"dev\":%u,\"va\":\"0x%llx\",\"hex\":\"", k->id, (unsigned long long)(b->va + o));
         emit_hex(f, pg, 4096);
         fputs("\"}\n", f);
      }
   }
   fprintf(f, "{\"ev\":\"ring\",\"dev\":%u,\"n\":%u,\"hex\":\"", k->id, k->kicks);
   emit_hex(f, (const uint8_t *)cs, (size_t)n * 8);
   fputs("\"}\n", f);
   fflush(f);
}

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
   k->id = g_fake_ndev++;
   *out = k;
   return 0;
}

static void
fake_device_destroy(struct mali_kdev *k)
{
   if (!k)
      return;
   pthread_mutex_destroy(&k->lock);
   for (unsigned i = 0; i < k->nbos; i++)
      free(k->bos[i].phash);
   free(k->bos);
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
   pthread_mutex_lock(&k->lock);
   reg_bo(k, va, size, flags, 0, p);
   pthread_mutex_unlock(&k->lock);
   return 0;
}

static int
fake_bo_alloc(struct mali_kdev *k, size_t size, uint32_t flags, struct mali_bo *out)
{
   if (getenv("MALI_FAKE_BADFLAGS"))   /* self-test of the flag rules: every BO asks for a combination the kernel would silently downgrade */
      flags |= MALI_BO_EVENT | MALI_BO_CACHED;
   const char *rule = fake_flag_rule(size, flags);
   if (rule) {
      fprintf(stderr, "mali-fake RULE VIOLATION: bo_alloc(size %zu, flags 0x%x): %s\n", size, flags, rule);
      return -EINVAL;
   }
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
   pthread_mutex_lock(&k->lock);
   for (unsigned i = 0; i < k->nbos; i++)
      if (k->bos[i].live && k->bos[i].kind == 0 && k->bos[i].va == bo->gpu_va)
         k->bos[i].live = false;
   pthread_mutex_unlock(&k->lock);
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
   dump_kick(k, cs, n);
   pthread_mutex_unlock(&k->lock);
   return lost ? -EIO : 0;
}

static int
fake_wait_counter(struct mali_kdev *k, const volatile uint64_t *c, uint64_t target, int timeout_ms)
{
   /* nothing executes: the wait stands for the GPU retiring the frames, so advance the event counter (else hw_retire spins on a full in-flight list) */
   if (*c < target)
      *(volatile uint64_t *)c = target;
   return 0;
}

static int
fake_heap_create(struct mali_kdev *k, uint32_t chunk, uint32_t initial, uint32_t max, struct mali_heap *out)
{
   *out = (struct mali_heap){.ctx_va = 0xf0000000ull, .first_chunk_va = 0xf1000000ull, .chunk_size = chunk};
   pthread_mutex_lock(&k->lock);
   reg_bo(k, out->ctx_va, 4096, 0, 1, NULL);
   reg_bo(k, out->first_chunk_va, chunk ? chunk : 4096, 0, 1, NULL);
   pthread_mutex_unlock(&k->lock);
   return 0;
}

static void
fake_heap_destroy(struct mali_kdev *k, struct mali_heap *heap)
{
}

/* The fake GPU counter is CLOCK_MONOTONIC_RAW in ns (ts_hz stays 0: period 1.0). */
static int
fake_gpu_timestamp(struct mali_kdev *k, uint64_t *ticks)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
   *ticks = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
   return 0;
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
   .gpu_timestamp = fake_gpu_timestamp,
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
