/* SPDX-License-Identifier: MIT
 * Linked (with -Wl,--wrap=ioctl) into the host-only "real kbase backend on libkbase's fake kernel" driver build.
 * Counts the kbase ioctls the product backend issues into the file CEFPROF_KSTATS, shared with cefprof_layer.c:
 *   [0..255] calls by ioctl nr (type 0x80), [256..511] other ioctl types by nr, [512] MEM_SYNC bytes CPU->device,
 *   [513] MEM_SYNC bytes device->CPU, [514] MEM_ALLOC(_EX) bytes, [515] MEM_SYNC calls CPU->device,
 *   [516] MEM_SYNC calls device->CPU, [517] alloc+import-MEM_FREE ioctls, [518] SAME_VA frees by munmap (live = [517]-[518]). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define NKS 520
int __real_ioctl(int fd, unsigned long req, ...);
unsigned kb_fake_mem_free_count(void);

static uint64_t *
stats(void)
{
   static uint64_t *s, fallback[NKS];
   if (s)
      return s;
   const char *p = getenv("CEFPROF_KSTATS");
   int f = p ? open(p, O_RDWR | O_CREAT, 0600) : -1;
   if (f >= 0 && !ftruncate(f, NKS * 8)) {
      void *m = mmap(NULL, NKS * 8, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
      if (m != MAP_FAILED)
         s = m;
   }
   if (f >= 0)
      close(f);
   if (!s)
      s = fallback;
   return s;
}

int
__wrap_ioctl(int fd, unsigned long req, ...)
{
   va_list ap;
   va_start(ap, req);
   void *arg = va_arg(ap, void *);
   va_end(ap);
   uint64_t *s = stats();
   unsigned nr = _IOC_NR(req) & 255, type = _IOC_TYPE(req);
#define ADD(i, v) __atomic_add_fetch(&s[i], (uint64_t)(v), __ATOMIC_RELAXED)
   ADD(type == 0x80 ? nr : 256 + nr, 1);
   uint64_t alloc_pages = 0;
   if (type == 0x80 && (nr == 59 || nr == 5))
      alloc_pages = ((const uint64_t *)arg)[0]; /* in.va_pages: read before the kernel overwrites the union */
   int r = __real_ioctl(fd, req, arg);
   if (type == 0x80 && r >= 0) {
      if (nr == 15) {
         const struct { uint64_t handle, user_addr, size; uint8_t type; } *m = arg;
         ADD(m->type == 1 ? 512 : 513, m->size);
         ADD(m->type == 1 ? 515 : 516, 1);
      } else if (nr == 59 || nr == 5) {
         ADD(514, alloc_pages * 4096);
         ADD(517, 1);
      } else if (nr == 22) {
         ADD(517, 1);
      } else if (nr == 7) {
         ADD(517, -1);
      }
   }
   __atomic_store_n(&s[518], kb_fake_mem_free_count(), __ATOMIC_RELAXED);
   return r;
}
