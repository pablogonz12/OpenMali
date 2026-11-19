/* SPDX-License-Identifier: MIT */
/* Host test of the per-submit MEM_SYNC policy in mali_backend_kbase.c (fake kernel from libkbase/src/backend_fake.c counts MEM_SYNC per BO).
 *  - an uncached exported BO (swapchain image) is host-lazy: no sync until bo_host_touch (vkMapMemory)
 *  - after bo_host_touch it is synced at EVERY submit (persistent map: the CPU may read/write it between any two submits)
 *  - a CACHED export (foreign CPU writers) is never lazy
 *  - an imported dma-buf: kbase_bo_import does not track_bo(), so imports are NOT synced per submit (pre-existing, not a regression of the lazy-export
 *    change; pinned here so a change is deliberate). Foreign CPU writers must use DMA_BUF_IOCTL_SYNC / implicit fences.
 *  - VkDeviceMemory (HOSTLAZY) is lazy until touched, then synced every submit
 *  - an arch 11 driver-internal BO (NOCOH) is never synced; with MALI_INT_NOCOH=0 it is */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mali_backend.h"

extern const struct mali_backend_ops mali_backend_kbase;
extern unsigned kb_fake_memsync_count(uint64_t va);

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); } else { printf("ok    "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
   const struct mali_backend_ops *b = &mali_backend_kbase;
   struct mali_gpu_info gi = {.arch_major = 11, .uapi_major = 1, .uapi_minor = 21, .core_count = 2};
   struct mali_kdev *k;
   if (b->device_create(&gi, &k)) { printf("FAIL  device_create\n"); return 1; }

   struct mali_bo exp, expc, hl, imp, internal, src;
   int r = 0;
   r |= b->bo_alloc(k, 65536, MALI_BO_EXPORT, &exp);
   r |= b->bo_alloc(k, 65536, MALI_BO_EXPORT | MALI_BO_CACHED, &expc);
   r |= b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &hl);
   r |= b->bo_alloc(k, 65536, 0, &internal);
   r |= b->bo_alloc(k, 65536, MALI_BO_EXPORT, &src);
   if (r) { printf("FAIL  bo_alloc\n"); return 1; }
   struct mali_bo hot;
   int hr = b->bo_alloc(k, 65536, MALI_BO_HOT | MALI_BO_COH, &hot);
   if (hr) { printf("FAIL  bo_alloc hot\n"); return 1; }
   int fd = b->bo_export(k, &src);
   if (fd < 0 || b->bo_import(k, fd, 65536, 0, &imp)) { printf("FAIL  bo_export/import\n"); return 1; }

   {  /* hot BO (arena): written only by the frame build, so host_sync leaves it to submit_cs: exactly one MEM_SYNC per CS submit */
      unsigned h0n = kb_fake_memsync_count(hot.gpu_va);
      b->host_sync(k);
      EXPECT(kb_fake_memsync_count(hot.gpu_va) == h0n, "host_sync: hot BO not synced (%u)", kb_fake_memsync_count(hot.gpu_va) - h0n);
      uint64_t c0s[8] = {0};
      b->submit_cs(k, c0s, 0);
      EXPECT(kb_fake_memsync_count(hot.gpu_va) - h0n == 1, "submit_cs: hot BO synced once (%u)", kb_fake_memsync_count(hot.gpu_va) - h0n);
   }
   unsigned e0 = kb_fake_memsync_count(exp.gpu_va), c0 = kb_fake_memsync_count(expc.gpu_va), h0 = kb_fake_memsync_count(hl.gpu_va),
            i0 = kb_fake_memsync_count(imp.gpu_va), n0 = kb_fake_memsync_count(internal.gpu_va);
   uint64_t cs[8] = {0};
   enum { N = 5 };
   for (int s = 0; s < 2; s++) {
      for (int i = 0; i < N; i++) { b->host_sync(k); b->submit_cs(k, cs, 0); }
      if (s == 0) {
         EXPECT(kb_fake_memsync_count(exp.gpu_va) == e0, "unmapped uncached export: no MEM_SYNC in %d submits", N);
         EXPECT(kb_fake_memsync_count(hl.gpu_va) == h0, "unmapped VkDeviceMemory (HOSTLAZY): no MEM_SYNC in %d submits", N);
         b->bo_host_touch(k, &exp);   /* vkMapMemory */
         b->bo_host_touch(k, &hl);
         e0 = kb_fake_memsync_count(exp.gpu_va); h0 = kb_fake_memsync_count(hl.gpu_va);
      }
   }
   EXPECT(kb_fake_memsync_count(exp.gpu_va) - e0 >= N, "mapped export (persistent map): synced at each of %d submits (%u)", N, kb_fake_memsync_count(exp.gpu_va) - e0);
   EXPECT(kb_fake_memsync_count(hl.gpu_va) - h0 >= N, "mapped VkDeviceMemory: synced at each of %d submits (%u)", N, kb_fake_memsync_count(hl.gpu_va) - h0);
   EXPECT(kb_fake_memsync_count(expc.gpu_va) - c0 >= 2 * N, "CACHED export: never lazy, synced at each submit (%u)", kb_fake_memsync_count(expc.gpu_va) - c0);
   EXPECT(kb_fake_memsync_count(imp.gpu_va) - i0 == 0, "imported dma-buf: untracked, no per-submit MEM_SYNC (known gap, pre-existing)");
   EXPECT(kb_fake_memsync_count(internal.gpu_va) == n0, "arch 11 internal BO: no MEM_SYNC (NOCOH)");
   /* the device -> CPU direction after a fence wait covers mapped BOs only */
   unsigned before = kb_fake_memsync_count(exp.gpu_va);
   static volatile uint64_t cnt = 1;
   b->wait_counter(k, &cnt, 1, 100);
   EXPECT(kb_fake_memsync_count(exp.gpu_va) > before, "mapped export: device->CPU invalidate after a counter wait");

   /* ---- present path: wsi_common waits on its own per-image throttle fence before every present (wsi_common.c:2469)
    * and that wait precedes no CPU read of driver memory, so the device->CPU sweep must be skipped there. The app's
    * own vkWaitForFences must keep it. li = mapped BOs the sweep covers (exp, expc, hl). ---- */
   {
      #define SWEPT() (kb_fake_memsync_count(exp.gpu_va) + kb_fake_memsync_count(expc.gpu_va) + kb_fake_memsync_count(hl.gpu_va))
      int fd = -1;
      EXPECT(b->submit_signal(k, &fd) == 0 && fd > 0, "submit_signal: fence fd (%d)", fd);
      unsigned w0 = SWEPT();
      EXPECT(b->fence_wait(k, fd, 100) == 0, "app fence wait: signalled");
      EXPECT(SWEPT() > w0, "app fence wait (vkWaitForFences): still invalidates every mapped BO (%u)", SWEPT() - w0);

      /* the throttle wait, blocking (wsi_common.c:2469: timeout ~0ull) and as the timeout-0 poll zink issues */
      w0 = SWEPT();
      mali_fence_wait_no_csync++;
      EXPECT(b->fence_wait(k, fd, 100) == 0, "wsi throttle wait (blocking): signalled");
      EXPECT(SWEPT() == w0, "wsi throttle wait: no MEM_SYNC sweep (%u)", SWEPT() - w0);
      EXPECT(b->fence_wait(k, fd, 0) == 0, "wsi throttle wait (poll, timeout 0): signalled");
      EXPECT(SWEPT() == w0, "wsi throttle poll: no MEM_SYNC sweep (%u)", SWEPT() - w0);
      mali_fence_wait_no_csync--;
      close(fd);

      /* mutation check: with the flag clear the same poll must sweep again, otherwise the case above could pass
       * because the sweep never runs at all. */
      fd = -1;
      b->submit_signal(k, &fd);
      w0 = SWEPT();
      EXPECT(b->fence_wait(k, fd, 0) == 0, "flag clear, poll: signalled");
      EXPECT(SWEPT() > w0, "flag clear, poll: the sweep runs (mutation check, %u)", SWEPT() - w0);
      close(fd);

      /* measurement: 300 present-shaped frames (frame CS + signal + throttle wait), old sweep vs skipped */
      enum { FR = 300 };
      double best[2] = {1e30, 1e30};
      unsigned n[2] = {0, 0};
      for (int pass = 0; pass < 3; pass++) {
         for (int skip = 0; skip < 2; skip++) {
            unsigned tot = 0;
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int f = 0; f < FR; f++) {
               int f2 = -1;
               b->submit_cs(k, cs, 0);
               if (b->submit_signal(k, &f2) != 0 || f2 < 0) { EXPECT(0, "present bench: submit_signal"); break; }
               unsigned b0 = SWEPT();
               if (skip)
                  mali_fence_wait_no_csync++;
               b->fence_wait(k, f2, (f & 1) ? 0 : 100);   /* both wsi wait shapes */
               if (skip)
                  mali_fence_wait_no_csync--;
               close(f2);
               tot += SWEPT() - b0;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
            if (us < best[skip]) best[skip] = us;
            n[skip] = tot;
         }
      }
      printf("present-path bench (host fake kernel, 300 presents, 3 mapped BOs):\n");
      printf("  throttle wait sweeping (today): MEM_SYNC %u = %.2f/present, %.2f us/present\n", n[0], n[0] / 300.0, best[0] / 300.0);
      printf("  throttle wait skipped (fix):    MEM_SYNC %u = %.2f/present, %.2f us/present\n", n[1], n[1] / 300.0, best[1] / 300.0);
      EXPECT(n[1] == 0, "300 presents: the throttle wait issues no MEM_SYNC at all (%u)", n[1]);
      EXPECT(n[0] >= 300, "300 presents without the fix: at least one MEM_SYNC sweep per present (%u)", n[0]);
      #undef SWEPT
   }
   b->device_destroy(k);
   printf("{\"test\":\"t_memsync\",\"pass\":%s}\n", fails ? "false" : "true");
   return fails != 0;
}
