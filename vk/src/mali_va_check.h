/* SPDX-License-Identifier: MIT */
#ifndef MALI_VA_CHECK_H
#define MALI_VA_CHECK_H

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Diagnostic only: one report per call site and architecture, including
 * concurrent pipeline compilation. Zero is intentionally not reported: null
 * descriptors are legal. The caller must pass the untagged GPU address. */
#define MALI_VA_CHECK(site, arch, value, base_value, offset, bytes) do { \
   uint64_t mvc_va = (value), mvc_base = (base_value), mvc_off = (offset), mvc_size = (bytes); \
   unsigned mvc_arch = (arch); \
   if (mvc_va && mvc_va < 0x100000 && getenv("MALI_VA_CHECK")) { \
      static atomic_uint_fast64_t mvc_seen; \
      uint64_t mvc_bit = UINT64_C(1) << (mvc_arch < 63 ? mvc_arch : 63); \
      if (!(atomic_fetch_or(&mvc_seen, mvc_bit) & mvc_bit)) \
         fprintf(stderr, "mali_va: %s arch=%u va=0x%" PRIx64 " base=0x%" PRIx64 \
                 " off=0x%" PRIx64 " size=%" PRIu64 "\n", \
                 (site), mvc_arch, mvc_va, mvc_base, mvc_off, mvc_size); \
   } \
} while (0)
#endif
