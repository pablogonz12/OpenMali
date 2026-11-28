/* SPDX-License-Identifier: MIT */
/* Real backend over libkbase (driver/libkbase): probe GPU id/arch/cores from kbase_props and own
 * one kernel context plus one CSF queue per VkDevice. */
#include "mali_backend.h"
#include "mali_kbase_sync.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "kbase.h"
#include "kbase_cs.h"
#include "kbase_uapi.h"

extern uint64_t kb_bo_extra_flags; /* libkbase bo.c: OR'd into plain GPU allocations */

#define POOL_MAX_ENTS 64
#define POOL_DEFAULT_BYTES (64u << 20)   /* MALI_BO_POOL_MB overrides, 0 disables */
#define POOL_MAX_BO_BYTES (16u << 20)    /* bigger BOs are really freed: one of them must not flush the whole pool */

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
   bool sync_all_pending;   /* M15: a write the front end could not pin to a BO: next device->CPU sync sweeps everything */
   /* MEM_SYNC sweep counters (mali_kbase_sync.h), guarded by bo_lock like the sweeps that update them */
   struct mali_sync_counter sc[MALI_SYNC_K_N][MALI_SYNC_DIR_N];
   uint64_t stats_t0;       /* start of the current MALI_SYNC_STATS=1 print interval (ns), 0 = none yet */
   /* MALI_SYNC_* switches, read once per device (sync_env_load); written only under bo_lock */
   struct sync_cfg {
      bool sync_all;   /* MALI_SYNC_ALL=1: every device->CPU sync sweeps every BO (M15 escape hatch) */
      bool track_off;  /* MALI_SYNC_TRACK=0: same, under its older name */
      bool dirty;      /* MALI_SYNC_DIRTY (default on): CPU->device only for user BOs the CPU may have written */
      bool prof;       /* MALI_PROF set: the legacy every-600-sweeps BO dump */
      bool stats;      /* MALI_SYNC_STATS=1: one counter line per second of sweep activity */
      bool lists;      /* MALI_SYNC_LISTS (default on): sweeps walk the sync lists below, not the registry */
   } cfg;
   /* MALI_SYNC_LISTS: the BOs a sweep has to visit, kept up to date on every state change (bo_lock held, O(1) each).
    * push[0]/push[1]: CPU->device candidates of the cold / hot sweep (the same rule the registry walk applies, see
    * push_wanted). pend: native BOs with a device->CPU write mark not yet covered (gpu_writes != synced_writes). imp:
    * dma-buf imports (synced at every blocking wait, as by the walk). A BO that needs no sync is in no list. */
   struct bo_set {
      struct kb_bo **v;
      unsigned n, cap;
   } push[2], pend, imp;
   bool lists_broken;       /* a list could not grow: back to the registry walk for the rest of the device's life */

   /* Free list of recently freed VkDeviceMemory BOs (HOSTLAZY only), oldest first; guarded by bo_lock. A hit skips MEM_ALLOC + mmap (+ the zero/flush of a
    * CACHED BO) and the matching MEM_FREE + munmap. Pooled BOs are not in bos[] (no MEM_SYNC for them). */
   struct pool_ent {
      struct kb_bo *bo;
      uint32_t flags;
   } pool[POOL_MAX_ENTS];
   unsigned npool;
   size_t pool_bytes, pool_max;
   /* VRAM budget hook (mali_memory.c): a pooled BO is still committed in kbase, so it stays charged until it is
    * really freed or handed out again. Called without bo_lock held. */
   void (*pool_release)(void *ctx, size_t size);
   void *pool_release_ctx;
};

/* MALI_SUBMIT_DBG bits (hardware diagnosis, default 0). */
#define DBG_PAD128  0x20u /* 128-byte submissions (ring accounting with another size) */
#define DBG_VERBOSE 0x80u /* on a failed submit print queue state and the ring around CS_EXTRACT */

/* Builds that link libkbase's fake kernel (backend_fake.c defines this test hook; the .so never does) re-read the
 * MALI_SYNC_* switches at every sweep, so a host test can flip them with setenv. On the real kernel they are read once. */
extern unsigned kb_fake_memsync_count(uint64_t va) __attribute__((weak));

static bool
env_is(const char *name, char c)
{
   const char *e = getenv(name);
   return e && e[0] == c;
}

static void sync_lists_rebuild_locked(struct mali_kdev *k);

/* bo_lock held (or the device not yet shared). */
static void
sync_env_load_locked(struct mali_kdev *k)
{
   struct sync_cfg c = {
      .sync_all = env_is("MALI_SYNC_ALL", '1'),
      .track_off = env_is("MALI_SYNC_TRACK", '0'),
      .dirty = !env_is("MALI_SYNC_DIRTY", '0'),
      .prof = getenv("MALI_PROF") != NULL,
      .stats = env_is("MALI_SYNC_STATS", '1'),
      .lists = !env_is("MALI_SYNC_LISTS", '0'),
   };
   if (memcmp(&c, &k->cfg, sizeof c)) {
      k->cfg = c;
      sync_lists_rebuild_locked(k);   /* membership depends on the policy switches */
   }
}

