// SPDX-License-Identifier: MIT
/*
 * t_triangle_v13: t_triangle ported to arch 13 (Mali-G925): RUN_IDVS2, v13 IDVS register map (kbase_cs_v13.h), v13 descriptors
 * (kbase_draw_v13.h), shaders_v13.h. Flat + varying triangle (--only 3 default, validated on the Poco); see V13-NEXT2.md.
 * Extra options: --hsr (PanVK HSR bits in FBD + DCD2), --tile-log2 N (tiler ctx Effective Tile Size, default 10), --max-warps N.
 * Derived from t_triangle:
 * One JSON object per line on stdout. Stops at the first failure (a hang = timeout on the fence, then clean teardown).
 *   triangle_flat      one triangle, 3 vertices from a vertex buffer, flat colour from the FS
 *   triangle_varying   same triangle, per-vertex colours interpolated through the hardware varying buffer
 * Options: --dump (hex of every descriptor) --hmask HEX --timeout MS --pk N --zs N --nofpk --only 1|2 --chunk BYTES
 *          --reqzero (REQ_RESOURCE(0) between tiling and fragment) --slot N --tiles16
 *          --revert MASK  undo one r18 fix for bisecting (see TRIANGLE-NEXT.md): 1 attribute/varying pixel format (v6 swizzle 0x688),
 *                         2 no TSD in r24, 4 blend w0/w3, 8 DCD0 (no front-face CCW, weak-early ZS update), 16 varying SPD warp limit
 * Descriptor layouts: Mesa MIT genxml v11.xml; call sequence after panvk (csf/panvk_vX_cmd_draw.c). See kbase_draw.h.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <poll.h>
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
#include <sys/mman.h>
#include "shaders_v13.h"
#include "kbase_draw_v13.h"
#include "kbase_cs_v13.h"
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

#include "replay_v13.h"

struct opts {
	bool dump, notiler, nofpk, reqzero, probe, fresh_heap, fresh_geo, arena_rewind, no_flush_wait;
	unsigned hmask, pk, zs, slot, timeout_ms, chunk, only, revert, settle_us, tile_log2, max_warps;
	bool hsr;
	/* PanVK-difference toggles (each default off; --all-panvk = all): see V13-NEXT2.md */
	bool zsext, cs_tctx, sb_prologue, heap_panvk, panvk_desc, all_panvk;
};

#define W 256u
#define H 256u
#define CLEAR_R 0x10
#define CLEAR_G 0x20
#define CLEAR_B 0x40
#define CLEAR_A 0x80

static unsigned heap_init(const struct opts *o) { return o->heap_panvk ? 10 : 1; }
static unsigned heap_max(const struct opts *o) { return o->heap_panvk ? 400 : 4; }
extern uint64_t kb_bo_extra_flags;   /* src/bo.c opt-in. v13 DEFAULT 0x800 (COHERENT_LOCAL): without it the tiler bins nothing (V13-NEXT2.md). --bo-flags 0 = old behaviour */
static bool g_var_attr, g_sweep;
static unsigned g_idvs_mode, g_rep = 1;
static uint64_t g_exec_flags;   /* --exec-flags F: raw MEM_ALLOC_EX flags for the shader BO + MEM_SYNC (PanVK 0x1817) */
static unsigned g_tf_or = 0x40000, g_vary = 0xffffffffu, g_fbd0_or, g_fbd12_or, g_dcd2_or, g_dcd1;   /* PanVK-diff overrides: --tf-or --vary --fbd0-or --fbd12-or --dcd2-or --dcd1 */
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
		jb("notification"); jok(!ev.fatal && !ev.status); jstr("text", ev.text); ji("type", ev.type); ji("group", ev.group);
		ji("error_type", ev.error_type); jx("status", ev.status); jstr("exception", kb_exception_name(ev.status));
		jx("sideband", ev.sideband); jbool("fatal", ev.fatal); jend();
	}
}

static int wait_log(struct fx *f, int fd, int timeout_ms)
{
	/* own wait: print every kbase notification (kb_fence_wait swallowed the fatal one) */
	int64_t end = now_ns() / 1000000 + timeout_ms;
	for (;;) {
		int64_t left = end - now_ns() / 1000000;
		if (left < 0) left = 0;
		struct pollfd pf[2] = { { fd, POLLIN, 0 }, { kb_fd(f->ctx), POLLIN, 0 } };
		int r = poll(pf, 2, (int)left);
		if (r < 0 && errno == EINTR) continue;
		if (r < 0) return -errno;
		if (pf[0].revents & (POLLIN | POLLERR)) return (pf[0].revents & POLLERR) ? -EIO : 0;
		if (pf[1].revents & POLLIN) {
			struct kb_event ev;
			int e = kb_event_read(f->ctx, &ev, 0);
			jb("notification"); jok(e > 0 && !ev.fatal && !ev.status); ji("rc", e); jstr("text", ev.text); ji("type", ev.type); ji("group", ev.group);
			ji("error_type", ev.error_type); jx("status", ev.status); jstr("exception", kb_exception_name(ev.status));
			jx("sideband", ev.sideband); jbool("fatal", ev.fatal); jend();
			if (e > 0 && ev.fatal) return -EIO;
			continue;
		}
		if (r == 0) return -ETIMEDOUT;
	}
}

static uint32_t ubits(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }

/* triangle in framebuffer pixels: NDC (-.5,-.5) (.5,-.5) (0,.5) through scale/offset 128 */
static const float k_pos[3][4] = { { -0.5f, -0.5f, 0.0f, 1.0f }, { 0.5f, -0.5f, 0.0f, 1.0f }, { 0.0f, 0.5f, 0.0f, 1.0f } };
static const float k_col[3][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 } };
static const double k_v[3][2] = { { 64, 64 }, { 192, 64 }, { 128, 192 } };
#define AREA 8192.0

struct dres {
	int rc; bool fence_ok; double us;
	long covered, exact; double area_err_pct;
	int in_bad, out_bad; int in_n, out_n;
	int interp_max_err; int interp_n;
	uint32_t seen[6]; unsigned nseen;
};

