// SPDX-License-Identifier: MIT
/*
 * t_clear: fragment-only clear of an imported dma-heap RGBA8 buffer on Mali v11 (CSF), built on libkbase.
 * One JSON object per line on stdout. Stops at the first failure (never loops on a fault).
 *   clear_256_*   256x256, colours red / grey / asymmetric, every pixel checked via the dma-buf's own mmap
 *   clear_1280x720  tile-alignment check
 *   clear_loop    200 clears, mean/p99 submit->signal
 *   clear_poll    KCPU fence attached as the dma-buf write fence; poll(POLLIN) ready only after the clear
 * Options: --slot N (endpoint scoreboard slot, default 3) --finish (emit FINISH_FRAGMENT) --noclean
 *          (clear Clean Tile Write Enable) --tiled (block format u-interleaved) --n N (loop count)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kbase.h"
#include "kbase_fb.h"

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
static void jd(const char *k, double v) { jk(k); printf("%.2f", v); }
static void jbool(const char *k, bool v) { jk(k); fputs(v ? "true" : "false", stdout); }
static void jend(void) { fputs("}\n", stdout); fflush(stdout); }

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

struct opts { unsigned slot; bool finish, noclean, tiled, nowait, noflush; int n; };

struct fx {
	struct kb_ctx *ctx;
	struct kb_bo desc;   /* descriptors, uncached CPU */
	struct kb_bo sync;   /* CSF event memory */
	struct kb_queue q;
	uint64_t target;
	struct opts o;
};

struct clr { uint8_t r, g, b, a; const char *name; };
static const struct clr k_red = { 0xff, 0, 0, 0xff, "red" };
static const struct clr k_grey = { 0x80, 0x80, 0x80, 0xff, "grey50" };
static const struct clr k_asym = { 0x10, 0x20, 0x40, 0x80, "asym_0x80402010" };

struct rt { struct kb_bo bo; int fd; unsigned w, h, stride; };

static int rt_make(struct fx *f, struct rt *rt, unsigned w, unsigned h)
{
	memset(rt, 0, sizeof *rt);
	rt->w = w; rt->h = h; rt->stride = w * 4;
	size_t sz = ((size_t)rt->stride * h + 4095) & ~(size_t)4095;
	rt->fd = kb_dmaheap_alloc(KB_HEAP_SYSTEM, sz);
	if (rt->fd < 0) return rt->fd;
	int r = kb_bo_import_dmabuf(f->ctx, rt->fd, sz, KB_BO_GPU_RD | KB_BO_GPU_WR, &rt->bo);
	if (r) { close(rt->fd); rt->fd = -1; }
	return r;
}
static void rt_free(struct rt *rt)
{
	if (rt->bo.ctx) kb_bo_free(&rt->bo);
	if (rt->fd >= 0) close(rt->fd);
}

static const uint32_t k_prefill = 0x5a3c1e87u;

static void rt_prefill(struct rt *rt)
{
	kb_bo_dmabuf_sync(&rt->bo, false, true);
	volatile uint32_t *p = rt->bo.cpu;
	for (size_t i = 0; i < (size_t)rt->stride * rt->h / 4; i++) p[i] = k_prefill;
	kb_bo_dmabuf_sync(&rt->bo, true, true);
}

struct vres { size_t bad, untouched, total; size_t first_bad; uint32_t first_val, exp; uint32_t seen[4]; unsigned nseen; };

static struct vres rt_verify(struct rt *rt, const struct clr *c)
{
	struct vres v;
	memset(&v, 0, sizeof v);
	v.exp = kb_fb_clear_word(c->r, c->g, c->b, c->a);
	kb_bo_dmabuf_sync(&rt->bo, false, false);
	const volatile uint32_t *p = rt->bo.cpu;
	v.total = (size_t)rt->w * rt->h;
	for (unsigned y = 0; y < rt->h; y++)
		for (unsigned x = 0; x < rt->w; x++) {
			uint32_t got = p[(size_t)y * rt->stride / 4 + x];
			if (got == v.exp) continue;
			if (!v.bad) { v.first_bad = (size_t)y * rt->w + x; v.first_val = got; }
			v.bad++;
			if (got == k_prefill) v.untouched++;
			bool known = false;
			for (unsigned i = 0; i < v.nseen; i++) known |= v.seen[i] == got;
			if (!known && v.nseen < 4) v.seen[v.nseen++] = got;
		}
	kb_bo_dmabuf_sync(&rt->bo, true, false);
	return v;
}

