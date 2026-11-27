/* SPDX-License-Identifier: MIT */
/* Host test of MALI_RP_STATS (mali_rpstats.c): the per-render-pass line, its attachment numbers and its limits.
 * No Mesa, no GPU, no kernel: the pass record is plain data and the BO is a malloc'd buffer, so every branch is
 * reachable here (the fake backend executes nothing, so "the GPU wrote it" is the shadow copied in at
 * bo_cache(invalidate), exactly where the driver's device->CPU sync makes the frame visible to a CPU read).
 *
 *  - MALI_RP_STATS=0 (or no sink) -> no line at all
 *  - MALI_STATS_COMM that does not match this process -> no line
 *  - a pass whose attachment holds a rendered colour -> nonzero > 0, alpha_nonzero > 0
 *  - the same pass with an untouched (zero) attachment -> nonzero = 0 (a black pass)
 *  - colour present, alpha 0 -> nonzero > 0 but alpha_nonzero = 0 (the "alpha only black" case)
 *  - a resolve target is sampled as a second attachment (nonzero per attachment, comma separated)
 *  - MALI_RP_STATS_MAX caps the number of lines
 *  - a BO the live callback refuses is not read
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mali_rpstats.h"

static int fails;
#define EXPECT(c, ...)                                                                             \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         fails++;                                                                                  \
         printf("FAIL  ");                                                                         \
      } else {                                                                                     \
         printf("ok    ");                                                                         \
      }                                                                                            \
      printf(__VA_ARGS__);                                                                         \
      printf("\n");                                                                                \
   } while (0)

/* ---- the log sink (mali_sync.c), stubbed --------------------------------------------------------------------- */
static char log_buf[16384];
static size_t log_len;

int
mali_wait_debug(void)
{
   return 1;
}

void
mali_wait_line(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   const int n = vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, ap);
   va_end(ap);
   if (n > 0 && (size_t)n < sizeof(log_buf) - log_len) {
      log_len += (size_t)n;
      log_buf[log_len++] = '\n';
      log_buf[log_len] = '\0';
   }
}

static unsigned
count(const char *needle)
{
   unsigned n = 0;
   for (const char *p = log_buf; (p = strstr(p, needle)); p++)
      n++;
   return n;
}

/* ---- backend seam -------------------------------------------------------------------------------------------- */
struct kernel {
   unsigned touches, caches;
   uint8_t *gpu;      /* what the GPU wrote, NULL = nothing */
   size_t size;
};

static void
fake_touch(struct mali_kdev *k, struct mali_bo *bo)
{
   struct kernel *x = (struct kernel *)k;
   x->touches++;
   (void)bo;
}

static void
fake_cache(struct mali_kdev *k, const struct mali_bo *bo, size_t off, size_t len, bool invalidate)
{
   struct kernel *x = (struct kernel *)k;
   x->caches++;
   if (invalidate && x->gpu && bo->cpu && off + len <= x->size)
      memcpy((uint8_t *)bo->cpu + off, x->gpu + off, len);
}

static const struct mali_backend_ops fake_ops = {.bo_host_touch = fake_touch, .bo_cache = fake_cache};

/* The live-BO guard, stubbed: everything is live unless the test says otherwise. */
static int refuse_owner;
static int guard_hits, unguard_hits;
static bool
fake_lock_live(const void *owner)
{
   guard_hits++;
   return owner != &refuse_owner;
}

static void
fake_unlock(void)
{
   unguard_hits++;
}

static void
make_bo(struct mali_bo *bo, size_t size)
{
   memset(bo, 0, sizeof *bo);
   bo->cpu = calloc(1, size);
   bo->size = size;
   bo->dmabuf_fd = -1;
}

/* A rendered colour: RGBA (64,64,64,255) everywhere - the clear colour of a grey page. */
static uint8_t *
paint(size_t size, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
   uint8_t *p = malloc(size);
   for (size_t i = 0; i + 3 < size; i += 4) {
      p[i + 0] = r; p[i + 1] = g; p[i + 2] = b; p[i + 3] = a;
   }
   return p;
}

