/* SPDX-License-Identifier: MIT */
/*
 * rung2_v13.h: v13 (Mali-G925, RUN_IDVS2) port of rung2.h for t_uniform_v13.c / t_depth_v13.c (header-only, static). Same device setup, JSON output and CS sequence as
 * t_triangle.c, generalised to N draws per frame (one IDVS per draw, one FINISH_TILING + RUN_FRAGMENT at the end), a per-draw
 * FAU table (viewport sysvals + push constants), an optional depth attachment, and an analytic whole-image check.
 * Shaders: tests/shaders_v13.h vs_uni / fs_uni (uni.vert/uni.frag). VS push block = FAU byte 32.. (scale.xy offset.zw z), FS = byte 0.. (colour).
 * Options: --dump --only N --timeout MS --hmask HEX --pk N --zs N --slot N --chunk BYTES --func N (depth compare override, experiments)
 *          --zsnostore (no depth write-back, no depth readback check)
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
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "shaders_v13.h"
#include "kbase_draw_v13.h"
#include "kbase_cs_v13.h"
#include "loop.h"
extern uint64_t kb_bo_extra_flags;   /* src/bo.c: v13 default 0x800 (COHERENT_LOCAL), set in r2_run1 */

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
static void jd(const char *k, double v) { jk(k); printf("%.4f", v); }
static void jbool(const char *k, bool v) { jk(k); fputs(v ? "true" : "false", stdout); }
static void jend(void) { fputs("}\n", stdout); fflush(stdout); }

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

#define W 256u
#define H 256u
#define CLEAR_R 0x10
#define CLEAR_G 0x20
#define CLEAR_B 0x40
#define CLEAR_A 0x80
#define MAXD 4

struct draw { float so[4]; float z; float color[4]; };   /* NDC scale.xy, offset.xy; depth; RGBA */
struct scene {
	const char *name;
	unsigned nd; struct draw d[MAXD];
	int depth; enum kb_func func; enum kb_zs_format zf; float clear_z;   /* depth == 0: ZSD off, no depth buffer */
};

struct opts { bool dump, zsnostore; unsigned hmask, pk, zs, slot, timeout_ms, chunk, only, tile_log2; int func; };
static unsigned g_var;   /* --var MASK / --sweep bisect: 1 = CS_WAIT after every draw, 2 = pixel-kill 2 (weak early), 4 = depth func ALWAYS */
static unsigned g_tf_or = 0x40000;   /* tiler flags bit 18 (secondary_shader): needed for varying/multi-draw on the Poco, see V13-NEXT2.md */

struct fx {
	struct kb_ctx *ctx;
	struct kb_arena ar;
	struct kb_bo geo, shd, sync, hbuf;
	struct kb_tiler_heap heap;
	struct kb_queue q;
	uint64_t target;
	struct opts o;
};

/* FAU block size: the qword count given to the hardware (kb_draw_fau_ptr), rounded up to 64 B. The block must cover every word the shader can read. */
#define R2_FAU_BYTES(words32) (((((size_t)(words32) + 1) / 2) * 8 + 63) & ~(size_t)63)
struct dr { const char *tag; const void *p; uint64_t va; size_t n; };
static struct dr g_dr[64];
static int g_ndr;
static uint64_t g_cs[512];
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
		jb("notification"); jok(!ev.fatal && !ev.status); jstr("text", ev.text); ji("type", ev.type); ji("group", ev.group);
		ji("error_type", ev.error_type); jx("status", ev.status); jstr("exception", kb_exception_name(ev.status));
		jx("sideband", ev.sideband); jbool("fatal", ev.fatal); jend();
	}
}

/* base triangle, NDC; every draw transforms it by its own scale/offset (VS push constants) */
static const float k_pos[3][4] = { { -0.5f, -0.5f, 0.0f, 1.0f }, { 0.5f, -0.5f, 0.0f, 1.0f }, { 0.0f, 0.5f, 0.0f, 1.0f } };

/* framebuffer-pixel triangle of a draw: viewport scale/offset 128 (W/2, H/2), as kb_draw_fau_viewport */
static void tri_px(const struct draw *d, double v[3][2])
{
	for (int i = 0; i < 3; i++) {
		v[i][0] = 128.0 + 128.0 * (double)(k_pos[i][0] * d->so[0] + d->so[2]);
		v[i][1] = 128.0 + 128.0 * (double)(k_pos[i][1] * d->so[1] + d->so[3]);
	}
}