void
mali_kbase_sync_env_reload(struct mali_kdev *k)
{
   pthread_mutex_lock(&k->bo_lock);
   sync_env_load_locked(k);
   pthread_mutex_unlock(&k->bo_lock);
}

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

   struct stat st;
   if (fstat(kb_fd(ctx), &st) == 0)   /* the descriptor actually probed: the path may have been replaced since */
      out->node_rdev = st.st_rdev;

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
   if (getenv("MALI_WSI_DEBUG") || getenv("MALI_SUBMIT_DBG"))
      fprintf(stderr, "mali_vk: kbase bo flags 0x%llx (%s): COHERENT_LOCAL applies to native and imported BOs; "
              "internal BOs %s\n", (unsigned long long)kb_bo_extra_flags,
              kb_bo_extra_flags & 0x800 ? "COHERENT_LOCAL" : "none",
              int_nocoh ? "driver-internal BOs without COHERENT_LOCAL (MALI_INT_NOCOH)" : "every BO gets it");
   const char *fi = getenv("MALI_FAULT_INJECT");
   k->fault_at = fi ? strtoul(fi, NULL, 10) : 0;
   pthread_mutex_init(&k->submit_lock, NULL);
   pthread_mutex_init(&k->wait_lock, NULL);
   pthread_mutex_init(&k->bo_lock, NULL);
   sync_env_load_locked(k);
   {
      const char *pe = getenv("MALI_BO_POOL_MB");
      k->pool_max = pe ? (size_t)strtoul(pe, NULL, 10) << 20 : POOL_DEFAULT_BYTES;
   }

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
   for (unsigned i = 0; i < k->npool; i++) {   /* no other thread uses the device any more */
      size_t sz = k->pool[i].bo->size;
      kb_bo_free(k->pool[i].bo);
      free(k->pool[i].bo);
      if (k->pool_release)   /* the pool dies with the device: give the budget back before the pdev goes away */
         k->pool_release(k->pool_release_ctx, sz);
   }
   k->npool = 0;
   if (k->cqs_live)
      kb_bo_free(&k->cqs);
   if (k->queue_live)
      kb_queue_destroy(&k->queue);
   kb_close(k->ctx);
   pthread_mutex_destroy(&k->submit_lock);
   pthread_mutex_destroy(&k->wait_lock);
   pthread_mutex_destroy(&k->bo_lock);
   free(k->bos);
   free(k->push[0].v);
   free(k->push[1].v);
   free(k->pend.v);
   free(k->imp.v);
   free(k);
}

/* ---- registry and sync lists (bo_lock held) ---- */
enum { IDX_REG, IDX_PUSH, IDX_PEND, IDX_IMP };

static bool
set_add(struct bo_set *s, struct kb_bo *b, unsigned slot)
{
   if (b->sync_idx[slot])
      return true;
   if (s->n == s->cap) {
      unsigned nc = s->cap ? s->cap * 2 : 64;
      struct kb_bo **nv = realloc(s->v, nc * sizeof *nv);
      if (!nv)
         return false;
      s->v = nv;
      s->cap = nc;
   }
   s->v[s->n++] = b;
   b->sync_idx[slot] = s->n;
   if (slot == IDX_PEND)
      __atomic_store_n(&b->pend_queued, true, __ATOMIC_SEQ_CST);
   return true;
}

/* O(1): the last member takes the hole. A sweep that walks a list backwards may remove the member it is looking at. */
static void
set_del(struct bo_set *s, struct kb_bo *b, unsigned slot)
{
   unsigned i = b->sync_idx[slot];
   if (!i)
      return;
   struct kb_bo *last = s->v[--s->n];
   s->v[i - 1] = last;
   last->sync_idx[slot] = i;
   b->sync_idx[slot] = 0;
   if (slot == IDX_PEND)
      __atomic_store_n(&b->pend_queued, false, __ATOMIC_SEQ_CST);
}

static bool
lists_active(const struct mali_kdev *k)
{
   return k->cfg.lists && !k->lists_broken;
}

/* The CPU->device rule of the registry walk (mem_sync_bos), per BO: true = this BO is synced by its sweep. */
static bool
push_wanted(const struct mali_kdev *k, const struct kb_bo *b)
{
   if (b->host_lazy || (b->flags & KB_BO_NOCOH) || b->imported)
      return false;
   /* MALI_SYNC_DIRTY: a user BO the CPU cannot have written since the last sync has nothing to push */
   if (b->user_mem && k->cfg.dirty && !b->cpu_dirty && !b->cpu_mapped)
      return false;
   return true;
}

/* The device->CPU rule for native BOs: a write mark the last sync did not cover. */
static bool
pend_wanted(const struct kb_bo *b)
{
   if (b->host_lazy || (b->flags & KB_BO_NOCOH) || b->imported)
      return false;
   return __atomic_load_n(&b->gpu_writes, __ATOMIC_SEQ_CST) != b->synced_writes;
}

static void
sync_lists_drop_locked(struct mali_kdev *k, struct kb_bo *b)
{
   set_del(&k->push[b->hot], b, IDX_PUSH);
   set_del(&k->pend, b, IDX_PEND);
   set_del(&k->imp, b, IDX_IMP);
}

static void sync_lists_clear_locked(struct mali_kdev *k);

