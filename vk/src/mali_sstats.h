/* SPDX-License-Identifier: MIT */
/* MALI_SHARED_STATS=1: where in the dma-buf handoff a frame goes black.
 *
 * The X8 (Mali-G925, arch 13) shows Steam's GPU-composited menu black while every process submits GPU work without an
 * error: chromium (steamwebhelper) renders, Xwayland's glamor-on-zink blits, gamescope composites, DroidDeck imports.
 * Each of those handoffs is a dma-buf: the producer writes it on the GPU, somebody else samples it. If a stage ends
 * up with a black buffer, the numbers of that buffer (how much of it is non-zero, and its mean byte) say which stage
 * turned it black, and the same dma-buf inode names the SAME buffer in every process.
 *
 * So each participant samples the CPU view of every shared dma-buf of at least MALI_SSTATS_MIN_SIZE and logs:
 *   shared_write bo=<inode> size=<bytes> nonzero=<%> mean=<byte> seq=<submit>
 *     after a submit this process made that WROTE an exported or imported shared BO (the dma-buf is the producer's
 *     output), once that submit has retired on the CPU (fence_wait) and after the device->CPU cache sync the driver
 *     does before it reads a BO;
 *   shared_read bo=<inode> size=<bytes> nonzero=<%> mean=<byte> seq=<submit>
 *     before a submit that READS an imported BO (what this process is about to sample as input);
 *   present bo=<inode> size=<bytes> nonzero=<%> mean=<byte> seq=<submit>
 *     when a swapchain image is presented (what the compositor is handed).
 * One line per (bo, kind): the first MALI_SSTATS_FIRST, then every MALI_SSTATS_EVERYth. Every 64th byte is sampled.
 *
 * Lines go to the MALI_WAIT_DBG sink (mali_wait_debug.h): MALI_WAIT_DBG=<prefix> names one file per process
 * (<prefix>-<comm>-<pid>.log), which is what makes "per process and per bo" readable afterwards. MALI_SHARED_STATS
 * without the sink does nothing.
 *
 * The driver keeps every BO CPU mapped (struct mali_bo.cpu, "always CPU mapped": kbase mmaps at alloc/import,
 * mali_backend_fake mallocs), so nothing is mapped here and nothing is unmapped; a BO without a CPU mapping is
 * counted (shared stats total nomap=) and skipped rather than forced through the kernel. */
#ifndef MALI_SSTATS_H
#define MALI_SSTATS_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "mali_backend.h"

enum mali_sstats_kind {
   MALI_SSTATS_WRITE = 0,   /* after a submit that wrote the shared BO */
   MALI_SSTATS_READ = 1,    /* before a submit that reads an imported BO */
   MALI_SSTATS_PRESENT = 2, /* a present of a swapchain image */
};

#define MALI_SSTATS_MIN_SIZE (64u * 1024u) /* smaller BOs are not a composited picture */
#define MALI_SSTATS_STRIDE   64u           /* sample every 64th byte */
#define MALI_SSTATS_FIRST    20u           /* the first N lines per (bo, kind)... */
#define MALI_SSTATS_EVERY    50u           /* ...then every Nth */
#define MALI_SSTATS_SLOTS    64u           /* distinct (bo, kind) pairs tracked; the rest is counted, not logged */

struct mali_sstats_slot {
   int ino;             /* dma-buf inode: the buffer's identity across processes */
   unsigned char kind;  /* enum mali_sstats_kind */
   unsigned seen;       /* every note of this (bo, kind) */
   unsigned emitted;    /* lines printed */
};

struct mali_sstats {
   int on;                          /* MALI_SHARED_STATS set and the MALI_WAIT_DBG file sink open */
   pthread_mutex_t lock;
   uint64_t seq;                    /* submit counter, read/advanced by the queue and the WSI */
   struct mali_sstats_slot slot[MALI_SSTATS_SLOTS];
   unsigned nslot;
   unsigned stat_lines, stat_nomap, stat_nofd, stat_noslot;
};

void mali_sstats_init(struct mali_sstats *s);
void mali_sstats_finish(struct mali_sstats *s);
bool mali_sstats_on(struct mali_sstats *s);

/* The per-process submit counter, advanced by the queue for every submit (and by the WSI for a present). */
uint64_t mali_sstats_next(struct mali_sstats *s);

/* Sample one BO and log the line. bo->size < MALI_SSTATS_MIN_SIZE, a BO without a dma-buf fd, one that is not rate
 * limited, and one without a CPU mapping are no-ops. Safe from several threads (the lock covers the rate table; the
 * cache sync and the read run outside it). */
void mali_sstats_note(struct mali_sstats *s, const struct mali_backend_ops *be, struct mali_kdev *kdev,
                      const struct mali_bo *bo, enum mali_sstats_kind kind, uint64_t seq);

#endif
