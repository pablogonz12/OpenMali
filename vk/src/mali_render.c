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
#include <vulkan/vulkan_core.h>

#define SW(r, g, b, a) ((r) | (g) << 3 | (b) << 6 | (a) << 9)
#define PF(idx, ord) (((uint32_t)(idx) << 12) | (ord))
#define SRGB (1u << 20)
/* Blendable colour formats beyond RGBA8 (pan_format.c pan_blendable_formats): internal, writeback, ncomp, alloc, float, swizzle, blend format */
static const struct mali_rtfmt rt_tab[] = {
   { VK_FORMAT_R8_UNORM, 1, 16, 1, 4, 0, SW(0, 1, 2, 3), PF(237, 0) },
   { VK_FORMAT_R8_SRGB, 1, 16, 1, 4, 0, SW(0, 1, 2, 3), PF(237, 0) | SRGB },
   { VK_FORMAT_R8G8_UNORM, 1, 17, 2, 4, 0, SW(0, 1, 2, 3), PF(237, 0) },
   { VK_FORMAT_R8G8_SRGB, 1, 17, 2, 4, 0, SW(0, 1, 2, 3), PF(237, 0) | SRGB },
   { VK_FORMAT_R8G8B8A8_SRGB, 1, 19, 4, 4, 0, SW(0, 1, 2, 3), PF(237, 0) | SRGB },
   { VK_FORMAT_B8G8R8A8_UNORM, 1, 19, 4, 4, 0, SW(2, 1, 0, 3), PF(237, 0) },
   { VK_FORMAT_B8G8R8A8_SRGB, 1, 19, 4, 4, 0, SW(2, 1, 0, 3), PF(237, 0) | SRGB },
   { VK_FORMAT_R5G6B5_UNORM_PACK16, 5, 21, 3, 4, 0, SW(2, 1, 0, 3), PF(227, 16) },
   { VK_FORMAT_B5G6R5_UNORM_PACK16, 5, 21, 3, 4, 0, SW(0, 1, 2, 3), PF(227, 16) },
   { VK_FORMAT_R4G4B4A4_UNORM_PACK16, 4, 20, 4, 4, 0, SW(3, 2, 1, 0), PF(233, 0) },
   { VK_FORMAT_B4G4R4A4_UNORM_PACK16, 4, 20, 4, 4, 0, SW(3, 0, 1, 2), PF(233, 0) },
   { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 2, 24, 4, 4, 0, SW(0, 1, 2, 3), PF(238, 0) },
   { VK_FORMAT_A2R10G10B10_UNORM_PACK32, 2, 24, 4, 4, 0, SW(2, 1, 0, 3), PF(238, 0) },
   { VK_FORMAT_R5G5B5A1_UNORM_PACK16, 6, 28, 4, 4, 0, SW(3, 2, 1, 0), PF(225, 0) },
   { VK_FORMAT_B5G5R5A1_UNORM_PACK16, 6, 28, 4, 4, 0, SW(3, 0, 1, 2), PF(225, 0) },
   { VK_FORMAT_R16_SFLOAT, 16, 16, 1, 4, 1, SW(0, 1, 2, 3), PF(199, 0) },
   { VK_FORMAT_R16G16_SFLOAT, 17, 17, 2, 4, 1, SW(0, 1, 2, 3), PF(207, 0) },
   { VK_FORMAT_R16G16B16A16_SFLOAT, 18, 19, 4, 8, 1, SW(0, 1, 2, 3), PF(223, 0) },
   { VK_FORMAT_B10G11R11_UFLOAT_PACK32, 20, 20, 3, 4, 1, SW(0, 1, 2, 3), PF(89, 0) },
};

/* supported: false = not a render target we can do. Returns NULL for R8G8B8A8_UNORM (the proven default) with *supported true. */
const struct mali_rtfmt *
mali_rtfmt_lookup(int vk_format, bool *supported)
{
   *supported = true;
   if (vk_format == VK_FORMAT_R8G8B8A8_UNORM)
      return NULL;
   for (unsigned i = 0; i < sizeof rt_tab / sizeof rt_tab[0]; i++)
      if (rt_tab[i].vk_format == vk_format)
         return &rt_tab[i];
   *supported = false;
   return NULL;
}

static uint32_t
f2h(float f)
{
   union { float f; uint32_t u; } v = { f };
   uint32_t s = (v.u >> 16) & 0x8000, e = (v.u >> 23) & 0xff, m = v.u & 0x7fffff;
   if (e == 0xff)
      return s | 0x7c00 | (m ? 0x200 : 0);
   int x = (int)e - 127 + 15;
   if (x >= 31)
      return s | 0x7c00;
   if (x <= 0) {
      if (x < -10)
         return s;
      m |= 0x800000;
      return s | (m >> (14 - x));
   }
   return s | (x << 10) | (m >> 13);
}

static uint32_t
unorm(float c, unsigned bits)
{
   uint32_t mx = (1u << bits) - 1;
   if (!(c > 0.0f))
      return 0;
   if (c >= 1.0f)
      return mx;
   return (uint32_t)lroundf(c * (float)mx);
}

