/* SPDX-License-Identifier: MIT */
/* M5: VkFormat -> Mali hardware formats, and the format/image-format property tables.
 * Data comes from Mesa's pan_format tables (pan_pipe_format_vN / pan_blendable_formats_vN, linked as libpan-format-vN.a), so every
 * pixel-format word is the v10+ encoding (component order in bits 0..11, format in 12..19, sRGB bit 20), never the v6 swizzle one.
 * A vertex attribute descriptor word 0 is 5 | 1<<4 | 1<<8 | hw<<10 (RGBA32F: 0x2fc00115). Nothing here needs a device or a GPU. */
#ifndef MALI_FORMAT_H
#define MALI_FORMAT_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* Limits shared with mali_physical_device.c (raise both together; t_vk_feat imgmax is the device test).
 * 16384 is the largest 2D texture the tiler/descriptor path was proven on (X8, t_vk_feat imgmax), not the number PanVK
 * advertises: panvk_vX_physical_device.c:864-868 says 2^16 for every dimension, which no allocation of ours can back
 * (a 65536^2 R8 image is 4 GiB against a 1 GiB maxMemoryAllocationSize, mali_physical_device.c:193). The 3D depth and
 * the array-layer count are hardware descriptor fields, not allocation sizes: 2048 each, and 3D x/y proved at 2048. */
#define MALI_MAX_DIM_1D 16384u
#define MALI_MAX_DIM_2D 16384u
#define MALI_MAX_DIM_3D 2048u
#define MALI_MAX_LAYERS 2048u
#define MALI_MAX_MIPS   16u
#define MALI_BUF_ALIGN  256u /* minUniform/Storage/TexelBufferOffsetAlignment */

struct mali_fmt {
   VkFormat vk;
   int pipe;             /* enum pipe_format of the whole VkFormat */
   uint32_t hw;          /* 22-bit pixel format word: texture descriptor bits 10.., attribute descriptor bits 10.. */
   uint32_t bind;        /* PAN_BIND_* of the pan table (DS 1, RT 2, SAMPLER_VIEW 4, VERTEX 8, STORAGE 16, TEXEL_BUFFER 32) */
   /* Render target (colour). blendable = fixed-function blend works (Blend descriptor "internal"/"conversion" fields). */
   bool rt, blendable;
   uint16_t rt_internal;   /* mali_color_buffer_internal_format (RT descriptor), 0 when !blendable */
   uint16_t rt_writeback;  /* mali_mfbd_color_format: RT descriptor writeback format (R8G8B8A8 = 19) */
   uint32_t rt_tb;         /* dithered tilebuffer pixel format word = Blend "conversion memory format" (RGBA8: 237<<12) */
   /* Depth / stencil. zs_write: ZS Target write format (D16 1, D24X8 4, D32 14), zs_internal: FBD Z internal format (D16 0, D24 1, D32 2). */
   uint8_t zs_write, zs_internal;
   bool has_depth, has_stencil;
   uint8_t bs, bw, bh;     /* bytes per block (of the whole format), block width/height */
   uint8_t nplanes;        /* memory planes: 1, or 2 for D+S formats (depth plane, then S8 plane) */
   struct {
      int pipe;            /* plane pipe format */
      uint8_t clump;       /* Generic Plane "clump format" */
      uint8_t bs, bw, bh;
      uint32_t hw;         /* texture pixel format word of the plane */
      VkImageAspectFlagBits aspect;
      uint8_t ssx, ssy;    /* log2 chroma subsampling of the plane's extent against the image extent (NV12 plane 1: 1, 1) */
   } plane[2];
   VkFormatFeatureFlags linear, optimal, buffer;
   bool compressed;
   bool ycbcr;             /* multi-planar YCbCr (G8_B8R8_2PLANE_420 only): plane 0 R8 (Y), plane 1 R8G8 (CbCr) at half size; sampled through a conversion */
};

/* The one YCbCr format we implement (gamescope's NV12). */
#define MALI_YCBCR_FORMAT VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
#define MALI_YCBCR_PLANES 2u   /* descriptors of the image: luma, chroma */
#define MALI_YCBCR_SLOTS  3u   /* 32-byte descriptor slots of one combined image sampler element with a conversion: luma, chroma, sampler */

/* arch: 10..13 (11 = G615, 13 = G925). false when the format is not advertised. */
bool mali_format_get(unsigned arch, VkFormat f, struct mali_fmt *out);
/* Number of formats advertised, and the i-th (stable order = VkFormat value). */
unsigned mali_format_count(unsigned arch);
bool mali_format_nth(unsigned arch, unsigned i, struct mali_fmt *out);

/* Vertex attribute descriptor word 0 for format f (0 when the format cannot be a vertex attribute). */
uint32_t mali_format_attr_word0(const struct mali_fmt *f);

/* Pure image-format query (the core of vkGetPhysicalDeviceImageFormatProperties2). VK_SUCCESS or VK_ERROR_FORMAT_NOT_SUPPORTED. */
/* MALI_NO_MODIFIER_COMM=<comm substring>: hide VK_EXT_image_drm_format_modifier from that process (zink then takes
 * the non-modifier DRI3 path). For the X8 A/B: its surface is the only screen-size modifier image there.
 * mali_image.c owns the switch; the enumeration (mali_physical_device.c), the modifier-list queries and image
 * creation all ask it. */
bool mali_modifier_disabled(void);

VkResult mali_image_format_properties(unsigned arch, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
                                      VkImageCreateFlags flags, VkImageFormatProperties *out);

/* Entrypoints (physical device level; plain names, the dispatch table lists them). */
VKAPI_ATTR void VKAPI_CALL mali_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice, VkFormat, VkFormatProperties2 *);
VKAPI_ATTR VkResult VKAPI_CALL mali_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2 *,
                                                                          VkImageFormatProperties2 *);
VKAPI_ATTR void VKAPI_CALL mali_GetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *,
                                                                            uint32_t *, VkSparseImageFormatProperties2 *);

#endif
