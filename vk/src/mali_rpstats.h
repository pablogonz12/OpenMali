/* SPDX-License-Identifier: MIT */
/* MALI_RP_STATS=1: which render pass is the first one that comes out black.
 *
 * The X8 (Mali-G925, arch 13) shows Steam's menu black while the X4 and the CMF paint the same build, and
 * MALI_SHARED_STATS showed the first process in the chain (chromium's GPU process, ANGLE GL -> zink -> us) already
 * writes zeros into every shared buffer: it is a v13 rendering defect, not a handoff. This narrows it to one pass.
 *
 * At the end of every pass (dynamic rendering, and the meta paths: clear, copy, blit, resolve) the pass is recorded
 * with what it draws into (BO, extent, format, load/store, resolve), how many draws it had and a compact signature of
 * the pipelines those draws used. When the submit that contains it has retired (CPU wait + the driver's
 * device->CPU cache sync, mali_isync/bo_cache) each colour attachment and resolve target is sampled through its CPU
 * mapping (every 64th byte) and one line is printed:
 *
 *   rp n=3 kind=render extent=1024x600 layers=1 samples=1 fmt=37 load=CLEAR store=STORE resolve=none draws=12
 *      nonzero=0.0 mean=0.00 alpha_nonzero=0.0 pipes=2 p0=3f2a1b04:9c01aa77 blend=0 wmask=15 depth=0/0/0
 *      samples=1 prim=8 tls=0 regs=24 tex=3 ubo=2 descs=sampled:3 ubos:2 imports:0 sets:2 writes:1
 *
 * nonzero is per attachment (comma separated), alpha_nonzero is the % of sampled alpha bytes != 0 (4-channel formats
 * only). A pass whose colour is present but whose alpha is 0 shows up as nonzero>0 alpha_nonzero=0.
 *
 * MALI_STATS_COMM=<substring> limits the whole thing to processes whose /proc/self/comm contains it (the file sink is
 * naming one file per process anyway, this just keeps a 60 Hz compositor out of the others). MALI_RP_STATS_MAX
 * overrides the default of 400 logged passes per process.
 *
 * Like MALI_SHARED_STATS this needs the MALI_WAIT_DBG file sink (mali_wait_debug.h): it writes through mali_wait_line.
 * Nothing is compiled in but a few loads when the environment is unset, and the pass records only exist while it is
 * on. */
#ifndef MALI_RPSTATS_H
#define MALI_RPSTATS_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "mali_backend.h"

enum mali_rp_kind {
   MALI_RP_RENDER = 0,
   MALI_RP_CLEAR = 1,
   MALI_RP_COPY = 2,
   MALI_RP_BLIT = 3,
   MALI_RP_RESOLVE = 4,
};

#define MALI_RP_MAX_ATT   4   /* colour attachments of one pass */
#define MALI_RP_MAX_PIPE  4   /* pipelines whose signature is printed (first four) */
#define MALI_RP_MAX_PASS  256 /* pending passes per device; older ones are dropped and counted */
#define MALI_RP_DEFAULT_MAX 400u

/* One sampled colour attachment: the BO of the image's memory, sampled whole (a sub-rect would need the plane
 * layout of every format and buys nothing for "is this black"). */
struct mali_rp_att {
   const void *owner;  /* the VkDeviceMemory the BO lives in (opaque here: mali_memory is a Vulkan type) */
   struct mali_bo *bo;
   uint32_t fmt;       /* VkFormat of the attachment */
   uint32_t bpp;       /* bytes per pixel, for the alpha channel offset */
};

struct mali_rp_pipe {
   uint32_t vsh, fsh;  /* FNV-1a of the shader binaries: the same shader has the same number in both processes */
   uint32_t blend;     /* bit0 enable, then src_c, dst_c, src_a, dst_a, op_c, op_a */
   uint32_t wmask;
   uint32_t dtest, dwrite, dfunc;
   uint32_t samples, prim;
   uint32_t tls, regs, ubo, tex;   /* mali_compiled: tls_size, work_reg_count, ubo_count, texture_count */
};

struct mali_rp_pass {
   uint32_t kind;      /* enum mali_rp_kind */
   uint32_t w, h, layers, samples;
   uint32_t fmt;       /* attachment 0 */
   uint32_t load, store;          /* VkAttachmentLoadOp / VkAttachmentStoreOp (0 = none for a meta pass) */
   uint32_t resolve_mode;         /* 0 none, 1 single-sample resolve (copy/blit), 2 MSAA resolve */
   uint32_t resolve_fmt;
   uint64_t va;        /* colour attachment 0 as the pass was built (VkImage memory + offset) */
   uint32_t pitch;     /* its row stride in the descriptor; compare with what the app was told (GetImageSubresourceLayout) */
   uint32_t draws;
   uint32_t npipes;
   struct mali_rp_pipe pipe[MALI_RP_MAX_PIPE];
   uint32_t desc_sampled, desc_ubo, desc_imports, desc_sets, desc_writes;
   struct mali_rp_att att[MALI_RP_MAX_ATT];
   uint32_t natt;
};

struct mali_rpstats {
   int on;                  /* MALI_RP_STATS and the sink and (optionally) the comm filter allow this process */
   uint32_t max_lines;      /* MALI_RP_STATS_MAX */
   pthread_mutex_t lock;
   struct mali_rp_pass pass[MALI_RP_MAX_PASS];
   unsigned npending;        /* entries in pass[] not yet sampled */
   unsigned nlogged, ndropped;
   /* Liveness of a BO: the record-time pointer may name memory the app freed before the submit, exactly like the
    * implicit-sync marks. lock_live() is mali_memory_guard() (mali_memory.c): true = live and the registry lock is
    * HELD, so the BO cannot go away while it is sampled; false = gone. Installed by the device. */
   bool (*lock_live)(const void *owner);
   void (*unlock)(void);
};

/* The device's instance, for the record-time hooks (mali_cmd.c has no device in its signatures). One device per
 * process, as the driver already assumes for g_hw. */
void mali_rpstats_set_current(struct mali_rpstats *s);
struct mali_rpstats *mali_rpstats_get(void);

void mali_rpstats_init(struct mali_rpstats *s, bool (*lock_live)(const void *), void (*unlock)(void));
void mali_rpstats_finish(struct mali_rpstats *s);
bool mali_rpstats_on(struct mali_rpstats *s);
/* Record time: queue one finished pass (dropped past MALI_RP_STATS_MAX or when the ring is full). */
void mali_rpstats_pass(struct mali_rpstats *s, const struct mali_rp_pass *p);
/* Submit time, after the submit retired: sample the queued passes and log them. */
void mali_rpstats_flush(struct mali_rpstats *s, const struct mali_backend_ops *be, struct mali_kdev *kdev);

#endif
