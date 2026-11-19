/* SPDX-License-Identifier: MIT */
/* mali_render.c on the host (no Mesa, no backend): clear words per tile-buffer format (every internal-format arm of
 * mali_rtfmt_clear_words, sRGB, integer RAW packing, NaN/clamp), mali_unorm8, and mali_render_pack_fbd's frame -> kb_fb_params mapping
 * (oracle: kb_fb_pack with hand-written params; v13 word 11 patch). Usage: t_render */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <vulkan/vulkan_core.h>

#include "kbase_fb.h"
#include "../src/mali_cs_emit.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void
cw(int fmt, const float c[4], const uint32_t want[4], const char *what)
{
   bool ok = false;
   const struct mali_rtfmt *rt = mali_rtfmt_lookup(fmt, &ok);
   uint32_t w[4] = { 0xdead, 0xdead, 0xdead, 0xdead };
   CHECK(ok && rt, "%s: no row", what);
   if (!ok || !rt)
      return;
   mali_rtfmt_clear_words(rt, c, w);
   CHECK(!memcmp(w, want, 16), "%s: %08x %08x %08x %08x want %08x %08x %08x %08x", what, w[0], w[1], w[2], w[3], want[0], want[1], want[2], want[3]);
}
#define CW(fmt, c, a, b, d, e, what) do { const uint32_t _w[4] = { a, b, d, e }; cw(fmt, c, _w, what); } while (0)
#define SAME(fmt, c, v, what) CW(fmt, c, v, v, v, v, what)
#define PAIR(fmt, c, a, b, what) CW(fmt, c, a, b, a, b, what)

