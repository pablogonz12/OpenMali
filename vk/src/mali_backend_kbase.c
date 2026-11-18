/* SPDX-License-Identifier: MIT */
/* Real backend over libkbase (driver/libkbase): probe GPU id/arch/cores from kbase_props and own
 * one kernel context plus one CSF queue per VkDevice. */
#include "mali_backend.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "kbase.h"
#include "kbase_cs.h"
#include "kbase_uapi.h"

extern uint64_t kb_bo_extra_flags; /* libkbase bo.c: OR'd into plain GPU allocations */

struct mali_kdev {
   struct kb_ctx *ctx;
   struct kb_queue queue;
   bool queue_live;

   /* M4 submit state */
   pthread_mutex_t submit_lock; /* queue ring, KCPU queue, target counter */
   pthread_mutex_t wait_lock;   /* notification reads (kb_fence_wait) */
   struct kb_bo cqs;            /* CSF_EVENT BO: u64 counter at +0 */
   bool cqs_live;
   uint64_t target;             /* value the counter reaches after the last submit */
   unsigned long submits;
   unsigned long fault_at;      /* MALI_FAULT_INJECT=n: the n-th submit carries an invalid instruction */
   bool lost;
   unsigned dbg;                /* MALI_SUBMIT_DBG bit mask (DBG_*) */

   /* M9: every BO handed out by bo_alloc (MEM_SYNC list for COHERENT_LOCAL on arch 13) */
   pthread_mutex_t bo_lock;
   struct kb_bo **bos;
   unsigned nbos, capbos;
};

/* MALI_SUBMIT_DBG bits (hardware diagnosis, default 0). */
#define DBG_PAD128  0x20u /* 128-byte submissions (ring accounting with another size) */
#define DBG_VERBOSE 0x80u /* on a failed submit print queue state and the ring around CS_EXTRACT */

static bool k_int_nocoh_default;   /* arch 11: driver-internal BOs without COHERENT_LOCAL/MEM_SYNC */
static int int_nocoh = -1;
static const char *
product_name(unsigned arch_major)
{
   switch (arch_major) {
   case 10: return "Mali-G610"; /* Poco X4 GT, arch 10.8 (G710/G510/G310 share the arch; the exact product id is printed by the probe) */
   case 11: return "Mali-G615"; /* CMF Phone 1, arch 11.8 */
   case 13: return "Mali-G925"; /* Poco X8 Pro Max, arch 13.8 */
   default: return "Mali-unknown";
   }
}

static int
kbase_probe(struct mali_gpu_info *out)
{
   struct kb_ctx *ctx = NULL;
   int r = kb_open(&ctx, NULL);

   if (r != 0)
      return r < 0 ? r : -ENODEV;

   const struct kb_gpu_props *p = kb_props(ctx);
   unsigned major, minor;
   kb_uapi_version(ctx, &major, &minor);

   memset(out, 0, sizeof(*out));
   out->gpu_id = p->gpu_id;
   out->arch_major = p->arch_major;
   out->arch_minor = p->arch_minor;
   out->core_count = __builtin_popcountll(p->shader_present);
   out->core_id_range = p->shader_present ? 64 - __builtin_clzll(p->shader_present) : 1;
   out->tls_threads = p->max_threads ? p->max_threads : 2048;
   out->ts_hz = kb_timestamp_hz(ctx);
   out->uapi_major = major;
   out->uapi_minor = minor;

   long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGE_SIZE);
   out->heap_bytes = pages > 0 && psz > 0 ? (uint64_t)pages * psz / 2 : 1ull << 30;
   snprintf(out->name, sizeof(out->name), "%s MC%u", product_name(p->arch_major), out->core_count);

   kb_close(ctx); /* the probe holds no fd after returning */
   return 0;
}

