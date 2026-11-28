/* SPDX-License-Identifier: MIT */
/* Queue submit (M4): the command stream only carries a flush and the fence signal; real command
 * buffers arrive with M8. vk_queue runs in DEFERRED mode (emulated timeline semaphores), so this is
 * called from the runtime's submit thread, one submission at a time. */
#include "mali_queue.h"
#include "mali_fprof.h"
#include <time.h>
static double prof_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double prof_acc[6]; static unsigned prof_n;

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kbase_cs.h"
#include "kbase_draw.h"
#include "mali_backend.h"
#include "mali_cmd.h"
#include "mali_event.h"
#include "mali_meta.h"
#include "mali_soft.h"
#include "vk_command_buffer.h"

/* ---- M9: frame resources ---- */
/* Submissions in flight (MALI_QUEUE_DEPTH=1..64, read once per device). An entry is the arena range a submission still reads: the
 * CPU blocks on the oldest one when the list is full, so the depth counts SUBMISSIONS, not displayed frames (an ANGLE frame is
 * several). The list is sized for the maximum; MALI_MAX_OVF keeps two overflow slots per entry. */
#define MALI_QUEUE_DEPTH_MAX 64u
#define MALI_QUEUE_DEPTH_DEFAULT 3u
#define MALI_MAX_OVF (2u * MALI_QUEUE_DEPTH_MAX)
#define MALI_OVF_POOL 64u

/* MALI_QUEUE_STATS=1: once a second (and once at device teardown) one stderr line with what the queue did and where the CPU
 * blocked. Off: one predictable branch per call site. */
enum qs_cause { QS_INFLIGHT, QS_ARENA, QS_OVF, QS_RING, QS_IDLE, QS_SEMWAIT, QS_N };
static const char *const qs_name[QS_N] = {"inflight", "arena", "ovf", "ring", "idle", "semwait"};
struct mali_qstats {
   uint64_t submits, cmdbufs, frames, kicks, kb_kicks, appends, depth_sum, arena_bytes, wraps, ovf_bos, check_fail;
   unsigned depth_max;
   uint64_t blk_n[QS_N], blk_ns[QS_N];
};

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
   /* async submit: frames in flight (retire counter value + arena range [start, end), circular), oldest first */
   struct { uint64_t expected; size_t start, end, bytes; } fl[MALI_QUEUE_DEPTH_MAX];
   unsigned nfl;
   unsigned depth;                /* MALI_QUEUE_DEPTH: entries allowed in fl[] (1..MALI_QUEUE_DEPTH_MAX) */
   bool arena_drain;              /* MALI_QUEUE_DEPTH set: a full arena retires every submission (the behaviour before the default changed) */
   bool check;                    /* MALI_QUEUE_CHECK=1: abort when the arena hands out a range an unretired submission reads */
   size_t cur_start;
   size_t cur_bytes;              /* arena bytes handed out since hw_job_begin */
   struct { bool on; uint64_t t0, t_last; struct mali_qstats win, tot; uint64_t ring_n0, ring_ns0, kb_kicks0; } qs;
   struct mali_bo *old_tls;       /* replaced TLS BOs: freed at teardown, never earlier (in-flight frames AND frames built but not yet submitted, e.g. two direct-draw frames of one command buffer, still hold their VA) */
   unsigned n_old_tls, cap_old_tls;
   /* overflow descriptor memory: a frame that does not fit the rolling arena (even with every frame retired) takes extra BOs, freed once the
    * submission that used them has retired (tag = retire counter value, 0 = not yet submitted) */
   struct { struct mali_bo bo; uint64_t tag; } ovf[MALI_MAX_OVF];
   unsigned n_ovf;
   struct mali_bo ovf_pool[MALI_OVF_POOL];   /* retired overflow BOs kept for the next big frame (mapping and kbase alloc are slow) */
   unsigned n_pool;
   size_t pool_bytes;
   size_t ovf_off, ovf_last;
   uint32_t meta_sz[16];          /* byte size of each meta_sh[] upload (dedupe on re-bind) */
   int light_pending;             /* see mali_env.light_pending */
   struct mali_hw hw;             /* what mali_cmd/mali_meta singletons are bound to while this device is the current one */
   struct mali_meta_dev md;
   struct mali_fprof fp;          /* M15: MALI_FRAME_PROF; off (all no-ops) unless the env var asks for it */
   struct mali_hwctx *below;      /* previously current device (stack of live devices) */
};

/* mali_cmd.c / mali_meta.c keep process-global bindings to ONE device. dEQP (and apps) create short-lived extra devices
 * while the main one stays alive: destroying the extra one must re-bind the singletons to the one below it, not unbind them
 * (that made vkCreateEvent/meta allocations of the surviving device fail with VK_ERROR_OUT_OF_DEVICE_MEMORY). */
static pthread_mutex_t g_bind_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mali_hwctx *g_cur;

static void
bind_hw(struct mali_hwctx *h)
{
   if (h) {
      mali_cmd_set_hw(&h->hw);
      mali_cmd_set_res(&mali_cmd_res);
      mali_meta_set_dev(&h->md);
      /* Not on the fake backend: the goldens dump its whole BO table, and the compile BOs a background prewarm allocates would land in it
         at a thread-timing-dependent index. The fake device has no first-use hitch to hide. */
      if (mali_device_pdev(h->dev)->backend != &mali_backend_fake)
         mali_meta_prewarm();
   } else {
      mali_cmd_set_hw(NULL);
      mali_cmd_set_res(NULL);
      mali_meta_set_dev(NULL);
   }
}

/* M15: per-BO write tracking for the fence-wait CSYNC (mali_backend.h bo_wrote, vk/MEMORY.md). */
static void
hw_bo_wrote(void *user, struct mali_bo *bo)
{
   struct mali_hwctx *h = user;
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   if (be->bo_wrote)
      be->bo_wrote(h->dev->kdev, bo);
}

/* ---- M12/M13 device seam of mali_meta.c: long-lived zeroed memory (one uncached BO each, 64 B header keeps the struct mali_bo) and
 * executable shader memory ---- */