/* signed distance in pixels of point p inside the triangle: > 0 inside (all edges), min over edges */
static double tri_dist(double v[3][2], double px, double py)
{
	double area = (v[1][0] - v[0][0]) * (v[2][1] - v[0][1]) - (v[1][1] - v[0][1]) * (v[2][0] - v[0][0]);
	double s = area < 0 ? -1.0 : 1.0, m = 1e30;
	for (int i = 0; i < 3; i++) {
		const double *a = v[i], *b = v[(i + 1) % 3];
		double ex = b[0] - a[0], ey = b[1] - a[1];
		double dd = s * (ex * (py - a[1]) - ey * (px - a[0])) / sqrt(ex * ex + ey * ey);
		if (dd < m) m = dd;
	}
	return m;
}

static bool cmp_pass(enum kb_func fn, double nw, double old)
{
	switch (fn) {
	case KB_FUNC_NEVER: return false;
	case KB_FUNC_LESS: return nw < old;
	case KB_FUNC_EQUAL: return nw == old;
	case KB_FUNC_LEQUAL: return nw <= old;
	case KB_FUNC_GREATER: return nw > old;
	case KB_FUNC_NOT_EQUAL: return nw != old;
	case KB_FUNC_GEQUAL: return nw >= old;
	default: return true;
	}
}

static uint32_t col_word(const float c[4])
{
	uint32_t r = 0;
	for (int i = 0; i < 4; i++) r |= (uint32_t)lround(255.0 * (double)c[i]) << (8 * i);
	return r;
}

