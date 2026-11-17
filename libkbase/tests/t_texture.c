// SPDX-License-Identifier: MIT
/*
 * t_texture: rung 3, a textured draw on Mali v11 (CSF). One quad (two triangles) covers the whole 256x256 target and samples an
 * 8x8-texel RGBA8 checkerboard (linear layout, nearest filter, clamp), so each texel is a 32x32 pixel cell.
 * One JSON object per line on stdout. Options: --dump --timeout MS --only 1 --hmask HEX --pk N --zs N --slot N --chunk BYTES
 * Layout, field sources and fallbacks: R3-NEXT.md. Descriptor sources: Mesa v11.xml + pan_texture.c.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kbase.h"
#include "kbase_draw.h"
#include "kbase_samples.h"
#include "kbase_fb.h"
#include "kbase_uapi.h"
#include <sys/ioctl.h>
#include "shaders_v11.h"
#include "loop.h"

typedef unsigned long long ull;

static void js(const char *s)
{
	putchar('"');
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
		else if (c < 0x20 || c == 0x7f) printf("\\u%04x", c);
		else putchar(c);
	}
	putchar('"');
}
static void jb(const char *t) { printf("{\"test\":"); js(t); }
static void jk(const char *k) { putchar(','); js(k); putchar(':'); }
static void jok(int ok) { jk("ok"); fputs(ok ? "true" : "false", stdout); }
static void jstr(const char *k, const char *v) { jk(k); js(v); }
static void ji(const char *k, long long v) { jk(k); printf("%lld", v); }
static void jx(const char *k, ull v) { jk(k); printf("\"0x%llx\"", v); }
static void jd(const char *k, double v) { jk(k); printf("%.3f", v); }
static void jbool(const char *k, bool v) { jk(k); fputs(v ? "true" : "false", stdout); }
static void jend(void) { fputs("}\n", stdout); fflush(stdout); }

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

struct opts {
	bool dump;
	unsigned hmask, pk, zs, slot, timeout_ms, chunk, only;
};
#define W 256u
#define H 256u
#define CLEAR_R 0x10
#define CLEAR_G 0x20
#define CLEAR_B 0x40
#define CLEAR_A 0x80

struct fx {
	struct kb_ctx *ctx;
	struct kb_arena ar;       /* descriptors + vertex data, uncached */
	struct kb_bo geo;         /* tiler heap desc @0, geometry buffer @4096 */
	struct kb_bo shd;         /* shader code */
	struct kb_bo sync;
	struct kb_bo hbuf;        /* tiler heap buffer descriptor */
	struct kb_tiler_heap heap;
	struct kb_queue q;
	uint64_t target, rt_va;
	struct opts o;
};

/* every descriptor we send, for --dump and failure reports */
struct dr { const char *tag; const void *p; uint64_t va; size_t n; };
static struct dr g_dr[64];
static int g_ndr;
static uint64_t g_cs[256];
static unsigned g_ncs;

static void *alloc(struct fx *f, const char *tag, size_t n, size_t al, uint64_t *va)
{
	void *p = kb_arena_alloc(&f->ar, n, al, va);
	if (p && g_ndr < 64) g_dr[g_ndr++] = (struct dr){ tag, p, *va, n };
	return p;
}

static void dump_all(const char *test)
{
	for (int i = 0; i < g_ndr; i++) {
		const uint32_t *d = g_dr[i].p;
		jb("descriptor_dump"); jstr("draw", test); jstr("tag", g_dr[i].tag); jx("va", g_dr[i].va);
		jk("words"); putchar('[');
		for (size_t k = 0; k < g_dr[i].n / 4; k++) printf("%s\"%08x\"", k ? "," : "", d[k]);
		putchar(']');
		jend();
	}
	jb("cs_dump"); jstr("draw", test); jk("ins"); putchar('[');
	for (unsigned i = 0; i < g_ncs; i++) printf("%s\"%016llx\"", i ? "," : "", (ull)g_cs[i]);
	putchar(']'); jend();
}

