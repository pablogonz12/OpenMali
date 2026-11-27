/* SPDX-License-Identifier: MIT */
/* Host test of the implicit-sync decision logic (mali_isync.c, vk/ISYNC). No Mesa, no GPU: the backend seam is
 * three function pointers, so every branch of the policy is reachable from here.
 *
 *  - MALI_IMPLICIT_SYNC=0 turns everything off
 *  - a submit that touches no shared dma-buf forces no fence
 *  - kernel import ENOTTY (Poco X4, 5.10) -> the submit CPU-waits its own fence, latched (no second ioctl)
 *  - kernel import works -> one WRITE fence per shared dma-buf, no CPU wait
 *  - consumer: export ENOTTY -> nothing; export works -> one wait per pending fence, and the fd is closed
 *  - a submit whose shared BOs could not all be listed (wrote_full) takes the CPU wait whatever the kernel
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "mali_isync.h"

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); } \
   else { printf("ok    "); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct fake_kernel {
   int import_r;        /* what dmabuf_import_sync_file answers: 0, -ENOTTY or a soft error (-ENOMEM) */
   int import_fail_at;  /* 1-based import call that answers import_soft_r (0 = every call answers import_r) */
   int import_soft_r;
   int export_r;        /* 0, -ENOTTY or a soft error (-EMFILE) */
   int wait_r;          /* what fence_wait answers: 0 or -EIO */
   int export_fd;       /* the sync_file the export hands out (-1 = none pending) */
   unsigned imports, exports, waits, polls, blocked;
   bool pending;        /* the fence is not signalled yet: a 0-timeout poll answers TIMEOUT */
   bool fast_poll;      /* the fence is signalled (a poll answers 0, no blocking wait) */
   int last_flags;      /* flags of the last import/export */
   int last_fence;      /* sync_fd of the last import */
   int last_fd_closed;  /* the sync_file the consumer was given: closed by it? */
};

static int
fake_import(struct mali_kdev *kdev, int dmabuf_fd, uint32_t flags, int sync_fd)
{
   struct fake_kernel *k = (struct fake_kernel *)kdev;
   k->imports++;
   if (k->import_fail_at && k->imports >= k->import_fail_at)
      return k->import_soft_r;
   k->last_flags = (int)flags;
   k->last_fence = sync_fd;
   return k->import_r;
}

static int
fake_export(struct mali_kdev *kdev, int dmabuf_fd, uint32_t flags, int *sync_fd)
{
   struct fake_kernel *k = (struct fake_kernel *)kdev;
   k->exports++;
   k->last_flags = (int)flags;
   if (k->export_r != 0)
      return k->export_r;
   *sync_fd = k->export_fd >= 0 ? dup(k->export_fd) : -1;   /* a dup of the pending fence, like the ioctl */
   return 0;
}

static int
fake_wait(struct mali_kdev *kdev, int fence_fd, int timeout_ms)
{
   struct fake_kernel *k = (struct fake_kernel *)kdev;
   if (timeout_ms == 0) {
      k->polls++;
      if (k->wait_r < 0)
         return k->wait_r;
      return k->pending ? -ETIMEDOUT : 0;   /* a fence that has not been signalled yet */
   }
   k->waits++;
   if (k->wait_r < 0)
      return k->wait_r;
   if (timeout_ms < 0) {
      k->blocked++;
      k->pending = false;
   }
   (void)fence_fd;
   return 0;
}

static const struct mali_backend_ops fake_ops = {
   .dmabuf_import_sync_file = fake_import,
   .dmabuf_export_sync_file = fake_export,
   .fence_wait = fake_wait,
};

