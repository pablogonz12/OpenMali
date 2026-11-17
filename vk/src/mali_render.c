/* SPDX-License-Identifier: MIT */
/* M8: render pass instance -> Framebuffer Descriptor (v11 and v13). Fields come from libkbase kb_fb_pack (hardware-proven on v11);
 * v13 patches word 11 (tile log2, colour allocation in KiB) as in tests/rung2_v13.h. Load/store ops map as follows:
 *   colour loadOp CLEAR -> clear colour in the FBD; DONT_CARE -> clear to 0 (no FBD "no clear" knob in kb_fb_pack yet);
 *   colour LOAD         -> not representable (rejected by mali_cmd.c before it gets here);
 *   colour storeOp      -> always stored (Clean Tile Write on);
 *   depth loadOp CLEAR  -> Z clear value; depth storeOp DONT_CARE/NONE -> Z write enable off (zs_no_store).
 * FBD word 12 bits 31:28 are NOT set on v13 (they fault the queue, V13-NEXT2.md). */
#include <errno.h>
#include <math.h>
#include <string.h>

#include "kbase_fb.h"
#include "mali_cs_emit.h"

uint8_t
mali_unorm8(float c)
{
   if (!(c > 0.0f))
      return 0;
   if (c >= 1.0f)
      return 255;
   return (uint8_t)lroundf(c * 255.0f);
}

int
mali_render_pack_fbd(const struct mali_env *env, const struct mali_frame *f, uint8_t *out, uint64_t tiler_ctx_va,
                     uint64_t sample_locations_va)
{
   struct kb_fb_params p;

   if (!f->has_colour || !f->width || !f->height)
      return -EINVAL;
   memset(&p, 0, sizeof p);
   p.width = f->width;
   p.height = f->height;
   p.rt_gpu_va = f->colour.va;
   p.row_stride = f->colour.row_stride ? f->colour.row_stride : f->width * 4;
   if (f->colour.clear) {
      p.clear_r = mali_unorm8(f->colour.clear_color[0]);
      p.clear_g = mali_unorm8(f->colour.clear_color[1]);
      p.clear_b = mali_unorm8(f->colour.clear_color[2]);
      p.clear_a = mali_unorm8(f->colour.clear_color[3]);
   }
   p.tiler_va = tiler_ctx_va;
   p.sample_locations_va = sample_locations_va;
   if (f->has_zs) {
      p.zs_format = (enum kb_zs_format)f->zs.zs_format;
      p.zs_gpu_va = f->zs.va;
      p.zs_row_stride = f->zs.row_stride;
      p.clear_depth = f->zs.clear ? f->zs.clear_depth : 0.0f;
      p.zs_no_store = !f->zs.store;
   }
   p.arch = env->arch == 13 ? 13 : 0;
   int rc = kb_fb_pack(out, &p);
   if (rc)
      return rc;
   if (env->arch == 13) {
      uint32_t *w = (uint32_t *)out;
      unsigned al = ((1u << env->tile_log2) * 4u) >> 10;
      if (!al)
         al = 1;
      w[11] = (w[11] & ~((0xfu << 9) | (0xffu << 24))) | ((env->tile_log2 & 0xfu) << 9) | (al << 24);
   }
   return 0;
}