#define META_HDR 64u
struct meta_hdr {
   struct mali_bo bo;
};
_Static_assert(sizeof(struct meta_hdr) <= META_HDR, "meta header");

/* Device-scoped: the BO is allocated and freed through the device the caller names, never through the process-global binding
 * (g_cur) and never through a pointer stored in the BO (which could dangle once another device is destroyed). */
void *
mali_hw_meta_alloc(struct mali_device *dev, size_t size, size_t align, uint64_t *va)
{
   struct meta_hdr hdr;
   if (!dev || mali_device_pdev(dev)->backend->bo_alloc(dev->kdev, size + META_HDR, MALI_BO_COH, &hdr.bo) != 0)
      return NULL;
   struct mali_bo bo = hdr.bo;
   memcpy(bo.cpu, &hdr, sizeof hdr);
   *va = bo.gpu_va + META_HDR;
   return (char *)bo.cpu + META_HDR;
}

void
mali_hw_meta_free(struct mali_device *dev, void *cpu)
{
   struct meta_hdr hdr;
   if (!cpu)
      return;
   memcpy(&hdr, (char *)cpu - META_HDR, sizeof hdr);
   mali_device_pdev(dev)->backend->bo_free(dev->kdev, &hdr.bo);
}

static void *
meta_alloc(void *u, size_t size, size_t align, uint64_t *va)
{
   struct mali_hwctx *h = u;
   return mali_hw_meta_alloc(h->dev, size, align, va);
}

static void
meta_free(void *u, void *cpu, uint64_t va, size_t size)
{
   struct mali_hwctx *h = u;
   mali_hw_meta_free(h->dev, cpu);
}

static int
meta_upload_shader(void *u, const uint8_t *bin, uint32_t size, uint64_t *va)
{
   struct mali_hwctx *h = u;
   if (h->n_meta_sh >= 16)
      return -ENOMEM;
   for (unsigned i = 0; i < h->n_meta_sh; i++)   /* re-bind after another device was destroyed: reuse the identical upload */
      if (h->meta_sz[i] == size && !memcmp(h->meta_sh[i].cpu, bin, size)) {
         *va = h->meta_sh[i].gpu_va;
         return 0;
      }
   struct mali_bo *bo = &h->meta_sh[h->n_meta_sh];
   h->meta_sz[h->n_meta_sh] = size;
   if (mali_device_pdev(h->dev)->backend->bo_alloc(h->dev->kdev, size, MALI_BO_EXEC, bo))
      return -ENOMEM;
   h->n_meta_sh++;
   memcpy(bo->cpu, bin, size);
   *va = bo->gpu_va;
   return 0;
}

static uint64_t g_alloc_bytes;   /* test statistic: bytes asked of hw_alloc (MALI_ALLOC_STATS=<file> writes the running total at each submit) */
static uint64_t g_zero_bytes;    /* test statistic: bytes of those that hw_alloc memset to zero (the second number in that file) */

#define ARENA_BYTES (32u << 20)   /* per-frame descriptor arena: ~4.5 KB per draw (two 2 KiB driver sets); 1 MiB capped a frame at ~230 draws */
#define HEAP_CHUNK (2u << 20)

static int hw_wait(struct mali_hwctx *h, uint64_t target, enum qs_cause why);
static void hw_retire(struct mali_hwctx *h, unsigned max_left, enum qs_cause why);

static uint64_t
qs_now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void
qs_block(struct mali_hwctx *h, enum qs_cause why, uint64_t ns)
{
   h->qs.win.blk_n[why]++;
   h->qs.win.blk_ns[why] += ns;
}

static void
qs_print(const struct mali_hwctx *h, const struct mali_qstats *s, const char *what, double secs)
{
   char b[640];
   int o = snprintf(b, sizeof b, "mali_queue_stats %s %.2fs depth_limit %u submits %llu cmdbufs %llu frames %llu frames_per_submit %.2f "
                    "kicks %llu kb_kicks %llu inflight_max %u inflight_avg %.2f arena_kb %llu wraps %llu ovf_bos %llu check_fail %llu", what, secs, h->depth,
                    (unsigned long long)s->submits, (unsigned long long)s->cmdbufs, (unsigned long long)s->frames,
                    s->submits ? (double)s->frames / s->submits : 0.0, (unsigned long long)s->kicks, (unsigned long long)s->kb_kicks,
                    s->depth_max, s->appends ? (double)s->depth_sum / s->appends : 0.0, (unsigned long long)(s->arena_bytes >> 10),
                    (unsigned long long)s->wraps, (unsigned long long)s->ovf_bos, (unsigned long long)s->check_fail);
   for (unsigned i = 0; i < QS_N && o > 0 && (size_t)o < sizeof b; i++)
      o += snprintf(b + o, sizeof b - o, " blk_%s %llu/%.2fms", qs_name[i], (unsigned long long)s->blk_n[i], s->blk_ns[i] / 1e6);
   fprintf(stderr, "%s\n", b);
}

static void
qs_add(struct mali_qstats *t, const struct mali_qstats *w)
{
   t->submits += w->submits; t->cmdbufs += w->cmdbufs; t->frames += w->frames; t->kicks += w->kicks; t->kb_kicks += w->kb_kicks;
   t->appends += w->appends; t->depth_sum += w->depth_sum; t->arena_bytes += w->arena_bytes; t->wraps += w->wraps; t->ovf_bos += w->ovf_bos; t->check_fail += w->check_fail;
   if (w->depth_max > t->depth_max)
      t->depth_max = w->depth_max;
   for (unsigned i = 0; i < QS_N; i++) {
      t->blk_n[i] += w->blk_n[i];
      t->blk_ns[i] += w->blk_ns[i];
   }
}

