/* SPDX-License-Identifier: MIT */
/*
 * compiled.h: shared header-only engine for t_compiled.c / t_present.c (ladder R2): one varying triangle drawn with shaders compiled by
 * Mesa's offline compiler (driver/compiler-spike/gen-compiled.sh -> tests/shaders_compiled_v11.h, rot.vert + var.frag) into any dma-heap
 * render target. Frame sequence = t_triangle's (same descriptors, same CS), generalised to a per-frame push constant and N frames in flight.
 *   CMP_REF  shaders of t_triangle (tests/shaders_v11.h pos.vert + var.frag), vertex positions straight from the vertex buffer
 *   CMP_ROT  rot.vert: rotation + offset from a push constant (VS FAU byte 32: cos, sin, ox, oy), same FS
 * Geometry/colours as t_triangle: base triangle (-.5,-.5) (.5,-.5) (0,.5) in NDC, red/green/blue vertices, 256x256 target.
 * Needs rung2.h first (JSON helpers, struct fx, alloc_target, tri_dist).
 */
#include "shaders_compiled_v11.h"

enum cmp_mode { CMP_REF = 0, CMP_ROT = 1 };

#define CMP_SHD_REF_VS 0u
#define CMP_SHD_REF_FS 512u
#define CMP_SHD_ROT_VS 1024u
#define CMP_SHD_ROT_FS 2048u

static const float cmp_pos[3][4] = { { -0.5f, -0.5f, 0.0f, 1.0f }, { 0.5f, -0.5f, 0.0f, 1.0f }, { 0.0f, 0.5f, 0.0f, 1.0f } };
static const float cmp_col[3][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 } };

struct cmp_xf { float c, s, ox, oy; };   /* rotation (cos, sin) then offset, NDC */

static int cmp_setup(struct fx *f)
{
	int e[5];
	e[0] = kb_bo_alloc(f->ctx, 1u << 20, KB_BO_GPU_RD | KB_BO_GPU_WR, &f->ar.bo);   /* 1 MiB arena: many frames in flight, rolling */
	e[1] = kb_bo_alloc(f->ctx, 4096 + 65536, KB_BO_GPU_RD | KB_BO_GPU_WR, &f->geo);
	e[2] = kb_bo_alloc(f->ctx, 4096, KB_BO_GPU_RD | KB_BO_GPU_EX, &f->shd);
	e[3] = kb_bo_alloc(f->ctx, 4096, KB_BO_CSF_EVENT | KB_BO_GPU_RD | KB_BO_GPU_WR, &f->sync);
	e[4] = kb_bo_alloc(f->ctx, 4096, KB_BO_GPU_RD | KB_BO_GPU_WR, &f->hbuf);
	if (e[0] || e[1] || e[2] || e[3] || e[4]) return -1;
	memset(f->sync.cpu, 0, 4096); memset(f->geo.cpu, 0, 4096 + 65536); memset(f->shd.cpu, 0, 4096); memset(f->hbuf.cpu, 0, 4096);
	memcpy((char *)f->shd.cpu + CMP_SHD_REF_VS, vs_bin, VS_SIZE);
	memcpy((char *)f->shd.cpu + CMP_SHD_REF_FS, fs_var_bin, FS_VAR_SIZE);
	memcpy((char *)f->shd.cpu + CMP_SHD_ROT_VS, crot_vs_bin, CROT_VS_SIZE);
	memcpy((char *)f->shd.cpu + CMP_SHD_ROT_FS, crot_fs_bin, CROT_FS_SIZE);
	if (kb_queue_create(f->ctx, NULL, &f->q)) return -2;
	if (kb_tiler_heap_create(f->ctx, f->o.chunk, 1, 4, &f->heap)) return -3;
	return 0;
}

static void cmp_teardown(struct fx *f)
{
	if (f->q.ctx) kb_queue_destroy(&f->q);
	kb_tiler_heap_destroy(&f->heap);
	if (f->ar.bo.ctx) kb_bo_free(&f->ar.bo);
	if (f->geo.ctx) kb_bo_free(&f->geo);
	if (f->shd.ctx) kb_bo_free(&f->shd);
	if (f->sync.ctx) kb_bo_free(&f->sync);
	if (f->hbuf.ctx) kb_bo_free(&f->hbuf);
}

