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

   struct mali_bo exp, expc, hl, imp, internal, src, ex;
   int r = 0;
   r |= b->bo_alloc(k, 65536, MALI_BO_EXPORT, &exp);
   r |= b->bo_alloc(k, 65536, MALI_BO_EXPORT | MALI_BO_CACHED, &expc);
   r |= b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &hl);
   r |= b->bo_alloc(k, 65536, 0, &internal);
   r |= b->bo_alloc(k, 65536, MALI_BO_EXPORT, &src);
   r |= b->bo_alloc(k, 4096, MALI_BO_EXEC, &ex);
   if (r) { printf("FAIL  bo_alloc\n"); return 1; }
   int fd = b->bo_export(k, &src);
   if (fd < 0 || b->bo_import(k, fd, 65536, 0, &imp)) { printf("FAIL  bo_export/import\n"); return 1; }

   unsigned e0 = kb_fake_memsync_count(exp.gpu_va), c0 = kb_fake_memsync_count(expc.gpu_va), h0 = kb_fake_memsync_count(hl.gpu_va),
            i0 = kb_fake_memsync_count(imp.gpu_va), n0 = kb_fake_memsync_count(internal.gpu_va), x0 = kb_fake_memsync_count(ex.gpu_va);
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
   EXPECT(kb_fake_memsync_count(ex.gpu_va) == x0, "EXEC BO (shader binary, write-combined): no per-submit MEM_SYNC");
   /* the device -> CPU direction after a fence wait covers mapped BOs only */
   unsigned before = kb_fake_memsync_count(exp.gpu_va);
   static volatile uint64_t cnt = 1;
   b->wait_counter(k, &cnt, 1, 100);
   EXPECT(kb_fake_memsync_count(exp.gpu_va) > before, "mapped export: device->CPU invalidate after a counter wait");
   b->device_destroy(k);
   printf("{\"test\":\"t_memsync\",\"pass\":%s}\n", fails ? "false" : "true");
   return fails != 0;
}