static void drain_events(struct fx *f)
{
	struct kb_event ev;
	int g = 0;
	while (g++ < 10 && kb_event_read(f->ctx, &ev, 300) > 0) {
		jb("notification"); jok(0); jstr("text", ev.text); ji("type", ev.type); ji("group", ev.group);
		ji("error_type", ev.error_type); jx("status", ev.status); jstr("exception", kb_exception_name(ev.status));
		jx("sideband", ev.sideband); jbool("fatal", ev.fatal); jend();
	}
}

static uint32_t ubits(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }

#define TW 8u
#define TH 8u
#define NV 6u
#define CELL (W / TW)

static void texel(unsigned tx, unsigned ty, uint8_t c[4])
{
	unsigned p = (tx + ty) & 1;
	c[0] = (uint8_t)((p ? 200 : 0) + ty * 8);
	c[1] = (uint8_t)((p ? 0 : 200) + ty * 8);
	c[2] = (uint8_t)(32 * tx);
	c[3] = 255;
}

/* full-target quad, NDC +-1; the VS viewport maps NDC to pixels (x+1)*128; UV = (ndc+1)/2 so u = px/256 */
static const float q_xy[NV][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };

struct dres {
	int rc; bool fence_ok; double us;
	long checked, bad, edge, centres_bad;
	int max_err;
	uint32_t seen[6]; unsigned nseen;
};

