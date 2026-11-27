/* SPDX-License-Identifier: MIT */
/* Valhall v11 IDVS descriptor packers; layouts per Mesa genxml v11.xml (see include/kbase_draw.h). */
#include <stdio.h>
#include <string.h>

#include "kbase_draw.h"
#include "kbase_cs.h"

int kb_v10;   /* 1 when the GPU is arch 10 (set by the driver): SET_SB_ENTRY scoreboard select, ZSD bit 21 clear. See kbase_cs.h */

void *kb_arena_alloc_nz(struct kb_arena *a, size_t size, size_t align, uint64_t *gpu_va)
{
	size_t o = (a->off + align - 1) & ~(align - 1);
	if (a->rolling && !a->lapped && o + size > a->bo.size) { o = 0; a->lapped = 1; }   /* wrap onto the oldest job's memory */
	if (a->rolling && a->lapped ? o + size > a->start : o + size > a->bo.size) return NULL;
	a->off = o + size;
	if (gpu_va) *gpu_va = a->bo.gpu_va + o;
	return (char *)a->bo.cpu + o;
}

void *kb_arena_alloc(struct kb_arena *a, size_t size, size_t align, uint64_t *gpu_va)
{
	void *p = kb_arena_alloc_nz(a, size, align, gpu_va);
	if (p) memset(p, 0, size);
	return p;
}

void kb_arena_begin(struct kb_arena *a)
{
	a->rolling = 1;
	a->lapped = 0;
	a->off = (a->off + 63) & ~(size_t)63;
	if (a->off >= a->bo.size) a->off = 0;
	a->start = a->off;
}

void kb_arch_warn_v11_only(unsigned arch, const char *who)
{
	static int warned;
	if (arch == 0 || arch == 10 || arch == 11 || warned) return;   /* arch 10 differences: kb_v10 */
	warned = 1;
	fprintf(stderr, "libkbase: %s packs Valhall v11 descriptors, GPU is arch %u: layout UNVERIFIED, expect faults\n", who, arch);
}

static void w64(uint32_t *d, unsigned word, uint64_t v) { d[word] = (uint32_t)v; d[word + 1] = (uint32_t)(v >> 32); }

void kb_draw_tiler_heap_desc(uint32_t *d, uint64_t base, uint32_t size)
{
	memset(d, 0, KB_TILER_HEAP_DESC_SIZE);
	d[0] = 9u | (2u << 4);              /* type Buffer, buffer type Tiler heap */
	d[1] = size;
	w64(d, 2, base);
	w64(d, 4, base + 64);
	w64(d, 6, base + size);
}

/* Tiler hierarchy mask for an effective fragment tile of 1 << tile_log2 pixels (log2 of the tile AREA, as the driver's
 * env->tile_log2: 10 = 32x32, 12 = 64x64):
 * pan_desc.c GENX(pan_select_tiler_hierarchy_mask) "Disable hierarchies falling under the effective tile size".
 * Mask bit 0 = 16x16 bins, bit n = (16 << n)^2, so a 32x32 tile keeps 0xfe and a 64x64 tile keeps 0xfc (the levels below
 * the fragment tile would be walked by the tiler and never used by the fragment job). */
uint32_t kb_tiler_hmask_from_tile(unsigned tile_log2)
{
	const uint32_t tile_px = 1u << tile_log2;
	unsigned disable;
	for (disable = 0; tile_px > (16u * 16u) << (disable * 2); disable++)
		;
	return 0xfeu & ~((1u << disable) - 1u);
}