/* Fold the backend's counters into the window; print and restart it once a second (or always when `final`). */
static void
qs_tick(struct mali_hwctx *h, bool final)
{
   if (!h->qs.on)
      return;
   uint64_t rn = __atomic_load_n(&mali_qstat_ring_n, __ATOMIC_RELAXED), rns = __atomic_load_n(&mali_qstat_ring_ns, __ATOMIC_RELAXED);
   uint64_t kk = __atomic_load_n(&kb_stat_kicks, __ATOMIC_RELAXED);
   h->qs.win.blk_n[QS_RING] += rn - h->qs.ring_n0;
   h->qs.win.blk_ns[QS_RING] += rns - h->qs.ring_ns0;
   h->qs.win.kb_kicks += kk - h->qs.kb_kicks0;
   h->qs.ring_n0 = rn, h->qs.ring_ns0 = rns, h->qs.kb_kicks0 = kk;
   uint64_t now = qs_now();
   if (!final && now - h->qs.t_last < 1000000000ull)
      return;
   qs_print(h, &h->qs.win, "window", (now - h->qs.t_last) / 1e9);
   qs_add(&h->qs.tot, &h->qs.win);
   memset(&h->qs.win, 0, sizeof h->qs.win);
   h->qs.t_last = now;
   if (final)
      qs_print(h, &h->qs.tot, "total", (now - h->qs.t0) / 1e9);
}

/* MALI_QUEUE_CHECK: [o, o+n) must not meet the arena range of a submission the GPU has not retired. */
static bool
range_meets(size_t o, size_t n, size_t s, size_t e, size_t bytes, size_t size)
{
   if (!bytes)
      return false;
   if (s < e)
      return o < e && o + n > s;
   if (s == e)
      return true;   /* the whole arena */
   return o + n > s || o < e;   /* wrapped: [s, size) and [0, e) */
}

static void
arena_check(struct mali_hwctx *h, size_t o, size_t n)
{
   uint64_t c = *(volatile uint64_t *)h->sync.cpu;
   for (unsigned i = 0; i < h->nfl; i++)
      if (h->fl[i].expected > c && range_meets(o, n, h->fl[i].start, h->fl[i].end, h->fl[i].bytes, h->arena.size)) {
         if (h->qs.win.check_fail++ < 8)   /* counted (stats line), not fatal: the test reads the report */
            fprintf(stderr, "mali_queue: CHECK FAILED: arena [%zx,+%zx) handed out while submission %llu (counter %llu) still reads [%zx,%zx) "
                    "(%u in flight, limit %zx lapped %d)\n", o, n, (unsigned long long)h->fl[i].expected, (unsigned long long)c,
                    h->fl[i].start, h->fl[i].end, h->nfl, h->ar.start, h->ar.lapped);
         break;
      }
}

/* Bound the current submission's arena allocations by `limit`, the start of the oldest range a submission in flight still reads (or,
 * with none left, the current submission's own start). The free space is the circular interval [off, limit): linear when limit is
 * ahead of off (kb_arena "lapped": bounded by start), otherwise the tail of the BO and then [0, limit). limit == off is either
 * nothing free (`full`: something in flight, or this submission went all the way round) or the whole arena. Every submission starts
 * 64-byte aligned, so kb_arena_begin rounding off up never passes a start; it can land exactly on one, and then the old
 * "lapped = 0" handed the whole arena out over the submission still in flight (MALI_QUEUE_CHECK found it with a 64 KiB arena). */
static void
arena_limit(struct mali_hwctx *h, size_t limit, bool full)
{
   h->ar.start = limit;
   h->ar.lapped = limit > h->ar.off ? 1 : limit < h->ar.off ? 0 : full;
}

static void *
hw_alloc_ovf(struct mali_hwctx *h, size_t n, size_t al, uint64_t *va, bool zero)
{
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   struct mali_bo *cur = h->n_ovf && !h->ovf[h->n_ovf - 1].tag ? &h->ovf[h->n_ovf - 1].bo : NULL;
   size_t o = cur ? (h->ovf_off + al - 1) & ~(al - 1) : 0;
   if (!cur || o + n > cur->size) {
      if (h->n_ovf == MALI_MAX_OVF) {   /* all slots taken by submitted ones: wait for them, then reuse */
         hw_retire(h, 0, QS_OVF);
         if (h->n_ovf == MALI_MAX_OVF)
            return NULL;
      }
      size_t sz = h->ovf_last ? h->ovf_last * 2 : (16u << 20);
      if (sz < n + al)
         sz = (n + al + (1u << 20) - 1) & ~((size_t)(1u << 20) - 1);
      if (sz > (512u << 20))
         sz = n + al > (512u << 20) ? n + al : (512u << 20);
      unsigned best = h->n_pool;
      for (unsigned i = 0; i < h->n_pool; i++)
         if (h->ovf_pool[i].size >= sz && h->ovf_pool[i].size <= 2 * sz && (best == h->n_pool || h->ovf_pool[i].size < h->ovf_pool[best].size))
            best = i;
      if (best < h->n_pool) {
         h->ovf[h->n_ovf].bo = h->ovf_pool[best];
         h->pool_bytes -= h->ovf_pool[best].size;
         h->ovf_pool[best] = h->ovf_pool[--h->n_pool];
         sz = h->ovf[h->n_ovf].bo.size;
      } else if (be->bo_alloc(h->dev->kdev, sz, MALI_BO_HOT, &h->ovf[h->n_ovf].bo))
         return NULL;
      h->ovf[h->n_ovf].tag = 0;
      cur = &h->ovf[h->n_ovf++].bo;
      h->qs.win.ovf_bos++;
      h->ovf_last = sz;
      o = 0;
   }
   h->ovf_off = o + n;
   if (zero) {
      memset((char *)cur->cpu + o, 0, n);
      g_zero_bytes += n;
   }
   if (va)
      *va = cur->gpu_va + o;
   return (char *)cur->cpu + o;
}



