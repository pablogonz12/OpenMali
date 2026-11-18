/* SPDX-License-Identifier: MIT */
/* Colour render-target format table (header-only so mali_format.c and the render path agree without a link dependency). */
#ifndef MALI_RTFMT_TAB_H
#define MALI_RTFMT_TAB_H
#include <stdbool.h>
#include <vulkan/vulkan_core.h>
#include "mali_cs_emit.h"

#define SW(r, g, b, a) ((r) | (g) << 3 | (b) << 6 | (a) << 9)
#define PF(idx, ord) (((uint32_t)(idx) << 12) | (ord))
#define SRGB (1u << 20)
/* Blendable colour formats beyond RGBA8 (pan_format.c pan_blendable_formats): internal, writeback, ncomp, alloc, float, swizzle, blend format */
static const struct mali_rtfmt mali_rt_tab[] = {
   { VK_FORMAT_R8_UNORM, 1, 16, 1, 4, 0, SW(0, 1, 2, 3), PF(237, 0) },
   { VK_FORMAT_R8_SRGB, 1, 16, 1, 4, 0, SW(0, 1, 2, 3), PF(237, 0) | SRGB },
   { VK_FORMAT_R8G8_UNORM, 1, 17, 2, 4, 0, SW(0, 1, 2, 3), PF(237, 0) },
   { VK_FORMAT_R8G8_SRGB, 1, 17, 2, 4, 0, SW(0, 1, 2, 3), PF(237, 0) | SRGB },
   { VK_FORMAT_R8G8B8A8_SRGB, 1, 19, 4, 4, 0, SW(0, 1, 2, 3), PF(237, 0) | SRGB },
   { VK_FORMAT_B8G8R8A8_UNORM, 1, 19, 4, 4, 0, SW(2, 1, 0, 3), PF(237, 0) },
   /* A8B8G8R8 packed 32 = R8G8B8A8 byte order on a little-endian target (R in byte 0): the same rows as the RGBA8 formats */
   { VK_FORMAT_A8B8G8R8_UNORM_PACK32, 1, 19, 4, 4, 0, SW(0, 1, 2, 3), PF(237, 0) },
   { VK_FORMAT_A8B8G8R8_SRGB_PACK32, 1, 19, 4, 4, 0, SW(0, 1, 2, 3), PF(237, 0) | SRGB },
   { VK_FORMAT_B8G8R8A8_SRGB, 1, 19, 4, 4, 0, SW(2, 1, 0, 3), PF(237, 0) | SRGB },
   { VK_FORMAT_R5G6B5_UNORM_PACK16, 5, 21, 3, 4, 0, SW(2, 1, 0, 3), PF(227, 16) },
   { VK_FORMAT_B5G6R5_UNORM_PACK16, 5, 21, 3, 4, 0, SW(0, 1, 2, 3), PF(227, 16) },
   { VK_FORMAT_R4G4B4A4_UNORM_PACK16, 4, 20, 4, 4, 0, SW(3, 2, 1, 0), PF(233, 0) },
   { VK_FORMAT_B4G4R4A4_UNORM_PACK16, 4, 20, 4, 4, 0, SW(3, 0, 1, 2), PF(233, 0) },
   { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 2, 24, 4, 4, 0, SW(0, 1, 2, 3), PF(238, 0) },
   { VK_FORMAT_A2R10G10B10_UNORM_PACK32, 2, 24, 4, 4, 0, SW(2, 1, 0, 3), PF(238, 0) },
   { VK_FORMAT_R5G5B5A1_UNORM_PACK16, 6, 28, 4, 4, 0, SW(3, 2, 1, 0), PF(225, 0) },
   { VK_FORMAT_B5G5R5A1_UNORM_PACK16, 6, 28, 4, 4, 0, SW(3, 0, 1, 2), PF(225, 0) },
   /* alpha at bit 15 = Mesa B5G5R5A1 (RGB5_A1, BGRA swizzle) */
   { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 6, 28, 4, 4, 0, SW(2, 1, 0, 3), PF(225, 0) },
   { VK_FORMAT_R16_SFLOAT, 16, 16, 1, 4, 1, SW(0, 1, 2, 3), PF(199, 0) },
   { VK_FORMAT_R16G16_SFLOAT, 17, 17, 2, 4, 1, SW(0, 1, 2, 3), PF(207, 0) },
   { VK_FORMAT_R16G16B16A16_SFLOAT, 18, 19, 4, 8, 1, SW(0, 1, 2, 3), PF(223, 0) },
   { VK_FORMAT_B10G11R11_UFLOAT_PACK32, 20, 20, 3, 4, 1, SW(0, 1, 2, 3), PF(89, 0) },
   /* integer colour formats: RAW tile buffer (internal 32 + log2 bytes, writeback RAW8/16/32/64/128), opaque conversion = the texture format word */
   { VK_FORMAT_R8_UINT, 32, 0, 1, 1, 0, SW(0, 1, 2, 3), PF(131, 16), 8, 0 },
   { VK_FORMAT_R8G8_UINT, 33, 1, 2, 2, 0, SW(0, 1, 2, 3), PF(139, 16), 8, 0 },
   { VK_FORMAT_R8G8B8A8_UINT, 34, 3, 4, 4, 0, SW(0, 1, 2, 3), PF(155, 0), 8, 0 },
   { VK_FORMAT_R16_UINT, 33, 1, 1, 2, 0, SW(0, 1, 2, 3), PF(132, 16), 16, 0 },
   { VK_FORMAT_R16G16_UINT, 34, 3, 2, 4, 0, SW(0, 1, 2, 3), PF(140, 16), 16, 0 },
   { VK_FORMAT_R16G16B16A16_UINT, 35, 5, 4, 8, 0, SW(0, 1, 2, 3), PF(156, 0), 16, 0 },
   { VK_FORMAT_R32_UINT, 34, 3, 1, 4, 0, SW(0, 1, 2, 3), PF(133, 16), 32, 0 },
   { VK_FORMAT_R32G32_UINT, 35, 5, 2, 8, 0, SW(0, 1, 2, 3), PF(141, 16), 32, 0 },
   { VK_FORMAT_R32G32B32A32_UINT, 36, 7, 4, 16, 0, SW(0, 1, 2, 3), PF(157, 0), 32, 0 },
   { VK_FORMAT_R8_SINT, 32, 0, 1, 1, 0, SW(0, 1, 2, 3), PF(195, 16), 8, 1 },
   { VK_FORMAT_R8G8_SINT, 33, 1, 2, 2, 0, SW(0, 1, 2, 3), PF(203, 16), 8, 1 },
   { VK_FORMAT_R8G8B8A8_SINT, 34, 3, 4, 4, 0, SW(0, 1, 2, 3), PF(219, 0), 8, 1 },
   { VK_FORMAT_R16_SINT, 33, 1, 1, 2, 0, SW(0, 1, 2, 3), PF(196, 16), 16, 1 },
   { VK_FORMAT_R16G16_SINT, 34, 3, 2, 4, 0, SW(0, 1, 2, 3), PF(204, 16), 16, 1 },
   { VK_FORMAT_R16G16B16A16_SINT, 35, 5, 4, 8, 0, SW(0, 1, 2, 3), PF(220, 0), 16, 1 },
   { VK_FORMAT_R32_SINT, 34, 3, 1, 4, 0, SW(0, 1, 2, 3), PF(197, 16), 32, 1 },
   { VK_FORMAT_R32G32_SINT, 35, 5, 2, 8, 0, SW(0, 1, 2, 3), PF(205, 16), 32, 1 },
   { VK_FORMAT_R32G32B32A32_SINT, 36, 7, 4, 16, 0, SW(0, 1, 2, 3), PF(221, 0), 32, 1 },
   /* A8B8G8R8 integer: byte order of R8G8B8A8, same rows */
   { VK_FORMAT_A8B8G8R8_UINT_PACK32, 34, 3, 4, 4, 0, SW(0, 1, 2, 3), PF(155, 0), 8, 0 },
   { VK_FORMAT_A8B8G8R8_SINT_PACK32, 34, 3, 4, 4, 0, SW(0, 1, 2, 3), PF(219, 0), 8, 1 },
   /* A2B10G10R10 integer: RAW32; int_bits = 10 only marks "integer, no rounding" (clear words are packed 10/10/10/2 in mali_render.c) */
   { VK_FORMAT_A2B10G10R10_UINT_PACK32, 34, 3, 4, 4, 0, SW(0, 1, 2, 3), PF(71, 0), 10, 0 },
   { VK_FORMAT_A2B10G10R10_SINT_PACK32, 34, 3, 4, 4, 0, SW(0, 1, 2, 3), PF(73, 0), 10, 1 },
   /* SNORM / UNORM16 / 32-bit float: not blendable on Valhall, so a RAW tile buffer (internal 32 + log2 bytes, writeback RAW8..RAW128) with the
    * blend unit's opaque conversion = the format's texture word (PF(hw >> 12, hw & 0xfff), t_image cross-checks every row against mali_fmt.hw);
    * is_float stays 0 (clear words are the packed pixel, mali_render.c). int_bits 0: the conversion rounds. */
   { VK_FORMAT_R8_SNORM, 32, 0, 1, 1, 0, SW(0, 1, 2, 3), PF(99, 16) },
   { VK_FORMAT_R8G8_SNORM, 33, 1, 2, 2, 0, SW(0, 1, 2, 3), PF(107, 16) },
   { VK_FORMAT_R8G8B8A8_SNORM, 34, 3, 4, 4, 0, SW(0, 1, 2, 3), PF(123, 0) },
   { VK_FORMAT_A8B8G8R8_SNORM_PACK32, 34, 3, 4, 4, 0, SW(0, 1, 2, 3), PF(123, 0) },
   { VK_FORMAT_R16_UNORM, 33, 1, 1, 2, 0, SW(0, 1, 2, 3), PF(164, 16) },
   { VK_FORMAT_R16G16_UNORM, 34, 3, 2, 4, 0, SW(0, 1, 2, 3), PF(172, 16) },
   { VK_FORMAT_R16G16B16A16_UNORM, 35, 5, 4, 8, 0, SW(0, 1, 2, 3), PF(188, 0) },
   { VK_FORMAT_R16_SNORM, 33, 1, 1, 2, 0, SW(0, 1, 2, 3), PF(100, 16) },
   { VK_FORMAT_R16G16_SNORM, 34, 3, 2, 4, 0, SW(0, 1, 2, 3), PF(108, 16) },
   { VK_FORMAT_R16G16B16A16_SNORM, 35, 5, 4, 8, 0, SW(0, 1, 2, 3), PF(124, 0) },
   { VK_FORMAT_R32_SFLOAT, 34, 3, 1, 4, 0, SW(0, 1, 2, 3), PF(167, 16) },
   { VK_FORMAT_R32G32_SFLOAT, 35, 5, 2, 8, 0, SW(0, 1, 2, 3), PF(175, 16) },
   { VK_FORMAT_R32G32B32A32_SFLOAT, 36, 7, 4, 16, 0, SW(0, 1, 2, 3), PF(191, 0) },
};

/* NULL + *supported true for R8G8B8A8_UNORM (the proven default); *supported false = not a render target we can draw to. */
static inline const struct mali_rtfmt *
mali_rtfmt_find(int vk_format, bool *supported)
{
   *supported = true;
   if (vk_format == VK_FORMAT_R8G8B8A8_UNORM)
      return NULL;
   for (unsigned i = 0; i < sizeof mali_rt_tab / sizeof mali_rt_tab[0]; i++)
      if (mali_rt_tab[i].vk_format == vk_format)
         return &mali_rt_tab[i];
   *supported = false;
   return NULL;
}

#endif
