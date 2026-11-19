/* SPDX-License-Identifier: MIT */
/* Host test: O(1) untracking of BOs (kb_bo.track_idx) keeps the tracked array consistent. Frees BOs in scrambled order, then
 * checks via MEM_SYNC counts that exactly the surviving touched BOs are still synced and the freed ones are not. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/mali_backend_kbase.c"   /* white box: reads k->nbos */
extern unsigned kb_fake_memsync_count(uint64_t va);
static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL  "); } else printf("ok    "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define N 200
int main(void)
{
   const struct mali_backend_ops *b = &mali_backend_kbase;
   struct mali_gpu_info gi = {.arch_major = 11, .uapi_major = 1, .uapi_minor = 21, .core_count = 2};
   struct mali_kdev *k;
   if (b->device_create(&gi, &k)) return 1;
   static struct mali_bo bo[N]; static int live[N]; static uint64_t base[N];
   for (int i = 0; i < N; i++) { if (b->bo_alloc(k, 4096, MALI_BO_HOSTLAZY | MALI_BO_EXPORT, &bo[i])) return 1; b->bo_host_touch(k, &bo[i]); live[i] = 1; }
   for (int step = 0; step < 150; step++) {   /* scrambled order */
      int i = (step * 73 + 11) % N;
      if (live[i]) { b->bo_free(k, &bo[i]); live[i] = 0; }
   }
   int nl = 0; for (int i = 0; i < N; i++) nl += live[i];
   for (int i = 0; i < N; i++) base[i] = kb_fake_memsync_count(bo[i].gpu_va);
   uint64_t cs[8] = {0};
   int ghost = 0;
   for (int i = 0; i < N; i += 2) if (live[i]) { b->bo_free(k, &bo[i]); live[i] = 0; nl--; }
   b->host_sync(k); b->submit_cs(k, cs, 0);
   for (int i = 0; i < N; i++) if (!live[i] && kb_fake_memsync_count(bo[i].gpu_va) != base[i]) ghost++;
   EXPECT(ghost == 0, "freed BOs are not synced any more (%d ghosts)", ghost);
   int bad = 0, synced = 0;
   for (int i = 0; i < N; i++) if (live[i]) { if (kb_fake_memsync_count(bo[i].gpu_va) == base[i]) bad++; else synced++; }
   EXPECT(nl > 0 && bad == 0, "all %d surviving BOs still synced (%d missed)", nl, bad);
   /* re-add after removals, then free everything */
   struct mali_bo again; if (b->bo_alloc(k, 4096, MALI_BO_HOSTLAZY | MALI_BO_EXPORT, &again)) return 1; b->bo_host_touch(k, &again);
   unsigned a0 = kb_fake_memsync_count(again.gpu_va); b->host_sync(k); b->submit_cs(k, cs, 0);
   EXPECT(kb_fake_memsync_count(again.gpu_va) > a0, "BO allocated after removals is tracked");
   b->bo_free(k, &again);
   for (int i = 0; i < N; i++) if (live[i]) b->bo_free(k, &bo[i]);
   EXPECT(k->nbos == 0, "all BOs untracked after freeing everything (nbos=%u)", k->nbos);
   b->device_destroy(k);
   printf(fails ? "FAILED\n" : "PASS\n");
   return fails != 0;
}