static int build_and_submit(struct fx *f, struct dres *r, int *fence_out, uint64_t *t0)
{
	kb_arena_begin(&f->ar);   /* rolling: never the previous frame's VAs (INTERMITTENT.md) */
	g_ndr = 0;
	uint64_t va_tctx, va_fbd, va_spd_p, va_spd_v, va_spd_f, va_texels, va_plane, va_uset;
	uint64_t va_vsset, va_vst, va_vsfau, va_fsset, va_fst, va_fsfau, va_vbp, va_vbc, va_bl, va_zsd, va_tsd;

	uint64_t va_smp; void *smp = alloc(f, "sample_locations", sizeof kb_sample_positions, 64, &va_smp);
	uint32_t *tctx = alloc(f, "tiler_ctx", KB_TILER_CTX_SIZE, 64, &va_tctx);
	uint8_t *fbd = alloc(f, "fbd_rtd", KB_FB_DESC_SIZE, 64, &va_fbd);
	uint32_t *spd_p = alloc(f, "spd_vs_pos", KB_SPD_SIZE, 32, &va_spd_p);
	uint32_t *spd_v = alloc(f, "spd_vs_varying", KB_SPD_SIZE, 32, &va_spd_v);
	uint32_t *spd_f = alloc(f, "spd_fs", KB_SPD_SIZE, 32, &va_spd_f);
	float *vbp = alloc(f, "vb_pos", NV * 16, 64, &va_vbp);
	float *vbc = alloc(f, "vb_uv", NV * 16, 64, &va_vbc);
	unsigned nb = 2, vs_entries = KB_VS_ATTRS + 1 + nb;
	uint8_t *vsset = alloc(f, "vs_driver_set", vs_entries * KB_DESC_SET_STRIDE, 64, &va_vsset);
	uint32_t *vst = alloc(f, "vs_res_table", 4 * KB_RES_SIZE, 64, &va_vst);
	uint32_t *vsfau = alloc(f, "vs_fau", 64, 64, &va_vsfau);
	uint8_t *fsset = alloc(f, "fs_driver_set", 2 * KB_DESC_SET_STRIDE, 64, &va_fsset);   /* [0] varying attr, [1] dummy sampler */
	uint32_t *fst = alloc(f, "fs_res_table", 4 * KB_RES_SIZE, 64, &va_fst);
	uint32_t *fsfau = alloc(f, "fs_fau", 64, 64, &va_fsfau);
	uint8_t *texels = alloc(f, "texels", (size_t)kb_tex_row_stride_rgba8(TW) * TH, KB_TEX_ROW_ALIGN, &va_texels);
	uint32_t *plane = alloc(f, "tex_plane", KB_PLANE_SIZE, 32, &va_plane);
	uint32_t *uset = alloc(f, "fs_user_set0", 2 * KB_DESC_SET_STRIDE, 64, &va_uset);   /* [0] texture, [1] sampler */
	uint32_t *bl = alloc(f, "blend", KB_BLEND_SIZE, 16, &va_bl);
	uint32_t *zsd = alloc(f, "depth_stencil", KB_ZSD_SIZE, 32, &va_zsd);
	uint32_t *tsd = alloc(f, "local_storage", KB_LS_SIZE, 64, &va_tsd);
	if (!tsd) return -ENOMEM;
	kb_draw_local_storage_none(tsd);
	const uint32_t fmt_rgba32f = KB_PIXFMT_RGBA32F;
	if (!smp || !tctx || !fbd || !spd_p || !spd_v || !spd_f || !vbp || !vbc || !vsset || !vst || !vsfau || !fsset || !fst || !fsfau ||
	    !texels || !plane || !uset || !bl || !zsd) return -ENOMEM;

	for (unsigned v = 0; v < NV; v++) {
		vbp[v * 4 + 0] = q_xy[v][0]; vbp[v * 4 + 1] = q_xy[v][1]; vbp[v * 4 + 2] = 0.0f; vbp[v * 4 + 3] = 1.0f;
		vbc[v * 4 + 0] = (q_xy[v][0] + 1.0f) * 0.5f; vbc[v * 4 + 1] = (q_xy[v][1] + 1.0f) * 0.5f; vbc[v * 4 + 2] = 0.0f; vbc[v * 4 + 3] = 1.0f;
	}

	/* texture: CPU-written linear RGBA8, row stride 64-aligned */
	uint32_t rs = kb_tex_row_stride_rgba8(TW);
	memset(texels, 0, (size_t)rs * TH);
	for (unsigned ty = 0; ty < TH; ty++)
		for (unsigned tx = 0; tx < TW; tx++) texel(tx, ty, texels + (size_t)ty * rs + tx * 4);
	kb_draw_plane_linear(plane, va_texels, TW, TH, rs);
	kb_draw_texture_2d_rgba8(uset, TW, TH, va_plane);
	kb_draw_sampler_nearest_clamp(uset + KB_DESC_SET_STRIDE / 4);

	/* tiler */
	kb_draw_tiler_heap_desc(f->geo.cpu, f->heap.first_chunk_va, f->heap.chunk_size);
	kb_draw_tiler_ctx(tctx, W, H, f->o.hmask, f->geo.gpu_va, (f->geo.gpu_va + 4096) | (65536u >> 12));

	/* shaders */
	uint64_t vs_va = f->shd.gpu_va, fs_va = f->shd.gpu_va + 512;
	kb_draw_spd(spd_p, 3, vs_va, VS_WORK_REG_COUNT, VS_PRELOAD);
	kb_draw_spd_vs_varying(spd_v, vs_va + VS_SECONDARY_OFFSET, VS_SECONDARY_WORK_REG_COUNT, VS_SECONDARY_PRELOAD);
	kb_draw_spd(spd_f, 2, fs_va, FS_TEX_WORK_REG_COUNT, FS_TEX_PRELOAD);

	/* VS driver set: attr 0 = position, attr 1 = UV (named "col" in pos.vert) */
	kb_draw_attr_1d((uint32_t *)(vsset + 0 * 32), fmt_rgba32f, 0, KB_VS_ATTRS + 1 + 0, 0, 16);
	kb_draw_attr_1d((uint32_t *)(vsset + 1 * 32), fmt_rgba32f, 0, KB_VS_ATTRS + 1 + 1, 0, 16);
	for (unsigned i = 2; i < KB_VS_ATTRS; i++) kb_draw_attr_unused((uint32_t *)(vsset + i * 32));
	kb_draw_buffer((uint32_t *)(vsset + (KB_VS_ATTRS + 1) * 32), va_vbp, NV * 16);
	kb_draw_buffer((uint32_t *)(vsset + (KB_VS_ATTRS + 2) * 32), va_vbc, NV * 16);
	kb_draw_resource(vst, va_vsset, vs_entries * KB_DESC_SET_STRIDE, 1);

	vsfau[0] = ubits(128.0f); vsfau[1] = ubits(128.0f); vsfau[2] = ubits(0.5f);
	vsfau[3] = ubits(128.0f); vsfau[4] = ubits(128.0f); vsfau[5] = ubits(0.5f);
	for (unsigned i = 0; i < VS_NIMM; i++) vsfau[vs_imm[i][0]] = vs_imm[i][1];

	/* FS: table 0 = driver set (varying attr, dummy zero sampler), table 1 = user set 0; shader handle (table 1, tex 0, sampler 1) is an immediate */
	kb_draw_attr_varying((uint32_t *)fsset, fmt_rgba32f, 16, 32);
	memset(fsset + KB_DESC_SET_STRIDE, 0, KB_DESC_SET_STRIDE);
	for (unsigned i = 0; i < FS_TEX_NIMM; i++) fsfau[fs_tex_imm[i][0]] = fs_tex_imm[i][1];
	kb_draw_resource(fst, va_fsset, 2 * KB_DESC_SET_STRIDE, 1);
	kb_draw_resource(fst + KB_RES_SIZE / 4 * KB_TEX_USER_TABLE, va_uset, 2 * KB_DESC_SET_STRIDE, 1);

	kb_draw_blend_opaque_rgba8(bl);
	kb_draw_zsd_off(zsd);

	struct kb_fb_params p;
	memset(&p, 0, sizeof p);
	p.width = W; p.height = H; p.rt_gpu_va = f->rt_va; p.row_stride = W * 4;
	p.clear_r = CLEAR_R; p.clear_g = CLEAR_G; p.clear_b = CLEAR_B; p.clear_a = CLEAR_A;
	p.tiler_va = va_tctx; p.sample_locations_va = va_smp;
	memcpy(smp, kb_sample_positions, sizeof kb_sample_positions);
	int rc = kb_fb_pack(fbd, &p);
	if (rc) return rc;
	kb_wmb();
	uint64_t target = f->target + 1;
	int fence = -1;
	if ((rc = kb_kcpu_cqs_wait_gt64(f->ctx, f->sync.gpu_va, target - 1)) || (rc = kb_kcpu_fence_signal(f->ctx, &fence))) return rc;

	struct kb_dcd_opts dop = { f->o.pk, f->o.zs, 1, 1, 1 };
	struct kb_cs_buf b;
	b.n = 0;
#define E(x) kb_cs_emit(&b, (x))
#define M48(r, v) E(kb_cs_move48((r), (v)))
#define M32(r, v) E(kb_cs_move32((r), (v)))
#define M64(r, v) do { uint64_t m64_ = (v); M32((r), (uint32_t)m64_); M32((r) + 1, (uint32_t)(m64_ >> 32)); } while (0)  /* M48 drops bits 63:48 (FAU count) */
	E(kb_cs_nop());
	M32(72, 0);
	E(kb_cs_flush_cache2(3, 3, 2, 72));
	E(kb_cs_wait(1u << KB_CS_SLOT_LS));   /* the flush is asynchronous: complete it before RUN_IDVS */
	M48(KB_IDVS_VERTEX_SRT, kb_draw_srt_ptr(va_vst, 1));
	M48(KB_IDVS_FRAGMENT_SRT, kb_draw_srt_ptr(va_fst, 2));
	M64(KB_IDVS_VERTEX_FAU, kb_draw_fau_ptr(va_vsfau, VS_FAU_COUNT));
	M64(KB_IDVS_FRAGMENT_FAU, kb_draw_fau_ptr(va_fsfau, FS_TEX_FAU_COUNT));
	M48(KB_IDVS_VERTEX_POS_SPD, va_spd_p);
	M48(KB_IDVS_VERTEX_VARY_SPD, va_spd_v);
	M48(KB_IDVS_FRAGMENT_SPD, va_spd_f);
	M48(KB_IDVS_TSD, va_tsd);   /* PanVK: MOVE48 r24 = Local Storage descriptor (word 1 = 0x1f) */
	M32(KB_IDVS_GLOBAL_ATTR_OFFSET, 0);
	M32(KB_IDVS_INDEX_COUNT, NV);
	M32(KB_IDVS_INSTANCE_COUNT, 1);
	M32(KB_IDVS_INDEX_OFFSET, 0);
	M32(KB_IDVS_VERTEX_OFFSET, 0);
	M32(KB_IDVS_INSTANCE_OFFSET, 0);
	M32(KB_IDVS_DCD2, kb_draw_dcd2(0, 1, 1, 1));
	M48(KB_IDVS_TILER_CTX, va_tctx);
	{ uint64_t sc = kb_draw_scissor(0, 0, W - 1, H - 1);   /* two 32-bit words: M48 would drop the max-Y half (PanVK r43 = 0x00ff00ff) */
	  M32(KB_IDVS_SCISSOR, (uint32_t)sc); M32(KB_IDVS_SCISSOR + 1, (uint32_t)(sc >> 32)); }
	M32(KB_IDVS_LOW_DEPTH_CLAMP, 0);
	M32(KB_IDVS_HIGH_DEPTH_CLAMP, 0x3f800000);
	M48(KB_IDVS_OQ, 0);
	M32(KB_IDVS_VARY_SIZE, VS_GENERIC_SIZE_B);
	M48(KB_IDVS_BLEND_DESC, va_bl | 1);
	M48(KB_IDVS_ZSD, va_zsd);
	M32(KB_IDVS_TILER_FLAGS, kb_draw_prim_flags_triangles(1));
	M32(KB_IDVS_DCD0, kb_draw_dcd0(&dop));
	M32(KB_IDVS_DCD1, kb_draw_dcd1(0xffff, 1));
	M32(KB_IDVS_PRIMITIVE_SIZE, 0);
	M32(KB_IDVS_TILER_FLAGS2, 0);
	M48(64, f->heap.ctx_va);
	E(kb_cs_heap_set(64));
	E(kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, f->o.slot));
	E(kb_cs_set_state_imm32(KB_CS_SB_SEL_OTHER, KB_CS_SLOT_LS));
	E(kb_cs_req_res(KB_CS_RES_IDVS | KB_CS_RES_TILER));
	E(kb_cs_heap_operation(KB_CS_HEAP_VT_STARTED));
	E(kb_cs_run_idvs());
	E(kb_cs_finish_tiling());
	E(kb_cs_wait(1u << f->o.slot));
	E(kb_cs_heap_operation(KB_CS_HEAP_VT_COMPLETED));
		E(kb_cs_req_res(KB_CS_RES_FRAGMENT));
	E(kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, f->o.slot));   /* t_clear re-selects the endpoint SB right before RUN_FRAGMENT */
	M48(40, kb_fb_pointer(va_fbd, 1));
	M32(42, kb_fb_bbox_min(0, 0));
	M32(43, kb_fb_bbox_max(W - 1, H - 1));
	M48(74, va_tctx);
	M48(44, 0); M32(46, 0);   /* FRAGMENT TEM_POINTER / ROW_STRIDE alias the IDVS depth clamps (r44/r45): zero them like panfrost */
	E(kb_cs_run_fragment(0));
	E(kb_cs_wait(1u << f->o.slot));
	E(kb_cs_load_multiple(10, 74, 0xf, 40));
	E(kb_cs_wait(1u << KB_CS_SLOT_LS));
	E(kb_cs_finish_fragment_heap(true, 10, 12));
	E(kb_cs_req_res(0));
	M32(72, 0);
	E(kb_cs_flush_cache2(1, 1, 0, 72));   /* clean L2 + LSC like PanVK's chunk tail so the CPU sees the tile writeback */
	E(kb_cs_wait(1u << KB_CS_SLOT_LS));
	M48(68, f->sync.gpu_va);
	M48(70, 1);
	E(kb_cs_sync_add64(68, 70, KB_CS_SCOPE_SYSTEM));
	memcpy(g_cs, b.ins, b.n * 8);
	g_ncs = b.n;

	*t0 = now_ns();
	rc = kb_queue_submit(&f->q, b.ins, b.n, KB_KICK_IOCTL);
	if (rc) { close(fence); return rc; }
	*fence_out = fence;
	(void)r;
	return 0;
}