static int build_and_submit(struct fx *f, bool var, struct dres *r, int *fence_out, uint64_t *t0)
{
	/* Default: rolling arena, never the previous draw's VAs. --arena-rewind = the old behaviour (same VAs every draw). */
	if (f->o.arena_rewind) f->ar.off = 0; else kb_arena_begin(&f->ar);
	g_ndr = 0;
	uint64_t va_tctx, va_fbd, va_spd_p, va_spd_v, va_spd_f;
	uint64_t va_vsset, va_vst, va_vsfau, va_fsset, va_fst, va_fsfau, va_vbp, va_vbc, va_bl, va_zsd, va_tsd;

	uint64_t va_smp; void *smp = alloc(f, "sample_locations", sizeof kb_sample_positions, 64, &va_smp);
	uint32_t *tctx = alloc(f, "tiler_ctx", KB_TILER_CTX_SIZE, 64, &va_tctx);
	uint8_t *fbd = alloc(f, "fbd_rtd", KB_FB_DESC_SIZE + 64, 64, &va_fbd);   /* +64: ZS/CRC extension slot (--zsext) */
	uint64_t va_crc = 0, va_sqc = 0;
	uint32_t *crcbuf = f->o.zsext ? alloc(f, "crc_buffer", 4096, 64, &va_crc) : NULL;
	uint32_t *sqc = f->o.cs_tctx ? alloc(f, "subqueue_ctx", 256, 64, &va_sqc) : NULL;
	if ((f->o.zsext && !crcbuf) || (f->o.cs_tctx && !sqc)) return -ENOMEM;
	uint32_t *spd_p = alloc(f, "spd_vs_pos", KB_SPD_SIZE, 32, &va_spd_p);
	uint32_t *spd_v = alloc(f, "spd_vs_varying", KB_SPD_SIZE, 32, &va_spd_v);
	uint32_t *spd_f = alloc(f, "spd_fs", KB_SPD_SIZE, 32, &va_spd_f);
	float *vbp = alloc(f, "vb_pos", sizeof k_pos, 64, &va_vbp);
	float *vbc = alloc(f, "vb_col", sizeof k_col, 64, &va_vbc);
	unsigned nb = 2, vs_entries = KB_VS_ATTRS + 1 + nb;
	uint8_t *vsset = alloc(f, "vs_driver_set", vs_entries * KB_DESC_SET_STRIDE, 64, &va_vsset);
	uint32_t *vst = alloc(f, "vs_res_table", 4 * KB_RES_SIZE, 64, &va_vst);
	uint32_t *vsfau = alloc(f, "vs_fau", 64, 64, &va_vsfau);
	uint8_t *fsset = alloc(f, "fs_driver_set", KB_DESC_SET_STRIDE, 64, &va_fsset);
	uint32_t *fst = alloc(f, "fs_res_table", 4 * KB_RES_SIZE, 64, &va_fst);
	uint32_t *fsfau = alloc(f, "fs_fau", 64, 64, &va_fsfau);
	uint32_t *bl = alloc(f, "blend", KB_BLEND_SIZE, 16, &va_bl);
	uint32_t *zsd = alloc(f, "depth_stencil", KB_ZSD_SIZE, 32, &va_zsd);
	uint32_t *tsd = alloc(f, "local_storage", KB_LS_SIZE, 64, &va_tsd);
	if (!tsd) return -ENOMEM;
	kb_draw_local_storage_none(tsd);
	const uint32_t fmt_rgba32f = KB_PIXFMT_RGBA32F | ((f->o.revert & 1) ? KB_PIXFMT_V6_SWIZZLE_RGBA : 0);
	if (!smp || !tctx || !fbd || !spd_p || !spd_v || !spd_f || !vbp || !vbc || !vsset || !vst || !vsfau || !fsset || !fst || !fsfau || !bl || !zsd) {
		return -ENOMEM;
	}

	memcpy(vbp, k_pos, sizeof k_pos);
	memcpy(vbc, k_col, sizeof k_col);

	/* tiler */
	kb_draw_tiler_heap_desc(f->geo.cpu, f->heap.first_chunk_va, f->heap.chunk_size);
	if (f->o.cs_tctx) {   /* PanVK: context zeroed on the CPU, built by the CS from the subqueue ctx (heap desc @+0x30, geom buf @+0x38) */
		memset(tctx, 0, KB_TILER_CTX_SIZE);
		memset(sqc, 0, 256);
		((uint64_t *)sqc)[KB13_SQC_TILER_HEAP / 8] = f->geo.gpu_va;
		((uint64_t *)sqc)[KB13_SQC_GEOM_BUF / 8] = (f->geo.gpu_va + 4096) | (65536u >> 12);
	} else
		kb_v13_tiler_ctx(tctx, W, H, f->o.hmask, f->o.tile_log2, f->geo.gpu_va, (f->geo.gpu_va + 4096) | (65536u >> 12));

	/* shaders */
	uint64_t vs_va = f->shd.gpu_va, fsc_va = f->shd.gpu_va + 512, fsv_va = f->shd.gpu_va + 1024;
	kb_v13_spd(spd_p, 3, vs_va, VS_WORK_REG_COUNT, VS_PRELOAD, f->o.max_warps);
	(void)spd_v;   /* v13 VS has no secondary (varying) shader: one SPD at r16 */
	kb_v13_spd(spd_f, 2, var ? fsv_va : fsc_va, var ? FS_VAR_WORK_REG_COUNT : FS_CONST_WORK_REG_COUNT, var ? FS_VAR_PRELOAD : FS_CONST_PRELOAD, f->o.max_warps);

	/* VS driver set: attr 0 = position, attr 1 = colour, 2..15 unused, 16 sampler (zero), 17.. vertex buffers */
	kb_draw_attr_1d((uint32_t *)(vsset + 0 * 32), fmt_rgba32f, 0, KB_VS_ATTRS + 1 + 0, 0, 16);
	kb_draw_attr_1d((uint32_t *)(vsset + 1 * 32), fmt_rgba32f, 0, KB_VS_ATTRS + 1 + 1, 0, 16);
	for (unsigned i = 2; i < KB_VS_ATTRS; i++) kb_draw_attr_unused((uint32_t *)(vsset + i * 32));
	kb_draw_buffer((uint32_t *)(vsset + (KB_VS_ATTRS + 1) * 32), va_vbp, sizeof k_pos);
	kb_draw_buffer((uint32_t *)(vsset + (KB_VS_ATTRS + 2) * 32), va_vbc, sizeof k_col);
	if (f->o.panvk_desc) ((uint32_t *)(vsset + KB_VS_ATTRS * 32))[0] = 0x02a99901;   /* PanVK sampler word 0 in entry 16 */
	kb_draw_resource(vst, va_vsset, vs_entries * KB_DESC_SET_STRIDE, 1);

	/* VS FAU: 0..5 viewport scale/offset (x, y, z), 6..7 reserved, 8.. promoted immediates */
	vsfau[0] = ubits(128.0f); vsfau[1] = ubits(128.0f); vsfau[2] = ubits(0.5f);
	vsfau[3] = ubits(128.0f); vsfau[4] = ubits(128.0f); vsfau[5] = ubits(0.5f);
	if (f->o.panvk_desc) { vsfau[2] = ubits(1.0f); vsfau[5] = 0; }   /* PanVK: z scale 1.0, offset 0 (FAU reserved 6 needs a recompile: not done) */
	for (unsigned i = 0; i < VS_NIMM; i++) vsfau[vs_imm[i][0]] = vs_imm[i][1];

	/* FS */
	if (var) {
		/* v13 FS is compiled against the VS layout (LD_VAR_BUF_IMM, spike SPIKE_LAYOUT_IN): no varying attribute descriptor (panvk_vX_cmd_draw.c:519). --var-attr = old descriptor path. */
		if (g_var_attr) kb_v13_attr_varying((uint32_t *)fsset, fmt_rgba32f, 16, 32);
		(void)fs_var_imm;
	} else {
		for (unsigned i = 0; i < FS_CONST_NIMM; i++) fsfau[fs_const_imm[i][0]] = fs_const_imm[i][1];
	}
	if (f->o.panvk_desc && !var) ((uint32_t *)fsset)[0] = 0x02a99901;   /* PanVK flat FS driver set = one Sampler */
	kb_draw_resource(fst, va_fsset, KB_DESC_SET_STRIDE, 1);

	kb_draw_blend_opaque_rgba8(bl);
	fsfau[4] = bl[2]; fsfau[5] = bl[3];   /* PanVK v13 FS FAU words 4,5 = RT conversion descriptor (0x19, 0x000ed000), read by the BLEND instruction */
	if (f->o.revert & 4) { bl[0] = 1u << 9; bl[3] = KB_PIXFMT_RGBA8 | KB_PIXFMT_V6_SWIZZLE_RGBA; }
	kb_v13_zsd_depth(zsd, KB_FUNC_ALWAYS, 0);

	/* render target, FBD */
	struct kb_fb_params p;
	memset(&p, 0, sizeof p);
	p.width = W; p.height = H; p.rt_gpu_va = f->rt_va; p.row_stride = W * 4;
	p.clear_r = CLEAR_R; p.clear_g = CLEAR_G; p.clear_b = CLEAR_B; p.clear_a = CLEAR_A;
	p.tiler_va = f->o.notiler ? 0 : va_tctx; p.sample_locations_va = va_smp;
	memcpy(smp, kb_sample_positions, sizeof kb_sample_positions);
	p.arch = 13;
	int rc = kb_fb_pack(fbd, &p);
	if (rc) return rc;
	{ uint32_t *w = (uint32_t *)fbd;   /* PanVK on v13: tile 2^12 px, colour alloc 16 KiB (w11[12:9], [31:24]=bytes>>10) */
	  unsigned alloc = ((1u << f->o.tile_log2) * 4u) >> 10; if (!alloc) alloc = 1;
	  w[11] = (w[11] & ~((0xfu << 9) | (0xffu << 24))) | ((f->o.tile_log2 & 0xfu) << 9) | (alloc << 24); }
	((uint32_t *)fbd)[0] |= g_fbd0_or; ((uint32_t *)fbd)[12] |= g_fbd12_or;
	if (f->o.zsext) {   /* PanVK layout: FBD(0..31) | ZS/CRC ext (32..47) | RT (48..63); w12 bits 31:28,21; pointer tag 3 */
		uint32_t *w = (uint32_t *)fbd;
		memmove(w + 48, w + 32, 64);
		memset(w + 32, 0, 64);
		w[12] |= 0xd0200000u;
		w[33] = 0x80;                         /* CRC row stride (16 tiles x 8 B) */
		w[34] = 0xc0e00000u; w[35] = 0x40070381u;   /* CRC clear colour of 0x80402010 (PanVK capture) */
		w[36] = (uint32_t)va_crc; w[37] = (uint32_t)(va_crc >> 32);
		memset(crcbuf, 0, 4096);
	}
	if (f->o.hsr) kb_v13_fbd_hsr((uint32_t *)fbd, 1, 1, 0, 0);   /* interleave 0: z_tile_buf_budget unknown */
	kb_wmb();
	if (kb_bo_extra_flags) {   /* cached/coherent-local BOs: CPU->device MEM_SYNC like PanVK */
		struct kb_bo *bs[3] = { &f->ar.bo, &f->geo, &f->hbuf };
		for (int i = 0; i < 3; i++) {
			struct { uint64_t handle, user_addr, size; uint8_t type, pad[7]; } ms = { bs[i]->gpu_va, (uint64_t)(uintptr_t)bs[i]->cpu, bs[i]->size, 1, { 0 } };
			if (ioctl(kb_fd(f->ctx), _IOW(KBASE_IOCTL_TYPE, 15, ms), &ms)) { jb("bo_sync"); jok(0); ji("i", i); ji("errno", errno); jend(); }
		}
	}
	if (f->o.settle_us) {   /* bisect: let CPU descriptor writes settle before anything is queued */
		struct timespec ts = { f->o.settle_us / 1000000u, (long)(f->o.settle_us % 1000000u) * 1000L };
		nanosleep(&ts, NULL);
	}

	uint64_t target = f->target + 1;
	int fence = -1;
	if ((rc = kb_kcpu_cqs_wait_gt64(f->ctx, f->sync.gpu_va, target - 1)) || (rc = kb_kcpu_fence_signal(f->ctx, &fence))) return rc;

	struct kb_dcd_opts dop = { f->o.pk, (f->o.revert & 8) ? 2 : f->o.zs, !f->o.nofpk, !f->o.nofpk, !(f->o.revert & 8) };
	struct kb_cs_buf b;
	b.n = 0;
#define E(x) kb_cs_emit(&b, (x))
#define M48(r, v) E(kb_cs_move48((r), (v)))
#define M32(r, v) E(kb_cs_move32((r), (v)))
#define M64(r, v) do { uint64_t m64_ = (v); M32((r), (uint32_t)m64_); M32((r) + 1, (uint32_t)(m64_ >> 32)); } while (0)  /* M48 drops bits 63:48 (FAU count) */
	/* scratch registers r80+ only: r64:65 is the v13 occlusion-query pointer, r40..47 alias fragment staging */
	E(kb_cs_nop());
	if (f->o.sb_prologue) {   /* PanVK CS preamble (SET_STATE_IMM32 x5): endpoint 3, mask wait 8, other 0, deferred 1, mask stream 0xfff0 */
		E(kb_cs_set_state_imm32(KB13_SET_STATE_SB_SEL_ENDPOINT, f->o.slot));
		E(kb_cs_set_state_imm32(KB13_SET_STATE_SB_MASK_WAIT, 8));
		E(kb_cs_set_state_imm32(KB13_SET_STATE_SB_SEL_OTHER, KB_CS_SLOT_LS));
		E(kb_cs_set_state_imm32(KB13_SET_STATE_SB_SEL_DEFERRED, 1));
		E(kb_cs_set_state_imm32(KB13_SET_STATE_SB_MASK_STREAM, 0xfff0));
	}
	M32(80, 0);
	E(kb_cs_flush_cache2(3, 3, 2, 80));
	/* FLUSH_CACHE2 is asynchronous: WAIT on its slot before RUN_IDVS2 (INTERMITTENT.md). */
	if (!f->o.no_flush_wait) E(kb_cs_wait(1u << KB_CS_SLOT_LS));
	M48(KB13_IDVS_VERTEX_SRT, kb_draw_srt_ptr(va_vst, 1));
	M48(KB13_IDVS_FRAGMENT_SRT, kb_draw_srt_ptr(va_fst, 1));
	M64(KB13_IDVS_VERTEX_FAU, kb_draw_fau_ptr(va_vsfau, VS_FAU_COUNT));
	M64(KB13_IDVS_FRAGMENT_FAU, kb_draw_fau_ptr(va_fsfau, var ? FS_VAR_FAU_COUNT : FS_CONST_FAU_COUNT));
	M48(KB13_IDVS_VERTEX_SPD, va_spd_p);
	M48(KB13_IDVS_FRAGMENT_SPD, va_spd_f);
	M48(KB13_IDVS_VERTEX_TSD, va_tsd);      /* PanVK arch>=12: both TSD registers = the same Local Storage */
	M48(KB13_IDVS_FRAGMENT_TSD, va_tsd);
	M32(KB13_IDVS_GLOBAL_ATTR_OFFSET, 0);
	M32(KB13_IDVS_INDEX_COUNT, 3);
	M32(KB13_IDVS_INSTANCE_COUNT, 1);
	M32(KB13_IDVS_INDEX_OFFSET, 0);
	M32(KB13_IDVS_VERTEX_OFFSET, 0);
	M32(KB13_IDVS_INSTANCE_OFFSET, 0);
	M48(KB13_IDVS_TILER_CTX, va_tctx);
	{ uint64_t sc = kb_draw_scissor(0, 0, W - 1, H - 1);
	  M32(KB13_IDVS_SCISSOR, (uint32_t)sc); M32(KB13_IDVS_SCISSOR + 1, (uint32_t)(sc >> 32)); }
	{ uint64_t vh = kb_v13_viewport_hi(0, 0, W - 1, H - 1), vl = kb_v13_viewport_lo(0.0f, 1.0f);
	  M32(KB13_IDVS_VIEWPORT_HIGH, (uint32_t)vh); M32(KB13_IDVS_VIEWPORT_HIGH + 1, (uint32_t)(vh >> 32));
	  M32(KB13_IDVS_VIEWPORT_LOW, (uint32_t)vl); M32(KB13_IDVS_VIEWPORT_LOW + 1, (uint32_t)(vl >> 32)); }
	M64(KB13_IDVS_INDEX_BUFFER, 0);
	M32(KB13_IDVS_INDEX_BUFFER_SIZE, 0);
	M32(KB13_IDVS_VARY_SIZE, g_vary != 0xffffffffu ? g_vary : VS_GENERIC_SIZE_B);
	M48(KB13_IDVS_ZSD, va_zsd);
	M48(KB13_IDVS_BLEND_DESC, va_bl | 1);
	M32(KB13_IDVS_TILER_FLAGS, kb_v13_prim_flags_triangles() | g_tf_or);
	M32(KB13_IDVS_DCD0, kb_draw_dcd0(&dop));
	M32(KB13_IDVS_DCD1, g_dcd1 ? g_dcd1 : kb_draw_dcd1(0xffff, 1));
	M32(KB13_IDVS_DCD2, kb_v13_dcd2(0, 1, 1, 1, f->o.hsr, f->o.hsr) | g_dcd2_or);
	M32(KB13_IDVS_LINE_WIDTH, 0);
	M32(KB13_IDVS_TILER_FLAGS2, 0);
	M64(KB13_IDVS_OQ, 0);
	M48(88, f->heap.ctx_va);
	E(kb_cs_heap_set(88));
	E(kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, f->o.slot));
	E(kb_cs_set_state_imm32(KB_CS_SB_SEL_OTHER, KB_CS_SLOT_LS));
	E(kb_cs_req_res(KB_CS_RES_IDVS | KB_CS_RES_TILER));
	if (f->o.cs_tctx) {   /* PanVK cs_vt_start prologue (panvk_vX_cmd_draw.c:1296-1423): build the tiler context in the CS */
		M48(KB13_SQC_REG, va_sqc);
		M48(66, 0);
		M32(68, (1u << 18) | ((f->o.tile_log2 & 0xfu) << 20) | (f->o.hmask & 0x1fff));
		M32(69, (W - 1) | ((H - 1) << 16));
		M32(71, 0);
		E(kb_cs_load_multiple(72, KB13_SQC_REG, 0xf, KB13_SQC_TILER_HEAP));   /* r72:73 heap desc, r74:75 geom buf */
		M48(76, 0); M48(78, 0); M48(80, 0);
		M32(70, 0);   /* word 4: layer count - 1 */
		E(kb_cs_wait(1u << KB_CS_SLOT_LS));
		E(kb_cs_store_multiple(66, KB13_IDVS_TILER_CTX, 0xffff, 0));
		E(kb_cs_store_multiple(66, KB13_IDVS_TILER_CTX, 0xfc03, 64));
		E(kb_cs_store_multiple(66, KB13_IDVS_TILER_CTX, 0xfc03, 96));
		M32(66, 1); M32(67, 0x01000000);
		E(kb_cs_store_multiple(66, KB13_SQC_REG, 0x3, KB13_SQC_TD_COUNT));
		E(kb_cs_wait(1u << KB_CS_SLOT_LS));
	}
	if (f->o.sb_prologue) {   /* cs_next_iter_sb: pick the next endpoint scoreboard, then set the indirect wait / stream masks */
		E(kb13_cs_next_sb_entry(66, KB13_SB_TYPE_ENDPOINT, 0));
		M32(67, 0);
		E(kb13_cs_bit_set32(67, 67, 66));
		E(kb13_cs_set_state(KB13_SET_STATE_SB_MASK_WAIT, 67));
		M32(67, 0xfff8);
		E(kb13_cs_bit_clear32(67, 67, 66));
		E(kb13_cs_set_state(KB13_SET_STATE_SB_MASK_STREAM, 67));
	}
	E(kb_cs_heap_operation(KB_CS_HEAP_VT_STARTED));
	E(kb13_cs_run_idvs2(0, g_idvs_mode));   /* flags_override 0: non-indexed draw */
	E(kb_cs_finish_tiling());
	E(kb_cs_wait(1u << f->o.slot));
	E(kb_cs_heap_operation(KB_CS_HEAP_VT_COMPLETED));
	if (f->o.reqzero) E(kb_cs_req_res(0));
	E(kb_cs_req_res(KB_CS_RES_FRAGMENT));
	E(kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, f->o.slot));
	M48(40, kb_fb_pointer_ex(va_fbd, 1, f->o.zsext));
	M32(42, kb_fb_bbox_min(0, 0));
	M32(43, kb_fb_bbox_max(W - 1, H - 1));
	M48(82, va_tctx);
	M48(44, 0); M32(46, 0);   /* FRAGMENT TEM_POINTER / ROW_STRIDE alias the IDVS viewport regs: zero like t_triangle */
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
	(void)r;
	return 0;
}

