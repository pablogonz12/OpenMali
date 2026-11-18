/* SPDX-License-Identifier: MIT */
/*
 * kbase_fb: Mali Valhall v11 (arch 11.x, e.g. G615) framebuffer descriptor packing, v11 only.
 * One framebuffer descriptor (FBD, 128 B) followed by one RGB render-target descriptor (64 B).
 * Field layouts are documented from Mesa's MIT genxml (src/panfrost/genxml/v11.xml,
 * "Framebuffer Parameters" and "RGB Render Target"); the packers are our own code.
 * Format used: R8G8B8A8, linear block format, row stride caller-chosen (multiple of 16).
 */
#ifndef KBASE_FB_H
#define KBASE_FB_H

#include <stddef.h>
#include <stdint.h>

#define KB_FB_FBD_SIZE   128u
#define KB_FB_RTD_SIZE   64u
#define KB_FB_DESC_SIZE  (KB_FB_FBD_SIZE + KB_FB_RTD_SIZE)  /* 192, 64-byte aligned in GPU memory */

/* Depth buffer (ZS Target in the ZS/CRC extension, 64 B between the FBD and the RTs). 0 = no depth attachment (default). */
enum kb_zs_format { KB_ZS_NONE = 0, KB_ZS_D32F = 1, KB_ZS_D24X8 = 2, KB_ZS_D16 = 3 };
#define KB_FB_ZSEXT_SIZE      64u
#define KB_FB_DESC_SIZE_ZS    (KB_FB_FBD_SIZE + KB_FB_ZSEXT_SIZE + KB_FB_RTD_SIZE)   /* 256 */
unsigned kb_zs_bytes_per_pixel(enum kb_zs_format f);

struct kb_fb_params {
	unsigned width, height;       /* pixels, 1..65536 */
	unsigned rt_none;             /* no colour attachment: the render target is declared but Write Enable is off, no buffer */
	uint64_t rt_gpu_va;           /* colour buffer, 64-byte aligned */
	uint32_t row_stride;          /* bytes between rows, multiple of 16 */
	uint32_t surface_stride;      /* bytes per layer (0 = stride*height) */
	uint8_t clear_r, clear_g, clear_b, clear_a;
	uint64_t sample_locations_va; /* sample-position table (pattern 0 at offset 0); PanVK always sets it */
	uint64_t tiler_va;            /* Tiler Context pointer; 0 = geometry-less frame */
	/* experiment knobs (defaults 0 = the documented configuration) */
	unsigned no_clean_tile_write; /* 0 (default): Clean Tile Write Enable set, empty tiles are written */
	unsigned block_format;        /* 0 = linear (2) , 1 = tiled u-interleaved (1) */
	/* depth attachment (genxml v11 "ZS Target" + FBD Z fields). Zero-initialised = none: output stays 192 B, identical to before. */
	enum kb_zs_format zs_format;
	uint64_t zs_gpu_va;           /* 64-byte aligned */
	uint32_t zs_row_stride;       /* bytes, multiple of 16; 0 = width * bytes per pixel rounded up to 16 */
	float clear_depth;            /* FBD "Z Clear" */
	unsigned zs_no_store;         /* 1: Z Write Enable off (depth only used inside the frame, nothing written back) */
	/* stencil attachment (genxml v10+ "S Target" in the same ZS/CRC extension, words 0:16.. and 12..15; FBD "S Clear" 12:0, "S Write Enable" 12:8).
	 * Independent of zs_format: stencil-only (S8) frames use the extension with the Z fields idle. */
	unsigned has_stencil;
	uint64_t s_gpu_va;            /* 64-byte aligned, S8 plane (1 byte per pixel) */
	uint32_t s_row_stride;        /* bytes, multiple of 16; 0 = width rounded up to 16 */
	uint8_t clear_stencil;        /* FBD "S Clear" */
	unsigned s_no_store;          /* 1: S Write Enable off */
	/* non-RGBA8 colour target (0 = RGBA8 as before): Internal Format, writeback colour format, 12-bit RT swizzle (0 = identity),
	 * Color Buffer Allocation (>>10 units) and the four raw clear words. */
	unsigned rt_internal, rt_wb, rt_swizzle, rt_alloc;
	uint32_t rt_clear[4];
	/* MSAA (all 0 = single-sampled, as before). samples_log2: 0 or 2 (4x). sample_pattern: enum mali_sample_pattern (2 = rotated 4x).
	 * rt_msaa / zs_msaa: RT writeback / ZS "MSAA" mode (0 single, 1 average = resolve into rt_gpu_va, 3 layered = store all samples
	 * as consecutive surface_stride planes). tile_log2: Effective Tile Size override (0 = 10, 32x32; 4x needs 8 = 16x16). */
	unsigned samples_log2, sample_pattern, rt_msaa, zs_msaa, tile_log2;
	unsigned arch;                /* GPU arch major (kb_props()->arch); 0 = assume 11. Layouts below are v11 only: other values
	                               * (e.g. 13 on Mali-G925) still pack the v11 layout but warn once on stderr. */
};

/* Fill out[KB_FB_DESC_SIZE] (KB_FB_DESC_SIZE_ZS with a depth attachment; layout FBD | [ZS ext] | RT). Returns 0 or -EINVAL. */
int kb_fb_pack(uint8_t *out, const struct kb_fb_params *p);
/* Value for RUN_FRAGMENT staging register FBD_POINTER (r40:r41): descriptor address | type 1 | (rt_count-1)<<2. */
uint64_t kb_fb_pointer(uint64_t fbd_gpu_va, unsigned rt_count);
/* Same with the ZS/CRC extension bit (pointer bit 1) set when the descriptor carries a ZS target. */
uint64_t kb_fb_pointer_ex(uint64_t fbd_gpu_va, unsigned rt_count, int has_zs_ext);
/* Staging registers r42 / r43: bound min / max (inclusive), x in 15:0, y in 31:16. */
uint32_t kb_fb_bbox_min(unsigned x, unsigned y);
uint32_t kb_fb_bbox_max(unsigned x, unsigned y);
/* Clear colour word replicated into the four RT clear words: little-endian R8G8B8A8 = r | g<<8 | b<<16 | a<<24. */
uint32_t kb_fb_clear_word(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* TODO(v13): the descriptor packers (fb.c, draw.c) encode the Mesa genxml v11 layouts. Mesa has separate v12/v13 genxml
 * (Framebuffer/Render Target/Tiler Context/SPD may differ) that we have not compared. For arch not in {0, 11} this warns
 * once on stderr and carries on with the v11 bit layout. */
void kb_arch_warn_v11_only(unsigned arch, const char *who);

#endif