void kb_draw_tiler_ctx(uint32_t *d, unsigned fb_w, unsigned fb_h, uint32_t hmask, uint64_t heap_desc_va, uint64_t geom_buf)
{
	memset(d, 0, KB_TILER_CTX_SIZE);
	/* TODO(v13): hierarchy-mask/provoking-vertex bits below are v11 genxml; compare with Mesa v13.xml before trusting on arch 13
	 * (callers: warn via kb_arch_warn_v11_only(kb_props()->arch, ...)). */
	d[2] = (hmask & 0x1fff) | (1u << 18); /* first provoking vertex, as PanVK */                                   /* hierarchy mask; sample pattern 0, first provoking vertex 0 */
	d[3] = (fb_w - 1) | ((fb_h - 1) << 16);
	d[4] = 0;                                                /* layer count - 1 = 0, layer offset 0 */
	w64(d, 6, heap_desc_va);
	w64(d, 8, geom_buf);
}

void kb_draw_spd(uint32_t *d, unsigned stage, uint64_t binary_va, unsigned work_regs, uint64_t preload)
{
	memset(d, 0, KB_SPD_SIZE);
	d[0] = 8u | (stage << 4);
	d[0] |= 1u << 8;   /* fragment: coverage bitmask type GL (1); vertex: warp limit Half (1 in bits 9:8), as panvk does */
	d[0] |= (work_regs <= 32 ? 2u : 0u) << 30;               /* register allocation: 32 per thread / 64 per thread */
	d[1] = (uint32_t)(preload >> 48) & 0xffff;
	w64(d, 2, binary_va);
}

void kb_draw_spd_vs_varying(uint32_t *d, uint64_t binary_va, unsigned work_regs, uint64_t preload)
{
	kb_draw_spd(d, 3, binary_va, work_regs, preload);
	d[0] &= ~(3u << 8);                                      /* warp limit Full: panvk word 0 = 0x80000038 (position SPD = 0x80000138) */
}

void kb_draw_local_storage_none(uint32_t *d)
{
	memset(d, 0, KB_LS_SIZE);
	d[1] = 31;                                               /* WLS instances = log2(No Workgroup Mem 0x80000000); TLS size 0, no pointers */
}

void kb_draw_attr_1d(uint32_t *d, uint32_t pixfmt, unsigned table, unsigned buffer_index, uint32_t offset, uint32_t stride)
{
	memset(d, 0, KB_ATTR_SIZE);
	d[0] = 5u | (1u << 4) | (1u << 8) | (pixfmt << 10);       /* type Attribute, 1D, offset enable */
	d[1] = table;                                            /* frequency Vertex = 0 */
	d[2] = offset;
	d[3] = buffer_index;
	d[4] = stride;
}

void kb_draw_attr_instance(uint32_t *d, uint32_t divisor)
{
	/* instance frequency (word1[7:6] = 1), offset enable off; per panvk (csf/panvk_vX_cmd_draw.c) and genxml v10 "Attribute" */
	d[0] &= ~(1u << 8);
	d[1] |= 1u << 6;
	if (divisor <= 1)
		return;                                          /* type stays 1D: divisor 1 is the plain instance counter */
	unsigned r = 31 - __builtin_clz(divisor);                 /* floor(log2) */
	d[0] &= ~(0xfu << 4);
	if (!(divisor & (divisor - 1))) {
		d[0] |= 2u << 4;                                 /* 1D POT divisor: R = log2 */
		d[1] |= r << 24;
		return;
	}
	/* 1D NPOT divisor: D = round(2^(32+r) / divisor) - 2^31, E = the rounding remainder is above divisor/2 (pan_compute_npot_divisor) */
	uint64_t f0 = ((uint64_t)1 << (32 + r)) + divisor / 2;
	uint64_t fi = f0 / divisor, ff = f0 - fi * divisor;
	d[0] |= 3u << 4;
	d[1] |= (r << 24) | ((ff > divisor / 2 ? 1u : 0u) << 29);
	d[5] = (uint32_t)(fi - ((uint64_t)1 << 31));
}

void kb_draw_attr_unused(uint32_t *d)
{
	memset(d, 0, KB_ATTR_SIZE);
	d[0] = 5u | (KB_PIXFMT_R16F_RGBA << 10);
	d[1] = 17;
}