static uint32_t px(const uint32_t *img, unsigned x, unsigned y) { return img[(size_t)y * W + x]; }

static bool near_px(uint32_t got, const int e[4], int tol, int *maxerr)
{
	bool ok = true;
	for (int c = 0; c < 4; c++) {
		int d = (int)((got >> (8 * c)) & 0xff) - e[c];
		if (d < 0) d = -d;
		if (d > *maxerr) *maxerr = d;
		if (d > tol) ok = false;
	}
	return ok;
}

static void note(struct dres *r, uint32_t v)
{
	for (unsigned i = 0; i < r->nseen; i++) if (r->seen[i] == v) return;
	if (r->nseen < 6) r->seen[r->nseen++] = v;
}

static void verify(const uint32_t *img, bool var, struct dres *r)
{
	const uint32_t clear = kb_fb_clear_word(CLEAR_R, CLEAR_G, CLEAR_B, CLEAR_A);
	r->covered = 0; r->exact = 0;
	for (unsigned y = 0; y < H; y++)
		for (unsigned x = 0; x < W; x++) {
			if ((img[(size_t)y * W + x] >> 24) == 0xff) r->covered++;
			double px_ = x + 0.5, py = y + 0.5;
			double e0 = (k_v[1][0] - k_v[0][0]) * (py - k_v[0][1]) - (k_v[1][1] - k_v[0][1]) * (px_ - k_v[0][0]);
			double e1 = (k_v[2][0] - k_v[1][0]) * (py - k_v[1][1]) - (k_v[2][1] - k_v[1][1]) * (px_ - k_v[1][0]);
			double e2 = (k_v[0][0] - k_v[2][0]) * (py - k_v[2][1]) - (k_v[0][1] - k_v[2][1]) * (px_ - k_v[2][0]);
			if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) r->exact++;
		}
	r->area_err_pct = ((double)r->covered - AREA) * 100.0 / AREA;

	static const unsigned out_pts[][2] = { { 10, 10 }, { 245, 10 }, { 10, 245 }, { 245, 245 }, { 128, 40 }, { 128, 230 }, { 30, 128 }, { 230, 128 } };
	for (unsigned i = 0; i < sizeof out_pts / sizeof out_pts[0]; i++) {
		uint32_t v = px(img, out_pts[i][0], out_pts[i][1]);
		r->out_n++;
		if (v != clear) { r->out_bad++; note(r, v); }
	}
	if (!var) {
		static const unsigned in_pts[][2] = { { 128, 106 }, { 100, 70 }, { 156, 70 }, { 128, 150 } };
		const int e[4] = { 51, 102, 153, 255 };
		for (unsigned i = 0; i < sizeof in_pts / sizeof in_pts[0]; i++) {
			uint32_t v = px(img, in_pts[i][0], in_pts[i][1]);
			int me = 0;
			r->in_n++;
			if (!near_px(v, e, 1, &me)) { r->in_bad++; note(r, v); }
			if (me > r->interp_max_err) r->interp_max_err = me;
		}
	} else {
		/* barycentric weights: centroid and near each corner */
		static const double wt[4][3] = { { 1 / 3.0, 1 / 3.0, 1 / 3.0 }, { 0.9, 0.05, 0.05 }, { 0.05, 0.9, 0.05 }, { 0.05, 0.05, 0.9 } };
		for (unsigned i = 0; i < 4; i++) {
			double X = 0, Y = 0;
			for (int k = 0; k < 3; k++) { X += wt[i][k] * k_v[k][0]; Y += wt[i][k] * k_v[k][1]; }
			unsigned x = (unsigned)X, y = (unsigned)Y;
			/* exact weights at this pixel's centre */
			double cx = x + 0.5, cy = y + 0.5;
			double d = (k_v[1][1] - k_v[2][1]) * (k_v[0][0] - k_v[2][0]) + (k_v[2][0] - k_v[1][0]) * (k_v[0][1] - k_v[2][1]);
			double l0 = ((k_v[1][1] - k_v[2][1]) * (cx - k_v[2][0]) + (k_v[2][0] - k_v[1][0]) * (cy - k_v[2][1])) / d;
			double l1 = ((k_v[2][1] - k_v[0][1]) * (cx - k_v[2][0]) + (k_v[0][0] - k_v[2][0]) * (cy - k_v[2][1])) / d;
			double l2 = 1.0 - l0 - l1;
			int e[4] = { (int)lround(255 * l0), (int)lround(255 * l1), (int)lround(255 * l2), 255 };
			int me = 0;
			uint32_t v = px(img, x, y);
			r->in_n++;
			if (!near_px(v, e, 4, &me)) { r->in_bad++; note(r, v); }
			if (me > r->interp_max_err) r->interp_max_err = me;
			r->interp_n++;
		}
	}
}

