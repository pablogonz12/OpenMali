/* SPDX-License-Identifier: MIT */
/* Stage 4 host test: AFBC RT emission in mali_render_pack_fbd (flag/attachment gated) and AFBC header init. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kbase_fb.h"
#include "mali_cs_emit.h"

void kb_arch_warn_v11_only(unsigned arch, const char *who) { (void)arch; (void)who; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void frame_init(struct mali_frame *f, bool zs)
{
   static const struct mali_rtfmt *rt; bool ok;
   rt = mali_rtfmt_lookup(37, &ok);
   memset(f, 0, sizeof *f);
   f->width = 256; f->height = 256; f->area[2] = 256; f->area[3] = 256;
   f->has_colour = true; f->colour.rt = rt; f->colour.va = 0x100000; f->colour.row_stride = 1024; f->colour.clear = true; f->colour.store = true;
   if (zs) { f->has_zs = true; f->zs.zs_format = 1; f->zs.va = 0x200000; f->zs.row_stride = 1024; f->zs.store = true; }
}

int main(void)
{
   struct mali_env env = { .arch = 13, .tile_log2 = 8, .slot = 3 };
   for (int zs = 0; zs < 2; zs++) {
      struct mali_frame f; uint8_t base[512], on[512];
      frame_init(&f, zs);
      CHECK(mali_render_pack_fbd(&env, &f, base, 0x300000, 0) == 0, "pack base");
      const unsigned rto = KB_FB_FBD_SIZE + (zs ? KB_FB_ZSEXT_SIZE : 0), n = zs ? KB_FB_DESC_SIZE_ZS : KB_FB_DESC_SIZE;
      /* afbc unset + stale words: output unchanged (default path byte-identical) */
      struct mali_frame g = f; g.colour.afbc_rt.row_stride = 77; g.colour.afbc_rt.w2 = 5;
      CHECK(mali_render_pack_fbd(&env, &g, on, 0x300000, 0) == 0 && !memcmp(base, on, n), "afbc unset changed output");

      struct mali_afbc_image_layout al;
      CHECK(mali_afbc_image_layout(13, MALI_AFBC_SB_16x16, 32, 256, 256, 3, 2, &al) == 0, "layout");
      CHECK(mali_render_att_afbc(&g.colour, &al, false, 0x10000000, 0, 0, 37, 256) == -1 && !g.colour.afbc, "flag off set afbc");
      CHECK(mali_render_att_afbc(&g.colour, &al, true, 0x10000000, 3, 0, 37, 256) == -1, "level range");
      CHECK(mali_render_att_afbc(&g.colour, &al, true, 0x10000000, 0, 2, 37, 256) == -1, "layer range");
      CHECK(mali_render_att_afbc(&g.colour, &al, true, 0x10000000, 1, 1, 37, 256) == 0 && g.colour.afbc, "att afbc");
      const uint64_t hva = 0x10000000ull + al.array_stride + al.level_offset[1];
      CHECK(g.colour.afbc_rt.header_va == hva, "header va %llx want %llx", (unsigned long long)g.colour.afbc_rt.header_va, (unsigned long long)hva);
      CHECK(mali_render_pack_fbd(&env, &g, on, 0x300000, 0) == 0, "pack afbc");
      const uint32_t *b = (const uint32_t *)(void *)(base + rto), *w = (const uint32_t *)(void *)(on + rto);
      CHECK(!memcmp(base, on, rto), "FBD/ZS ext changed by afbc");
      CHECK(((w[1] >> 8) & 0xf) == 12, "block format %u", (w[1] >> 8) & 0xf);
      CHECK(w[2] == (10u << 10), "w2 %x", w[2]);   /* tile 256 = 16x16 = superblock: no reverse issue */
      CHECK(w[8] == (uint32_t)hva && w[9] == (uint32_t)(hva >> 32), "header words");
      CHECK(w[10] == al.level[1].header_row_stride && w[11] == (uint32_t)al.level[1].body_offset, "stride/body %u %u", w[10], w[11]);
      CHECK(w[0] == b[0] && w[12] == b[12] && w[13] == b[13] && (w[1] & ~(0xfu << 8)) == (b[1] & ~(0xfu << 8)), "other RT words changed");
      /* smaller tile -> reverse issue order bit */
      env.tile_log2 = 8;
      CHECK(mali_render_att_afbc(&g.colour, &al, true, 0x10000000, 0, 0, 37, 128) == 0 && (g.colour.afbc_rt.w2 & 8), "reverse issue 128px");
   }
   /* header init: zero only the headers of every level/layer, leave bodies */
   struct mali_afbc_image_layout al;
   CHECK(mali_afbc_image_layout(13, MALI_AFBC_SB_16x16, 32, 64, 64, 3, 2, &al) == 0, "layout2");
   uint8_t *buf = malloc(al.size);
   memset(buf, 0xaa, al.size);
   uint64_t z = mali_afbc_image_header_init(&al, buf), want = 0;
   for (unsigned l = 0; l < 3; l++) want += 2 * al.level[l].header_size;
   CHECK(z == want && z > 0, "init bytes %llu want %llu", (unsigned long long)z, (unsigned long long)want);
   for (unsigned ly = 0; ly < 2; ly++)
      for (unsigned l = 0; l < 3; l++) {
         const uint8_t *h = buf + ly * al.array_stride + al.level_offset[l], *bd = h + al.level[l].body_offset;
         CHECK(h[0] == 0 && h[al.level[l].header_size - 1] == 0, "header not cleared l%u ly%u", l, ly);
         CHECK(h[al.level[l].header_size] == 0xaa || al.level[l].header_size == al.level[l].body_offset, "pad touched");
         CHECK(bd[0] == 0xaa, "body touched l%u ly%u", l, ly);
      }
   free(buf);
   printf(fails ? "FAIL (%d)\n" : "t_render_afbc: all ok\n", fails);
   return fails != 0;
}