void kb_draw_attr_varying(uint32_t *d, uint32_t pixfmt, uint32_t varying_offset, uint32_t attr_stride)
{
	memset(d, 0, KB_ATTR_SIZE);
	d[0] = 5u | (6u << 4) | (pixfmt << 10);                   /* vertex packet, offset enable 0 */
	d[1] = 61;                                               /* hardware varying buffer table */
	d[2] = 1024 + varying_offset;
	d[3] = 0;
	d[4] = attr_stride | ((attr_stride + 16) << 16);          /* attribute stride | packet stride */
}

void kb_draw_buffer(uint32_t *d, uint64_t va, uint32_t size)
{
	memset(d, 0, KB_BUF_SIZE);
	d[0] = 9u | (1u << 4);
	d[1] = size;
	w64(d, 2, va);
}

void kb_draw_resource(uint32_t *d, uint64_t va, uint64_t size, int contains)
{
	memset(d, 0, KB_RES_SIZE);
	d[0] = (uint32_t)va;
	d[1] = (uint32_t)(va >> 32) & 0x00ffffffu;
	if (contains) d[1] |= 1u << 24;
	w64(d, 2, size);
}

void kb_draw_blend_opaque_rgba8(uint32_t *d)
{
	memset(d, 0, KB_BLEND_SIZE);
	d[0] = (1u << 9) | (1u << 11);                           /* enable | round to FB precision (panvk sets it unconditionally) */
	/* replace equation: A=Src(2) B=Src(2<<4) C=Zero(1<<8) for RGB and alpha, colour mask 0xf */
	d[1] = 0x122u | (0x122u << 12) | (0xfu << 28);
	d[2] = 1u | (3u << 3);                                   /* mode Opaque, 4 components, RT 0 */
	d[3] = KB_PIXFMT_RGBA8_TB;                               /* conversion memory format: the dithered/blendable format, not the pipe format */
}

void kb_draw_blend_opaque_fmt(uint32_t *d, uint32_t pixfmt, unsigned ncomp)
{
	kb_draw_blend_opaque_rgba8(d);
	d[2] = 1u | ((ncomp - 1u) << 3);
	d[3] = pixfmt;
}

void kb_draw_zsd_off(uint32_t *d)
{
	memset(d, 0, KB_ZSD_SIZE);
	d[0] = 7u;
	d[4] = (1u << 21) | (1u << 22) | (1u << 23) | (2u << 25) | (7u << 29);  /* sep. dependency, depth cull, clamp [0,1], fixed-function depth, Always */
	if (kb_v10) d[4] &= ~(1u << 21);                         /* v10.xml: bit 21 is "Shader read only Z/S", not separated dependency tracking */
}

void kb_draw_zsd_depth(uint32_t *d, enum kb_func func, int write)
{
	kb_draw_zsd_off(d);
	d[4] &= ~(7u << 29);
	d[4] |= ((uint32_t)func & 7u) << 29;                     /* Depth function (v11.xml Depth/stencil 4:29) */
	if (write) d[4] |= 1u << 27;                             /* Depth write enable (4:27) */
}

void kb_draw_fau_imm(uint32_t *fau, const uint32_t (*imm)[2], unsigned nimm)
{
	for (unsigned i = 0; i < nimm; i++) fau[imm[i][0]] = imm[i][1];
}
void kb_draw_fau_viewport(uint32_t *fau, float w, float h, float zs, float zo)
{
	const float v[6] = { w * 0.5f, h * 0.5f, zs, w * 0.5f, h * 0.5f, zo };
	memcpy(fau, v, sizeof v);
}
void kb_draw_fau_push(uint32_t *fau, unsigned byte_off, const float *v, unsigned n)
{
	memcpy(fau + byte_off / 4, v, n * 4);
}