int
main(void)
{
   int fd0 = eventfd(1, EFD_CLOEXEC);   /* signalled: the "pending fence" the export hands out */
   int fds[2] = {fd0, fd0};

   /* 1: MALI_IMPLICIT_SYNC=0 - nothing at all, whatever the kernel supports */
   setenv("MALI_IMPLICIT_SYNC", "0", 1);
   struct mali_isync_dev off;
   mali_isync_dev_init(&off);
   struct fake_kernel k0 = {.import_r = 0, .export_r = 0, .export_fd = fd0};
   EXPECT(!mali_isync_enabled(&off), "MALI_IMPLICIT_SYNC=0 disables it");
   EXPECT(mali_isync_plan_producer(&off, 2, false) == MALI_ISYNC_NONE, "off: producer plans nothing");
   EXPECT(mali_isync_plan_consumer(&off, 2) == MALI_ISYNC_NONE, "off: consumer plans nothing");
   EXPECT(mali_isync_producer(&off, &fake_ops, (struct mali_kdev *)&k0, fds, 2, false, fd0) == MALI_ISYNC_NONE,
          "off: producer does nothing");
   mali_isync_consumer(&off, &fake_ops, (struct mali_kdev *)&k0, fds, 2);
   EXPECT(k0.imports == 0 && k0.exports == 0 && k0.waits == 0, "off: no ioctl, no wait (%u/%u/%u)",
          k0.imports, k0.exports, k0.waits);
   mali_isync_dev_finish(&off);

   /* 2: on, but the submit touches nothing shared */
   setenv("MALI_IMPLICIT_SYNC", "1", 1);
   struct mali_isync_dev d;
   mali_isync_dev_init(&d);
   EXPECT(mali_isync_enabled(&d), "default: enabled");
   EXPECT(mali_isync_plan_producer(&d, 0, false) == MALI_ISYNC_NONE, "no shared BO: producer plans nothing");
   EXPECT(mali_isync_plan_consumer(&d, 0) == MALI_ISYNC_NONE, "no shared BO: consumer plans nothing");
   EXPECT(mali_isync_plan_producer(&d, 1, false) == MALI_ISYNC_FENCE, "unprobed kernel: try the fence first");

   /* 3: 5.10 in the opt-in mode - the import ioctl answers ENOTTY, so the submit waits its own fence instead */
   setenv("MALI_IMPLICIT_SYNC", "wait", 1);
   mali_isync_dev_finish(&d);
   mali_isync_dev_init(&d);
   struct fake_kernel k1 = {.import_r = -ENOTTY, .export_r = -ENOTTY, .export_fd = -1};
   EXPECT(mali_isync_producer(&d, &fake_ops, (struct mali_kdev *)&k1, fds, 2, false, fd0) == MALI_ISYNC_CPU_WAIT,
          "ENOTTY: the submit takes the CPU wait");
   EXPECT(k1.imports == 1 && k1.waits == 1, "ENOTTY: one probe ioctl, one wait (%u/%u)", k1.imports, k1.waits);
   EXPECT(k1.last_flags == MALI_DMA_BUF_SYNC_WRITE && k1.last_fence == fd0, "ENOTTY: the probe asked for a WRITE fence");
   EXPECT(mali_isync_plan_producer(&d, 2, false) == MALI_ISYNC_CPU_WAIT, "ENOTTY is latched: no second probe");
   EXPECT(mali_isync_producer(&d, &fake_ops, (struct mali_kdev *)&k1, fds, 2, false, fd0) == MALI_ISYNC_CPU_WAIT,
          "ENOTTY: later submits still wait");
   EXPECT(k1.imports == 1 && k1.waits == 2, "ENOTTY: no ioctl again (%u/%u)", k1.imports, k1.waits);

   /* 3b: 5.10 in the DEFAULT mode (fence only): a handoff is left unsynced, the producer is not blocked */
   {
      setenv("MALI_IMPLICIT_SYNC", "1", 1);
      struct mali_isync_dev df;
      mali_isync_dev_init(&df);
      struct fake_kernel kf = {.import_r = -ENOTTY, .export_r = -ENOTTY, .export_fd = -1};
      EXPECT(mali_isync_plan_producer(&df, 1, false) == MALI_ISYNC_FENCE, "default mode: fence first");
      EXPECT(mali_isync_producer(&df, &fake_ops, (struct mali_kdev *)&kf, fds, 1, false, fd0) == MALI_ISYNC_NONE,
             "ENOTTY in the default mode: no CPU wait, the producer keeps running");
      EXPECT(kf.waits == 0 && kf.blocked == 0, "ENOTTY default: the fence is not waited (%u/%u)", kf.waits, kf.blocked);
      EXPECT(mali_isync_plan_producer(&df, 1, false) == MALI_ISYNC_NONE, "ENOTTY is latched in the default mode too");
      mali_isync_dev_finish(&df);
      setenv("MALI_IMPLICIT_SYNC", "wait", 1);
   }

   /* 4: a kernel with the ioctl - one WRITE fence per shared dma-buf, no CPU wait */
   struct mali_isync_dev d2;
   mali_isync_dev_init(&d2);
   struct fake_kernel k2 = {.import_r = 0, .export_r = 0, .export_fd = -1};
   EXPECT(mali_isync_producer(&d2, &fake_ops, (struct mali_kdev *)&k2, fds, 2, false, fd0) == MALI_ISYNC_FENCE,
          "supported: the fence is attached");
   EXPECT(k2.imports == 2 && k2.waits == 0, "supported: one import per dma-buf, no wait (%u/%u)", k2.imports, k2.waits);
   EXPECT(k2.last_flags == MALI_DMA_BUF_SYNC_WRITE, "supported: WRITE fence");

   /* 5: consumer on 5.10 - ENOTTY means the producer's CPU wait covered it */
   struct fake_kernel k3 = {.import_r = -ENOTTY, .export_r = -ENOTTY, .export_fd = -1};
   mali_isync_consumer(&d2, &fake_ops, (struct mali_kdev *)&k3, fds, 2);
   EXPECT(k3.exports == 1 && k3.waits == 0, "consumer ENOTTY: one probe, no wait (%u/%u)", k3.exports, k3.waits);
   EXPECT(mali_isync_plan_consumer(&d2, 2) == MALI_ISYNC_NONE, "consumer ENOTTY is latched");
   mali_isync_consumer(&d2, &fake_ops, (struct mali_kdev *)&k3, fds, 2);
   EXPECT(k3.exports == 1, "consumer ENOTTY: no second probe (%u)", k3.exports);

   /* 6: consumer with the ioctl - waits each pending fence and closes it */
   struct mali_isync_dev d3;
   mali_isync_dev_init(&d3);
   struct fake_kernel k4 = {.import_r = 0, .export_r = 0, .export_fd = fd0};
   mali_isync_consumer(&d3, &fake_ops, (struct mali_kdev *)&k4, fds, 2);
   EXPECT(k4.exports == 2 && k4.polls == 2 && k4.blocked == 0, "consumer: one export and one polled fence per dma-buf (%u/%u)",
          k4.exports, k4.polls);
   EXPECT(k4.last_flags == MALI_DMA_BUF_SYNC_READ, "consumer: READ fence");
   EXPECT(fcntl(k4.export_fd, F_GETFD) >= 0, "consumer keeps the original fd");

   /* 7: a submit that cannot name all its shared BOs: CPU wait in the opt-in mode, nothing in the default one */
   EXPECT(mali_isync_plan_producer(&d3, 0, true) == MALI_ISYNC_CPU_WAIT, "handoff_full: CPU wait (wait mode)");
   struct fake_kernel k5 = {.import_r = 0, .export_r = 0, .export_fd = -1};
   EXPECT(mali_isync_producer(&d3, &fake_ops, (struct mali_kdev *)&k5, fds, 0, true, fd0) == MALI_ISYNC_CPU_WAIT,
          "wrote_full: the submit waits its fence");
   EXPECT(k5.imports == 0 && k5.waits == 1, "wrote_full: no import, one wait (%u/%u)", k5.imports, k5.waits);

   /* 7b: the consumer polls the exported fence first; a pending one is waited for, a signalled one is not */
   {
      struct mali_isync_dev d4;
      mali_isync_dev_init(&d4);
      struct fake_kernel kp = {.import_r = 0, .export_r = 0, .export_fd = fd0, .pending = true};
      mali_isync_consumer(&d4, &fake_ops, (struct mali_kdev *)&kp, fds, 1);
      EXPECT(kp.polls == 1 && kp.blocked == 1, "consumer polls first, then blocks on a pending fence (%u/%u)",
             kp.polls, kp.blocked);
      struct fake_kernel ks = {.import_r = 0, .export_r = 0, .export_fd = fd0};
      mali_isync_consumer(&d4, &fake_ops, (struct mali_kdev *)&ks, fds, 1);
      EXPECT(ks.polls == 1 && ks.blocked == 0, "consumer does not block on a signalled fence (%u/%u)",
             ks.polls, ks.blocked);
      mali_isync_dev_finish(&d4);
   }

   /* 7c: MALI_IMPLICIT_SYNC_COMM filters by /proc/self/comm (this test's comm contains its name) */
   {
      setenv("MALI_IMPLICIT_SYNC_COMM", "t_isync", 1);
      struct mali_isync_dev dm;
      mali_isync_dev_init(&dm);
      EXPECT(mali_isync_enabled(&dm), "MALI_IMPLICIT_SYNC_COMM matching this process keeps it on");
      mali_isync_dev_finish(&dm);
      setenv("MALI_IMPLICIT_SYNC_COMM", "no-such-process-comm", 1);
      mali_isync_dev_init(&dm);
      EXPECT(!mali_isync_enabled(&dm), "MALI_IMPLICIT_SYNC_COMM not matching turns it off");
      EXPECT(mali_isync_plan_producer(&dm, 2, false) == MALI_ISYNC_NONE, "filtered out: producer plans nothing");
      EXPECT(mali_isync_plan_consumer(&dm, 2) == MALI_ISYNC_NONE, "filtered out: consumer plans nothing");
      mali_isync_dev_finish(&dm);
      unsetenv("MALI_IMPLICIT_SYNC_COMM");
   }

   /* 8: no fence to wait (-1): the CPU-wait path must not crash or call the backend with -1 */
   struct fake_kernel k6 = {.import_r = -ENOTTY, .export_r = -ENOTTY, .export_fd = -1};
   EXPECT(mali_isync_producer(&d3, &fake_ops, (struct mali_kdev *)&k6, fds, 1, false, -1) == MALI_ISYNC_CPU_WAIT,
          "no fence fd: still reports the CPU wait");
   EXPECT(k6.waits == 0, "no fence fd: nothing to wait (%u)", k6.waits);

   /* 9: the ioctl exists but one attach fails (reservation object, -ENOMEM): that submit takes the CPU wait, and
    * the failure is not latched as "this kernel has no implicit fences". */
   struct mali_isync_dev d4;
   mali_isync_dev_init(&d4);
   struct fake_kernel k7 = {.import_r = 0, .import_fail_at = 2, .import_soft_r = -ENOMEM, .export_r = 0, .export_fd = -1};
   int w7 = 0;
   EXPECT(mali_isync_producer_ex(&d4, &fake_ops, (struct mali_kdev *)&k7, fds, 2, false, fd0, &w7) == MALI_ISYNC_CPU_WAIT,
          "second attach fails: this submit takes the CPU wait");
   EXPECT(k7.imports == 2 && k7.waits == 1 && w7 == 0, "second attach fails: two imports, one good wait (%u/%u/%d)",
          k7.imports, k7.waits, w7);
   EXPECT(mali_isync_plan_producer(&d4, 2, false) == MALI_ISYNC_FENCE, "a soft attach error is not latched");
   struct fake_kernel k8 = {.import_r = 0, .export_r = 0, .export_fd = -1};
   EXPECT(mali_isync_producer(&d4, &fake_ops, (struct mali_kdev *)&k8, fds, 2, false, fd0) == MALI_ISYNC_FENCE,
          "... the next submit tries the fence again");
   EXPECT(k8.imports == 2, "next submit: one import per dma-buf (%u)", k8.imports);
   mali_isync_dev_finish(&d4);

   /* 10: the fallback CPU wait itself fails (GPU fault, -EIO): the status must reach the caller, which reports
    * DEVICE_LOST instead of VK_SUCCESS. */
   struct mali_isync_dev d5;
   mali_isync_dev_init(&d5);
   struct fake_kernel k9 = {.import_r = -ENOTTY, .export_r = -ENOTTY, .export_fd = -1, .wait_r = -EIO};
   int w9 = 0;
   EXPECT(mali_isync_producer_ex(&d5, &fake_ops, (struct mali_kdev *)&k9, fds, 1, false, fd0, &w9) == MALI_ISYNC_CPU_WAIT &&
          w9 == -EIO, "fallback wait -EIO is reported (%d)", w9);
   mali_isync_dev_finish(&d5);

   /* 11: consumer export failures block the submission; a WRITE export (imported buffer this submit writes) asks the
    * kernel for the readers' fences too. */
   struct mali_isync_dev d6;
   mali_isync_dev_init(&d6);
   struct fake_kernel ka = {.import_r = 0, .export_r = -EMFILE, .export_fd = -1};
   EXPECT(mali_isync_consumer_ex(&d6, &fake_ops, (struct mali_kdev *)&ka, fds, 2, MALI_DMA_BUF_SYNC_READ) == -EMFILE,
          "consumer export -EMFILE is reported to the queue");
   struct mali_isync_dev d7;
   mali_isync_dev_init(&d7);
   struct fake_kernel kb = {.import_r = 0, .export_r = 0, .export_fd = fd0};
   EXPECT(mali_isync_consumer_ex(&d7, &fake_ops, (struct mali_kdev *)&kb, fds, 1, MALI_DMA_BUF_SYNC_WRITE) == 0,
          "consumer WRITE export succeeds");
   EXPECT(kb.last_flags == MALI_DMA_BUF_SYNC_WRITE, "an imported write asks for WRITE, i.e. the readers' fences too");
   EXPECT(kb.exports == 1 && kb.polls == 1 && kb.blocked == 0, "consumer WRITE: one export and one polled fence (%u/%u)",
          kb.exports, kb.polls);
   mali_isync_dev_finish(&d6);
   mali_isync_dev_finish(&d7);

   mali_isync_dev_finish(&d);
   mali_isync_dev_finish(&d2);
   mali_isync_dev_finish(&d3);
   close(fd0);
   printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
   return fails != 0;
}
