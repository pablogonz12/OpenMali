/* SPDX-License-Identifier: MIT */
/* Targeted implicit sync for dma-bufs shared outside a swapchain (zink/DRI3 back buffers, Chromium's shared images).
 *
 * kbase attaches no implicit fence to its dma-bufs (mali_queue.c), so a consumer process can sample a shared buffer
 * while the producer's GPU work is still writing it: the X4's Steam menu flickers (7 flicker frames in 20 s, 0 with
 * MALI_SYNC_SUBMIT=1) and the X8's menu can come up black. The WSI swapchains already carry their own fence
 * (mali_present_fence_ok), so only buffers shared *outside* a swapchain are handled here.
 *
 * Producer, per queue submit that writes such a buffer:
 *   - kernel has DMA_BUF_IOCTL_IMPORT_SYNC_FILE (>= 6.0, or a 5.10 with the backport): attach the submit's own
 *     sync_file to every such dma-buf as a WRITE fence, so any consumer's DMA_BUF_IOCTL_SYNC / read fence wait sees it;
 *   - kernel answers ENOTTY (Poco X4, 5.10): no implicit fences exist at all, so the submit CPU-waits for its own
 *     fence before returning - MALI_SYNC_SUBMIT for the submits that touch a shared buffer, and nothing else.
 * Consumer, before a submit that references an imported dma-buf: export its current fences (READ) and wait them.
 * On 5.10 the producer's CPU wait is what covers the consumer, and the export ioctl is not even attempted (ENOTTY).
 *
 * MALI_IMPLICIT_SYNC=0 turns all of it off. MALI_ISYNC_DBG=1 prints one line per device with the counts.
 *
 * What a submit references: the producer side uses the per-command-buffer GPU-write marks (mali_cmd.h wrote[]).
 * The consumer side uses the read marks (mali_cmd.h read[]): imported dma-bufs reached through a bound descriptor
 * set (sampled/storage/input attachment, UBO/SSBO, texel buffer), a copy/blit source or a vertex/index buffer.
 * Only imported BOs are recorded, so the cost is O(imported dma-bufs actually bound), not O(buffers in the frame).
 * A command buffer whose write list overflowed (wrote_full) cannot name its buffers: its submit takes the CPU wait,
 * which is correct for any of them. */
#ifndef MALI_ISYNC_H
#define MALI_ISYNC_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "mali_backend.h"

/* What one submit must do; also the return of the plan functions below (pure, tests/t_isync.c drives them). */
enum mali_isync_action {
   MALI_ISYNC_NONE = 0,     /* nothing shared, or MALI_IMPLICIT_SYNC=0 */
   MALI_ISYNC_FENCE,        /* producer: attach the submit's fence; consumer: wait the buffer's fences */
   MALI_ISYNC_CPU_WAIT,     /* producer only: this kernel cannot take a fence, wait the submit out */
};

/* struct mali_memory.isync (mali_queue.h): how this BO is shared. */
#define MALI_ISYNC_BO_NONE     0
#define MALI_ISYNC_BO_EXPORTED 1 /* vkGetMemoryFdKHR handed its dma-buf fd out: we are the producer */
#define MALI_ISYNC_BO_IMPORTED 2 /* allocated from somebody else's dma-buf: we may be a consumer */
#define MALI_ISYNC_BO_WSI      3 /* swapchain image: wsi_common imports the present fence itself, leave it alone */

struct mali_isync_dev {
   pthread_mutex_t lock;   /* guards the probes and the counters (submits come from several threads) */
   int enabled;            /* MALI_IMPLICIT_SYNC, read once: -1 unread, 0 off, 1 on */
   int import_ok;          /* -1 unknown, 0 ENOTTY (no implicit fences here), 1 the kernel took a fence */
   int export_ok;          /* -1 unknown, 0 ENOTTY, 1 the kernel handed one out */
   unsigned stat_fence, stat_cpu_wait, stat_wait, stat_err;
   unsigned stat_polled, stat_blocked;   /* consumer: fence already signalled (0-timeout poll) vs a real wait */
};

void mali_isync_dev_init(struct mali_isync_dev *d);
void mali_isync_dev_finish(struct mali_isync_dev *d);
bool mali_isync_enabled(struct mali_isync_dev *d);

/* Pure decisions. nfds = shared dma-bufs this submit names, unlisted = it may write more than it names. */
enum mali_isync_action mali_isync_plan_producer(const struct mali_isync_dev *d, unsigned nfds, bool unlisted);
enum mali_isync_action mali_isync_plan_consumer(const struct mali_isync_dev *d, unsigned nfds);

/* One submit. fds[] are the shared dma-bufs it references (producer: written by it, consumer: imported by it).
 * Producer runs after the submit's fence exists and waits it (CPU_WAIT) or attaches it (FENCE). */
enum mali_isync_action mali_isync_producer_ex(struct mali_isync_dev *d, const struct mali_backend_ops *be,
                                             struct mali_kdev *kdev, const int *fds, unsigned nfds, bool unlisted,
                                             int fence_fd, int *wait_rc);
/* dir is MALI_DMA_BUF_SYNC_READ (this submit reads the buffer) or MALI_DMA_BUF_SYNC_WRITE (it writes it: the
 * export then also returns the current readers' fences). Returns 0, or -errno: the caller must not submit. */
int mali_isync_consumer_ex(struct mali_isync_dev *d, const struct mali_backend_ops *be, struct mali_kdev *kdev,
                           const int *fds, unsigned nfds, unsigned dir);

/* Convenience forms without the status out-params (tests, callers that cannot act on a failure). */
static inline enum mali_isync_action
mali_isync_producer(struct mali_isync_dev *d, const struct mali_backend_ops *be, struct mali_kdev *kdev,
                    const int *fds, unsigned nfds, bool unlisted, int fence_fd)
{
   return mali_isync_producer_ex(d, be, kdev, fds, nfds, unlisted, fence_fd, NULL);
}

static inline void
mali_isync_consumer(struct mali_isync_dev *d, const struct mali_backend_ops *be, struct mali_kdev *kdev,
                    const int *fds, unsigned nfds)
{
   (void)mali_isync_consumer_ex(d, be, kdev, fds, nfds, MALI_DMA_BUF_SYNC_READ);
}

#endif