static int
kbase_device_create(const struct mali_gpu_info *gpu, struct mali_kdev **out)
{
   struct mali_kdev *k = calloc(1, sizeof(*k));
   if (!k)
      return -ENOMEM;

   int r = kb_open(&k->ctx, NULL);
   if (r != 0) {
      free(k);
      return r < 0 ? r : -ENODEV;
   }

   const char *dbg = getenv("MALI_SUBMIT_DBG");
   k->dbg = dbg ? (unsigned)strtoul(dbg, NULL, 0) : 0;

   struct kb_queue_opts qo = {.ring_size = getenv("MALI_RING") ? (uint32_t)atoi(getenv("MALI_RING")) : 1u << 20};   /* one frame record must fit: 4 KiB capped a frame at ~250 draws (mdbench drawcalls:300 = DEVICE_LOST) */
   r = kb_queue_create(k->ctx, &qo, &k->queue);
   if (r != 0) {
      kb_close(k->ctx);
      free(k);
      return r < 0 ? r : -EIO;
   }
   k->queue_live = true;

   /* The v13 bisect found plain allocations fine; COHERENT_LOCAL (0x800, what PanVK sets on every BO)
    * stays an opt-in for experiments. */
   /* M11: v11 compute needs it: without it SSBO atomics are done in each core's L1 and updates from different cores are lost
    * (CMF, 2 cores: atomicAdd count ~half). So it is the default on arch 11; MALI_BO_COHERENT_LOCAL=0/1 overrides on any arch. */
   const char *cl = getenv("MALI_BO_COHERENT_LOCAL");
   k_int_nocoh_default = gpu->arch_major == 11;
   if (cl ? cl[0] == '1' : gpu->arch_major == 10 || gpu->arch_major == 11 || gpu->arch_major == 13)
      kb_bo_extra_flags |= 0x800;
   const char *fi = getenv("MALI_FAULT_INJECT");
   k->fault_at = fi ? strtoul(fi, NULL, 10) : 0;
   pthread_mutex_init(&k->submit_lock, NULL);
   pthread_mutex_init(&k->wait_lock, NULL);
   pthread_mutex_init(&k->bo_lock, NULL);

   r = kb_bo_alloc(k->ctx, 4096, KB_BO_CSF_EVENT | KB_BO_GPU_RD | KB_BO_GPU_WR, &k->cqs);
   if (r != 0) {
      kb_queue_destroy(&k->queue);
      kb_close(k->ctx);
      free(k);
      return r < 0 ? r : -EIO;
   }
   k->cqs_live = true;
   *(volatile uint64_t *)k->cqs.cpu = 0;
   *out = k;
   return 0;
}

static void
kbase_device_destroy(struct mali_kdev *k)
{
   if (!k)
      return;
   if (k->cqs_live)
      kb_bo_free(&k->cqs);
   if (k->queue_live)
      kb_queue_destroy(&k->queue);
   kb_close(k->ctx);
   pthread_mutex_destroy(&k->submit_lock);
   pthread_mutex_destroy(&k->wait_lock);
   pthread_mutex_destroy(&k->bo_lock);
   free(k->bos);
   free(k);
}

static void
track_bo(struct mali_kdev *k, struct kb_bo *bo, bool add)
{
   pthread_mutex_lock(&k->bo_lock);
   if (add) {
      if (k->nbos == k->capbos) {
         unsigned nc = k->capbos ? k->capbos * 2 : 64;
         struct kb_bo **nb = realloc(k->bos, nc * sizeof *nb);
         if (!nb) {
            pthread_mutex_unlock(&k->bo_lock);
            return;
         }
         k->bos = nb;
         k->capbos = nc;
      }
      k->bos[k->nbos++] = bo;
   } else {
      for (unsigned i = 0; i < k->nbos; i++)
         if (k->bos[i] == bo) {
            k->bos[i] = k->bos[--k->nbos];
            break;
         }
   }
   pthread_mutex_unlock(&k->bo_lock);
}

