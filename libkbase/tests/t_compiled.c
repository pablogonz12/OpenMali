// SPDX-License-Identifier: MIT
/*
 * t_compiled (ladder R2): a varying triangle whose shaders were compiled by Mesa's offline compiler at build time
 * (driver/compiler-spike/gen-compiled.sh: rot.vert + var.frag, GLSL -> SPIR-V -> Valhall v11, embedded in tests/shaders_compiled_v11.h),
 * rendered by libkbase into dma-heap buffers. Engine: tests/compiled.h.
 *   ref_identity       t_triangle's shaders (pos.vert/var.frag, shaders_v11.h) = the reference
 *   compiled_identity  rot.vert with identity push constant: must equal ref_identity bit for bit (FNV-1a of the whole image) and the analytic check
 *   compiled_rotated   rot.vert, 30 deg + offset (0.10,-0.05) from the push constant
 *   animation          N frames (--frames, default 600), ring of R buffers (--ring, default 4), D frames in flight (--depth, default 1, < ring),
 *                      one rotation per 120 frames, new push constant every frame; every frame read back and verified (--verify-every K);
 *                      prints submit->fence percentiles (frame submit -> its fence signalled, so depth>1 includes queueing).
 * Options: --only N (1..4) --frames N --ring R --depth D --verify-every K --dump --hmask HEX --timeout MS --pk N --zs N --slot N --chunk B --no-anim
 * Pass = analytic check (interior colours +-4, outside = clear colour, area within 2 % of exact pixel-centre count) on every verified frame.
 */
#include "rung2.h"
#include "compiled.h"

struct anim { unsigned frames, ring, depth, every; };

static int cmp_cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static void emit_res(const char *name, const struct cmp_res *r, bool ok, double us, enum cmp_mode m)
{
	jb(name); jok(ok); jstr("shaders", m == CMP_REF ? "shaders_v11.h pos.vert+var.frag" : "shaders_compiled_v11.h rot.vert+var.frag");
	jd("submit_to_signal_us", us); ji("covered_pixels", r->covered); ji("exact_pixels", r->exact); jd("area_error_pct", r->area_err_pct);
	ji("interior_checked", r->in_n); ji("interior_bad", r->in_bad); ji("outside_checked", r->out_n); ji("outside_bad", r->out_bad);
	ji("max_channel_err", r->maxerr); jx("image_fnv", r->fnv); jend();
}

/* one frame into a fresh target, wait, verify */
static bool one(struct fx *f, const char *name, enum cmp_mode m, struct cmp_xf xf, struct cmp_res *res)
{
	struct kb_bo rt; int fd = -1, fence = -1; uint64_t t0 = 0;
	if (alloc_target(f, (size_t)W * H * 4, 0x5a3c1e87u, &rt, &fd)) { jb(name); jok(0); jstr("error", "target alloc"); jend(); return false; }
	int rc = cmp_render(f, m, xf, rt.gpu_va, &fence, &t0);
	double us = 0; bool ok = false;
	if (!rc) {
		rc = kb_fence_wait(f->ctx, fence, (int)f->o.timeout_ms);
		us = (double)(now_ns() - t0) / 1000.0; close(fence);
		if (!rc) {
			uint32_t *img = cmp_read(&rt);
			cmp_verify(img, m, xf, res);
			ok = cmp_ok(res);
			free(img);
		}
	}
	if (rc) { jb(name); jok(0); ji("rc", rc); jstr("error", kb_last_error()); jend(); drain_events(f); }
	else {
		emit_res(name, res, ok, us, m);
		if (!ok || f->o.dump) dump_all(name);
	}
	kb_bo_free(&rt); close(fd);
	return ok;
}