static uint32_t px(const uint32_t *img, unsigned x, unsigned y) { return img[(size_t)y * W + x]; }

static int chan_err(uint32_t got, const uint8_t e[4])
{
	int m = 0;
	for (int c = 0; c < 4; c++) {
		int d = (int)((got >> (8 * c)) & 0xff) - e[c];
		if (d < 0) d = -d;
		if (d > m) m = d;
	}
	return m;
}

static void note(struct dres *r, uint32_t v)
{
	for (unsigned i = 0; i < r->nseen; i++) if (r->seen[i] == v) return;
	if (r->nseen < 6) r->seen[r->nseen++] = v;
}

/* every pixel: exact cell colour (+-1/channel) when >1.5px from a cell edge, either neighbour's colour within it (sample point / rounding tolerance) */
static void verify(const uint32_t *img, struct dres *r)
{
	for (unsigned y = 0; y < H; y++)
		for (unsigned x = 0; x < W; x++) {
			unsigned tx = x / CELL, ty = y / CELL;
			double fx = (x % CELL) + 0.5, fy = (y % CELL) + 0.5;
			bool ex = fx < 1.5 || fx > CELL - 1.5, ey = fy < 1.5 || fy > CELL - 1.5;
			int best = 255;
			unsigned x0 = tx, x1 = tx, y0 = ty, y1 = ty;
			if (ex) { if (fx < CELL / 2.0 && tx > 0) x0 = tx - 1; else if (fx >= CELL / 2.0 && tx + 1 < TW) x1 = tx + 1; }
			if (ey) { if (fy < CELL / 2.0 && ty > 0) y0 = ty - 1; else if (fy >= CELL / 2.0 && ty + 1 < TH) y1 = ty + 1; }
			for (unsigned cy = y0; cy <= y1; cy++)
				for (unsigned cx = x0; cx <= x1; cx++) {
					uint8_t e[4]; texel(cx, cy, e);
					int d = chan_err(px(img, x, y), e);
					if (d < best) best = d;
				}
			r->checked++;
			if (ex || ey) r->edge++;
			if (best > r->max_err) r->max_err = best;
			if (best > 1) { r->bad++; note(r, px(img, x, y)); }
		}
	for (unsigned ty = 0; ty < TH; ty++)
		for (unsigned tx = 0; tx < TW; tx++) {
			uint8_t e[4]; texel(tx, ty, e);
			if (chan_err(px(img, tx * CELL + CELL / 2, ty * CELL + CELL / 2), e) > 1) r->centres_bad++;
		}
}