static void
test_clear(void)
{
   const float c[4] = { 0.5f, 0.25f, 0.75f, 1.0f };
   const float neg[4] = { -1.0f, 1.0f, 0.5f, NAN };
   uint32_t u[4];
   float f[4];

   /* default 8-bit arm: round-half-away, sRGB transfer on RGB only (alpha stays linear) */
   PAIR(VK_FORMAT_B8G8R8A8_UNORM, c, 0xffbf4080, 0xffbf4080, "BGRA8 unorm");
   PAIR(VK_FORMAT_R8G8B8A8_SRGB, c, 0xffe189bc, 0xffe189bc, "RGBA8 srgb");
   {  /* the linear segment (c <= 0.0031308): 12.92 * 0.001 * 255 = 3.3 */
      const float lo[4] = { 0.001f, 0.0f, 1.0f, 0.001f };
      PAIR(VK_FORMAT_R8G8B8A8_SRGB, lo, 0x00ff0003, 0x00ff0003, "RGBA8 srgb linear segment");
   }
   /* packed internal formats */
   PAIR(VK_FORMAT_A2B10G10R10_UNORM_PACK32, c, 0xeff40200, 0xeff40200, "A2B10G10R10");
   PAIR(VK_FORMAT_R4G4B4A4_UNORM_PACK16, c, 0xf0b04080, 0xf0b04080, "R4G4B4A4");
   PAIR(VK_FORMAT_R5G6B5_UNORM_PACK16, c, 0x2e040200, 0x2e040200, "R5G6B5");
   PAIR(VK_FORMAT_R5G5B5A1_UNORM_PACK16, c, 0xae040200, 0xae040200, "R5G5B5A1");
   /* float internal formats (half floats), 16/17/18 */
   PAIR(VK_FORMAT_R16_SFLOAT, c, 0x38003800, 0x38003800, "R16F");
   PAIR(VK_FORMAT_R16G16_SFLOAT, c, 0x34003800, 0x34003800, "R16G16F");
   PAIR(VK_FORMAT_R16G16B16A16_SFLOAT, c, 0x34003800, 0x3c003a00, "R16G16B16A16F");
   PAIR(VK_FORMAT_B10G11R11_UFLOAT_PACK32, c, 0x741a0380, 0x741a0380, "B10G11R11 pos");
   PAIR(VK_FORMAT_B10G11R11_UFLOAT_PACK32, neg, 0x701e0000, 0x701e0000, "B10G11R11 negative clamps to 0");
   f[0] = 1e10f; f[1] = 1e-9f; f[2] = INFINITY; f[3] = NAN;   /* overflow -> inf, underflow -> 0 */
   PAIR(VK_FORMAT_R16G16B16A16_SFLOAT, f, 0x00007c00, 0x7e007c00, "half overflow/underflow/inf/nan");
   f[0] = 98304.0f; f[1] = -1e-20f; f[2] = 1e-20f; f[3] = -98304.0f;   /* just past half max: inf, not NaN; far-underflow -> signed 0 */
   PAIR(VK_FORMAT_R16G16B16A16_SFLOAT, f, 0x80007c00, 0xfc000000, "half x==31 and deep underflow");
   f[0] = -2.0f; f[1] = 0.0f; f[2] = 1.0f; f[3] = 0.0f;
   PAIR(VK_FORMAT_R16G16B16A16_SFLOAT, f, 0x0000c000, 0x00003c00, "half negative");

   /* integer colours: raw uint32 words masked to the channel width, packed and replicated by byte size (1, 2, 4, 8, 16) */
   u[0] = 0x1ab; u[1] = 2; u[2] = 3; u[3] = 4; memcpy(f, u, 16);
   SAME(VK_FORMAT_R8_UINT, f, 0xabababab, "R8_UINT 1 byte");
   SAME(VK_FORMAT_R8G8_UINT, f, 0x02ab02ab, "R8G8_UINT 2 bytes");
   SAME(VK_FORMAT_R8G8B8A8_UINT, f, 0x040302ab, "R8G8B8A8_UINT 4 bytes");
   u[0] = 0x12345; memcpy(f, u, 16);
   SAME(VK_FORMAT_R16_UINT, f, 0x23452345, "R16_UINT masks to 16 bits");
   SAME(VK_FORMAT_R16G16_UINT, f, 0x00022345, "R16G16_UINT");
   PAIR(VK_FORMAT_R16G16B16A16_UINT, f, 0x00022345, 0x00040003, "R16G16B16A16_UINT 8 bytes");
   u[0] = 0xdeadbeef; u[1] = 0x01020304; u[2] = 0x0a0b0c0d; u[3] = 0xf0f1f2f3; memcpy(f, u, 16);
   SAME(VK_FORMAT_R32_UINT, f, 0xdeadbeef, "R32_UINT");
   PAIR(VK_FORMAT_R32G32_SINT, f, 0xdeadbeef, 0x01020304, "R32G32_SINT 8 bytes");
   CW(VK_FORMAT_R32G32B32A32_UINT, f, 0xdeadbeef, 0x01020304, 0x0a0b0c0d, 0xf0f1f2f3, "R32G32B32A32_UINT 16 bytes");
   u[0] = 0xffffffff; u[1] = 2; u[2] = 0xfffffffe; u[3] = 0x80; memcpy(f, u, 16);
   SAME(VK_FORMAT_R8G8B8A8_SINT, f, 0x80fe02ff, "R8G8B8A8_SINT masks each channel");

   /* RAW special formats: NaN -> 0, clamp, round-half-even of the scaled value */
   f[0] = NAN; f[1] = 2.0f; f[2] = -2.0f; f[3] = 0.5f;
   SAME(VK_FORMAT_R16_UNORM, f, 0x00000000, "R16_UNORM NaN");
   PAIR(VK_FORMAT_R16G16B16A16_UNORM, f, 0xffff0000, 0x80000000, "R16G16B16A16_UNORM clamp");
   PAIR(VK_FORMAT_R16G16B16A16_SNORM, f, 0x7fff0000, 0x40008001, "R16G16B16A16_SNORM clamp");
   f[0] = -2.0f;
   SAME(VK_FORMAT_R16_SNORM, f, 0x80018001, "R16_SNORM below -1");
}

static void
test_unorm8(void)
{
   CHECK(mali_unorm8(NAN) == 0, "unorm8 NaN");
   CHECK(mali_unorm8(-1.0f) == 0, "unorm8 negative");
   CHECK(mali_unorm8(0.0f) == 0, "unorm8 0");
   CHECK(mali_unorm8(0.5f) == 128, "unorm8 0.5 = %u", mali_unorm8(0.5f));
   CHECK(mali_unorm8(0.998f) == 254, "unorm8 0.998 = %u", mali_unorm8(0.998f));
   CHECK(mali_unorm8(1.0f) == 255 && mali_unorm8(7.0f) == 255, "unorm8 >= 1");
}

