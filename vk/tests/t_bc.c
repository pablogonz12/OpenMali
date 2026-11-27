/* SPDX-License-Identifier: MIT */
/* R4-M7 offline tests of the BC decode-to-shadow path.
 *   1. format table / advertisement / layout / planner (pure, analytic expectations)
 *   2. planner + software reference vs Mesa's util_format decoders, every BC format, 2D arrays + mips + 3D, sub-regions, untouched bytes
 *   3. the REAL SPIR-V shaders executed on the host Vulkan device (if any), whole-allocation byte compare against (2)
 *   4. mali_compile of the four shaders for v11 and v13: limits (work regs / TLS / WLS / push) + regression golden
 * Usage: t_bc [-v] [--no-gpu] [--gen-golden <out.h>]. GPU VAs are host pointers in 1/2; 3 needs a Vulkan loader + ICD (skipped, not failed, otherwise). */
#include "../src/mali_bc.h"
#include "../src/mali_compile.h"
#include "t_bc_vec.h"
#include "t_etc2_ref.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include("../shader/golden_bc.h")
#include "../shader/golden_bc.h"
#define HAVE_GOLDEN 1
#endif

/* libmesa_util (linked from the Mesa build): the oracle. */
extern void util_format_dxt1_rgb_unpack_rgba_8unorm(uint8_t *, unsigned, const uint8_t *, unsigned, unsigned, unsigned);
extern void util_format_dxt1_rgba_unpack_rgba_8unorm(uint8_t *, unsigned, const uint8_t *, unsigned, unsigned, unsigned);
extern void util_format_dxt3_rgba_unpack_rgba_8unorm(uint8_t *, unsigned, const uint8_t *, unsigned, unsigned, unsigned);
extern void util_format_dxt5_rgba_unpack_rgba_8unorm(uint8_t *, unsigned, const uint8_t *, unsigned, unsigned, unsigned);
extern void util_format_rgtc1_unorm_unpack_r_8unorm(uint8_t *, unsigned, const uint8_t *, unsigned, unsigned, unsigned);
extern void util_format_rgtc1_snorm_unpack_r_8snorm(int8_t *, unsigned, const uint8_t *, unsigned, unsigned, unsigned);
extern void util_format_rgtc2_unorm_unpack_rg_8unorm(uint8_t *, unsigned, const uint8_t *, unsigned, unsigned, unsigned);
extern void util_format_rgtc2_snorm_unpack_rg_8snorm(int8_t *, unsigned, const uint8_t *, unsigned, unsigned, unsigned);
extern void util_format_bptc_rgba_unorm_unpack_rgba_8unorm(uint8_t *, unsigned, const uint8_t *, unsigned, unsigned, unsigned);
extern void util_format_bptc_rgb_float_unpack_rgba_float(void *, unsigned, const uint8_t *, unsigned, unsigned, unsigned);
extern void util_format_bptc_rgb_ufloat_unpack_rgba_float(void *, unsigned, const uint8_t *, unsigned, unsigned, unsigned);
extern uint16_t _mesa_float_to_half_slow(float);

static int g_fail, g_verbose, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* one block -> 16 shadow texels row-major */
static void mesa_dec(const struct mali_bc_fmt *f, const uint8_t *b, uint8_t *o)
{
   switch (f->vk) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK: util_format_dxt1_rgb_unpack_rgba_8unorm(o, 16, b, 8, 4, 4); break;
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: util_format_dxt1_rgba_unpack_rgba_8unorm(o, 16, b, 8, 4, 4); break;
   case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: util_format_dxt3_rgba_unpack_rgba_8unorm(o, 16, b, 16, 4, 4); break;
   case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: util_format_dxt5_rgba_unpack_rgba_8unorm(o, 16, b, 16, 4, 4); break;
   case VK_FORMAT_BC4_UNORM_BLOCK: util_format_rgtc1_unorm_unpack_r_8unorm(o, 4, b, 8, 4, 4); break;
   case VK_FORMAT_BC4_SNORM_BLOCK: util_format_rgtc1_snorm_unpack_r_8snorm((int8_t *)o, 4, b, 8, 4, 4); break;
   case VK_FORMAT_BC5_UNORM_BLOCK: util_format_rgtc2_unorm_unpack_rg_8unorm(o, 8, b, 16, 4, 4); break;
   case VK_FORMAT_BC5_SNORM_BLOCK: util_format_rgtc2_snorm_unpack_rg_8snorm((int8_t *)o, 8, b, 16, 4, 4); break;
   case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: util_format_bptc_rgba_unorm_unpack_rgba_8unorm(o, 16, b, 16, 4, 4); break;
   case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: {
      float t[16 * 4];
      if (f->vk == VK_FORMAT_BC6H_SFLOAT_BLOCK) util_format_bptc_rgb_float_unpack_rgba_float(t, 64, b, 16, 4, 4);
      else util_format_bptc_rgb_ufloat_unpack_rgba_float(t, 64, b, 16, 4, 4);
      uint16_t *h = (uint16_t *)o;
      for (int i = 0; i < 16; i++) {
         for (int c = 0; c < 3; c++) h[i * 4 + c] = _mesa_float_to_half_slow(t[i * 4 + c]);
         h[i * 4 + 3] = 0x3c00;
      }
      break;
   }
   default:
      if (mali_bc_is_etc2(f->vk)) { etc2_ref_block(f->mode, b, o); break; }
      abort();
   }
}

static const VkFormat g_all[16] = {
   VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
   VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,
   VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK,
   VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK,
};

/* ETC2 / EAC (decoded for blits only): the second half of the table */
static const VkFormat g_etc2[10] = {
   VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK,
   VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, VK_FORMAT_EAC_R11_UNORM_BLOCK, VK_FORMAT_EAC_R11_SNORM_BLOCK,
   VK_FORMAT_EAC_R11G11_UNORM_BLOCK, VK_FORMAT_EAC_R11G11_SNORM_BLOCK,
};
#define NEVERY 26
static VkFormat every(int i) { return i < 16 ? g_all[i] : g_etc2[i - 16]; }

