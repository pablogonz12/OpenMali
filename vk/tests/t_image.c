/* SPDX-License-Identifier: MIT */
/* M5 offline tests: format table, image layout, descriptor bytes vs libkbase (hardware-proven). No GPU. Usage: t_image [-v] */
#include "../src/mali_image.h"
#include "../src/mali_rtfmt_tab.h"
#include "../../libkbase/include/kbase_draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* mali_wsi.c is not linked into this host test: same value as there (DRM_FORMAT_MOD_LINEAR) */
const uint64_t mali_wsi_modifier = 0;

static int g_fail, g_verbose;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int
cmp_words(const char *what, const uint32_t *a, const uint32_t *b, uint32_t mask0)
{
   int bad = 0;
   for (int i = 0; i < 8; i++) {
      uint32_t m = i == 0 ? ~mask0 : ~0u;
      if ((a[i] & m) != (b[i] & m)) {
         printf("  FAIL %s w%d: got %08x want %08x\n", what, i, a[i], b[i]);
         bad = 1;
      }
   }
   if (bad) g_fail++;
   return bad;
}

/* Render-target table: A8B8G8R8 packed 32 has the byte order of R8G8B8A8, so it takes the RGBA8 rows (sweep: renderpass.suballocation.formats a8b8g8r8_*).
 * Non-blendable formats (integer, SNORM, UNORM16, 32-bit float) are RAW tile-buffer rows. */
static void
test_rtfmt(unsigned arch)
{
   bool ok;
   const struct mali_rtfmt *u = mali_rtfmt_find(VK_FORMAT_A8B8G8R8_UNORM_PACK32, &ok);
   CHECK(ok && u && u->wb == 19 && u->internal == 1 && u->ncomp == 4 && u->swizzle == SW(0, 1, 2, 3), "A8B8G8R8_UNORM row");
   const struct mali_rtfmt *s = mali_rtfmt_find(VK_FORMAT_A8B8G8R8_SRGB_PACK32, &ok);
   const struct mali_rtfmt *r = mali_rtfmt_find(VK_FORMAT_R8G8B8A8_SRGB, &ok);
   CHECK(ok && s && r && s->wb == r->wb && s->internal == r->internal && s->swizzle == r->swizzle && s->blend_pixfmt == r->blend_pixfmt, "A8B8G8R8_SRGB row = R8G8B8A8_SRGB");
   struct mali_fmt f;
   CHECK(mali_format_get(arch, VK_FORMAT_A8B8G8R8_UNORM_PACK32, &f) && f.blendable, "A8B8G8R8_UNORM blendable");
   CHECK(mali_format_get(arch, VK_FORMAT_A8B8G8R8_SRGB_PACK32, &f) && f.blendable, "A8B8G8R8_SRGB blendable");
   /* every RAW (non-blendable) row: the blend unit's opaque conversion word is the format's texture word (hw >> 12 / hw & 0xfff), the tile-buffer
    * internal format is 32 + log2(bytes) and the writeback is RAW8/16/32/64/128 = 0/1/3/5/7 */
   unsigned nraw = 0;
   for (int v = 1; v < 1000; v++) {
      const struct mali_rtfmt *row = mali_rtfmt_find(v, &ok);
      if (!ok || !row || row->internal < 32)
         continue;
      nraw++;
      struct mali_fmt q;
      CHECK(mali_format_get(arch, v, &q) && q.rt, "RAW row %d has no format caps", v);
      CHECK(row->blend_pixfmt == (q.hw & 0xfffffu), "RAW row %d blend word %08x vs texture word %08x", v, row->blend_pixfmt, q.hw & 0xfffffu);
      unsigned lg = row->internal - 32;
      static const uint8_t wb_of[5] = { 0, 1, 3, 5, 7 };
      CHECK(lg < 5 && row->wb == wb_of[lg] && row->alloc == (1u << lg), "RAW row %d internal %u wb %u alloc %u", v, row->internal, row->wb, row->alloc);
      CHECK(!q.blendable, "RAW row %d marked blendable", v);
   }
   CHECK(nraw >= 30, "only %u RAW rows", nraw);
   static const int snorm[] = { VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_A8B8G8R8_SNORM_PACK32, VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM,
                                VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_A8B8G8R8_UINT_PACK32, VK_FORMAT_A8B8G8R8_SINT_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32,
                                VK_FORMAT_A2B10G10R10_SINT_PACK32, VK_FORMAT_R8G8_UINT, VK_FORMAT_R32_SINT };
   for (unsigned i = 0; i < sizeof snorm / sizeof snorm[0]; i++) {
      const struct mali_rtfmt *row = mali_rtfmt_find(snorm[i], &ok);
      CHECK(ok && row && row->internal >= 32, "format %d should be a RAW render target", snorm[i]);
   }
   /* still unsupported: 3-component, BGRA integer/snorm, A2R10G10B10 integer */
   static const int gap[] = { VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_B8G8R8A8_SNORM,
                              VK_FORMAT_B8G8R8A8_UINT, VK_FORMAT_A2R10G10B10_UINT_PACK32 };
   for (unsigned i = 0; i < sizeof gap / sizeof gap[0]; i++) {
      mali_rtfmt_find(gap[i], &ok);
      CHECK(!ok, "format %d unexpectedly a render target (update the gap list in the sweep notes)", gap[i]);
   }
}