#define W(o, i) (((uint32_t *)(o))[i])
static struct mali_frame
mkframe(void)
{
   struct mali_frame f;
   memset(&f, 0, sizeof f);
   f.width = 64; f.height = 32; f.has_colour = true;
   f.colour.va = 0x10000; f.colour.clear = true; f.colour.store = true;
   f.colour.clear_color[0] = 1.0f; f.colour.clear_color[2] = 0.5f; f.colour.clear_color[3] = 1.0f;
   return f;
}

static void
check_fbd(const char *what, const struct mali_env *env, const struct mali_frame *f, struct kb_fb_params *p, bool v13patch, unsigned tl, unsigned al)
{
   uint8_t got[KB_FB_DESC_SIZE_ZS] __attribute__((aligned(64))) = { 0 }, want[KB_FB_DESC_SIZE_ZS] __attribute__((aligned(64))) = { 0 };
   int rc = mali_render_pack_fbd(env, f, got, 0x5000, 0x6000);
   p->tiler_va = 0x5000;
   if (!p->sample_locations_va)
      p->sample_locations_va = 0x6000;   /* passed through unchanged when single-sampled */
   int rc2 = kb_fb_pack(want, p);
   CHECK(rc == 0 && rc2 == 0, "%s: rc %d / %d", what, rc, rc2);
   if (v13patch)
      W(want, 11) = (W(want, 11) & ~((0xfu << 9) | (0xffu << 24))) | (tl << 9) | (al << 24);
   CHECK(!memcmp(got, want, sizeof got), "%s: descriptor differs from kb_fb_pack oracle (first differing word %d)", what, ({
      int d = -1; for (unsigned i = 0; i < sizeof got / 4 && d < 0; i++) if (W(got, i) != W(want, i)) d = (int)i; d; }));
}

