/* SPDX-License-Identifier: MIT */
/* AFBC (Arm frame-buffer compression) layout maths, prototype. Pure functions, no hardware, no Mesa deps, so it
 * is host-testable. Mirrors panfrost lib/pan_afbc.h + pan_mod.c (pan_mod_afbc_init_slice_layout) for ONE plane,
 * 2D, single sample. NOT wired into mali_image.c: gated by MALI_AFBC=1 (mali_afbc_enabled), default off.
 * Design notes and next steps: /tmp/oc/lane-afbc-proto.md. */
#ifndef MALI_AFBC_H
#define MALI_AFBC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define MALI_AFBC_HEADER_BYTES 16u   /* one header per superblock */

/* Same bit values as drm_fourcc.h AFBC_FORMAT_MOD_*; kept local so this header stands alone. */
#define MALI_AFBC_SB_16x16 1u
#define MALI_AFBC_SB_32x8  2u
#define MALI_AFBC_SB_64x4  3u
#define MALI_AFBC_MOD_YTR    (1ull << 4)
#define MALI_AFBC_MOD_SPLIT  (1ull << 5)
#define MALI_AFBC_MOD_SPARSE (1ull << 6)
#define MALI_AFBC_MOD_TILED  (1ull << 8)

struct mali_afbc_params {
   unsigned arch;        /* GPU arch major (v13 here) */
   uint64_t modifier;    /* block size in low nibble + flags above (modifier & 0xf selects superblock) */
   unsigned bpp_bits;    /* bits per pixel of the (single) plane, e.g. 32 */
   uint32_t width, height; /* mip extent in pixels */
   uint32_t depth;       /* 1 for 2D */
};

struct mali_afbc_layout {
   uint32_t sb_w, sb_h;         /* superblock size, pixels */
   uint32_t tile;               /* superblock tiling factor: 1 or 8 (<=32bpp) or 4 */
   uint32_t aligned_w, aligned_h;
   uint32_t header_row_stride;  /* bytes between header rows */
   uint32_t stride_blocks;      /* header row stride in superblocks (descriptor "row stride"/"line stride" unit) */
   uint32_t nblocks;            /* superblocks per slice */
   uint64_t header_size;        /* bytes of header area per slice */
   uint64_t body_offset;        /* aligned offset of body from slice start */
   uint64_t payload_per_sb;     /* uncompressed payload bytes per superblock */
   uint64_t slice_stride;       /* header+body per depth slice */
   uint64_t size;               /* total bytes */
   uint32_t offset_align;       /* required alignment of the slice base address */
};

/* MALI_AFBC=1 enables the (future) AFBC image path. Off by default. */
static inline bool
mali_afbc_enabled(void)
{
   const char *e = getenv("MALI_AFBC");
   return e && e[0] == '1' && !e[1];
}

