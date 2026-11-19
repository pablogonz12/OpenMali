/* SPDX-License-Identifier: MIT */
/*
 * libkbase: standalone C11 access to Arm's mali_kbase (CSF) kernel driver.
 * Layers: context (A), memory (B), CSF queue group (C), sync/KCPU (D).
 * No dependency beyond libc. Not thread-safe except kb_event_read()/kb_fence_wait()
 * being callable from one dedicated thread while another submits.
 * All functions return 0 / a positive result on success and -errno on failure;
 * kb_last_error() gives a human-readable message for the last failure on this thread.
 */
#ifndef KBASE_H
#define KBASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kbase_cs.h"
#include "kbase_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *kb_last_error(void);

/* ---------------------------------------------------------------- A: context */
struct kb_ctx;

/* Known kernel interface versions; anything else is refused (kb_open fails with -ENOTSUP). */
struct kb_caps {
	uint16_t major, minor;
	const char *ddk_note;
	bool mem_alloc_ex;        /* MEM_ALLOC_EX (nr 59) */
	bool group_create_40b;    /* 40-byte QUEUE_GROUP_CREATE layout */
	bool kcpu_cqs_operation;  /* CQS_WAIT_OPERATION / CQS_SET_OPERATION KCPU commands */
	bool notif_fatal_queue_16b; /* 16-byte fatal payloads without trace fields */
	bool group_create_112b;   /* try the 112-byte QUEUE_GROUP_CREATE first (falls back to 40 B on ENOTTY) */
	bool tiler_heap_24b;      /* try the 24-byte TILER_HEAP_INIT (buf_desc_va) first (falls back to 16 B on ENOTTY) */
	bool group_create_1_6;    /* uAPI < 1.21 (r32): 32-byte QUEUE_GROUP_CREATE at nr 42 instead of nr 58 */
};

struct kb_gpu_props {
	uint32_t gpu_id, product_id;
	unsigned arch_major, arch_minor, arch_rev, product_major;
	unsigned arch;              /* = arch_major (11 = CMF G615, 13 = Poco G925); gates the v11-only packers, see kbase_fb.h/kbase_draw.h */
	unsigned revision_major, revision_minor;
	uint64_t shader_present, tiler_present, l2_present, stack_present;
	uint32_t l2_features, mem_features, mmu_features, tiler_features, as_present;
	uint32_t max_threads, max_registers, gpu_freq_khz_max, coherency_groups;
	unsigned va_bits, pa_bits;
	/* CSF global interface */
	uint32_t csf_glb_version, csf_features, csf_prfcnt_size;
	uint32_t csf_group_num, csf_total_streams, csf_streams_per_group_min;
	uint32_t csf_group0_suspend_size;
};

struct kb_open_opts {
	const char *path;           /* default "/dev/mali0" */
	uint32_t context_flags;     /* SET_FLAGS create_flags, default 0 (BASE_CONTEXT_CSF_EVENT_THREAD is optional) */
	bool allow_unknown_uapi;    /* research only: continue with the newest known layouts on an unknown version */
};

int kb_open(struct kb_ctx **out, const struct kb_open_opts *opts);  /* NULL opts = defaults */
void kb_close(struct kb_ctx *ctx);                                  /* warns on stderr about leaked objects */
int kb_fd(const struct kb_ctx *ctx);
const struct kb_gpu_props *kb_props(const struct kb_ctx *ctx);
const struct kb_caps *kb_caps(const struct kb_ctx *ctx);
void kb_uapi_version(const struct kb_ctx *ctx, unsigned *major, unsigned *minor);
const char *kb_ddk_version(const struct kb_ctx *ctx);               /* "" if unavailable */
const struct kb_caps *kb_caps_lookup(unsigned major, unsigned minor); /* NULL if unknown */
struct kb_leaks { int bos, queues, kcpu_queues; };
void kb_leaks(const struct kb_ctx *ctx, struct kb_leaks *out);      /* live object counts */