static bool run_anim(struct fx *f, const struct anim *a)
{
	struct kb_bo rt[8]; int fd[8], n = (int)a->ring;
	for (int i = 0; i < n; i++)
		if (alloc_target(f, (size_t)W * H * 4, 0x5a3c1e87u, &rt[i], &fd[i])) { jb("animation"); jok(0); jstr("error", "target alloc"); jend(); return false; }
	double *us = calloc(a->frames, sizeof *us);
	struct inflight { int fence; uint64_t t0; unsigned frame; } q[8];
	unsigned qh = 0, qn = 0, bad = 0, verified = 0, fails = 0; long in_bad = 0, out_bad = 0; int maxerr = 0; double worst_area = 0;
	bool first_bad_dumped = false; unsigned first_bad = ~0u;
	uint64_t wall0 = now_ns();
	for (unsigned i = 0; i < a->frames + a->depth; i++) {
		if (i < a->frames) {
			struct cmp_xf xf = cmp_xf_frame(i, 120);
			struct inflight *s = &q[(qh + qn) % 8];
			int rc = cmp_render(f, CMP_ROT, xf, rt[i % n].gpu_va, &s->fence, &s->t0);
			if (rc) { jb("animation"); jok(0); ji("frame", i); ji("rc", rc); jstr("error", kb_last_error()); jend(); fails++; break; }
			s->frame = i; qn++;
		}
		while (qn && (qn >= a->depth || i >= a->frames)) {
			struct inflight *s = &q[qh];
			int rc = kb_fence_wait(f->ctx, s->fence, (int)f->o.timeout_ms);
			us[s->frame] = (double)(now_ns() - s->t0) / 1000.0; loop_add_us(us[s->frame]);
			close(s->fence);
			unsigned fr = s->frame; qh = (qh + 1) % 8; qn--;
			if (rc) { jb("animation"); jok(0); ji("frame", fr); ji("fence_rc", rc); jend(); fails++; drain_events(f); goto out; }
			if (a->every && fr % a->every == 0) {
				struct cmp_res r; uint32_t *img = cmp_read(&rt[fr % n]);
				/* the buffer may already be reused by a later in-flight frame only if depth >= ring: depth < ring is enforced */
				cmp_verify(img, CMP_ROT, cmp_xf_frame(fr, 120), &r); free(img); verified++;
				if (!cmp_ok(&r)) { bad++; if (first_bad == ~0u) first_bad = fr; }
				in_bad += r.in_bad; out_bad += r.out_bad; if (r.maxerr > maxerr) maxerr = r.maxerr;
				if (fabs(r.area_err_pct) > fabs(worst_area)) worst_area = r.area_err_pct;
				if (!cmp_ok(&r) && !first_bad_dumped) { first_bad_dumped = true; dump_all("animation_first_bad"); }
			}
			if (i < a->frames && qn < a->depth) break;
		}
		if (fails) break;
	}
out:;
	double wall = (double)(now_ns() - wall0) / 1e6;
	unsigned done = a->frames;
	double *s = malloc(done * sizeof *s), sum = 0; memcpy(s, us, done * sizeof *s);
	qsort(s, done, sizeof *s, cmp_cmpd);
	for (unsigned i = 0; i < done; i++) sum += s[i];
	bool ok = !fails && !bad && verified > 0;
	jb("animation"); jok(ok); ji("frames", a->frames); ji("ring", a->ring); ji("depth", a->depth); ji("verified", verified); ji("frames_bad", bad);
	ji("first_bad_frame", first_bad == ~0u ? -1 : (long long)first_bad); ji("interior_bad_total", in_bad); ji("outside_bad_total", out_bad);
	ji("max_channel_err", maxerr); jd("worst_area_error_pct", worst_area);
	jd("submit_to_fence_us_min", s[0]); jd("submit_to_fence_us_mean", sum / done); jd("submit_to_fence_us_p50", s[done / 2]);
	jd("submit_to_fence_us_p95", s[done * 95 / 100]); jd("submit_to_fence_us_p99", s[done * 99 / 100]); jd("submit_to_fence_us_max", s[done - 1]);
	jd("wall_ms_incl_verify", wall);
	jk("per_frame_us"); putchar('[');
	for (unsigned i = 0; i < done; i++) printf("%s%.0f", i ? "," : "", us[i]);
	putchar(']'); jend();
	free(s); free(us);
	for (int i = 0; i < n; i++) { kb_bo_free(&rt[i]); close(fd[i]); }
	return ok;
}

static int run(int argc, char **argv, int inner)
{
	(void)inner;
	struct fx f; cmp_opts_default(&f);
	struct anim a = { 600, 4, 1, 1 };
	unsigned only = 0; bool noanim = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dump")) f.o.dump = true;
		else if (!strcmp(argv[i], "--no-anim")) noanim = true;
		else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) a.frames = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ring") && i + 1 < argc) a.ring = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--depth") && i + 1 < argc) a.depth = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--verify-every") && i + 1 < argc) a.every = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--hmask") && i + 1 < argc) f.o.hmask = (unsigned)strtoul(argv[++i], NULL, 16);
		else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) f.o.timeout_ms = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--pk") && i + 1 < argc) f.o.pk = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--zs") && i + 1 < argc) f.o.zs = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--slot") && i + 1 < argc) f.o.slot = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--chunk") && i + 1 < argc) f.o.chunk = (unsigned)strtoul(argv[++i], NULL, 0);
	}
	if (a.ring < 2) a.ring = 2;
	if (a.ring > 8) a.ring = 8;
	if (a.depth < 1) a.depth = 1;
	if (a.depth >= a.ring) a.depth = a.ring - 1;
	if (a.frames < 1) a.frames = 1;
	int failures = 0;
	int r = kb_open(&f.ctx, NULL);
	jb("open"); jok(r == 0); ji("rc", r); jx("opt_hmask", f.o.hmask); jend();
	if (r) { jb("open"); jstr("error", kb_last_error()); jend(); return 1; }
	r = cmp_setup(&f);
	jb("setup"); jok(r == 0); ji("rc", r); jbool("fs_binary_identical", sizeof crot_fs_bin == sizeof fs_var_bin && !memcmp(crot_fs_bin, fs_var_bin, sizeof fs_var_bin));
	ji("vs_ref_bytes", VS_SIZE); ji("vs_compiled_bytes", CROT_VS_SIZE); ji("vs_compiled_fau_words", CROT_VS_FAU_COUNT); if (r) jstr("error", kb_last_error());
	jend();
	if (r) { failures++; goto teardown; }
	struct cmp_xf id = { 1, 0, 0, 0 }, rotd = { (float)cos(0.5235987755982988), (float)sin(0.5235987755982988), 0.10f, -0.05f };
	struct cmp_res rref, rid, rrot;
	bool ok1 = true, ok2 = true;
	if (!only || only == 1) { ok1 = one(&f, "ref_identity", CMP_REF, id, &rref); if (!ok1) failures++; }
	if (!only || only == 2) {
		ok2 = one(&f, "compiled_identity", CMP_ROT, id, &rid);
		if (!ok2) failures++;
		if (!only) {
			bool match = ok1 && ok2 && rref.fnv == rid.fnv;
			jb("compiled_vs_reference"); jok(match); jx("fnv_reference", rref.fnv); jx("fnv_compiled", rid.fnv); jend();
			if (!match) failures++;
		}
	}
	if (!only || only == 3) if (!one(&f, "compiled_rotated", CMP_ROT, rotd, &rrot)) failures++;
	if (!noanim && (!only || only == 4)) if (!run_anim(&f, &a)) failures++;
teardown:
	cmp_teardown(&f);
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
	return loop_main("t_compiled", argc, argv, (int (*)(int, char **, int))run);
}