/* Re-evaluate one BO's list membership after any change of its state. */
static void
sync_lists_update_locked(struct mali_kdev *k, struct kb_bo *b)
{
   if (!lists_active(k) || !b->sync_idx[IDX_REG])
      return;
   bool ok = true;
   if (push_wanted(k, b))
      ok &= set_add(&k->push[b->hot], b, IDX_PUSH);
   else
      set_del(&k->push[b->hot], b, IDX_PUSH);
   if (pend_wanted(b))
      ok &= set_add(&k->pend, b, IDX_PEND);
   else
      set_del(&k->pend, b, IDX_PEND);
   if (b->imported && !b->host_lazy && !(b->flags & KB_BO_NOCOH))
      ok &= set_add(&k->imp, b, IDX_IMP);
   else
      set_del(&k->imp, b, IDX_IMP);
   if (!ok) {
      /* Out of memory: a BO missing from a list would miss its sync. The registry is complete, so walk it from now on. */
      fprintf(stderr, "mali_vk: MEM_SYNC lists out of memory: back to the full registry walk\n");
      k->lists_broken = true;
      sync_lists_clear_locked(k);
   }
}

static void
sync_lists_clear_locked(struct mali_kdev *k)
{
   for (unsigned i = 0; i < k->nbos; i++)
      sync_lists_drop_locked(k, k->bos[i]);
}

static void
sync_lists_rebuild_locked(struct mali_kdev *k)
{
   sync_lists_clear_locked(k);
   for (unsigned i = 0; i < k->nbos && lists_active(k); i++)
      sync_lists_update_locked(k, k->bos[i]);
}

/* Registry add/remove, both O(1). The registry stays complete in every mode: it is what MALI_SYNC_LISTS=0, a full
 * device->CPU sweep (bo_wrote(NULL), MALI_SYNC_ALL=1) and the stats walk. */
static bool
track_bo(struct mali_kdev *k, struct kb_bo *bo, bool add)
{
   pthread_mutex_lock(&k->bo_lock);
   if (add) {
      if (k->nbos == k->capbos) {
         unsigned nc = k->capbos ? k->capbos * 2 : 64;
         struct kb_bo **nb = realloc(k->bos, nc * sizeof *nb);
         if (!nb) {
            pthread_mutex_unlock(&k->bo_lock);
            return false;    /* the caller must not keep a BO the sync sweep cannot see */
         }
         k->bos = nb;
         k->capbos = nc;
      }
      memset(bo->sync_idx, 0, sizeof bo->sync_idx);   /* a pooled BO comes back with stale positions */
      bo->pend_queued = false;
      k->bos[k->nbos++] = bo;
      bo->sync_idx[IDX_REG] = k->nbos;
      bo->sync_owner = k;
      sync_lists_update_locked(k, bo);
   } else if (bo->sync_idx[IDX_REG]) {
      sync_lists_drop_locked(k, bo);
      unsigned i = bo->sync_idx[IDX_REG];
      struct kb_bo *last = k->bos[--k->nbos];
      k->bos[i - 1] = last;
      last->sync_idx[IDX_REG] = i;
      bo->sync_idx[IDX_REG] = 0;
      bo->sync_owner = NULL;
   }
   pthread_mutex_unlock(&k->bo_lock);
   return true;
}

/* Only VkDeviceMemory: Vulkan does not promise zeroed memory, driver-internal BOs may rely on it. */
static bool
pool_eligible(uint32_t flags)
{
   return (flags & MALI_BO_HOSTLAZY) && !(flags & (MALI_BO_EXPORT | MALI_BO_EVENT | MALI_BO_EXEC | MALI_BO_HOT));
}

static struct kb_bo *
pool_take(struct mali_kdev *k, size_t size, uint32_t flags)
{
   struct kb_bo *bo = NULL;
   pthread_mutex_lock(&k->bo_lock);
   for (unsigned i = k->npool; i-- > 0;)   /* newest first: warmest pages */
      if (k->pool[i].flags == flags && k->pool[i].bo->size == size) {
         bo = k->pool[i].bo;
         memmove(&k->pool[i], &k->pool[i + 1], (k->npool - i - 1) * sizeof k->pool[0]);
         k->npool--;
         k->pool_bytes -= size;
         break;
      }
   pthread_mutex_unlock(&k->bo_lock);
   /* the BO leaves the pool: its charge moves to the VkDeviceMemory that is being allocated now */
   if (bo && k->pool_release)
      k->pool_release(k->pool_release_ctx, size);
   return bo;
}

/* can the pool ever retain a BO of this size (pool disabled with MALI_BO_POOL_MB=0, or too big)? */
static bool
pool_fits(const struct mali_kdev *k, size_t size)
{
   return size <= POOL_MAX_BO_BYTES && size <= k->pool_max;
}

/* true: the pool kept bo (the caller must not free it). Evicted BOs are freed outside the lock. */
static bool
pool_put(struct mali_kdev *k, struct kb_bo *bo, uint32_t flags)
{
   struct kb_bo *victim[POOL_MAX_ENTS];
   unsigned nv = 0;
   bool kept = false;
   if (!pool_fits(k, bo->size))
      return false;
   pthread_mutex_lock(&k->bo_lock);
   unsigned drop = 0;
   size_t bytes = k->pool_bytes;
   while (drop < k->npool && (bytes + bo->size > k->pool_max || k->npool - drop >= POOL_MAX_ENTS)) {
      bytes -= k->pool[drop].bo->size;
      victim[nv++] = k->pool[drop++].bo;
   }
   memmove(&k->pool[0], &k->pool[drop], (k->npool - drop) * sizeof k->pool[0]);
   k->npool -= drop;
   k->pool[k->npool++] = (struct pool_ent){bo, flags};
   k->pool_bytes = bytes + bo->size;
   kept = true;
   pthread_mutex_unlock(&k->bo_lock);
   for (unsigned i = 0; i < nv; i++) {
      size_t sz = victim[i]->size;
      kb_bo_free(victim[i]);
      free(victim[i]);
      if (k->pool_release)   /* evicted: really freed, so no longer committed */
         k->pool_release(k->pool_release_ctx, sz);
   }
   return kept;
}