static void cmp_opts_default(struct fx *f)
{
	memset(f, 0, sizeof *f);
	f->o.hmask = 0xfe; f->o.pk = 2; f->o.zs = 0; f->o.slot = 3; f->o.timeout_ms = 3000; f->o.chunk = 2u << 20; f->o.func = -1;
}

/* Build descriptors + CS for one frame and submit. Returns the KCPU fence fd (signalled when the frame is complete incl. cache clean). */
static int cmp_render(struct fx *f, enum cmp_mode mode, struct cmp_xf xf, uint64_t rt_va, int *fence_out, uint64_t *t0)
{
	kb_arena_begin(&f->ar);
	g_ndr = 0;
	uint64_t va_tctx, va_fbd, va_spd_p, va_spd_v, va_spd_f, va_vsset, va_vst, va_vsfau, va_fsset, va_fst, va_fsfau, va_vbp, va_vbc, va_bl, va_zsd, va_tsd, va_smp;
	void *smp = alloc(f, "sample_locations", sizeof kb_sample_positions, 64, &va_smp);
	uint32_t *tctx = alloc(f, "tiler_ctx", KB_TILER_CTX_SIZE, 64, &va_tctx);
	uint8_t *fbd = alloc(f, "fbd_rtd", KB_FB_DESC_SIZE, 64, &va_fbd);
	uint32_t *spd_p = alloc(f, "spd_vs_pos", KB_SPD_SIZE, 32, &va_spd_p);
	uint32_t *spd_v = alloc(f, "spd_vs_varying", KB_SPD_SIZE, 32, &va_spd_v);
	uint32_t *spd_f = alloc(f, "spd_fs", KB_SPD_SIZE, 32, &va_spd_f);
	float *vbp = alloc(f, "vb_pos", sizeof cmp_pos, 64, &va_vbp);
	float *vbc = alloc(f, "vb_col", sizeof cmp_col, 64, &va_vbc);
	unsigned vs_entries = KB_VS_ATTRS + 1 + 2;
	uint8_t *vsset = alloc(f, "vs_driver_set", vs_entries * KB_DESC_SET_STRIDE, 64, &va_vsset);
	uint32_t *vst = alloc(f, "vs_res_table", 4 * KB_RES_SIZE, 64, &va_vst);
	uint32_t *vsfau = alloc(f, "vs_fau", 128, 64, &va_vsfau);
	uint8_t *fsset = alloc(f, "fs_driver_set", KB_DESC_SET_STRIDE, 64, &va_fsset);
	uint32_t *fst = alloc(f, "fs_res_table", 4 * KB_RES_SIZE, 64, &va_fst);
	uint32_t *fsfau = alloc(f, "fs_fau", 64, 64, &va_fsfau);
	uint32_t *bl = alloc(f, "blend", KB_BLEND_SIZE, 16, &va_bl);
	uint32_t *zsd = alloc(f, "depth_stencil", KB_ZSD_SIZE, 32, &va_zsd);
	uint32_t *tsd = alloc(f, "local_storage", KB_LS_SIZE, 64, &va_tsd);
	if (!smp || !tctx || !fbd || !spd_p || !spd_v || !spd_f || !vbp || !vbc || !vsset || !vst || !vsfau || !fsset || !fst || !fsfau || !bl || !zsd || !tsd)
		return -ENOMEM;
	kb_draw_local_storage_none(tsd);
	memcpy(vbp, cmp_pos, sizeof cmp_pos);
	memcpy(vbc, cmp_col, sizeof cmp_col);
	kb_draw_tiler_heap_desc(f->geo.cpu, f->heap.first_chunk_va, f->heap.chunk_size);
	kb_draw_tiler_ctx(tctx, W, H, f->o.hmask, f->geo.gpu_va, (f->geo.gpu_va + 4096) | (65536u >> 12));

	const bool rot = mode == CMP_ROT;
	uint64_t vs_va = f->shd.gpu_va + (rot ? CMP_SHD_ROT_VS : CMP_SHD_REF_VS), fs_va = f->shd.gpu_va + (rot ? CMP_SHD_ROT_FS : CMP_SHD_REF_FS);
	unsigned vs_sec_off = rot ? CROT_VS_SECONDARY_OFFSET : VS_SECONDARY_OFFSET;
	kb_draw_spd(spd_p, 3, vs_va, rot ? CROT_VS_WORK_REG_COUNT : VS_WORK_REG_COUNT, rot ? CROT_VS_PRELOAD : VS_PRELOAD);
	kb_draw_spd_vs_varying(spd_v, vs_va + vs_sec_off, rot ? CROT_VS_SECONDARY_WORK_REG_COUNT : VS_SECONDARY_WORK_REG_COUNT,
			       rot ? CROT_VS_SECONDARY_PRELOAD : VS_SECONDARY_PRELOAD);
	kb_draw_spd(spd_f, 2, fs_va, rot ? CROT_FS_WORK_REG_COUNT : FS_VAR_WORK_REG_COUNT, rot ? CROT_FS_PRELOAD : FS_VAR_PRELOAD);

	kb_draw_attr_1d((uint32_t *)(vsset + 0 * 32), KB_PIXFMT_RGBA32F, 0, KB_VS_ATTRS + 1 + 0, 0, 16);
	kb_draw_attr_1d((uint32_t *)(vsset + 1 * 32), KB_PIXFMT_RGBA32F, 0, KB_VS_ATTRS + 1 + 1, 0, 16);
	for (unsigned i = 2; i < KB_VS_ATTRS; i++) kb_draw_attr_unused((uint32_t *)(vsset + i * 32));
	kb_draw_buffer((uint32_t *)(vsset + (KB_VS_ATTRS + 1) * 32), va_vbp, sizeof cmp_pos);
	kb_draw_buffer((uint32_t *)(vsset + (KB_VS_ATTRS + 2) * 32), va_vbc, sizeof cmp_col);
	kb_draw_resource(vst, va_vsset, vs_entries * KB_DESC_SET_STRIDE, 1);

	/* VS FAU: words 0..5 viewport scale/offset (x, y, z), then (ROT) push block at byte 32, promoted immediates (the 0.5 depth) */
	kb_draw_fau_viewport(vsfau, (float)W, (float)H, 1.0f, 0.0f);
	vsfau[2] = 0x3f000000; vsfau[5] = 0x3f000000;   /* t_triangle: z scale/offset 0.5 */
	unsigned vs_fau_count;
	if (rot) {
		const float pc[4] = { xf.c, xf.s, xf.ox, xf.oy };
		kb_draw_fau_imm(vsfau, crot_vs_imm, CROT_VS_NIMM);
		kb_draw_fau_push(vsfau, 32, pc, 4);
		vs_fau_count = CROT_VS_FAU_COUNT;
	} else {
		kb_draw_fau_imm(vsfau, vs_imm, VS_NIMM);
		vs_fau_count = VS_FAU_COUNT;
	}
	kb_draw_attr_varying((uint32_t *)fsset, KB_PIXFMT_RGBA32F, 16, 32);
	kb_draw_resource(fst, va_fsset, KB_DESC_SET_STRIDE, 1);
	kb_draw_blend_opaque_rgba8(bl);
	kb_draw_zsd_off(zsd);

	struct kb_fb_params p;
	memset(&p, 0, sizeof p);
	p.width = W; p.height = H; p.rt_gpu_va = rt_va; p.row_stride = W * 4;
	p.clear_r = CLEAR_R; p.clear_g = CLEAR_G; p.clear_b = CLEAR_B; p.clear_a = CLEAR_A;
	p.tiler_va = va_tctx; p.sample_locations_va = va_smp;
	memcpy(smp, kb_sample_positions, sizeof kb_sample_positions);
	int rc = kb_fb_pack(fbd, &p);
	if (rc) return rc;
	kb_wmb();

	uint64_t target = f->target + 1;
	int fence = -1;
	if ((rc = kb_kcpu_cqs_wait_gt64(f->ctx, f->sync.gpu_va, target - 1)) || (rc = kb_kcpu_fence_signal(f->ctx, &fence))) return rc;
	f->target++;   /* submitted-frame counter: frame k waits for k-1's SYNC_ADD */

	struct kb_dcd_opts dop = { f->o.pk, f->o.zs, 1, 1, 1 };
	struct kb_cs_buf b;
	b.n = 0;
#define E(x) kb_cs_emit(&b, (x))
#define M48(r, v) E(kb_cs_move48((r), (v)))
#define M32(r, v) E(kb_cs_move32((r), (v)))
#define M64(r, v) do { uint64_t m64_ = (v); M32((r), (uint32_t)m64_); M32((r) + 1, (uint32_t)(m64_ >> 32)); } while (0)
	E(kb_cs_nop());
	M32(72, 0);
	E(kb_cs_flush_cache2(3, 3, 2, 72));
	E(kb_cs_wait(1u << KB_CS_SLOT_LS));
	M48(KB_IDVS_VERTEX_SRT, kb_draw_srt_ptr(va_vst, 1));
	M48(KB_IDVS_FRAGMENT_SRT, kb_draw_srt_ptr(va_fst, 1));
	M64(KB_IDVS_VERTEX_FAU, kb_draw_fau_ptr(va_vsfau, vs_fau_count));
	M64(KB_IDVS_FRAGMENT_FAU, kb_draw_fau_ptr(va_fsfau, rot ? CROT_FS_FAU_COUNT : FS_VAR_FAU_COUNT));
	M48(KB_IDVS_VERTEX_POS_SPD, va_spd_p);
	M48(KB_IDVS_VERTEX_VARY_SPD, va_spd_v);
	M48(KB_IDVS_FRAGMENT_SPD, va_spd_f);
	M48(KB_IDVS_TSD, va_tsd);
	M32(KB_IDVS_GLOBAL_ATTR_OFFSET, 0);
	M32(KB_IDVS_INDEX_COUNT, 3);
	M32(KB_IDVS_INSTANCE_COUNT, 1);
	M32(KB_IDVS_INDEX_OFFSET, 0);
	M32(KB_IDVS_VERTEX_OFFSET, 0);
	M32(KB_IDVS_INSTANCE_OFFSET, 0);
	M32(KB_IDVS_DCD2, kb_draw_dcd2(0, 1, 1, 1));
	M48(KB_IDVS_TILER_CTX, va_tctx);
	{ uint64_t sc = kb_draw_scissor(0, 0, W - 1, H - 1);
	  M32(KB_IDVS_SCISSOR, (uint32_t)sc); M32(KB_IDVS_SCISSOR + 1, (uint32_t)(sc >> 32)); }
	M32(KB_IDVS_LOW_DEPTH_CLAMP, 0);
	M32(KB_IDVS_HIGH_DEPTH_CLAMP, 0x3f800000);
	M48(KB_IDVS_OQ, 0);
	M32(KB_IDVS_VARY_SIZE, rot ? CROT_VS_GENERIC_SIZE_B : VS_GENERIC_SIZE_B);
	M48(KB_IDVS_BLEND_DESC, va_bl | 1);
	M48(KB_IDVS_ZSD, va_zsd);
	M32(KB_IDVS_TILER_FLAGS, kb_draw_prim_flags_triangles(rot ? CROT_VS_SECONDARY_ENABLE : VS_SECONDARY_ENABLE));
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
	E(kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, f->o.slot));
	M48(40, kb_fb_pointer(va_fbd, 1));
	M32(42, kb_fb_bbox_min(0, 0));
	M32(43, kb_fb_bbox_max(W - 1, H - 1));
	M48(74, va_tctx);
	M48(44, 0); M32(46, 0);
	E(kb_cs_run_fragment(0));
	E(kb_cs_wait(1u << f->o.slot));
	E(kb_cs_load_multiple(10, 74, 0xf, 40));
	E(kb_cs_wait(1u << KB_CS_SLOT_LS));
	E(kb_cs_finish_fragment_heap(true, 10, 12));
	E(kb_cs_req_res(0));
	M32(72, 0);
	E(kb_cs_flush_cache2(1, 1, 0, 72));
	E(kb_cs_wait(1u << KB_CS_SLOT_LS));
	M48(68, f->sync.gpu_va);
	M48(70, 1);
	E(kb_cs_sync_add64(68, 70, KB_CS_SCOPE_SYSTEM));
