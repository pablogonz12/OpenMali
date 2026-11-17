/* SPDX-License-Identifier: MIT */
/* R4-M7: deterministic BC test vectors shared by t_bc.c (offline, generates the golden) and t_bc_hw.c (device). Header only, plain C + <vulkan/vulkan_core.h>. */
#ifndef T_BC_VEC_H
#define T_BC_VEC_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* The device vector image: 2D, 20x12, 2 levels (20x12 = 5x3 blocks, 10x6 = 3x2 blocks), 1 layer. Blocks: level 0 raster then level 1 raster. */
#define BCV_W 20u
#define BCV_H 12u
#define BCV_LEVELS 2u
#define BCV_NBLOCKS (5u * 3u + 3u * 2u)

static inline uint64_t bcv_rng(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }

/* Fill n blocks. wild = 1: ~1/16 blocks are left fully random (reserved BC6H/BC7 modes), else every block carries a valid mode, cycling through all modes. */
static inline void bcv_fill(VkFormat f, uint64_t seed, uint8_t *b, size_t n, unsigned bs, int wild)
{
   static const uint8_t bc6[14] = { 0x00, 0x01, 0x02, 0x06, 0x0a, 0x0e, 0x12, 0x16, 0x1a, 0x1e, 0x03, 0x07, 0x0b, 0x0f };
   uint64_t s = seed * 0x9e3779b97f4a7c15ull + 1;
   for (size_t k = 0; k < n; k++) {
      uint8_t *p = b + k * bs;
      for (unsigned i = 0; i < bs; i++) p[i] = (uint8_t)(bcv_rng(&s) >> 24);
      int raw = wild && (bcv_rng(&s) & 15) == 0;
      if (raw) continue;
      if (f == VK_FORMAT_BC7_UNORM_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK) {
         unsigned m = (unsigned)(k % 8);
         p[0] = (uint8_t)((p[0] & ~((2u << m) - 1u)) | (1u << m));
      } else if (f == VK_FORMAT_BC6H_UFLOAT_BLOCK || f == VK_FORMAT_BC6H_SFLOAT_BLOCK) {
         uint8_t c = bc6[k % 14];
         unsigned nb = c < 2 ? 2 : 5;
         p[0] = (uint8_t)((p[0] & ~((1u << nb) - 1u)) | c);
      }
   }
}

static inline uint64_t bcv_fnv(const void *d, size_t n, uint64_t h)
{
   const uint8_t *p = d;
   for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
   return h;
}
#define BCV_FNV0 0xcbf29ce484222325ull
#endif