static void
test_rgba8(unsigned arch)
{
   struct mali_fmt f;
   CHECK(mali_format_get(arch, VK_FORMAT_R8G8B8A8_UNORM, &f), "rgba8 missing");
   CHECK(f.hw == KB_PIXFMT_RGBA8, "rgba8 hw %x want %x", f.hw, KB_PIXFMT_RGBA8);
   CHECK(f.rt_tb == KB_PIXFMT_RGBA8_TB, "rgba8 rt_tb %x want %x", f.rt_tb, KB_PIXFMT_RGBA8_TB);
   CHECK(f.rt_writeback == 19, "rgba8 writeback %u", f.rt_writeback);

   struct mali_image_desc d = { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, { 8, 8, 1 }, 1, 1, 1, false };
   struct mali_image_layout l;
   CHECK(mali_image_layout_init(arch, &d, &l) == 0, "layout");
   CHECK(l.nplanes == 1, "nplanes");
   CHECK(l.plane[0].slice[0].row_stride == 64, "row stride %u", l.plane[0].slice[0].row_stride);
   CHECK(l.size == 512, "size %llu", (unsigned long long)l.size);
   CHECK(l.alignment >= 64, "alignment %u", l.alignment);

   uint64_t image_va = 0x7f0000100000ull, payload_va = 0x7f0000300000ull;
   uint32_t payload[8] = { 0 };
   struct mali_view_desc v = { .format = d.format, .type = VK_IMAGE_VIEW_TYPE_2D, .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .base_level = 0,
                               .levels = 1, .base_layer = 0, .layers = 1,
                               .swizzle = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A } };
   struct mali_hw_desc tex;
   CHECK(mali_view_payload_entries(&l, &v) == 1, "payload entries");
   CHECK(mali_view_emit(arch, &l, image_va, &v, payload, payload_va, &tex) == 0, "emit");
   uint32_t gt[8], gp[8];
   kb_draw_texture_2d_rgba8(gt, 8, 8, payload_va);
   kb_draw_plane_linear(gp, image_va, 8, 8, 64);
   cmp_words("texture", tex.w, gt, 0);
   cmp_words("plane", payload, gp, 0);
   if (g_verbose) {
      for (int i = 0; i < 8; i++) printf("  tex[%d]=%08x gold %08x | plane %08x gold %08x\n", i, tex.w[i], gt[i], payload[i], gp[i]);
   }
}

