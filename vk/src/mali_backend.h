/* SPDX-License-Identifier: MIT */
/* Kernel-side backend seam of the Vulkan front end.
 *
 * M1: probe only (enough for vkEnumeratePhysicalDevices).
 * M2 grows this into a thin wrapper over libkbase (kb_open, kb_bo_*, kb_queue_*, kb_kcpu_*);
 * the "fake" backend then becomes libkbase + its backend_fake.c (see R3-PLAN.md, M2).
 */
#ifndef MALI_BACKEND_H
#define MALI_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mali_gpu_info {
   uint32_t gpu_id;          /* raw GPU_ID: arch major 31:28, arch minor 27:24, product major 19:16 */
   uint8_t arch_major;       /* 11 = Valhall CSF (G615), 13 = fifth gen (G725/G925) */
   uint8_t arch_minor;
   uint16_t core_count;
   uint16_t uapi_major;      /* kbase CSF uAPI */
   uint16_t uapi_minor;
   uint64_t heap_bytes;      /* size advertised for the single memory heap */
   uint64_t ts_hz;           /* GPU timestamp counter frequency (0 = unknown) */
   bool fake;                /* no hardware behind it */
   uint32_t core_id_range;   /* util_last_bit(shader_present): compute TLS sizing */
   uint32_t tls_threads;     /* threads per core that need TLS (max_threads) */
   char name[64];            /* marketing name, e.g. "Mali-G615 MC2" */
};

struct mali_kdev;

/* ---- M4: memory, submit, fences ---- */
#define MALI_BO_CACHED (1u << 0) /* CPU-cached mapping: needs bo_cache() before GPU reads / after GPU writes */
#define MALI_BO_EXPORT (1u << 1) /* dma-heap backed, can be exported as a dma-buf fd */

#define MALI_BO_EVENT  (1u << 2) /* CSF event memory (CQS words the CS updates; kernel-visible, uncached) */
#define MALI_BO_HOSTLAZY (1u << 8) /* device memory the CPU has not mapped yet: no CPU<->device MEM_SYNC until bo_host_touch */
#define MALI_BO_NOCOH  (1u << 10) /* no COHERENT_LOCAL (images that are not storage images); no per-submit MEM_SYNC either */
#define MALI_BO_HOT    (1u << 9) /* rewritten by the CPU every frame (arena): MEM_SYNC at every CS submit */
#define MALI_BO_EXEC   (1u << 3) /* GPU-executable (shader binaries) */

/* M9: tiler heap (kb_tiler_heap) as the CS needs it. */
struct mali_heap {
   uint64_t ctx_va, first_chunk_va;
   uint32_t chunk_size;
   void *priv;
};

struct mali_bo {
   uint64_t gpu_va;
   void *cpu;      /* always CPU mapped */
   size_t size;
   uint32_t flags; /* MALI_BO_* */
   int dmabuf_fd;  /* -1 unless dma-buf backed (owned by the BO) */
   void *priv;
};

struct mali_backend_ops {
   const char *name;
   /* 0 and *out filled when one GPU is usable, negative errno otherwise. */
   int (*probe)(struct mali_gpu_info *out);
   /* One kernel context (fd) plus one command queue per VkDevice. *out may stay NULL for the fake. */
   int (*device_create)(const struct mali_gpu_info *gpu, struct mali_kdev **out);
   void (*device_destroy)(struct mali_kdev *kdev);

   /* BOs. alloc: size is rounded up to the page size; memory is zeroed. import: dup()s the fd. */
   int (*bo_alloc)(struct mali_kdev *kdev, size_t size, uint32_t flags, struct mali_bo *out);
   int (*bo_import)(struct mali_kdev *kdev, int dmabuf_fd, size_t size, uint32_t flags,
                    struct mali_bo *out);
   void (*bo_free)(struct mali_kdev *kdev, struct mali_bo *bo);
   /* Cache maintenance for MALI_BO_CACHED: invalidate = false makes CPU writes visible to the GPU,
    * true makes GPU writes visible to the CPU. */
   void (*bo_cache)(struct mali_kdev *kdev, const struct mali_bo *bo, size_t off, size_t len,
                    bool invalidate);
   /* dup of the dma-buf fd, or -errno. */
   int (*bo_export)(struct mali_kdev *kdev, const struct mali_bo *bo);

   /* One submission whose command stream is only FLUSH_CACHE2(+wait) and a signal of a fresh fence.
    * *fence_fd gets a sync_file that signals when it retires. Serialised by the backend. */
   int (*submit_signal)(struct mali_kdev *kdev, int *fence_fd);
   /* Wait for a sync_file: 0 signalled, -ETIMEDOUT, -EIO = GPU fault (device lost). timeout_ms < 0 =
    * forever. Safe to call from several threads. */
   int (*fence_wait)(struct mali_kdev *kdev, int fence_fd, int timeout_ms);
   bool (*device_lost)(struct mali_kdev *kdev);
   /* VK_KHR_calibrated_timestamps: current GPU timestamp counter in ticks (period = gpu.ts_hz). 0 or -errno. */
   int (*gpu_timestamp)(struct mali_kdev *kdev, uint64_t *ticks);

   /* M9: append a finished frame command stream to the queue. Longer than the 4 KiB ring: it is split into 256-instruction
    * pieces (64-byte multiples), each waiting for ring space (the CS is a linear stream; registers persist across pieces).
    * arch 13: MEM_SYNC of the BOs allocated through bo_alloc first when COHERENT_LOCAL BOs are enabled. 0 or -errno. */
   void (*host_sync)(struct mali_kdev *kdev);   /* CPU -> device MEM_SYNC of every BO (call once per queue submit) */
   void (*bo_host_touch)(struct mali_kdev *kdev, struct mali_bo *bo);
   int (*submit_cs)(struct mali_kdev *kdev, const uint64_t *cs, unsigned ninstr);
   /* Wait until *counter_cpu >= target (a CQS word the CS increments). 0, -ETIMEDOUT, -EIO (GPU fault). */
   int (*wait_counter)(struct mali_kdev *kdev, const volatile uint64_t *counter_cpu, uint64_t target, int timeout_ms);
   int (*heap_create)(struct mali_kdev *kdev, uint32_t chunk_size, uint32_t initial, uint32_t max, struct mali_heap *out);
   void (*heap_destroy)(struct mali_kdev *kdev, struct mali_heap *heap);
};

struct mali_kdev;
extern const struct mali_backend_ops mali_backend_kbase;

extern const struct mali_backend_ops mali_backend_fake;

/* MALI_BACKEND=fake selects the fake backend, anything else the kbase backend (libkbase). */
const struct mali_backend_ops *mali_backend_select(void);

#endif
