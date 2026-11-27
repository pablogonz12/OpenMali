/* SPDX-License-Identifier: MIT */
/* Host test of the per-submit MEM_SYNC policy in mali_backend_kbase.c (fake kernel from libkbase/src/backend_fake.c counts MEM_SYNC per BO).
 *  - an uncached exported BO (swapchain image) is host-lazy: no sync until bo_host_touch (vkMapMemory)
 *  - after bo_host_touch it is synced at EVERY submit (persistent map: the CPU may read/write it between any two submits)
 *  - a CACHED export (foreign CPU writers) is never lazy
 *  - an imported dma-buf: kbase_bo_import does not track_bo(), so imports are NOT synced per submit (pre-existing, not a regression of the lazy-export
 *    change; pinned here so a change is deliberate). Foreign CPU writers must use DMA_BUF_IOCTL_SYNC / implicit fences.
 *  - VkDeviceMemory (HOSTLAZY) is lazy until touched, then synced every submit
 *  - an arch 11 driver-internal BO (NOCOH) is never synced; with MALI_INT_NOCOH=0 it is
 *  - M15: per-BO write tracking. A fence wait CSYNCs only BOs marked with bo_wrote; the fake kernel models a
 *    non-coherent GPU (kb_fake_gpu_write writes a shadow that only MEM_SYNC type 2 makes visible), so an
 *    unmarked GPU write reads back as stale data - that is the case the test asserts on purpose first. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mali_backend.h"

extern const struct mali_backend_ops mali_backend_kbase;
extern unsigned kb_fake_memsync_count(uint64_t va);
/* fake kernel: write into the BO's shadow (the fake "GPU cache"). The CPU mapping stays stale until a MEM_SYNC
 * type 2 (device->CPU) copies it back, so a skipped sync is observable as stale data. */
