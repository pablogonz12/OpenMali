/* SPDX-License-Identifier: MIT */
/* Host test for mali_afbc.h layout maths. Expected values are hand-derived from pan_afbc.h / pan_mod.c formulas
 * (independent arithmetic, not a call into the same code). Build: tests/build-t_afbc.sh */
#include "../src/mali_afbc.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define EQ(a, b) do { unsigned long long _a = (a), _b = (b); if (_a != _b) { printf("FAIL %s:%d %s=%llu want %llu\n", __FILE__, __LINE__, #a, _a, _b); fails++; } } while (0)

static struct mali_afbc_layout L(unsigned arch, uint64_t mod, unsigned bpp, uint32_t w, uint32_t h, uint32_t d, int *rc)
{
   struct mali_afbc_params p = {arch, mod, bpp, w, h, d};
   struct mali_afbc_layout o;
   *rc = mali_afbc_layout(&p, &o);
   return o;
}

int main(void)
{
   int rc;
   /* 1280x720 RGBA8, 16x16, v13: 80 x 45 superblocks. header row = 80*16 = 1280 B (16-aligned). */
   struct mali_afbc_layout a = L(13, MALI_AFBC_SB_16x16, 32, 1280, 720, 1, &rc);
   EQ(rc, 0); EQ(a.aligned_w, 1280); EQ(a.aligned_h, 720);
   EQ(a.header_row_stride, 1280); EQ(a.stride_blocks, 80); EQ(a.nblocks, 80 * 45);
   EQ(a.header_size, 3600 * 16);          /* 57600 */
   EQ(a.offset_align, 128); EQ(a.body_offset, 57600);  /* 57600 = 450*128 */
   EQ(a.payload_per_sb, 1024); EQ(a.slice_stride, 57600 + 3600ull * 1024); EQ(a.size, a.slice_stride);

   /* Non-multiple extent: 1366x768 pads width to 1376 (86 sb), height 768 = 48 sb. */
   a = L(13, MALI_AFBC_SB_16x16, 32, 1366, 768, 1, &rc);
   EQ(a.aligned_w, 1376); EQ(a.stride_blocks, 86); EQ(a.nblocks, 86 * 48); EQ(a.header_size, 86 * 48 * 16);
   EQ(a.body_offset, mali_afbc_align(66048, 128)); /* 66048 = 516*128 already aligned */
   EQ(a.body_offset, 66048);

   /* Tiny image 1x1: one superblock; body offset rounds header 16 -> 128. */
   a = L(13, MALI_AFBC_SB_16x16, 32, 1, 1, 1, &rc);
   EQ(a.nblocks, 1); EQ(a.header_size, 16); EQ(a.body_offset, 128); EQ(a.slice_stride, 128 + 1024);

   /* 32x8 wide block: render height rounds 8 -> 16, so a 100x20 image aligns to 128x32; sb rows = 32/8 = 4. */
   a = L(13, MALI_AFBC_SB_32x8, 32, 100, 20, 1, &rc);
   EQ(rc, 0); EQ(a.aligned_w, 128); EQ(a.aligned_h, 32); EQ(a.stride_blocks, 4); EQ(a.nblocks, 16);
   EQ(a.payload_per_sb, 32 * 8 * 4);
   /* 64x4: height 4 -> 16; 70x5 -> 128x16, sb rows 4, cols 2. */
   a = L(13, MALI_AFBC_SB_64x4, 32, 70, 5, 1, &rc);
   EQ(a.aligned_w, 128); EQ(a.aligned_h, 16); EQ(a.stride_blocks, 2); EQ(a.nblocks, 8); EQ(a.payload_per_sb, 64 * 4 * 4);

   /* Tiled headers (8x8 sb tiles for <=32bpp): 16x16 sb => align 128x128. 1280x720 -> 1280x768.
    * row stride = 80*8*16 = 10240 (1024-aligned: yes, 10*1024). stride_blocks = 10240/(16*8) = 80.
    * nblocks = 80 * (768/16) = 3840; header 61440; 4096-aligned body = 61440 (15*4096). */
   a = L(13, MALI_AFBC_SB_16x16 | MALI_AFBC_MOD_TILED, 32, 1280, 720, 1, &rc);
   EQ(rc, 0); EQ(a.tile, 8); EQ(a.aligned_w, 1280); EQ(a.aligned_h, 768);
   EQ(a.header_row_stride, 10240); EQ(a.stride_blocks, 80); EQ(a.nblocks, 3840);
   EQ(a.header_size, 61440); EQ(a.offset_align, 4096); EQ(a.body_offset, 61440);
   /* Tiled, narrow image 100x100: align 128 -> 128x128, 8 sb cols; row stride 8*8*16=1024 (1024-aligned). */
   a = L(13, MALI_AFBC_SB_16x16 | MALI_AFBC_MOD_TILED, 32, 100, 100, 1, &rc);
   EQ(a.aligned_w, 128); EQ(a.aligned_h, 128); EQ(a.header_row_stride, 1024); EQ(a.nblocks, 64);
   EQ(a.body_offset, 4096);
   /* 64bpp uses 4x4 tiles: 100x100 -> align 64 -> 128x128 (16x16 sb*4=64, 100->128), cols 8, stride 8*4*16=512 -> aligned 256 ok. */
   a = L(13, MALI_AFBC_SB_16x16 | MALI_AFBC_MOD_TILED, 64, 100, 100, 1, &rc);
   EQ(a.tile, 4); EQ(a.aligned_w, 128); EQ(a.header_row_stride, 512); EQ(a.payload_per_sb, 2048);

   /* Row stride alignment bump on tiled v13: 16x16 sb, tiled, width 128 -> 8 cols*8*16 = 1024 ok; width 192 -> 12*128=1536 -> 2048. */
   a = L(13, MALI_AFBC_SB_16x16 | MALI_AFBC_MOD_TILED, 32, 192, 128, 1, &rc);
   EQ(a.aligned_w, 256); /* tile align 128 pushes 192 -> 256, so stride = 16*8*16 = 2048 */
   EQ(a.header_row_stride, 2048);

   /* Depth slices multiply size. */
   a = L(13, MALI_AFBC_SB_16x16, 32, 16, 16, 3, &rc);
   EQ(a.size, 3 * a.slice_stride); EQ(a.slice_stride, 128 + 1024);

   /* arch gating and invalid inputs. */
   L(6, MALI_AFBC_SB_16x16 | MALI_AFBC_MOD_TILED, 32, 64, 64, 1, &rc); CHECK(rc == -1);
   L(5, MALI_AFBC_SB_16x16 | MALI_AFBC_MOD_SPLIT, 32, 64, 64, 1, &rc); CHECK(rc == -1);
   L(13, 0, 32, 64, 64, 1, &rc); CHECK(rc == -1);              /* no superblock size */
   L(13, 7, 32, 64, 64, 1, &rc); CHECK(rc == -1);
   L(13, MALI_AFBC_SB_16x16, 32, 0, 64, 1, &rc); CHECK(rc == -1);
   L(13, MALI_AFBC_SB_16x16, 12, 64, 64, 1, &rc); CHECK(rc == -1);
   a = L(6, MALI_AFBC_SB_16x16, 32, 64, 64, 1, &rc); EQ(rc, 0); EQ(a.offset_align, 128);
   a = L(5, MALI_AFBC_SB_16x16, 32, 64, 64, 1, &rc); EQ(rc, 0); EQ(a.offset_align, 64);

   /* Env flag defaults off, only exactly "1" enables. */
   unsetenv("MALI_AFBC"); CHECK(!mali_afbc_enabled());
   setenv("MALI_AFBC", "1", 1); CHECK(mali_afbc_enabled());
   setenv("MALI_AFBC", "10", 1); CHECK(!mali_afbc_enabled());
   setenv("MALI_AFBC", "0", 1); CHECK(!mali_afbc_enabled());
   unsetenv("MALI_AFBC");

   /* Bandwidth estimate: 720p RGBA8 at 50% payload ratio. */
   a = L(13, MALI_AFBC_SB_16x16, 32, 1280, 720, 1, &rc);
   double full = 1280.0 * 720 * 4, afbc = mali_afbc_bw_estimate(0.5, &a);
   CHECK(afbc < 0.55 * full && afbc > 0.5 * full);   /* headers add ~1.6% over 50% */

   printf(fails ? "t_afbc: %d FAILED\n" : "t_afbc: all ok\n", fails);
   return fails != 0;
}
