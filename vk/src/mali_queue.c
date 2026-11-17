/* SPDX-License-Identifier: MIT */
/* Queue submit (M4): the command stream only carries a flush and the fence signal; real command
 * buffers arrive with M8. vk_queue runs in DEFERRED mode (emulated timeline semaphores), so this is
 * called from the runtime's submit thread, one submission at a time. */
#include "mali_queue.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kbase_draw.h"
#include "mali_cmd.h"
#include "mali_meta.h"
#include "vk_command_buffer.h"

/* ---- M9: frame resources ---- */
struct mali_hwctx {
   struct mali_device *dev;
   struct mali_bo arena, geo, sync, tls;
   bool tls_live;
   bool arena_live, geo_live, sync_live, heap_live;
   struct kb_arena ar;            /* only bo.{cpu,gpu_va,size} are used: a view of `arena` */
   struct mali_heap heap;
   uint64_t expected;             /* retire counter value after the last submitted frame */
   unsigned wait_ms;              /* MALI_FRAME_TIMEOUT_MS, default 10000 */
   struct mali_bo meta_sh[16];    /* M12: uploaded meta shader binaries, freed at device teardown */
   unsigned n_meta_sh;
};

/* ---- M12/M13 device seam of mali_meta.c: long-lived zeroed memory (one uncached BO each, 64 B header keeps the struct mali_bo) and
 * executable shader memory ---- */
#define META_HDR 64u
_Static_assert(sizeof(struct mali_bo) <= META_HDR, "mali_bo header");

static void *
meta_alloc(void *u, size_t size, size_t align, uint64_t *va)
{
   struct mali_hwctx *h = u;
   struct mali_bo bo;
   if (h->dev && mali_device_pdev(h->dev)->backend->bo_alloc(h->dev->kdev, size + META_HDR, 0, &bo) != 0)
      return NULL;
   memcpy(bo.cpu, &bo, sizeof bo);
   *va = bo.gpu_va + META_HDR;
   return (char *)bo.cpu + META_HDR;
}

static void
meta_free(void *u, void *cpu, uint64_t va, size_t size)
{
   struct mali_hwctx *h = u;
   struct mali_bo bo;
   if (!cpu)
      return;
   memcpy(&bo, (char *)cpu - META_HDR, sizeof bo);
   mali_device_pdev(h->dev)->backend->bo_free(h->dev->kdev, &bo);
}

static int
meta_upload_shader(void *u, const uint8_t *bin, uint32_t size, uint64_t *va)
{
   struct mali_hwctx *h = u;
   if (h->n_meta_sh >= 16)
      return -ENOMEM;
   struct mali_bo *bo = &h->meta_sh[h->n_meta_sh];
   if (mali_device_pdev(h->dev)->backend->bo_alloc(h->dev->kdev, size, MALI_BO_EXEC, bo))
      return -ENOMEM;
   h->n_meta_sh++;
   memcpy(bo->cpu, bin, size);
   *va = bo->gpu_va;
   return 0;
}

#define ARENA_BYTES (32u << 20)   /* per-frame descriptor arena: ~4.5 KB per draw (two 2 KiB driver sets); 1 MiB capped a frame at ~230 draws */
#define HEAP_CHUNK (2u << 20)

static void *
hw_alloc(void *u, size_t n, size_t al, uint64_t *va)
{
   return kb_arena_alloc(&((struct mali_hwctx *)u)->ar, n, al, va);
}

static void
hw_job_begin(void *u)
{
   kb_arena_begin(&((struct mali_hwctx *)u)->ar);
}

static void *
hw_tls(void *u, size_t size, uint64_t *va)
{
   struct mali_hwctx *h = u;
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   if (!h->tls_live || h->tls.size < size) {
      struct mali_bo nb;
      if (be->bo_alloc(h->dev->kdev, size, 0, &nb))
         return NULL;
      if (h->tls_live)
         be->bo_free(h->dev->kdev, &h->tls);   /* frames retire in order: the previous TLS user is finished */
      h->tls = nb;
      h->tls_live = true;
   }
   *va = h->tls.gpu_va;
   return h->tls.cpu;
}

