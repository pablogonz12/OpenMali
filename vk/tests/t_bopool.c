/* SPDX-License-Identifier: MIT */
/* Host test of the VkDeviceMemory BO free list in mali_backend_kbase.c (libkbase fake kernel counts MEM_ALLOC / MEM_FREE ioctls).
 *  - a freed HOSTLAZY BO is reused by the next alloc of the same size and flags: no MEM_ALLOC, no MEM_FREE; a different size or flags misses
 *  - a steady 8-alloc/frame loop costs MEM_ALLOC only for the first frame
 *  - never pooled: EXPORT, EXEC, EVENT and driver-internal (non HOSTLAZY) BOs are really freed
 *  - the pool is bounded: past MALI_BO_POOL_MB the oldest are really freed (MEM_FREE count grows), a BO over the per-BO cap is never pooled
 *  - a reused BO is lazy again (no MEM_SYNC until bo_host_touch) and, once touched, tracked for MEM_SYNC like a fresh one
 *  - device_destroy frees everything still pooled (MEM_FREE count == MEM_ALLOC count) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mali_backend.h"

extern const struct mali_backend_ops mali_backend_kbase;
extern unsigned kb_fake_mem_alloc_count(void), kb_fake_mem_free_count(void);
extern unsigned kb_fake_memsync_count(uint64_t va);
extern unsigned kb_test_cache_flush_count(void);

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); } else { printf("ok    "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
   setenv("MALI_BO_POOL_MB", "8", 1);
   setenv("MALI_BO_COHERENT_LOCAL", "1", 1);   /* the MEM_SYNC assertions below need COHERENT_LOCAL whatever the caller exported */
   const struct mali_backend_ops *b = &mali_backend_kbase;
   struct mali_gpu_info gi = {.arch_major = 11, .uapi_major = 1, .uapi_minor = 21, .core_count = 2};
   struct mali_kdev *k;
   if (b->device_create(&gi, &k)) { printf("FAIL  device_create\n"); return 1; }
   unsigned a0 = kb_fake_mem_alloc_count(), f0 = kb_fake_mem_free_count();
   struct mali_bo x, y;

   /* reuse */
   if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &x)) return 1;
   uint64_t va = x.gpu_va;
   b->bo_free(k, &x);
   EXPECT(kb_fake_mem_free_count() == f0, "free of a HOSTLAZY BO keeps it (no MEM_FREE)");
   a0 = kb_fake_mem_alloc_count();
   if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &y)) return 1;
   EXPECT(kb_fake_mem_alloc_count() == a0 && y.gpu_va == va && y.size == 65536, "same size+flags: reused, no MEM_ALLOC");
   b->bo_free(k, &y);
   if (b->bo_alloc(k, 131072, MALI_BO_HOSTLAZY, &y)) return 1;
   EXPECT(kb_fake_mem_alloc_count() == a0 + 1, "other size: real MEM_ALLOC");
   b->bo_free(k, &y);
   if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY | MALI_BO_CACHED, &y)) return 1;
   EXPECT(kb_fake_mem_alloc_count() == a0 + 2 && y.gpu_va != va, "other flags: real MEM_ALLOC");
   b->bo_free(k, &y);

   /* steady state: 8 allocs per frame, ring of 3 frames */
   a0 = kb_fake_mem_alloc_count();
   struct mali_bo ring[3][8];
   for (int f = 0; f < 30; f++) {
      if (f >= 3) for (int i = 0; i < 8; i++) b->bo_free(k, &ring[f % 3][i]);
      for (int i = 0; i < 8; i++) if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &ring[f % 3][i])) return 1;
   }
   EXPECT(kb_fake_mem_alloc_count() - a0 <= 8 * 3, "30 frames x 8 allocs: %u MEM_ALLOC (first 3 frames only)", kb_fake_mem_alloc_count() - a0);
   for (int f = 0; f < 3; f++) for (int i = 0; i < 8; i++) b->bo_free(k, &ring[f][i]);

   /* never pooled */
   uint32_t never[] = {MALI_BO_EXPORT, MALI_BO_EXEC, MALI_BO_EVENT, 0, MALI_BO_HOT};
   for (unsigned i = 0; i < sizeof never / sizeof never[0]; i++) {
      f0 = kb_fake_mem_free_count();
      if (b->bo_alloc(k, 4096, never[i], &x)) { printf("FAIL  alloc flags %x\n", never[i]); return 1; }
      b->bo_free(k, &x);
      EXPECT(kb_fake_mem_free_count() == f0 + 1, "flags 0x%x: really freed", never[i]);
   }

   /* bound: 8 MiB pool, 2 MiB BOs -> at most 4 stay */
   struct mali_bo many[8];
   f0 = kb_fake_mem_free_count();
   for (int i = 0; i < 8; i++) if (b->bo_alloc(k, 2u << 20, MALI_BO_HOSTLAZY, &many[i])) return 1;
   for (int i = 0; i < 8; i++) b->bo_free(k, &many[i]);
   a0 = kb_fake_mem_alloc_count();
   for (int i = 0; i < 8; i++) if (b->bo_alloc(k, 2u << 20, MALI_BO_HOSTLAZY, &many[i])) return 1;
   EXPECT(kb_fake_mem_alloc_count() - a0 == 4, "8 x 2 MiB freed into an 8 MiB pool: 4 kept, 4 real MEM_ALLOC on re-alloc (%u)", kb_fake_mem_alloc_count() - a0);
   for (int i = 0; i < 8; i++) b->bo_free(k, &many[i]);
   f0 = kb_fake_mem_free_count();
   if (b->bo_alloc(k, 32u << 20, MALI_BO_HOSTLAZY, &x)) return 1;
   b->bo_free(k, &x);
   EXPECT(kb_fake_mem_free_count() == f0 + 1, "BO larger than the pool is never kept");

   /* sync state of a reused BO */
   if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &x)) return 1;
   b->bo_host_touch(k, &x);
   va = x.gpu_va;
   b->bo_free(k, &x);
   if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &y)) return 1;
   EXPECT(y.gpu_va == va, "reused the touched BO");
   uint64_t cs[8] = {0};
   unsigned s0 = kb_fake_memsync_count(va);
   b->host_sync(k); b->submit_cs(k, cs, 0);
   EXPECT(kb_fake_memsync_count(va) == s0, "reused BO is lazy again: no MEM_SYNC before bo_host_touch");
   b->bo_host_touch(k, &y);
   b->host_sync(k); b->submit_cs(k, cs, 0);
   EXPECT(kb_fake_memsync_count(va) > s0, "after bo_host_touch the reused BO is synced");
   b->bo_free(k, &y);
   s0 = kb_fake_memsync_count(va);
   b->host_sync(k); b->submit_cs(k, cs, 0);
   EXPECT(kb_fake_memsync_count(va) == s0, "pooled BO is not synced");

   /* a pooled CPU-cached BO must not hold dirty lines: written back on release if the CPU touched it, nothing to do if it never did */
   {
      if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY | MALI_BO_CACHED, &x)) return 1;
      unsigned c0 = kb_test_cache_flush_count();
      b->bo_free(k, &x);   /* never touched: lazy */
      EXPECT(kb_test_cache_flush_count() == c0, "untouched cached BO: no cache maintenance on release");
      if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY | MALI_BO_CACHED, &x)) return 1;
      b->bo_host_touch(k, &x);
      c0 = kb_test_cache_flush_count();
      b->bo_free(k, &x);
      EXPECT(kb_test_cache_flush_count() > c0, "CPU-touched cached BO is cleaned before it enters the pool");
      if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &x)) return 1;
      b->bo_host_touch(k, &x);
      c0 = kb_test_cache_flush_count();
      b->bo_free(k, &x);
      EXPECT(kb_test_cache_flush_count() == c0, "uncached BO: no cache maintenance");
      /* a BO the pool cannot keep (over POOL_MAX_BO_BYTES) is freed for real: no point flushing it first */
      if (b->bo_alloc(k, 32u << 20, MALI_BO_HOSTLAZY | MALI_BO_CACHED, &x)) return 1;
      b->bo_host_touch(k, &x);
      c0 = kb_test_cache_flush_count();
      b->bo_free(k, &x);
      EXPECT(kb_test_cache_flush_count() == c0, "oversize cached BO: no cache maintenance before the real free");
   }

   b->device_destroy(k);

   /* destroy frees what is still pooled: device A pools 5 BOs, device B none; A must do exactly 5 more frees */
   unsigned d[2];
   for (int pooled = 1; pooled >= 0; pooled--) {
      if (b->device_create(&gi, &k)) { printf("FAIL  device_create\n"); return 1; }
      if (pooled) {
         struct mali_bo five[5];
         for (int i = 0; i < 5; i++) if (b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &five[i])) return 1;
         for (int i = 0; i < 5; i++) b->bo_free(k, &five[i]);
      }
      f0 = kb_fake_mem_free_count();
      b->device_destroy(k);
      d[pooled] = kb_fake_mem_free_count() - f0;
   }
   EXPECT(d[1] - d[0] == 5, "device_destroy frees the 5 pooled BOs (%u vs %u frees)", d[1], d[0]);
   printf("{\"test\":\"t_bopool\",\"pass\":%s}\n", fails ? "false" : "true");
   return fails != 0;
}