static void
test_fbd(void)
{
   struct mali_env e11 = { .arch = 11, .tile_log2 = 12 }, e13 = { .arch = 13, .tile_log2 = 12 };
   struct kb_fb_params p;
   struct mali_frame f = mkframe();
   uint8_t out[KB_FB_DESC_SIZE_ZS] __attribute__((aligned(64))), out13[KB_FB_DESC_SIZE_ZS] __attribute__((aligned(64)));
   bool ok;

   f.width = 0;
   CHECK(mali_render_pack_fbd(&e11, &f, out, 0, 0) == -EINVAL, "zero width must be -EINVAL");
   f.width = 64; f.height = 0;
   CHECK(mali_render_pack_fbd(&e11, &f, out, 0, 0) == -EINVAL, "zero height must be -EINVAL");
   f.height = 32; f.colour.va = 0x10001;
   CHECK(mali_render_pack_fbd(&e11, &f, out, 0, 0) != 0, "kb_fb_pack error must propagate (unaligned colour va)");
   f.colour.va = 0x10000;

   /* plain colour clear, no rt row: 8-bit clear channels, default row stride, v11 arch 0 */
   memset(&p, 0, sizeof p);
   p.width = 64; p.height = 32; p.rt_gpu_va = 0x10000; p.row_stride = 256;
   p.clear_r = 255; p.clear_g = 0; p.clear_b = 128; p.clear_a = 255;
   check_fbd("rgba8 clear v11", &e11, &f, &p, false, 0, 0);
   f.colour.row_stride = 512; p.row_stride = 512;
   check_fbd("explicit row stride", &e11, &f, &p, false, 0, 0);
   f.colour.row_stride = 0; p.row_stride = 256;
   /* v13 patches word 11 only; v11 words differ from v13 there */
   p.arch = 13;
   check_fbd("rgba8 v13 patch", &e13, &f, &p, true, 12, 16);
   CHECK(mali_render_pack_fbd(&e11, &f, out, 0, 0) == 0 && mali_render_pack_fbd(&e13, &f, out13, 0, 0) == 0 && W(out, 11) != W(out13, 11),
         "v13 must patch word 11");

   /* no clear: DONT_CARE leaves clear words 0 */
   f.colour.clear = false; p.arch = 0; p.clear_r = p.clear_g = p.clear_b = p.clear_a = 0;
   check_fbd("no clear", &e11, &f, &p, false, 0, 0);
   f.colour.clear = true;

   /* rt row: clear words in the tile buffer format, colour allocation from the row (8 B -> 32 KiB-units on v13) */
   const struct mali_rtfmt *rt = mali_rtfmt_lookup(VK_FORMAT_R16G16B16A16_SFLOAT, &ok);
   CHECK(ok && rt, "rt row");
   f.colour.rt = rt;
   memset(&p, 0, sizeof p);
   p.width = 64; p.height = 32; p.rt_gpu_va = 0x10000; p.row_stride = 256;
   p.rt_internal = rt->internal; p.rt_wb = rt->wb; p.rt_swizzle = rt->swizzle; p.rt_alloc = rt->alloc;
   mali_rtfmt_clear_words(rt, f.colour.clear_color, p.rt_clear);
   check_fbd("rt row v11", &e11, &f, &p, false, 0, 0);
   p.arch = 13;
   check_fbd("rt row v13 alloc 8", &e13, &f, &p, true, 12, 32);
   e13.tile_log2 = 10;
   check_fbd("rt row v13 tile 10", &e13, &f, &p, true, 10, 8);
   e13.tile_log2 = 0;   /* (1<<0)*8>>10 = 0 -> clamped to 1 */
   check_fbd("rt row v13 alloc clamp", &e13, &f, &p, true, 0, 1);
   e13.tile_log2 = 12;

   /* no colour attachment */
   f.colour.rt = NULL; f.has_colour = false;
   memset(&p, 0, sizeof p);
   p.width = 64; p.height = 32; p.rt_gpu_va = 0x10000; p.rt_none = 1; p.row_stride = 0;
   p.clear_r = 255; p.clear_b = 128; p.clear_a = 255;
   check_fbd("rt none", &e11, &f, &p, false, 0, 0);
   f.has_colour = true;

   /* depth + stencil: formats, strides, clear values, store flags */
   f.has_zs = true; f.zs.va = 0x20000; f.zs.row_stride = 256; f.zs.zs_format = KB_ZS_D32F; f.zs.clear = true; f.zs.clear_depth = 0.25f; f.zs.store = false;
   f.has_st = true; f.st.va = 0x30000; f.st.row_stride = 64; f.st.clear = true; f.st.clear_stencil = 0x5a; f.st.store = true;
   memset(&p, 0, sizeof p);
   p.width = 64; p.height = 32; p.rt_gpu_va = 0x10000; p.row_stride = 256;
   p.clear_r = 255; p.clear_b = 128; p.clear_a = 255;
   p.zs_format = KB_ZS_D32F; p.zs_gpu_va = 0x20000; p.zs_row_stride = 256; p.clear_depth = 0.25f; p.zs_no_store = 1;
   p.has_stencil = 1; p.s_gpu_va = 0x30000; p.s_row_stride = 64; p.clear_stencil = 0x5a; p.s_no_store = 0;
   check_fbd("zs + stencil", &e11, &f, &p, false, 0, 0);
   f.zs.clear = false; f.zs.store = true; f.st.clear = false; f.st.store = false;
   p.clear_depth = 0.0f; p.zs_no_store = 0; p.clear_stencil = 0; p.s_no_store = 1;
   check_fbd("zs + stencil no clear", &e11, &f, &p, false, 0, 0);
   f.has_zs = f.has_st = false;

   /* 4x MSAA: resolve target (Average) and layered store; v11 and v13 tile size /4 */
   f.samples = 4;
   memset(&p, 0, sizeof p);
   p.width = 64; p.height = 32; p.rt_gpu_va = 0x10000; p.row_stride = 256;
   p.clear_r = 255; p.clear_b = 128; p.clear_a = 255;
   p.samples_log2 = 2; p.sample_pattern = 2; p.tile_log2 = 8; p.sample_locations_va = 0x6000 + 2 * 256; p.zs_msaa = 3;
   p.rt_msaa = 3; p.surface_stride = 0x4000;
   f.colour.surface_stride = 0x4000;
   check_fbd("msaa layered", &e11, &f, &p, false, 0, 0);
   f.colour.resolve_va = 0x40000; f.colour.resolve_row_stride = 128;
   p.rt_gpu_va = 0x40000; p.row_stride = 128; p.rt_msaa = 1; p.surface_stride = 0;
   check_fbd("msaa resolve", &e11, &f, &p, false, 0, 0);
   p.arch = 13;
   check_fbd("msaa resolve v13 tile-2", &e13, &f, &p, true, 10, 16);
}

int main(void)
{
   test_clear();
   test_unorm8();
   test_fbd();
   printf("{\"test\":\"t_render\",\"pass\":%s}\n", g_fail ? "false" : "true");
   return g_fail ? 1 : 0;
}