/* Really free pooled BOs until `want` bytes are back in the budget (or the pool is empty). Called when a
 * reservation is refused: the pool is holding committed memory the caller could use. Returns the bytes freed. */
static size_t
kbase_pool_purge(struct mali_kdev *k, size_t want)
{
   size_t freed = 0;
   while (freed < want) {
      struct kb_bo *bo = NULL;
      size_t sz = 0;
      pthread_mutex_lock(&k->bo_lock);
      if (k->npool) {
         bo = k->pool[0].bo;   /* oldest first: the least likely to be reused */
         sz = bo->size;
         memmove(&k->pool[0], &k->pool[1], (k->npool - 1) * sizeof k->pool[0]);
         k->npool--;
         k->pool_bytes -= sz;
      }
      pthread_mutex_unlock(&k->bo_lock);
      if (!bo)
         break;
      kb_bo_free(bo);
      free(bo);
      if (k->pool_release)
         k->pool_release(k->pool_release_ctx, sz);
      freed += sz;
   }
   return freed;
}

static void
kbase_pool_watch(struct mali_kdev *k, void (*release)(void *, size_t), void *ctx)
{
   pthread_mutex_lock(&k->bo_lock);
   k->pool_release = release;
   k->pool_release_ctx = ctx;
   pthread_mutex_unlock(&k->bo_lock);
}

static int
kbase_bo_alloc(struct mali_kdev *k, size_t size, uint32_t flags, struct mali_bo *out)
{
   size = (size + 4095) & ~(size_t)4095;
   if (pool_eligible(flags)) {
      struct kb_bo *hit = pool_take(k, size, flags);
      if (hit) {
         hit->host_lazy = true;   /* the next user has not touched it yet; contents are stale (Vulkan allows it) */
         hit->cpu_dirty = true;   /* the last owner's writes were never MEM_SYNCed after its unmap: never skip the first sync */
         hit->cpu_mapped = false;
         hit->user_mem = (flags & MALI_BO_HOSTLAZY) && !(flags & MALI_BO_EXPORT);
         hit->hot = false;
         hit->gpu_writes = hit->synced_writes = 0;   /* a pooled BO starts with no pending write marks */
         *out = (struct mali_bo){.gpu_va = hit->gpu_va, .cpu = hit->cpu, .size = size, .flags = flags,
                                 .dmabuf_fd = hit->dmabuf_fd, .priv = hit};
         (void)track_bo(k, hit, true);
         return 0;
      }
   }
   struct kb_bo *bo = calloc(1, sizeof(*bo));
   if (!bo)
      return -ENOMEM;

   /* MALI_INT_NOCOH=1 (experiment): driver-internal BOs (arena, descriptors, CS, heaps: no HOSTLAZY/EXPORT, i.e. not VkDeviceMemory) without COHERENT_LOCAL.
    * Only VkDeviceMemory (SSBO atomics from the app) needs the cross-core coherency; internal data is written by the CPU (WC) and read by the GPU. */
   if (int_nocoh < 0) {
      const char *ie = getenv("MALI_INT_NOCOH");   /* default: on for arch 11 only (v10/v13 keep it: the v13 tiler bins nothing without it); =0/1 overrides */
      int_nocoh = ie ? ie[0] == '1' : k_int_nocoh_default;
   }
   if (int_nocoh && !(flags & (MALI_BO_EXPORT | MALI_BO_HOSTLAZY | MALI_BO_EVENT | MALI_BO_EXEC | MALI_BO_CACHED | MALI_BO_COH)))
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
   /* MALI_SYNC_DIRTY: a new CPU-side BO starts dirty (the CPU may write it as soon as it is handed out; a CACHED one
    * was zeroed and written back above), so its first submit still syncs it, exactly like before this tracking. */
   bo->cpu_dirty = true;
   bo->cpu_mapped = false;
   /* MALI_SYNC_DIRTY covers only VkDeviceMemory this driver allocated and nobody else can map: NOT an exported dma-buf
    * (foreign CPU writers we cannot see) and not a driver-internal BO (the front end does not call us for its writes). */
   bo->user_mem = (flags & MALI_BO_HOSTLAZY) && !(flags & MALI_BO_EXPORT);
   (void)track_bo(k, bo, true);
   return 0;
}

static void
kbase_bo_wrote(struct mali_kdev *k, struct mali_bo *b)
{
   if (!k)
      return;
   if (!b) {
      /* the front end could not enumerate the writes: one full sweep at the next device->CPU sync */
      __atomic_store_n(&k->sync_all_pending, true, __ATOMIC_RELAXED);
      return;
   }
   struct kb_bo *bo = b->priv;
   if (!bo)
      return;
   __atomic_add_fetch(&bo->gpu_writes, 1, __ATOMIC_SEQ_CST);
   /* MALI_SYNC_LISTS: file the BO in the pending list unless it is there already. Lock-free when it is: a sweep that
    * takes it out first clears pend_queued and then re-reads gpu_writes (sync_pend_retire_locked), so either the sweep
    * sees this mark and keeps it, or this thread sees the flag clear and files it under bo_lock. */
   if (__atomic_load_n(&k->cfg.lists, __ATOMIC_RELAXED) && !__atomic_load_n(&bo->pend_queued, __ATOMIC_SEQ_CST)) {
      pthread_mutex_lock(&k->bo_lock);
      sync_lists_update_locked(k, bo);
      pthread_mutex_unlock(&k->bo_lock);
   }
}