static void dump_desc(const struct fx *f, const char *tag)
{
	const uint32_t *d = f->desc.cpu;
	jb("descriptor_dump"); jstr("tag", tag);
	jx("fbd_gpu_va", f->desc.gpu_va);
	jk("words"); putchar('[');
	for (unsigned i = 0; i < KB_FB_DESC_SIZE / 4; i++) printf("%s\"%08x\"", i ? "," : "", d[i]);
	putchar(']');
	jend();
}

static void drain_events(struct fx *f)
{
	struct kb_event ev;
	int g = 0;
	const struct kb_event *lf = kb_last_fatal(f->ctx);
	if (lf) { jb("last_fatal"); jok(0); jstr("text", lf->text); jx("status", lf->status); jstr("exception", kb_exception_name(lf->status)); jx("sideband", lf->sideband); jend(); }
	while (g++ < 10 && kb_event_read(f->ctx, &ev, 300) > 0) {
		jb("notification"); jok(0); jstr("text", ev.text); ji("type", ev.type); ji("group", ev.group);
		ji("error_type", ev.error_type); jx("status", ev.status); jstr("exception", kb_exception_name(ev.status));
		jx("sideband", ev.sideband); jbool("fatal", ev.fatal); jend();
	}
}

/* One clear. attach_poll: import the KCPU fence as write fence of the dma-buf and test poll() ordering. */
struct cres { int rc; double us; bool fence_ok; bool poll_before_ready, poll_after_ready; double poll_us; };

static struct cres do_clear(struct fx *f, struct rt *rt, const struct clr *c, bool attach_poll)
{
	struct cres s;
	memset(&s, 0, sizeof s);
	struct kb_fb_params p;
	memset(&p, 0, sizeof p);
	p.width = rt->w; p.height = rt->h; p.rt_gpu_va = rt->bo.gpu_va; p.row_stride = rt->stride;
	p.clear_r = c->r; p.clear_g = c->g; p.clear_b = c->b; p.clear_a = c->a;
	p.no_clean_tile_write = f->o.noclean; p.block_format = f->o.tiled;
	p.arch = kb_props(f->ctx)->arch;
	s.rc = kb_fb_pack(f->desc.cpu, &p);
	if (s.rc) return s;
	kb_wmb();

	uint64_t target = f->target + 1;
	int fence = -1;
	if ((s.rc = kb_kcpu_cqs_wait_gt64(f->ctx, f->sync.gpu_va, target - 1)) || (s.rc = kb_kcpu_fence_signal(f->ctx, &fence)))
		return s;
	if (attach_poll) {
		s.rc = kb_dmabuf_import_sync_file(rt->fd, KB_DMA_BUF_SYNC_WRITE, fence);
		if (s.rc) { close(fence); return s; }
		struct pollfd pf = { rt->fd, POLLIN, 0 };
		s.poll_before_ready = poll(&pf, 1, 0) > 0;
	}

