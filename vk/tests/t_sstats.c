/* SPDX-License-Identifier: MIT */
/* Host test of MALI_SHARED_STATS (mali_sstats.c): the line a stage prints for a shared dma-buf, and the rate limit.
 * No Mesa, no GPU, no kernel: the backend seam is bo_host_touch/bo_cache and an fstat-able fd, and the log sink is
 * stubbed, so every branch is reachable here.
 *
 *  - MALI_SHARED_STATS=0 (or with no sink) -> nothing at all
 *  - a submitted BO that the GPU WROTE (a clear colour) -> shared_write with nonzero > 0 and mean > 0, and the bo id
 *    is the dma-buf inode, i.e. the same number in every process that has the buffer
 *  - a BO of the same size that nothing wrote -> shared_write with nonzero = 0 and mean = 0 (a black buffer)
 *  - a submitted read and a present print shared_read / present instead
 *  - first 20 lines per (bo, kind), then every 50th
 *  - under 64 KiB, or without a dma-buf fd, or without a CPU mapping: no line
 *
 * The fake backend executes nothing (check.sh: "the fake backend executes nothing"), so the "GPU writes" of a clear
 * happen exactly where the driver's device->CPU cache sync makes them visible: the bo_cache(invalidate=true) that
 * mali_sstats_note issues before it reads. The shadow buffer below is that GPU-side picture; an untouched BO has no
 * shadow and stays zero, which is the same thing the X8's black stage shows. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mali_sstats.h"

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

/* ---- the log sink (mali_sync.c's mali_wait_*), stubbed: one buffer, greppable lines ---------------------------- */
static char log_buf[8192];
static size_t log_len;

