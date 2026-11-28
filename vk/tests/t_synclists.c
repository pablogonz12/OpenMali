/* SPDX-License-Identifier: MIT */
/* Host test (libkbase fake kernel, arch 13: internal BOs carry COHERENT_LOCAL and are synced by the walk): 5,000 idle
 * BOs and a submit that touches one BO. The default mode (MALI_SYNC_LISTS + MALI_SYNC_WC) must visit and MEM_SYNC
 * O(1) BOs per submit/wait, independent of the idle population; the registry walk (MALI_SYNC_LISTS=0) is measured for
 * comparison. Idle = 2,000 internal write-combined BOs (meta copies, descriptor pools, image-view payloads),
 * 2,000 user BOs that were mapped, written, unmapped and synced once, 1,000 never-mapped user BOs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mali_backend.h"
#include "mali_kbase_sync.h"

extern const struct mali_backend_ops mali_backend_kbase;
extern unsigned kb_fake_memsync_count(uint64_t va);

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL  "); } else printf("ok    "); printf(__VA_ARGS__); printf("\n"); } while (0)

enum { N_INT = 2000, N_USED = 2000, N_LAZY = 1000, N = N_INT + N_USED + N_LAZY, FRAMES = 100 };

static double
now_us(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

int main(void)
{
   const char *le = getenv("MALI_SYNC_LISTS"), *we = getenv("MALI_SYNC_WC");
   const bool lists = !le || le[0] != '0';
   const bool wc_skip = lists && (!we || we[0] != '0');
   const struct mali_backend_ops *b = &mali_backend_kbase;
   struct mali_gpu_info gi = {.arch_major = 13, .uapi_major = 1, .uapi_minor = 30, .core_count = 2};
   struct mali_kdev *k;
   if (b->device_create(&gi, &k)) { printf("FAIL  device_create\n"); return 1; }
   printf("mode: %s\n", !lists ? "walk (MALI_SYNC_LISTS=0)" : wc_skip ? "lists + wc skip (default)" : "lists (MALI_SYNC_WC=0)");

   static struct mali_bo idle[N];
   uint64_t cs[8] = {0};
   for (int i = 0; i < N; i++) {
      uint32_t f = i < N_INT ? 0 : MALI_BO_HOSTLAZY;
      if (b->bo_alloc(k, 4096, f, &idle[i])) { printf("FAIL  bo_alloc %d\n", i); return 1; }
      if (i >= N_INT && i < N_INT + N_USED) {   /* vkMapMemory, write, vkUnmapMemory */
         b->bo_host_touch(k, &idle[i]);
         memset(idle[i].cpu, 0x33, 64);
         b->bo_host_unmap(k, &idle[i]);
      }
   }
   b->host_sync(k);   /* covers the unmapped writes once; from here on those BOs are clean */
   b->submit_cs(k, cs, 0);

   /* the one BO this workload touches: a CPU-cached user BO (the kind that still needs MEM_SYNC), mapped persistently,
    * written by the CPU and by the GPU every frame */
   struct mali_bo hot;
   if (b->bo_alloc(k, 4096, MALI_BO_HOSTLAZY | MALI_BO_CACHED, &hot)) { printf("FAIL  bo_alloc hot\n"); return 1; }
   b->bo_host_touch(k, &hot);
   static volatile uint64_t cnt = 1;
   b->wait_counter(k, &cnt, 1, 100);

   struct mali_sync_stats s;
   mali_kbase_sync_stats(k, &s, true);
   unsigned h0 = kb_fake_memsync_count(hot.gpu_va);
   double t0 = now_us();
   for (int f = 0; f < FRAMES; f++) {
      memset(hot.cpu, f, 64);
      b->host_sync(k);         /* vkQueueSubmit */
      b->submit_cs(k, cs, 0);  /* its command stream */
      b->bo_wrote(k, &hot);    /* the GPU writes it */
      b->wait_counter(k, &cnt, 1, 100);   /* vkWaitForFences */
   }
   double us = now_us() - t0;
   mali_kbase_sync_stats(k, &s, true);
   uint64_t io = 0, vis = 0;
   for (int kind = 0; kind < MALI_SYNC_K_N; kind++)
      for (int d = 0; d < MALI_SYNC_DIR_N; d++) {
         io += s.c[kind][d].ioctls;
         vis += s.c[kind][d].visited;
      }
   printf("%d BOs alive, %d frames: %.2f MEM_SYNC/frame, %.1f BOs visited/frame, %.2f us/frame (host fake kernel)\n",
          s.nbos, FRAMES, (double)io / FRAMES, (double)vis / FRAMES, us / FRAMES);
   EXPECT(kb_fake_memsync_count(hot.gpu_va) - h0 == 2 * FRAMES, "the touched BO: one push + one pull per frame (%u)",
          kb_fake_memsync_count(hot.gpu_va) - h0);
   bool idle_user_clean = true;
   for (int i = N_INT; i < N; i++)
      idle_user_clean &= kb_fake_memsync_count(idle[i].gpu_va) <= 1;
   EXPECT(idle_user_clean, "idle user BOs: at most the one sync that covered their unmapped writes");
   if (wc_skip) {
      EXPECT(io == 2 * FRAMES, "default mode: O(1) ioctls, exactly 2 per frame for one touched BO (%llu)", (unsigned long long)io);
      EXPECT(vis == 2 * FRAMES, "default mode: O(1) BOs visited, 2 per frame among %d (%llu)", N + 1, (unsigned long long)vis);
      bool int_clean = true;
      for (int i = 0; i < N_INT; i++)
         int_clean &= kb_fake_memsync_count(idle[i].gpu_va) == 0;
      EXPECT(int_clean, "internal write-combined BOs: never a MEM_SYNC");
   } else if (lists) {
      /* without the WC skip the internal BOs keep today's per-submit push; the idle user BOs cost nothing */
      EXPECT(io == (uint64_t)(N_INT + 2) * FRAMES, "lists only: internal BOs + the touched one per frame (%llu)", (unsigned long long)io);
      EXPECT(vis == io, "lists only: every visit is an ioctl (%llu)", (unsigned long long)vis);
   } else {
      EXPECT(vis >= (uint64_t)3 * (N + 1) * FRAMES, "walk: every sweep visits every BO (%llu)", (unsigned long long)vis);
   }

   /* removal is O(1): freeing the whole population must not be quadratic (5,001 frees) */
   t0 = now_us();
   for (int i = 0; i < N; i++)
      b->bo_free(k, &idle[i]);
   b->bo_free(k, &hot);
   printf("freeing %d BOs: %.0f us\n", N + 1, now_us() - t0);
   mali_kbase_sync_stats(k, &s, false);
   EXPECT(s.nbos == 0 && s.push_list == 0 && s.pend_list == 0, "registry and lists empty after the frees (%u %u %u)", s.nbos,
          s.push_list, s.pend_list);
   b->device_destroy(k);
   printf("{\"test\":\"t_synclists\",\"pass\":%s}\n", fails ? "false" : "true");
   return fails != 0;
}