static int
kbase_bo_alloc(struct mali_kdev *k, size_t size, uint32_t flags, struct mali_bo *out)
{
   struct kb_bo *bo = calloc(1, sizeof(*bo));
   if (!bo)
      return -ENOMEM;
   size = (size + 4095) & ~(size_t)4095;

   /* MALI_INT_NOCOH=1 (experiment): driver-internal BOs (arena, descriptors, CS, heaps: no HOSTLAZY/EXPORT, i.e. not VkDeviceMemory) without COHERENT_LOCAL.
    * Only VkDeviceMemory (SSBO atomics from the app) needs the cross-core coherency; internal data is written by the CPU (WC) and read by the GPU. */
   if (int_nocoh < 0) {
      const char *ie = getenv("MALI_INT_NOCOH");   /* default: on for arch 11 only (v10/v13 keep it: the v13 tiler bins nothing without it); =0/1 overrides */
      int_nocoh = ie ? ie[0] == '1' : k_int_nocoh_default;
   }
   if (int_nocoh && !(flags & (MALI_BO_EXPORT | MALI_BO_HOSTLAZY | MALI_BO_EVENT | MALI_BO_EXEC | MALI_BO_CACHED)))
      flags |= MALI_BO_NOCOH;
   /* Why this is safe: CPU-side these BOs are write-combined (not CPU_CACHED), so kbase_sync_now() is a no-op for them (it returns early for
    * regions without KBASE_REG_CPU_CACHED); the WC buffer drains before the doorbell, and every frame CS starts with FLUSH_CACHE2 + WAIT. */

   int r;
   if (flags & MALI_BO_EXPORT) {
      /* dma-heap buffer imported into the GPU: the heap decides cacheability (system = cached,
       * system-uncached = write-combined). */
      int fd = kb_dmaheap_alloc((flags & MALI_BO_CACHED) ? KB_HEAP_SYSTEM : KB_HEAP_SYSTEM_UNCACHED, size);
      if (fd < 0) {
         free(bo);
         return fd;
      }
      r = kb_bo_import_dmabuf(k->ctx, fd, size, KB_BO_GPU_RD | KB_BO_GPU_WR, bo);
      close(fd);
   } else if (flags & MALI_BO_EVENT) {
      r = kb_bo_alloc(k->ctx, size, KB_BO_CSF_EVENT | KB_BO_GPU_RD | KB_BO_GPU_WR, bo);
   } else if (flags & MALI_BO_EXEC) {
      r = kb_bo_alloc(k->ctx, size, KB_BO_GPU_RD | KB_BO_GPU_EX, bo);
   } else {
      r = kb_bo_alloc(k->ctx, size,
                      KB_BO_GPU_RD | KB_BO_GPU_WR | ((flags & MALI_BO_CACHED) ? KB_BO_CPU_CACHED : 0) | ((flags & MALI_BO_NOCOH) ? KB_BO_NOCOH : 0), bo);
   }
   if (r != 0) {
      free(bo);
      return r < 0 ? r : -ENOMEM;
   }
   if ((flags & MALI_BO_CACHED) && !(flags & MALI_BO_EXPORT)) {
      /* kbase hands CPU-cached pages back from its pool without clearing them (seen on the CMF: old
       * contents, also after dc civac). Zero them and write the zeroes back so the GPU sees them too. */
      memset(bo->cpu, 0, size);
      kb_bo_cache_flush_inv(bo, 0, size);
   }
   *out = (struct mali_bo){.gpu_va = bo->gpu_va, .cpu = bo->cpu, .size = size, .flags = flags,
                           .dmabuf_fd = bo->dmabuf_fd, .priv = bo};
   /* Exported VkDeviceMemory (swapchain images) is lazy like every other VkDeviceMemory: the GPU writes it and nothing on the CPU has touched it
    * until vkMapMemory (bo_host_touch). Before this, 4 swapchain dma-bufs were MEM_SYNCed (a real cache op on an imported dma-buf) at every submit. */
   bo->host_lazy = (flags & MALI_BO_HOSTLAZY) || ((flags & MALI_BO_EXPORT) && !(flags & MALI_BO_CACHED));
   bo->hot = (flags & MALI_BO_HOT) != 0;
   track_bo(k, bo, true);
   return 0;
}