/* ---------------------------------------------------------------- B: memory */
#define KB_BO_CPU_CACHED   (1u << 0) /* cached CPU mapping (needs kb_bo_cache_*; GPU is not IO-coherent) */
#define KB_BO_GPU_RD       (1u << 1)
#define KB_BO_GPU_WR       (1u << 2)
#define KB_BO_GPU_EX       (1u << 3) /* executable (shader code); then no GPU_WR, no SAME_VA */
#define KB_BO_CSF_EVENT    (1u << 4) /* CQS / sync-object memory (kernel-visible, uncached) */
#define KB_BO_GPU_UNCACHED (1u << 5)
#define KB_BO_NOCOH        (1u << 6) /* do not add kb_bo_extra_flags (COHERENT_LOCAL): driver-internal CPU(WC)->GPU data */

struct kb_bo {
	struct kb_ctx *ctx;
	uint64_t gpu_va;
	void *cpu;       /* CPU view. For imported dma-bufs this is the dma-buf's own mmap. */
	size_t size;
	uint32_t flags;
	int dmabuf_fd;   /* -1 for native allocations; a dup owned by the BO for imports */
	void *kmap;      /* kbase mapping that creates the GPU VA of an import. NEVER dereference (SIGBUS). */
	bool same_va;
	bool hot;        /* driver-level: CPU rewrites it every frame (synced on every CS submit, others once per queue submit) */
	bool host_lazy;  /* driver-level: CPU never touched yet (skip MEM_SYNC until bo_host_touch) */
	unsigned track_idx; /* driver-level: 1 + slot in the backend's tracked-BO array (0 = not tracked), makes untracking O(1) */
};

/* ioctl with optional per-request counters: MALI_IOCTL_PROF=1 prints count and time by request (type/nr) at exit */
int kb_ioctl_fd(int fd, unsigned long req, void *arg);
int kb_bo_alloc(struct kb_ctx *ctx, size_t size, uint32_t flags, struct kb_bo *out);
int kb_bo_import_dmabuf(struct kb_ctx *ctx, int dmabuf_fd, size_t size, uint32_t flags, struct kb_bo *out);
void kb_bo_free(struct kb_bo *bo);
bool kb_bo_gpu_va_live(struct kb_ctx *ctx, uint64_t gpu_va);        /* MEM_QUERY: true if the region still exists */
/* Barrier after CPU writes the GPU will read (descriptors, ring): aarch64 `dsb st`, like the kernel's wmb(). Native BOs without
 * KB_BO_CPU_CACHED are Normal-NC (write-combining) mappings; a DMB only orders the writes, a DSB completes them. */
static inline void kb_wmb(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dsb st" ::: "memory");
#else
	__sync_synchronize();
#endif
}
/* Cache maintenance for KB_BO_CPU_CACHED natives: clean+invalidate [off,off+len) (aarch64 `dc civac` from EL0). */
void kb_bo_cache_flush_inv(const struct kb_bo *bo, size_t off, size_t len);
/* DMA_BUF_IOCTL_SYNC around CPU access of an imported dma-buf (needed for cached heaps). */
int kb_bo_dmabuf_sync(const struct kb_bo *bo, bool end, bool write);

enum kb_heap { KB_HEAP_SYSTEM = 0, KB_HEAP_SYSTEM_UNCACHED = 1 };
int kb_dmaheap_alloc(enum kb_heap heap, size_t len);                /* dma-buf fd or -errno */
/* DMA_BUF_IOCTL_EXPORT/IMPORT_SYNC_FILE (flags = KB_DMA_BUF_SYNC_*). Return 0 or -errno. */
int kb_dmabuf_export_sync_file(int dmabuf_fd, uint32_t flags, int *sync_fd);
int kb_dmabuf_import_sync_file(int dmabuf_fd, uint32_t flags, int sync_fd);

/* ---------------------------------------------------------------- C: CSF queue group (1 group, 1 queue) */
struct kb_queue_opts {
	uint32_t ring_size;      /* bytes, power of two >= 4096; default 4096 */
	uint8_t group_priority;  /* BASE_QUEUE_GROUP_PRIORITY_*; default LOW */
};

struct kb_queue {
	struct kb_ctx *ctx;
	struct kb_bo ring;
	uint32_t ring_size;
	uint8_t group_handle;
	uint32_t group_uid;
	void *user_io;                 /* 3 pages: doorbell, input, output */
	volatile uint64_t *cs_insert;  /* input page */
	volatile uint64_t *cs_extract; /* output page */
	volatile uint32_t *cs_active;
	volatile uint32_t *doorbell;
	uint64_t insert;               /* monotonic byte counter (ring address = insert & (size-1)) */
	bool registered, bound, group_live;
	uint64_t kicks, doorbells;
};