	struct kb_cs_buf b;
	b.n = 0;
	kb_cs_emit(&b, kb_cs_nop());
	kb_cs_emit(&b, kb_cs_move48(40, kb_fb_pointer(f->desc.gpu_va, 1)));
	kb_cs_emit(&b, kb_cs_move32(42, kb_fb_bbox_min(0, 0)));
	kb_cs_emit(&b, kb_cs_move32(43, kb_fb_bbox_max(rt->w - 1, rt->h - 1)));
	if (!f->o.noflush) {
		kb_cs_emit(&b, kb_cs_move32(72, 0));
		kb_cs_emit_flush_wait(&b, 3, 3, 2, 72); /* drop stale descriptor lines before the GPU reads them; waited, the flush is async */
	}
	kb_cs_emit(&b, kb_cs_req_res(KB_CS_RES_FRAGMENT));
	kb_cs_emit(&b, kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, f->o.slot));
	kb_cs_emit(&b, kb_cs_run_fragment(0));
	if (f->o.finish)
		kb_cs_emit(&b, kb_cs_finish_fragment(1u << f->o.slot, 0));
	if (!f->o.nowait)
		kb_cs_emit(&b, kb_cs_wait(1u << f->o.slot));
	kb_cs_emit(&b, kb_cs_req_res(0));
	kb_cs_emit(&b, kb_cs_move48(68, f->sync.gpu_va));
	kb_cs_emit(&b, kb_cs_move48(70, 1));
	kb_cs_emit(&b, kb_cs_sync_add64(68, 70, KB_CS_SCOPE_SYSTEM));

	uint64_t t0 = now_ns();
	s.rc = kb_queue_submit(&f->q, b.ins, b.n, KB_KICK_IOCTL);
	if (s.rc) { close(fence); return s; }
	if (attach_poll) {
		struct pollfd pf = { rt->fd, POLLIN, 0 };
		int pr = poll(&pf, 1, 3000);
		s.poll_us = (double)(now_ns() - t0) / 1000.0;
		s.poll_after_ready = pr > 0;
	}
	s.rc = kb_fence_wait(f->ctx, fence, 3000);
	s.us = (double)(now_ns() - t0) / 1000.0;
	close(fence);
	s.fence_ok = s.rc == 0;
	if (s.fence_ok) f->target = target;
	return s;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static void report_clear(const char *name, struct fx *f, struct rt *rt, const struct clr *c, const struct cres *s, const struct vres *v)
{
	bool ok = s->rc == 0 && s->fence_ok && v && v->bad == 0;
	jb(name); jok(ok);
	jstr("colour", c->name); ji("width", rt->w); ji("height", rt->h); ji("row_stride_bytes", rt->stride);
	jstr("format", f->o.tiled ? "R8G8B8A8 tiled-u-interleaved" : "R8G8B8A8 linear");
	ji("fence_rc", s->rc); jbool("fence_signalled", s->fence_ok); jd("submit_to_signal_us", s->us);
	if (v) {
		jx("expected_pixel", v->exp); ji("pixels", (long long)v->total); ji("mismatches", (long long)v->bad);
		ji("untouched_prefill", (long long)v->untouched);
		if (v->bad) {
			ji("first_bad_index", (long long)v->first_bad); jx("first_bad_value", v->first_val);
			jk("seen_values"); putchar('[');
			for (unsigned i = 0; i < v->nseen; i++) printf("%s\"0x%08x\"", i ? "," : "", v->seen[i]);
			putchar(']');
		}
	}
	if (!ok && s->rc) jstr("error", kb_last_error());
	jend();
}

/* returns true on pass; on a hardware failure dumps descriptors + notifications */
static bool step(struct fx *f, const char *name, unsigned w, unsigned h, const struct clr *c)
{
	struct rt rt;
	int r = rt_make(f, &rt, w, h);
	if (r) { jb(name); jok(0); jstr("error", kb_last_error()); ji("rc", r); jend(); return false; }
	rt_prefill(&rt);
	struct cres s = do_clear(f, &rt, c, false);
	struct vres v;
	bool have_v = false;
	if (s.fence_ok) { v = rt_verify(&rt, c); have_v = true; }
	report_clear(name, f, &rt, c, &s, have_v ? &v : NULL);
	bool ok = s.fence_ok && have_v && v.bad == 0;
	if (!ok) {
		dump_desc(f, name);
		drain_events(f);
		jb("fault_state"); jok(0); jbool("queue_faulted", kb_queue_faulted(&f->q));
		jx("cs_extract", kb_queue_extract(&f->q)); jx("cs_insert", f->q.insert); jend();
	}
	rt_free(&rt);
	return ok;
}