static void
kbase_bo_host_touch(struct mali_kdev *k, struct mali_bo *b)
{
   if (b && b->priv)
      ((struct kb_bo *)b->priv)->host_lazy = false;
}

static int
kbase_bo_import(struct mali_kdev *k, int dmabuf_fd, size_t size, uint32_t flags, struct mali_bo *out)
{
   struct kb_bo *bo = calloc(1, sizeof(*bo));
   if (!bo)
      return -ENOMEM;
   size = (size + 4095) & ~(size_t)4095;
   int r = kb_bo_import_dmabuf(k->ctx, dmabuf_fd, size, KB_BO_GPU_RD | KB_BO_GPU_WR, bo);
   if (r != 0) {
      free(bo);
      return r < 0 ? r : -EINVAL;
   }
   *out = (struct mali_bo){.gpu_va = bo->gpu_va, .cpu = bo->cpu, .size = size,
                           .flags = flags | MALI_BO_EXPORT, .dmabuf_fd = bo->dmabuf_fd, .priv = bo};
   return 0;
}

static void
kbase_bo_free(struct mali_kdev *k, struct mali_bo *bo)
{
   if (!bo->priv)
      return;
   track_bo(k, bo->priv, false);
   kb_bo_free(bo->priv);
   free(bo->priv);
   bo->priv = NULL;
}

static void
kbase_bo_cache(struct mali_kdev *k, const struct mali_bo *bo, size_t off, size_t len, bool invalidate)
{
   if (!(bo->flags & MALI_BO_CACHED) || off >= bo->size)
      return;
   if (len > bo->size - off)
      len = bo->size - off;
   if (bo->flags & MALI_BO_EXPORT)
      kb_bo_dmabuf_sync(bo->priv, !invalidate /* end after CPU writes, start before CPU reads */,
                        !invalidate);
   else
      kb_bo_cache_flush_inv(bo->priv, off, len); /* dc civac works in both directions */
}

static int
kbase_bo_export(struct mali_kdev *k, const struct mali_bo *bo)
{
   if (bo->dmabuf_fd < 0)
      return -EINVAL;
   int fd = fcntl(bo->dmabuf_fd, F_DUPFD_CLOEXEC, 3);
   return fd < 0 ? -errno : fd;
}

/* Wait until `bytes` fit while one 64-byte slot stays free. The ring must never be completely full:
 * with CS_INSERT - CS_EXTRACT == size the firmware raises CS_CONFIG_FAULT (status 0x40, sideband 0)
 * as soon as it looks at the queue (CMF, arch 11; R3-PLAN.md M4). A 4096-byte ring therefore holds
 * 63 empty submissions. */
static int
wait_ring_space(struct mali_kdev *k, size_t bytes)
{
   for (int i = 0; i < 200000; i++) { /* ~10 s */
      if (pthread_mutex_trylock(&k->wait_lock) == 0) {
         struct kb_event ev;
         while (kb_event_read(k->ctx, &ev, 0) > 0)
            if (ev.fatal || getenv("MALI_PIPE_DBG"))
               fprintf(stderr, "mali_vk: %sevent: %s\n", ev.fatal ? "FATAL " : "", ev.text);
         pthread_mutex_unlock(&k->wait_lock);
      }
      if (kb_queue_faulted(&k->queue))
         return -EIO;
      if (k->queue.insert - kb_queue_extract(&k->queue) + bytes + 64 <= k->queue.ring_size)
         return 0;
      usleep(50);
   }
   return -ETIMEDOUT;
}