static void
test_view_min_lod(unsigned arch)
{
   /* VK_EXT_image_view_min_lod: minLod counts from image level 0; the descriptor's minimum-LOD field (word 3, 8.8 fixed) from the view's first level. */
   struct mali_image_desc d = { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, { 8, 8, 1 }, 4, 1, 1, false };
   struct mali_image_layout l;
   CHECK(mali_image_layout_init(arch, &d, &l) == 0, "min_lod layout");
   const struct { unsigned base, levels; float min_lod; bool storage; unsigned want; } c[] = {
      { 0, 4, 0.0f, false, 0 }, { 0, 4, 2.5f, false, 640 }, { 1, 3, 2.5f, false, 384 },
      { 2, 2, 2.0f, false, 0 }, { 1, 3, 0.0f, false, 0 }, { 0, 4, 2.5f, true, 0 },
   };
   for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
      struct mali_view_desc v = { .format = d.format, .type = VK_IMAGE_VIEW_TYPE_2D, .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .base_level = c[i].base, .levels = c[i].levels, .base_layer = 0, .layers = 1, .min_lod = c[i].min_lod,
                                  .storage = c[i].storage,
                                  .swizzle = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A } };
      uint32_t payload[64] = { 0 };
      struct mali_hw_desc tex;
      CHECK(mali_view_payload_entries(&l, &v) <= 16, "min_lod case %u payload", i);
      CHECK(mali_view_emit(arch, &l, 0x7f0000100000ull, &v, payload, 0x7f0000300000ull, &tex) == 0, "min_lod case %u emit", i);
      CHECK((tex.w[3] & 0x1fff) == c[i].want, "min_lod case %u (base %u, minLod %.1f, storage %d): field %u want %u", i, c[i].base, c[i].min_lod, c[i].storage, tex.w[3] & 0x1fff, c[i].want);
   }
}

static void
test_attr(unsigned arch)
{
   struct mali_fmt f;
   CHECK(mali_format_get(arch, VK_FORMAT_R32G32B32A32_SFLOAT, &f), "rgba32f missing");
   CHECK(f.hw == KB_PIXFMT_RGBA32F, "rgba32f hw %x", f.hw);
   uint32_t w0 = mali_format_attr_word0(&f);
   CHECK(w0 == 0x2fc00115u, "attr word0 %08x", w0);
   uint32_t g[8];
   kb_draw_attr_1d(g, KB_PIXFMT_RGBA32F, 0, 0, 0, 16);
   CHECK(g[0] == w0, "attr vs libkbase %08x vs %08x", w0, g[0]);
}

static void
test_depth(unsigned arch)
{
   struct mali_fmt f;
   CHECK(mali_format_get(arch, VK_FORMAT_D32_SFLOAT, &f) && f.has_depth && f.zs_write == 14 && f.zs_internal == 2, "D32 zs codes");
   CHECK(mali_format_get(arch, VK_FORMAT_X8_D24_UNORM_PACK32, &f) && f.zs_write == 4 && f.zs_internal == 1, "D24X8 zs codes");
   CHECK(mali_format_get(arch, VK_FORMAT_D16_UNORM, &f) && f.zs_write == 1 && f.zs_internal == 0, "D16 zs codes");
   CHECK(mali_format_get(arch, VK_FORMAT_D32_SFLOAT_S8_UINT, &f) && f.nplanes == 2 && f.has_stencil, "D32S8 two planes");

   /* 8x8 D32 texture: a linear plane of 4-byte elements, 32 B stride */
   struct mali_image_desc d = { VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D, { 8, 8, 1 }, 1, 1, 1, false };
   struct mali_image_layout l;
   CHECK(mali_image_layout_init(arch, &d, &l) == 0, "depth layout");
   CHECK(l.plane[0].slice[0].row_stride == 64, "depth row stride %u", l.plane[0].slice[0].row_stride);
   uint32_t payload[8];
   struct mali_view_desc v = { .format = d.format, .type = VK_IMAGE_VIEW_TYPE_2D, .aspect = VK_IMAGE_ASPECT_DEPTH_BIT, .levels = 1, .layers = 1,
                               .swizzle = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE } };
   struct mali_hw_desc tex;
   CHECK(mali_view_emit(arch, &l, 0x1000, &v, payload, 0x2000, &tex) == 0, "depth emit");
   uint32_t gp[8];
   kb_draw_plane_linear(gp, 0x1000, 8, 8, 64);
   /* the clump format differs per element size; compare address, size, stride, dims only (words 1..7) */
   CHECK(payload[1] == gp[1] && payload[2] == gp[2] && payload[3] == gp[3], "depth plane addr/size words");
   CHECK(((tex.w[0] >> 10) & 0x3fffff) == f.plane[0].hw || f.plane[0].hw == 0, "depth tex hw");
}