enum kb_kick { KB_KICK_IOCTL = 0, KB_KICK_DOORBELL = 1 };

/* GPU timestamp counter frequency in Hz, measured against CLOCK_MONOTONIC_RAW (GET_CPU_GPU_TIMEINFO); 0 if unavailable. */
uint64_t kb_timestamp_hz(struct kb_ctx *ctx);
/* One GET_CPU_GPU_TIMEINFO call: GPU timestamp counter (ticks); 0 or -errno. */
int kb_gpu_timestamp(struct kb_ctx *ctx, uint64_t *ticks);
int kb_queue_create(struct kb_ctx *ctx, const struct kb_queue_opts *opts, struct kb_queue *q);
/* Append instructions (padded with NOPs to 64 bytes), advance CS_INSERT, then kick. -EAGAIN if the ring is full. */
int kb_queue_submit(struct kb_queue *q, const uint64_t *ins, size_t n, enum kb_kick how);
uint64_t kb_queue_extract(const struct kb_queue *q);
bool kb_queue_idle(const struct kb_queue *q);                       /* CS_EXTRACT == CS_INSERT */
bool kb_queue_faulted(const struct kb_queue *q);                    /* a fatal notification was read for this group */
void kb_queue_destroy(struct kb_queue *q);                          /* group term, queue term, unmap, free ring; idempotent */

/* Notifications (read() on the kbase fd). */
struct kb_event {
	int type;                 /* BASE_CSF_NOTIFICATION_* */
	int group;                /* group handle for errors, else -1 */
	int error_type;           /* BASE_GPU_QUEUE_GROUP_* ; -1 if not an error */
	int csi_index;            /* queue index for queue errors, else -1 */
	uint32_t status;          /* exception code in the low byte, data above */
	uint64_t sideband;
	bool fatal;
	char text[200];           /* readable decoding */
};
const struct kb_event *kb_last_fatal(const struct kb_ctx *ctx); /* last fatal notification seen by any reader, NULL if none */
int kb_event_read(struct kb_ctx *ctx, struct kb_event *ev, int timeout_ms); /* 1 event, 0 timeout, <0 error */
const char *kb_exception_name(unsigned code);
unsigned long long kb_events_seen(const struct kb_ctx *ctx);

/* ---------------------------------------------------------------- D: sync */
/* KCPU queue (one per context, created lazily). Commands execute in the kernel in order. */
int kb_kcpu_cqs_wait_gt64(struct kb_ctx *ctx, uint64_t cqs_gpu_va, uint64_t val);   /* wait until *cqs > val (u64) */
int kb_kcpu_cqs_set64(struct kb_ctx *ctx, uint64_t cqs_gpu_va, uint64_t val, bool add);
int kb_kcpu_fence_signal(struct kb_ctx *ctx, int *sync_file_fd);                      /* new sync_file, signals when prior KCPU commands finish */
/* Wait on a sync_file while servicing kbase notifications. 0 = signalled, -ETIMEDOUT, -EIO = group fault seen. */
int kb_fence_wait(struct kb_ctx *ctx, int sync_fd, int timeout_ms);
/* CQS objects are 16 bytes: u64 value at +0, u32 error at +8. They must live in KB_BO_CSF_EVENT memory. */
#define KB_CQS_STRIDE 16

/* ---------------------------------------------------------------- E: tiler heap */
struct kb_tiler_heap {
	struct kb_ctx *ctx;
	uint64_t ctx_va;          /* heap context VA (HEAP_SET operand) */
	uint64_t first_chunk_va;
	uint32_t chunk_size;
	bool live;
};
int kb_tiler_heap_create(struct kb_ctx *ctx, uint32_t chunk_size, uint32_t initial_chunks, uint32_t max_chunks, struct kb_tiler_heap *h);
void kb_tiler_heap_destroy(struct kb_tiler_heap *h);   /* idempotent */

#ifdef __cplusplus
}
#endif
#endif
