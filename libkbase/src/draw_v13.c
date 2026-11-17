// SPDX-License-Identifier: MIT
/* Arch-13 descriptor packers. Sources: Mesa genxml/v13.xml (line numbers in V13-NEXT.md), panvk csf/panvk_vX_cmd_draw.c. */
#include <string.h>

#include "kbase_draw_v13.h"

static void w64(uint32_t *d, unsigned word, uint64_t v) { d[word] = (uint32_t)v; d[word + 1] = (uint32_t)(v >> 32); }

void kb_v13_tiler_ctx(uint32_t *d, unsigned fb_w, unsigned fb_h, uint32_t hmask, unsigned tile_log2, uint64_t heap_desc_va, uint64_t geom_buf)
{
	memset(d, 0, KB_TILER_CTX_SIZE);
	/* v13.xml:2633-2637: hierarchy mask 2:0 (13b), first provoking vertex 2:18, Effective Tile Size 2:20 (4b, log2). NEW vs v11. */
	d[2] = (hmask & 0x1fff) | (1u << 18) | ((tile_log2 & 0xfu) << 20);
	d[3] = (fb_w - 1) | ((fb_h - 1) << 16);     /* 2638-2639 */
	d[4] = 0;                                    /* layer count - 1 = 0, offset 0 (2640-2641) */
	w64(d, 6, heap_desc_va);                     /* 2642 */
	w64(d, 8, geom_buf);                         /* 2643-2644: size>>12 in 11:0, address>>12 in 63:12 */
}

void kb_v13_spd(uint32_t *d, unsigned stage, uint64_t binary_va, unsigned work_regs, uint64_t preload, unsigned max_warps)
{
	memset(d, 0, KB_SPD_SIZE);
	d[0] = 8u | (stage << 4);                                /* type Shader, stage (2090-2092) */
	if (stage == 2) d[0] |= 1u << 8;                         /* fragment coverage bitmask type GL (0:8). v11 vertex warp-limit bits are gone */
	d[0] |= (work_regs <= 32 ? 2u : 0u) << 30;               /* register allocation (0:30), as v11 */
	d[1] = ((uint32_t)(preload >> 48) & 0xffffu) | ((max_warps & 0xffffu) << 16);   /* Preload 1:0, Max Warps 1:16 (GUESS 0) */
	w64(d, 2, binary_va);
}

void kb_v13_zsd_depth(uint32_t *d, enum kb_func func, int write)
{
	memset(d, 0, KB_ZSD_SIZE);
	d[0] = 7u;                                               /* type Depth/stencil; stencil off (v13.xml:2678-2689) */
	/* 4:22 depth cull, 4:23 clamp [0,1] (1), 4:25 fixed-function depth (2), 4:27 write, 4:29 func. Bit 21 is gone (v11 sep. dependency). */
	d[4] = (1u << 22) | (1u << 23) | (2u << 25) | (((uint32_t)func & 7u) << 29) | (write ? 1u << 27 : 0u);
}

uint32_t kb_v13_dcd2(unsigned rd, unsigned wr, int nss, int nsd, int hsr_cull, int hsr_culled)
{
	/* v13.xml:2753-2763; bits 26/27 HSR can cull / can be culled, 28 z write or stencil, 30 HSR update, 31 prepass kill (all 0 = early) */
	return (rd & 0xfffu) | ((wr & 0xfffu) << 12) | (nss ? 1u << 24 : 0u) | (nsd ? 1u << 25 : 0u) | (hsr_cull ? 1u << 26 : 0u) |
	       (hsr_culled ? 1u << 27 : 0u);
}

uint64_t kb_v13_viewport_hi(unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
	return (uint64_t)(x0 | (y0 << 16)) | ((uint64_t)(x1 | (y1 << 16)) << 32);   /* v13.xml:2272-2276 words 0,1 */
}

uint64_t kb_v13_viewport_lo(float mn, float mx)
{
	uint32_t a, b;
	memcpy(&a, &mn, 4); memcpy(&b, &mx, 4);
	return (uint64_t)a | ((uint64_t)b << 32);                                   /* words 2,3 */
}

uint32_t kb_v13_prim_flags_triangles(void)
{
	return 8u | (1u << 16) | (1u << 17) | (1u << 22);   /* Triangles, depth cull bits, FIFO basic; no secondary shader / point size (GUESS bit layout from v11) */
}

void kb_v13_attr_varying(uint32_t *d, uint32_t pixfmt, uint32_t varying_offset, uint32_t attr_stride)
{
	kb_draw_attr_varying(d, pixfmt, varying_offset, attr_stride);
	d[3] = 1;                                                /* buffer_index 1 on arch >= 12 (panvk_vX_cmd_draw.c:504) */
}

void kb_v13_fbd_hsr(uint32_t *fbd, int filter, int hier, int interleave, int prepass)
{
	fbd[0] &= ~(0xfu << 11);                                 /* v13.xml:2286-2289, pan_desc.c:1573-1587 */
	fbd[0] |= ((prepass ? 1u : 0u) << 11) | ((interleave ? 1u : 0u) << 12) | ((filter ? 1u : 0u) << 13) | ((hier ? 1u : 0u) << 14);
}
