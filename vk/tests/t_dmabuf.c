/* SPDX-License-Identifier: MIT */
/* mali_dmabuf_probe (vk/src/mali_dmabuf.c): the fd check behind VkImportMemoryFdInfoKHR and
 * vkGetMemoryFdPropertiesKHR. Both directions, because the phone's guest fails the statfs arm on real dma-bufs.
 *   t_dmabuf
 * Accepts: a real dma-buf, both arms (magic, and the ioctl fallback forced with MALI_TEST_NO_STATFS_MAGIC=1).
 * Refuses: memfd, pipe, regular file, /dev/null, /dev/kvm (unknown ioctls answer EINVAL there), fd -1, closed fd -
 * the same refusals with the magic arm forced off. A host without a dma-buf provider (no /dev/udmabuf and no usable
 * /dev/dma_heap) prints SKIP and the positive cases are proved only by the X8 run. Exit 0 = pass. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/magic.h>
#include <linux/udmabuf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "mali_dmabuf.h"

static int g_fail;
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                       \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                               \
      }                                                                                            \
   } while (0)

static void
expect(int fd, bool want, const char *what)
{
   const bool got = mali_dmabuf_probe(fd);
   printf("%-6s %-10s %s%s\n", got == want ? "ok" : "FAIL", want ? "accept" : "refuse", what,
          got == want ? "" : (got ? " (was accepted)" : " (was refused)"));
   if (got != want)
      g_fail = 1;
}

/* memfd_create has no libc wrapper below API 30 (the bionic build of this test would otherwise fail to compile:
 * "call to undeclared function 'memfd_create'"), the same reason vk/src/mali_backend_fake.c:299 goes through syscall(). */
static int
test_memfd_create(const char *name, unsigned int flags)
{
#ifdef __BIONIC__
   return (int)syscall(SYS_memfd_create, name, flags);
#else
   return memfd_create(name, flags);
#endif
}

/* A real dma-buf: udmabuf over a sealed memfd (the host's /dev/udmabuf), else the system dma-heap. */
static int
make_dmabuf(char *why, size_t whylen)
{
   int memfd = test_memfd_create("t_dmabuf", MFD_ALLOW_SEALING | MFD_CLOEXEC);
   if (memfd < 0) {
      snprintf(why, whylen, "memfd_create");
      return -1;
   }
   if (ftruncate(memfd, 4096) < 0 || fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
      snprintf(why, whylen, "sealing the memfd");
      close(memfd);
      return -1;
   }
   int u = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
   if (u >= 0) {
      struct udmabuf_create c = {.memfd = (uint32_t)memfd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = 4096};
      int d = ioctl(u, UDMABUF_CREATE, &c);
      close(u);
      close(memfd);
      if (d < 0)
         snprintf(why, whylen, "UDMABUF_CREATE failed");
      return d;
   }
   int h = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
   close(memfd);
   if (h < 0) {
      snprintf(why, whylen, "no /dev/udmabuf and no /dev/dma_heap/system");
      return -1;
   }
   struct dma_heap_allocation_data a = {.len = 4096, .fd_flags = O_RDWR | O_CLOEXEC};
   int d = ioctl(h, DMA_HEAP_IOCTL_ALLOC, &a);
   const int e = errno;
   close(h);
   if (d < 0) {
      snprintf(why, whylen, "DMA_HEAP_IOCTL_ALLOC: %s", strerror(e));
      return -1;
   }
   return (int)a.fd;
}

int
main(void)
{
   char why[160] = "";
   const int db = make_dmabuf(why, sizeof(why));

   /* The negative cases first: they need no provider. */
   expect(-1, false, "fd -1");
   int closed = dup(0);
   close(closed);
   expect(closed, false, "closed fd");

   int memfd = test_memfd_create("plain", MFD_CLOEXEC);
   expect(memfd, false, "memfd");
   int p[2];
   CHECK(pipe(p) == 0, "pipe");
   expect(p[0], false, "pipe read end");
   expect(p[1], false, "pipe write end");
   char tmpl[] = "/tmp/opencode/t_dmabuf_reg_XXXXXX";
   int reg = mkstemp(tmpl);
   if (reg < 0) {
      snprintf(tmpl, sizeof(tmpl), "/tmp/t_dmabuf_reg_XXXXXX");
      reg = mkstemp(tmpl);
   }
   CHECK(reg >= 0, "mkstemp");
   expect(reg, false, "regular file");
   int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
   CHECK(nul >= 0, "/dev/null");
   expect(nul, false, "/dev/null (S_IFCHR)");
   int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
   if (kvm >= 0)
      expect(kvm, false, "/dev/kvm (EINVAL to unknown ioctls)");
   else
      printf("SKIP   /dev/kvm not openable, the char-device case is not exercised\n");

   /* The positive cases, and the same negatives with the statfs arm forced off (the phone's condition). */
   if (db < 0) {
      printf("SKIP   no real dma-buf on this host (%s); the accept direction is proved by the X8 run\n", why);
   } else {
      struct statfs sf;
      const bool magic = fstatfs(db, &sf) == 0 && (unsigned long)sf.f_type == DMA_BUF_MAGIC;
      expect(db, true, "real dma-buf (magic arm)");
      setenv("MALI_TEST_NO_STATFS_MAGIC", "1", 1);
      expect(db, true, "real dma-buf (ioctl fallback forced)");
      expect(memfd, false, "memfd, fallback forced");
      expect(p[0], false, "pipe, fallback forced");
      expect(reg, false, "regular file, fallback forced");
      unsetenv("MALI_TEST_NO_STATFS_MAGIC");
      /* Not a failure: the arm that carried the guest has its own line above either way. */
      printf("note   dma-buf fstatfs magic %s on this host\n", magic ? "matches DMA_BUF_MAGIC" : "does NOT match");
   }

   if (db >= 0) close(db);
   if (memfd >= 0) close(memfd);
   if (reg >= 0) { close(reg); unlink(tmpl); }
   if (nul >= 0) close(nul);
   if (kvm >= 0) close(kvm);
   close(p[0]); close(p[1]);
   printf(g_fail ? "FAILED\n" : "OK\n");
   return g_fail;
}