int
main(void)
{
   const size_t sz = 512u * 1024u;

   /* 1: off. */
   unsetenv("MALI_RP_STATS");
   struct mali_rpstats off;
   mali_rpstats_init(&off, fake_lock_live, fake_unlock);
   EXPECT(!mali_rpstats_on(&off), "MALI_RP_STATS unset: off");
   struct mali_bo bo;
   make_bo(&bo, sz);
   struct mali_rp_pass p;
   memset(&p, 0, sizeof p);
   p.kind = MALI_RP_RENDER;
   p.w = 1024; p.h = 600; p.layers = 1; p.samples = 1; p.fmt = 37;
   p.load = 2; p.store = 0; p.draws = 7;
   p.att[0].bo = &bo; p.att[0].owner = &bo; p.att[0].bpp = 4;
   p.natt = 1;
   mali_rpstats_pass(&off, &p);
   mali_rpstats_flush(&off, &fake_ops, (struct mali_kdev *)&(struct kernel){0});
   EXPECT(log_len == 0, "off: no line");
   mali_rpstats_finish(&off);

   /* 2: on, a pass that rendered a colour. */
   setenv("MALI_RP_STATS", "1", 1);
   struct mali_rpstats on;
   mali_rpstats_init(&on, fake_lock_live, fake_unlock);
   mali_rpstats_set_current(&on);
   EXPECT(mali_rpstats_on(&on), "MALI_RP_STATS=1 with a sink: on");

   struct kernel k = {0};
   k.size = sz;
   k.gpu = paint(sz, 64, 64, 64, 255);
   mali_rpstats_pass(&on, &p);
   mali_rpstats_flush(&on, &fake_ops, (struct mali_kdev *)&k);
   EXPECT(count("rp n=1 kind=render extent=1024x600 layers=1 samples=1 fmt=37 load=2 store=0 resolve=none,0 draws=7") == 1,
          "render: the pass identity, extent, load/store and draw count are on the line");
   EXPECT(count("nonzero=100.0 mean=111.75 alpha_nonzero=100.0") == 1,
          "render: a painted attachment (64,64,64,255) reads nonzero=100.0, mean=111.75, alpha 100.0");
   EXPECT(k.touches == 1 && k.caches == 1, "render: one host touch and one cache sync before the read (%u/%u)",
          k.touches, k.caches);

   /* 3: an untouched attachment: the black pass, which is what the X8 shows. */
   log_len = 0; log_buf[0] = '\0';
   struct mali_bo black;
   make_bo(&black, sz);
   struct kernel k0 = {0};
   struct mali_rp_pass pb = p;
   pb.att[0].bo = &black;
   pb.att[0].owner = &black;
   mali_rpstats_pass(&on, &pb);
   mali_rpstats_flush(&on, &fake_ops, (struct mali_kdev *)&k0);
   EXPECT(count("nonzero=0.0 mean=0.00 alpha_nonzero=0.0") == 1, "untouched: nonzero=0.0 alpha 0.0 (black pass)");

   /* 4: colour present, alpha 0. */
   log_len = 0; log_buf[0] = '\0';
   struct kernel ka = {0};
   ka.size = sz;
   ka.gpu = paint(sz, 200, 30, 30, 0);
   mali_rpstats_pass(&on, &pb);
   mali_rpstats_flush(&on, &fake_ops, (struct mali_kdev *)&ka);
   /* Whole pixels now: 200,30,30,0 -> three non-zero of four, mean 65.00, alpha 0. */
   EXPECT(count("nonzero=75.0 mean=65.00 alpha_nonzero=0.0") == 1,
          "alpha 0: colour present (75 %), alpha_nonzero 0.0");

   /* 5: MSAA resolve: a second attachment, nonzero per attachment. */
   log_len = 0; log_buf[0] = '\0';
   struct mali_bo res;
   make_bo(&res, sz);
   struct kernel kr = {0};
   kr.size = sz;
   kr.gpu = paint(sz, 64, 64, 64, 255);   /* both attachments get the same shadow: 2 x 100.0 */
   struct mali_rp_pass pr = p;
   pr.samples = 4;
   pr.resolve_mode = 2;
   pr.resolve_fmt = 37;
   pr.natt = 2;
   pr.att[1].bo = &res;
   pr.att[1].owner = &res;
   pr.att[1].bpp = 4;
   mali_rpstats_pass(&on, &pr);
   mali_rpstats_flush(&on, &fake_ops, (struct mali_kdev *)&kr);
   EXPECT(count("samples=4 fmt=37 load=2 store=0 resolve=msaa,37 draws=7 nonzero=100.0,100.0") == 1,
          "resolve: samples=4 and one nonzero per attachment");

   /* 6: a BO the live guard refuses is not read. */
   log_len = 0; log_buf[0] = '\0';
   struct mali_bo dead;
   make_bo(&dead, sz);
   struct mali_rp_pass pd = p;
   pd.att[0].bo = &dead;
   pd.att[0].owner = &refuse_owner;
   const unsigned g0 = guard_hits, u0 = unguard_hits;
   mali_rpstats_pass(&on, &pd);
   mali_rpstats_flush(&on, &fake_ops, (struct mali_kdev *)&(struct kernel){0});
   EXPECT(count("nonzero=- mean=-") == 1, "freed memory: the pass is logged, nothing is read (nonzero=-)");
   EXPECT(guard_hits == g0 + 1 && unguard_hits == u0, "freed memory: the guard refused, no unlock (%u/%u)",
          guard_hits - g0, unguard_hits - u0);

   /* 7: MALI_RP_STATS_MAX caps the lines. */
   log_len = 0; log_buf[0] = '\0';
   setenv("MALI_RP_STATS_MAX", "2", 1);
   struct mali_rpstats lim;
   mali_rpstats_init(&lim, fake_lock_live, fake_unlock);
   for (int i = 0; i < 10; i++)
      mali_rpstats_pass(&lim, &p);
   mali_rpstats_flush(&lim, &fake_ops, (struct mali_kdev *)&k);
   EXPECT(count("rp n=") == 2, "MALI_RP_STATS_MAX=2: two lines of ten passes (%u)", count("rp n="));
   mali_rpstats_finish(&lim);
   unsetenv("MALI_RP_STATS_MAX");

   /* 8: MALI_STATS_COMM that does not match this process turns it off (the comm is never "nope-nope"). */
   setenv("MALI_STATS_COMM", "nope-nope", 1);
   struct mali_rpstats filtered;
   mali_rpstats_init(&filtered, fake_lock_live, fake_unlock);
   EXPECT(!mali_rpstats_on(&filtered), "MALI_STATS_COMM mismatch: off");
   mali_rpstats_finish(&filtered);
   unsetenv("MALI_STATS_COMM");

   mali_rpstats_set_current(NULL);
   mali_rpstats_finish(&on);
   printf("%d failure(s)\n", fails);
   return fails ? 1 : 0;
}