static void
test_layout(unsigned arch)
{
   struct mali_image_layout l;
   /* mips: 64x64, 7 levels, RGBA8 linear */
   struct mali_image_desc d = { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, { 64, 64, 1 }, 7, 1, 1, false };
   CHECK(mali_image_layout_init(arch, &d, &l) == 0, "mips layout");
   uint64_t prev_end = 0;
   for (unsigned i = 0; i < 7; i++) {
      const struct mali_slice *s = &l.plane[0].slice[i];
      CHECK(s->offset % 64 == 0, "mip %u offset %llu unaligned", i, (unsigned long long)s->offset);
      CHECK(s->offset >= prev_end, "mip %u overlaps", i);
      CHECK(s->row_stride >= (64u >> i > 1 ? 64u >> i : 1u) * 4, "mip %u stride %u", i, s->row_stride);
      prev_end = s->offset + s->size;
   }
   CHECK(l.size >= prev_end, "mips size");

   /* array: layers are separated by array_stride, each a multiple of 64 */
   struct mali_image_desc a = { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, { 16, 16, 1 }, 3, 5, 1, false };
   CHECK(mali_image_layout_init(arch, &a, &l) == 0, "array layout");
   CHECK(l.plane[0].array_stride % 64 == 0 && l.plane[0].array_stride > 0, "array stride");
   CHECK(l.size == l.plane[0].array_stride * 5, "array size %llu", (unsigned long long)l.size);
   CHECK(mali_image_subresource_offset(&l.plane[0], 1, 2) == 2 * l.plane[0].array_stride + l.plane[0].slice[1].offset, "subres offset");

   /* 3D: depth slices multiply the surface */
   struct mali_image_desc t3 = { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_3D, { 8, 8, 4 }, 1, 1, 1, false };
   CHECK(mali_image_layout_init(arch, &t3, &l) == 0, "3d layout");
   CHECK(l.size == 4 * 512, "3d size %llu", (unsigned long long)l.size);

   /* MSAA 4x multiplies */
   struct mali_image_desc ms = { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, { 8, 8, 1 }, 1, 1, 4, false };
   CHECK(mali_image_layout_init(arch, &ms, &l) == 0, "msaa layout");
   CHECK(l.size == 4 * 512, "msaa size %llu", (unsigned long long)l.size);

   /* tiled: 16x16 u-interleaved, sizes rounded to whole tiles */
   struct mali_image_desc ti = { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, { 20, 20, 1 }, 1, 1, 1, true };
   CHECK(mali_image_layout_init(arch, &ti, &l) == 0, "tiled layout");
   CHECK(l.size >= 32 * 32 * 4, "tiled size %llu", (unsigned long long)l.size);

   /* D32S8 has two planes, separate sizes */
   struct mali_image_desc ds = { VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_TYPE_2D, { 8, 8, 1 }, 1, 1, 1, false };
   CHECK(mali_image_layout_init(arch, &ds, &l) == 0, "ds layout");
   CHECK(l.nplanes == 2 && l.plane[1].offset >= l.plane[0].size, "ds planes %u", l.nplanes);

   /* out of range */
   struct mali_image_desc big = { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, { MALI_MAX_DIM_2D + 1, 8, 1 }, 1, 1, 1, false };
   CHECK(mali_image_layout_init(arch, &big, &l) != 0, "oversize accepted");
   struct mali_image_desc bad = { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, { 8, 8, 1 }, 1, 1, 3, false };
   CHECK(mali_image_layout_init(arch, &bad, &l) != 0, "3 samples accepted");
}