#undef E
#undef M48
#undef M32
#undef M64
	memcpy(g_cs, b.ins, b.n * 8);
	g_ncs = b.n;
	*t0 = now_ns();
	rc = kb_queue_submit(&f->q, b.ins, b.n, KB_KICK_IOCTL);
	if (rc) { close(fence); return rc; }
	*fence_out = fence;
	return 0;
}

/* ---- verification: analytic triangle from the transform, interpolated colours ---- */
struct cmp_res {
	long covered, exact, in_n, in_bad, out_n, out_bad; int maxerr; double area_err_pct; uint32_t fnv;
};

static void cmp_px_verts(enum cmp_mode mode, struct cmp_xf xf, double v[3][2])
{
	if (mode == CMP_REF) xf = (struct cmp_xf){ 1, 0, 0, 0 };
	for (int i = 0; i < 3; i++) {
		double x = (double)cmp_pos[i][0], y = (double)cmp_pos[i][1];
		double nx = x * (double)xf.c - y * (double)xf.s + (double)xf.ox, ny = x * (double)xf.s + y * (double)xf.c + (double)xf.oy;
		v[i][0] = 128.0 + 128.0 * nx; v[i][1] = 128.0 + 128.0 * ny;
	}
}

static void cmp_verify(const uint32_t *img, enum cmp_mode mode, struct cmp_xf xf, struct cmp_res *r)
{
	double v[3][2];
	cmp_px_verts(mode, xf, v);
	const uint32_t clear = kb_fb_clear_word(CLEAR_R, CLEAR_G, CLEAR_B, CLEAR_A);
	double area = (v[1][0] - v[0][0]) * (v[2][1] - v[0][1]) - (v[1][1] - v[0][1]) * (v[2][0] - v[0][0]);
	memset(r, 0, sizeof *r);
	uint32_t h = 2166136261u;
	for (unsigned y = 0; y < H; y++)
		for (unsigned x = 0; x < W; x++) {
			uint32_t g = img[(size_t)y * W + x];
			h = (h ^ g) * 16777619u;
			double d = tri_dist(v, x + 0.5, y + 0.5);
			if ((g >> 24) == 0xff) r->covered++;
			if (d >= 0) r->exact++;
			if (d >= 1.5) {
				double px_ = x + 0.5, py = y + 0.5;
				double w0 = ((v[1][0] - px_) * (v[2][1] - py) - (v[1][1] - py) * (v[2][0] - px_)) / area;
				double w1 = ((v[2][0] - px_) * (v[0][1] - py) - (v[2][1] - py) * (v[0][0] - px_)) / area;
				double w2 = 1.0 - w0 - w1, w[3] = { w0, w1, w2 };
				r->in_n++;
				bool ok = true;
				for (int c = 0; c < 3; c++) {
					double e = 0; for (int k = 0; k < 3; k++) e += w[k] * (double)cmp_col[k][c];
					int ev = (int)lround(e * 255.0), got = (int)((g >> (8 * c)) & 0xff), df = abs(got - ev);
					if (df > r->maxerr) r->maxerr = df;
					if (df > 4) ok = false;
				}
				if ((g >> 24) != 0xff) ok = false;
				if (!ok) r->in_bad++;
			} else if (d <= -1.5) {
				r->out_n++;
				if (g != clear) r->out_bad++;
			}
		}
	r->fnv = h;
	r->area_err_pct = r->exact ? ((double)r->covered - (double)r->exact) * 100.0 / (double)r->exact : 0.0;
}

static bool cmp_ok(const struct cmp_res *r) { return r->in_n > 0 && r->in_bad == 0 && r->out_bad == 0 && r->area_err_pct >= -2.0 && r->area_err_pct <= 2.0; }

/* Copy a render target to malloc memory (CPU read through the dma-buf's own map, bracketed by DMA_BUF_IOCTL_SYNC) */
static uint32_t *cmp_read(struct kb_bo *rt)
{
	size_t sz = (size_t)W * H * 4;
	uint32_t *img = malloc(sz);
	if (!img) return NULL;
	kb_bo_dmabuf_sync(rt, false, false);
	for (size_t i = 0; i < sz / 4; i++) img[i] = ((volatile uint32_t *)rt->cpu)[i];
	kb_bo_dmabuf_sync(rt, true, false);
	return img;
}

static struct cmp_xf cmp_xf_frame(unsigned i, unsigned period)
{
	double a = 6.283185307179586 * (double)(i % period) / (double)period;
	return (struct cmp_xf){ (float)cos(a), (float)sin(a), (float)(0.15 * cos(2 * a)), (float)(0.1 * sin(3 * a)) };
}
