/* SPDX-License-Identifier: MIT */
/* Driver-internal GPU memory: BO statistics by allocation site (MALI_BO_STATS=1) and the per-device suballocator that
 * carves small allocations (meta descriptor sets, image-view payloads, descriptor pools, query/event memory) out of a few
 * big chunk BOs, so steady-state rendering performs no BO allocation, mmap or free (MALI_BO_SUBALLOC=0 restores one BO
 * per object). See vk/MEMORY.md "Suballocated driver memory". */
#ifndef MALI_SUBALLOC_H
#define MALI_SUBALLOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mali_backend.h"

/* Who asked for a BO. INTERNAL = every allocation not tagged by a site wrapper (arena, overflow, TLS, tiler heap, sync,
 * shader binaries); CHUNK = the suballocator's own chunk BOs. */
enum mali_bo_site {
   MALI_SITE_INTERNAL,
   MALI_SITE_META,    /* per-command-buffer meta memory (copy/blit/clear/preload descriptor sets) */
   MALI_SITE_VIEW,    /* image-view texture payloads */
   MALI_SITE_DPOOL,   /* descriptor pool memory */
   MALI_SITE_RES,     /* other resources (mali_resources.c seam users) */
   MALI_SITE_QEV,     /* query pools and events */
   MALI_SITE_USER,    /* VkDeviceMemory */
   MALI_SITE_CHUNK,   /* suballocator chunks */
   MALI_SITE_N
};

struct mali_bo_stats {
   uint64_t allocs[MALI_SITE_N], frees[MALI_SITE_N];   /* backend bo_alloc / bo_free calls */
   uint64_t alloc_bytes[MALI_SITE_N], free_bytes[MALI_SITE_N];
   uint64_t alloc_ns, free_ns;                          /* time spent inside the backend's bo_alloc / bo_free */
   uint64_t sub_allocs, sub_frees, sub_reused, sub_fresh, sub_dedicated, sub_reclaimed;   /* suballocator ranges */
};

/* Snapshot of the process-wide counters (always counted; MALI_BO_STATS=1 only adds the stderr line). */
void mali_bo_stats_get(struct mali_bo_stats *out);
const char *mali_bo_site_name(enum mali_bo_site s);
/* Backend hook (weak in the backends): one bo_alloc (alloc = true) or bo_free of `size` bytes that took `ns`. The site is
 * the calling thread's current tag (mali_bo_alloc_site / mali_bo_free_site), INTERNAL otherwise. */
void mali_bo_stats_note(bool alloc, size_t size, uint64_t ns);
/* The same for a BO that did not come from bo_alloc (a dma-buf import that is later freed through bo_free). */
void mali_bo_stats_note_site(enum mali_bo_site site, bool alloc, size_t size, uint64_t ns);
/* MALI_BO_STATS=1: prints one line per second to stderr (called from the allocation paths; no thread). */
void mali_bo_stats_tick(void);

/* be->bo_alloc / be->bo_free with the site tag set for the backend hook. */
int mali_bo_alloc_site(const struct mali_backend_ops *be, struct mali_kdev *k, enum mali_bo_site site, size_t size,
                       uint32_t flags, struct mali_bo *out);
void mali_bo_free_site(const struct mali_backend_ops *be, struct mali_kdev *k, enum mali_bo_site site, struct mali_bo *bo);

/* ---- suballocator ---- */
struct mali_suballoc;
/* GPU progress of the owning device: *submitted = serial of the last submission handed to the GPU, *retired = serial the
 * GPU has completed. A range freed at submitted = s is handed out again only once retired >= s. */
typedef void (*mali_suballoc_progress_fn)(void *user, uint64_t *submitted, uint64_t *retired);

/* on: suballocate (MALI_BO_SUBALLOC unset or != 0) or one BO per allocation (the pre-suballocator path, exactly). */
bool mali_suballoc_env_on(void);
struct mali_suballoc *mali_suballoc_create(const struct mali_backend_ops *be, struct mali_kdev *k, bool on,
                                           mali_suballoc_progress_fn progress, void *user);
/* Frees every chunk; the device must be idle. */
void mali_suballoc_destroy(struct mali_suballoc *sa);
/* Zeroed GPU memory of `size` bytes with exactly the memory properties of bo_alloc(flags), aligned to max(align, 64).
 * NULL on failure. */
void *mali_suballoc_alloc(struct mali_suballoc *sa, enum mali_bo_site site, uint32_t flags, size_t size, size_t align,
                          uint64_t *va);
/* Gives a range back; it is reused once every submission made before this call has retired. */
void mali_suballoc_free(struct mali_suballoc *sa, void *cpu);
/* The BO that holds `cpu` (for bo_wrote / bo_host_dirty marks: a mark on a range is a mark on its chunk). */
struct mali_bo *mali_suballoc_bo_of(struct mali_suballoc *sa, const void *cpu, size_t *off);
bool mali_suballoc_is_on(const struct mali_suballoc *sa);
/* Test hook: chunk BOs alive and bytes they hold. */
void mali_suballoc_usage(struct mali_suballoc *sa, unsigned *chunks, size_t *chunk_bytes, size_t *live_bytes);

#endif