static int build_and_submit(struct fx *f, const struct scene *sc, uint64_t rt_va, uint64_t zs_va, int *fence_out, uint64_t *t0)
{
	kb_arena_begin(&f->ar);   /* rolling: never the previous frame's VAs (INTERMITTENT.md) */
	g_ndr = 0;
	uint64_t va_tctx, va_fbd, va_spd_p, va_spd_f, va_vsset, va_vst, va_fsset, va_fst, va_vbp, va_bl, va_zsd, va_tsd, va_smp;
	uint64_t va_vfau[MAXD], va_ffau[MAXD];
	void *smp = alloc(f, "sample_locations", sizeof kb_sample_positions, 64, &va_smp);
	uint32_t *tctx = alloc(f, "tiler_ctx", KB_TILER_CTX_SIZE, 64, &va_tctx);
	uint8_t *fbd = alloc(f, "fbd_zsext_rtd", KB_FB_DESC_SIZE_ZS, 64, &va_fbd);
	uint32_t *spd_p = alloc(f, "spd_vs_pos", KB_SPD_SIZE, 32, &va_spd_p);
		uint32_t *spd_f = alloc(f, "spd_fs", KB_SPD_SIZE, 32, &va_spd_f);
	float *vbp = alloc(f, "vb_pos", sizeof k_pos, 64, &va_vbp);
	unsigned vs_entries = KB_VS_ATTRS + 1 + 1;
	uint8_t *vsset = alloc(f, "vs_driver_set", vs_entries * KB_DESC_SET_STRIDE, 64, &va_vsset);
	uint32_t *vst = alloc(f, "vs_res_table", 4 * KB_RES_SIZE, 64, &va_vst);
	uint8_t *fsset = alloc(f, "fs_driver_set", KB_DESC_SET_STRIDE, 64, &va_fsset);
	uint32_t *fst = alloc(f, "fs_res_table", 4 * KB_RES_SIZE, 64, &va_fst);
	uint32_t *bl = alloc(f, "blend", KB_BLEND_SIZE, 16, &va_bl);
	uint32_t *zsd = alloc(f, "depth_stencil", KB_ZSD_SIZE, 32, &va_zsd);
	uint32_t *tsd = alloc(f, "local_storage", KB_LS_SIZE, 64, &va_tsd);
	uint32_t *vfau[MAXD], *ffau[MAXD];
	for (unsigned i = 0; i < sc->nd; i++) {
		vfau[i] = alloc(f, "vs_fau", R2_FAU_BYTES(VS_UNI_FAU_COUNT), 64, &va_vfau[i]);   /* 17 words = 9 qwords = 72 B: a 64 B block let the FS colour overwrite the VS immediate (word 16) */
		ffau[i] = alloc(f, "fs_fau", R2_FAU_BYTES(FS_UNI_FAU_COUNT), 64, &va_ffau[i]);
		if (!vfau[i] || !ffau[i]) return -ENOMEM;
	}
	if (!smp || !tctx || !fbd || !spd_p || !spd_f || !vbp || !vsset || !vst || !fsset || !fst || !bl || !zsd || !tsd) return -ENOMEM;
	kb_draw_local_storage_none(tsd);
	memcpy(vbp, k_pos, sizeof k_pos);

	kb_draw_tiler_heap_desc(f->geo.cpu, f->heap.first_chunk_va, f->heap.chunk_size);
	kb_v13_tiler_ctx(tctx, W, H, f->o.hmask, f->o.tile_log2, f->geo.gpu_va, (f->geo.gpu_va + 4096) | (65536u >> 12));

	uint64_t vs_va = f->shd.gpu_va, fs_va = f->shd.gpu_va + 512;
	kb_v13_spd(spd_p, 3, vs_va, VS_UNI_WORK_REG_COUNT, VS_UNI_PRELOAD, 0);
	kb_v13_spd(spd_f, 2, fs_va, FS_UNI_WORK_REG_COUNT, FS_UNI_PRELOAD, 0);

	kb_draw_attr_1d((uint32_t *)vsset, KB_PIXFMT_RGBA32F, 0, KB_VS_ATTRS + 1, 0, 16);
	for (unsigned i = 1; i < KB_VS_ATTRS; i++) kb_draw_attr_unused((uint32_t *)(vsset + i * 32));
	kb_draw_buffer((uint32_t *)(vsset + (KB_VS_ATTRS + 1) * 32), va_vbp, sizeof k_pos);
	kb_draw_resource(vst, va_vsset, vs_entries * KB_DESC_SET_STRIDE, 1);
	kb_draw_resource(fst, va_fsset, KB_DESC_SET_STRIDE, 1);
	kb_draw_blend_opaque_rgba8(bl);

	/* per-draw FAU: VS = viewport (Vulkan depth range: scale z 1, offset 0) at words 0..5, push {so, z} at byte 32; FS = push {colour} at byte 0 */
	for (unsigned i = 0; i < sc->nd; i++) {
		kb_draw_fau_imm(vfau[i], vs_uni_imm, VS_UNI_NIMM);
		kb_draw_fau_viewport(vfau[i], (float)W, (float)H, 1.0f, 0.0f);
		kb_draw_fau_push(vfau[i], 32, sc->d[i].so, 4);
		kb_draw_fau_push(vfau[i], 48, &sc->d[i].z, 1);
		kb_draw_fau_imm(ffau[i], fs_uni_imm, FS_UNI_NIMM);
		kb_draw_fau_push(ffau[i], 0, sc->d[i].color, 4);
		ffau[i][4] = bl[2]; ffau[i][5] = bl[3];   /* v13 FS FAU words 4,5 = RT conversion descriptor, read by BLEND */
	}
	enum kb_func fn = f->o.func >= 0 ? (enum kb_func)f->o.func : (g_var & 4) ? KB_FUNC_ALWAYS : sc->func;
	if (sc->depth) kb_v13_zsd_depth(zsd, fn, 1);
	else kb_v13_zsd_depth(zsd, KB_FUNC_ALWAYS, 0);

	struct kb_fb_params p;
	memset(&p, 0, sizeof p);
	p.width = W; p.height = H; p.rt_gpu_va = rt_va; p.row_stride = W * 4;
	p.clear_r = CLEAR_R; p.clear_g = CLEAR_G; p.clear_b = CLEAR_B; p.clear_a = CLEAR_A;
	p.tiler_va = va_tctx; p.sample_locations_va = va_smp;
	if (sc->depth && !(g_var & 128)) {   /* var 128: no ZS attachment (tile-buffer depth only) */
		p.zs_format = sc->zf; p.zs_gpu_va = zs_va; p.clear_depth = sc->clear_z; p.zs_no_store = f->o.zsnostore;
	}
	memcpy(smp, kb_sample_positions, sizeof kb_sample_positions);
	p.arch = 13;
	int rc = kb_fb_pack(fbd, &p);
	if (rc) return rc;
	{ uint32_t *w = (uint32_t *)fbd;   /* PanVK on v13: tile 2^tile_log2 px, colour alloc (4 B/px) in KiB (w11[12:9], [31:24]) */
	  unsigned al = ((1u << f->o.tile_log2) * 4u) >> 10; if (!al) al = 1;
	  w[11] = (w[11] & ~((0xfu << 9) | (0xffu << 24))) | ((f->o.tile_log2 & 0xfu) << 9) | (al << 24); }
	{   /* PanVK v13 FBD: HSR bits (w0 12..14), empty-tile/CRC read-write (w12 31:28); var 16 ORs w12 31:28 (faults the queue), 256 = PanVK HSR set (w0 12..14 + DCD2 26/27[/28 with depth]; renders the same, no fix); 128 = no ZS attachment */
	  uint32_t *w = (uint32_t *)fbd; if (g_var & 256) kb_v13_fbd_hsr(w, 1, 1, 1, 0); if (g_var & 16) w[12] |= 0xf0000000u; }
	kb_wmb();
	if (kb_bo_extra_flags) {   /* coherent-local BOs: CPU->device MEM_SYNC like PanVK */
		struct kb_bo *bs[3] = { &f->ar.bo, &f->geo, &f->hbuf };
		for (int i = 0; i < 3; i++) {
			struct { uint64_t handle, user_addr, size; uint8_t type, pad[7]; } ms = { bs[i]->gpu_va, (uint64_t)(uintptr_t)bs[i]->cpu, bs[i]->size, 1, { 0 } };
			ioctl(kb_fd(f->ctx), _IOW(KBASE_IOCTL_TYPE, 15, ms), &ms);
		}
	}

	uint64_t target = f->target + 1;
	int fence = -1;
	if ((rc = kb_kcpu_cqs_wait_gt64(f->ctx, f->sync.gpu_va, target - 1)) || (rc = kb_kcpu_fence_signal(f->ctx, &fence))) return rc;

	struct kb_dcd_opts dop = { (g_var & 2) ? 2 : f->o.pk, f->o.zs, 1, 1, 1 };
	struct kb_cs_buf b;
	b.n = 0;
#define E(x) kb_cs_emit(&b, (x))
#define M48(r, v) E(kb_cs_move48((r), (v)))
#define M32(r, v) E(kb_cs_move32((r), (v)))
#define M64(r, v) do { uint64_t m64_ = (v); M32((r), (uint32_t)m64_); M32((r) + 1, (uint32_t)(m64_ >> 32)); } while (0)
	E(kb_cs_nop());
	M32(80, 0);
	E(kb_cs_flush_cache2(3, 3, 2, 80));
	E(kb_cs_wait(1u << KB_CS_SLOT_LS));   /* the flush is asynchronous: complete it before RUN_IDVS2 */
	M48(KB13_IDVS_VERTEX_SRT, kb_draw_srt_ptr(va_vst, 1));
	M48(KB13_IDVS_FRAGMENT_SRT, kb_draw_srt_ptr(va_fst, 1));
	M48(KB13_IDVS_VERTEX_SPD, va_spd_p);
	M48(KB13_IDVS_FRAGMENT_SPD, va_spd_f);
	M48(KB13_IDVS_VERTEX_TSD, va_tsd);
	M48(KB13_IDVS_FRAGMENT_TSD, va_tsd);
	M32(KB13_IDVS_GLOBAL_ATTR_OFFSET, 0);
	M32(KB13_IDVS_INDEX_COUNT, 3);
	M32(KB13_IDVS_INSTANCE_COUNT, 1);
	M32(KB13_IDVS_INDEX_OFFSET, 0);
	M32(KB13_IDVS_VERTEX_OFFSET, 0);
	M32(KB13_IDVS_INSTANCE_OFFSET, 0);
	M48(KB13_IDVS_TILER_CTX, va_tctx);
	{ uint64_t sc2 = kb_draw_scissor(0, 0, W - 1, H - 1);
	  M32(KB13_IDVS_SCISSOR, (uint32_t)sc2); M32(KB13_IDVS_SCISSOR + 1, (uint32_t)(sc2 >> 32)); }
	{ uint64_t vh = kb_v13_viewport_hi(0, 0, W - 1, H - 1), vl = kb_v13_viewport_lo(0.0f, 1.0f);
	  M32(KB13_IDVS_VIEWPORT_HIGH, (uint32_t)vh); M32(KB13_IDVS_VIEWPORT_HIGH + 1, (uint32_t)(vh >> 32));
	  M32(KB13_IDVS_VIEWPORT_LOW, (uint32_t)vl); M32(KB13_IDVS_VIEWPORT_LOW + 1, (uint32_t)(vl >> 32)); }
	M64(KB13_IDVS_INDEX_BUFFER, 0);
	M32(KB13_IDVS_INDEX_BUFFER_SIZE, 0);
	M32(KB13_IDVS_VARY_SIZE, VS_UNI_GENERIC_SIZE_B);
	M48(KB13_IDVS_ZSD, va_zsd);
	M48(KB13_IDVS_BLEND_DESC, va_bl | 1);
	M32(KB13_IDVS_TILER_FLAGS, kb_v13_prim_flags_triangles() | g_tf_or);
	M32(KB13_IDVS_DCD0, kb_draw_dcd0(&dop));
	M32(KB13_IDVS_DCD1, kb_draw_dcd1(0xffff, 1));
	M32(KB13_IDVS_DCD2, kb_v13_dcd2(0, 1, 1, 1, !!(g_var & 256), !!(g_var & 256)) | ((sc->depth && (g_var & 256)) ? 1u << 28 : 0u));
	M32(KB13_IDVS_LINE_WIDTH, 0);
	M32(KB13_IDVS_TILER_FLAGS2, 0);
	M64(KB13_IDVS_OQ, 0);
	M48(88, f->heap.ctx_va);
	E(kb_cs_heap_set(88));
	E(kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, f->o.slot));
	E(kb_cs_set_state_imm32(KB_CS_SB_SEL_OTHER, KB_CS_SLOT_LS));
	E(kb_cs_req_res(KB_CS_RES_IDVS | KB_CS_RES_TILER));
	E(kb_cs_heap_operation(KB_CS_HEAP_VT_STARTED));
	for (unsigned i = 0; i < sc->nd; i++) {   /* FAU pointers are the only per-draw state; M64: M48 would drop the count in bits 63:56 */
		M64(KB13_IDVS_VERTEX_FAU, kb_draw_fau_ptr(va_vfau[i], VS_UNI_FAU_COUNT));
		M64(KB13_IDVS_FRAGMENT_FAU, kb_draw_fau_ptr(va_ffau[i], FS_UNI_FAU_COUNT));
		E(kb13_cs_run_idvs2(0, 0));
		if (g_var & 1) E(kb_cs_wait(1u << f->o.slot));
	}
	E(kb_cs_finish_tiling());
	E(kb_cs_wait(1u << f->o.slot));
	E(kb_cs_heap_operation(KB_CS_HEAP_VT_COMPLETED));
	E(kb_cs_req_res(KB_CS_RES_FRAGMENT));
	E(kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, f->o.slot));
	M48(40, kb_fb_pointer_ex(va_fbd, 1, sc->depth && !(g_var & 128)));
	M32(42, kb_fb_bbox_min(0, 0));
	M32(43, kb_fb_bbox_max(W - 1, H - 1));
	M48(82, va_tctx);
	M48(44, 0); M32(46, 0);   /* FRAGMENT TEM_POINTER / ROW_STRIDE alias the IDVS viewport regs: zero */
	E(kb_cs_run_fragment(0));
	E(kb_cs_wait(1u << f->o.slot));
	E(kb_cs_load_multiple(10, 82, 0xf, 40));
	E(kb_cs_wait(1u << KB_CS_SLOT_LS));
	E(kb_cs_finish_fragment_heap(true, 10, 12));
	E(kb_cs_req_res(0));
	M32(80, 0);
	E(kb_cs_flush_cache2(1, 1, 0, 80));
	E(kb_cs_wait(1u << KB_CS_SLOT_LS));
	M48(84, f->sync.gpu_va);
	M48(86, 1);
	E(kb_cs_sync_add64(84, 86, KB_CS_SCOPE_SYSTEM));
	memcpy(g_cs, b.ins, b.n * 8);
	g_ncs = b.n;

	*t0 = now_ns();
	rc = kb_queue_submit(&f->q, b.ins, b.n, KB_KICK_IOCTL);
	if (rc) { close(fence); return rc; }
	*fence_out = fence;
	return 0;
}

