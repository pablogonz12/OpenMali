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