int main(int argc, char **argv)
{
	struct fx f;
	memset(&f, 0, sizeof f);
	f.o.slot = 3; f.o.n = 200;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--slot") && i + 1 < argc) f.o.slot = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--finish")) f.o.finish = true;
		else if (!strcmp(argv[i], "--noclean")) f.o.noclean = true;
		else if (!strcmp(argv[i], "--tiled")) f.o.tiled = true;
		else if (!strcmp(argv[i], "--nowait")) f.o.nowait = true;
		else if (!strcmp(argv[i], "--noflush")) f.o.noflush = true;
		else if (!strcmp(argv[i], "--n") && i + 1 < argc) f.o.n = atoi(argv[++i]);
	}
	int failures = 0;
	int r = kb_open(&f.ctx, NULL);
	jb("open"); jok(r == 0); ji("rc", r);
	jbool("opt_finish", f.o.finish); jbool("opt_noclean", f.o.noclean); jbool("opt_tiled", f.o.tiled); ji("opt_slot", f.o.slot);
	if (r) { jstr("error", kb_last_error()); jend(); return 1; }
	jend();
	r = kb_bo_alloc(f.ctx, 4096, KB_BO_GPU_RD | KB_BO_GPU_WR, &f.desc);
	int r2 = kb_bo_alloc(f.ctx, 4096, KB_BO_CSF_EVENT | KB_BO_GPU_RD | KB_BO_GPU_WR, &f.sync);
	if (r || r2) { jb("setup"); jok(0); jstr("error", kb_last_error()); jend(); failures++; goto teardown; }
	memset(f.sync.cpu, 0, 4096);
	r = kb_queue_create(f.ctx, NULL, &f.q);
	jb("queue_create"); jok(r == 0); ji("rc", r); jend();
	if (r) { failures++; goto teardown; }

	if (!step(&f, "clear_256_red", 256, 256, &k_red)) { failures++; goto teardown; }
	if (!step(&f, "clear_256_grey", 256, 256, &k_grey)) { failures++; goto teardown; }
	if (!step(&f, "clear_256_asym", 256, 256, &k_asym)) { failures++; goto teardown; }
	if (!step(&f, "clear_1280x720_asym", 1280, 720, &k_asym)) { failures++; goto teardown; }
	if (!step(&f, "clear_1280x720_red", 1280, 720, &k_red)) { failures++; goto teardown; }

	/* loop */
	{
		struct rt rt;
		const struct clr *cs[3] = { &k_red, &k_grey, &k_asym };
		double *v = calloc((size_t)f.o.n, sizeof *v);
		int done = 0, bad = 0;
		if (rt_make(&f, &rt, 256, 256) || !v) { failures++; goto teardown; }
		for (int i = 0; i < f.o.n && !bad; i++) {
			rt_prefill(&rt);
			struct cres s = do_clear(&f, &rt, cs[i % 3], false);
			if (!s.fence_ok) { bad = 1; dump_desc(&f, "loop"); drain_events(&f); break; }
			struct vres vr = rt_verify(&rt, cs[i % 3]);
			if (vr.bad) { bad = 1; report_clear("clear_loop_mismatch", &f, &rt, cs[i % 3], &s, &vr); break; }
			v[done++] = s.us;
		}
		double sum = 0;
		for (int i = 0; i < done; i++) sum += v[i];
		qsort(v, (size_t)done, sizeof *v, cmp_d);
		jb("clear_loop"); jok(!bad && done == f.o.n); ji("clears", f.o.n); ji("completed_correct", done);
		jd("mean_us", done ? sum / done : 0); jd("min_us", done ? v[0] : 0); jd("p50_us", done ? v[done / 2] : 0);
		jd("p99_us", done ? v[done * 99 / 100] : 0); jd("max_us", done ? v[done - 1] : 0);
		jend();
		free(v);
		rt_free(&rt);
		if (bad || done != f.o.n) { failures++; goto teardown; }
	}

	/* explicit sync via dma-buf write fence */
	{
		struct rt rt;
		if (rt_make(&f, &rt, 256, 256)) { failures++; goto teardown; }
		rt_prefill(&rt);
		struct cres s = do_clear(&f, &rt, &k_asym, true);
		struct vres vr;
		memset(&vr, 0, sizeof vr);
		if (s.fence_ok) vr = rt_verify(&rt, &k_asym);
		bool ok = s.rc == 0 && s.fence_ok && !s.poll_before_ready && s.poll_after_ready && vr.bad == 0 && vr.total;
		jb("clear_poll"); jok(ok); ji("rc", s.rc);
		jbool("poll_ready_before_kick", s.poll_before_ready); jbool("poll_ready_after_clear", s.poll_after_ready);
		jd("kick_to_poll_ready_us", s.poll_us); jd("kick_to_fence_us", s.us);
		ji("mismatches", (long long)vr.bad);
		if (s.rc) jstr("error", kb_last_error());
		jend();
		rt_free(&rt);
		if (!ok) { failures++; drain_events(&f); }
	}

teardown:
	if (f.q.ctx) kb_queue_destroy(&f.q);
	if (f.desc.ctx) kb_bo_free(&f.desc);
	if (f.sync.ctx) kb_bo_free(&f.sync);
	{
		struct kb_leaks lk;
		kb_leaks(f.ctx, &lk);
		jb("teardown"); jok(lk.bos == 0 && lk.queues == 0); ji("leaked_bos", lk.bos); ji("leaked_queues", lk.queues); jend();
		kb_close(f.ctx);
	}
	jb("summary"); jok(failures == 0); ji("failures", failures); jend();
	return failures ? 1 : 0;
}