/* CPU-visible dma-heap target (as t_triangle's render target), pre-filled with `fill` */
static int alloc_target(struct fx *f, size_t sz, uint32_t fill, struct kb_bo *bo, int *fd)
{
	memset(bo, 0, sizeof *bo);
	*fd = kb_dmaheap_alloc(KB_HEAP_SYSTEM, sz);
	if (*fd < 0) return -1;
	if (kb_bo_import_dmabuf(f->ctx, *fd, sz, KB_BO_GPU_RD | KB_BO_GPU_WR, bo)) { close(*fd); *fd = -1; return -2; }
	kb_bo_dmabuf_sync(bo, false, true);
	for (size_t i = 0; i < sz / 4; i++) ((volatile uint32_t *)bo->cpu)[i] = fill;
	kb_bo_dmabuf_sync(bo, true, true);
	return 0;
}

static double decode_z(uint32_t v, enum kb_zs_format zf, int mode)
{
	if (zf == KB_ZS_D32F) { float x; memcpy(&x, &v, 4); return (double)x; }
	if (zf == KB_ZS_D24X8) return (double)(mode ? v >> 8 : v & 0xffffff) / 16777215.0;
	return (double)(v & 0xffff) / 65535.0;
}

struct vres {
	long covered, expect_vis; double area_err_pct;
	long checked, bad; long cls_n[1u << MAXD], cls_bad[1u << MAXD];   /* classes: bitmask of draws strongly covering the pixel */
	long zs_checked, zs_bad[2]; int zs_mode; double zs_maxerr;
	uint32_t seen[6]; unsigned nseen;
};