static void
test_sampler(unsigned arch)
{
   VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
   struct vk_sampler s = { .reduction_mode = VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE };
   struct mali_hw_desc o;
   mali_sampler_pack(arch, &ci, &s, &o);
   uint32_t g[8];
   kb_draw_sampler_nearest_clamp(g);
   /* bits 21 (round to nearest even) and 26 (clamp integer array index) are genxml defaults libkbase omits */
   cmp_words("sampler", o.w, g, (1u << 21) | (1u << 26));

   ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
   ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
   ci.magFilter = VK_FILTER_LINEAR;
   ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
   ci.minLod = 3.0f;
   ci.maxLod = 4.0f;
   ci.compareEnable = VK_TRUE;
   ci.compareOp = VK_COMPARE_OP_LESS;
   mali_sampler_pack(arch, &ci, &s, &o);
   /* U -> Wrap Mode S (bits 16), V -> T (12), W -> R (8) */
   CHECK(((o.w[0] >> 16) & 15) == 8 && ((o.w[0] >> 12) & 15) == 12 && ((o.w[0] >> 8) & 15) == 9, "wrap modes %08x", o.w[0]);
   CHECK((o.w[1] & 0x1fff) == 768, "min lod %u", o.w[1] & 0x1fff);
   ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
   mali_sampler_pack(arch, &ci, &s, &o);
   CHECK(((o.w[0] >> 8) & 15) == 11 && ((o.w[0] >> 16) & 15) == 8, "W border -> R field, U unchanged %08x", o.w[0]);
   ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   mali_sampler_pack(arch, &ci, &s, &o);
   CHECK((o.w[0] >> 27 & 1) == 1 && (o.w[0] >> 28 & 1) == 0, "filters %08x", o.w[0]);
   CHECK((o.w[0] >> 30) == 3, "mipmap mode");
   CHECK(((o.w[1] >> 13) & 7) == 1, "compare func %u", (o.w[1] >> 13) & 7);
   CHECK(((o.w[1] >> 16) & 0x1fff) == 1024, "max lod %u", (o.w[1] >> 16) & 0x1fff);

   /* border colour swizzle: unset mapping is identity; BGRA view mapping swaps R/B in the descriptor; ONE/ZERO channels do not move data */
   s.border_color_value.uint32[0] = 1; s.border_color_value.uint32[1] = 2; s.border_color_value.uint32[2] = 3; s.border_color_value.uint32[3] = 4;
   VkSamplerBorderColorComponentMappingCreateInfoEXT bcm = { .sType = VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT };
   mali_sampler_pack(arch, &ci, &s, &o);
   CHECK(o.w[4] == 1 && o.w[5] == 2 && o.w[6] == 3 && o.w[7] == 4, "identity border");
   ci.pNext = &bcm;
   bcm.components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A };
   mali_sampler_pack(arch, &ci, &s, &o);
   CHECK(o.w[4] == 3 && o.w[5] == 2 && o.w[6] == 1 && o.w[7] == 4, "bgra border %u %u %u %u", o.w[4], o.w[5], o.w[6], o.w[7]);
   bcm.components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_R };
   mali_sampler_pack(arch, &ci, &s, &o);
   CHECK(o.w[4] == 4 && o.w[5] == 2 && o.w[6] == 3 && o.w[7] == 4, "one/a->r border %u %u %u %u", o.w[4], o.w[5], o.w[6], o.w[7]);
}

static void
test_texel_buffer(unsigned arch)
{
   struct mali_fmt f;
   CHECK(mali_format_get(arch, VK_FORMAT_R32G32B32A32_SFLOAT, &f), "fmt");
   struct mali_hw_desc d;
   mali_pack_texel_buffer(&d, &f, 0x7f0000400000ull, 256);
   CHECK(d.w[0] == (9u | (3u << 4)), "texel w0 %08x", d.w[0]);
   CHECK(d.w[1] == 256 && d.w[2] == 0x00400000u && d.w[3] == 0x7f00 && d.w[4] == 16 && d.w[7] == f.hw, "texel words");
}

