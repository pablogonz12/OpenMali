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

   /* Overflow: every wrapped product must be rejected, never returned as success with a zero/tiny size. */
   (void)L(13, MALI_AFBC_SB_16x16, 32, UINT32_MAX, 16, 1, &rc); CHECK(rc == -1);                       /* aligned width = 2^32 */
   (void)L(13, MALI_AFBC_SB_16x16 | MALI_AFBC_MOD_TILED, 32, 1u << 29, 16, 1, &rc); CHECK(rc == -1);   /* tiled: header row stride overflows */
   (void)L(13, MALI_AFBC_SB_16x16, 32, 1u << 20, 1u << 20, 1, &rc); CHECK(rc == -1);                   /* 65536*65536 blocks wrap to 0 */
   (void)L(13, MALI_AFBC_SB_16x16, 32, 65536, 65536, 1u << 30, &rc); CHECK(rc == -1);                  /* slice_stride * depth wraps to 2^58 */
   (void)L(13, MALI_AFBC_SB_16x16, 32, 65536, 16, 1, &rc); CHECK(rc == 0);                              /* large but valid still passes */

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

   /* Stage 1: eligibility. Base: 1280x720 RGBA8_UNORM optimal 2D, colour attachment|sampled, v13, flag on. */
   struct mali_afbc_image_info e = {13, 0, 1, 1, 0x10 | 0x4 | 0x1 | 0x2, 0, 37, 1280, 720, true, false, false};
   unsigned bpp = 0;
   EQ(mali_afbc_image_eligible(&e, &bpp), MALI_AFBC_OK); EQ(bpp, 32);
   struct mali_afbc_image_info e2;
   e2 = e; e2.render_only = true; e2.usage = 0x10; EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_OK);   /* colour attachment only */
   for (unsigned u = 0; u < 4; u++) { e2.usage = 0x10 | (u == 0 ? 1u : u == 1 ? 2u : u == 2 ? 4u : 0x80u); EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_USAGE); }
   e2.render_only = false; EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_OK);   /* default stage-1 behaviour unchanged */
   e2 = e; e2.afbc_env = false; EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_DISABLED);
   e2 = e; e2.arch = 6;         EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_ARCH);
   e2 = e; e2.tiling = 1;       EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_TILING);
   e2 = e; e2.image_type = 2;   EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_TYPE);
   e2 = e; e2.samples = 4;      EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_SAMPLES);
   e2 = e; e2.usage |= MALI_AFBC_USAGE_STORAGE;       EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_USAGE);
   e2 = e; e2.usage |= MALI_AFBC_USAGE_HOST_TRANSFER; EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_USAGE);
   e2 = e; e2.flags = MALI_AFBC_FLAG_DISJOINT;        EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_FLAGS);
   e2 = e; e2.flags = MALI_AFBC_FLAG_ALIAS;           EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_FLAGS);
   e2 = e; e2.flags = MALI_AFBC_FLAG_SPARSE_BINDING;  EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_FLAGS);
   e2 = e; e2.flags = 0x80 /* MUTABLE_FORMAT */;      EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_OK); /* allowed here; same-mode check is stage 5 */
   e2 = e; e2.width = 8;        EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_EXTENT);
   e2 = e; e2.height = 15;      EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_EXTENT);
   /* Formats: hits, then float / integer / depth / compressed / undefined / multiplanar misses. */
   static const struct { unsigned f, bpp; } hit[] = {{4,16},{9,8},{16,16},{37,32},{43,32},{44,32},{50,32},{58,32},{64,32},{70,16}};
   for (unsigned k = 0; k < sizeof hit / sizeof hit[0]; k++) {
      e2 = e; e2.format = hit[k].f; bpp = 0;
      EQ(mali_afbc_image_eligible(&e2, &bpp), MALI_AFBC_OK); EQ(bpp, hit[k].bpp);
   }
   static const unsigned miss[] = {0, 41 /*R8G8B8A8_UINT*/, 109 /*R16G16B16A16_SFLOAT*/, 100 /*R32_SFLOAT*/, 126 /*D32_SFLOAT*/,
                                   129 /*D24S8*/, 131 /*BC1*/, 1000156000 /*G8B8G8R8 multiplanar*/};
   for (unsigned k = 0; k < sizeof miss / sizeof miss[0]; k++) {
      e2 = e; e2.format = miss[k]; EQ(mali_afbc_image_eligible(&e2, NULL), MALI_AFBC_REJ_FORMAT);
   }
   /* An eligible image's layout is computable with the reported bpp. */
   a = L(13, MALI_AFBC_SB_16x16, bpp ? bpp : 32, e.width, e.height, 1, &rc); EQ(rc, 0);


   /* Stage 2: whole-image layout. 64x64 RGBA8 16x16 v13, 3 levels (64,32,16): per level 16B*blocks header (128-aligned) + 1KiB*blocks.
    * L0: 4x4=16 sb: hdr 256 B, body 16384 -> 16640. L1: 2x2=4 sb: hdr 64->128, body 4096 -> 4224. L2: 1 sb: hdr 16->128, body 1024 -> 1152. */
   struct mali_afbc_image_layout il;
   EQ(mali_afbc_image_layout(13, MALI_AFBC_SB_16x16, 32, 64, 64, 3, 1, &il), 0);
   EQ(il.level[0].size, 256 + 16384); EQ(il.level[1].size, 128 + 4096); EQ(il.level[2].size, 128 + 1024);
   EQ(il.level_offset[0], 0); EQ(il.level_offset[1], 16640); EQ(il.level_offset[2], 16640 + 4224);
   EQ(il.base_align, 128); EQ(il.array_stride, 16640 + 4224 + 1152); EQ(il.size, il.array_stride);
   /* Layers repeat at array_stride. */
   EQ(mali_afbc_image_layout(13, MALI_AFBC_SB_16x16, 32, 64, 64, 3, 5, &il), 0); EQ(il.size, 5 * il.array_stride);
   /* Level 1 of a 1x1 chain clamps to 1 pixel -> 1 superblock. Tiled: 4096 alignment propagates to base and array stride. */
   EQ(mali_afbc_image_layout(13, MALI_AFBC_SB_16x16 | MALI_AFBC_MOD_TILED, 32, 128, 128, 2, 2, &il), 0);
   EQ(il.base_align, 4096); CHECK(il.level_offset[1] % 4096 == 0); EQ(il.array_stride % 4096, 0);
   EQ(il.level[1].aligned_w, 128); /* 64 px aligns up to the 128 tile */
   /* Invalid: zero/too many levels or layers, bad modifier, overflow. */
   CHECK(mali_afbc_image_layout(13, MALI_AFBC_SB_16x16, 32, 64, 64, 0, 1, &il) == -1);
   CHECK(mali_afbc_image_layout(13, MALI_AFBC_SB_16x16, 32, 64, 64, 17, 1, &il) == -1);
   CHECK(mali_afbc_image_layout(13, MALI_AFBC_SB_16x16, 32, 64, 64, 1, 0, &il) == -1);
   CHECK(mali_afbc_image_layout(13, 0, 32, 64, 64, 1, 1, &il) == -1);
   CHECK(mali_afbc_image_layout(13, MALI_AFBC_SB_16x16, 32, 65536, 65536, 1, 1u << 30, &il) == -1);
   /* Single level matches the plain layout. */
   EQ(mali_afbc_image_layout(13, MALI_AFBC_SB_16x16, 32, 1280, 720, 1, 1, &il), 0);
   a = L(13, MALI_AFBC_SB_16x16, 32, 1280, 720, 1, &rc); EQ(il.size, a.size);
   /* Stage 3: RT descriptor words, hand-derived from v13.xml "AFBC RGB Render Target" bit positions. */
   {
      struct mali_afbc_rt_words w; uint32_t rt[16];
      a = L(13, MALI_AFBC_SB_16x16, 32, 1280, 720, 1, &rc);   /* row stride 1280 B, 3600 sb -> header 57600 -> body_offset 57600 (128-aligned) */
      EQ(mali_afbc_rt_words(MALI_AFBC_SB_16x16, 37, 256, 0x123456789000ull, &a, &w), 0);
      EQ(w.w1_block_format, 12); EQ(w.w2, 10u << 10); /* R8G8B8A8, no YTR/split/wide, tile 16x16 = SB: no reverse issue */
      EQ(w.row_stride, 1280); EQ(w.body_offset, 57600);
      for (int i = 0; i < 16; i++) rt[i] = 0xa5a5a5a5u;
      rt[1] = 0x80000001u | (2u << 8);   /* write enable + clean tile + linear block format 2 */
      mali_afbc_rt_patch(rt, &w);
      EQ(rt[1], 0x80000001u | (12u << 8)); EQ(rt[2], 10u << 10); EQ(rt[8], 0x56789000u); EQ(rt[9], 0x1234u);
      EQ(rt[10], 1280); EQ(rt[11], 57600); EQ(rt[0], 0xa5a5a5a5u); EQ(rt[12], 0xa5a5a5a5u); EQ(rt[3], 0xa5a5a5a5u);
      /* Tile smaller than the superblock (128 px = 16x8, 64 px = 8x8) needs reverse issue order (bit 3). */
      EQ(mali_afbc_rt_words(MALI_AFBC_SB_16x16, 37, 128, 0, &a, &w), 0); EQ(w.w2, (10u << 10) | 8);
      EQ(mali_afbc_rt_words(MALI_AFBC_SB_16x16, 37, 64, 0, &a, &w), 0); EQ(w.w2, (10u << 10) | 8);
      /* Wide (32x8) block: wide bit; tile 16x16 narrower than 32 -> reverse too. Split + YTR + tiled. */
      a = L(13, MALI_AFBC_SB_32x8 | MALI_AFBC_MOD_SPLIT | MALI_AFBC_MOD_YTR | MALI_AFBC_MOD_TILED, 32, 256, 256, 1, &rc);
      EQ(mali_afbc_rt_words(MALI_AFBC_SB_32x8 | MALI_AFBC_MOD_SPLIT | MALI_AFBC_MOD_YTR | MALI_AFBC_MOD_TILED, 44, 256, 0, &a, &w), 0);
      EQ(w.w1_block_format, 13); EQ(w.w2, (10u << 10) | 1 | 2 | 4 | 8);
      /* Other compression modes and rejects. */
      a = L(13, MALI_AFBC_SB_16x16, 16, 64, 64, 1, &rc);
      EQ(mali_afbc_rt_words(MALI_AFBC_SB_16x16, 4, 256, 0, &a, &w), 0); EQ(w.w2 >> 10, 4);
      EQ(mali_afbc_rt_words(MALI_AFBC_SB_16x16, 64, 256, 0, &a, &w), 0); EQ(w.w2 >> 10, 11);
      CHECK(mali_afbc_rt_words(MALI_AFBC_SB_16x16, 109, 256, 0, &a, &w) == -1);   /* float */
      CHECK(mali_afbc_rt_words(0, 37, 256, 0, &a, &w) == -1);                    /* no superblock */
      uint32_t tw, th;
      mali_afbc_tile_block(256, &tw, &th); EQ(tw, 16); EQ(th, 16);
      mali_afbc_tile_block(128, &tw, &th); EQ(tw, 16); EQ(th, 8);
      mali_afbc_tile_block(32, &tw, &th); EQ(tw, 8); EQ(th, 4);
   }


   /* Stage 7: AFBC texture plane words. Hand-derived: w0 = Plane(10) | AFBC(6)<<4 | sb 1<<8 | prefetch 1<<20 | cm(RGBA8=10)<<26 = 0x2810016a. */
   {
      struct mali_afbc_layout lv = {0};
      lv.header_row_stride = 0x1400; lv.header_size = 0xe100; lv.size = 0x123456780ull;
      uint32_t o[8];
      const uint32_t v13[8] = { 0x2810016a, 0x23456780, 0x12345680, 0x0001abcd, 0x1400, 0xe100, 0, 0x02cf04ff };
      EQ(mali_afbc_tex_plane_words(13, MALI_AFBC_SB_16x16, 37, 0xabcd12345680ull, &lv, 1280, 720, o), 0);
      for (int i = 0; i < 8; i++) EQ(o[i], v13[i]);
      EQ(mali_afbc_tex_plane_words(12, MALI_AFBC_SB_16x16, 37, 0xabcd12345680ull, &lv, 1280, 720, o), 0); EQ(o[3], 0x0001abcd);
      /* v10/v11: 64-bit pointer (word 3 = pointer hi, no size hi), size must fit 32 bits */
      CHECK(mali_afbc_tex_plane_words(11, MALI_AFBC_SB_16x16, 37, 0xabcd12345680ull, &lv, 1280, 720, o) == -1);
      lv.size = 0x23456780u;
      for (unsigned a = 10; a <= 11; a++) {
         EQ(mali_afbc_tex_plane_words(a, MALI_AFBC_SB_16x16, 37, 0xffffabcd12345680ull, &lv, 1280, 720, o), 0);
         EQ(o[0], 0x2810016a); EQ(o[1], 0x23456780); EQ(o[2], 0x12345680); EQ(o[3], 0xffffabcdu); EQ(o[4], 0x1400); EQ(o[5], 0xe100); EQ(o[6], 0); EQ(o[7], 0x02cf04ff);
      }
      CHECK(mali_afbc_tex_plane_words(13, MALI_AFBC_SB_16x16, 37, 0xffffabcd12345680ull, &lv, 1280, 720, o) == -1);   /* beyond 48 bits */
      lv.header_row_stride = 1u << 27;
      CHECK(mali_afbc_tex_plane_words(13, MALI_AFBC_SB_16x16, 37, 0x1000, &lv, 16, 16, o) == -1);   /* 27-bit row stride */
      EQ(mali_afbc_tex_plane_words(11, MALI_AFBC_SB_16x16, 37, 0x1000, &lv, 16, 16, o), 0); EQ(o[4], 1u << 27);
      lv.header_row_stride = 0x1400;
      /* modifier bits and compression mode: 32x8 (2), YTR 1<<16, split 1<<17, tiled 1<<19; R5G6B5 mode 4 */
      EQ(mali_afbc_tex_plane_words(13, MALI_AFBC_SB_32x8 | MALI_AFBC_MOD_YTR | MALI_AFBC_MOD_SPLIT | MALI_AFBC_MOD_TILED, 37, 0x1000, &lv, 16, 16, o), 0);
      EQ(o[0], 0x281b026a);
      EQ(mali_afbc_tex_plane_words(13, MALI_AFBC_SB_16x16, 4, 0x1000, &lv, 16, 16, o), 0); EQ(o[0], 0x1010016a);
      /* unsupported: arch 7..9, float, depth, no superblock, empty extent */
      for (unsigned a = 7; a <= 9; a++) CHECK(mali_afbc_tex_plane_words(a, MALI_AFBC_SB_16x16, 37, 0x1000, &lv, 16, 16, o) == -1);
      CHECK(mali_afbc_tex_plane_words(13, MALI_AFBC_SB_16x16, 109, 0x1000, &lv, 16, 16, o) == -1);
      CHECK(mali_afbc_tex_plane_words(13, MALI_AFBC_SB_16x16, 126, 0x1000, &lv, 16, 16, o) == -1);
      CHECK(mali_afbc_tex_plane_words(13, 0, 37, 0x1000, &lv, 16, 16, o) == -1);
      CHECK(mali_afbc_tex_plane_words(13, MALI_AFBC_SB_16x16, 37, 0x1000, &lv, 0, 16, o) == -1);
      CHECK(!mali_afbc_tex_supported(9, 37) && mali_afbc_tex_supported(10, 37) && mali_afbc_tex_supported(13, 37) && !mali_afbc_tex_supported(14, 37));
      /* eligibility: SAMPLED allowed only with sample_ok on arch 10..13; transfers / input attachment never; cube / mutable not with SAMPLED */
      struct mali_afbc_image_info s = {13, 0, 1, 1, 0x10 | 0x4, 0, 37, 1280, 720, true, true, true};
      EQ(mali_afbc_image_eligible(&s, NULL), MALI_AFBC_OK);
      s.sample_ok = false; EQ(mali_afbc_image_eligible(&s, NULL), MALI_AFBC_REJ_USAGE); s.sample_ok = true;
      s.arch = 9; EQ(mali_afbc_image_eligible(&s, NULL), MALI_AFBC_REJ_USAGE); s.arch = 14; EQ(mali_afbc_image_eligible(&s, NULL), MALI_AFBC_REJ_USAGE);
      s.arch = 10; EQ(mali_afbc_image_eligible(&s, NULL), MALI_AFBC_OK); s.arch = 13;
      static const unsigned nu[] = {0x1, 0x2, 0x80};
      for (unsigned k = 0; k < 3; k++) { s.usage = 0x10 | 0x4 | nu[k]; EQ(mali_afbc_image_eligible(&s, NULL), MALI_AFBC_REJ_USAGE); }
      s.usage = 0x10 | 0x4; s.flags = MALI_AFBC_FLAG_CUBE;    EQ(mali_afbc_image_eligible(&s, NULL), MALI_AFBC_REJ_FLAGS);
      s.flags = MALI_AFBC_FLAG_MUTABLE;                       EQ(mali_afbc_image_eligible(&s, NULL), MALI_AFBC_REJ_FLAGS);
      s.usage = 0x10; EQ(mali_afbc_image_eligible(&s, NULL), MALI_AFBC_OK);   /* render-only keeps mutable/cube */
   }

   printf(fails ? "t_afbc: %d FAILED\n" : "t_afbc: all ok\n", fails);
   return fails != 0;
}