int
mali_wait_debug(void)
{
   return 1; /* a sink is open: the driver's MALI_WAIT_DBG file */
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
log_lines(void)
{
   unsigned n = 0;
   for (const char *p = log_buf; *p; p++)
      n += *p == '\n';
   return n;
}

static unsigned
log_count(const char *prefix, const char *tag)
{
   unsigned n = 0;
   for (const char *p = log_buf; (p = strstr(p, prefix)); p++) {
      const char *e = strchr(p, '\n');
      if (!e)
         break;
      char line[256];
      const size_t len = (size_t)(e - p) < sizeof line - 1 ? (size_t)(e - p) : sizeof line - 1;
      memcpy(line, p, len);
      line[len] = '\0';
      if (!tag || strstr(line, tag))
         n++;
      p = e;
   }
   return n;
}

/* ---- the backend seam: cache sync + touch, and the GPU-side shadow --------------------------------------------- */
struct fake_kernel {
   uint8_t *gpu; /* what the GPU wrote (device memory), NULL = never written */
   size_t size;
   unsigned touches, caches;
};

static void
fake_touch(struct mali_kdev *k, struct mali_bo *bo)
{
   struct fake_kernel *f = (struct fake_kernel *)k;
   f->touches++;
   (void)bo;
}

static void
fake_cache(struct mali_kdev *k, const struct mali_bo *bo, size_t off, size_t len, bool invalidate)
{
   struct fake_kernel *f = (struct fake_kernel *)k;
   f->caches++;
   /* device -> CPU: the invalidate is what makes the GPU's frame visible to a CPU read (kbase_bo_cache). */
   if (invalidate && f->gpu && bo->cpu && off + len <= f->size)
      memcpy((uint8_t *)bo->cpu + off, f->gpu + off, len);
}

static const struct mali_backend_ops fake_ops = {.bo_host_touch = fake_touch, .bo_cache = fake_cache};

/* A dma-buf-backed BO of `size` bytes: the fd is a memfd, which is all the diagnostic needs (fstat inode). */
static int
make_bo(struct mali_bo *bo, size_t size)
{
   int fd = memfd_create("t_sstats", MFD_CLOEXEC);
   if (fd < 0 || ftruncate(fd, (off_t)size) < 0)
      return -1;
   memset(bo, 0, sizeof *bo);
   bo->cpu = calloc(1, size);
   bo->size = size;
   bo->dmabuf_fd = fd;
   bo->flags = MALI_BO_CACHED;
   return bo->cpu ? 0 : -1;
}

/* The clear colour of a rendered frame, in the GPU's own (device) memory: every pixel 0x40 0x40 0x40 0xff. */
static void
render_clear(struct fake_kernel *f, size_t size)
{
   f->gpu = malloc(size);
   f->size = size;
   for (size_t i = 0; i + 3 < size; i += 4) {
      f->gpu[i + 0] = 0x40;
      f->gpu[i + 1] = 0x40;
      f->gpu[i + 2] = 0x40;
      f->gpu[i + 3] = 0xff;
   }
}

int
main(void)
{
   const size_t sz = 256u * 1024u;

   /* 1: off. Neither the variable nor the sink alone turns it on. */
   unsetenv("MALI_SHARED_STATS");
   struct mali_sstats off;
   mali_sstats_init(&off);
   EXPECT(!mali_sstats_on(&off), "MALI_SHARED_STATS unset: off");
   struct mali_bo bo_off;
   EXPECT(make_bo(&bo_off, sz) == 0, "off: BO");
   struct fake_kernel k_off = {0};
   mali_sstats_note(&off, &fake_ops, (struct mali_kdev *)&k_off, &bo_off, MALI_SSTATS_WRITE, 1);
   EXPECT(log_len == 0 && k_off.caches == 0, "off: no line, no cache sync");
   mali_sstats_finish(&off);
   close(bo_off.dmabuf_fd);
   free(bo_off.cpu);

   /* 2: a written exported BO -> shared_write with nonzero > 0 after the clear colour is rendered. */
   setenv("MALI_SHARED_STATS", "1", 1);
   struct mali_sstats on;
   mali_sstats_init(&on);
   EXPECT(mali_sstats_on(&on), "MALI_SHARED_STATS=1 with a sink: on");

   struct mali_bo bo;
   EXPECT(make_bo(&bo, sz) == 0, "BO %zu B", sz);
   struct stat st;
   EXPECT(fstat(bo.dmabuf_fd, &st) == 0, "BO fd is fstat-able");
   struct fake_kernel k = {0};
   render_clear(&k, sz);
   mali_sstats_note(&on, &fake_ops, (struct mali_kdev *)&k, &bo, MALI_SSTATS_WRITE, 7);
   EXPECT(k.touches == 1 && k.caches == 1, "write: one host touch, one cache sync (%u/%u)", k.touches, k.caches);
   char want[96];
   snprintf(want, sizeof want, "shared_write bo=%d size=%zu", (int)st.st_ino, sz);
   EXPECT(log_count("shared_write bo=", want) == 1, "write: shared_write names the dma-buf inode and the size");
   EXPECT(log_count("shared_write", "nonzero=0.0 ") == 0, "write: the clear colour is not black");
   EXPECT(log_count("shared_write", "mean=0.00 ") == 0, "write: the clear colour has a non-zero mean");
   EXPECT(log_count("shared_write", "seq=7 ") == 1, "write: the submit seqno is on the line");

   /* 3: the same size, untouched (nothing rendered into it) -> a black buffer. */
   struct mali_bo black;
   EXPECT(make_bo(&black, sz) == 0, "black BO");
   struct fake_kernel k0 = {0};
   mali_sstats_note(&on, &fake_ops, (struct mali_kdev *)&k0, &black, MALI_SSTATS_WRITE, 8);
   EXPECT(log_count("shared_write bo=", "nonzero=0.0 mean=0.00 ") == 1, "untouched: nonzero=0.0 mean=0.00");

   /* 4: the same buffer, as a consumer and as a present. */
   mali_sstats_note(&on, &fake_ops, (struct mali_kdev *)&k0, &black, MALI_SSTATS_READ, 9);
   EXPECT(log_count("shared_read bo=", "nonzero=0.0 ") == 1, "read: shared_read line");
   mali_sstats_note(&on, &fake_ops, (struct mali_kdev *)&k0, &black, MALI_SSTATS_PRESENT, 10);
   EXPECT(log_count("present bo=", "nonzero=0.0 ") == 1, "present: present line");

   /* 5: rate limit - the first 20 of a (bo, kind), then every 50th: 1..20, 50, 100 -> 22 of 130. */
   const unsigned before = log_count("shared_read bo=", NULL);
   for (unsigned i = 0; i < 130; i++)
      mali_sstats_note(&on, &fake_ops, (struct mali_kdev *)&k0, &black, MALI_SSTATS_READ, 100 + i);
   /* seen 1 came from case 4: 2..20 (19), 50 and 100 -> 21 more. */
   EXPECT(log_count("shared_read bo=", NULL) - before == 21, "rate limit: 21 of 130 (%u)",
          log_count("shared_read bo=", NULL) - before);
   EXPECT(log_count("shared_read bo=", "seen=20") == 1, "rate limit: the 20th is the last of the first block");
   EXPECT(log_count("shared_read bo=", "seen=21") == 0, "rate limit: nothing between 20 and 50");
   EXPECT(log_count("shared_read bo=", "seen=100") == 1, "rate limit: every 50th after that");

   /* 6: what is not sampled at all. */
   const unsigned lines = log_lines();
   struct mali_bo small;
   EXPECT(make_bo(&small, 64u * 1024u - 1) == 0, "small BO");
   mali_sstats_note(&on, &fake_ops, (struct mali_kdev *)&k0, &small, MALI_SSTATS_WRITE, 11);
   struct mali_bo nofd;
   EXPECT(make_bo(&nofd, sz) == 0, "BO without a dma-buf fd");
   close(nofd.dmabuf_fd);
   nofd.dmabuf_fd = -1;
   mali_sstats_note(&on, &fake_ops, (struct mali_kdev *)&k0, &nofd, MALI_SSTATS_WRITE, 12);
   struct mali_bo nomap;
   EXPECT(make_bo(&nomap, sz) == 0, "BO without a CPU mapping");
   free(nomap.cpu);
   nomap.cpu = NULL;
   mali_sstats_note(&on, &fake_ops, (struct mali_kdev *)&k0, &nomap, MALI_SSTATS_WRITE, 13);
   EXPECT(log_lines() == lines + 1, "under 64 KiB and no fd: no line; no CPU mapping: one line, no read");
   EXPECT(log_count("shared_write", "reason=no-cpu-mapping") == 1, "no CPU mapping: the line says so");

   mali_sstats_finish(&on);
   printf("%u lines sampled, %d failure(s)\n", log_lines(), fails);
   return fails ? 1 : 0;
}