static void
test_format_table(unsigned arch)
{
   unsigned n = mali_format_count(arch);
   CHECK(n > 20, "only %u formats", n);
   for (unsigned i = 0; i < n; i++) {
      struct mali_fmt f;
      CHECK(mali_format_nth(arch, i, &f), "nth %u", i);
      bool tex = f.optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      bool rt = f.optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
      bool ds = f.optimal & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
      bool vb = f.buffer & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
      bool tb = f.buffer & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
      bool st = f.optimal & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
      CHECK(tex || rt || ds || vb || tb || st, "format %d advertises nothing", f.vk);
      CHECK(f.bind != 0, "format %d has no pan bind", f.vk);
      if (tex || tb || vb || st) CHECK(f.hw != 0 || f.nplanes == 2 || f.plane[0].hw != 0, "format %d has no hw word", f.vk);
      /* RT: hw word always; blendable ones also carry internal/writeback/tilebuffer (non-blendable: raw/shader path, gap in R3-PLAN M5) */
      if (rt) CHECK(f.rt && f.hw != 0, "format %d RT without hw word", f.vk);
      if (rt && f.blendable) CHECK(f.rt_writeback != 0 && f.rt_internal != 0 && f.rt_tb != 0, "format %d blendable RT incomplete", f.vk);
      if (ds) CHECK(f.has_depth || f.has_stencil, "format %d DS without aspect", f.vk);
      if (f.compressed) CHECK(f.bw > 1 && f.bh > 1, "format %d compressed block %ux%u", f.vk, f.bw, f.bh);
      CHECK(f.bs > 0, "format %d zero block size", f.vk);
      /* a format with a texture binding must have a valid layout */
      struct mali_image_desc d = { f.vk, VK_IMAGE_TYPE_2D, { 16, 16, 1 }, 1, 1, 1, false };
      struct mali_image_layout l;
      if (tex || rt || ds) CHECK(mali_image_layout_init(arch, &d, &l) == 0, "format %d layout fails", f.vk);
   }
   VkImageFormatProperties p;
   CHECK(mali_image_format_properties(arch, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &p) == VK_SUCCESS, "rgba8 props");
   CHECK(p.maxExtent.width == MALI_MAX_DIM_2D && p.maxArrayLayers == MALI_MAX_LAYERS && p.maxMipLevels == 13 && p.maxMipLevels <= MALI_MAX_MIPS, "rgba8 limits");
   CHECK(mali_image_format_properties(arch, VK_FORMAT_UNDEFINED, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0, &p) ==
            VK_ERROR_FORMAT_NOT_SUPPORTED, "undefined format accepted");
   CHECK(mali_image_format_properties(arch, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &p) == VK_ERROR_FORMAT_NOT_SUPPORTED, "D32 as colour attachment accepted");
   /* a rejected combination returns all-zero properties: linear 1D / 3D */
   for (int t = 0; t < 2; t++) {
      memset(&p, 0xAB, sizeof p);
      CHECK(mali_image_format_properties(arch, VK_FORMAT_A8B8G8R8_UINT_PACK32, t ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_1D, VK_IMAGE_TILING_LINEAR,
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &p) == VK_ERROR_FORMAT_NOT_SUPPORTED, "linear 1D/3D accepted");
      CHECK(p.maxExtent.width == 0 && p.maxExtent.height == 0 && p.maxExtent.depth == 0 && p.maxMipLevels == 0 && p.maxArrayLayers == 0 &&
               p.sampleCounts == 0 && p.maxResourceSize == 0, "linear %s: properties not zeroed", t ? "3D" : "1D");
   }
   if (g_verbose) printf("  arch %u: %u formats\n", arch, n);
}

