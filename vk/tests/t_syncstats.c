/* SPDX-License-Identifier: MIT */
/* Host test of the MEM_SYNC sweep counters (mali_kbase_sync.h) on libkbase's fake kernel, arch 11.
 * A known BO population and a known sequence of host_sync / submit_cs / fence poll / counter wait; the counters must
 * report exactly the calls, BOs visited and ioctls the policy implies, and the live-BO classes must match.
 * Run once per mode: MALI_SYNC_LISTS=0 (the registry walk visits every BO at every sweep) and the default (lists:
 * only the BOs that need a sync are visited; write-combined natives cost nothing). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mali_backend.h"
#include "mali_kbase_sync.h"

extern const struct mali_backend_ops mali_backend_kbase;
extern unsigned kb_fake_memsync_count(uint64_t va);

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL  "); } else printf("ok    "); printf(__VA_ARGS__); printf("\n"); } while (0)

static const char *const kn[] = {"cold", "hot", "poll", "wait"};

static void
expect_counter(const struct mali_sync_stats *s, int kind, int dir, unsigned long long calls, unsigned long long visited,
               unsigned long long ioctls)
{
   const struct mali_sync_counter *c = &s->c[kind][dir];
   EXPECT(c->calls == calls && c->visited == visited && c->ioctls == ioctls,
          "%s.%s: calls %llu (want %llu) visited %llu (want %llu) ioctls %llu (want %llu)", kn[kind], dir ? "d2c" : "c2d",
          (unsigned long long)c->calls, calls, (unsigned long long)c->visited, visited, (unsigned long long)c->ioctls, ioctls);
}

int main(void)
{
   const char *le = getenv("MALI_SYNC_LISTS");
   const bool lists = !le || le[0] != '0';
   const char *we = getenv("MALI_SYNC_WC");
   const bool wc_skip = lists && (!we || we[0] != '0');
   const struct mali_backend_ops *b = &mali_backend_kbase;
   struct mali_gpu_info gi = {.arch_major = 11, .uapi_major = 1, .uapi_minor = 21, .core_count = 2};
   struct mali_kdev *k;
   if (b->device_create(&gi, &k)) { printf("FAIL  device_create\n"); return 1; }
   printf("mode: %s%s\n", lists ? "lists" : "walk (MALI_SYNC_LISTS=0)", lists ? (wc_skip ? " + wc skip" : " (MALI_SYNC_WC=0)") : "");

   /* population: 7 BOs */
   struct mali_bo u1, u2, iwc, icached, hot, ex, imp;
   int r = 0;
   r |= b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &u1);                 /* user, mapped below */
   r |= b->bo_alloc(k, 65536, MALI_BO_HOSTLAZY, &u2);                 /* user, never mapped: lazy */
   r |= b->bo_alloc(k, 65536, 0, &iwc);                               /* internal WC; arch 11: NOCOH, never synced */
   r |= b->bo_alloc(k, 65536, MALI_BO_CACHED, &icached);              /* internal CPU-cached: synced at every cold sweep */
   r |= b->bo_alloc(k, 65536, MALI_BO_HOT | MALI_BO_COH, &hot);       /* internal WC, hot: hot sweep only */
   r |= b->bo_alloc(k, 65536, MALI_BO_EXPORT, &ex);                   /* exported dma-buf, uncached: lazy until mapped */
   if (r) { printf("FAIL  bo_alloc\n"); return 1; }
   int fd = b->bo_export(k, &ex);
   if (fd < 0 || b->bo_import(k, fd, 65536, 0, &imp)) { printf("FAIL  export/import\n"); return 1; }
   close(fd);
   b->bo_host_touch(k, &u1);   /* vkMapMemory, stays mapped */

   struct mali_sync_stats s;
   mali_kbase_sync_stats(k, &s, true);
   EXPECT(s.nbos == 7, "registry holds 7 BOs (%u)", s.nbos);
   EXPECT(s.live[MALI_BOC_USER_MAPPED] == 1 && s.live[MALI_BOC_USER_UNMAPPED] == 1, "user BOs: 1 mapped, 1 unmapped (%u, %u)",
          s.live[MALI_BOC_USER_MAPPED], s.live[MALI_BOC_USER_UNMAPPED]);
   EXPECT(s.live[MALI_BOC_INT_WC] == 2 && s.live[MALI_BOC_INT_CACHED] == 1, "internal: 2 WC, 1 cached (%u, %u)",
          s.live[MALI_BOC_INT_WC], s.live[MALI_BOC_INT_CACHED]);
   EXPECT(s.live[MALI_BOC_IMPORTED] == 1 && s.live[MALI_BOC_EXPORTED] == 1, "1 imported, 1 exported (%u, %u)",
          s.live[MALI_BOC_IMPORTED], s.live[MALI_BOC_EXPORTED]);

   /* the sequence: 3 queue submits (cold), 2 command streams (hot), a GPU write to u1 + icached, one fence poll, one wait */
   uint64_t cs[8] = {0};
   for (int i = 0; i < 3; i++) b->host_sync(k);
   for (int i = 0; i < 2; i++) b->submit_cs(k, cs, 0);
   b->bo_wrote(k, &u1);
   b->bo_wrote(k, &icached);
   int ffd = -1;
   if (b->submit_signal(k, &ffd) || ffd < 0) { printf("FAIL  submit_signal\n"); return 1; }
   int pr = b->fence_wait(k, ffd, 0);
   EXPECT(pr == 0, "fence poll on a signalled fence returns 0 (%d)", pr);
   b->bo_wrote(k, &u1);
   static volatile uint64_t cnt = 1;
   b->wait_counter(k, &cnt, 1, 100);
   close(ffd);

   mali_kbase_sync_stats(k, &s, true);
   /* Policy per BO (both modes):            cold c2d   hot c2d   poll d2c          wait d2c
    *   u1 (user, mapped, WC)                 sync       -         marked: sync      marked again: sync
    *   u2 (user, lazy)                       -          -         -                 -
    *   iwc (NOCOH)                           -          -         -                 -
    *   icached (internal, cached)            sync       -         marked: sync      -
    *   hot (internal WC, hot)                -          sync      -                 -
    *   ex (exported, lazy)                   -          -         -                 -
    *   imp (import)                          -          -         - (poll skips)    sync
    * With the wc skip, u1 and hot are write-combined natives: kbase's MEM_SYNC is a no-op for them, no ioctl. */
   if (!lists) {
      expect_counter(&s, MALI_SYNC_K_COLD, MALI_SYNC_C2D, 3, 3 * 7, 3 * 2);
      expect_counter(&s, MALI_SYNC_K_HOT, MALI_SYNC_C2D, 2, 2 * 7, 2 * 1);
      expect_counter(&s, MALI_SYNC_K_POLL, MALI_SYNC_D2C, 1, 7, 2);
      expect_counter(&s, MALI_SYNC_K_WAIT, MALI_SYNC_D2C, 1, 7, 2);
   } else if (!wc_skip) {
      expect_counter(&s, MALI_SYNC_K_COLD, MALI_SYNC_C2D, 3, 3 * 2, 3 * 2);
      expect_counter(&s, MALI_SYNC_K_HOT, MALI_SYNC_C2D, 2, 2 * 1, 2 * 1);
      expect_counter(&s, MALI_SYNC_K_POLL, MALI_SYNC_D2C, 1, 2, 2);
      expect_counter(&s, MALI_SYNC_K_WAIT, MALI_SYNC_D2C, 1, 2, 2);   /* pending u1 + the import list */
   } else {
      expect_counter(&s, MALI_SYNC_K_COLD, MALI_SYNC_C2D, 3, 3 * 1, 3 * 1);
      expect_counter(&s, MALI_SYNC_K_HOT, MALI_SYNC_C2D, 2, 0, 0);
      expect_counter(&s, MALI_SYNC_K_POLL, MALI_SYNC_D2C, 1, 1, 1);
      expect_counter(&s, MALI_SYNC_K_WAIT, MALI_SYNC_D2C, 1, 1, 1);   /* the import only */
   }
   mali_kbase_sync_stats(k, &s, false);
   EXPECT(s.c[MALI_SYNC_K_COLD][MALI_SYNC_C2D].calls == 0, "reset clears the counters");

   /* the per-BO ioctl counts of the fake kernel agree with the counters */
   EXPECT(kb_fake_memsync_count(u2.gpu_va) == 0 && kb_fake_memsync_count(iwc.gpu_va) == 0 && kb_fake_memsync_count(ex.gpu_va) == 0,
          "lazy, NOCOH and unmapped-export BOs: no MEM_SYNC");
   EXPECT(kb_fake_memsync_count(icached.gpu_va) == 4, "cached internal BO: 3 cold + 1 poll = 4 MEM_SYNC (%u)",
          kb_fake_memsync_count(icached.gpu_va));
   EXPECT(kb_fake_memsync_count(u1.gpu_va) == (wc_skip ? 0u : 5u), "mapped WC user BO: %u MEM_SYNC (%u)", wc_skip ? 0u : 5u,
          kb_fake_memsync_count(u1.gpu_va));

   b->bo_free(k, &imp);
   b->bo_free(k, &ex);
   b->bo_free(k, &hot);
   b->bo_free(k, &icached);
   b->bo_free(k, &iwc);
   b->bo_free(k, &u2);
   b->bo_free(k, &u1);
   mali_kbase_sync_stats(k, &s, false);
   EXPECT(s.nbos == 0, "registry empty after freeing everything (%u)", s.nbos);
   b->device_destroy(k);
   printf("{\"test\":\"t_syncstats\",\"pass\":%s}\n", fails ? "false" : "true");
   return fails != 0;
}
