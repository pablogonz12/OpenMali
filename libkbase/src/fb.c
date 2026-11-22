// SPDX-License-Identifier: MIT
/* kbase_fb: v11 framebuffer + render-target descriptor packers. See kbase_fb.h. */
#include <errno.h>
#include <string.h>

#include "kbase_fb.h"

/* Insert `val` into bits [shift, shift+width) of 32-bit word `w` of the descriptor. */
static void put(uint32_t *d, unsigned w, unsigned shift, unsigned width, uint64_t val)
{
	uint32_t mask = width >= 32 ? 0xffffffffu : ((1u << width) - 1u);
	d[w] |= ((uint32_t)val & mask) << shift;
}

static uint32_t ubits_f(float v) { uint32_t u; memcpy(&u, &v, 4); return u; }

uint32_t kb_fb_clear_word(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
}

uint64_t kb_fb_pointer(uint64_t fbd, unsigned rt_count)
{
	/* low bits of the 64-byte aligned address: 0 type (1 = framebuffer), 1 ZS/CRC extension present, 2..4 RT count - 1 */
	return fbd | 1u | ((uint64_t)(rt_count - 1) << 2);
}

uint64_t kb_fb_pointer_ex(uint64_t fbd, unsigned rt_count, int zs_ext)
{
	return kb_fb_pointer(fbd, rt_count) | (zs_ext ? 2u : 0u);   /* FRAMEBUFFER_POINTER "ZS CRC Extension Present" = bit 1, as PanVK's FBD|3 */
}

unsigned kb_zs_bytes_per_pixel(enum kb_zs_format f)
{
	return f == KB_ZS_D16 ? 2u : (f == KB_ZS_NONE ? 0u : 4u);
}

uint32_t kb_fb_bbox_min(unsigned x, unsigned y) { return (x & 0xffff) | (y & 0xffff) << 16; }
uint32_t kb_fb_bbox_max(unsigned x, unsigned y) { return (x & 0xffff) | (y & 0xffff) << 16; }