static void note(struct vres *r, uint32_t v)
{
	for (unsigned i = 0; i < r->nseen; i++) if (r->seen[i] == v) return;
	if (r->nseen < 6) r->seen[r->nseen++] = v;
}

/* Whole-image analytic check. Pixels within 1.0 px of any triangle edge are ambiguous (fill-rule dependent) and skipped for the
 * per-pixel comparison; the area check uses exact pixel-centre coverage and the same 2 % limit as t_triangle. */
static void verify(const struct scene *sc, enum kb_func fn, const uint32_t *img, const uint32_t *zbuf, struct vres *r)
{
	const uint32_t clear = kb_fb_clear_word(CLEAR_R, CLEAR_G, CLEAR_B, CLEAR_A);
	double tv[MAXD][3][2];
	for (unsigned d = 0; d < sc->nd; d++) tri_px(&sc->d[d], tv[d]);
	for (int m = 0; m < 2; m++) r->zs_bad[m] = 0;
	for (unsigned y = 0; y < H; y++)
		for (unsigned x = 0; x < W; x++) {
			double cx = x + 0.5, cy = y + 0.5;
			uint32_t got = img[(size_t)y * W + x];
			uint32_t exp_c = clear, exp_e = clear;     /* exp_c: strict classes; exp_e: exact pixel-centre resolve */
			double zc = sc->clear_z, ze = sc->clear_z;
			bool amb = false;
			unsigned mask = 0;
			for (unsigned d = 0; d < sc->nd; d++) {
				double dist = tri_dist(tv[d], cx, cy);
				double z = (double)sc->d[d].z;
				if (dist >= 0.0 && (!sc->depth || cmp_pass(fn, z, ze))) { exp_e = col_word(sc->d[d].color); ze = sc->depth ? z : ze; }
				if (dist > 1.0) {
					mask |= 1u << d;
					if (!sc->depth || cmp_pass(fn, z, zc)) { exp_c = col_word(sc->d[d].color); if (sc->depth) zc = z; }
				} else if (dist >= -1.0) amb = true;
			}
			if (got != clear) r->covered++;
			if (exp_e != clear) r->expect_vis++;
			if (amb) continue;
			r->checked++; r->cls_n[mask]++;
			bool bad = false;
			for (int c = 0; c < 4; c++) {
				int dd = (int)((got >> (8 * c)) & 0xff) - (int)((exp_c >> (8 * c)) & 0xff);
				if (dd > 1 || dd < -1) bad = true;
			}
			if (bad) { r->bad++; r->cls_bad[mask]++; note(r, got); }
			if (sc->depth && zbuf) {
				r->zs_checked++;
				for (int m = 0; m < (sc->zf == KB_ZS_D24X8 ? 2 : 1); m++) {
					double dz = fabs(decode_z(zbuf[(size_t)y * W + x], sc->zf, m) - zc);
					if (dz > 2e-3) r->zs_bad[m]++;
					if (m == 0 && dz > r->zs_maxerr) r->zs_maxerr = dz;
				}
			}
		}
	r->area_err_pct = r->expect_vis ? ((double)r->covered - (double)r->expect_vis) * 100.0 / (double)r->expect_vis : 0.0;
	r->zs_mode = (sc->zf == KB_ZS_D24X8 && r->zs_bad[1] < r->zs_bad[0]) ? 1 : 0;
}