static bool draw_test(struct fx *f, const char *name)
{
	struct dres r;
	memset(&r, 0, sizeof r);
	size_t sz = (size_t)W * H * 4;
	int fd = kb_dmaheap_alloc(KB_HEAP_SYSTEM, sz);
	struct kb_bo rt;
	memset(&rt, 0, sizeof rt);
	if (fd < 0 || kb_bo_import_dmabuf(f->ctx, fd, sz, KB_BO_GPU_RD | KB_BO_GPU_WR, &rt)) {
		jb(name); jok(0); jstr("error", fd < 0 ? "dma-heap alloc" : kb_last_error()); jend();
		if (fd >= 0) close(fd);
		return false;
	}
	f->rt_va = rt.gpu_va;
	kb_bo_dmabuf_sync(&rt, false, true);
	for (size_t i = 0; i < sz / 4; i++) ((volatile uint32_t *)rt.cpu)[i] = 0x5a3c1e87u;
	kb_bo_dmabuf_sync(&rt, true, true);

	int fence = -1;
	uint64_t t0 = 0;
	r.rc = build_and_submit(f, &r, &fence, &t0);
	if (!r.rc) {
		r.rc = kb_fence_wait(f->ctx, fence, (int)f->o.timeout_ms);
		r.us = (double)(now_ns() - t0) / 1000.0;
		loop_add_us(r.us);
		close(fence);
		r.fence_ok = r.rc == 0;
		if (r.fence_ok) f->target++;
	}
	uint32_t *img = NULL;
	if (r.fence_ok) {
		img = malloc(sz);
		kb_bo_dmabuf_sync(&rt, false, false);
		for (size_t i = 0; i < sz / 4; i++) img[i] = ((volatile uint32_t *)rt.cpu)[i];
		kb_bo_dmabuf_sync(&rt, true, false);
		verify(img, &r);
	}
	bool ok = r.fence_ok && r.bad == 0 && r.centres_bad == 0;
	jb(name); jok(ok); ji("fence_rc", r.rc); jbool("fence_signalled", r.fence_ok); jd("submit_to_signal_us", r.us);
	jx("hmask", f->o.hmask);
	if (r.fence_ok) {
		ji("pixels_checked", r.checked); ji("edge_tolerant_pixels", r.edge); ji("pixels_bad", r.bad);
		ji("cell_centres_checked", TW * TH); ji("cell_centres_bad", r.centres_bad); ji("max_channel_err", r.max_err);
		jk("sample"); putchar('[');
		static const unsigned sp[][2] = { { 16, 16 }, { 48, 16 }, { 16, 48 }, { 240, 240 }, { 0, 0 } };
		for (unsigned i = 0; i < 5; i++) printf("%s\"0x%08x\"", i ? "," : "", px(img, sp[i][0], sp[i][1]));
		putchar(']');
		if (r.nseen) {
			jk("unexpected_values"); putchar('[');
			for (unsigned i = 0; i < r.nseen; i++) printf("%s\"0x%08x\"", i ? "," : "", r.seen[i]);
			putchar(']');
		}
	} else jstr("error", kb_last_error());
	jend();
	if (f->o.dump || !ok) dump_all(name);
	if (!ok) {
		drain_events(f);
		jb("fault_state"); jok(0); jbool("queue_faulted", kb_queue_faulted(&f->q));
		jx("cs_extract", kb_queue_extract(&f->q)); jx("cs_insert", f->q.insert); jend();
	}
	free(img);
	kb_bo_free(&rt);
	close(fd);
	return ok;
}