int kb_fb_pack(uint8_t *out, const struct kb_fb_params *p)
{
	if (!p->width || !p->height || p->width > 65536 || p->height > 65536 || (!p->rt_none && (p->rt_gpu_va & 63)) || (p->row_stride & 15))
		return -EINVAL;
	kb_arch_warn_v11_only(p->arch, "kb_fb_pack");
	/* TODO(v13): FBD / RGB RT bit positions below are Mesa genxml v11.xml; v13 not compared (no values invented here). */
	const int zs = p->zs_format != KB_ZS_NONE;
	const int st = p->has_stencil != 0;
	const int ext = zs || st;
	uint32_t s_row = p->s_row_stride ? p->s_row_stride : (p->width + 15u) & ~15u;
	if (st && ((p->s_gpu_va & 63) || (s_row & 15))) return -EINVAL;
	uint32_t zs_row = p->zs_row_stride ? p->zs_row_stride : (p->width * kb_zs_bytes_per_pixel(p->zs_format) + 15u) & ~15u;
	if (zs && ((p->zs_gpu_va & 63) || (zs_row & 15) || p->zs_format > KB_ZS_D16)) return -EINVAL;
	if (p->rt_extra_count > KB_FB_MAX_EXTRA_RT) return -EINVAL;
	for (unsigned n = 0; n < p->rt_extra_count; n++)
		if ((p->rt_extra[n].va & 63) || (p->rt_extra[n].row_stride & 15)) return -EINVAL;
	memset(out, 0, (ext ? KB_FB_DESC_SIZE_ZS : KB_FB_DESC_SIZE) + p->rt_extra_count * KB_FB_RTD_SIZE);
	uint32_t *f = (uint32_t *)(void *)out;                 /* FBD: Framebuffer Parameters */
	uint32_t *x = (uint32_t *)(void *)(out + KB_FB_FBD_SIZE); /* ZS/CRC extension, only if zs */
	uint32_t *r = (uint32_t *)(void *)(out + KB_FB_FBD_SIZE + (ext ? KB_FB_ZSEXT_SIZE : 0)); /* RGB Render Target */

	put(f, 4, 0, 32, (uint32_t)p->sample_locations_va);  /* Sample Locations pointer */
	put(f, 5, 0, 32, (uint32_t)(p->sample_locations_va >> 32));
	/* word 0: Pre Frame 0/1 (0:0, 0:3), Post Frame (0:6): all 0 = never. Words 2..7: frame argument, sample locations,
	 * frame shader DCDs: unused (0). */
	put(f, 8, 0, 16, p->width - 1);                          /* Width - 1 */
	put(f, 8, 16, 16, p->height - 1);                        /* Height - 1 */
	put(f, 9, 0, 16, 0);                                     /* Bound Min X */
	put(f, 9, 16, 16, 0);                                    /* Bound Min Y */
	put(f, 10, 0, 16, p->width - 1);                         /* Bound Max X (inclusive) */
	put(f, 10, 16, 16, p->height - 1);                       /* Bound Max Y (inclusive) */
	/* word 11 */
	put(f, 11, 0, 3, p->samples_log2);                       /* Sample Count log2 */
	put(f, 11, 3, 3, p->sample_pattern);                     /* Sample Pattern (0 single, 2 rotated 4x) */
	put(f, 11, 6, 3, 2);                                     /* Tie-break rule: MINUS_180_IN_0_OUT (Mesa default) */
	put(f, 11, 9, 4, p->tile_log2 ? p->tile_log2 : 10);                                    /* Effective Tile Size log2(1024 px) = 32x32, as PanVK (matches hierarchy mask 0xfe, smallest bin 32) */
	put(f, 11, 14, 1, !p->provoking_last);                 /* First provoking vertex (PanVK sets it unless VK_PROVOKING_VERTEX_MODE_LAST; also in the tiler context) */
	put(f, 11, 15, 1, 0);                                    /* Point sprite coord origin max Y: PanVK 0 */
	put(f, 11, 19, 4, p->rt_extra_count);                    /* Render Target Count - 1 */
	put(f, 11, 24, 8, p->rt_alloc ? p->rt_alloc : 4);                                    /* Color Buffer Allocation >> 10: 4 B * 1024 px = 4096 */
	put(f, 12, 29, 1, 1);                                    /* Empty Tile Write Enable (PanVK sets bits 31:28 of word 12) */
	/* Z Internal Format (v11.xml 12:16): D16 0, D24 1, D32 2; D24 when no ZS target attached, as pan_fb.c */
	put(f, 12, 16, 2, !zs ? 1 : p->zs_format == KB_ZS_D32F ? 2 : p->zs_format == KB_ZS_D24X8 ? 1 : 0);
	if (ext) put(f, 12, 21, 1, 1);                           /* Has ZS CRC Extension */
	if (st) {
		put(f, 12, 0, 8, p->clear_stencil);                  /* S Clear */
		put(f, 12, 8, 1, !p->s_no_store);                    /* S Write Enable */
		/* S Target (v10.xml): word 0 bits 16 Write Format S8 (1), 20 Block Format linear, 24 MSAA single, 31 Clean Tile Write Enable (shared bit with ZS) */
		put(x, 0, 16, 4, 1);
		put(x, 0, 20, 4, 2);
		put(x, 0, 24, 2, 0);
		put(x, 0, 31, 1, 1);
		put(x, 12, 0, 32, (uint32_t)p->s_gpu_va);            /* Base */
		put(x, 13, 0, 32, (uint32_t)(p->s_gpu_va >> 32));
		put(x, 14, 0, 32, s_row);                            /* Row Stride */
		put(x, 15, 0, 32, (uint32_t)((uint64_t)s_row * p->height));   /* Surface Stride */
	}
	if (zs) {
		put(f, 12, 18, 1, !p->zs_no_store);                  /* Z Write Enable */
		put(f, 13, 0, 32, ubits_f(p->clear_depth));          /* Z Clear (float) */
		/* ZS Target (v11.xml "ZS Target"; pan_desc.c pan_emit_linear_zs_attachment) */
		put(x, 0, 0, 4, p->zs_format == KB_ZS_D32F ? 14 : p->zs_format == KB_ZS_D24X8 ? 4 : 1);   /* Write Format D32 / D24X8 / D16 */
		put(x, 0, 4, 4, 2);                                  /* Block Format: Linear */
		put(x, 0, 8, 2, 0);                                  /* MSAA single */
		put(x, 0, 31, 1, 1);                                 /* Clean Tile Write Enable: write the cleared depth of empty tiles */
		put(x, 8, 0, 32, (uint32_t)p->zs_gpu_va);            /* Base */
		put(x, 9, 0, 32, (uint32_t)(p->zs_gpu_va >> 32));
		put(x, 10, 0, 27, zs_row);                           /* Row Stride */
		put(x, 10, 27, 5, ((uint64_t)zs_row * p->height) >> 32);   /* Surface Stride hi */
		put(x, 11, 0, 32, (uint32_t)((uint64_t)zs_row * p->height));   /* (per-sample plane; MSAA samples are consecutive) */   /* Surface Stride */
	}
	/* word 13: Z clear = 0. Words 14..15: Tiler Context pointer (0 = none). Words 16..17: VRS (0). */
	put(f, 14, 0, 32, (uint32_t)p->tiler_va);
	put(f, 15, 0, 32, (uint32_t)(p->tiler_va >> 32));

	/* RGB Render Target 0, then rt_extra_count more 64 B descriptors (Internal Buffer Offset = running sum of tile bytes). */
	for (unsigned n = 0; n <= p->rt_extra_count; n++) {
		uint32_t *q = r + 16u * n;
		uint64_t va = n ? p->rt_extra[n - 1].va : p->rt_gpu_va;
		uint32_t rs = n ? p->rt_extra[n - 1].row_stride : p->row_stride;
		uint32_t ss = n ? p->rt_extra[n - 1].surface_stride : p->surface_stride;
		unsigned in = n ? p->rt_extra[n - 1].internal : p->rt_internal;
		unsigned wb = n ? p->rt_extra[n - 1].wb : p->rt_wb;
		unsigned sw = n ? p->rt_extra[n - 1].swizzle : p->rt_swizzle;
		unsigned ibo = n ? p->rt_extra[n - 1].internal_offset : 0;
		const uint32_t *cl = n ? p->rt_extra[n - 1].clear : p->rt_clear;
		put(q, 0, 0, 4, 0);                                  /* Writeback Mode: compat */
		put(q, 0, 4, 12, ibo >> 4);                          /* Internal Buffer Offset >> 4 */
		put(q, 0, 26, 6, in ? in : 1);                       /* Internal Format: R8G8B8A8 */
		put(q, 1, 0, 1, !p->rt_none && !(n ? p->rt_extra[n - 1].no_store : p->rt_no_store));   /* Write Enable, per target */
		put(q, 1, 3, 5, in ? wb : 19);                       /* Writeback colour format R8G8B8A8 */
		put(q, 1, 8, 4, p->block_format ? 1 : 2);            /* Block format: 2 linear, 1 tiled u-interleaved */
		put(q, 1, 12, 2, p->rt_msaa);                        /* MSAA: 0 single, 1 average (resolve), 3 layered */
		put(q, 1, 16, 12, in ? sw : (0 | 1 << 3 | 2 << 6 | 3 << 9));   /* Swizzle RGBA identity */
		put(q, 1, 31, 1, !p->no_clean_tile_write && !(n ? p->rt_extra[n - 1].no_store : 0));   /* Clean Tile Write Enable */
		put(q, 8, 0, 32, (uint32_t)va);                      /* RT Buffer base */
		put(q, 9, 0, 32, (uint32_t)(va >> 32));
		put(q, 10, 0, 32, rs);                               /* Row stride */
		put(q, 11, 0, 32, ss ? ss : rs * p->height);         /* Surface stride */
		uint32_t c = kb_fb_clear_word(p->clear_r, p->clear_g, p->clear_b, p->clear_a);
		for (unsigned i = 0; i < 4; i++)
			/* an extra target has its own clear words; only RT0 without them falls back to the byte clear value */
			put(q, 12 + i, 0, 32, (n == 0 && !in) ? c : cl[i]);   /* Clear colour: RT0 without an internal format replicates the byte clear into all four words */
	}
	return 0;
}
