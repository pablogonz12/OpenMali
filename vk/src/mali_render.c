/* SPDX-License-Identifier: MIT */
/* M8: render pass instance -> Framebuffer Descriptor (v11 and v13). Fields come from libkbase kb_fb_pack (hardware-proven on v11);
 * v13 patches word 11 (tile log2, colour allocation in KiB) as in tests/rung2_v13.h. Load/store ops map as follows:
 *   colour loadOp CLEAR -> clear colour in the FBD; DONT_CARE -> clear to 0 (no FBD "no clear" knob in kb_fb_pack yet);
 *   colour LOAD         -> not representable (rejected by mali_cmd.c before it gets here);
 *   colour storeOp      -> DONT_CARE/NONE: RT Write Enable off (unless an MSAA resolve writes through it); STORE: Clean Tile Write on;
 *   depth loadOp CLEAR  -> Z clear value; depth storeOp DONT_CARE/NONE -> Z write enable off (zs_no_store).
 * FBD word 12 bits 31:28 are NOT set on v13 (they fault the queue, V13-NEXT2.md). */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "kbase_fb.h"
#include "mali_cs_emit.h"
#include "mali_rtfmt_tab.h"
#include <vulkan/vulkan_core.h>

/* supported: false = not a render target we can do. Returns NULL for R8G8B8A8_UNORM (the proven default) with *supported true. */
const struct mali_rtfmt *
mali_rtfmt_lookup(int vk_format, bool *supported)
{
   return mali_rtfmt_find(vk_format, supported);
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

/* RAW tile buffer, the formats that are not plain per-channel integers: the clear value as the pixel the memory format stores (SNORM, UNORM16,
 * 32-bit float, A2B10G10R10 integer), replicated like pan_pack_raw. Returns false for every other format. */
static bool
raw_special_words(int fmt, const float c[4], uint32_t w[4])
{
   uint32_t px[4] = {0, 0, 0, 0}, u[4];
   unsigned n, bits, bytes;
   int kind;   /* 0 snorm, 1 unorm, 2 f32, 3 a2b10g10r10 integer */
   memcpy(u, c, sizeof u);
   switch (fmt) {
   case VK_FORMAT_R8_SNORM: n = 1; bits = 8; kind = 0; break;
   case VK_FORMAT_R8G8_SNORM: n = 2; bits = 8; kind = 0; break;
   case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_A8B8G8R8_SNORM_PACK32: n = 4; bits = 8; kind = 0; break;
   case VK_FORMAT_R16_SNORM: n = 1; bits = 16; kind = 0; break;
   case VK_FORMAT_R16G16_SNORM: n = 2; bits = 16; kind = 0; break;
   case VK_FORMAT_R16G16B16A16_SNORM: n = 4; bits = 16; kind = 0; break;
   case VK_FORMAT_R16_UNORM: n = 1; bits = 16; kind = 1; break;
   case VK_FORMAT_R16G16_UNORM: n = 2; bits = 16; kind = 1; break;
   case VK_FORMAT_R16G16B16A16_UNORM: n = 4; bits = 16; kind = 1; break;
   case VK_FORMAT_R32_SFLOAT: n = 1; bits = 32; kind = 2; break;
   case VK_FORMAT_R32G32_SFLOAT: n = 2; bits = 32; kind = 2; break;
   case VK_FORMAT_R32G32B32A32_SFLOAT: n = 4; bits = 32; kind = 2; break;
   case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2B10G10R10_SINT_PACK32: n = 4; bits = 10; kind = 3; break;
   default: return false;
   }
   if (kind == 3) {
      px[0] = (u[0] & 0x3ff) | (u[1] & 0x3ff) << 10 | (u[2] & 0x3ff) << 20 | (u[3] & 3u) << 30;
      bytes = 4;
   } else {
      bytes = n * bits / 8;
      const uint32_t mask = bits == 32 ? ~0u : (1u << bits) - 1;
      for (unsigned i = 0; i < n; i++) {
         uint32_t v;
         if (kind == 2)
            v = u[i];
         else if (kind == 1)
            v = (uint32_t)lrintf(fminf(fmaxf(c[i] != c[i] ? 0.0f : c[i], 0.0f), 1.0f) * (float)mask);
         else {
            const float m = (float)(mask >> 1);
            v = (uint32_t)(int32_t)lrintf(fminf(fmaxf(c[i] != c[i] ? 0.0f : c[i], -1.0f), 1.0f) * m) & mask;
         }
         unsigned bit = i * bits;
         px[bit / 32] |= (v & mask) << (bit % 32);
      }
   }
   if (bytes == 1)
      px[0] |= px[0] << 8, px[0] |= px[0] << 16;
   else if (bytes == 2)
      px[0] |= px[0] << 16;
   if (bytes <= 4)
      w[0] = w[1] = w[2] = w[3] = px[0];
   else if (bytes == 8) {
      w[0] = w[2] = px[0]; w[1] = w[3] = px[1];
   } else
      memcpy(w, px, 16);
   return true;
}

/* Clear words in the tile buffer's internal format (pan_clear.c pan_pack_color), all four words filled. */
void
mali_rtfmt_clear_words(const struct mali_rtfmt *rt, const float cin[4], uint32_t w[4])
{
   uint32_t a, b;
   float c[4] = { cin[0], cin[1], cin[2], cin[3] };
   if (rt && rt->internal >= 32 && raw_special_words(rt->vk_format, cin, w))
      return;
   if (rt && rt->int_bits) {
      /* integer colour: the clear value's raw 32-bit words (VkClearColorValue uint32/int32), channels packed as stored, replicated per
       * pan_pack_raw (<=4 B: the word repeats, 8 B: two words twice, 16 B: as is) */
      uint32_t v[4], px[4] = {0, 0, 0, 0};
      memcpy(v, cin, sizeof v);
      const unsigned bits = rt->int_bits, bytes = rt->ncomp * bits / 8;
      const uint32_t mask = bits == 32 ? ~0u : (1u << bits) - 1;
      for (unsigned i = 0; i < rt->ncomp; i++) {
         unsigned bit = i * bits;
         px[bit / 32] |= (v[i] & mask) << (bit % 32);
      }
      if (bytes == 1)
         px[0] |= px[0] << 8, px[0] |= px[0] << 16;
      else if (bytes == 2)
         px[0] |= px[0] << 16;
      if (bytes <= 4)
         w[0] = w[1] = w[2] = w[3] = px[0];
      else if (bytes == 8) {
         w[0] = w[2] = px[0]; w[1] = w[3] = px[1];
      } else
         memcpy(w, px, 16);
      return;
   }
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

/* log2 of the tile AREA the FBD ends up declaring as its Effective Tile Size. kb_fb_pack writes 8 (16x16) for 4x MSAA and
 * its default 10 (32x32) otherwise; mali_render_pack_fbd then overrides it on v13 with env->tile_log2, quartered for 4x.
 * Anything that has to agree with the tile (the hierarchy mask, this bound box) must use this, not env->tile_log2. */
unsigned
mali_fbd_tile_log2(const struct mali_env *env, const struct mali_frame *f)
{
   if (env->arch == 13)
      return f->samples == 4 ? env->tile_log2 - 2 : env->tile_log2;
   return f->samples == 4 ? 8u : 10u;
}

/* FBD Bound Min/Max: the tile range the fragment job covers, as PanVK packs it (pan_fb.c pan_fb_bbox_align rounds the
 * draw extent to whole tiles, panvk_vX_cmd_draw.c puts it in the FBD). kb_fb_pack writes the whole attachment; a render
 * area smaller than the attachment then makes the fragment job cover (LOAD-preload) tiles no draw touches.
 * MALI_BBOX32: unset/0 = round to the effective tile (the real one, from mali_fbd_tile_log2); 1 = round to 32 px, the
 * pre-audit behaviour (a 32-px box is not tile-aligned on a 64x64 tile); 2 = the whole attachment, what main did before
 * this change, for a three-way phone A/B. out = { minx, miny, maxx, maxy }, inclusive, clipped to the attachment. */
void
mali_fbd_bounds(const struct mali_env *env, const struct mali_frame *f, uint32_t *out)
{
   const unsigned w = f->width, h = f->height;
   out[0] = 0; out[1] = 0; out[2] = w ? w - 1 : 0; out[3] = h ? h - 1 : 0;
   const char *e = getenv("MALI_BBOX32");
   const int mode = e ? atoi(e) : 0;
   if (mode == 2 || !w || !h || f->area[2] <= 0 || f->area[3] <= 0)
      return;
   /* Render area clipped to the attachment, in pixels. */
   long x0 = f->area[0] < 0 ? 0 : f->area[0], y0 = f->area[1] < 0 ? 0 : f->area[1];
   long x1 = (long)f->area[0] + f->area[2], y1 = (long)f->area[1] + f->area[3];
   if (x0 > (long)w || y0 > (long)h || x1 <= x0 || y1 <= y0)
      return;
   if (x1 > (long)w) x1 = w;
   if (y1 > (long)h) y1 = h;
   const unsigned tl = mode == 1 ? 10u : mali_fbd_tile_log2(env, f);   /* 10 = 32x32 */
   const long step = 1L << (tl / 2);                                   /* tile edge in pixels: 32 or 64 */
   long minx = x0 & ~(step - 1), miny = y0 & ~(step - 1);              /* round outward: never clip a pixel the pass covers */
   long maxx = ((x1 + step - 1) & ~(step - 1)) - 1, maxy = ((y1 + step - 1) & ~(step - 1)) - 1;
   if (maxx > (long)w - 1) maxx = w - 1;
   if (maxy > (long)h - 1) maxy = h - 1;
   out[0] = (uint32_t)minx; out[1] = (uint32_t)miny; out[2] = (uint32_t)maxx; out[3] = (uint32_t)maxy;
}

/* Pre-frame shader mode of the colour preload (see mali_cs_emit.h). Panfrost/PanVK reference, read only:
 * panvk_vX_cmd_frame_shaders.c cmd_preload_color_attachments (INTERSECT unless a written target is "always" = loadOp CLEAR),
 * cmd_preload_zs_attachments (Z/S: EARLY_ZS_ALWAYS v9..v12, PREPASS_ALWAYS v13), pan_desc.h pan_fix_frame_shader_mode and
 * csf/panvk_vX_cmd_draw.c (INTERSECT becomes ALWAYS when any target has Clean Tile Write or CRC: TSIX-2033), pan_fb.c
 * pan_fb_get_clean_tile (Clean Tile Write = load.always | store.always | AFBC superblock > tile). This driver has no CRC, no
 * AFBC render target and no incremental-rendering spill, so those ALWAYS cases cannot arise here; a change adding one of them
 * has to add its reason. */
enum mali_pf_mode
mali_preload_mode(const struct mali_frame *f)
{
   if (!f->pf_cand || !f->has_colour || f->colour.clear || !f->ndraws)
      return MALI_PF_NONE;
   if (f->pf_zs_load)
      return MALI_PF_ALWAYS_ZS_LOAD;
   if ((f->has_zs && f->zs.clear) || (f->has_st && f->st.clear))
      return MALI_PF_ALWAYS_CLEAN_TILE;
   /* Until attachment interactions are proven, even DONT_CARE Z/S must use
    * the existing full-area preload draw. Keep the more specific reasons above. */
   if (f->has_zs || f->has_st)
      return MALI_PF_ALWAYS_ZS_ATTACHMENT;
   if (f->colour.resolve_va)
      return MALI_PF_ALWAYS_RESOLVE;
   if (f->samples > 1)
      return MALI_PF_ALWAYS_MSAA;
   if (f->pf_src_va != f->colour.va)
      return MALI_PF_ALWAYS_SRC;
   if (f->ndraws < 2)
      return MALI_PF_ALWAYS_EMPTY;
   return MALI_PF_INTERSECT;
}

bool
mali_preload_intersect_on(void)
{
   const char *e = getenv("MALI_PRELOAD_INTERSECT");
   return e && e[0] == '1';
}

static unsigned g_pf_count[MALI_PF_NMODES];
static pthread_mutex_t g_pf_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_pf_atexit;
static const char *const g_pf_name[MALI_PF_NMODES] = { "none", "intersect", "always_zs_load", "always_clean_tile", "always_zs_attachment",
                                                       "always_msaa", "always_resolve", "always_src", "always_empty" };

static void
pf_print(void)
{
   unsigned c[MALI_PF_NMODES];
   mali_preload_mode_stats(c);
   fprintf(stderr, "mali_preload: switch=%s", mali_preload_intersect_on() ? "on" : "off");
   for (int i = 1; i < MALI_PF_NMODES; i++)
      fprintf(stderr, " %s=%u", g_pf_name[i], c[i]);
   fprintf(stderr, "\n");
}

void
mali_preload_mode_note(enum mali_pf_mode m)
{
   if (m <= MALI_PF_NONE || m >= MALI_PF_NMODES)
      return;
   pthread_mutex_lock(&g_pf_lock);
   g_pf_count[m]++;
   if (!g_pf_atexit) {
      const char *e = getenv("MALI_PRELOAD_STATS");
      g_pf_atexit = true;
      if (e && e[0] == '1')
         atexit(pf_print);
   }
   pthread_mutex_unlock(&g_pf_lock);
}

void
mali_preload_mode_stats(unsigned *out)
{
   pthread_mutex_lock(&g_pf_lock);
   memcpy(out, g_pf_count, sizeof g_pf_count);
   pthread_mutex_unlock(&g_pf_lock);
}

int
mali_render_pack_fbd(const struct mali_env *env, const struct mali_frame *f, uint8_t *out, uint64_t tiler_ctx_va,
                     uint64_t sample_locations_va)
{
   return mali_render_pack_fbd_pf(env, f, out, tiler_ctx_va, sample_locations_va, 0);
}

int
mali_render_pack_fbd_pf(const struct mali_env *env, const struct mali_frame *f, uint8_t *out, uint64_t tiler_ctx_va,
                        uint64_t sample_locations_va, uint64_t pf_dcds_va)
{
   struct kb_fb_params p;

   if (!f->width || !f->height)
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
   p.rt_none = !f->has_colour;
   /* storeOp DONT_CARE/NONE: Write Enable off, the tile is never written back. A resolve writes through the same RT, so it keeps the write. */
   p.rt_no_store = f->has_colour && !f->colour.store && !f->colour.resolve_va;
   if (p.rt_none)
      p.row_stride = 0;
   p.tiler_va = tiler_ctx_va;
   p.provoking_last = f->provoking_last;
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
      if (msaa) p.zs_layered_stride = f->zs.surface_stride;
      p.clear_depth = f->zs.clear ? f->zs.clear_depth : 0.0f;
      p.zs_no_store = !f->zs.store;
   }
   if (f->has_st) {
      p.has_stencil = 1;
      p.s_gpu_va = f->st.va;
      p.s_row_stride = f->st.row_stride;
      p.clear_stencil = f->st.clear ? f->st.clear_stencil : 0;
      p.s_no_store = !f->st.store;
      if (msaa) p.s_layered_stride = f->st.surface_stride;
   }
   if (pf_dcds_va) {
      /* colour LOAD as pre-frame shader 0, mode INTERSECT (2): only tiles some primitive touches are preloaded, and with
       * Clean Tile Write off on every target an untouched tile is not written back either, so its memory stays as it was
       * (what LOAD + STORE requires). Clean Tile Write must be off on all of them: TSIX-2033 (pan_desc.h). */
      p.pre_frame_0 = 2;
      p.frame_shader_dcds_va = pf_dcds_va;
      p.no_clean_tile_write = 1;
      p.zs_no_clean_tile_write = 1;
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
      unsigned tl = mali_fbd_tile_log2(env, f);   /* 4x: smaller tile, same bytes (pixels * samples unchanged) */
      w[11] = (w[11] & ~((0xfu << 9) | (0xffu << 24))) | ((tl & 0xfu) << 9) | (al << 24);
   }
   /* FBD Bound Min/Max (words 9, 10) = the render area rounded outward to whole tiles. */
   {
      uint32_t b[4], *w = (uint32_t *)out;
      mali_fbd_bounds(env, f, b);
      w[9] = (b[0] & 0xffffu) | (b[1] << 16);
      w[10] = (b[2] & 0xffffu) | (b[3] << 16);
   }
   return 0;
}
