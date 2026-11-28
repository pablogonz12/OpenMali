/* SPDX-License-Identifier: MIT */
/* Host test for the colour loadOp LOAD pre-frame shader (vk/src/mali_render.c mali_preload_mode / mali_render_pack_fbd_pf):
 *   (a) a partial-area LOAD/STORE colour pass (1363x27 inside 1920x128) picks INTERSECT, and its FBD has Pre Frame 0 = 2,
 *       the Frame Shader DCD pointer and Clean Tile Write off on the RT;
 *   (b) every mandatory-ALWAYS case (any bound Z/S, Z/S LOAD, Z or S CLEAR = clean tile write, in-tile resolve, MSAA, a different load
 *       source, no geometry) picks its ALWAYS reason, so the frame keeps the preload draw and the old FBD;
 *   (c) pf_dcds_va = 0 packs byte for byte what mali_render_pack_fbd packs (the switch-off stream). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mali_cs_emit.h"
#include "kbase_fb.h"

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); } \
                            else { printf("ok    "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void
base(struct mali_frame *f)
{
   memset(f, 0, sizeof *f);
   f->width = 1920; f->height = 128;
   f->area[0] = 16; f->area[1] = 50; f->area[2] = 1363; f->area[3] = 27;   /* the host Chromium shape */
   f->has_colour = true;
   f->samples = 1;
   f->colour.va = 0x5f00010000ull;
   f->colour.row_stride = 1920 * 4;
   f->colour.store = true;
   f->pf_cand = true;
   f->pf_src_va = f->colour.va;
   f->ndraws = 2;   /* the preload + one draw */
}

static void
add_zs(struct mali_frame *f, bool clear_z, bool clear_s)
{
   f->has_zs = true;
   f->zs.zs_format = KB_ZS_D32F;
   f->zs.va = 0x5f00400000ull;
   f->zs.row_stride = 1920 * 4;
   f->zs.store = true;
   f->zs.clear = clear_z;
   f->has_st = true;
   f->st.va = 0x5f00800000ull;
   f->st.row_stride = 1920;
   f->st.store = true;
   f->st.clear = clear_s;
}

static uint32_t
word(const uint8_t *d, unsigned i)
{
   uint32_t w;
   memcpy(&w, d + 4 * i, 4);
   return w;
}

