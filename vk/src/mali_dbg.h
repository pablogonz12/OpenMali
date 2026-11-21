/* SPDX-License-Identifier: MIT */
/* THROWAWAY instrument (MALI_DRAW_DBG=1), rung-2 blit/draw diff. One line per blit, per draw and per sampled
 * descriptor, each field the caller hands us (image, format, usage, tiling, extent, layout, sampler, view), capped
 * at 40 lines per translation unit plus one limit line (the unit that prints is named, so the cap stays readable).
 * It exists to diff one black frame against one painting frame; never merge it. */
#ifndef MALI_DBG_H
#define MALI_DBG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

static inline const char *
mali_dbg_fmt(VkFormat f)
{
   switch (f) {
   case VK_FORMAT_UNDEFINED: return "UNDEFINED";
   case VK_FORMAT_R8_UNORM: return "R8_UNORM";
   case VK_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
   case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
   case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
   case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
   case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
   case VK_FORMAT_B8G8R8_UNORM: return "B8G8R8_UNORM";
   case VK_FORMAT_R5G6B5_UNORM_PACK16: return "R5G6B5_UNORM";
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
   case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
   case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
   case VK_FORMAT_S8_UINT: return "S8_UINT";
   case VK_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
   default: return "?";
   }
}

static inline const char *
mali_dbg_tiling(VkImageTiling t)
{
   return t == VK_IMAGE_TILING_LINEAR ? "LINEAR" : t == VK_IMAGE_TILING_OPTIMAL ? "OPTIMAL" : "?";
}

static inline const char *
mali_dbg_layout(VkImageLayout l)
{
   switch (l) {
   case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
   case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
   case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATT";
   case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "XFER_SRC";
   case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "XFER_DST";
   case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_RO";
   case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "PRESENT";
   case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL: return "ATTACHMENT";
   default: return "?";
   }
}

/* one prefixed line; after 40 in this unit, a single limit line */
static inline void
mali_dbg_line(const char *unit, const char *fmt, ...)
{
   static unsigned n;
   if (!getenv("MALI_DRAW_DBG"))
      return;
   if (n >= 40) {
      if (n++ == 40)
         fprintf(stderr, "mali_draw: ... (MALI_DRAW_DBG limit, 40 lines, unit %s)\n", unit);
      return;
   }
   n++;
   fprintf(stderr, "mali_draw %s: ", unit);
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
}

#endif