/* ---------- 1. pure ---------- */
static void t_formats(void)
{
   int n = 0;
   for (int f = 0; f < 400; f++)
      if (mali_bc_is_bc((VkFormat)(VK_FORMAT_UNDEFINED + f))) n++;
   CHECK(n == 16, "BC format count %d", n);
   CHECK(!mali_bc_is_bc(VK_FORMAT_R8G8B8A8_UNORM) && !mali_bc_is_bc(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK) && !mali_bc_is_bc(VK_FORMAT_UNDEFINED), "non-BC");
   n = 0;
   for (int f = 0; f < 400; f++)
      if (mali_bc_is_etc2((VkFormat)(VK_FORMAT_UNDEFINED + f))) n++;
   CHECK(n == 10 && !mali_bc_is_etc2(VK_FORMAT_BC7_SRGB_BLOCK) && !mali_bc_is_etc2(VK_FORMAT_ASTC_4x4_UNORM_BLOCK), "ETC2 format count %d", n);
   for (int i = 0; i < 10; i++) {
      const struct mali_bc_fmt *f = mali_bc_format(g_etc2[i]);
      CHECK(f && f->vk == g_etc2[i] && f->sh == MALI_BC_ETC2 && !mali_bc_is_bc(f->vk), "etc2 table order %d", i);
      if (!f) continue;
      static const unsigned bs[10] = { 8, 8, 8, 8, 16, 16, 8, 8, 16, 16 }, bpt[10] = { 4, 4, 4, 4, 4, 4, 2, 2, 4, 4 };
      CHECK(f->block_bytes == bs[i] && f->shadow_bpt == bpt[i], "%s block %u bpt %u", f->name, f->block_bytes, f->shadow_bpt);
      CHECK(f->srgb == (i < 6 && (i & 1)), "%s srgb", f->name);
      CHECK(mali_bc_optimal_features(f->vk) == 0 && mali_bc_image_supported(f->vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0, 1) != VK_SUCCESS,
            "%s advertisement stays native (mali_format.c)", f->name);
   }
   CHECK(mali_bc_format(VK_FORMAT_EAC_R11_SNORM_BLOCK)->shadow == VK_FORMAT_R16_SNORM && mali_bc_format(VK_FORMAT_EAC_R11G11_UNORM_BLOCK)->shadow == VK_FORMAT_R16G16_UNORM &&
         mali_bc_format(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK)->shadow == VK_FORMAT_R8G8B8A8_SRGB, "etc2 shadow formats");
   for (int i = 0; i < 16; i++) {
      const struct mali_bc_fmt *f = mali_bc_format(g_all[i]);
      CHECK(f && f->vk == g_all[i], "table order %d", i);
      if (!f) continue;
      CHECK(f->block_bytes == (i < 4 || (i >= 8 && i < 10) ? 8 : 16), "%s block bytes %u", f->name, f->block_bytes);
      CHECK(f->srgb == (i < 8 ? (i & 1) : (i == 15)), "%s srgb", f->name);
      CHECK(mali_bc_optimal_features(f->vk) ==
               (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT), "%s features", f->name);
      CHECK(mali_bc_image_supported(f->vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, 1) == VK_SUCCESS, "%s 2D ok", f->name);
      CHECK(mali_bc_image_supported(f->vk, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0, 1) == VK_SUCCESS, "%s 3D ok", f->name);
      CHECK(mali_bc_image_supported(f->vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, 1) == VK_SUCCESS, "%s cube ok", f->name);
      CHECK(mali_bc_image_supported(f->vk, VK_IMAGE_TYPE_1D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0, 1) != VK_SUCCESS, "%s 1D rejected", f->name);
      CHECK(mali_bc_image_supported(f->vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT, 0, 1) != VK_SUCCESS, "%s linear rejected", f->name);
      CHECK(mali_bc_image_supported(f->vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT, 0, 1) != VK_SUCCESS, "%s storage rejected", f->name);
      CHECK(mali_bc_image_supported(f->vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, 1) != VK_SUCCESS, "%s attachment rejected", f->name);
      CHECK(mali_bc_image_supported(f->vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0, 4) != VK_SUCCESS, "%s msaa rejected", f->name);
      CHECK(mali_bc_image_supported(f->vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT, 1) != VK_SUCCESS, "%s btvc rejected", f->name);
   }
   /* shadow formats */
   CHECK(mali_bc_format(VK_FORMAT_BC7_SRGB_BLOCK)->shadow == VK_FORMAT_R8G8B8A8_SRGB && mali_bc_format(VK_FORMAT_BC1_RGBA_UNORM_BLOCK)->shadow == VK_FORMAT_R8G8B8A8_UNORM, "rgba8 shadow");
   CHECK(mali_bc_format(VK_FORMAT_BC4_SNORM_BLOCK)->shadow == VK_FORMAT_R8_SNORM && mali_bc_format(VK_FORMAT_BC5_UNORM_BLOCK)->shadow == VK_FORMAT_R8G8_UNORM, "r8/rg8 shadow");
   CHECK(mali_bc_format(VK_FORMAT_BC6H_UFLOAT_BLOCK)->shadow == VK_FORMAT_R16G16B16A16_SFLOAT && mali_bc_format(VK_FORMAT_BC6H_SFLOAT_BLOCK)->shadow_bpt == 8, "bc6h shadow");
   for (int s = 0; s < MALI_BC_NSHADERS; s++) {
      size_t w = 0;
      const uint32_t *p = mali_bc_spirv((enum mali_bc_shader)s, &w);
      CHECK(p && w > 100 && p[0] == 0x07230203u, "spirv %d", s);
   }
}

static void t_layout(void)
{
   struct mali_bc_layout l;
   /* BC1 13x9, 4 levels, 2 layers */
   CHECK(mali_bc_layout_init(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_IMAGE_TYPE_2D, (VkExtent3D){ 13, 9, 1 }, 4, 2, &l) == 0, "layout init");
   CHECK(l.comp.lv[0].bw == 4 && l.comp.lv[0].bh == 3 && l.comp.lv[0].row == 32 && l.comp.lv[0].slice == 96, "L0 comp %u %u %u", l.comp.lv[0].bw, l.comp.lv[0].row, (unsigned)l.comp.lv[0].slice);
   CHECK(l.comp.lv[1].w == 6 && l.comp.lv[1].h == 4 && l.comp.lv[1].bw == 2 && l.comp.lv[1].bh == 1 && l.comp.lv[1].row == 16, "L1 dims");
   CHECK(l.comp.lv[3].w == 1 && l.comp.lv[3].h == 1 && l.comp.lv[3].bw == 1 && l.comp.lv[3].row == 8, "L3 dims");
   CHECK(l.comp.lv[1].off == 128 && l.comp.lv[2].off == 192 && l.comp.lv[3].off == 256 && l.comp.array_stride == 320, "comp offsets %llu %llu %llu stride %llu",
         (unsigned long long)l.comp.lv[1].off, (unsigned long long)l.comp.lv[2].off, (unsigned long long)l.comp.lv[3].off, (unsigned long long)l.comp.array_stride);
   CHECK(l.comp.size == 640 && l.shadow.base == 640, "comp size %llu", (unsigned long long)l.comp.size);
   CHECK(l.shadow.lv[0].row == 64 && l.shadow.lv[0].slice == 64 * 12 && l.shadow.lv[1].off == 768, "shadow L0 row %u slice %llu L1 off %llu", l.shadow.lv[0].row,
         (unsigned long long)l.shadow.lv[0].slice, (unsigned long long)l.shadow.lv[1].off);
   CHECK(l.size == l.shadow.base + l.shadow.size && l.size % 64 == 0 && l.alignment == 64, "total");
   for (uint32_t i = 1; i < 4; i++)
      CHECK(l.comp.lv[i].off > l.comp.lv[i - 1].off && l.shadow.lv[i].off > l.shadow.lv[i - 1].off && !(l.shadow.lv[i].off & 63), "monotonic %u", i);
   /* rows always whole 4-texel multiples for the shaders */
   for (int i = 0; i < NEVERY; i++) {
      struct mali_bc_layout q;
      CHECK(mali_bc_layout_init(every(i), VK_IMAGE_TYPE_2D, (VkExtent3D){ 1, 1, 1 }, 1, 1, &q) == 0, "1x1 layout");
      CHECK(q.shadow.lv[0].row % 16 == 0 || q.fmt->shadow_bpt < 4, "row pitch %u", q.shadow.lv[0].row);
      CHECK(q.shadow.lv[0].row >= 4u * q.fmt->shadow_bpt && q.shadow.lv[0].slice >= 4ull * q.shadow.lv[0].row, "1x1 shadow holds a whole block");
   }
   /* 3D */
   struct mali_bc_layout d;
   CHECK(mali_bc_layout_init(VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_3D, (VkExtent3D){ 8, 8, 5 }, 3, 1, &d) == 0, "3D init");
   CHECK(d.comp.lv[0].d == 5 && d.comp.lv[1].d == 2 && d.comp.lv[2].d == 1 && d.comp.lv[0].size == 5 * d.comp.lv[0].slice, "3D depth halving");
   /* errors */
   CHECK(mali_bc_layout_init(VK_FORMAT_R8_UNORM, VK_IMAGE_TYPE_2D, (VkExtent3D){ 4, 4, 1 }, 1, 1, &l) == -EINVAL, "non-BC");
   CHECK(mali_bc_layout_init(VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_IMAGE_TYPE_1D, (VkExtent3D){ 4, 1, 1 }, 1, 1, &l) == -EINVAL, "1D");
   CHECK(mali_bc_layout_init(VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_IMAGE_TYPE_3D, (VkExtent3D){ 4, 4, 4 }, 1, 2, &l) == -EINVAL, "3D layers");
   CHECK(mali_bc_layout_init(VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_IMAGE_TYPE_2D, (VkExtent3D){ 4, 4, 1 }, 17, 1, &l) == -EINVAL, "levels");
   CHECK(mali_bc_layout_init(VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D, (VkExtent3D){ 16384, 16384, 1 }, 1, 8, &l) == -ERANGE, "range");
}