static void *
hw_alloc_x(void *u, size_t n, size_t al, uint64_t *va, bool zero)
{
   struct mali_hwctx *h = u;
   g_alloc_bytes += n;
   if (h->n_ovf && !h->ovf[h->n_ovf - 1].tag)   /* this submission already spilled: stay in order */
      return hw_alloc_ovf(h, n, al, va, zero);
   const size_t off0 = h->ar.off;
   void *p = kb_arena_alloc_nz(&h->ar, n, al, va);
   while (!p && h->nfl) {
      /* Arena full of in-flight submissions. Default: wait for the oldest only, so the CPU stays one range behind the GPU instead of
       * draining the queue (the arena's bytes, not a count, bound how far ahead it runs). MALI_QUEUE_DEPTH set: the old behaviour,
       * retire them all. A wait that could not retire anything (lost device, timeout) leaves the limit where it is: the overflow BOs
       * take the rest. */
      const unsigned before = h->nfl;
      hw_retire(h, h->arena_drain ? 0 : before - 1, QS_ARENA);
      if (h->nfl == before)
         break;
      arena_limit(h, h->nfl ? h->fl[0].start : h->cur_start, h->nfl || h->cur_bytes);
      p = kb_arena_alloc_nz(&h->ar, n, al, va);
   }
   if (!p)
      return hw_alloc_ovf(h, n, al, va, zero);
   h->cur_bytes += n;
   if (h->qs.on) {
      h->qs.win.arena_bytes += n;
      h->qs.win.wraps += (size_t)((char *)p - (char *)h->arena.cpu) < off0;
   }
   if (h->check)
      arena_check(h, (size_t)((char *)p - (char *)h->arena.cpu), n);
   if (zero) {
      memset(p, 0, n);
      g_zero_bytes += n;
   }
   return p;
}

static void *
hw_alloc(void *u, size_t n, size_t al, uint64_t *va)
{
   return hw_alloc_x(u, n, al, va, true);
}

static void *
hw_alloc_nz(void *u, size_t n, size_t al, uint64_t *va)
{
   return hw_alloc_x(u, n, al, va, false);
}

static int
hw_wait(struct mali_hwctx *h, uint64_t target, enum qs_cause why)
{
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   const bool timed = h->qs.on && *(volatile uint64_t *)h->sync.cpu < target;
   const uint64_t t0 = timed ? qs_now() : 0;
   int r = be->wait_counter(h->dev->kdev, (volatile uint64_t *)h->sync.cpu, target, h->wait_ms);
   if (timed)
      qs_block(h, why, qs_now() - t0);
   if (r == 0 && h->fp.on)
      mali_fprof_retired(&h->fp, h->dev, target);   /* M15: prompt retire observation (the CPU is blocked here anyway) */
   return r;
}

static void
hw_ovf_reap(struct mali_hwctx *h, uint64_t counter)
{
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   unsigned w = 0;
   for (unsigned i = 0; i < h->n_ovf; i++) {
      if (h->ovf[i].tag && h->ovf[i].tag <= counter) {
         if (h->n_pool < MALI_OVF_POOL && h->pool_bytes + h->ovf[i].bo.size <= (256u << 20)) {
            h->ovf_pool[h->n_pool++] = h->ovf[i].bo;
            h->pool_bytes += h->ovf[i].bo.size;
         } else
            be->bo_free(h->dev->kdev, &h->ovf[i].bo);
      } else
         h->ovf[w++] = h->ovf[i];
   }
   h->n_ovf = w;
}

/* Drop retired frames from the in-flight list; block on the oldest while the list is full. */
static void
hw_retire(struct mali_hwctx *h, unsigned max_left, enum qs_cause why)
{
   for (;;) {
      uint64_t c = *(volatile uint64_t *)h->sync.cpu;
      hw_ovf_reap(h, c);
      unsigned k = 0;
      while (k < h->nfl && h->fl[k].expected <= c)
         k++;
      if (k && h->fp.on)
         mali_fprof_retired(&h->fp, h->dev, c);   /* M15: the retire this pass can see (one GPU-clock sample) */
      if (k) {
         memmove(h->fl, h->fl + k, (h->nfl - k) * sizeof h->fl[0]);
         h->nfl -= k;
      }
      if (h->nfl <= max_left)
         return;
      if (hw_wait(h, h->fl[0].expected, why))
         return;
   }
}

static void
hw_drain(void *u)
{
   struct mali_hwctx *h = u;
   if (h->nfl)
      hw_wait(h, h->fl[h->nfl - 1].expected, QS_IDLE);
   hw_retire(h, 0, QS_IDLE);
}

static void
hw_job_begin(void *u)
{
   struct mali_hwctx *h = u;
   hw_retire(h, h->depth - 1, QS_INFLIGHT);
   kb_arena_begin(&h->ar);
   h->cur_start = h->ar.start;
   h->cur_bytes = 0;
   if (h->nfl && !getenv("MALI_SYNC_SUBMIT"))
      arena_limit(h, h->fl[0].start, true);   /* the new frame must stay clear of the oldest in-flight frame's arena range */
}

static void *
hw_tls(void *u, size_t size, uint64_t *va)
{
   struct mali_hwctx *h = u;
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   if (!h->tls_live || h->tls.size < size) {
      struct mali_bo nb;
      /* A replaced buffer is kept until teardown: a frame built earlier (not submitted yet, so hw_retire cannot wait for it) records its VA.
       * Growing to at least twice the current size bounds the list to log2(max size) entries. */
      if (h->tls_live && size < (size_t)h->tls.size * 2)
         size = (size_t)h->tls.size * 2;
      if (h->tls_live && h->n_old_tls == h->cap_old_tls) {
         unsigned nc = h->cap_old_tls ? h->cap_old_tls * 2 : 8;
         struct mali_bo *no = realloc(h->old_tls, nc * sizeof *no);
         if (!no)
            return NULL;
         h->old_tls = no;
         h->cap_old_tls = nc;
      }
      if (be->bo_alloc(h->dev->kdev, size, 0, &nb))
         return NULL;
      if (h->tls_live)
         h->old_tls[h->n_old_tls++] = h->tls;
      h->tls = nb;
      h->tls_live = true;
   }
   *va = h->tls.gpu_va;
   return h->tls.cpu;
}

/* MALI_SOFT=1: the desktop software rasteriser (mali_soft.c) renders each frame from its record instead of encoding a
 * command stream. A frame it cannot do fails the submit (-ENOTSUP in mali_frame_build), never silently draws nothing. */
const struct mali_hw *
mali_soft_device_hw(VkDevice dev_handle)
{
   VK_FROM_HANDLE(mali_device, dev, dev_handle);
   struct mali_hwctx *h = dev ? dev->hw : NULL;
   return h ? &h->hw : NULL;
}

static bool
hw_soft(void *u, const struct mali_frame *f)
{
   struct mali_hwctx *h = u;
   return mali_soft_frame(&h->hw, f);
}