/* The three host-write callbacks below run on the app's thread (vkMapMemory/vkUnmapMemory/vkFlushMappedMemoryRanges)
 * while mem_sync_bos() runs on whichever thread submits a queue, and that function reads and clears the same flags under
 * bo_lock. Without the lock the clear can overwrite a map() that happened just after the sync loop read cpu_mapped, and
 * the CPU's later writes would never be pushed. So take bo_lock here too, so a touch that overlaps a sync lands either
 * before the loop reads the flags or after it cleared them (and leaves the BO dirty for the next submit).
 * The buffer path passes a NULL kdev (mali_buffer.c bo_host_touch after a rebind, before any submit of that buffer), so
 * there is no lock to take there; kdev is what carries it. */
static void
kbase_bo_host_touch(struct mali_kdev *k, struct mali_bo *b)
{
   if (!b || !b->priv)
      return;
   struct kb_bo *bo = b->priv;
   if (!k)
      k = bo->sync_owner;   /* mali_buffer.c passes no kdev; a tracked BO knows its device (its lists must see this) */
   if (k) pthread_mutex_lock(&k->bo_lock);
   bo->host_lazy = false;
   /* MALI_SYNC_DIRTY: the CPU may write through this mapping at any time, so the BO is dirty until it is unmapped and
    * a sync has covered it (mem_sync_bos keeps it dirty while cpu_mapped is set). */
   bo->cpu_dirty = true;
   bo->cpu_mapped = true;
   if (k) sync_lists_update_locked(k, bo);
   if (k) pthread_mutex_unlock(&k->bo_lock);
}

/* MALI_SYNC_DIRTY: vkUnmapMemory. The bytes the CPU wrote are still not visible to the GPU, so cpu_dirty stays set:
 * only the per-submit sync clears it, and only when the CPU can no longer write behind our back. */
static void
kbase_bo_host_unmap(struct mali_kdev *k, struct mali_bo *b)
{
   if (!b || !b->priv)
      return;
   struct kb_bo *bo = b->priv;
   if (!k)
      k = bo->sync_owner;
   if (k) pthread_mutex_lock(&k->bo_lock);
   bo->cpu_mapped = false;   /* cpu_dirty stays set: only the sync under bo_lock clears it */
   if (k) sync_lists_update_locked(k, bo);
   if (k) pthread_mutex_unlock(&k->bo_lock);
}

/* MALI_SYNC_DIRTY: vkFlushMappedMemoryRanges / a driver CPU copy into an app BO (bo_host_touch already covers maps). */
static void
kbase_bo_host_dirty(struct mali_kdev *k, struct mali_bo *b)
{
   if (!b || !b->priv)
      return;
   struct kb_bo *bo = b->priv;
   if (!k)
      k = bo->sync_owner;
   if (k) pthread_mutex_lock(&k->bo_lock);
   bo->cpu_dirty = true;
   if (k) sync_lists_update_locked(k, bo);
   if (k) pthread_mutex_unlock(&k->bo_lock);
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
   /* Tracked like a native BO so kbase_fence_wait's device->CPU sweep reaches it: an import carries
    * COHERENT_LOCAL too (bo.c: kb_bo_extra_flags), so after a GPU write its CPU mapping needs the CSYNC before
    * the CPU reads it. Marked as foreign so mem_sync_bos does not push our CPU copy at the GPU before a submit
    * (that direction stays for BOs this driver allocated; an exported one is still ours to push). */
   bo->imported = true;   /* before track_bo: the lists file it by kind */
   if (!track_bo(k, bo, true)) {
      /* Without a registry slot the import would never be part of the device->CPU sweep: give it back. */
      kb_bo_free(bo);
      free(bo);
      return -ENOMEM;
   }
   return 0;
}

static void
kbase_bo_free(struct mali_kdev *k, struct mali_bo *bo)
{
   if (!bo->priv)
      return;
   (void)track_bo(k, bo->priv, false);
   /* A pooled CPU-cached BO keeps its mapping and is handed out again without the zero+flush of a fresh one: write back any CPU-dirty lines now, or they could
    * be evicted later over the next owner's GPU results. A BO never touched by the CPU (host_lazy still set) has no dirty lines. */
   if (pool_eligible(bo->flags) && pool_fits(k, bo->size) && (bo->flags & MALI_BO_CACHED) && !((struct kb_bo *)bo->priv)->host_lazy)
      kb_bo_cache_flush_inv(bo->priv, 0, bo->size);
   if (pool_eligible(bo->flags) && pool_put(k, bo->priv, bo->flags)) {
      bo->pooled = true;   /* the pool keeps it: its pages are still committed, so the charge stays */
   } else {
      bo->pooled = false;
      kb_bo_free(bo->priv);
      free(bo->priv);
   }
   bo->priv = NULL;
}