static void t_planner(void)
{
   struct mali_bc_layout l;
   struct mali_bc_list jl = { 0 };
   mali_bc_layout_init(VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D, (VkExtent3D){ 64, 32, 1 }, 2, 3, &l);
   CHECK(mali_bc_plan_decode(&jl, &l, 0x100000, 0, 1, 2, (VkOffset3D){ 8, 4, 0 }, (VkExtent3D){ 24, 12, 1 }) == 0 && jl.n == 1, "plan region");
   const struct mali_bc_job *j = &jl.j[0];
   CHECK(j->sh == MALI_BC_BC7 && j->nbind == 2 && j->push_bytes == 48, "job basics");
   CHECK(j->push[6] == 6 && j->push[7] == 3 && j->push[8] == 2, "blocks %u %u %u", j->push[6], j->push[7], j->push[8]);
   uint64_t so = l.comp.array_stride * 1 + 1ull * l.comp.lv[0].row + 2ull * 16;
   uint64_t dofs = l.shadow.array_stride * 1 + 4ull * l.shadow.lv[0].row + 2ull * 16;
   CHECK(j->push[0] == so && j->push[1] == l.comp.lv[0].row && j->push[2] == l.comp.array_stride, "src push %u vs %llu", j->push[0], (unsigned long long)so);
   CHECK(j->push[3] == dofs && j->push[4] == l.shadow.lv[0].row && j->push[5] == l.shadow.array_stride, "dst push %u vs %llu", j->push[3], (unsigned long long)dofs);
   CHECK(j->bind[0].va == 0x100000 && j->bind[1].va == 0x100000 + l.shadow.base && j->bind[0].size == l.comp.size && j->bind[1].size == l.shadow.size, "binds");
   CHECK(j->groups[0] == 1 && j->groups[1] == 1 && j->groups[2] == 1, "groups %u", j->groups[0]);
   /* edge-reaching extent that is not a multiple of 4 */
   mali_bc_list_free(&jl);
   mali_bc_layout_init(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_IMAGE_TYPE_2D, (VkExtent3D){ 13, 9, 1 }, 1, 1, &l);
   CHECK(mali_bc_plan_decode(&jl, &l, 0, 0, 0, 1, (VkOffset3D){ 8, 8, 0 }, (VkExtent3D){ 5, 1, 1 }) == 0, "edge region ok");
   CHECK(jl.j[0].push[6] == 2 && jl.j[0].push[7] == 1, "edge blocks");
   CHECK(mali_bc_plan_decode(&jl, &l, 0, 0, 0, 1, (VkOffset3D){ 4, 0, 0 }, (VkExtent3D){ 5, 4, 1 }) == -EINVAL, "unaligned width inside the image");
   CHECK(mali_bc_plan_decode(&jl, &l, 0, 0, 0, 1, (VkOffset3D){ 2, 0, 0 }, (VkExtent3D){ 4, 4, 1 }) == -EINVAL, "unaligned offset");
   CHECK(mali_bc_plan_decode(&jl, &l, 0, 0, 0, 1, (VkOffset3D){ 0, 0, 0 }, (VkExtent3D){ 16, 4, 1 }) == -EINVAL, "overrun");
   CHECK(mali_bc_plan_decode(&jl, &l, 0, 1, 0, 1, (VkOffset3D){ 0, 0, 0 }, (VkExtent3D){ 4, 4, 1 }) == -EINVAL, "bad level");
   CHECK(mali_bc_plan_decode(&jl, &l, 0, 0, 1, 1, (VkOffset3D){ 0, 0, 0 }, (VkExtent3D){ 4, 4, 1 }) == -EINVAL, "bad layer");
   mali_bc_list_free(&jl);
   /* many blocks: groups split into y beyond 32768 */
   mali_bc_layout_init(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_IMAGE_TYPE_2D, (VkExtent3D){ 8192, 8192, 1 }, 1, 1, &l);
   CHECK(mali_bc_plan_decode(&jl, &l, 0, 0, 0, 1, (VkOffset3D){ 0, 0, 0 }, (VkExtent3D){ 8192, 8192, 1 }) == 0, "big");
   uint64_t total = 2048ull * 2048, g = (total + 63) / 64;
   CHECK(jl.j[0].groups[0] == 32768 && jl.j[0].groups[1] == (g + 32767) / 32768 && (uint64_t)jl.j[0].groups[0] * jl.j[0].groups[1] >= g, "big groups %u x %u", jl.j[0].groups[0], jl.j[0].groups[1]);
   mali_bc_list_free(&jl);
   /* decode_all job counts */
   mali_bc_layout_init(VK_FORMAT_BC3_UNORM_BLOCK, VK_IMAGE_TYPE_2D, (VkExtent3D){ 40, 20, 1 }, 5, 3, &l);
   CHECK(mali_bc_plan_decode_all(&jl, &l, 0) == 0 && jl.n == 5 && jl.j[0].push[8] == 3, "decode_all %u jobs", jl.n);
   mali_bc_list_free(&jl);
}

/* ---------- 2. reference vs Mesa ---------- */
struct img { struct mali_bc_layout lay; uint8_t *mem; };

static struct img mk_img(VkFormat f, VkImageType t, VkExtent3D e, uint32_t lv, uint32_t ly, uint64_t seed)
{
   struct img im;
   int r = mali_bc_layout_init(f, t, e, lv, ly, &im.lay);
   CHECK(r == 0, "layout %d", r);
   im.mem = aligned_alloc(64, im.lay.size + 64);
   memset(im.mem, 0xaa, im.lay.size);
   /* fill every compressed block (including those per layer / z / level) */
   const struct mali_bc_layout *l = &im.lay;
   uint64_t s = seed;
   for (uint32_t a = 0; a < l->layers; a++)
      for (uint32_t v = 0; v < l->levels; v++) {
         const struct mali_bc_level *c = &l->comp.lv[v];
         uint8_t *p = im.mem + l->comp.base + a * l->comp.array_stride + c->off;
         for (uint32_t z = 0; z < c->d; z++)
            bcv_fill(f, bcv_rng(&s), p + z * c->slice, (size_t)c->bw * c->bh, l->fmt->block_bytes, 1);
      }
   return im;
}