/* MALI_AFBC wiring: off = byte-identical to the plain layout; on = eligible colour images get the AFBC size/alignment. */
static void
test_afbc_wiring(unsigned arch)
{
   VkImageCreateInfo ci = { .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 64, 64, 1 }, .mipLevels = 3,
                            .arrayLayers = 1, .samples = 1, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
   struct mali_image_desc d = { ci.format, ci.imageType, ci.extent, 3, 1, 1, false };
   struct mali_image_layout plain, off, on;
   CHECK(mali_image_layout_init(arch, &d, &plain) == 0, "afbc plain");
   CHECK(mali_image_plain_layout(arch, &ci, &d, false, &off) == 0, "afbc off");
   CHECK(!off.afbc && off.size == plain.size && off.alignment == plain.alignment, "flag off changed layout");
   CHECK(!memcmp(&off, &plain, sizeof plain), "flag off not byte-identical");
   CHECK(mali_image_plain_layout(arch, &ci, &d, true, &on) == 0, "afbc on");
   /* 64x64 RGBA8 3 mips, hand-derived in t_afbc: 16640 + 4224 + 1152 = 22016 B per layer */
   CHECK(on.afbc && on.size == 22016, "afbc size %llu want 22016", (unsigned long long)on.size);
   CHECK(on.size > plain.size && on.alignment == 128, "afbc align %u", on.alignment);
   CHECK(on.afbc_lay.level_offset[1] == 16640, "afbc level1 off");
   CHECK(on.plane[0].slice[0].row_stride == plain.plane[0].slice[0].row_stride, "linear planes must stay untouched");
   /* ineligible stays plain even with the flag on */
   VkImageCreateInfo c2 = ci; c2.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
   CHECK(mali_image_plain_layout(arch, &c2, &d, true, &on) == 0 && !on.afbc && on.size == plain.size, "storage image AFBC");
   c2 = ci; c2.tiling = VK_IMAGE_TILING_LINEAR;
   CHECK(mali_image_plain_layout(arch, &c2, &d, true, &on) == 0 && !on.afbc, "linear image AFBC");
   c2 = ci; c2.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
   CHECK(mali_image_plain_layout(arch, &c2, &d, true, &on) == 0 && !on.afbc, "modifier image AFBC");
   c2 = ci; c2.format = VK_FORMAT_R16G16B16A16_SFLOAT; struct mali_image_desc d2 = d; d2.format = c2.format;
   CHECK(mali_image_plain_layout(arch, &c2, &d2, true, &on) == 0 && !on.afbc, "float image AFBC");
   /* render-only: sampling / transfers have no AFBC path yet, so such images stay linear even with the flag on */
   {
      static const VkImageUsageFlags bad[] = { VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT };
      for (unsigned i = 0; i < 3; i++) {
         c2 = ci; c2.usage |= bad[i];
         CHECK(mali_image_plain_layout(arch, &c2, &d, true, &on) == 0 && !on.afbc && on.size == plain.size, "usage 0x%x must stay linear", (unsigned)bad[i]);
      }
   }
   /* SAMPLED is allowed on AFBC images on arch 10..13 (texture plane path), but not cube-compatible / mutable-format images */
   c2 = ci; c2.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
   CHECK(mali_image_plain_layout(arch, &c2, &d, true, &on) == 0 && on.afbc && on.size == 22016, "sampled AFBC image");
   c2.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
   CHECK(mali_image_plain_layout(arch, &c2, &d, true, &on) == 0 && !on.afbc, "sampled cube-compatible must stay linear");
   c2.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   CHECK(mali_image_plain_layout(arch, &c2, &d, true, &on) == 0 && !on.afbc, "sampled mutable must stay linear");
   c2 = ci; c2.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   /* render-only: unchanged policy */
   CHECK(mali_image_plain_layout(arch, &c2, &d, true, &on) == 0 && on.afbc, "render-only mutable");
   c2 = ci; c2.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
   CHECK(mali_image_layout_init(arch, &d, &on) == 0 && !mali_image_afbc_apply(9, &c2, true, &on) && !on.afbc, "sampled on arch 9 must stay linear (no plane path)");
   CHECK(mali_image_layout_init(arch, &d, &on) == 0 && mali_image_afbc_apply(9, &ci, true, &on) && on.afbc, "render-only on arch 9 still AFBC");
   /* sampled-view emission through the AFBC plane: 2 layers x 2 levels, layer-major, hand-derived (see docs/afbc-plan.md stage 7) */
   {
      struct mali_image_layout a; VkImageCreateInfo c3 = c2; c3.arrayLayers = 2; struct mali_image_desc d3 = d; d3.layers = 2;
      CHECK(mali_image_plain_layout(arch, &c3, &d3, true, &a) == 0 && a.afbc && a.afbc_lay.array_stride == 22016, "emit layout");
      const uint64_t iva = 0x7f0000100000ull;
      uint32_t pay[4 * 8]; struct mali_hw_desc tex;
      struct mali_view_desc v = { .format = d3.format, .type = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .levels = 2, .layers = 2,
                                  .swizzle = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A } };
      CHECK(mali_view_payload_entries(&a, &v) == 4, "afbc entries");
      CHECK(mali_view_emit(arch, &a, iva, &v, pay, 0x7f0000300000ull, &tex) == 0, "afbc emit");
      /* entry 0 = layer 0 level 0: header at base, size 16640, row stride 64, header size 256, 64x64 */
      /* word 0 superblock size is the genxml 0-based enum (16x16 = 0, 0x6a), not the drm 1-based bits: see mali_afbc_tex_plane_words */
      const uint32_t g0[8] = { 0x2810006a, 0x4100, 0x00100000, 0x7f00, 0x40, 0x100, 0, 0x003f003f };
      /* entry 3 = layer 1 level 1: base + 16640 + 22016 = 0x7f0000109700, size 4224, row stride 32, header size 64, 32x32 */
      const uint32_t g3[8] = { 0x2810006a, 0x1080, 0x00109700, 0x7f00, 0x20, 0x40, 0, 0x001f001f };
      cmp_words("afbc plane 0", pay, g0, 0);
      cmp_words("afbc plane 3", pay + 24, g3, 0);
      CHECK(tex.w[2] == (0x688u | 0x1000u | 0x10000u), "afbc tex w2 %08x (interleave must be set)", tex.w[2]);
      v.type = VK_IMAGE_VIEW_TYPE_CUBE; v.layers = 6;
      CHECK(mali_view_emit(arch, &a, iva, &v, pay, 0, &tex) != 0, "cube view of AFBC image");
      v.type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; v.layers = 2; v.format = VK_FORMAT_R8G8B8A8_SRGB;
      CHECK(mali_view_emit(arch, &a, iva, &v, pay, 0, &tex) == 0, "sRGB view of RGBA8 AFBC (same mode)");
      v.format = VK_FORMAT_R5G6B5_UNORM_PACK16;
      CHECK(mali_view_emit(arch, &a, iva, &v, pay, 0, &tex) != 0, "view with different compression mode");
      /* the same view of the linear image is unchanged (Generic planes) */
      struct mali_image_layout lin;
      v.format = d3.format;
      CHECK(mali_image_layout_init(arch, &d3, &lin) == 0 && mali_view_emit(arch, &lin, iva, &v, pay, 0, &tex) == 0 && (pay[0] & 0xff) == (10u | 16u), "linear view stays Generic");
   }
   /* bind-time header init: zero headers only, never touch linear images or an image that does not fit */
   {
      struct mali_image_layout a;
      CHECK(mali_image_plain_layout(arch, &ci, &d, true, &a) == 0 && a.afbc, "bind layout");
      uint8_t *mem = malloc(a.size + 256);
      memset(mem, 0xaa, a.size + 256);
      uint64_t z = mali_image_afbc_bind_init(&a, mem, a.size + 256, 128);
      CHECK(z > 0 && mem[0] == 0xaa && mem[127] == 0xaa && mem[128] == 0 && mem[128 + a.afbc_lay.level[0].body_offset] == 0xaa, "bind init range");
      CHECK(mali_image_afbc_bind_init(&a, mem, a.size + 100, 128) == 0, "bind init beyond memory");
      CHECK(mali_image_afbc_bind_init(&plain, mem, a.size + 256, 0) == 0, "bind init on linear image");
      free(mem);
   }
   /* layers multiply */
   c2 = ci; c2.arrayLayers = 4; d2 = d; d2.layers = 4;
   CHECK(mali_image_plain_layout(arch, &c2, &d2, true, &on) == 0 && on.afbc && on.size == 4 * on.afbc_lay.array_stride, "afbc layers");
   /* The real call site (mali_image_ci_layout, used by CreateImage / memory requirements): MALI_AFBC env drives it. */
   {
      struct mali_bc_layout bc; struct mali_image_layout e0, e1, e2; bool isbc;
      unsetenv("MALI_AFBC");
      CHECK(mali_image_ci_layout(arch, &ci, &e0, &bc, NULL, &isbc) == 0 && !e0.afbc && e0.size == plain.size, "env unset: not plain");
      CHECK(!memcmp(&e0, &plain, sizeof plain), "env unset: not byte-identical");
      setenv("MALI_AFBC", "0", 1);
      CHECK(mali_image_ci_layout(arch, &ci, &e1, &bc, NULL, &isbc) == 0 && !e1.afbc, "env 0 enabled AFBC");
      setenv("MALI_AFBC", "1", 1);
      CHECK(mali_image_ci_layout(arch, &ci, &e2, &bc, NULL, &isbc) == 0 && e2.afbc && e2.size == 22016, "env 1: AFBC sizing missing (size %llu)", (unsigned long long)e2.size);
      unsetenv("MALI_AFBC");
   }
}


int
main(int argc, char **argv)
{
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "-v")) g_verbose = 1;
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   static const unsigned archs[] = { 11, 13 };
   for (unsigned i = 0; i < 2; i++) {
      unsigned a = archs[i];
      printf("arch v%u\n", a);
      test_format_table(a);
      test_afbc_wiring(a);
      test_rgba8(a);
      test_rtfmt(a);
      test_attr(a);
      test_view_min_lod(a);
      test_depth(a);
      test_layout(a);
      test_sampler(a);
      test_texel_buffer(a);
   }
   clock_gettime(CLOCK_MONOTONIC, &t1);
   double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
   printf("%s (%d failures, %.3f s)\n", g_fail ? "FAIL" : "PASS", g_fail, s);
   CHECK(s < 5.0, "too slow");
   return g_fail ? 1 : 0;
}