static int run(int argc, char **argv, int inner)
{
	struct fx f;
	memset(&f, 0, sizeof f);
	f.o.hmask = 0xfe; f.o.pk = 2; f.o.zs = 0; f.o.slot = 3; f.o.timeout_ms = 3000; f.o.chunk = 2u << 20;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dump")) f.o.dump = true;
		else if (!strcmp(argv[i], "--hmask") && i + 1 < argc) f.o.hmask = (unsigned)strtoul(argv[++i], NULL, 16);
		else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) f.o.timeout_ms = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--pk") && i + 1 < argc) f.o.pk = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--zs") && i + 1 < argc) f.o.zs = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--slot") && i + 1 < argc) f.o.slot = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--only") && i + 1 < argc) f.o.only = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--chunk") && i + 1 < argc) f.o.chunk = (unsigned)strtoul(argv[++i], NULL, 0);
	}
	int failures = 0;
	int r = kb_open(&f.ctx, NULL);
	jb("open"); jok(r == 0); ji("rc", r); jx("opt_hmask", f.o.hmask); ji("opt_pk", f.o.pk); ji("opt_zs", f.o.zs); jend();
	if (r) { jb("open_error"); jstr("error", kb_last_error()); jend(); return 1; }
	int e[5];
	e[0] = kb_bo_alloc(f.ctx, 64 * 1024, KB_BO_GPU_RD | KB_BO_GPU_WR, &f.ar.bo);
	e[1] = kb_bo_alloc(f.ctx, 4096 + 65536, KB_BO_GPU_RD | KB_BO_GPU_WR, &f.geo);
	e[2] = kb_bo_alloc(f.ctx, 4096, KB_BO_GPU_RD | KB_BO_GPU_EX, &f.shd);
	e[3] = kb_bo_alloc(f.ctx, 4096, KB_BO_CSF_EVENT | KB_BO_GPU_RD | KB_BO_GPU_WR, &f.sync);
	e[4] = kb_bo_alloc(f.ctx, 4096, KB_BO_GPU_RD | KB_BO_GPU_WR, &f.hbuf);
	if (e[0] || e[1] || e[2] || e[3] || e[4]) { jb("setup"); jok(0); jstr("error", kb_last_error()); jend(); failures++; goto teardown; }
	memset(f.sync.cpu, 0, 4096);
	memset(f.geo.cpu, 0, 4096 + 65536);
	memset(f.shd.cpu, 0, 4096);
	memset(f.hbuf.cpu, 0, 4096);
	memcpy((char *)f.shd.cpu, vs_bin, VS_SIZE);
	memcpy((char *)f.shd.cpu + 512, fs_tex_bin, FS_TEX_SIZE);
	r = kb_queue_create(f.ctx, NULL, &f.q);
	jb("queue_create"); jok(r == 0); ji("rc", r); jend();
	if (r) { failures++; goto teardown; }

	r = kb_tiler_heap_create(f.ctx, f.o.chunk, 1, 4, &f.heap);
	jb("tiler_heap"); jok(r == 0); ji("rc", r); jx("heap_ctx_va", f.heap.ctx_va); jx("first_chunk_va", f.heap.first_chunk_va);
	ji("chunk_size", f.heap.chunk_size); if (r) jstr("error", kb_last_error()); jend();
	if (r) { failures++; goto teardown; }

	for (int it = 0; it < inner; it++) {
		loop_iter_begin();
		bool ok = true;
		if ((f.o.only == 0 || f.o.only == 1) && !draw_test(&f, "texture_checker")) ok = false;
		loop_iter_end(ok);
		if (!ok) { failures++; if (inner == 1 || loop_stop()) break; }
	}
teardown:
	if (f.q.ctx) kb_queue_destroy(&f.q);
	kb_tiler_heap_destroy(&f.heap);
	if (f.ar.bo.ctx) kb_bo_free(&f.ar.bo);
	if (f.geo.ctx) kb_bo_free(&f.geo);
	if (f.shd.ctx) kb_bo_free(&f.shd);
	if (f.sync.ctx) kb_bo_free(&f.sync);
	if (f.hbuf.ctx) kb_bo_free(&f.hbuf);
	{
		struct kb_leaks lk;
		kb_leaks(f.ctx, &lk);
		jb("teardown"); jok(lk.bos == 0 && lk.queues == 0); ji("leaked_bos", lk.bos); ji("leaked_queues", lk.queues); jend();
		kb_close(f.ctx);
	}
	jb("summary"); jok(failures == 0); ji("failures", failures); jend();
	return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
	return loop_main("t_texture", argc, argv, run);
}