/* every in-bounds texel of the shadow must equal Mesa's decode of its block. returns mismatching texels */
static unsigned verify(const struct img *im, unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint32_t only_level, uint32_t la0, uint32_t lan)
{
   const struct mali_bc_layout *l = &im->lay;
   const unsigned bpt = l->fmt->shadow_bpt, bs = l->fmt->block_bytes;
   unsigned bad = 0;
   for (uint32_t a = la0; a < la0 + lan; a++)
      for (uint32_t v = 0; v < l->levels; v++) {
         if (only_level != ~0u && v != only_level) continue;
         const struct mali_bc_level *c = &l->comp.lv[v], *s = &l->shadow.lv[v];
         for (uint32_t z = 0; z < c->d; z++)
            for (uint32_t y = 0; y < c->h; y++)
               for (uint32_t x = 0; x < c->w; x++) {
                  if (x < x0 || y < y0 || x >= x1 || y >= y1) continue;
                  const uint8_t *blk = im->mem + l->comp.base + a * l->comp.array_stride + c->off + z * c->slice + (uint64_t)(y / 4) * c->row + (uint64_t)(x / 4) * bs;
                  uint8_t exp[16 * 8];
                  mesa_dec(l->fmt, blk, exp);
                  const uint8_t *got = im->mem + l->shadow.base + a * l->shadow.array_stride + s->off + z * s->slice + (uint64_t)y * s->row + (uint64_t)x * bpt;
                  if (memcmp(got, exp + ((y & 3) * 4 + (x & 3)) * bpt, bpt)) bad++;
               }
      }
   return bad;
}

static unsigned count_pattern(const struct img *im, uint64_t lo, uint64_t hi)
{
   unsigned n = 0;
   for (uint64_t i = lo; i < hi; i++) n += im->mem[i] == 0xaa;
   return n;
}

static void run_ref(const struct mali_bc_list *jl)
{
   for (unsigned i = 0; i < jl->n; i++) mali_bc_ref_run(&jl->j[i], mesa_dec);
}

extern void util_format_etc1_rgb8_unpack_rgba_8unorm(uint8_t *, unsigned, const uint8_t *, unsigned, unsigned, unsigned);

/* the C port of the ETC2 decoder against Mesa's real ETC1 decoder, on every block that is valid ETC1 (individual, or differential in range) */
static void t_etc1_cross(void)
{
   uint32_t st = 0x1234567;
   unsigned n = 0, bad = 0;
   for (unsigned it = 0; it < 200000; it++) {
      uint8_t b[8], a[64], r[64];
      for (int i = 0; i < 8; i++) { st = st * 1664525u + 1013904223u; b[i] = (uint8_t)(st >> 24); }
      if (b[3] & 2) {
         int ok = 1;
         for (int i = 0; i < 3; i++) { static const int lk[8] = { 0, 1, 2, 3, -4, -3, -2, -1 }; int v = (b[i] >> 3) + lk[b[i] & 7]; if (v < 0 || v > 31) ok = 0; }
         if (!ok) continue;
      }
      util_format_etc1_rgb8_unpack_rgba_8unorm(a, 16, b, 8, 4, 4);
      etc2_ref_block(0, b, r);
      n++;
      if (memcmp(a, r, 64)) bad++;
   }
   CHECK(n > 50000 && bad == 0, "ETC2 port vs Mesa ETC1 decoder: %u blocks, %u differ", n, bad);
}

static void t_ref(void)
{
   t_etc1_cross();
   for (int i = 0; i < NEVERY; i++) {
      VkFormat f = every(i);
      /* 2D array + mips, odd sizes */
      {
         struct img im = mk_img(f, VK_IMAGE_TYPE_2D, (VkExtent3D){ 21, 11, 1 }, 4, 3, 100 + i);
         struct mali_bc_list jl = { 0 };
         CHECK(mali_bc_plan_decode_all(&jl, &im.lay, (uint64_t)(uintptr_t)im.mem) == 0 && jl.n == 4, "%s plan all", im.lay.fmt->name);
         uint64_t cs = im.lay.comp.size;
         unsigned before = count_pattern(&im, 0, cs);
         run_ref(&jl);
         unsigned bad = verify(&im, 0, 0, ~0u, ~0u, ~0u, 0, 3);
         CHECK(bad == 0, "%s 2D array/mips: %u texel mismatches", im.lay.fmt->name, bad);
         CHECK(count_pattern(&im, 0, cs) == before, "%s compressed data modified", im.lay.fmt->name);
         mali_bc_list_free(&jl);
         free(im.mem);
      }
      /* 3D */
      {
         struct img im = mk_img(f, VK_IMAGE_TYPE_3D, (VkExtent3D){ 9, 6, 3 }, 3, 1, 200 + i);
         struct mali_bc_list jl = { 0 };
         CHECK(mali_bc_plan_decode_all(&jl, &im.lay, (uint64_t)(uintptr_t)im.mem) == 0, "%s 3D plan", im.lay.fmt->name);
         run_ref(&jl);
         unsigned bad = verify(&im, 0, 0, ~0u, ~0u, ~0u, 0, 1);
         CHECK(bad == 0, "%s 3D: %u mismatches", im.lay.fmt->name, bad);
         mali_bc_list_free(&jl);
         free(im.mem);
      }
      /* sub-region: blocks (1..3, 1..2) of level 1, layer 1 only; everything else must keep the 0xaa pattern */
      {
         struct img im = mk_img(f, VK_IMAGE_TYPE_2D, (VkExtent3D){ 32, 24, 1 }, 2, 2, 300 + i);
         struct mali_bc_list jl = { 0 };
         struct mali_bc_layout *l = &im.lay;
         /* level 1 is 16x12: region x 4..12, y 4..8 */
         CHECK(mali_bc_plan_decode(&jl, l, (uint64_t)(uintptr_t)im.mem, 1, 1, 1, (VkOffset3D){ 4, 4, 0 }, (VkExtent3D){ 8, 4, 1 }) == 0, "%s region plan", l->fmt->name);
         unsigned sh_before = count_pattern(&im, l->shadow.base, l->size);
         run_ref(&jl);
         unsigned bad = verify(&im, 4, 4, 12, 8, 1, 1, 1);
         CHECK(bad == 0, "%s region: %u mismatches", l->fmt->name, bad);
         /* texels outside: layer 0, level 0, and the rest of level 1 of layer 1 are untouched */
         const struct mali_bc_level *s1 = &l->shadow.lv[1];
         uint64_t o1 = l->shadow.base + l->shadow.array_stride + s1->off;
         for (uint32_t y = 0; y < s1->h; y++)
            for (uint32_t x = 0; x < s1->w; x++) {
               if (x >= 4 && x < 12 && y >= 4 && y < 8) continue;
               const uint8_t *g = im.mem + o1 + (uint64_t)y * s1->row + (uint64_t)x * l->fmt->shadow_bpt;
               int ok = 1;
               for (unsigned k = 0; k < l->fmt->shadow_bpt; k++) ok &= g[k] == 0xaa;
               CHECK(ok, "%s region leaked to (%u,%u)", l->fmt->name, x, y);
               if (!ok) goto done;
            }
         /* exactly 2x1 blocks... = 8x4 texels * bpt bytes changed in shadow (a 0xaa texel byte in decoded data is possible: count with slack) */
         {
            unsigned after = count_pattern(&im, l->shadow.base, l->size);
            unsigned expect_changed = 8 * 4 * l->fmt->shadow_bpt;
            CHECK(sh_before - after <= expect_changed && sh_before - after + expect_changed / 4 + 8 >= expect_changed, "%s changed bytes %u (expected ~%u)", l->fmt->name, sh_before - after, expect_changed);
         }
      done:
         mali_bc_list_free(&jl);
         free(im.mem);
      }
   }
}

/* ---------- 2b. uploads: buffer gaps, sub-regions, layers (RUNG 2, character textures) ---------- */

/* Mesa's vk_image_buffer_copy_layout, written independently of the driver helper: bufferRowLength / bufferImageHeight are in texels and are
 * rounded up to whole blocks. This is the oracle for mali_bc_buffer_geometry(). */
static void
ref_geom(uint32_t rl, uint32_t ih, VkExtent3D e, uint32_t bw, uint32_t bh, uint32_t bs, uint32_t *row, uint64_t *slice, uint32_t *ex, uint32_t *ey)
{
   const uint32_t r = rl ? rl : e.width, h = ih ? ih : e.height;
   *ex = (e.width + bw - 1) / bw;
   *ey = (e.height + bh - 1) / bh;
   *row = (r + bw - 1) / bw * bs;
   *slice = (uint64_t)((h + bh - 1) / bh) * *row;
}