static bool draw_test(struct fx *f, const char *name, bool var)
{
	struct dres r;
	memset(&r, 0, sizeof r);
	if (f->o.fresh_heap) {   /* bisect 1: new tiler heap per draw */
		kb_tiler_heap_destroy(&f->heap);
		if (kb_tiler_heap_create(f->ctx, f->o.chunk, heap_init(&f->o), heap_max(&f->o), &f->heap)) { jb(name); jok(0); jstr("error", "heap recreate"); jend(); return false; }
	}
	if (f->o.fresh_geo) {    /* bisect 2: new geo BO per draw */
		kb_bo_free(&f->geo);
		if (kb_bo_alloc(f->ctx, 4096 + 65536, KB_BO_GPU_RD | KB_BO_GPU_WR, &f->geo)) { jb(name); jok(0); jstr("error", "geo recreate"); jend(); return false; }
		memset(f->geo.cpu, 0, 4096 + 65536);
	}
	/* render target */
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
	r.rc = build_and_submit(f, var, &r, &fence, &t0);
	if (!r.rc) {
		r.rc = wait_log(f, fence, (int)f->o.timeout_ms);
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
		verify(img, var, &r);
	}
	bool ok = r.fence_ok && r.out_bad == 0 && r.in_bad == 0 && r.in_n > 0 && fabs(r.area_err_pct) <= 2.0;
	jb(name); jok(ok); ji("fence_rc", r.rc); jbool("fence_signalled", r.fence_ok); jd("submit_to_signal_us", r.us);
	jx("hmask", f->o.hmask);
	for (int i = 0; i < g_ndr; i++)
		if (!strcmp(g_dr[i].tag, "tiler_ctx")) {   /* PanVK after one binned triangle: polygon-list bits 63:48 = 4, private state word 0 = 1 */
			const volatile uint32_t *tc = g_dr[i].p;
			jx("tiler_polylist_hi16", tc[1] >> 16); jx("tiler_state_w0", tc[24]);
			jk("tctx_after"); putchar('[');
			for (unsigned k = 0; k < 32; k++) printf("%s\"%08x\"", k ? "," : "", tc[k]);
			putchar(']');
		}
	if (r.fence_ok) {
		ji("covered_pixels", r.covered); ji("analytic_area", (long long)AREA); ji("analytic_pixel_centres", r.exact);
		jd("area_error_pct", r.area_err_pct);
		ji("inside_checked", r.in_n); ji("inside_bad", r.in_bad); ji("outside_checked", r.out_n); ji("outside_bad", r.out_bad);
		ji(var ? "interp_max_channel_err" : "flat_max_channel_err", r.interp_max_err);
		jk("sample"); putchar('[');
		static const unsigned sp[][2] = { { 128, 106 }, { 70, 66 }, { 186, 66 }, { 128, 186 }, { 10, 10 } };
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
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--replay") && i + 1 < argc) {   /* PanVK capture replay, see replay_v13.h */
			struct rp_opts ro;
			memset(&ro, 0, sizeof ro);
			ro.timeout_ms = 3000;
			for (int k = 1; k < argc; k++) {
				if (!strcmp(argv[k], "--swap-shaders")) ro.swap_shaders = true;
				else if (!strcmp(argv[k], "--swap-spd")) ro.swap_spd = true;
				else if (!strcmp(argv[k], "--no-sync")) ro.nosync = true;
				else if (!strcmp(argv[k], "--plain-flags")) ro.plain = true;
				else if (!strcmp(argv[k], "--exec-flags") && k + 1 < argc) ro.exec_flags = strtoull(argv[++k], NULL, 0);
				else if (!strcmp(argv[k], "--timeout") && k + 1 < argc) ro.timeout_ms = (unsigned)atoi(argv[++k]);
			}
			return rp_run(argv[i + 1], &ro);
		}
	struct fx f;
	memset(&f, 0, sizeof f);
	kb_bo_extra_flags = 0x800;   /* v13 default, see above */
	/* pk 2 / zs 0 = PanVK DCD0 0x0481000b: weak-early pixel kill, force-early ZS update */
	f.o.tile_log2 = 12; f.o.only = 3;   /* 3 = flat + varying, both validated; 1 / 2 = one of them */
	f.o.hmask = 0; f.o.pk = 2; f.o.zs = 0; f.o.slot = 3; f.o.timeout_ms = 3000; f.o.chunk = 2u << 20;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dump")) f.o.dump = true;
		else if (!strcmp(argv[i], "--probe")) f.o.probe = true;
		else if (!strcmp(argv[i], "--notiler")) f.o.notiler = true;
		else if (!strcmp(argv[i], "--fresh-heap")) f.o.fresh_heap = true;
		else if (!strcmp(argv[i], "--fresh-geo")) f.o.fresh_geo = true;
		else if (!strcmp(argv[i], "--arena-adv")) { /* now the default */ }
		else if (!strcmp(argv[i], "--arena-rewind")) f.o.arena_rewind = true;
		else if (!strcmp(argv[i], "--no-flush-wait")) f.o.no_flush_wait = true;
		else if (!strcmp(argv[i], "--settle-us") && i + 1 < argc) f.o.settle_us = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--hsr")) f.o.hsr = true;
		else if (!strcmp(argv[i], "--tile-log2") && i + 1 < argc) f.o.tile_log2 = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--max-warps") && i + 1 < argc) f.o.max_warps = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--tf-or") && i + 1 < argc) g_tf_or = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--vary") && i + 1 < argc) g_vary = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--fbd0-or") && i + 1 < argc) g_fbd0_or = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--fbd12-or") && i + 1 < argc) g_fbd12_or = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--dcd2-or") && i + 1 < argc) g_dcd2_or = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--dcd1") && i + 1 < argc) g_dcd1 = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--var-attr")) g_var_attr = true;
		else if (!strcmp(argv[i], "--rep") && i + 1 < argc) g_rep = (unsigned)atoi(argv[++i]);   /* repeat each draw N times (same settings) */
		else if (!strcmp(argv[i], "--sweep")) g_sweep = true;   /* varying bisect: several register variants in one process */
		else if (!strcmp(argv[i], "--idvs-mode") && i + 1 < argc) g_idvs_mode = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--zsext")) f.o.zsext = true;
		else if (!strcmp(argv[i], "--cs-tctx")) f.o.cs_tctx = true;
		else if (!strcmp(argv[i], "--sb-prologue")) f.o.sb_prologue = true;
		else if (!strcmp(argv[i], "--heap-panvk")) f.o.heap_panvk = true;
		else if (!strcmp(argv[i], "--panvk-desc")) f.o.panvk_desc = true;
		else if (!strcmp(argv[i], "--all-panvk")) f.o.all_panvk = true;
		else if (!strcmp(argv[i], "--bo-flags") && i + 1 < argc) kb_bo_extra_flags = strtoull(argv[++i], NULL, 0);   /* e.g. 0x1800 (+MEM_SYNC before submit) */
		else if (!strcmp(argv[i], "--exec-flags") && i + 1 < argc) g_exec_flags = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--nofpk")) f.o.nofpk = true;
		else if (!strcmp(argv[i], "--reqzero")) f.o.reqzero = true;
		else if (!strcmp(argv[i], "--hmask") && i + 1 < argc) f.o.hmask = (unsigned)strtoul(argv[++i], NULL, 16);
		else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) f.o.timeout_ms = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--pk") && i + 1 < argc) f.o.pk = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--zs") && i + 1 < argc) f.o.zs = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--slot") && i + 1 < argc) f.o.slot = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--only") && i + 1 < argc) f.o.only = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--revert") && i + 1 < argc) f.o.revert = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--chunk") && i + 1 < argc) f.o.chunk = (unsigned)strtoul(argv[++i], NULL, 0);
	}
	if (f.o.all_panvk) {   /* every PanVK difference at once, incl. the register-value ones (tiler flags, vary size, FBD w0, DCD2) */
		f.o.zsext = f.o.cs_tctx = f.o.sb_prologue = f.o.heap_panvk = f.o.panvk_desc = true;
		/* NOT --tf-or 0x40000 / --vary 16: they describe PanVK's VS (secondary shader, 16 B varying size); our VS binary has no secondary shader and a 32 B layout */
		g_fbd0_or |= 0x7000; g_dcd2_or |= 0x0c000000;
	}
	if (f.o.heap_panvk) f.o.chunk = 0x100000;   /* PanVK: 1 MiB chunks, 10 initial, 400 max */
	int failures = 0;
	int r = kb_open(&f.ctx, NULL);
	jb("open"); jok(r == 0); ji("rc", r); jx("opt_hmask", f.o.hmask); ji("opt_pk", f.o.pk); ji("opt_zs", f.o.zs);
	jbool("opt_zsext", f.o.zsext); jbool("opt_cs_tctx", f.o.cs_tctx); jbool("opt_sb_prologue", f.o.sb_prologue); jbool("opt_heap_panvk", f.o.heap_panvk);
	jbool("opt_panvk_desc", f.o.panvk_desc); jx("opt_tf_or", g_tf_or); jx("opt_fbd0_or", g_fbd0_or); jx("opt_dcd2_or", g_dcd2_or); jx("opt_bo_flags", kb_bo_extra_flags);
	jbool("opt_nofpk", f.o.nofpk); jbool("opt_reqzero", f.o.reqzero); jx("opt_revert", f.o.revert);
	jbool("opt_arena_rewind", f.o.arena_rewind); jbool("opt_no_flush_wait", f.o.no_flush_wait); ji("opt_settle_us", f.o.settle_us);
	if (r) { jstr("error", kb_last_error()); jend(); return 1; }
	jend();
	int e[5];
	e[0] = kb_bo_alloc(f.ctx, 64 * 1024, KB_BO_GPU_RD | KB_BO_GPU_WR, &f.ar.bo);
	e[1] = kb_bo_alloc(f.ctx, 4096 + 65536, KB_BO_GPU_RD | KB_BO_GPU_WR, &f.geo);
	e[2] = kb_bo_alloc(f.ctx, 4096, KB_BO_GPU_RD | KB_BO_GPU_EX, &f.shd);
	e[3] = kb_bo_alloc(f.ctx, 4096, KB_BO_CSF_EVENT | KB_BO_GPU_RD | KB_BO_GPU_WR, &f.sync);
	e[4] = kb_bo_alloc(f.ctx, 4096, KB_BO_GPU_RD | KB_BO_GPU_WR, &f.hbuf);
	if (e[0] || e[1] || e[2] || e[3] || e[4]) { jb("setup"); jok(0); jstr("error", kb_last_error()); jend(); failures++; goto teardown; }
	if (g_exec_flags) {   /* replace the shader BO by a raw allocation with PanVK's flags */
		kb_bo_free(&f.shd);
		union kbase_ioctl_mem_alloc_ex rq;
		memset(&rq, 0, sizeof rq);
		rq.in.va_pages = rq.in.commit_pages = 1;
		rq.in.flags = g_exec_flags;
		int xr = ioctl(kb_fd(f.ctx), KBASE_IOCTL_MEM_ALLOC_EX, &rq);
		void *xp = xr ? MAP_FAILED : mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, kb_fd(f.ctx), (off_t)rq.out.gpu_va);
		jb("exec_alloc"); jok(xr == 0 && xp != MAP_FAILED); jx("flags", g_exec_flags); jx("flags_out", rq.out.flags); jx("va", rq.out.gpu_va); jend();
		if (xr || xp == MAP_FAILED) { failures++; goto teardown; }
		f.shd.ctx = f.ctx; f.shd.cpu = xp; f.shd.size = 4096; f.shd.dmabuf_fd = -1; f.shd.same_va = (rq.out.flags & BASE_MEM_SAME_VA) != 0;
		f.shd.gpu_va = f.shd.same_va ? (uint64_t)(uintptr_t)xp : rq.out.gpu_va;
	}
	memset(f.sync.cpu, 0, 4096);
	memset(f.geo.cpu, 0, 4096 + 65536);
	memset(f.shd.cpu, 0, 4096);
	memset(f.hbuf.cpu, 0, 4096);
	memcpy((char *)f.shd.cpu, vs_bin, VS_SIZE);
	memcpy((char *)f.shd.cpu + 512, fs_const_bin, FS_CONST_SIZE);
	memcpy((char *)f.shd.cpu + 1024, fs_var_bin, FS_VAR_SIZE);
	if (g_exec_flags) {
		struct { uint64_t handle, user_addr, size; uint8_t type, pad[7]; } ms = { f.shd.gpu_va, (uint64_t)(uintptr_t)f.shd.cpu, 4096, 1, { 0 } };
		int sr = ioctl(kb_fd(f.ctx), _IOW(KBASE_IOCTL_TYPE, 15, ms), &ms);
		jb("exec_sync"); jok(sr == 0); ji("errno", sr ? errno : 0); jend();
	}
	r = kb_queue_create(f.ctx, NULL, &f.q);
	jb("queue_create"); jok(r == 0); ji("rc", r); jend();
	if (r) { failures++; goto teardown; }

	if (f.o.probe) {   /* CS_TILER_HEAP_INIT with 24-byte layout (buf_desc_va) and different buffer-descriptor BOs */
		struct kb_bo cb, ex;
		int rc1 = kb_bo_alloc(f.ctx, 4096, KB_BO_CPU_CACHED | KB_BO_GPU_RD | KB_BO_GPU_WR, &cb);
		int rc2 = kb_bo_alloc(f.ctx, 4096, KB_BO_CSF_EVENT | KB_BO_GPU_RD | KB_BO_GPU_WR, &ex);
		struct { const char *n; uint64_t va; } v[] = { { "zero", 0 }, { "uncached", f.hbuf.gpu_va }, { "cached", rc1 ? 0 : cb.gpu_va },
			{ "csf_event", rc2 ? 0 : ex.gpu_va }, { "arena64k", f.ar.bo.gpu_va } };
		for (unsigned i = 0; i < 5; i++) {
			struct { uint32_t chunk, init, max; uint16_t tif; uint8_t gid, pad; uint64_t buf; } in24;
			union { struct { uint32_t chunk, init, max; uint16_t tif; uint8_t gid, pad; uint64_t buf; } in; struct { uint64_t heap, chunk; } out; } u;
			(void)in24;
			memset(&u, 0, sizeof u);
			u.in.chunk = 1u << 20; u.in.init = 1; u.in.max = 4; u.in.tif = 1; u.in.buf = v[i].va;
			int rr = ioctl(kb_fd(f.ctx), _IOWR(KBASE_IOCTL_TYPE, 48, u), &u);
			int er = rr ? errno : 0;
			jb("probe"); jstr("buf", v[i].n); jx("buf_va", v[i].va); ji("rc", rr); ji("errno", er);
			if (!rr) {
				jx("heap_va", u.out.heap);
				struct kbase_ioctl_cs_tiler_heap_term t = { u.out.heap };
				ioctl(kb_fd(f.ctx), KBASE_IOCTL_CS_TILER_HEAP_TERM, &t);
			}
			jend();
		}
		if (!rc1) kb_bo_free(&cb);
		if (!rc2) kb_bo_free(&ex);
		goto teardown;
	}
	r = kb_tiler_heap_create(f.ctx, f.o.chunk, heap_init(&f.o), heap_max(&f.o), &f.heap);
	jb("tiler_heap"); jok(r == 0); ji("rc", r); jx("heap_ctx_va", f.heap.ctx_va); jx("first_chunk_va", f.heap.first_chunk_va);
	ji("chunk_size", f.heap.chunk_size); if (r) jstr("error", kb_last_error()); jend();
	if (r) { failures++; goto teardown; }

	for (int it = 0; it < inner; it++) {
		loop_iter_begin();
		bool ok = true;
		if (g_sweep) {
			static const struct { const char *n; unsigned mode, vary, tf, dcd2, pk; } sv[] = {
				{ "sweep_vary16", 0, 16, 0, 0, 2 }, { "sweep_vary48", 0, 48, 0, 0, 2 },
				{ "sweep_secondary", 0, 0xffffffffu, 0x40000, 0, 2 }, { "sweep_dcd2_29", 0, 0xffffffffu, 0, 0x20000000, 2 },
				{ "sweep_pk0", 0, 0xffffffffu, 0, 0, 0 } };
			for (unsigned k = 0; k < sizeof sv / sizeof sv[0]; k++) {
				g_idvs_mode = sv[k].mode; g_vary = sv[k].vary; g_tf_or = sv[k].tf; g_dcd2_or = sv[k].dcd2; f.o.pk = sv[k].pk;
				(void)0; if (!draw_test(&f, sv[k].n, true)) ok = false;
				if (kb_queue_faulted(&f.q)) break;
			}
		} else {
			for (unsigned k = 0; k < g_rep; k++) {
				if (f.o.only != 2 && !draw_test(&f, "triangle_flat", false)) ok = false;
				if (f.o.only != 1 && !draw_test(&f, "triangle_varying", true)) ok = false;
				if (kb_queue_faulted(&f.q)) break;
			}
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

int main(int argc, char **argv)
{
	return loop_main("t_triangle_v13", argc, argv, run);
}