extern int kb_fake_gpu_write(uint64_t va, size_t off, const void *src, size_t len);
extern int kb_fake_gpu_read(uint64_t va, size_t off, void *dst, size_t len);   /* what the GPU sees of a BO */

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
      /* the arena is write-combined: the dsb before the submit's doorbell is the ordering it needs, no ioctl */
      EXPECT(kb_fake_memsync_count(hot.gpu_va) == h0n, "submit_cs: write-combined hot BO needs no MEM_SYNC (%u)",
             kb_fake_memsync_count(hot.gpu_va) - h0n);
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
   /* CPU -> device policy: a write-combined mapping (no HOST_CACHED) never needs the ioctl; a cached one needs it
    * only after a CPU write. The old policy synced every mapped BO on every submit (audit-perf item 1). */
   EXPECT(kb_fake_memsync_count(exp.gpu_va) == e0, "mapped export (write-combined): no CPU->device MEM_SYNC in %d submits (%u)", N, kb_fake_memsync_count(exp.gpu_va) - e0);
   EXPECT(kb_fake_memsync_count(hl.gpu_va) == h0, "mapped VkDeviceMemory (write-combined): no CPU->device MEM_SYNC in %d submits (%u)", N, kb_fake_memsync_count(hl.gpu_va) - h0);
   EXPECT(kb_fake_memsync_count(expc.gpu_va) == c0, "CACHED export with no CPU write since the last clean: no MEM_SYNC (%u)", kb_fake_memsync_count(expc.gpu_va) - c0);
   memset(expc.cpu, 0x5a, 64);
   b->bo_cache(k, &expc, 0, 64, false);   /* vkFlushMappedMemoryRanges */
   b->host_sync(k);
   b->submit_cs(k, cs, 0);
   EXPECT(kb_fake_memsync_count(expc.gpu_va) > c0, "CACHED export: one CPU->device MEM_SYNC after a CPU write");
   c0 = kb_fake_memsync_count(expc.gpu_va);
   b->host_sync(k);
   b->submit_cs(k, cs, 0);
   EXPECT(kb_fake_memsync_count(expc.gpu_va) == c0, "CACHED export: the next submit does not sync it again");
   EXPECT(kb_fake_memsync_count(imp.gpu_va) - i0 == 0, "imported dma-buf: untracked, no per-submit MEM_SYNC (known gap, pre-existing)");


   /* ---- M15: per-BO write tracking for the fence wait (MALI_SYNC_TRACK, MALI_SYNC_ALL) ---- */
   {
      enum { NB = 5 };
      struct mali_bo wb[NB];
      unsigned base[NB];
      for (int i = 0; i < NB; i++) {
         if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &wb[i])) { printf("FAIL  bo_alloc write-track\n"); return 1; }
         b->bo_host_touch(k, &wb[i]);   /* app-mapped: these are the BOs the old code swept on every wait */
         memset(wb[i].cpu, 0x11, 64);
         base[i] = kb_fake_memsync_count(wb[i].gpu_va);
      }
      static volatile uint64_t c3 = 1;

      /* (a) no marks, no writes: a fence wait must not CSYNC anything */
      b->wait_counter(k, &c3, 1, 100);
      bool clean = true;
      for (int i = 0; i < NB; i++) clean &= kb_fake_memsync_count(wb[i].gpu_va) == base[i];
      EXPECT(clean, "no GPU write marked: a fence wait CSYNCs no BO (old code: %d)", NB);

      /* (b) the GPU writes BO 0 but the driver does not mark it: the CPU still reads the old bytes. This is the
       *     stale-data case; it must be *observable* here, otherwise the test could not catch a missing sync. */
      uint32_t gpu_val = 0xdeadbeef;
      if (kb_fake_gpu_write(wb[0].gpu_va, 0, &gpu_val, sizeof gpu_val)) { printf("FAIL  kb_fake_gpu_write\n"); return 1; }
      b->wait_counter(k, &c3, 1, 100);
      uint32_t seen = 0;
      memcpy(&seen, wb[0].cpu, sizeof seen);
      EXPECT(seen == 0x11111111, "unmarked GPU write stays invisible to the CPU (stale 0x%08x), i.e. the model detects a missing sync", seen);
      EXPECT(kb_fake_memsync_count(wb[0].gpu_va) == base[0], "unmarked BO: still no MEM_SYNC");

      /* (c) the same write, marked: exactly that BO is CSYNCed and the CPU sees the data */
      b->bo_wrote(k, &wb[0]);
      b->wait_counter(k, &c3, 1, 100);
      memcpy(&seen, wb[0].cpu, sizeof seen);
      EXPECT(seen == 0xdeadbeef, "marked GPU write is visible after the wait (0x%08x)", seen);
      EXPECT(kb_fake_memsync_count(wb[0].gpu_va) == base[0] + 1, "marked BO: exactly one device->CPU MEM_SYNC (%u)", kb_fake_memsync_count(wb[0].gpu_va) - base[0]);
      bool others_clean = true;
      for (int i = 1; i < NB; i++) others_clean &= kb_fake_memsync_count(wb[i].gpu_va) == base[i];
      EXPECT(others_clean, "unmarked BOs of the same device: no MEM_SYNC at that wait");

      /* (d) no mark is lost while a frame is in flight: mark, mark again (two frames), one wait covers both */
      uint32_t a1 = 0x0a0a0a0a, a2 = 0x0b0b0b0b;
      kb_fake_gpu_write(wb[1].gpu_va, 0, &a1, sizeof a1);
      b->bo_wrote(k, &wb[1]);
      kb_fake_gpu_write(wb[1].gpu_va, 0, &a2, sizeof a2);
      b->bo_wrote(k, &wb[1]);   /* second mark before any wait: must not be folded away */
      b->wait_counter(k, &c3, 1, 100);
      memcpy(&seen, wb[1].cpu, sizeof seen);
      EXPECT(seen == a2, "two marks on one BO before a wait are both covered (0x%08x)", seen);

      /* (e) a mark is not re-synced by the next wait with no new write */
      unsigned q0 = kb_fake_memsync_count(wb[1].gpu_va);
      b->wait_counter(k, &c3, 1, 100);
      EXPECT(kb_fake_memsync_count(wb[1].gpu_va) == q0, "already-synced BO is not CSYNCed again by the next wait");

      /* (f) front end could not place a write (bo_wrote(NULL)): one full sweep, then back to tracking */
      kb_fake_gpu_write(wb[2].gpu_va, 0, &gpu_val, sizeof gpu_val);
      b->bo_wrote(k, NULL);
      b->wait_counter(k, &c3, 1, 100);
      memcpy(&seen, wb[2].cpu, sizeof seen);
      EXPECT(seen == 0xdeadbeef, "unplaceable write: the fallback sweeps every BO and the data is visible");
      bool swept = true;
      for (int i = 0; i < NB; i++) swept &= kb_fake_memsync_count(wb[i].gpu_va) > base[i];
      EXPECT(swept, "unplaceable write: one full sweep covered all %d BOs", NB);
      unsigned after_sweep = kb_fake_memsync_count(wb[0].gpu_va);
      b->wait_counter(k, &c3, 1, 100);
      EXPECT(kb_fake_memsync_count(wb[0].gpu_va) == after_sweep, "the sweep is one-shot: tracking resumes");

      /* (g) escape hatch: MALI_SYNC_ALL=1 restores the old behaviour (every mapped BO, every wait) */
      setenv("MALI_SYNC_ALL", "1", 1);
      b->wait_counter(k, &c3, 1, 100);
      bool all = true;
      for (int i = 0; i < NB; i++) all &= kb_fake_memsync_count(wb[i].gpu_va) > base[i];
      EXPECT(all, "MALI_SYNC_ALL=1: every mapped BO is CSYNCed (old behaviour)");
      unsetenv("MALI_SYNC_ALL");

      /* ---- measurement: 300 fence waits, 5 BOs, one BO written per frame ---- */
      double best_track = 1e30, best_all = 1e30;
      unsigned n_track = 0, n_all = 0;
      for (int pass = 0; pass < 3; pass++) {   /* 3 passes: report the best, the host fake backend is noisy */
         unsigned i0 = kb_fake_memsync_count(wb[0].gpu_va), i1 = kb_fake_memsync_count(wb[1].gpu_va),
                  i2 = kb_fake_memsync_count(wb[2].gpu_va), i3 = kb_fake_memsync_count(wb[3].gpu_va),
                  i4 = kb_fake_memsync_count(wb[4].gpu_va);
         struct timespec t0, t1;
         clock_gettime(CLOCK_MONOTONIC, &t0);
         for (int f = 0; f < 300; f++) {
            b->bo_wrote(k, &wb[f % NB]);
            b->wait_counter(k, &c3, 1, 100);
         }
         clock_gettime(CLOCK_MONOTONIC, &t1);
         double us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
         if (us < best_track) best_track = us;
         n_track = (kb_fake_memsync_count(wb[0].gpu_va) - i0) + (kb_fake_memsync_count(wb[1].gpu_va) - i1) +
                   (kb_fake_memsync_count(wb[2].gpu_va) - i2) + (kb_fake_memsync_count(wb[3].gpu_va) - i3) +
                   (kb_fake_memsync_count(wb[4].gpu_va) - i4);

         i0 = kb_fake_memsync_count(wb[0].gpu_va); i1 = kb_fake_memsync_count(wb[1].gpu_va);
         i2 = kb_fake_memsync_count(wb[2].gpu_va); i3 = kb_fake_memsync_count(wb[3].gpu_va);
         i4 = kb_fake_memsync_count(wb[4].gpu_va);
         setenv("MALI_SYNC_ALL", "1", 1);
         clock_gettime(CLOCK_MONOTONIC, &t0);
         for (int f = 0; f < 300; f++)
            b->wait_counter(k, &c3, 1, 100);
         clock_gettime(CLOCK_MONOTONIC, &t1);
         unsetenv("MALI_SYNC_ALL");
         us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
         if (us < best_all) best_all = us;
         n_all = (kb_fake_memsync_count(wb[0].gpu_va) - i0) + (kb_fake_memsync_count(wb[1].gpu_va) - i1) +
                 (kb_fake_memsync_count(wb[2].gpu_va) - i2) + (kb_fake_memsync_count(wb[3].gpu_va) - i3) +
                 (kb_fake_memsync_count(wb[4].gpu_va) - i4);
      }
      printf("M15 sync bench (host fake kernel, 300 fence waits, 5 app-mapped BOs):\n");
      printf("  before (MALI_SYNC_ALL=1): MEM_SYNC %u total = %.2f/wait, %.2f us/wait\n", n_all, n_all / 300.0, best_all / 300.0);
      printf("  after  (per-BO tracking): MEM_SYNC %u total = %.2f/wait, %.2f us/wait\n", n_track, n_track / 300.0, best_track / 300.0);
      printf("  reduction: %.1fx fewer MEM_SYNC ioctls, %.1fx less wait latency\n",
             n_all ? (double)n_all / (double)(n_track ? n_track : 1) : 1.0, best_track > 0 ? best_all / best_track : 1.0);
      EXPECT(n_track >= 300 && n_track <= 600, "tracking syncs the written BO only (+ the in-flight one): %u MEM_SYNC for 300 waits", n_track);
      EXPECT(n_all == 1500, "full sweep: 5 BOs x 300 waits = %u", n_all);
   }
   /* ---- CPU -> device policy: a write-combined mapping needs no MEM_SYNC (the dsb before the doorbell orders it);
    * a CPU-cached one needs the clean, and only when the CPU wrote since the last clean. Before this, every mapped BO
    * was MEM_SYNCed (op 1) on every submit: ~8-20 no-op ioctls per frame-submit for a DXVK/zink app (audit-perf item 1). */
   {
      enum { NWC = 8 };
      struct mali_bo wc[NWC], cac;
      if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY | MALI_BO_CACHED, &cac)) { printf("FAIL  bo_alloc cached\n"); return 1; }
      b->bo_host_touch(k, &cac);
      for (int i = 0; i < NWC; i++) {
         if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &wc[i])) { printf("FAIL  bo_alloc wc\n"); return 1; }
         b->bo_host_touch(k, &wc[i]);   /* vkMapMemory: a persistent UBO/staging ring */
      }
      /* the app fills its rings, exactly the per-frame CPU writes that used to cost an ioctl each */
      for (int i = 0; i < NWC; i++) memset(wc[i].cpu, 0x40 + i, 256);
      memset(cac.cpu, 0x99, 256);
      b->bo_cache(k, &cac, 0, 256, false);   /* vkFlushMappedMemoryRanges on the cached mapping */

      unsigned w0[NWC], k0 = kb_fake_memsync_count(cac.gpu_va);
      for (int i = 0; i < NWC; i++) w0[i] = kb_fake_memsync_count(wc[i].gpu_va);
      uint64_t cs2[8] = {0};
      enum { NS = 5 };
      for (int s = 0; s < NS; s++) {
         for (int i = 0; i < NWC; i++) memset(wc[i].cpu, 0x60 + i, 256);   /* refill the ring every frame */
         b->host_sync(k);
         b->submit_cs(k, cs2, 0);
      }
      unsigned wc_total = 0;
      for (int i = 0; i < NWC; i++) wc_total += kb_fake_memsync_count(wc[i].gpu_va) - w0[i];
      unsigned cac_total = kb_fake_memsync_count(cac.gpu_va) - k0;
      printf("memsync per submit (host fake kernel, %d mapped WC BOs + 1 cached):\n", NWC);
      printf("  before (every mapped BO): MEM_SYNC %u + %u per %d submits = %.1f/submit\n",
             NWC * NS, NS, NS, (double)(NWC * NS + NS) / NS);
      printf("  after  (WC needs none, cached only when written): MEM_SYNC %u + %u per %d submits = %.1f/submit\n",
             wc_total, cac_total, NS, (double)(wc_total + cac_total) / NS);
      EXPECT(wc_total == 0, "write-combined mappings: no CPU->device MEM_SYNC at all (%u in %d submits)", wc_total, NS);
      EXPECT(cac_total <= 1, "cached mapping: one MEM_SYNC for the write, not one per submit (%u in %d submits)", cac_total, NS);
      uint8_t got = 0;
      if (kb_fake_gpu_read(wc[3].gpu_va, 0, &got, 1)) { printf("FAIL  kb_fake_gpu_read\n"); return 1; }
      EXPECT(got == 0x63, "write after map still reaches the GPU with no sync (GPU reads 0x%02x)", got);
      got = 0;
      if (kb_fake_gpu_read(cac.gpu_va, 0, &got, 1)) { printf("FAIL  kb_fake_gpu_read\n"); return 1; }
      EXPECT(got == 0x99, "cached mapping: the flushed write reaches the GPU (0x%02x)", got);
      /* escape hatch: MALI_SYNC_ALL=1 restores the old sweep for A/B */
      for (int i = 0; i < NWC; i++) w0[i] = kb_fake_memsync_count(wc[i].gpu_va);
      setenv("MALI_SYNC_ALL", "1", 1);
      b->host_sync(k);
      b->submit_cs(k, cs2, 0);
      unsetenv("MALI_SYNC_ALL");
      unsigned wc_old = 0;
      for (int i = 0; i < NWC; i++) wc_old += kb_fake_memsync_count(wc[i].gpu_va) - w0[i];
      EXPECT(wc_old == NWC, "MALI_SYNC_ALL=1: every mapped BO is synced again at the submit start (%u)", wc_old);
   }

   /* the device -> CPU direction after a fence wait covers mapped BOs only */
   unsigned before = kb_fake_memsync_count(exp.gpu_va);
   static volatile uint64_t cnt = 1;
   b->wait_counter(k, &cnt, 1, 100);
   EXPECT(kb_fake_memsync_count(exp.gpu_va) == before, "mapped export with no GPU-write mark: no device->CPU sync (M15 tracking)");
   b->bo_wrote(k, &exp);
   b->wait_counter(k, &cnt, 1, 100);
   EXPECT(kb_fake_memsync_count(exp.gpu_va) > before, "mapped export: device->CPU invalidate after a counter wait, once the GPU wrote it");
   b->device_destroy(k);
   printf("{\"test\":\"t_memsync\",\"pass\":%s}\n", fails ? "false" : "true");
   return fails != 0;
}