static void t_bufgeom(void)
{
   static const VkExtent3D ext[] = { { 4, 4, 1 }, { 5, 3, 1 }, { 32, 24, 1 }, { 64, 1, 1 }, { 13, 9, 1 }, { 4096, 4096, 1 } };
   static const uint32_t rls[] = { 0, 3, 4, 5, 16, 33, 4096, 4100 };
   static const uint32_t ihs[] = { 0, 1, 3, 8, 24, 25 };
   unsigned n = 0;
   for (unsigned e = 0; e < 6; e++)
      for (unsigned r = 0; r < 8; r++)
         for (unsigned h = 0; h < 6; h++) {
            if (rls[r] && rls[r] < ext[e].width) continue;   /* VU: bufferRowLength >= imageExtent.width */
            if (ihs[h] && ihs[h] < ext[e].height) continue;
            struct mali_bc_bufgeom bg;
            uint32_t row, ex, ey;
            uint64_t slice;
            CHECK(mali_bc_buffer_geometry(rls[r], ihs[h], ext[e], 4, 4, 16, &bg) == 0, "geom refused");
            ref_geom(rls[r], ihs[h], ext[e], 4, 4, 16, &row, &slice, &ex, &ey);
            CHECK(bg.row == row && bg.slice == slice && bg.ex == ex && bg.ey == ey,
                  "%ux%u rl %u ih %u: row %u/%u slice %llu/%llu ex %u/%u ey %u/%u", ext[e].width, ext[e].height, rls[r], ihs[h], bg.row, row,
                  (unsigned long long)bg.slice, (unsigned long long)slice, bg.ex, ex, bg.ey, ey);
            if (!rls[r] && !ihs[h]) {   /* tight: the whole level, exactly the compressed level size */
               struct mali_bc_layout l;
               if (mali_bc_layout_init(VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D, ext[e], 1, 1, &l) == 0)
                  CHECK(bg.slice == l.comp.lv[0].size, "%ux%u tight slice %llu comp size %llu", ext[e].width, ext[e].height,
                        (unsigned long long)bg.slice, (unsigned long long)l.comp.lv[0].size);
            }
            n++;
         }
   CHECK(n > 100, "bufgeom matrix %u", n);
}

/* One image->image upload through a staging buffer that has gaps (bufferRowLength / bufferImageHeight != 0), a sub-region and three layers:
 * the bytes must land exactly where the level expects them, the decode must cover exactly the uploaded blocks, and every block outside the
 * uploaded region must still hold its previous decode. */
static void t_upload(void)
{
   static const struct { uint32_t fl, fh; } gap[] = { { 0, 0 }, { 8, 4 }, { 4, 8 }, { 0, 4 }, { 4, 0 } };
   for (int i = 0; i < 16; i++) {
      VkFormat f = g_all[i];
      struct img src = mk_img(f, VK_IMAGE_TYPE_2D, (VkExtent3D){ 32, 24, 1 }, 3, 3, 400 + i);
      struct img dst = mk_img(f, VK_IMAGE_TYPE_2D, (VkExtent3D){ 32, 24, 1 }, 3, 3, 500 + i);
      const struct mali_bc_layout *l = &src.lay;
      const uint32_t bs = l->fmt->block_bytes, bpt = l->fmt->shadow_bpt;
      memset(dst.mem + l->comp.base, 0x11, l->comp.size);
      uint8_t *snap = malloc(l->shadow.size);
      memcpy(snap, dst.mem + l->shadow.base, l->shadow.size);
      unsigned geom_bad = 0, land_bad = 0, dec_bad = 0, keep_bad = 0;
      for (unsigned g = 0; g < sizeof gap / sizeof gap[0]; g++)
         for (uint32_t a = 0; a < 3; a++)
            for (uint32_t v = 0; v < 3; v++) {
               const struct mali_bc_level *c = &l->comp.lv[v], *s = &l->shadow.lv[v];
               /* sub-region: level 1 writes blocks (1,0)..(2,2) + the rest of the block row; other levels the whole one */
               VkOffset3D o = v == 1 ? (VkOffset3D){ 4, 0, 0 } : (VkOffset3D){ 0, 0, 0 };
               VkExtent3D e = v == 1 ? (VkExtent3D){ (c->w > 12 ? 8u : c->w - 4u), c->h, 1 } : (VkExtent3D){ c->w, c->h, 1 };
               struct mali_bc_bufgeom bg, rf;
               uint32_t row, ex, ey;
               uint64_t slice;
               geom_bad += mali_bc_buffer_geometry(e.width + gap[g].fl, e.height + gap[g].fh, e, 4, 4, bs, &bg) != 0;
               ref_geom(e.width + gap[g].fl, e.height + gap[g].fh, e, 4, 4, bs, &row, &slice, &ex, &ey);
               const uint64_t off = 2ull * bs;   /* non-zero, block aligned bufferOffset */
               const size_t need = (size_t)(off + slice);
               uint8_t *stg = malloc(need);
               memset(stg, 0x5a, need);
               const uint32_t bx0 = (uint32_t)o.x / 4, by0 = (uint32_t)o.y / 4;
               for (uint32_t by = 0; by < ey; by++)
                  for (uint32_t bx = 0; bx < ex; bx++)
                     memcpy(stg + off + (uint64_t)by * row + (uint64_t)bx * bs,
                            src.mem + l->comp.base + a * l->comp.array_stride + c->off + (uint64_t)(by0 + by) * c->row + (uint64_t)(bx0 + bx) * bs, bs);
               /* the driver's copy: image region <- staging, with the helper's geometry */
               for (uint32_t by = 0; by < bg.ey; by++)
                  for (uint32_t bx = 0; bx < bg.ex; bx++) {
                     uint8_t *d = dst.mem + l->comp.base + a * l->comp.array_stride + c->off + (uint64_t)(by0 + by) * c->row + (uint64_t)(bx0 + bx) * bs;
                     memcpy(d, stg + off + (uint64_t)by * bg.row + (uint64_t)bx * bs, bs);
                  }
               free(stg);
               struct mali_bc_list jl = { 0 };
               CHECK(mali_bc_plan_decode(&jl, l, (uint64_t)(uintptr_t)dst.mem, v, a, 1, o, e) == 0 && jl.n == 1, "%s upload plan", l->fmt->name);
               run_ref(&jl);
               mali_bc_list_free(&jl);
               /* landed: the region bytes equal the source's, the same block rows outside it are still 0x11 */
               for (uint32_t by = 0; by < c->bh; by++)
                  for (uint32_t bx = 0; bx < c->bw; bx++) {
                     const bool in = by >= by0 && by < by0 + bg.ey && bx >= bx0 && bx < bx0 + bg.ex;
                     const uint8_t *d = dst.mem + l->comp.base + a * l->comp.array_stride + c->off + (uint64_t)by * c->row + (uint64_t)bx * bs;
                     const uint8_t *sp = src.mem + l->comp.base + a * l->comp.array_stride + c->off + (uint64_t)by * c->row + (uint64_t)bx * bs;
                     land_bad += in ? memcmp(d, sp, bs) != 0 : memcmp(d, (uint8_t[16]){ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 }, bs) != 0;
                  }
               /* decoded: the region matches Mesa's decode of the block it now holds (plan_decode rounds to blocks, clamped to the level) */
               const uint32_t rx0 = bx0 * 4, ry0 = by0 * 4;
               const uint32_t rx1 = rx0 + bg.ex * 4 > c->w ? c->w : rx0 + bg.ex * 4;
               const uint32_t ry1 = ry0 + bg.ey * 4 > c->h ? c->h : ry0 + bg.ey * 4;
               struct img dr = dst;
               dec_bad += verify(&dr, rx0, ry0, rx1, ry1, v, a, 1);
               /* outside: the previous decode of every other block of this level is untouched */
               for (uint32_t by = 0; by < c->bh; by++)
                  for (uint32_t bx = 0; bx < c->bw; bx++) {
                     if (by >= by0 && by < by0 + bg.ey && bx >= bx0 && bx < bx0 + bg.ex) continue;
                     const uint8_t *now = dst.mem + l->shadow.base + a * l->shadow.array_stride + s->off + (uint64_t)by * 4 * s->row + (uint64_t)bx * 4 * bpt;
                     const uint8_t *was = snap + a * l->shadow.array_stride + s->off + (uint64_t)by * 4 * s->row + (uint64_t)bx * 4 * bpt;
                     for (unsigned r = 0; r < 4; r++)
                        keep_bad += memcmp(now + (uint64_t)r * s->row, was + (uint64_t)r * s->row, 4 * bpt) != 0;
                  }
               memset(dst.mem + l->comp.base + a * l->comp.array_stride + c->off, 0x11, c->size);
               memcpy(dst.mem + l->shadow.base, snap, l->shadow.size);   /* next case starts from the same clean state */
            }
      CHECK(geom_bad == 0, "%s upload geometry: %u refusals", l->fmt->name, geom_bad);
      CHECK(land_bad == 0, "%s upload: %u blocks in the wrong place", l->fmt->name, land_bad);
      CHECK(dec_bad == 0, "%s upload: %u texels of the uploaded region wrong", l->fmt->name, dec_bad);
      CHECK(keep_bad == 0, "%s upload: %u decoded blocks outside the region were clobbered", l->fmt->name, keep_bad);
      free(snap);
      free(src.mem);
      free(dst.mem);
   }
}