static inline uint64_t mali_afbc_align(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

static inline bool
mali_afbc_sb_size(uint64_t modifier, uint32_t *w, uint32_t *h)
{
   switch (modifier & 0xf) {
   case MALI_AFBC_SB_16x16: *w = 16; *h = 16; return true;
   case MALI_AFBC_SB_32x8:  *w = 32; *h = 8;  return true;
   case MALI_AFBC_SB_64x4:  *w = 64; *h = 4;  return true;
   default: return false;
   }
}

/* Superblock tiling factor (pan_afbc_tile_size). Tiling needs arch >= 7. */
static inline uint32_t
mali_afbc_tile_size(unsigned bpp_bits, uint64_t modifier)
{
   if (!(modifier & MALI_AFBC_MOD_TILED)) return 1;
   return bpp_bits <= 32 ? 8 : 4;
}

static inline uint32_t
mali_afbc_header_align(unsigned arch, uint64_t modifier)
{
   return (modifier & MALI_AFBC_MOD_TILED) ? 4096 : arch >= 6 ? 128 : 64;
}

static inline uint32_t
mali_afbc_row_stride_align(unsigned arch, unsigned bpp_bits, uint64_t modifier)
{
   if (arch <= 7 || !(modifier & MALI_AFBC_MOD_TILED)) return 16;
   return bpp_bits <= 32 ? 1024 : 256;
}

/* Mirrors pan_afbc_can_tile (arch>=7) and pan_afbc_can_split (arch>=6). */
static inline bool
mali_afbc_modifier_ok(unsigned arch, uint64_t modifier, unsigned bpp_bits)
{
   uint32_t w, h;
   if (!mali_afbc_sb_size(modifier, &w, &h)) return false;
   if ((modifier & MALI_AFBC_MOD_TILED) && arch < 7) return false;
   if ((modifier & MALI_AFBC_MOD_SPLIT) && arch < 6) return false;
   if (bpp_bits == 0 || bpp_bits % 8 || (bpp_bits / 8) * 16 % 16) return false; /* 4x4 subblock payload must be 16 B multiple */
   return true;
}

/* Layout for one mip level of one plane. Returns 0 or -1 (invalid). Matches pan_mod_afbc_init_slice_layout's
 * non-explicit (non-WSI) path with offset 0. */
static inline int
mali_afbc_layout(const struct mali_afbc_params *p, struct mali_afbc_layout *o)
{
   *o = (struct mali_afbc_layout){0};
   if (!p->width || !p->height || !p->depth || !mali_afbc_modifier_ok(p->arch, p->modifier, p->bpp_bits)) return -1;
   mali_afbc_sb_size(p->modifier, &o->sb_w, &o->sb_h);
   o->tile = mali_afbc_tile_size(p->bpp_bits, p->modifier);

   /* Render block: height rounded to 16 (GPU renders 16x16 tiles); wide blocks keep their width. */
   uint32_t aw = o->sb_w, ah = (uint32_t)mali_afbc_align(o->sb_h, 16);
   if (p->modifier & MALI_AFBC_MOD_TILED) {
      aw = (uint32_t)mali_afbc_align(aw, (uint64_t)o->sb_w * o->tile);
      ah = (uint32_t)mali_afbc_align(ah, (uint64_t)o->sb_h * o->tile);
   }
   /* Every product below is formed in 64 bits and range-checked BEFORE it is narrowed or stored: a wrapped value would return success
    * with a zero or tiny size (huge extents must be rejected, never under-allocated). */
   const uint64_t aw64 = mali_afbc_align(p->width, aw), ah64 = mali_afbc_align(p->height, ah);
   if (aw64 > UINT32_MAX || ah64 > UINT32_MAX) return -1;
   o->aligned_w = (uint32_t)aw64;
   o->aligned_h = (uint32_t)ah64;

   uint32_t ra = mali_afbc_row_stride_align(p->arch, p->bpp_bits, p->modifier);
   uint64_t rs = (uint64_t)(o->aligned_w / o->sb_w) * o->tile * MALI_AFBC_HEADER_BYTES;
   const uint64_t hrs = mali_afbc_align(rs, ra);
   if (hrs > UINT32_MAX) return -1;
   o->header_row_stride = (uint32_t)hrs;
   o->stride_blocks = o->header_row_stride / (MALI_AFBC_HEADER_BYTES * o->tile);
   const uint64_t nb = (uint64_t)o->stride_blocks * (o->aligned_h / o->sb_h);
   if (nb > UINT32_MAX || nb * MALI_AFBC_HEADER_BYTES > UINT32_MAX) return -1;   /* also the existing header-size limit */
   o->nblocks = (uint32_t)nb;
   o->header_size = nb * MALI_AFBC_HEADER_BYTES;
   o->offset_align = mali_afbc_header_align(p->arch, p->modifier);
   o->body_offset = mali_afbc_align(o->header_size, o->offset_align);
   o->payload_per_sb = (uint64_t)o->sb_w * o->sb_h * (p->bpp_bits / 8);
   if (nb > (UINT64_MAX - o->body_offset) / o->payload_per_sb) return -1;
   o->slice_stride = o->body_offset + nb * o->payload_per_sb;
   if (o->slice_stride > UINT64_MAX / p->depth) return -1;
   o->size = o->slice_stride * p->depth;
   return 0;
}

/* Optional header-area bytes to zero for debug determinism. PanVK has no explicit header init (grep of panvk/pan_desc
 * finds none): every Vulkan-valid first use writes whole superblocks (clear, store, graphics upload) and reading
 * undefined contents is invalid, so zeroing is NOT required. Kept as a debug aid only (unverified on hardware). */
static inline uint64_t
mali_afbc_init_bytes(const struct mali_afbc_layout *l)
{
   return l->header_size;
}

/* Memory-saving upper bound: compressed bandwidth ratio does NOT shrink the allocation. */
static inline double
mali_afbc_bw_estimate(double ratio_payload, const struct mali_afbc_layout *l)
{
   /* bytes touched per full-frame write = headers + ratio * payload */
   return (double)l->header_size + ratio_payload * (double)(l->nblocks * l->payload_per_sb);
}

/* ---- Stage 1: eligibility (which images may be AFBC). Pure function over plain integers (Vulkan enum values are
 * written as numbers so this header stays standalone). Mirrors panvk_image.c:176-235 for COLOUR images. Nothing calls
 * this yet; the result is advisory until the later stages land. Returns the reason an image must stay linear, or 0. */
enum mali_afbc_reject {
   MALI_AFBC_OK = 0,
   MALI_AFBC_REJ_DISABLED,        /* MALI_AFBC not 1 */
   MALI_AFBC_REJ_ARCH,            /* arch < 7 (v10/v11/v13 only targeted) */
   MALI_AFBC_REJ_TILING,          /* not VK_IMAGE_TILING_OPTIMAL */
   MALI_AFBC_REJ_TYPE,            /* not 2D */
   MALI_AFBC_REJ_SAMPLES,         /* MSAA */
   MALI_AFBC_REJ_USAGE,           /* STORAGE / HOST_TRANSFER (compute and CPU cannot touch AFBC) */
   MALI_AFBC_REJ_FLAGS,           /* DISJOINT, ALIAS, SPARSE_*, PROTECTED-less combos we do not handle */
   MALI_AFBC_REJ_FORMAT,          /* not in the colour table (float, integer, compressed, multiplanar, depth) */
   MALI_AFBC_REJ_EXTENT,          /* tiny images: not worth it / header overhead */
};

struct mali_afbc_image_info {
   unsigned arch;
   unsigned tiling;        /* VkImageTiling: 0 optimal, 1 linear, 1000158000 drm modifier */
   unsigned image_type;    /* VkImageType: 0=1D 1=2D 2=3D */
   unsigned samples;       /* sample count, 1 = single */
   uint32_t usage;         /* VkImageUsageFlags */
   uint32_t flags;         /* VkImageCreateFlags */
   unsigned format;        /* VkFormat */
   uint32_t width, height;
   bool afbc_env;          /* mali_afbc_enabled() (injected so tests do not depend on the environment) */
};

#define MALI_AFBC_USAGE_STORAGE        0x08u
#define MALI_AFBC_USAGE_HOST_TRANSFER  0x400000u   /* VK_IMAGE_USAGE_HOST_TRANSFER_BIT (EXT/1.4) */
#define MALI_AFBC_FLAG_SPARSE_BINDING  0x1u
#define MALI_AFBC_FLAG_SPARSE_RESIDENT 0x2u
#define MALI_AFBC_FLAG_SPARSE_ALIASED  0x4u
#define MALI_AFBC_FLAG_ALIAS           0x400u
#define MALI_AFBC_FLAG_DISJOINT        0x200u

/* Colour format table: bits per pixel for AFBC-capable formats, 0 if not supported. Float/integer-only, depth and
 * compressed formats are absent on purpose (pan_afbc_supports_format). */
static inline unsigned
mali_afbc_format_bpp(unsigned fmt)
{
   switch (fmt) {
   case 4: case 5:              return 16;   /* R5G6B5 UNORM/ B5G6R5 (pack16) */
   case 9: case 15:             return 8;    /* R8_UNORM, R8_SRGB */
   case 16: case 22:            return 16;   /* R8G8_UNORM, R8G8_SRGB */
   case 37: case 43:            return 32;   /* R8G8B8A8_UNORM / SRGB */
   case 44: case 50:            return 32;   /* B8G8R8A8_UNORM / SRGB */
   case 58: case 64:            return 32;   /* A2R10G10B10 / A2B10G10R10 UNORM */
   case 70:                     return 16;   /* R16_UNORM */
   default:                     return 0;
   }
}

static inline enum mali_afbc_reject
mali_afbc_image_eligible(const struct mali_afbc_image_info *i, unsigned *bpp_out)
{
   if (!i->afbc_env) return MALI_AFBC_REJ_DISABLED;
   if (i->arch < 7) return MALI_AFBC_REJ_ARCH;
   if (i->tiling != 0) return MALI_AFBC_REJ_TILING;
   if (i->image_type != 1) return MALI_AFBC_REJ_TYPE;
   if (i->samples != 1) return MALI_AFBC_REJ_SAMPLES;
   if (i->usage & (MALI_AFBC_USAGE_STORAGE | MALI_AFBC_USAGE_HOST_TRANSFER)) return MALI_AFBC_REJ_USAGE;
   if (i->flags & (MALI_AFBC_FLAG_DISJOINT | MALI_AFBC_FLAG_ALIAS | MALI_AFBC_FLAG_SPARSE_BINDING |
                   MALI_AFBC_FLAG_SPARSE_RESIDENT | MALI_AFBC_FLAG_SPARSE_ALIASED))
      return MALI_AFBC_REJ_FLAGS;
   unsigned bpp = mali_afbc_format_bpp(i->format);
   if (!bpp) return MALI_AFBC_REJ_FORMAT;
   if (i->width < 16 || i->height < 16) return MALI_AFBC_REJ_EXTENT;
   if (bpp_out) *bpp_out = bpp;
   return MALI_AFBC_OK;
}


/* ---- Stage 2 (host part): whole-image layout, mip chain + array layers. Within one array layer the mip levels are
 * packed back to back, each level start aligned to its offset_align; layers repeat at array_stride (aligned to the
 * largest level alignment). Level extent = max(1, dim >> level). Unused until mali_image.c is wired (default off). */
#define MALI_AFBC_MAX_LEVELS 16

struct mali_afbc_image_layout {
   uint32_t levels, layers;
   uint64_t level_offset[MALI_AFBC_MAX_LEVELS];  /* from layer start */
   struct mali_afbc_layout level[MALI_AFBC_MAX_LEVELS];
   uint64_t array_stride;
   uint64_t size;
   uint32_t base_align;    /* required alignment of the image base address */
};

static inline int
mali_afbc_image_layout(unsigned arch, uint64_t modifier, unsigned bpp_bits, uint32_t width, uint32_t height,
                       uint32_t levels, uint32_t layers, struct mali_afbc_image_layout *o)
{
   *o = (struct mali_afbc_image_layout){0};
   if (!levels || levels > MALI_AFBC_MAX_LEVELS || !layers || !width || !height) return -1;
   o->levels = levels; o->layers = layers;
   uint64_t off = 0;
   for (uint32_t l = 0; l < levels; l++) {
      struct mali_afbc_params p = {arch, modifier, bpp_bits, width >> l ? width >> l : 1, height >> l ? height >> l : 1, 1};
      if (mali_afbc_layout(&p, &o->level[l])) return -1;
      if (o->level[l].offset_align > o->base_align) o->base_align = o->level[l].offset_align;
      off = mali_afbc_align(off, o->level[l].offset_align);
      o->level_offset[l] = off;
      if (o->level[l].size > UINT64_MAX - off) return -1;
      off += o->level[l].size;
   }
   o->array_stride = mali_afbc_align(off, o->base_align);
   if (o->array_stride > UINT64_MAX / layers) return -1;
   o->size = o->array_stride * layers;
   return 0;
}

/* ---- Stage 3 (host part): AFBC RGB Render Target descriptor fields, v9+ packing (v10/v11/v13), genxml v13.xml:2476
 * ("AFBC RGB Render Target", 16 words). Only the AFBC-specific words are produced; the linear RT words (formats, swizzle,
 * clear colour, write enable) stay as the existing emission sets them. Pure, unused until the frame emission is wired. */
#define MALI_AFBC_BLOCK_FORMAT_AFBC       12u   /* enum Block Format */
#define MALI_AFBC_BLOCK_FORMAT_AFBC_TILED 13u
#define MALI_AFBC_CM_R8        0u
#define MALI_AFBC_CM_R16       1u
#define MALI_AFBC_CM_R8G8      3u
#define MALI_AFBC_CM_R5G6B5    4u
#define MALI_AFBC_CM_R8G8B8A8  10u
#define MALI_AFBC_CM_R10G10B10A2 11u

/* VkFormat -> AFBC compression mode; -1 when not an AFBC colour format (same table as mali_afbc_format_bpp). */
static inline int
mali_afbc_compression_mode(unsigned fmt)
{
   switch (fmt) {
   case 4: case 5:   return MALI_AFBC_CM_R5G6B5;
   case 9: case 15:  return MALI_AFBC_CM_R8;
   case 16: case 22: return MALI_AFBC_CM_R8G8;
   case 37: case 43: case 44: case 50: return MALI_AFBC_CM_R8G8B8A8;
   case 58: case 64: return MALI_AFBC_CM_R10G10B10A2;
   case 70:          return MALI_AFBC_CM_R16;
   default:          return -1;
   }
}

/* Effective tile block for a tile of `tile_px` pixels (pan_effective_tile_block_size): square or twice as wide as high. */
static inline void
mali_afbc_tile_block(unsigned tile_px, uint32_t *w, uint32_t *h)
{
   unsigned sh = 0;
   while ((2u << sh) <= tile_px) sh++;   /* floor(log2) */
   *h = 1u << (sh >> 1);
   *w = 1u << ((sh + 1) >> 1);
}

/* reverse_issue_order is required when the effective tile is smaller than the superblock (v7+). */
static inline bool
mali_afbc_needs_reverse_issue(uint64_t modifier, unsigned tile_px)
{
   uint32_t sw, sh, tw, th;
   if (!mali_afbc_sb_size(modifier, &sw, &sh)) return false;
   mali_afbc_tile_block(tile_px, &tw, &th);
   return tw < sw || th < sh;
}

struct mali_afbc_rt_words {
   uint32_t w1_block_format;  /* OR into word 1 bits 8..11 (Block Format) */
   uint32_t w2;               /* word 2 complete: YTR, split, wide, reverse issue order, compression mode (bits 10..15) */
   uint64_t header_va;        /* words 8-9 */
   uint32_t row_stride;       /* word 10: header row stride, bytes */
   uint32_t body_offset;      /* word 11: bytes from header start */
};

/* Returns 0, or -1 for an invalid modifier/format/layout or a body offset beyond 32 bits. `level` is the layout of the rendered mip. */
static inline int
mali_afbc_rt_words(uint64_t modifier, unsigned vk_format, unsigned tile_px, uint64_t header_va,
                   const struct mali_afbc_layout *level, struct mali_afbc_rt_words *o)
{
   *o = (struct mali_afbc_rt_words){0};
   uint32_t sw, sh;
   int cm = mali_afbc_compression_mode(vk_format);
   if (cm < 0 || !mali_afbc_sb_size(modifier, &sw, &sh) || level->body_offset > UINT32_MAX) return -1;
   o->w1_block_format = (modifier & MALI_AFBC_MOD_TILED) ? MALI_AFBC_BLOCK_FORMAT_AFBC_TILED : MALI_AFBC_BLOCK_FORMAT_AFBC;
   o->w2 = ((modifier & MALI_AFBC_MOD_YTR) ? 1u << 0 : 0) | ((modifier & MALI_AFBC_MOD_SPLIT) ? 1u << 1 : 0) |
           (sw > 16 ? 1u << 2 : 0) | (mali_afbc_needs_reverse_issue(modifier, tile_px) ? 1u << 3 : 0) | ((uint32_t)cm << 10);
   o->header_va = header_va;
   o->row_stride = level->header_row_stride;
   o->body_offset = (uint32_t)level->body_offset;
   return 0;
}

/* Patch the words above into a 16-word RT descriptor that already carries the linear fields. Word 1 block-format bits are replaced. */
static inline void
mali_afbc_rt_patch(uint32_t rt[16], const struct mali_afbc_rt_words *w)
{
   rt[1] = (rt[1] & ~(0xfu << 8)) | (w->w1_block_format << 8);
   rt[2] = w->w2;
   rt[8] = (uint32_t)w->header_va;
   rt[9] = (uint32_t)(w->header_va >> 32);
   rt[10] = w->row_stride;
   rt[11] = w->body_offset;
}

#endif