static int
kbase_submit_signal(struct mali_kdev *k, int *fence_fd)
{
   pthread_mutex_lock(&k->submit_lock);
   int r = 0, fd = -1;

   if (k->lost || kb_queue_faulted(&k->queue)) {
      r = -EIO;
      goto out;
   }
   uint64_t target = k->target + 1;
   struct kb_cs_buf b = {.n = 0};
   if (k->fault_at && k->submits + 1 == k->fault_at) {
      kb_cs_emit(&b, kb_cs_invalid());
   } else {
      /* Head: make earlier GPU writes (and host writes already barriered by kb_wmb) coherent; the flush
       * is asynchronous, so WAIT on its slot before anything depends on it (INTERMITTENT.md). */
      kb_cs_emit_flush_wait(&b, 1, 1, 0, 80);
      kb_cs_emit(&b, kb_cs_move48(68, k->cqs.gpu_va));
      kb_cs_emit(&b, kb_cs_move48(70, 1));
      kb_cs_emit(&b, kb_cs_sync_add64(68, 70, KB_CS_SCOPE_SYSTEM)); /* counter += 1 */
      kb_cs_emit(&b, kb_cs_wait(1u << KB_CS_SLOT_LS)); /* sync_add is asynchronous too */
      while ((k->dbg & DBG_PAD128) && b.n < 16)
         kb_cs_emit(&b, kb_cs_nop());
   }
   if ((r = wait_ring_space(k, ((size_t)b.n * 8 + 63) & ~(size_t)63)))
      goto out;

   /* KCPU first (as t_submit): fence signals once the counter exceeds target - 1. */
   if ((r = kb_kcpu_cqs_wait_gt64(k->ctx, k->cqs.gpu_va, target - 1)) ||
       (r = kb_kcpu_fence_signal(k->ctx, &fd)))
      goto out;
   if ((r = kb_queue_submit(&k->queue, b.ins, b.n, KB_KICK_IOCTL))) {
      close(fd);
      fd = -1;
      goto out;
   }
   k->target = target;
   k->submits++;
   *fence_fd = fd;
out:
   if (r) {
      fprintf(stderr, "mali_vk: submit #%llu failed: %d (extract=%llu insert=%llu faulted=%d)\n",
              (unsigned long long)k->submits + 1, r, (unsigned long long)kb_queue_extract(&k->queue),
              (unsigned long long)k->queue.insert, (int)kb_queue_faulted(&k->queue));
      if (k->dbg & DBG_VERBOSE) {
         uint64_t ext = kb_queue_extract(&k->queue);
         const uint64_t *ring = k->queue.ring.cpu;
         fprintf(stderr, "mali_vk: dbg=0x%x active=%u kicks=%llu doorbells=%llu cqs=%llu target=%llu\n", k->dbg,
                 (unsigned)*k->queue.cs_active, (unsigned long long)k->queue.kicks,
                 (unsigned long long)k->queue.doorbells,
                 (unsigned long long)*(volatile uint64_t *)k->cqs.cpu, (unsigned long long)k->target);
         for (uint64_t o = ext >= 64 ? (ext & ~63ull) - 64 : 0; o < (ext & ~63ull) + 128 && o < k->queue.insert; o += 8)
            fprintf(stderr, "mali_vk: ring[%4llu]%s %016llx\n", (unsigned long long)o, o == ext ? "*" : " ",
                    (unsigned long long)ring[(o & (k->queue.ring_size - 1)) / 8]);
      }
      k->lost = true;
   }
   pthread_mutex_unlock(&k->submit_lock);
   return r;
}