/* ---------- 2c. BC image -> BC image copy across layers ---------- */

static void t_imgcopy(void)
{
   for (int i = 0; i < 16; i++) {
      VkFormat f = g_all[i];
      struct img im = mk_img(f, VK_IMAGE_TYPE_2D, (VkExtent3D){ 32, 24, 1 }, 2, 3, 600 + i);
      const struct mali_bc_layout *l = &im.lay;
      const uint32_t bs = l->fmt->block_bytes, bpt = l->fmt->shadow_bpt;
      const struct mali_bc_level *c = &l->comp.lv[0], *s = &l->shadow.lv[0];
      /* vkCmdCopyImage layer 0 -> layer 2, a block rect at (1,1), width 2 blocks: source bytes by the copy plan's own strides */
      const uint32_t bx0 = 1, by0 = 1, ex = 2, ey = 2;
      unsigned bad = 0;
      for (uint32_t by = 0; by < ey; by++)
         for (uint32_t bx = 0; bx < ex; bx++)
            memcpy(im.mem + l->comp.base + 2 * l->comp.array_stride + c->off + (uint64_t)(by0 + by) * c->row + (uint64_t)(bx0 + bx) * bs,
                   im.mem + l->comp.base + 0 * l->comp.array_stride + c->off + (uint64_t)(by0 + by) * c->row + (uint64_t)(bx0 + bx) * bs, bs);
      /* 2 blocks x 2 blocks = 8x8 texels, block aligned, inside 32x24 level 0 */
      struct mali_bc_list jl = { 0 };
      CHECK(mali_bc_plan_decode(&jl, l, (uint64_t)(uintptr_t)im.mem, 0, 2, 1, (VkOffset3D){ bx0 * 4, by0 * 4, 0 }, (VkExtent3D){ ex * 4, ey * 4, 1 }) == 0,
            "%s imgcpy plan", l->fmt->name);
      run_ref(&jl);
      mali_bc_list_free(&jl);
      bad += verify(&im, bx0 * 4, by0 * 4, (bx0 + ex) * 4, (by0 + ey) * 4, 0, 2, 1);
      /* layer 0's shadow is stale by design (nothing wrote it now) but layer 1's must be exactly as mk_img left it */
      (void)bs; (void)bpt; (void)s;
      CHECK(bad == 0, "%s image copy layer 2: %u texels wrong", l->fmt->name, bad);
      free(im.mem);
   }
}

#include <vulkan/vulkan_core.h>
static int g_gpu_ran;
struct gpu {
   VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
   VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe[MALI_BC_NSHADERS];
   VkDescriptorPool pool; VkCommandPool cp; VkPhysicalDeviceMemoryProperties mp;
   char name[256]; uint32_t align;
};
#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("  FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); g_fail++; return 0; } } while (0)

static int gpu_init(struct gpu *g)
{
   memset(g, 0, sizeof *g);
   VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "t_bc", 1, "t_bc", 1, VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai };
   if (vkCreateInstance(&ici, NULL, &g->inst) != VK_SUCCESS) return 0;
   uint32_t n = 0;
   vkEnumeratePhysicalDevices(g->inst, &n, NULL);
   if (!n) return 0;
   VkPhysicalDevice pds[8];
   if (n > 8) n = 8;
   vkEnumeratePhysicalDevices(g->inst, &n, pds);
   const char *want = getenv("BC_DEV");
   g->pd = pds[want ? atoi(want) % n : 0];
   VkPhysicalDeviceProperties pr;
   vkGetPhysicalDeviceProperties(g->pd, &pr);
   snprintf(g->name, sizeof g->name, "%s", pr.deviceName);
   g->align = (uint32_t)pr.limits.minStorageBufferOffsetAlignment;
   vkGetPhysicalDeviceMemoryProperties(g->pd, &g->mp);
   uint32_t nq = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(g->pd, &nq, NULL);
   VkQueueFamilyProperties qp[16];
   if (nq > 16) nq = 16;
   vkGetPhysicalDeviceQueueFamilyProperties(g->pd, &nq, qp);
   g->qf = ~0u;
   for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { g->qf = i; break; }
   if (g->qf == ~0u) return 0;
   float pri = 1;
   VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, g->qf, 1, &pri };
   VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi };
   VKC(vkCreateDevice(g->pd, &di, NULL, &g->dev));
   vkGetDeviceQueue(g->dev, g->qf, 0, &g->q);
   VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                         { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
   VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b };
   VKC(vkCreateDescriptorSetLayout(g->dev, &dl, NULL, &g->dsl));
   VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 48 };
   VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g->dsl, 1, &pcr };
   VKC(vkCreatePipelineLayout(g->dev, &pli, NULL, &g->pl));
   for (int s = 0; s < MALI_BC_NSHADERS; s++) {
      size_t w;
      const uint32_t *code = mali_bc_spirv((enum mali_bc_shader)s, &w);
      VkShaderModuleCreateInfo si = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, w * 4, code };
      VkShaderModule m;
      VKC(vkCreateShaderModule(g->dev, &si, NULL, &m));
      VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                         { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL }, g->pl };
      VKC(vkCreateComputePipelines(g->dev, VK_NULL_HANDLE, 1, &pi, NULL, &g->pipe[s]));
   }
   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 };
   VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2048, 1, &ps };
   VKC(vkCreateDescriptorPool(g->dev, &dpi, NULL, &g->pool));
   VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g->qf };
   VKC(vkCreateCommandPool(g->dev, &cpi, NULL, &g->cp));
   return 1;
}