static void *
hw_va_cpu(void *u, uint64_t va, size_t size)
{
   struct mali_hwctx *h = u;
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   return be->va_cpu ? be->va_cpu(h->dev->kdev, va, size) : NULL;
}

static int
hw_submit_multi(void *u, const uint64_t *cs, unsigned n, unsigned nframes)
{
   struct mali_hwctx *h = u;
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   if (getenv("MALI_CSDUMP") && n == 27) { for (unsigned i = 0; i < n; i++) fprintf(stderr, "CS %02u %016llx\n", i, (unsigned long long)cs[i]); }
   {
      const char *sf = getenv("MALI_ALLOC_STATS");
      FILE *fp = sf ? fopen(sf, "w") : NULL;
      if (fp) {
         fprintf(fp, "%llu %llu\n", (unsigned long long)g_alloc_bytes, (unsigned long long)g_zero_bytes);
         fclose(fp);
      }
   }
   /* M15 (MALI_FRAME_PROF): `pending` is what the GPU had *not* retired at the kick, read at the kick (lag-free).
    * submit_us below covers only the backend kick; the command stream was built before this call. */
   if (h->fp.on) {
      uint64_t done = *(volatile uint64_t *)h->sync.cpu;
      mali_fprof_kick(&h->fp, h->dev, h->expected + nframes, h->expected > done ? (unsigned)(h->expected - done) : 0);
   }
   double q0 = prof_now();
   int r = be->submit_cs(h->dev->kdev, cs, n);
   if (h->qs.on) {
      h->qs.win.kicks++;
      h->qs.win.frames += nframes;
   }
   if (h->fp.on && r == 0)
      mali_fprof_kicked(&h->fp, prof_now() - q0);
   if (getenv("MALI_PROF")) {
      static double acc; static unsigned cnt;
      acc += prof_now() - q0;
      if (++cnt % 200 == 0) fprintf(stderr, "PROF/hw_submit submit_cs avg %.3f ms (n=%u instr)\n", acc / cnt, n);
   }
   if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: submit_cs n=%u arena off=%zx start=%zx rc=%d\n", n, h->ar.off, h->ar.start, r);
   if (r)
      return r;
   /* The arena is rolling and must not hand out the VAs of an in-flight frame (INTERMITTENT.md): every frame retires
    * (CS SYNC_ADD64 on the event word) before the next one is built. */
   h->expected += nframes;
   for (unsigned i = 0; i < h->n_ovf; i++)
      if (!h->ovf[i].tag)
         h->ovf[i].tag = h->expected;
   h->ovf_last = 0;   /* the next frame's overflow sequence starts small again (and finds its BOs in the pool) */
   if (!getenv("MALI_SYNC_SUBMIT")) {
      /* The list is bounded (h->depth) and hw_job_begin only guarantees nfl <= depth-1 when it runs, which is not
       * enough: ONE command buffer can submit several times (an indirect or CPU-polygon frame flushes the frames
       * accumulated before it, mali_cmd_submit), and a drain whose wait fails leaves the entries in place, so the next
       * append wrote h->fl[h->nfl] one past the array -- over h->nfl/h->cur_start themselves (mali_queue.c, audit-sync
       * #1). Retire before appending, and wait for the oldest frame if retiring did not free a slot: an entry is the
       * arena range a submission still reads, so it is never dropped. A wait that cannot reach the oldest frame is a
       * lost device and is reported, not skipped. */
      if (h->nfl >= h->depth) {
         hw_retire(h, h->depth - 1, QS_INFLIGHT);
         if (h->nfl >= h->depth && hw_wait(h, h->fl[0].expected, QS_INFLIGHT))
            return -EIO;
         if (h->nfl >= h->depth)
            hw_retire(h, h->depth - 1, QS_INFLIGHT);
         if (h->nfl >= h->depth)
            return -EIO;   /* the counter never reached the oldest frame: device lost */
      }
      if (getenv("MALI_FL_DBG")) fprintf(stderr, "mali_queue: fl append nfl=%u cap=%u expected=%llu\n", h->nfl, h->depth, (unsigned long long)h->expected);
      h->fl[h->nfl].expected = h->expected;
      h->fl[h->nfl].start = h->cur_start;
      h->fl[h->nfl].end = h->ar.off;
      h->fl[h->nfl].bytes = h->cur_bytes;
      h->nfl++;
      if (h->qs.on) {
         uint64_t c = *(volatile uint64_t *)h->sync.cpu;
         unsigned live = 0;
         for (unsigned i = 0; i < h->nfl; i++)
            live += h->fl[i].expected > c;
         h->qs.win.appends++;
         h->qs.win.depth_sum += live;
         if (live > h->qs.win.depth_max)
            h->qs.win.depth_max = live;
      }
      return 0;
   }
   double w0 = prof_now();
   int wr = hw_wait(h, h->expected, QS_IDLE);
   if (getenv("MALI_PROF")) { static double acc[2]; static unsigned cnt; acc[0] += prof_now() - w0; if (++cnt % 100 == 0) fprintf(stderr, "PROF/syncwait frame GPU latency avg %.3f ms (n=%u instr)\n", acc[0] / cnt, n); }
   if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk:   wait rc=%d counter=%llu expected=%llu\n", wr, (unsigned long long)*(volatile uint64_t *)h->sync.cpu, (unsigned long long)h->expected);
   return wr;
}

static int
hw_submit(void *u, const uint64_t *cs, unsigned n)
{
   return hw_submit_multi(u, cs, n, 1);
}

/* MALI_QUEUE_DEPTH: unset = the default; otherwise a whole number 1..MALI_QUEUE_DEPTH_MAX. Anything else is refused (-1) rather than
 * clamped, so an A/B run can never silently measure a different setting than the one it names. */
int
mali_queue_depth_env(unsigned *out)
{
   const char *e = getenv("MALI_QUEUE_DEPTH");
   *out = MALI_QUEUE_DEPTH_DEFAULT;
   if (!e || !e[0])
      return 0;
   char *end;
   errno = 0;
   unsigned long v = strtoul(e, &end, 10);
   if (errno || *end || e[0] == '-' || v < 1 || v > MALI_QUEUE_DEPTH_MAX)
      return -1;
   *out = (unsigned)v;
   return 0;
}

