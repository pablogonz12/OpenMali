/* SPDX-License-Identifier: MIT */
/* Host test for mali_fbd_bounds (vk/src/mali_render.c): the FBD Bound Min/Max box is the render area rounded OUTWARD to
 * whole tiles of the real tile size (PanVK pan_fb_bbox_align), not to 32 px and not the whole attachment.
 *   MALI_BBOX32 unset/0: round to the FBD's effective tile (32x32 on v11, 64x64 on v13, 16x16 for 4x MSAA)
 *   MALI_BBOX32=1:        round to 32 px (the pre-audit behaviour, wrong on a 64x64 tile)
 *   MALI_BBOX32=2:        the whole attachment (main before this change) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mali_cs_emit.h"

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); } \
                            else { printf("ok    "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void
box(const struct mali_env *env, const struct mali_frame *f, uint32_t *b)
{
   mali_fbd_bounds(env, f, b);
}

int main(void)
{
   struct mali_env env = {.arch = 11, .tile_log2 = 12};   /* 64x64 by the env; the v11 FBD carries kb_fb_pack's 32x32 */
   struct mali_frame f;
   uint32_t b[4];

   /* 1. full-attachment render area: exactly the attachment, every mode */
   memset(&f, 0, sizeof f);
   f.width = 1280; f.height = 720;
   f.area[0] = 0; f.area[1] = 0; f.area[2] = 1280; f.area[3] = 720;
   box(&env, &f, b);
   EXPECT(b[0] == 0 && b[1] == 0 && b[2] == 1279 && b[3] == 719, "full area: 0,0..1279,719 (got %u,%u..%u,%u)", b[0], b[1], b[2], b[3]);

   /* 2. a small render area in the middle: rounded outward to 32-px tiles on v11 (arch 11, tile_log2 not 64x64) */
   f.area[0] = 100; f.area[1] = 60; f.area[2] = 200; f.area[3] = 100;
   box(&env, &f, b);
   EXPECT(b[0] == 96 && b[1] == 32 && b[2] == 319 && b[3] == 159, "v11 32x32 tile: 96,32..319,159 (got %u,%u..%u,%u)", b[0], b[1], b[2], b[3]);
   EXPECT(b[2] - b[0] + 1 <= 3 * 32 + 200, "v11: the box grew by less than one tile per edge");

   /* 3. the same area on v13's 64x64 tile: the box follows the real tile size (env.tile_log2 = 12) */
   env.arch = 13;
   box(&env, &f, b);
   EXPECT(b[0] == 64 && b[1] == 0 && b[2] == 319 && b[3] == 191, "v13 64x64 tile: 64,0..319,191 (got %u,%u..%u,%u)", b[0], b[1], b[2], b[3]);

   /* 4. MALI_BBOX32=1: 32 px, whatever the tile (the pre-audit behaviour) */
   setenv("MALI_BBOX32", "1", 1);
   box(&env, &f, b);
   EXPECT(b[0] == 96 && b[1] == 32 && b[2] == 319 && b[3] == 159, "MALI_BBOX32=1: 32-px box 96,32..319,159 (got %u,%u..%u,%u)", b[0], b[1], b[2], b[3]);
   setenv("MALI_BBOX32", "0", 1);
   box(&env, &f, b);
   EXPECT(b[0] == 64 && b[1] == 0, "MALI_BBOX32=0: back to the real tile (got %u,%u)", b[0], b[1]);

   /* 5. MALI_BBOX32=2: the whole attachment, i.e. main's behaviour before this change */
   setenv("MALI_BBOX32", "2", 1);
   box(&env, &f, b);
   EXPECT(b[0] == 0 && b[1] == 0 && b[2] == 1279 && b[3] == 719, "MALI_BBOX32=2: whole attachment (got %u,%u..%u,%u)", b[0], b[1], b[2], b[3]);
   unsetenv("MALI_BBOX32");

   /* 6. a render area running past the attachment is clipped to it; a negative offset is clamped to 0 */
   f.area[0] = -40; f.area[1] = -8; f.area[2] = 200; f.area[3] = 100;
   box(&env, &f, b);
   EXPECT(b[0] == 0 && b[1] == 0 && b[2] == 191 && b[3] == 127, "negative offset clamped: 0,0..191,127 (got %u,%u..%u,%u)", b[0], b[1], b[2], b[3]);
   f.area[0] = 1200; f.area[1] = 700; f.area[2] = 400; f.area[3] = 400;
   box(&env, &f, b);
   EXPECT(b[2] == 1279 && b[3] == 719, "past the attachment: clipped to 1279,719 (got %u,%u)", b[2], b[3]);

   /* 7. the box always covers the render area (never clips a pixel the pass covers) */
   for (unsigned i = 0; i < 5; i++) {
      f.area[0] = 1 + (int)(i * 37); f.area[1] = 3 + (int)(i * 11); f.area[2] = 97; f.area[3] = 61;
      box(&env, &f, b);
      bool covers = b[0] <= (uint32_t)f.area[0] && b[1] <= (uint32_t)f.area[1] &&
                    b[2] >= (uint32_t)(f.area[0] + f.area[2] - 1) && b[3] >= (uint32_t)(f.area[1] + f.area[3] - 1);
      EXPECT(covers, "box %u,%u..%u,%u covers the render area %d,%d %dx%d", b[0], b[1], b[2], b[3], f.area[0], f.area[1], f.area[2], f.area[3]);
   }

   /* 8. 4x MSAA: v13 declares env.tile_log2 - 2 (32x32), v11 keeps kb_fb_pack's 16x16 */
   memset(&f, 0, sizeof f);
   f.width = 640; f.height = 480; f.samples = 4;
   f.area[0] = 33; f.area[1] = 17; f.area[2] = 100; f.area[3] = 50;
   box(&env, &f, b);
   EXPECT(b[0] == 32 && b[1] == 0 && b[2] == 159 && b[3] == 95, "v13 4x MSAA 32x32 tile: 32,0..159,95 (got %u,%u..%u,%u)", b[0], b[1], b[2], b[3]);
   EXPECT(mali_fbd_tile_log2(&env, &f) == env.tile_log2 - 2, "v13 4x MSAA: the tile is env.tile_log2 - 2");
   env.arch = 11;
   box(&env, &f, b);
   EXPECT(b[0] == 32 && b[1] == 16 && b[2] == 143 && b[3] == 79, "v11 4x MSAA 16x16 tile: 32,16..143,79 (got %u,%u..%u,%u)", b[0], b[1], b[2], b[3]);
   EXPECT(mali_fbd_tile_log2(&env, &f) == 8, "v11 4x MSAA: kb_fb_pack's 16x16 tile");

   printf("{\"test\":\"t_bbox\",\"pass\":%s}\n", fails ? "false" : "true");
   return fails != 0;
}
