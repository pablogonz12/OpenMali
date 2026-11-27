/* SPDX-License-Identifier: MIT */
/* Host test for kb_tiler_hmask_from_tile (libkbase/src/draw.c): the tiler hierarchy mask must follow the effective fragment
 * tile the way pan_desc.c GENX(pan_select_tiler_hierarchy_mask) does - every bin level below the tile disabled, the levels
 * from the tile up enabled. The driver used to hardcode 0xfe, which is only right for a 32x32 tile (v11's FBD default). */
#include <stdio.h>

#include "kbase_draw.h"

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); } \
                            else { printf("ok    "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
   /* The table panvk's rule gives for this driver's base mask (bit 0, the 16x16 level, is never enabled here). */
   static const struct { unsigned tile_log2, want; const char *what; } tab[] = {
      {8, 0xfe, "16x16 (4x MSAA on v13) -> same as before"},   /* no level below 16x16 exists */
      {10, 0xfe, "32x32 (the v11 FBD default) -> 0xfe, i.e. today's literal"},
      {12, 0xfc, "64x64 (v13) -> 0xfc: the 16x16 and 32x32 levels are disabled"},
      {14, 0xf8, "128x128 -> 0xf8"},
      {16, 0xf0, "256x256 -> 0xf0"},
   };
   for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++) {
      uint32_t m = kb_tiler_hmask_from_tile(tab[i].tile_log2);
      EXPECT(m == tab[i].want, "tile_log2 %u: mask 0x%02x (want 0x%02x): %s", tab[i].tile_log2, m, tab[i].want, tab[i].what);
   }
   /* Invariants: no level below the tile is enabled, the top level (bit 7) always is, and a bigger tile never enables more. */
   for (unsigned tl = 8; tl <= 18; tl++) {
      uint32_t m = kb_tiler_hmask_from_tile(tl);
      unsigned below = 0;
      while ((256u << (below * 2)) < (1u << tl) && below < 8)
         below++;                       /* number of bin levels smaller than the tile */
      bool ok = (m >> 7) & 1u;            /* the whole-FB level stays on */
      for (unsigned b = 0; b < below; b++)
         ok = ok && !((m >> b) & 1u);
      EXPECT(ok, "tile_log2 %u: mask 0x%02x has no level below the tile and keeps the top level", tl, m);
      if (tl > 8 && (tl & 1) == 0)   /* square power-of-two tiles only: PanVK's levels are (16<<n)^2 */
         EXPECT((m & ~kb_tiler_hmask_from_tile(tl - 2)) == 0, "tile_log2 %u: mask 0x%02x only drops levels the next smaller tile has", tl, m);
   }
   /* 0xfe is what mali_queue.c used to hardcode: the same value for a 32x32 tile, a different one for 64x64. */
   EXPECT(kb_tiler_hmask_from_tile(10) == 0xfe, "a 32x32 tile keeps the old literal 0xfe (CMF, arch 11)");
   EXPECT(kb_tiler_hmask_from_tile(12) != 0xfe, "a 64x64 tile no longer uses 0xfe (would be 2 extra hierarchy levels)");
   printf("{\"test\":\"t_hmask\",\"pass\":%s}\n", fails ? "false" : "true");
   return fails != 0;
}