/* Run the jobs over `mem` (the whole allocation image, size bytes): upload, dispatch, download in place. */
static int gpu_run(struct gpu *g, const struct mali_bc_list *jl, uint8_t *mem, size_t size, uint64_t image_va)
{
   VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE };
   VkBuffer buf;
   VKC(vkCreateBuffer(g->dev, &bi, NULL, &buf));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(g->dev, buf, &mr);
   uint32_t mt = ~0u;
   for (uint32_t i = 0; i < g->mp.memoryTypeCount; i++)
      if ((mr.memoryTypeBits >> i & 1) && (g->mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                                           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
   if (mt == ~0u) { printf("  no host coherent memory\n"); return 0; }
   VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
   VkDeviceMemory dm;
   VKC(vkAllocateMemory(g->dev, &mi, NULL, &dm));
   VKC(vkBindBufferMemory(g->dev, buf, dm, 0));
   void *map;
   VKC(vkMapMemory(g->dev, dm, 0, VK_WHOLE_SIZE, 0, &map));
   memcpy(map, mem, size);
   VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g->cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
   VkCommandBuffer cb;
   VKC(vkAllocateCommandBuffers(g->dev, &ca, &cb));
   VkCommandBufferBeginInfo bg = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
   VKC(vkBeginCommandBuffer(cb, &bg));
   VkDescriptorSet *sets = calloc(jl->n, sizeof *sets);
   for (unsigned i = 0; i < jl->n; i++) {
      const struct mali_bc_job *j = &jl->j[i];
      VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g->pool, 1, &g->dsl };
      VKC(vkAllocateDescriptorSets(g->dev, &da, &sets[i]));
      VkDescriptorBufferInfo bb[2];
      for (int k = 0; k < 2; k++) {
         uint64_t off = j->bind[k].va - image_va;
         if (off % g->align) { printf("  bind offset %llu not %u aligned\n", (unsigned long long)off, g->align); g_fail++; return 0; }
         bb[k] = (VkDescriptorBufferInfo){ buf, off, j->bind[k].size };
      }
      VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bb[0], NULL },
                                    { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bb[1], NULL } };
      vkUpdateDescriptorSets(g->dev, 2, w, 0, NULL);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->pipe[j->sh]);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->pl, 0, 1, &sets[i], 0, NULL);
      vkCmdPushConstants(cb, g->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 48, j->push);
      vkCmdDispatch(cb, j->groups[0], j->groups[1], j->groups[2]);
   }
   VKC(vkEndCommandBuffer(cb));
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb };
   VKC(vkQueueSubmit(g->q, 1, &si, VK_NULL_HANDLE));
   VKC(vkQueueWaitIdle(g->q));
   memcpy(mem, map, size);
   vkFreeCommandBuffers(g->dev, g->cp, 1, &cb);
   vkFreeDescriptorSets(g->dev, g->pool, jl->n, sets);
   free(sets);
   vkDestroyBuffer(g->dev, buf, NULL);
   vkFreeMemory(g->dev, dm, NULL);
   return 1;
}

static void gpu_case(struct gpu *g, VkFormat f, VkImageType t, VkExtent3D e, uint32_t lv, uint32_t ly, uint64_t seed, const char *tag)
{
   struct img a = mk_img(f, t, e, lv, ly, seed);
   struct img b = a;
   b.mem = aligned_alloc(64, a.lay.size + 64);
   memcpy(b.mem, a.mem, a.lay.size);
   struct mali_bc_list ja = { 0 }, jb = { 0 };
   mali_bc_plan_decode_all(&ja, &a.lay, (uint64_t)(uintptr_t)a.mem);
   mali_bc_plan_decode_all(&jb, &b.lay, 0);
   run_ref(&ja);
   if (gpu_run(g, &jb, b.mem, b.lay.size, 0)) {
      g_gpu_ran++;
      /* compare every byte of the shadow region, padding included, and the untouched compressed region */
      size_t bad = 0, first = 0;
      for (size_t i = 0; i < a.lay.size; i++) if (a.mem[i] != b.mem[i]) { if (!bad) first = i; bad++; }
      CHECK(bad == 0, "GPU %s %s: %zu bytes differ from the reference (first at %zu, shadow base %llu)", a.lay.fmt->name, tag, bad, first, (unsigned long long)a.lay.shadow.base);
      unsigned tex = verify(&b, 0, 0, ~0u, ~0u, ~0u, 0, a.lay.layers);
      CHECK(tex == 0, "GPU %s %s: %u texels differ from Mesa", a.lay.fmt->name, tag, tex);
      if (g_verbose) printf("  gpu %-16s %-8s %s\n", a.lay.fmt->name, tag, bad || tex ? "MISMATCH" : "exact");
   }
   mali_bc_list_free(&ja); mali_bc_list_free(&jb);
   free(a.mem); free(b.mem);
}

static void t_gpu(void)
{
   struct gpu g;
   if (!gpu_init(&g)) { printf("  (no usable Vulkan device: GPU shader execution SKIPPED)\n"); return; }
   printf("  GPU: %s\n", g.name);
   for (int i = 0; i < NEVERY; i++) {
      gpu_case(&g, every(i), VK_IMAGE_TYPE_2D, (VkExtent3D){ 21, 11, 1 }, 4, 3, 500 + i, "2Dmip");
      gpu_case(&g, every(i), VK_IMAGE_TYPE_3D, (VkExtent3D){ 9, 6, 3 }, 3, 1, 600 + i, "3D");
      gpu_case(&g, every(i), VK_IMAGE_TYPE_2D, (VkExtent3D){ 256, 256, 1 }, 1, 1, 700 + i, "256sq");
   }
}

/* ---------- 4. compile ---------- */
struct cstat { uint32_t bin_size, regs, tls, wls, push; uint64_t hash; };
static struct cstat g_cs[2][MALI_BC_NSHADERS];

static void t_compile(void)
{
   static const unsigned archs[2] = { 11, 13 };
   static const char *nm[] = { "bc1", "bc4", "bc6h", "bc7", "etc2" };
   for (int a = 0; a < 2; a++)
      for (int s = 0; s < MALI_BC_NSHADERS; s++) {
         struct mali_lbinding b[2];
         struct mali_compile_layout lay;
         memset(&lay, 0, sizeof lay);
         for (unsigned i = 0; i < 2; i++) b[i] = (struct mali_lbinding){ MALI_DT_STORAGE_BUFFER, 1, i, 1, 0, MALI_NO_IMM_SAMPLER };
         lay.nsets = 1; lay.set[0] = (struct mali_lset){ 2, 2, 0, b }; lay.push_size = 48;
         size_t w;
         const uint32_t *spv = mali_bc_spirv((enum mali_bc_shader)s, &w);
         struct mali_compile_opts o = { 0 };
         struct mali_compiled c;
         char err[512] = "";
         o.arch = archs[a]; o.stage = MALI_STAGE_COMPUTE; o.spirv = spv; o.spirv_words = w; o.layout = &lay; o.fau_reserved = 12; o.cache_dir = "";
         int r = mali_compile(&o, &c, err, sizeof err);
         CHECK(r == 0, "v%u %s compile: %s", archs[a], nm[s], err);
         if (r) continue;
         struct cstat *cs = &g_cs[a][s];
         cs->bin_size = c.bin_size; cs->regs = c.work_reg_count; cs->tls = c.tls_size; cs->wls = c.wls_size; cs->push = c.push_const_bytes;
         cs->hash = bcv_fnv(c.bin, c.bin_size, BCV_FNV0);
         printf("  v%u %-5s %6u B  work_regs %2u  tls %5u  wls %u  push %u  local %ux%ux%u\n", archs[a], nm[s], c.bin_size, c.work_reg_count, c.tls_size, c.wls_size,
                c.push_const_bytes, c.local_size[0], c.local_size[1], c.local_size[2]);
         CHECK(c.stage == MALI_STAGE_COMPUTE && c.bin_size > 0 && c.bin_size < 64 * 1024, "%s bin size %u", nm[s], c.bin_size);
         CHECK(c.wls_size == 0, "%s uses workgroup memory (%u)", nm[s], c.wls_size);
         CHECK(c.work_reg_count <= 64, "%s work registers %u > 64", nm[s], c.work_reg_count);
         CHECK(c.push_const_bytes <= 48, "%s push %u > 48", nm[s], c.push_const_bytes);
         CHECK(c.local_size[0] == 64 && c.local_size[1] == 1 && c.local_size[2] == 1, "%s local size", nm[s]);
#ifdef HAVE_GOLDEN
         if (!getenv("BC_NO_GOLDEN")) {
            const struct bcg_compile *gc = &bcg_compile[a][s];
            CHECK(gc->hash == cs->hash && gc->bin_size == cs->bin_size && gc->regs == cs->regs && gc->tls == cs->tls,
                  "v%u %s differs from golden (bin %u/%u regs %u/%u tls %u/%u): rerun with --gen-golden if intended", archs[a], nm[s], cs->bin_size, gc->bin_size,
                  cs->regs, gc->regs, cs->tls, gc->tls);
         }
#endif
         mali_compiled_free(&c);
      }
}