static void
kbase_bo_cache(struct mali_kdev *k, const struct mali_bo *bo, size_t off, size_t len, bool invalidate)
{
   /* A dma-buf backed BO (every swapchain image) has no cache lines of ours to touch, but it does carry the
    * writer's fence: DMA_BUF_IOCTL_SYNC START|READ waits for the GPU's writes, and that is what a CPU read of
    * the presented image needs (the X11 software present copies it out). Without it the copy can read a frame
    * the GPU has not finished writing, which is a black window, not a torn one. */
   if (!(bo->flags & MALI_BO_CACHED)) {
      if (bo->priv && (bo->flags & MALI_BO_EXPORT) && bo->dmabuf_fd >= 0)
         kb_bo_dmabuf_sync(bo->priv, false /* START */, false /* READ: wait for the GPU writer */);
      return;
   }
   if (off >= bo->size)
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
enum sync_sel { SYNC_ALL, SYNC_HOT, SYNC_COLD, SYNC_POLL };
static void mem_sync_bos(struct mali_kdev *k, unsigned op, enum sync_sel sel);
static int
kbase_fence_wait(struct mali_kdev *k, int fence_fd, int timeout_ms)
{
   /* kb_fence_wait reads kbase notifications from the shared fd, so only one thread at a time; slice
    * the wait so that others (and the timeout) get their turn. */
   if (timeout_ms == 0) {
      /* A poll must not queue behind another thread's slice on wait_lock (up to 20 ms): the sync_file itself says signalled or not,
       * and a signalled fence has no notification left to read. Error state is still reported (POLLERR / faulted queue). */
      struct pollfd pf = {fence_fd, POLLIN, 0};
      int pr;
      while ((pr = poll(&pf, 1, 0)) < 0 && errno == EINTR) {}
      if (pr < 0)
         return -errno;
      if (pr == 0) {
         if (kb_queue_faulted(&k->queue)) {
            k->lost = true;
            return -EIO;
         }
         return -ETIMEDOUT;
      }
      if (pf.revents & POLLERR) {
         k->lost = true;
         return -EIO;
      }
      /* A timeout-0 poll is a query, not a wait: sync the native BOs the GPU wrote (as before) but leave foreign
       * imports alone. We cannot tell whether an import is a dependency of this fence, and a poll happens far
       * more often than a wait; a blocking wait (below) still covers every import. */
      mem_sync_bos(k, 2, SYNC_POLL);
      return 0;
   }
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

static void
mem_sync_all(struct mali_kdev *k, unsigned op)
{
   mem_sync_bos(k, op, SYNC_ALL);
}

static uint64_t
sync_now_ns(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static enum mali_bo_class
bo_class(const struct kb_bo *b)
{
   if (b->imported)
      return MALI_BOC_IMPORTED;
   if (b->dmabuf_fd >= 0)
      return MALI_BOC_EXPORTED;
   if (b->user_mem)
      return b->cpu_mapped ? MALI_BOC_USER_MAPPED : MALI_BOC_USER_UNMAPPED;
   return (b->flags & KB_BO_CPU_CACHED) ? MALI_BOC_INT_CACHED : MALI_BOC_INT_WC;
}

/* bo_lock held. The class counts walk the registry: only done for a snapshot or a once-a-second print. */
static void
sync_stats_locked(struct mali_kdev *k, struct mali_sync_stats *out, bool reset)
{
   memset(out, 0, sizeof *out);
   memcpy(out->c, k->sc, sizeof out->c);
   for (unsigned i = 0; i < k->nbos; i++)
      out->live[bo_class(k->bos[i])]++;
   out->nbos = k->nbos;
   out->push_list = k->push[0].n + k->push[1].n;
   out->pend_list = k->pend.n + k->imp.n;
   if (reset)
      memset(k->sc, 0, sizeof k->sc);
}

void
mali_kbase_sync_stats(struct mali_kdev *k, struct mali_sync_stats *out, bool reset)
{
   pthread_mutex_lock(&k->bo_lock);
   sync_stats_locked(k, out, reset);
   pthread_mutex_unlock(&k->bo_lock);
}

/* MALI_SYNC_STATS=1: one stderr line per second of sweep activity (bo_lock held). */
static void
sync_stats_tick(struct mali_kdev *k, uint64_t now)
{
   if (!k->stats_t0) {
      k->stats_t0 = now;
      return;
   }
   if (now - k->stats_t0 < 1000000000ull)
      return;
   struct mali_sync_stats s;
   sync_stats_locked(k, &s, true);
   static const char *const kn[MALI_SYNC_K_N] = {"cold", "hot", "poll", "wait"};
   char line[768];
   int n = snprintf(line, sizeof line,
                    "mali_vk: sync-stats %.2fs bos %u (user mapped %u unmapped %u, int wc %u cached %u, imported %u, exported %u) lists push %u pend %u |",
                    (now - k->stats_t0) / 1e9, s.nbos, s.live[MALI_BOC_USER_MAPPED], s.live[MALI_BOC_USER_UNMAPPED],
                    s.live[MALI_BOC_INT_WC], s.live[MALI_BOC_INT_CACHED], s.live[MALI_BOC_IMPORTED],
                    s.live[MALI_BOC_EXPORTED], s.push_list, s.pend_list);
   for (int kind = 0; kind < MALI_SYNC_K_N; kind++)
      for (int d = 0; d < MALI_SYNC_DIR_N; d++) {
         const struct mali_sync_counter *c = &s.c[kind][d];
         if (!c->calls || n <= 0 || (size_t)n >= sizeof line)
            continue;
         n += snprintf(line + n, sizeof line - n, " %s.%s %llu calls %llu bos %llu ioctl %llu us", kn[kind],
                       d == MALI_SYNC_C2D ? "c2d" : "d2c", (unsigned long long)c->calls,
                       (unsigned long long)c->visited, (unsigned long long)c->ioctls,
                       (unsigned long long)(c->ns / 1000));
      }
   fprintf(stderr, "%s\n", line);
   k->stats_t0 = now;
}

/* One KBASE MEM_SYNC (bo_lock held). */
static int
memsync_ioctl_locked(struct mali_kdev *k, const struct kb_bo *b, unsigned op, struct mali_sync_counter *sc)
{
   struct {
      uint64_t handle, user_addr, size;
      uint8_t type, pad[7];
   } ms = {b->gpu_va, (uint64_t)(uintptr_t)b->cpu, b->size, (uint8_t)op, {0}};
   sc->ioctls++;
   return kb_ioctl_fd(kb_fd(k->ctx), _IOW(KBASE_IOCTL_TYPE, 15, ms), &ms);
}

/* A pending-list member whose marks are all covered leaves the list. pend_queued is cleared BEFORE gpu_writes is
 * re-read, so a concurrent kbase_bo_wrote either is seen here (the BO stays) or sees the flag clear and files the BO
 * again once bo_lock is free; a mark is never left outside the list. */
static void
sync_pend_retire_locked(struct mali_kdev *k, struct kb_bo *b)
{
   if (b->host_lazy || (b->flags & KB_BO_NOCOH)) {   /* cannot be synced now; host_touch files it again */
      set_del(&k->pend, b, IDX_PEND);
      return;
   }
   __atomic_store_n(&b->pend_queued, false, __ATOMIC_SEQ_CST);
   if (__atomic_load_n(&b->gpu_writes, __ATOMIC_SEQ_CST) != b->synced_writes)
      __atomic_store_n(&b->pend_queued, true, __ATOMIC_SEQ_CST);
   else
      set_del(&k->pend, b, IDX_PEND);
}

/* MALI_SYNC_LISTS: the same syncs as the registry walk below, visiting only the list members (bo_lock held). The
 * lists are walked backwards so that a member leaving its list (set_del moves the last one into its slot) is never
 * skipped and never visited twice. */
static void
mem_sync_lists_locked(struct mali_kdev *k, unsigned op, enum sync_sel sel, struct mali_sync_counter *sc)
{
   if (op == 1) {
      struct bo_set *ps = &k->push[sel == SYNC_HOT];
      for (unsigned i = ps->n; i-- > 0;) {
         struct kb_bo *b = ps->v[i];
         sc->visited++;
         int sr = memsync_ioctl_locked(k, b, 1, sc);
         if (sr >= 0 && !b->cpu_mapped)
            b->cpu_dirty = false;   /* the CPU cannot write it now: this ioctl covered everything it wrote */
         if (!push_wanted(k, b))
            set_del(ps, b, IDX_PUSH);
      }
      return;
   }
   for (unsigned i = k->pend.n; i-- > 0;) {
      struct kb_bo *b = k->pend.v[i];
      sc->visited++;
      uint64_t wrote = __atomic_load_n(&b->gpu_writes, __ATOMIC_SEQ_CST);
      if (!b->host_lazy && !(b->flags & KB_BO_NOCOH) && wrote != b->synced_writes &&
          memsync_ioctl_locked(k, b, 2, sc) >= 0)
         b->synced_writes = wrote;   /* what this ioctl covered; a concurrent submit's mark stays pending */
      sync_pend_retire_locked(k, b);
   }
   if (sel == SYNC_POLL)
      return;   /* see the poll path in kbase_fence_wait: a poll leaves foreign imports alone */
   for (unsigned i = k->imp.n; i-- > 0;) {
      sc->visited++;
      (void)memsync_ioctl_locked(k, k->imp.v[i], 2, sc);
   }
}

static void
mem_sync_bos(struct mali_kdev *k, unsigned op, enum sync_sel sel)
{
   /* COHERENT_LOCAL BOs (arch 13, MALI_BO_COHERENT_LOCAL=1): CPU -> device MEM_SYNC before every submit (V13-NEXT2.md). */
   if (!(kb_bo_extra_flags & 0x800))
      return;
   /* M15: a device->CPU sync only has to cover BOs the GPU wrote since the last one. The front end marks every
    * GPU write (copy destinations, render targets, clears, storage descriptors) with bo_wrote, whose counter is
    * monotonic per BO: a BO also marked by a frame that is still in flight is synced again at the next wait
    * instead of being skipped. MALI_SYNC_ALL=1 restores the old full sweep for A/B; a mark the front end could
    * not place (bo_wrote(NULL)) sweeps everything once. The CPU->device direction (op 1) is unchanged. */
   const bool dev2cpu = op == 2;
   const uint64_t t_start = sync_now_ns();
   struct mali_sync_counter *sc = &k->sc[sel == SYNC_COLD ? MALI_SYNC_K_COLD : sel == SYNC_HOT ? MALI_SYNC_K_HOT
                                         : sel == SYNC_POLL ? MALI_SYNC_K_POLL : MALI_SYNC_K_WAIT]
                                        [dev2cpu ? MALI_SYNC_D2C : MALI_SYNC_C2D];
   pthread_mutex_lock(&k->bo_lock);
   if (kb_fake_memsync_count)
      sync_env_load_locked(k);   /* fake-kernel test build: the tests flip the switches between sweeps */
   sc->calls++;
   bool sweep_all = dev2cpu && (k->sync_all_pending || k->cfg.sync_all || k->cfg.track_off);
   k->sync_all_pending = false;
   /* MALI_SYNC_LISTS (default): visit only the BOs that need this sync. A full device->CPU sweep (an unplaceable
    * write, MALI_SYNC_ALL=1) still walks the registry below and then prunes the pending list. */
   const bool lists = lists_active(k);
   if (lists && !sweep_all) {
      mem_sync_lists_locked(k, op, sel, sc);
      goto done;
   }
   sc->visited += k->nbos;
   static unsigned prof_calls;
   if (k->cfg.prof && ++prof_calls == 600) {
      unsigned n = 0;
      for (unsigned i = 0; i < k->nbos; i++) if (!k->bos[i]->host_lazy && !(k->bos[i]->flags & KB_BO_NOCOH)) { n++; fprintf(stderr, "PROF/memsync bo size %zu flags %x hot %d dmabuf %d\n", k->bos[i]->size, k->bos[i]->flags, k->bos[i]->hot, k->bos[i]->dmabuf_fd >= 0); }
      fprintf(stderr, "PROF/memsync nbos=%u non-lazy=%u\n", k->nbos, n);
   }
   for (unsigned i = 0; i < k->nbos; i++) {
      struct kb_bo *b = k->bos[i];
      if (b->host_lazy || (b->flags & KB_BO_NOCOH) || (sel == SYNC_HOT && !b->hot) || (sel == SYNC_COLD && b->hot))
         continue;
      /* dma-buf imports are in k->bos[] for the device->CPU sweep only: we must not push our CPU copy of somebody
       * else's buffer at the GPU before a submit (that direction stays native-BO only). */
      if (!dev2cpu && b->imported)
         continue;
      if (sel == SYNC_POLL && b->imported)
         continue;   /* see the poll path in kbase_fence_wait */
      /* MALI_SYNC_DIRTY (CPU -> device direction only): a BO the CPU has not written since the last sync has nothing
       * to push. Concretely: a VkDeviceMemory BO that was never mapped (host_lazy, skipped above) or whose last mapping
       * was unmapped and already synced is skipped; a BO that is mapped RIGHT NOW is always synced, because the CPU can
       * write it between this check and the next submit without telling us. Driver-internal BOs (arena, heap, geo, sync,
       * TLS, CS) are not covered by the rule at all - they keep the per-submit sync - since only the front end knows who
       * writes them. MALI_SYNC_DIRTY=0 restores the old "sync every mapped BO at every submit". */
      if (!dev2cpu && b->user_mem && k->cfg.dirty && !b->cpu_dirty && !b->cpu_mapped)
         continue;
      uint64_t wrote = 0;
      /* Imports are not marked per submission (the front end marks BOs it renders into), so a write-counter
       * check would either skip them or claim them synced before their submission completes: sync a foreign
       * import on every device->CPU wait until write generations exist for it. */
      if (dev2cpu && !b->imported) {
         wrote = __atomic_load_n(&b->gpu_writes, __ATOMIC_RELAXED);
         if (!sweep_all && wrote == b->synced_writes)
            continue;
      }
      int sr = memsync_ioctl_locked(k, b, op, sc);
      if (dev2cpu && !b->imported && sr >= 0)
         b->synced_writes = wrote;   /* what this ioctl covered; a concurrent submit's mark stays pending */
      else if (!dev2cpu && sr >= 0 && !b->cpu_mapped)
         b->cpu_dirty = false;   /* the CPU cannot write it now: this ioctl covered everything it wrote */
   }
   if (lists)   /* after a full sweep: drop the pending members it covered */
      for (unsigned i = k->pend.n; i-- > 0;)
         sync_pend_retire_locked(k, k->pend.v[i]);
done:;
   const uint64_t t_end = sync_now_ns();
   sc->ns += t_end - t_start;
   if (k->cfg.stats)
      sync_stats_tick(k, t_end);
   pthread_mutex_unlock(&k->bo_lock);
}

static void
kbase_host_sync(struct mali_kdev *k)
{
   /* Hot BOs (arena, overflow) are written only by the frame build, which always ends in kbase_submit_cs; that syncs them
    * after the build. Syncing them here too was a second ioctl per hot BO per queue submit that covered no CPU write. */
   mem_sync_bos(k, 1, SYNC_COLD);
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
   mem_sync_bos(k, 1, SYNC_HOT);   /* the other BOs were synced by host_sync at the start of the queue submit */
   /* One kick per piece: the old fixed 2 KiB piece kicked every 256 instructions (a 3k-draw frame is 3064
    * instructions = 12 kicks), and a piece larger than the ring made wait_ring_space spin until it timed out.
    * A quarter of the ring keeps three quarters free for the GPU, so any frame size makes progress. */
   const unsigned piece = kb_queue_piece_instrs(k->queue.ring_size);
   for (unsigned i = 0; i < n && !r; i += piece) {
      unsigned m = n - i < piece ? n - i : piece;
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
   .bo_pool_watch = kbase_pool_watch,
   .bo_pool_purge = kbase_pool_purge,
   .bo_cache = kbase_bo_cache,
   .bo_export = kbase_bo_export,
   .submit_signal = kbase_submit_signal,
   .fence_wait = kbase_fence_wait,
   .device_lost = kbase_device_lost,
   .gpu_timestamp = kbase_gpu_timestamp,
   .host_sync = kbase_host_sync,
   .bo_host_touch = kbase_bo_host_touch,
   .bo_host_unmap = kbase_bo_host_unmap,
   .bo_host_dirty = kbase_bo_host_dirty,
   .bo_wrote = kbase_bo_wrote,
   .submit_cs = kbase_submit_cs,
   .wait_counter = kbase_wait_counter,
   .heap_create = kbase_heap_create,
   .heap_destroy = kbase_heap_destroy,
};
