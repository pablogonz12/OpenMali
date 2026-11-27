/* SPDX-License-Identifier: MIT */
/* Cross-process fences for dma-bufs on a kernel without DMA_BUF_IOCTL_{IMPORT,EXPORT}_SYNC_FILE (Poco X4, 5.10).
 *
 * Both ends of the Steam UI's frame are OUR driver (Chromium's zink produces it, Xwayland's glamor/zink samples it
 * as a texture), so the fence can travel through a file only we know about instead of through kbase:
 *
 *   producer   on a submit that hands a dma-buf off: increment `pending` in the buffer's shared page and remember
 *              the submit's sync_file. At every following submit it POLLS those remembered fences (timeout 0) and,
 *              when one is signalled, sets `done` and futex-wakes. No blocking, no thread: the producer keeps
 *              overlapping CPU and GPU, which is exactly what #404's producer-side CPU wait destroyed (X4: 26 fps
 *              instead of 52) and its consumer never had a fence to wait on.
 *   consumer   before a submit that READS an imported dma-buf: if `pending != done`, wait on `done` (futex, hard
 *              timeout, default 100 ms). A producer that died mid-frame leaves pending > done, so the wait times out
 *              and the submit proceeds: stale state is a count, never a hang.
 *
 * The rendezvous is one page per (device, inode): dma-buf fstat st_dev/st_ino are the same in every process that
 * imported the same buffer, so the name is unguessable beyond that pair and needs no daemon. The directory is
 * $XDG_RUNTIME_DIR, else /dev/shm, else /tmp (mode 0600). Files are created by whoever needs them and unlinked by
 * their owner at device destroy; a page still mapped by another process keeps working (it just stops being reachable
 * by new openers, who start a fresh one at seqno 0, which is consistent for both ends).
 *
 * No thread and no fd outlives the device: every fence is a dup owned by this struct, closed in mali_xsync_dev_finish
 * together with the mappings, so vkDestroyDevice -> dlclose is safe (a detached thread segfaulted the Steam client on
 * 2026-11-27). */
#ifndef MALI_XSYNC_H
#define MALI_XSYNC_H

#include <stdbool.h>
#include <stdint.h>

struct mali_xsync;
struct mali_xsync_ops {
   /* Same contract as mali_backend_ops.fence_wait: 0 signalled, -ETIMEDOUT, -EIO. timeout 0 is a poll. */
   int (*fence_wait)(void *kdev, int fence_fd, int timeout_ms);
};

/* 0 and *out set, or -errno (no writable directory: the mechanism is simply absent). */
int mali_xsync_dev_init(struct mali_xsync **out, const char *dir);
void mali_xsync_dev_finish(struct mali_xsync *x);

/* Producer: the buffer behind dmabuf_fd is given away by a submit whose completion is fence_fd (dup'd here). */
void mali_xsync_publish(struct mali_xsync *x, const struct mali_xsync_ops *ops, void *kdev, int dmabuf_fd, int fence_fd);
/* Producer: mark the published writes that have completed. Never blocks. */
void mali_xsync_observe(struct mali_xsync *x, const struct mali_xsync_ops *ops, void *kdev);
/* Consumer: wait for the published write of dmabuf_fd to complete, at most timeout_ms. *timeouts counts the waits
 * that ran out (a dead or slow producer). Returns true when it had to wait. */
bool mali_xsync_await(struct mali_xsync *x, int dmabuf_fd, int timeout_ms, unsigned *timeouts);
/* Counters for MALI_ISYNC_DBG / tests. */
struct mali_xsync_stats { unsigned published, observed, awaited, polled, timeouts; };
void mali_xsync_stats(struct mali_xsync *x, struct mali_xsync_stats *out);

/* The rendezvous file name for an fd (-errno if it cannot be named); exposed for the tests. */
int mali_xsync_path(const char *dir, int dmabuf_fd, char *buf, unsigned len);

#endif