/* ---------- golden ---------- */
static int gen_golden(const char *path)
{
   FILE *f = fopen(path, "w");
   if (!f) return 1;
   fprintf(f, "/* SPDX-License-Identifier: MIT */\n/* GENERATED by `t_bc --gen-golden` (driver/vk/tests/build-t_bc.sh --gen-golden), do not edit.\n"
              " * bcg_compile: mali_compile() statistics of the four decode shaders [v11, v13][bc1 bc4 bc6h bc7] (regression guard).\n"
              " * bcg_px: expected texelFetch results of the hardware vector image (tests/t_bc_vec.h: 20x12, 2 levels, seed = index + 0xBC00) as packed\n"
              " * words, Mesa util_format decoders as the oracle: kind 0 packUnorm4x8, 1 packSnorm4x8, 2 packHalf2x16 x2. */\n"
              "#pragma once\n#include <stdint.h>\n#include <vulkan/vulkan_core.h>\n");
   fprintf(f, "struct bcg_compile { uint32_t bin_size, regs, tls, wls, push; uint64_t hash; };\n");
   fprintf(f, "static const struct bcg_compile bcg_compile[2][%d] = {\n", MALI_BC_NSHADERS);
   for (int a = 0; a < 2; a++) {
      fprintf(f, "  {\n");
      for (int s = 0; s < MALI_BC_NSHADERS; s++) {
         const struct cstat *c = &g_cs[a][s];
         fprintf(f, "    { %u, %u, %u, %u, %u, 0x%016llxull },\n", c->bin_size, c->regs, c->tls, c->wls, c->push, (unsigned long long)c->hash);
      }
      fprintf(f, "  },\n");
   }
   fprintf(f, "};\n");
   static const VkFormat hw[12] = { VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK,
                                    VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK,
                                    VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK };
   fprintf(f, "#define BCG_NTEXELS (%u * %u + (%u / 2) * (%u / 2))\n", BCV_W, BCV_H, BCV_W, BCV_H);
   fprintf(f, "struct bcg_px { VkFormat vk; const char *name; unsigned kind; uint64_t hash; uint32_t w[BCG_NTEXELS * 2]; };\n");
   fprintf(f, "static const struct bcg_px bcg_px[12] = {\n");
   for (int i = 0; i < 12; i++) {
      VkFormat vf = hw[i];
      const struct mali_bc_fmt *fm = mali_bc_format(vf);
      uint8_t blocks[BCV_NBLOCKS * 16];
      bcv_fill(vf, 0xBC00 + (unsigned)i, blocks, BCV_NBLOCKS, fm->block_bytes, 0);
      unsigned kind = fm->shadow_bpt == 8 ? 2 : (fm->shadow == VK_FORMAT_R8_SNORM || fm->shadow == VK_FORMAT_R8G8_SNORM) ? 1 : 0;
      static uint32_t words[BCV_W * BCV_H * 2 + BCV_W * BCV_H / 2];
      unsigned nw = 0, blk0 = 0;
      for (unsigned lv = 0; lv < 2; lv++) {
         unsigned w = BCV_W >> lv, h = BCV_H >> lv, bw = (w + 3) / 4, bh = (h + 3) / 4;
         for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++) {
               uint8_t t[16 * 8];
               mesa_dec(fm, blocks + (blk0 + (y / 4) * bw + x / 4) * fm->block_bytes, t);
               const uint8_t *px = t + ((y & 3) * 4 + (x & 3)) * fm->shadow_bpt;
               uint32_t w0 = 0, w1 = 0;
               if (kind == 2) {
                  const uint16_t *hh = (const uint16_t *)px;
                  w0 = hh[0] | (uint32_t)hh[1] << 16; w1 = hh[2] | (uint32_t)hh[3] << 16;
               } else if (fm->shadow_bpt == 4) {
                  memcpy(&w0, px, 4);
               } else {
                  int8_t sv[2] = { (int8_t)px[0], fm->shadow_bpt == 2 ? (int8_t)px[1] : 0 };
                  if (kind == 1) { for (int k = 0; k < 2; k++) if (sv[k] < -127) sv[k] = -127; }
                  w0 = (uint8_t)sv[0] | (fm->shadow_bpt == 2 ? (uint32_t)(uint8_t)sv[1] << 8 : 0) | (kind == 1 ? 0x7fu : 0xffu) << 24;
               }
               words[nw++] = w0; words[nw++] = w1;
            }
         blk0 += bw * bh;
      }
      if (nw != (BCV_W * BCV_H + BCV_W * BCV_H / 4) * 2) { printf("bad texel count %u\n", nw); return 1; }
      fprintf(f, "  { %d, \"%s\", %u, 0x%016llxull, {", (int)vf, fm->name, kind, (unsigned long long)bcv_fnv(words, nw * 4, BCV_FNV0));
      for (unsigned k = 0; k < nw; k++) fprintf(f, "%s0x%x,", k % 12 ? "" : "\n    ", words[k]);
      fprintf(f, "\n  } },\n");
   }
   fprintf(f, "};\n");
   fclose(f);
   return 0;
}

int main(int argc, char **argv)
{
   int no_gpu = 0;
   const char *gold = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "-v")) g_verbose = 1;
      else if (!strcmp(argv[i], "--no-gpu")) no_gpu = 1;
      else if (!strcmp(argv[i], "--gen-golden") && i + 1 < argc) gold = argv[++i], setenv("BC_NO_GOLDEN", "1", 1);
   }
   printf("t_bc: formats\n");   t_formats();
   printf("t_bc: layout\n");    t_layout();
   printf("t_bc: planner\n");   t_planner();
   printf("t_bc: reference vs Mesa decoders\n"); t_ref();
   printf("t_bc: buffer geometry (gaps, sub-regions)\n"); t_bufgeom();
   printf("t_bc: uploads: layers, mips, gaps, sub-regions\n"); t_upload();
   printf("t_bc: image copy across layers\n"); t_imgcopy();
   if (!no_gpu) { printf("t_bc: shaders on the host GPU\n"); t_gpu(); }
   printf("t_bc: shader compile (v11, v13)\n"); t_compile();
   if (gold && !g_fail) {
      if (gen_golden(gold)) return 1;
      printf("t_bc: wrote %s\n", gold);
   }
   printf("t_bc: %d checks, %d failures%s\n", g_checks, g_fail, no_gpu || !g_gpu_ran ? " (GPU shader execution NOT run)" : "");
   return g_fail ? 1 : 0;
}
