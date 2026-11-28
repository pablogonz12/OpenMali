/* SPDX-License-Identifier: MIT */
/* MEM_SYNC sweep counters and test hooks of the kbase backend (mali_backend_kbase.c).
 * The counters are always kept (a few adds and two clock reads per sweep); MALI_SYNC_STATS=1 prints one line per
 * second of sweep activity to stderr, nothing otherwise. */
#ifndef MALI_KBASE_SYNC_H
#define MALI_KBASE_SYNC_H

#include <stdbool.h>
#include <stdint.h>

struct mali_kdev;

/* cold = host_sync (once per vkQueueSubmit), hot = submit_cs (once per command stream), poll = fence query
 * (timeout 0), wait = blocking fence wait / counter wait. */
enum mali_sync_kind { MALI_SYNC_K_COLD, MALI_SYNC_K_HOT, MALI_SYNC_K_POLL, MALI_SYNC_K_WAIT, MALI_SYNC_K_N };
enum mali_sync_dir { MALI_SYNC_C2D, MALI_SYNC_D2C, MALI_SYNC_DIR_N };   /* CPU->device (op 1), device->CPU (op 2) */

struct mali_sync_counter {
   uint64_t calls;    /* sweeps */
   uint64_t visited;  /* BOs looked at by those sweeps */
   uint64_t ioctls;   /* KBASE MEM_SYNC ioctls issued */
   uint64_t ns;       /* time inside the sweeps (bo_lock held) */
};

/* Live BOs in the registry by class. user = VkDeviceMemory this driver allocated (HOSTLAZY, not exported). */
enum mali_bo_class {
   MALI_BOC_USER_MAPPED, MALI_BOC_USER_UNMAPPED, MALI_BOC_INT_WC, MALI_BOC_INT_CACHED, MALI_BOC_IMPORTED,
   MALI_BOC_EXPORTED, MALI_BOC_N
};

struct mali_sync_stats {
   struct mali_sync_counter c[MALI_SYNC_K_N][MALI_SYNC_DIR_N];
   unsigned live[MALI_BOC_N];
   unsigned nbos;
   unsigned push_list, pend_list;   /* current list sizes (0 with MALI_SYNC_LISTS=0) */
};

/* Snapshot (and optionally reset) the counters of one device. */
void mali_kbase_sync_stats(struct mali_kdev *k, struct mali_sync_stats *out, bool reset);
/* Re-read the MALI_SYNC_* switches (tests). Builds against libkbase's fake kernel also re-read them at every
 * sweep, so a test can flip them with setenv; a build on the real kernel reads them once per device. */
void mali_kbase_sync_env_reload(struct mali_kdev *k);

#endif
