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

#endif