void
mali_hw_finish(struct mali_device *dev)
{
   struct mali_hwctx *h = dev->hw;
   const struct mali_backend_ops *be = mali_device_pdev(dev)->backend;

   if (!h)
      return;
   if (h->expected)
      hw_wait(h, h->expected, QS_IDLE);
   qs_tick(h, true);
   for (unsigned i = 0; i < h->n_old_tls; i++)
      be->bo_free(dev->kdev, &h->old_tls[i]);
   free(h->old_tls);
   h->old_tls = NULL;
   h->n_old_tls = h->cap_old_tls = 0;
   for (unsigned i = 0; i < h->n_ovf; i++)
      be->bo_free(dev->kdev, &h->ovf[i].bo);
   h->n_ovf = 0;
   for (unsigned i = 0; i < h->n_pool; i++)
      be->bo_free(dev->kdev, &h->ovf_pool[i]);
   h->n_pool = 0;
   pthread_mutex_lock(&g_bind_lock);
   if (g_cur == h) {
      g_cur = h->below;
      bind_hw(g_cur);
   } else {
      for (struct mali_hwctx *c = g_cur; c; c = c->below)
         if (c->below == h) {
            c->below = h->below;
            break;
         }
   }
   pthread_mutex_unlock(&g_bind_lock);
   for (unsigned i = 0; i < h->n_meta_sh; i++)
      be->bo_free(dev->kdev, &h->meta_sh[i]);
   if (h->heap_live)
      be->heap_destroy(dev->kdev, &h->heap);
   mali_fprof_finish(&h->fp);
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
   const unsigned arch = pdev->gpu.arch_major >= 12 ? 13 : pdev->gpu.arch_major == 10 ? 10 : 11;
   kb_v10 = arch == 10;   /* libkbase: SET_SB_ENTRY scoreboard select, ZSD bit 21 (see kbase_cs.h) */

   if (!h)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (mali_queue_depth_env(&h->depth)) {
      fprintf(stderr, "mali_vk: MALI_QUEUE_DEPTH=%s refused: expected an integer 1..%u; device not created\n", getenv("MALI_QUEUE_DEPTH"),
              MALI_QUEUE_DEPTH_MAX);
      free(h);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   h->dev = dev;
   dev->hw = h;
   mali_fprof_init(&h->fp, dev);
   const char *t = getenv("MALI_FRAME_TIMEOUT_MS");
   h->wait_ms = t ? (unsigned)strtoul(t, NULL, 0) : 10000;
   h->arena_drain = true;   /* a full arena retires every submission in flight */
   h->check = getenv("MALI_QUEUE_CHECK") && getenv("MALI_QUEUE_CHECK")[0] == '1';
   const char *qse = getenv("MALI_QUEUE_STATS");
   h->qs.on = qse && qse[0] == '1';
   h->qs.t0 = h->qs.t_last = qs_now();
   h->qs.ring_n0 = __atomic_load_n(&mali_qstat_ring_n, __ATOMIC_RELAXED);
   h->qs.ring_ns0 = __atomic_load_n(&mali_qstat_ring_ns, __ATOMIC_RELAXED);
   h->qs.kb_kicks0 = __atomic_load_n(&kb_stat_kicks, __ATOMIC_RELAXED);
   /* MALI_ARENA_KB (tests, A/B): arena size in KiB, 64..1048576; default ARENA_BYTES */
   const char *akb = getenv("MALI_ARENA_KB");
   unsigned long arena_bytes = ARENA_BYTES;
   if (akb && akb[0]) {
      char *end;
      unsigned long v = strtoul(akb, &end, 10);
      if (*end || v < 64 || v > (1ul << 20)) {
         fprintf(stderr, "mali_vk: MALI_ARENA_KB=%s refused: expected 64..1048576; device not created\n", akb);
         mali_hw_finish(dev);
         return VK_ERROR_INITIALIZATION_FAILED;
      }
      arena_bytes = v << 10;
   }
   if (be->bo_alloc(dev->kdev, arena_bytes, MALI_BO_HOT, &h->arena))
      goto fail;
   h->arena_live = true;
   if (be->bo_alloc(dev->kdev, 4096 + 65536, MALI_BO_COH, &h->geo)   /* tiler heap descriptor: the tiler updates it from several cores; coverage 0 at random without COHERENT_LOCAL (arch 11) */)
      goto fail;
   h->geo_live = true;
   if (be->bo_alloc(dev->kdev, 4096, MALI_BO_EVENT, &h->sync))
      goto fail;
   h->sync_live = true;
   if (be->heap_create(dev->kdev, getenv("MALI_HEAP_CHUNK") ? (uint32_t)atoi(getenv("MALI_HEAP_CHUNK")) : HEAP_CHUNK,
                       getenv("MALI_HEAP_INIT") ? (uint32_t)atoi(getenv("MALI_HEAP_INIT")) : 4, 64, &h->heap))
      goto fail;
   h->heap_live = true;
   h->ar.bo.cpu = h->arena.cpu;
   h->ar.bo.gpu_va = h->arena.gpu_va;
   h->ar.bo.size = h->arena.size;

   /* Tiler hierarchy mask: derived from the tile the FBD actually carries instead of the literal 0xfe, so the two cannot
    * drift apart. mali_render.c sets the FBD "Effective Tile Size" from env->tile_log2 only on v13 (64x64) and for 4x MSAA
    * (16x16); on v11 kb_fb_pack's default 32x32 applies, where 0xfe is the matching mask (levels 32x32 and up). v13 keeps
    * 0: PanVK prefers letting the hardware decide there (pan_desc.c: "On v12+, the hierarchy_mask is deprecated"), and that
    * is the value proven on the X8. MALI_HMASK_OLD=1 restores the old literal 0xfe on every arch for the phone A/B. */
   const char *hmo = getenv("MALI_HMASK_OLD");
   const bool hmask_old = hmo && hmo[0] == '1';
   const unsigned hmask = hmask_old ? 0xfe : (arch == 13 ? 0 : kb_tiler_hmask_from_tile(10));
   if (getenv("MALI_PIPE_DBG"))
      fprintf(stderr, "mali_vk: tiler hmask 0x%x (arch %u, FBD tile %ux%u)%s\n", hmask, arch,
              arch == 13 ? 64 : 32, arch == 13 ? 64 : 32, hmask_old ? ", MALI_HMASK_OLD=1" : "");
   struct mali_hw hw = {.user = h, .alloc = hw_alloc, .alloc_nz = hw_alloc_nz, .job_begin = hw_job_begin, .submit = hw_submit, .submit_multi = hw_submit_multi, .tls = hw_tls, .drain = hw_drain, .bo_wrote = hw_bo_wrote};
   hw.env = (struct mali_env){.arch = arch,
                              .hmask = hmask,
                              .tile_log2 = 12,
                              .slot = 3,
                              .tf_or = arch == 13 ? 0x40000 : 0,
                              .heap_ctx_va = h->heap.ctx_va,
                              .heap_first_chunk_va = h->heap.first_chunk_va,
                              .heap_chunk_size = h->heap.chunk_size,
                              .geo_cpu = h->geo.cpu,
                              .geo_va = h->geo.gpu_va,
                              .sync_va = h->sync.gpu_va,
                              .light_pending = &h->light_pending,
                              .core_id_range = pdev->gpu.core_id_range,
                              .tls_threads = pdev->gpu.tls_threads};
   kb_draw_tiler_heap_desc(h->geo.cpu, h->heap.first_chunk_va, h->heap.chunk_size);
   hw.env.heap_desc_ready = true;
   if (getenv("MALI_SOFT") && getenv("MALI_SOFT")[0] && strcmp(getenv("MALI_SOFT"), "0") != 0 &&
       pdev->backend->va_cpu) {
      hw.soft = hw_soft;
      hw.va_cpu = hw_va_cpu;
   }
   if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: va arena %llx+%zx geo %llx sync %llx heap ctx %llx first %llx\n", (unsigned long long)h->arena.gpu_va, (size_t)h->arena.size, (unsigned long long)h->geo.gpu_va, (unsigned long long)h->sync.gpu_va, (unsigned long long)h->heap.ctx_va, (unsigned long long)h->heap.first_chunk_va);
   h->hw = hw;
   h->md = (struct mali_meta_dev){.user = h, .arch = arch, .alloc = meta_alloc, .free = meta_free, .upload_shader = meta_upload_shader};
   pthread_mutex_lock(&g_bind_lock);
   h->below = g_cur;
   g_cur = h;
   bind_hw(h);
   pthread_mutex_unlock(&g_bind_lock);
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
   if (dev->present_fence_fd >= 0)
      close(dev->present_fence_fd);
   dev->present_fence_fd = -1;
   pthread_cond_destroy(&dev->sync_cv);
   pthread_mutex_destroy(&dev->sync_lock);
}

int
mali_queue_last_fence_dup(struct mali_device *dev)
{
   if (!dev)
      return -1;
   int d = -1;
   pthread_mutex_lock(&dev->sync_lock);
   if (dev->present_fence_fd >= 0)
      d = dup(dev->present_fence_fd);
   pthread_mutex_unlock(&dev->sync_lock);
   return d;
}

/* May a present submission return before the frame it presents has finished on the GPU? Only when a fence travels with
 * the buffer: wsi_common imports the present's sync_file into the swapchain dma-buf (DMA_BUF_IOCTL_IMPORT_SYNC_FILE,
 * needs kernel >= 6.0: CMF 6.1 and poco-x8 6.6 have it, poco-x4's 5.10 returns ENOTTY) and the compositor waits on it.
 * kbase attaches no implicit fence to its dma-bufs itself, so without the ioctl the CPU wait is the only thing
 * keeping the compositor from sampling a half-drawn buffer, and stays. wsi_common's own probe result (made at the
 * first swapchain creation) is the decision; MALI_PRESENT_FENCE=0/1 forces it (tests), MALI_QUEUE_CPU_WAIT=1 = 0. */
static bool
mali_present_fence_ok(struct mali_device *dev)
{
   static int forced = -2;
   if (forced == -2) {
      const char *e = getenv("MALI_PRESENT_FENCE");
      forced = getenv("MALI_QUEUE_CPU_WAIT") ? 0 : e ? atoi(e) : -1;
   }
   if (forced >= 0)
      return forced;
   const struct wsi_device *w = &mali_device_pdev(dev)->wsi;
   if (w->sw)
      return 0;   /* software present: the CPU copies the frame, so the queue must wait for rendering first */
   return __atomic_load_n(&w->cached_sync_file_import_export_result, __ATOMIC_RELAXED) == UINT32_MAX;   /* wsi stores result - 1; VK_SUCCESS -> UINT32_MAX */
}

static VkResult mali_queue_submit_impl(struct vk_queue *vq, struct vk_queue_submit *submit, uint32_t *imported);

/* A submission that loses the queue still signals what it was to signal, except the signals that already took the
 * GPU fence (index < imported): those complete with the work. Waiters (a present's acquire, a fence wait) would
 * otherwise block on a sync that nothing completes; with the queue lost they then observe DEVICE_LOST instead of
 * hanging. A failure that does not lose the queue signals nothing (the work may still be pending). */
static VkResult
mali_queue_submit(struct vk_queue *vq, struct vk_queue_submit *submit)
{
   uint32_t imported = 0;
   VkResult res = mali_queue_submit_impl(vq, submit, &imported);
   if (res != VK_SUCCESS && vk_queue_is_lost(vq)) {
      struct vk_device *vkdev = vq->base.device;
      for (uint32_t i = imported; i < submit->signal_count; i++) {
         VkResult sr = vk_sync_signal(vkdev, submit->signals[i].sync, submit->signals[i].signal_value);
         (void)sr; /* best effort: the queue is lost already */
      }
   }
   return res;
}

static VkResult
mali_queue_submit_impl(struct vk_queue *vq, struct vk_queue_submit *submit, uint32_t *imported)
{
   struct mali_device *dev = container_of(vq->base.device, struct mali_device, vk);
   const struct mali_backend_ops *be = mali_device_pdev(dev)->backend;

   if (vk_queue_is_lost(vq))
      return VK_ERROR_DEVICE_LOST;
   if (vk_queue_submit_has_bind(submit))
      return vk_error(vq, VK_ERROR_FEATURE_NOT_PRESENT); /* sparse: not supported */

   double p0 = prof_now();
   /* Every wait is a CPU wait for now (this queue is the only producer, external sync_files are the
    * rare case); the GPU-side wait is M8 work. */
   for (uint32_t i = 0; i < submit->wait_count; i++) {
      const struct vk_sync_wait *w = &submit->waits[i];
      /* A semaphore signalled by an earlier submission of THIS queue needs no CPU wait: the queue is one in-order
       * CS ring, so everything after it already runs behind that work. Waiting here made vkQueuePresent block
       * until the GPU finished the frame it presents, which serialised CPU and GPU whatever the frames in flight.
       * Only valid while the present carries the frame's fence to the compositor (mali_present_fence_ok). */
      if (w->sync->type == &mali_sync_type && mali_present_fence_ok(dev)) {
         struct mali_sync *ws = container_of(w->sync, struct mali_sync, vk);
         pthread_mutex_lock(&dev->sync_lock);
         bool own = ws->owner == vq && ws->fd >= 0;
         pthread_mutex_unlock(&dev->sync_lock);
         if (own)
            continue;
      }
      const uint64_t sw0 = dev->hw && dev->hw->qs.on ? qs_now() : 0;
      VkResult r = vk_sync_wait(&dev->vk, w->sync, w->wait_value, VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      if (sw0)
         qs_block(dev->hw, QS_SEMWAIT, qs_now() - sw0);
      if (r != VK_SUCCESS)
         return vk_queue_set_lost(vq, "queue wait failed: %d", r);
   }

   double p1 = prof_now();
   if (dev->hw && dev->hw->qs.on) {
      dev->hw->qs.win.submits++;
      dev->hw->qs.win.cmdbufs += submit->command_buffer_count;
   }
   if (submit->command_buffer_count && be->host_sync)
      be->host_sync(dev->kdev);
   /* Recorded frames: each is built, submitted and retired in order (mali_cmd_submit -> hw_submit). */
   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      VkResult cr = mali_cmd_submit(submit->command_buffers[i]);
      if (cr == VK_ERROR_DEVICE_LOST && getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: frame submit lost\n");
      if (cr == VK_ERROR_DEVICE_LOST)
         return vk_queue_set_lost(vq, "frame submit failed");
      if (cr != VK_SUCCESS)
         return vk_error(vq, cr);
   }

   double p2 = prof_now();
   int fd = -1;
   /* The fence is only consumed through submit->signals (VkFence, semaphores, QueueWaitIdle and present all signal), so a
    * submission without signals skips the 2 KCPU ioctls, the kick and the fd close. Exception: a light ops frame (event.c)
    * relies on submit_signal's head flush; MALI_FENCE_ALWAYS=1 keeps the old behaviour. */
   static int always = -1;
   if (always < 0)
      always = getenv("MALI_FENCE_ALWAYS") != NULL;
   bool light = mali_ops_take_light(&dev->hw->light_pending);
   int r = 0;
   if (submit->signal_count || light || always) {
      r = be->submit_signal(dev->kdev, &fd);
      if (dev->hw && dev->hw->qs.on)
         dev->hw->qs.win.kicks++;
   }
   if (r != 0) {
      if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: submit_signal failed: %d\n", r);
      return vk_queue_set_lost(vq, "queue submit failed: %d", r);
   }

   VkResult res = VK_SUCCESS;
   for (uint32_t i = 0; i < submit->signal_count && res == VK_SUCCESS; i++) {
      struct vk_sync *s = submit->signals[i].sync;
      res = vk_sync_import_sync_file(&dev->vk, s, fd);
      if (res == VK_SUCCESS && s->type == &mali_sync_type && fd >= 0) {
         pthread_mutex_lock(&dev->sync_lock);
         container_of(s, struct mali_sync, vk)->owner = vq;
         pthread_mutex_unlock(&dev->sync_lock);
      }
      if (res == VK_SUCCESS)
         *imported = i + 1;
   }
   /* Keep a duplicate of this submission's sync_file for the CPU-side present paths (X11 shared memory): they have no
    * CPU-visible semaphore to wait, so they wait this fence before reading the image this submission wrote. */
   if (fd >= 0) {
      int d = dup(fd);
      pthread_mutex_lock(&dev->sync_lock);
      if (dev->present_fence_fd >= 0)
         close(dev->present_fence_fd);
      /* -1 on a failed dup: keeping the previous submission's fence would make the shm present wait a dependency
       * that has nothing to do with the frame it is about to copy. No fence means "present it now". */
      dev->present_fence_fd = d;
      pthread_mutex_unlock(&dev->sync_lock);
   }
   close(fd);
   /* M15: vk_common's present submit has no command buffers, the image acquire semaphores as waits and the render
    * semaphore as signal (an app's fence-only submit has no waits, so it is not counted). */
   if (dev->hw && dev->hw->fp.on && submit->command_buffer_count == 0 && submit->wait_count > 0 && submit->signal_count > 0)
      mali_fprof_present(&dev->hw->fp);
   if (dev->hw)
      qs_tick(dev->hw, false);
   if (res != VK_SUCCESS)
      return vk_queue_set_lost(vq, "queue signal import failed: %d", res); /* GPU work is in flight, its fence is lost */
   if (getenv("MALI_PROF")) {
      double p3 = prof_now();
      prof_acc[0] += p1 - p0; prof_acc[1] += p2 - p1; prof_acc[2] += p3 - p2;
      if (++prof_n % 200 == 0) {
         fprintf(stderr, "PROF/submit(avg ms of %u): waits %.3f cmd_submit %.3f signal+import %.3f\n", prof_n, prof_acc[0] / prof_n, prof_acc[1] / prof_n, prof_acc[2] / prof_n);
      }
   }
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
   dev->present_fence_fd = -1;
   dev->queue.vk.driver_submit = mali_queue_submit;
   dev->vk.check_status = mali_device_check_status;
}
