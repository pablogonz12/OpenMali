/* SPDX-License-Identifier: MIT */
/* Exercise the fake backend directly: equal-sized BOs in two contexts must
 * not alias when disjoint VA mode is used for ownership regression tests. */
#include "mali_backend.h"
#include <stdio.h>
#include <stdlib.h>

const struct mali_backend_ops mali_backend_kbase = {0};

int main(void)
{
   const struct mali_backend_ops *be = &mali_backend_fake;
   struct mali_gpu_info gpu;
   struct mali_kdev *a, *b;
   struct mali_bo ba, bb;
   if (setenv("MALI_FAKE_DISJOINT_VA", "1", 1) || be->probe(&gpu) ||
       be->device_create(&gpu, &a) || be->device_create(&gpu, &b) ||
       be->bo_alloc(a, 4096, 0, &ba) || be->bo_alloc(b, 4096, 0, &bb))
      return 1;
   if (ba.gpu_va == bb.gpu_va ||
       be->va_cpu(a, ba.gpu_va, 64) != ba.cpu ||
       be->va_cpu(b, bb.gpu_va, 64) != bb.cpu ||
       be->va_cpu(a, bb.gpu_va, 64) || be->va_cpu(b, ba.gpu_va, 64))
      return 2;
   /* MOVE48 r0, immediate: only the other device's VA must be rejected. */
   if (setenv("MALI_FAKE_CHECK_CS_VA", "1", 1))
      return 4;
   uint64_t local = (1ull << 56) | ba.gpu_va;
   uint64_t foreign = (1ull << 56) | (bb.gpu_va + 64);
   if (be->submit_cs(a, &local, 1) || !be->submit_cs(a, &foreign, 1))
      return 5;
   be->bo_free(a, &ba);
   be->device_destroy(a);
   if (be->va_cpu(b, bb.gpu_va, 64) != bb.cpu)
      return 3;
   be->bo_free(b, &bb);
   be->device_destroy(b);
   puts("fake context VA isolation OK");
   return 0;
}