int main(void)
{
   static const unsigned archs[] = { 10, 11, 13 };
   const uint64_t dcds = 0x5f00020040ull;
   uint8_t a[KB_FB_DESC_SIZE_ZS], b[KB_FB_DESC_SIZE_ZS];
   struct mali_frame f;
   unsetenv("MALI_BBOX32");

   for (unsigned ai = 0; ai < 3; ai++) {
      struct mali_env env = {.arch = archs[ai], .tile_log2 = archs[ai] == 13 ? 12 : 10};
      const unsigned arch = archs[ai];

      /* (a) partial-area colour LOAD/STORE: INTERSECT, Pre Frame 0 = 2, DCD pointer, Clean Tile Write off */
      base(&f);
      EXPECT(mali_preload_mode(&f) == MALI_PF_INTERSECT, "v%u (a) 1363x27 in 1920x128 colour LOAD/STORE -> INTERSECT", arch);
      memset(a, 0, sizeof a); memset(b, 0, sizeof b);
      EXPECT(!mali_render_pack_fbd_pf(&env, &f, a, 0x5f00030000ull, 0x5f00031000ull, dcds), "v%u (a) pack", arch);
      EXPECT((word(a, 0) & 7) == 2 && (word(a, 0) & 0x1f8) == 0, "v%u (a) FBD Pre Frame 0 = INTERSECT (2), Pre Frame 1 / Post = never (word0 %#x)", arch, word(a, 0));
      EXPECT(word(a, 6) == (uint32_t)dcds && word(a, 7) == (uint32_t)(dcds >> 32), "v%u (a) FBD Frame Shader DCDs = the array", arch);
      EXPECT(!(word(a, 32 + 1) >> 31), "v%u (a) RT Clean Tile Write Enable off (RT word1 %#x)", arch, word(a, 33));
      EXPECT(!mali_render_pack_fbd(&env, &f, b, 0x5f00030000ull, 0x5f00031000ull), "v%u (a) pack old", arch);
      EXPECT((word(b, 0) & 0x1ff) == 0 && word(b, 6) == 0 && (word(b, 33) >> 31), "v%u (a) the old FBD: no frame shader, Clean Tile Write on", arch);
      EXPECT(word(a, 9) == word(b, 9) && word(a, 10) == word(b, 10), "v%u (a) same tile-aligned bound box (%#x..%#x)", arch, word(a, 9), word(a, 10));
      /* Independent absolute oracle: a matching shrink in both packers must not pass. */
      {
         uint32_t bounds[4];
         mali_fbd_bounds(&env, &f, bounds);
         EXPECT(bounds[0] == 0 && bounds[1] == (arch == 13 ? 0u : 32u) &&
                bounds[2] == 1407 && bounds[3] == (arch == 13 ? 127u : 95u),
                "v%u (a) bounds contain the full tile-rounded render area: %u,%u..%u,%u",
                arch, bounds[0], bounds[1], bounds[2], bounds[3]);
         EXPECT(word(a, 9) == ((arch == 13 ? 0u : 32u) << 16) &&
                word(a, 10) == (1407u | ((arch == 13 ? 127u : 95u) << 16)),
                "v%u (a) emitted INTERSECT bounds match absolute oracle", arch);
      }
      for (unsigned i = 0; i < KB_FB_DESC_SIZE / 4; i++)
         if (i != 0 && i != 6 && i != 7 && i != 33 && word(a, i) != word(b, i)) {
            EXPECT(0, "v%u (a) FBD word %u differs only where the frame shader / clean tile live (%#x vs %#x)", arch, i, word(a, i), word(b, i));
            break;
         }

      /* Each attachment independently forces fallback, regardless of STORE. */
      for (unsigned mask = 1; mask <= 3; mask++) {
         for (unsigned store = 0; store <= 1; store++) {
            base(&f); add_zs(&f, false, false);
            f.has_zs = !!(mask & 1); f.has_st = !!(mask & 2);
            f.zs.store = store; f.st.store = store;
            EXPECT(mali_preload_mode(&f) == MALI_PF_ALWAYS_ZS_ATTACHMENT,
                   "v%u Z/S mask %u DONT_CARE store %u -> ALWAYS", arch, mask, store);
         }
      }

      /* (b) mandatory ALWAYS: each case keeps the preload draw */
      base(&f); f.pf_zs_load = true; add_zs(&f, false, false);
      EXPECT(mali_preload_mode(&f) == MALI_PF_ALWAYS_ZS_LOAD, "v%u (b) colour + depth/stencil LOAD -> ALWAYS (zs_load)", arch);
      base(&f); add_zs(&f, true, false);
      EXPECT(mali_preload_mode(&f) == MALI_PF_ALWAYS_CLEAN_TILE, "v%u (b) depth CLEAR (clean tile write, TSIX-2033) -> ALWAYS", arch);
      base(&f); add_zs(&f, false, true);
      EXPECT(mali_preload_mode(&f) == MALI_PF_ALWAYS_CLEAN_TILE, "v%u (b) stencil CLEAR -> ALWAYS", arch);
      base(&f); f.samples = 4; f.colour.resolve_va = 0x5f00c00000ull;
      EXPECT(mali_preload_mode(&f) == MALI_PF_ALWAYS_RESOLVE, "v%u (b) in-tile MSAA resolve -> ALWAYS", arch);
      base(&f); f.samples = 4;
      EXPECT(mali_preload_mode(&f) == MALI_PF_ALWAYS_MSAA, "v%u (b) 4x colour LOAD -> ALWAYS", arch);
      base(&f); f.pf_src_va = 0x5f00d00000ull;
      EXPECT(mali_preload_mode(&f) == MALI_PF_ALWAYS_SRC, "v%u (b) load source != store target -> ALWAYS", arch);
      base(&f); f.ndraws = 1;
      EXPECT(mali_preload_mode(&f) == MALI_PF_ALWAYS_EMPTY, "v%u (b) no geometry besides the preload -> ALWAYS", arch);

      /* (c) no candidate: NONE; pf 0 = the old packer byte for byte */
      base(&f); f.pf_cand = false;
      EXPECT(mali_preload_mode(&f) == MALI_PF_NONE, "v%u (c) no colour preload -> NONE", arch);
      base(&f); f.colour.clear = true;
      EXPECT(mali_preload_mode(&f) == MALI_PF_NONE, "v%u (c) colour CLEAR -> NONE", arch);
      for (int k = 0; k < 3; k++) {
         base(&f);
         if (k == 1) add_zs(&f, true, false);
         if (k == 2) { f.area[0] = 0; f.area[1] = 0; f.area[2] = 1920; f.area[3] = 128; }
         memset(a, 0, sizeof a); memset(b, 0, sizeof b);
         mali_render_pack_fbd_pf(&env, &f, a, 0x5f00030000ull, 0x5f00031000ull, 0);
         mali_render_pack_fbd(&env, &f, b, 0x5f00030000ull, 0x5f00031000ull);
         EXPECT(!memcmp(a, b, sizeof a), "v%u (c) pf_dcds_va 0 == mali_render_pack_fbd (case %d)", arch, k);
      }
   }

   /* switch + counters */
   unsetenv("MALI_PRELOAD_INTERSECT");
   EXPECT(!mali_preload_intersect_on(), "switch off by default");
   setenv("MALI_PRELOAD_INTERSECT", "1", 1);
   EXPECT(mali_preload_intersect_on(), "MALI_PRELOAD_INTERSECT=1 turns it on");
   unsetenv("MALI_PRELOAD_INTERSECT");
   unsigned c0[MALI_PF_NMODES], c1[MALI_PF_NMODES];
   mali_preload_mode_stats(c0);
   mali_preload_mode_note(MALI_PF_INTERSECT);
   mali_preload_mode_note(MALI_PF_ALWAYS_ZS_LOAD);
   mali_preload_mode_note(MALI_PF_NONE);
   mali_preload_mode_stats(c1);
   EXPECT(c1[MALI_PF_INTERSECT] == c0[MALI_PF_INTERSECT] + 1 && c1[MALI_PF_ALWAYS_ZS_LOAD] == c0[MALI_PF_ALWAYS_ZS_LOAD] + 1 &&
          c1[MALI_PF_NONE] == c0[MALI_PF_NONE], "counters: one per noted mode, NONE not counted");

   printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
   return fails ? 1 : 0;
}