static bool run_scene(struct fx *f, const struct scene *sc)
{
	struct vres r;
	memset(&r, 0, sizeof r);
	size_t sz = (size_t)W * H * 4;
	struct kb_bo rt, zb;
	int fd = -1, zfd = -1;
	int e = alloc_target(f, sz, 0x5a3c1e87u, &rt, &fd);
	int ez = sc->depth ? alloc_target(f, sz, 0xa5a5a5a5u, &zb, &zfd) : 0;
	if (e || ez) {
		jb(sc->name); jok(0); jstr("error", (e == -1 || ez == -1) ? "dma-heap alloc" : kb_last_error()); jend();
		if (!e) { kb_bo_free(&rt); close(fd); }
		return false;
	}
	int fence = -1, rc;
	uint64_t t0 = 0;
	double us = 0;
	bool fence_ok = false;
	rc = build_and_submit(f, sc, rt.gpu_va, sc->depth ? zb.gpu_va : 0, &fence, &t0);
	if (!rc) {
		rc = kb_fence_wait(f->ctx, fence, (int)f->o.timeout_ms);
		us = (double)(now_ns() - t0) / 1000.0;
		loop_add_us(us);
		close(fence);
		fence_ok = rc == 0;
		if (fence_ok) f->target++;
	}
	uint32_t *img = NULL, *zimg = NULL;
	enum kb_func fn = f->o.func >= 0 ? (enum kb_func)f->o.func : (g_var & 4) ? KB_FUNC_ALWAYS : sc->func;
	bool zs_checked = sc->depth && !f->o.zsnostore;
	if (fence_ok) {
		img = malloc(sz);
		kb_bo_dmabuf_sync(&rt, false, false);
		for (size_t i = 0; i < sz / 4; i++) img[i] = ((volatile uint32_t *)rt.cpu)[i];
		kb_bo_dmabuf_sync(&rt, true, false);
		if (zs_checked) {
			zimg = malloc(sz);
			kb_bo_dmabuf_sync(&zb, false, false);
			for (size_t i = 0; i < sz / 4; i++) zimg[i] = ((volatile uint32_t *)zb.cpu)[i];
			kb_bo_dmabuf_sync(&zb, true, false);
		}
		verify(sc, fn, img, zimg, &r);
	}
	bool colour_ok = fence_ok && r.bad == 0 && r.checked > 0 && fabs(r.area_err_pct) <= 2.0;
	bool zs_ok = !zs_checked || (fence_ok && r.zs_bad[r.zs_mode] == 0 && r.zs_checked > 0);
	bool ok = colour_ok && zs_ok;
	jb(sc->name); jok(ok); ji("fence_rc", rc); jbool("fence_signalled", fence_ok); jd("submit_to_signal_us", us);
	jx("hmask", f->o.hmask); ji("draws", sc->nd); ji("depth_func", sc->depth ? (int)fn : -1);
	if (fence_ok) {
		ji("covered_pixels", r.covered); ji("analytic_visible_pixels", r.expect_vis); jd("area_error_pct", r.area_err_pct);
		ji("checked", r.checked); ji("bad", r.bad);
		jk("class_checked"); putchar('[');
		for (unsigned m = 0; m < (1u << sc->nd); m++) printf("%s%ld", m ? "," : "", r.cls_n[m]);
		putchar(']'); jk("class_bad"); putchar('[');
		for (unsigned m = 0; m < (1u << sc->nd); m++) printf("%s%ld", m ? "," : "", r.cls_bad[m]);
		putchar(']');
		if (zs_checked) {
			jbool("zs_ok", zs_ok); ji("zs_checked", r.zs_checked); ji("zs_bad", r.zs_bad[r.zs_mode]);
			ji("zs_decode_mode", r.zs_mode); jd("zs_max_err_mode0", r.zs_maxerr);
			jk("zs_sample"); putchar('[');
			static const unsigned sp[][2] = { { 10, 10 }, { 100, 100 }, { 128, 100 }, { 128, 140 }, { 160, 80 } };
			for (unsigned i = 0; i < 5; i++) printf("%s\"0x%08x\"", i ? "," : "", zimg[sp[i][1] * W + sp[i][0]]);
			putchar(']');
		}
		jk("sample"); putchar('[');
		static const unsigned sp[][2] = { { 10, 10 }, { 100, 100 }, { 128, 100 }, { 128, 140 }, { 160, 80 } };
		for (unsigned i = 0; i < 5; i++) printf("%s\"0x%08x\"", i ? "," : "", img[sp[i][1] * W + sp[i][0]]);
		putchar(']');
		if (r.nseen) {
			jk("unexpected_values"); putchar('[');
			for (unsigned i = 0; i < r.nseen; i++) printf("%s\"0x%08x\"", i ? "," : "", r.seen[i]);
			putchar(']');
		}
	} else jstr("error", kb_last_error());
	jend();
	if (f->o.dump || !ok) dump_all(sc->name);
	if (!ok) {
		drain_events(f);
		jb("fault_state"); jok(0); jbool("queue_faulted", kb_queue_faulted(&f->q));
		jx("cs_extract", kb_queue_extract(&f->q)); jx("cs_insert", f->q.insert); jend();
	}
	free(img); free(zimg);
	kb_bo_free(&rt); close(fd);
	if (sc->depth) { kb_bo_free(&zb); close(zfd); }
	return ok;
}