static int
hw_submit(void *u, const uint64_t *cs, unsigned n)
{
   struct mali_hwctx *h = u;
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   int r = be->submit_cs(h->dev->kdev, cs, n);
   if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: submit_cs n=%u arena off=%zx start=%zx rc=%d\n", n, h->ar.off, h->ar.start, r);
   if (r)
      return r;
   /* The arena is rolling and must not hand out the VAs of an in-flight frame (INTERMITTENT.md): every frame retires
    * (CS SYNC_ADD64 on the event word) before the next one is built. */
   h->expected++;
   return be->wait_counter(h->dev->kdev, (volatile uint64_t *)h->sync.cpu, h->expected, h->wait_ms);
}

void
mali_hw_finish(struct mali_device *dev)
{
   struct mali_hwctx *h = dev->hw;
   const struct mali_backend_ops *be = mali_device_pdev(dev)->backend;

   if (!h)
      return;
   mali_cmd_set_hw(NULL);
   mali_cmd_set_res(NULL);
   mali_meta_set_dev(NULL);
   for (unsigned i = 0; i < h->n_meta_sh; i++)
      be->bo_free(dev->kdev, &h->meta_sh[i]);
   if (h->heap_live)
      be->heap_destroy(dev->kdev, &h->heap);
   if (h->tls_live)
      be->bo_free(dev->kdev, &h->tls);
   if (h->arena_live)
      be->bo_free(dev->kdev, &h->arena);
   if (h->geo_live)
      be->bo_free(dev->kdev, &h->geo);
   if (h->sync_live)
      be->bo_free(dev->kdev, &h->sync);
   free(h);
   dev->hw = NULL;
}