static void mem_sync_all(struct mali_kdev *k, unsigned op);
static void mem_sync_bos(struct mali_kdev *k, unsigned op, bool hot_only);
static int
kbase_fence_wait(struct mali_kdev *k, int fence_fd, int timeout_ms)
{
   /* kb_fence_wait reads kbase notifications from the shared fd, so only one thread at a time; slice
    * the wait so that others (and the timeout) get their turn. */
   struct timespec t0;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (;;) {
      struct timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      long long el = (t1.tv_sec - t0.tv_sec) * 1000ll + (t1.tv_nsec - t0.tv_nsec) / 1000000;
      long long left = timeout_ms < 0 ? 1000000 : timeout_ms - el;
      if (left < 0)
         left = 0;
      int slice = left > 20 ? 20 : (int)left;
      pthread_mutex_lock(&k->wait_lock);
      int r = kb_fence_wait(k->ctx, fence_fd, slice);
      pthread_mutex_unlock(&k->wait_lock);
      if (r == 0) {
         mem_sync_all(k, 2); /* device -> CPU (CSYNC): CPU caches may hold stale lines of COHERENT_LOCAL BOs */
         return 0;
      }
      if (r == -EIO) {
         k->lost = true;
         return -EIO;
      }
      if (r == -ETIMEDOUT && kb_queue_faulted(&k->queue)) {
         k->lost = true; /* another thread read the fatal notification */
         return -EIO;
      }
      if (r != -ETIMEDOUT)
         return r;
      if (timeout_ms >= 0 && left <= slice)
         return -ETIMEDOUT;
   }
}

static bool
kbase_device_lost(struct mali_kdev *k)
{
   bool l = k->lost || (k->queue_live && kb_queue_faulted(&k->queue));
   if (l && getenv("MALI_PIPE_DBG")) {
      struct kb_event ev;
      fprintf(stderr, "mali_vk: device lost (lost=%d faulted=%d)\n", (int)k->lost, (int)(k->queue_live && kb_queue_faulted(&k->queue)));
      while (kb_event_read(k->ctx, &ev, 0) > 0)
         fprintf(stderr, "mali_vk: event: %s\n", ev.text);
   }
   return l;
}

/* ---- M9: frame command streams, counter wait, tiler heap ---- */
#define CS_PIECE 256u /* instructions per ring write: 2 KiB, a multiple of 64 bytes */

static void
mem_sync_all(struct mali_kdev *k, unsigned op)
{
   mem_sync_bos(k, op, false);
}

static void
mem_sync_bos(struct mali_kdev *k, unsigned op, bool hot_only)
{
   /* COHERENT_LOCAL BOs (arch 13, MALI_BO_COHERENT_LOCAL=1): CPU -> device MEM_SYNC before every submit (V13-NEXT2.md). */
   if (!(kb_bo_extra_flags & 0x800))
      return;
   pthread_mutex_lock(&k->bo_lock);
   static unsigned prof_calls;
   if (getenv("MALI_PROF") && ++prof_calls == 600) {
      unsigned n = 0;
      for (unsigned i = 0; i < k->nbos; i++) if (!k->bos[i]->host_lazy && !(k->bos[i]->flags & KB_BO_NOCOH)) { n++; fprintf(stderr, "PROF/memsync bo size %zu flags %x hot %d dmabuf %d\n", k->bos[i]->size, k->bos[i]->flags, k->bos[i]->hot, k->bos[i]->dmabuf_fd >= 0); }
      fprintf(stderr, "PROF/memsync nbos=%u non-lazy=%u\n", k->nbos, n);
   }
   for (unsigned i = 0; i < k->nbos; i++) {
      struct kb_bo *b = k->bos[i];
      if (b->host_lazy || (b->flags & KB_BO_NOCOH) || (hot_only && !b->hot))
         continue;
      struct {
         uint64_t handle, user_addr, size;
         uint8_t type, pad[7];
      } ms = {b->gpu_va, (uint64_t)(uintptr_t)b->cpu, b->size, (uint8_t)op, {0}};
      kb_ioctl_fd(kb_fd(k->ctx), _IOW(KBASE_IOCTL_TYPE, 15, ms), &ms);
   }
   pthread_mutex_unlock(&k->bo_lock);
}

static void
kbase_host_sync(struct mali_kdev *k)
{
   mem_sync_bos(k, 1, false);
}