/* Runs scenes[0..n); --only K runs just scene K (1-based). Stops at the first failure. Default pk/zs: pk0/zs0 (force early) with depth,
 * pk2/zs0 (weak early kill) without, as pan_earlyzs_get gives for this shader. */
static int r2_run1(int argc, char **argv, const struct scene *scenes, unsigned n, bool any_depth, int inner)
{
	bool sweep = false;
	struct fx f;
	memset(&f, 0, sizeof f);
	kb_bo_extra_flags = 0x800; f.o.tile_log2 = 12; f.o.hmask = 0; f.o.pk = any_depth ? 0 : 2; f.o.zs = 0; f.o.slot = 3; f.o.timeout_ms = 3000; f.o.chunk = 2u << 20; f.o.func = -1;
	f.o.only = 0;   /* all scenes (--only N = scene N only) */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dump")) f.o.dump = true;
		else if (!strcmp(argv[i], "--zsnostore")) f.o.zsnostore = true;
		else if (!strcmp(argv[i], "--hmask") && i + 1 < argc) f.o.hmask = (unsigned)strtoul(argv[++i], NULL, 16);
		else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) f.o.timeout_ms = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--pk") && i + 1 < argc) f.o.pk = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--zs") && i + 1 < argc) f.o.zs = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--slot") && i + 1 < argc) f.o.slot = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--only") && i + 1 < argc) f.o.only = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--var") && i + 1 < argc) g_var = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--sweep")) sweep = true;
		else if (!strcmp(argv[i], "--tf-or") && i + 1 < argc) g_tf_or = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--tile-log2") && i + 1 < argc) f.o.tile_log2 = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bo-flags") && i + 1 < argc) kb_bo_extra_flags = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--func") && i + 1 < argc) f.o.func = atoi(argv[++i]) & 7;
		else if (!strcmp(argv[i], "--chunk") && i + 1 < argc) f.o.chunk = (unsigned)strtoul(argv[++i], NULL, 0);
	}
	int failures = 0;
	int r = kb_open(&f.ctx, NULL);
	jb("open"); jok(r == 0); ji("rc", r); jx("opt_hmask", f.o.hmask); ji("opt_pk", f.o.pk); ji("opt_zs", f.o.zs); jend();
	if (r) { jb("open"); jstr("error", kb_last_error()); jend(); return 1; }
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
	memcpy((char *)f.shd.cpu, vs_uni_bin, VS_UNI_SIZE);
	memcpy((char *)f.shd.cpu + 512, fs_uni_bin, FS_UNI_SIZE);
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
		static const unsigned sv[] = { 1, 2, 3, 5, 7 };
		for (unsigned v = 0; v < (sweep ? sizeof sv / sizeof sv[0] : 1u); v++) {
			if (sweep) { ok = true; g_var = sv[v]; jb("variant"); jok(1); ji("var", g_var); jend(); }
			for (unsigned i = 0; i < n; i++) {
				if (f.o.only && f.o.only != i + 1) continue;
				if (!run_scene(&f, &scenes[i])) { ok = false; break; }
			}
			if (sweep && (ok || kb_queue_faulted(&f.q))) break;   /* first passing variant ends the sweep */
		}
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

static const struct scene *g_r2_sc; static unsigned g_r2_n; static bool g_r2_depth;
static int r2_thunk(int argc, char **argv, int inner) { return r2_run1(argc, argv, g_r2_sc, g_r2_n, g_r2_depth, inner); }
static int r2_run(const char *name, int argc, char **argv, const struct scene *scenes, unsigned n, bool any_depth)
{
	g_r2_sc = scenes; g_r2_n = n; g_r2_depth = any_depth;
	return loop_main(name, argc, argv, r2_thunk);
}