VkResult
mali_hw_init(struct mali_device *dev)
{
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   const struct mali_backend_ops *be = pdev->backend;
   struct mali_hwctx *h = calloc(1, sizeof *h);
   const unsigned arch = pdev->gpu.arch_major >= 12 ? 13 : 11;

   if (!h)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   h->dev = dev;
   dev->hw = h;
   const char *t = getenv("MALI_FRAME_TIMEOUT_MS");
   h->wait_ms = t ? (unsigned)strtoul(t, NULL, 0) : 10000;
   if (be->bo_alloc(dev->kdev, ARENA_BYTES, 0, &h->arena))
      goto fail;
   h->arena_live = true;
   if (be->bo_alloc(dev->kdev, 4096 + (4u << 20), 0, &h->geo))
      goto fail;
   h->geo_live = true;
   if (be->bo_alloc(dev->kdev, 4096, MALI_BO_EVENT, &h->sync))
      goto fail;
   h->sync_live = true;
   if (be->heap_create(dev->kdev, HEAP_CHUNK, 4, 64, &h->heap))
      goto fail;
   h->heap_live = true;
   h->ar.bo.cpu = h->arena.cpu;
   h->ar.bo.gpu_va = h->arena.gpu_va;
   h->ar.bo.size = h->arena.size;

   struct mali_hw hw = {.user = h, .alloc = hw_alloc, .job_begin = hw_job_begin, .submit = hw_submit, .tls = hw_tls};
   hw.env = (struct mali_env){.arch = arch,
                              .hmask = arch == 13 ? 0 : 0xfe,
                              .tile_log2 = 12,
                              .slot = 3,
                              .tf_or = arch == 13 ? 0x40000 : 0,
                              .heap_ctx_va = h->heap.ctx_va,
                              .heap_first_chunk_va = h->heap.first_chunk_va,
                              .heap_chunk_size = h->heap.chunk_size,
                              .geo_cpu = h->geo.cpu,
                              .geo_va = h->geo.gpu_va,
                              .sync_va = h->sync.gpu_va,
                              .core_id_range = pdev->gpu.core_id_range,
                              .tls_threads = pdev->gpu.tls_threads};
   if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: va arena %llx+%zx geo %llx sync %llx heap ctx %llx first %llx\n", (unsigned long long)h->arena.gpu_va, (size_t)h->arena.size, (unsigned long long)h->geo.gpu_va, (unsigned long long)h->sync.gpu_va, (unsigned long long)h->heap.ctx_va, (unsigned long long)h->heap.first_chunk_va);
   mali_cmd_set_hw(&hw);
   mali_cmd_set_res(&mali_cmd_res);
   struct mali_meta_dev md = {.user = h, .arch = arch, .alloc = meta_alloc, .free = meta_free, .upload_shader = meta_upload_shader};
   mali_meta_set_dev(&md);
   return VK_SUCCESS;
fail:
   mali_hw_finish(dev);
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

VkResult
mali_queue_device_init(struct mali_device *dev)
{
   pthread_condattr_t ca;

   pthread_mutex_init(&dev->sync_lock, NULL);
   pthread_condattr_init(&ca);
   pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
   pthread_cond_init(&dev->sync_cv, &ca);
   pthread_condattr_destroy(&ca);
   return VK_SUCCESS;
}

void
mali_queue_device_finish(struct mali_device *dev)
{
   pthread_cond_destroy(&dev->sync_cv);
   pthread_mutex_destroy(&dev->sync_lock);
}

static VkResult
mali_queue_submit(struct vk_queue *vq, struct vk_queue_submit *submit)
{
   struct mali_device *dev = container_of(vq->base.device, struct mali_device, vk);
   const struct mali_backend_ops *be = mali_device_pdev(dev)->backend;

   if (vk_queue_is_lost(vq))
      return VK_ERROR_DEVICE_LOST;
   if (vk_queue_submit_has_bind(submit))
      return vk_error(vq, VK_ERROR_FEATURE_NOT_PRESENT); /* sparse: not supported */

   /* Every wait is a CPU wait for now (this queue is the only producer, external sync_files are the
    * rare case); the GPU-side wait is M8 work. */
   for (uint32_t i = 0; i < submit->wait_count; i++) {
      const struct vk_sync_wait *w = &submit->waits[i];
      VkResult r = vk_sync_wait(&dev->vk, w->sync, w->wait_value, VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      if (r != VK_SUCCESS)
         return r;
   }

   /* Recorded frames: each is built, submitted and retired in order (mali_cmd_submit -> hw_submit). */
   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      VkResult cr = mali_cmd_submit(submit->command_buffers[i]);
      if (cr == VK_ERROR_DEVICE_LOST && getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: frame submit lost\n");
      if (cr == VK_ERROR_DEVICE_LOST)
         return vk_queue_set_lost(vq, "frame submit failed");
      if (cr != VK_SUCCESS)
         return vk_error(vq, cr);
   }

   int fd = -1;
   int r = be->submit_signal(dev->kdev, &fd);
   if (r != 0) {
      if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: submit_signal failed: %d\n", r);
      return vk_queue_set_lost(vq, "queue submit failed: %d", r);
   }

   VkResult res = VK_SUCCESS;
   for (uint32_t i = 0; i < submit->signal_count && res == VK_SUCCESS; i++) {
      struct vk_sync *s = submit->signals[i].sync;
      res = vk_sync_import_sync_file(&dev->vk, s, fd);
   }
   close(fd);
   return res;
}

static VkResult
mali_device_check_status(struct vk_device *vk)
{
   struct mali_device *dev = container_of(vk, struct mali_device, vk);

   if (mali_device_pdev(dev)->backend->device_lost(dev->kdev))
      return VK_ERROR_DEVICE_LOST;
   return VK_SUCCESS;
}

void
mali_queue_setup(struct mali_device *dev)
{
   dev->queue.vk.driver_submit = mali_queue_submit;
   dev->vk.check_status = mali_device_check_status;
}