static float
lin2srgb(float c)
{
   if (!(c > 0.0f))
      return 0.0f;
   if (c >= 1.0f)
      return 1.0f;
   return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* fixed point with `ib` integer + `fb` fractional bits (not dithered: fraction stays zero), as pan_clear.c float_to_fixed */
static uint32_t
fx(float f, unsigned ib, unsigned fb)
{
   return unorm(f, ib) << fb;
}

/* Clear words in the tile buffer's internal format (pan_clear.c pan_pack_color), all four words filled. */
void
mali_rtfmt_clear_words(const struct mali_rtfmt *rt, const float cin[4], uint32_t w[4])
{
   uint32_t a, b;
   float c[4] = { cin[0], cin[1], cin[2], cin[3] };
   if (rt && !rt->is_float && (rt->blend_pixfmt & (1u << 20)))
      for (unsigned i = 0; i < 3; i++)
         c[i] = lin2srgb(c[i]);
   switch (rt ? rt->internal : 1) {
   case 2: a = unorm(c[0], 10) | unorm(c[1], 10) << 10 | unorm(c[2], 10) << 20 | unorm(c[3], 2) << 30; b = a; break;
   case 4: a = fx(c[0], 4, 4) | fx(c[1], 4, 4) << 8 | fx(c[2], 4, 4) << 16 | fx(c[3], 4, 4) << 24; b = a; break;
   case 5: a = fx(c[0], 5, 5) | fx(c[1], 6, 4) << 10 | fx(c[2], 5, 5) << 20 | fx(c[3], 0, 2) << 30; b = a; break;
   case 6: a = fx(c[0], 5, 5) | fx(c[1], 5, 5) << 10 | fx(c[2], 5, 5) << 20 | fx(c[3], 1, 1) << 30; b = a; break;
   case 16: a = f2h(c[0]); a |= a << 16; b = a; break;
   case 17: a = f2h(c[0]) | f2h(c[1]) << 16; b = a; break;
   case 18: a = f2h(c[0]) | f2h(c[1]) << 16; b = f2h(c[2]) | f2h(c[3]) << 16; break;
   case 20: {   /* R11G11B10F: unsigned halves truncated to 11/11/10 bits */
      uint32_t r = c[0] > 0 ? f2h(c[0]) >> 4 : 0, g = c[1] > 0 ? f2h(c[1]) >> 4 : 0, bl = c[2] > 0 ? f2h(c[2]) >> 5 : 0;
      a = (r & 0x7ff) | (g & 0x7ff) << 11 | (bl & 0x3ff) << 22; b = a; break;
   }
   default:
      a = mali_unorm8(c[0]) | (uint32_t)mali_unorm8(c[1]) << 8 | (uint32_t)mali_unorm8(c[2]) << 16 | (uint32_t)mali_unorm8(c[3]) << 24;
      b = a;
   }
   w[0] = a; w[1] = b; w[2] = a; w[3] = b;
}

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
   if (f->colour.rt) {
      const struct mali_rtfmt *rt = f->colour.rt;
      p.rt_internal = rt->internal;
      p.rt_wb = rt->wb;
      p.rt_swizzle = rt->swizzle;
      p.rt_alloc = rt->alloc;
      if (f->colour.clear)
         mali_rtfmt_clear_words(rt, f->colour.clear_color, p.rt_clear);
   }
   if (f->colour.clear && !f->colour.rt) {
      p.clear_r = mali_unorm8(f->colour.clear_color[0]);
      p.clear_g = mali_unorm8(f->colour.clear_color[1]);
      p.clear_b = mali_unorm8(f->colour.clear_color[2]);
      p.clear_a = mali_unorm8(f->colour.clear_color[3]);
   }
   p.tiler_va = tiler_ctx_va;
   p.sample_locations_va = sample_locations_va;
   bool msaa = f->samples == 4;
   if (msaa) {
      /* PanVK pan_desc.c: sample count 4 (log2 2), Rotated 4x pattern (2), tile size / 4 (same tile-buffer bytes), sample locations
       * pointer = table + pattern * 256 (pan_sample_positions_offset). */
      p.samples_log2 = 2;
      p.sample_pattern = 2;
      p.tile_log2 = 8;
      p.sample_locations_va = sample_locations_va + 2 * 256;
      if (f->colour.resolve_va) {
         p.rt_gpu_va = f->colour.resolve_va;      /* Average: RT base is the single-sample resolve image */
         p.row_stride = f->colour.resolve_row_stride;
         p.rt_msaa = 1;
      } else {
         p.rt_msaa = 3;                           /* Layered: all samples stored, surface stride = one sample plane */
         p.surface_stride = f->colour.surface_stride;
      }
      p.zs_msaa = 3;
   }
   if (f->has_zs) {
      p.zs_format = (enum kb_zs_format)f->zs.zs_format;
      p.zs_gpu_va = f->zs.va;
      p.zs_row_stride = f->zs.row_stride;
      p.clear_depth = f->zs.clear ? f->zs.clear_depth : 0.0f;
      p.zs_no_store = !f->zs.store;
   }
   if (f->has_st) {
      p.has_stencil = 1;
      p.s_gpu_va = f->st.va;
      p.s_row_stride = f->st.row_stride;
      p.clear_stencil = f->st.clear ? f->st.clear_stencil : 0;
      p.s_no_store = !f->st.store;
   }
   p.arch = env->arch == 13 ? 13 : 0;
   int rc = kb_fb_pack(out, &p);
   if (rc)
      return rc;
   if (env->arch == 13) {
      uint32_t *w = (uint32_t *)out;
      unsigned al = ((1u << env->tile_log2) * (f->colour.rt ? f->colour.rt->alloc : 4u)) >> 10;
      if (!al)
         al = 1;
      unsigned tl = env->tile_log2 - (msaa ? 2 : 0);   /* 4x: smaller tile, same bytes (pixels * samples unchanged) */
      w[11] = (w[11] & ~((0xfu << 9) | (0xffu << 24))) | ((tl & 0xfu) << 9) | (al << 24);
   }
   return 0;
}