uint32_t kb_draw_dcd0(const struct kb_dcd_opts *o)
{
	return (o->allow_fpk ? 1u : 0u) | (o->allow_fpk_killed ? 2u : 0u) | ((o->pixel_kill & 3u) << 2) | ((o->zs_update & 3u) << 4) |
	       (o->front_ccw ? 1u << 16 : 0u) | (1u << 23) /* scissor to bounding box */ | (1u << 26) /* cull zero area */;
}
uint32_t kb_draw_dcd1(uint16_t sample_mask, unsigned rt_mask) { return sample_mask | ((rt_mask & 0x7ffu) << 16); }
uint32_t kb_draw_dcd2(unsigned rd, unsigned wr, int nss, int nsd)
{
	return (rd & 0xfff) | ((wr & 0xfffu) << 12) | (nss ? 1u << 24 : 0) | (nsd ? 1u << 25 : 0);
}
uint32_t kb_draw_prim_flags_triangles(int secondary)
{
	return 8u | (1u << 16) | (1u << 17) | (secondary ? 1u << 18 : 0) | (1u << 22);   /* Triangles, depth cull, FIFO basic */
}
uint64_t kb_draw_scissor(unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
	return (uint64_t)(x0 | (y0 << 16)) | ((uint64_t)(x1 | (y1 << 16)) << 32);
}
uint64_t kb_draw_srt_ptr(uint64_t va, unsigned n) { return va | ((n + KB_RES_TABLE_ALIGN - 1) & ~(KB_RES_TABLE_ALIGN - 1)); }
uint64_t kb_draw_fau_ptr(uint64_t va, unsigned words32) { return va | ((uint64_t)((words32 + 1) / 2) << 56); }

/* Mesa src/panfrost/genxml/v11.xml, emitted by lib/pan_texture.c (Generic Plane :426-459, Texture :1272-1339) */
uint32_t kb_tex_row_stride_rgba8(unsigned w)
{
	return (w * 4 + (KB_TEX_ROW_ALIGN - 1)) & ~(KB_TEX_ROW_ALIGN - 1);
}

void kb_draw_plane_linear(uint32_t *d, uint64_t va, unsigned w, unsigned h, uint32_t row_stride)
{
	uint32_t size = row_stride * h;
	memset(d, 0, KB_PLANE_SIZE);
	d[0] = 10u | (1u << 4) | (2u << 8) | (2u << 24);          /* type Plane, Generic, clump ordering Linear, clump format RAW32 */
	d[1] = size;
	d[2] = (uint32_t)va;
	d[3] = (uint32_t)(va >> 32) & 0xffffu;                    /* size hi (bits 31:16) = 0 */
	d[4] = row_stride;                                        /* slice stride hi (27+) = 0 */
	d[6] = 0;                                                 /* slice stride */
	d[7] = (w - 1) | ((h - 1) << 16);
}

void kb_draw_texture_2d_rgba8(uint32_t *d, unsigned w, unsigned h, uint64_t plane_va)
{
	memset(d, 0, KB_TEX_SIZE);
	d[0] = 2u | (2u << 4) | (KB_PIXFMT_RGBA8 << 10);           /* type Texture, dimension 2D, format */
	d[1] = (w - 1) | ((h - 1) << 16);
	d[2] = 0x688u;                                            /* swizzle RGBA identity, texel interleave 0, 1 level */
	d[4] = (uint32_t)plane_va;
	d[5] = (uint32_t)(plane_va >> 32);
}

void kb_draw_sampler_nearest_clamp(uint32_t *d)
{
	memset(d, 0, KB_SAMPLER_SIZE);
	d[0] = 1u | (9u << 8) | (9u << 12) | (9u << 16) | (1u << 23) | (1u << 25);   /* wrap R/T/S clamp-to-edge, seamless cube, normalized coords; nearest filters = 0 */
	d[0] |= (1u << 27) | (1u << 28);                          /* minify / magnify nearest (genxml v11 sampler) */
}