static int
kbase_submit_cs(struct mali_kdev *k, const uint64_t *cs, unsigned n)
{
   pthread_mutex_lock(&k->submit_lock);
   int r = 0;

   if (k->lost || kb_queue_faulted(&k->queue)) {
      r = -EIO;
      goto out;
   }
   mem_sync_bos(k, 1, true);   /* the other BOs were synced by host_sync at the start of the queue submit */
   for (unsigned i = 0; i < n && !r; i += CS_PIECE) {
      unsigned m = n - i < CS_PIECE ? n - i : CS_PIECE;
      if ((r = wait_ring_space(k, ((size_t)m * 8 + 63) & ~(size_t)63)))
         break;
      r = kb_queue_submit(&k->queue, cs + i, m, KB_KICK_IOCTL);
   }
   if (r) {
      fprintf(stderr, "mali_vk: CS submit failed: %d (extract=%llu insert=%llu)\n", r,
              (unsigned long long)kb_queue_extract(&k->queue), (unsigned long long)k->queue.insert);
      k->lost = true;
   }
out:
   pthread_mutex_unlock(&k->submit_lock);
   return r;
}

static int
kbase_wait_counter(struct mali_kdev *k, const volatile uint64_t *c, uint64_t target, int timeout_ms)
{
   struct timespec t0;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (unsigned spin = 0;; spin++) {
      if (*c >= target) {
         mem_sync_all(k, 2);
         return 0;
      }
      if (pthread_mutex_trylock(&k->wait_lock) == 0) {
         struct kb_event ev;
         while (kb_event_read(k->ctx, &ev, 0) > 0)
            if (ev.fatal)
               fprintf(stderr, "mali_vk: %s\n", ev.text);
         pthread_mutex_unlock(&k->wait_lock);
      }
      if (k->lost || kb_queue_faulted(&k->queue)) {
         k->lost = true;
         return -EIO;
      }
      if (spin > 100)
         usleep(50);
      struct timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      if (timeout_ms >= 0 && (t1.tv_sec - t0.tv_sec) * 1000ll + (t1.tv_nsec - t0.tv_nsec) / 1000000 > timeout_ms)
         return *c >= target ? 0 : -ETIMEDOUT;
   }
}

static int
kbase_heap_create(struct mali_kdev *k, uint32_t chunk, uint32_t initial, uint32_t max, struct mali_heap *out)
{
   struct kb_tiler_heap *h = calloc(1, sizeof *h);
   if (!h)
      return -ENOMEM;
   int r = kb_tiler_heap_create(k->ctx, chunk, initial, max, h);
   if (r) {
      free(h);
      return r < 0 ? r : -EIO;
   }
   *out = (struct mali_heap){.ctx_va = h->ctx_va, .first_chunk_va = h->first_chunk_va, .chunk_size = h->chunk_size, .priv = h};
   return 0;
}

static void
kbase_heap_destroy(struct mali_kdev *k, struct mali_heap *heap)
{
   if (!heap->priv)
      return;
   kb_tiler_heap_destroy(heap->priv);
   free(heap->priv);
   heap->priv = NULL;
}

static int
kbase_gpu_timestamp(struct mali_kdev *k, uint64_t *ticks)
{
   return kb_gpu_timestamp(k->ctx, ticks);
}

const struct mali_backend_ops mali_backend_kbase = {
   .name = "kbase",
   .probe = kbase_probe,
   .device_create = kbase_device_create,
   .device_destroy = kbase_device_destroy,
   .bo_alloc = kbase_bo_alloc,
   .bo_import = kbase_bo_import,
   .bo_free = kbase_bo_free,
   .bo_cache = kbase_bo_cache,
   .bo_export = kbase_bo_export,
   .submit_signal = kbase_submit_signal,
   .fence_wait = kbase_fence_wait,
   .device_lost = kbase_device_lost,
   .gpu_timestamp = kbase_gpu_timestamp,
   .host_sync = kbase_host_sync,
   .bo_host_touch = kbase_bo_host_touch,
   .submit_cs = kbase_submit_cs,
   .wait_counter = kbase_wait_counter,
   .heap_create = kbase_heap_create,
   .heap_destroy = kbase_heap_destroy,
};
