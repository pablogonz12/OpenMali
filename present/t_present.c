// SPDX-License-Identifier: MIT
/*
 * t_present: presentation-path test for our own Mali driver stack (rung R2, no shaders).
 * Runs inside the DroidDeck guest. Creates a fullscreen xdg_toplevel on the session's Wayland display
 * (gamescope --expose-wayland), allocates a ring of N linear dma-heap buffers, imports each into libkbase,
 * clears every frame with a GPU fragment job to a changing colour, attaches the completion fence to the
 * dma-buf (DMA_BUF_IOCTL_IMPORT_SYNC_FILE, write fence = implicit/explicit-by-fence sync, no CPU wait) and
 * commits the buffer through zwp_linux_dmabuf_v1 (create_immed, linear). Pacing: wl_surface.frame callbacks,
 * timing: wp_presentation feedback. JSON lines on stdout.
 *
 * Options: --frames N (0 = until --seconds) --seconds S (default 20 when --frames is 0) --buffers N (default 4)
 *          --size WxH (default: the compositor's fullscreen configure, else 1280x720) --no-sync (no fence
 *          attached, no CPU wait: the consumer may read unfinished frames) --cpu-wait (no fence attached, but
 *          the CPU waits for the GPU before commit) --format xb24|ab24
 * Colour of frame n: hue = n * 0.5 degrees (5 top bits per channel), the low 3 bits of R, G, B carry
 * n & 7, (n >> 3) & 7, (n >> 6) & 7. A "colour" JSON line is written for every 30th frame.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "kbase.h"
#include "kbase_fb.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FMT_XB24 FOURCC('X', 'B', '2', '4')
#define FMT_AB24 FOURCC('A', 'B', '2', '4')
#define MAXBUF 16
#define DESC_STRIDE 4096u

typedef unsigned long long ull;

/* ---------------------------------------------------------------- JSON */
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
static void jb(const char *t) { printf("{\"event\":"); js(t); }
static void jk(const char *k) { putchar(','); js(k); putchar(':'); }
static void jstr(const char *k, const char *v) { jk(k); js(v); }
static void ji(const char *k, long long v) { jk(k); printf("%lld", v); }
static void jd(const char *k, double v) { jk(k); printf("%.3f", v); }
static void jbool(const char *k, int v) { jk(k); fputs(v ? "true" : "false", stdout); }
static void jend(void) { fputs("}\n", stdout); fflush(stdout); }

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---------------------------------------------------------------- stats */
struct stat_acc { double sum, min, max; long n; double *v; long cap; };
static void st_add(struct stat_acc *s, double x)
{
	if (!s->n || x < s->min) s->min = x;
	if (!s->n || x > s->max) s->max = x;
	s->sum += x;
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 1024;
		s->v = realloc(s->v, (size_t)s->cap * sizeof *s->v);
	}
	if (s->v) s->v[s->n] = x;
	s->n++;
}
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void st_report(struct stat_acc *s, const char *name)
{
	jk(name); putchar('{');
	if (!s->n || !s->v) { printf("\"n\":0}"); return; }
	qsort(s->v, (size_t)s->n, sizeof *s->v, cmp_d);
	printf("\"n\":%ld,\"mean\":%.3f,\"min\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f}", s->n, s->sum / (double)s->n, s->min,
	       s->v[s->n / 2], s->v[s->n * 95 / 100], s->v[s->n * 99 / 100], s->max);
}

/* intervals: <12.5 -> "8", <20.8 -> "16", <29.2 -> "25", <37.5 -> "33", else "slow"; <6 -> "fast" */
static const char *k_bucket[] = { "fast_lt6", "8ms", "16ms", "25ms", "33ms", "slow_ge37" };
static int bucket_of(double ms)
{
	if (ms < 6.0) return 0;
	if (ms < 12.5) return 1;
	if (ms < 20.8) return 2;
	if (ms < 29.2) return 3;
	if (ms < 37.5) return 4;
	return 5;
}
struct hist { long c[6]; double last_ms; int have; };
static void hist_add(struct hist *h, double t_ms, struct stat_acc *iv)
{
	if (h->have) {
		double d = t_ms - h->last_ms;
		h->c[bucket_of(d)]++;
		st_add(iv, d);
	}
	h->last_ms = t_ms;
	h->have = 1;
}
static void hist_report(const struct hist *h, const char *name)
{
	jk(name); putchar('{');
	for (int i = 0; i < 6; i++) printf("%s\"%s\":%ld", i ? "," : "", k_bucket[i], h->c[i]);
	putchar('}');
}

/* ---------------------------------------------------------------- state */
struct buf {
	int fd;
	struct kb_bo bo;
	struct wl_buffer *wlb;
	unsigned stride;
	int busy;            /* committed, release not yet seen */
	uint64_t commit_ns;
	long frame;
};

struct app {
	/* options */
	long frames; double seconds; int nbuf; unsigned w, h; int no_sync, cpu_wait; uint32_t fmt_pref;
	/* wayland */
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *comp;
	struct xdg_wm_base *wm;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	uint32_t dmabuf_ver;
	struct wp_presentation *pres;
	struct wl_surface *surf;
	struct xdg_surface *xs;
	struct xdg_toplevel *top;
	struct wl_callback *cb;
	int configured, closed, cfg_w, cfg_h, frame_cb_ready, have_xb24, have_ab24, have_linear_xb24, have_linear_ab24, nmods;
	uint32_t fmt;
	char globals[1024];
	/* gpu */
	struct kb_ctx *ctx;
	struct kb_bo desc, sync;
	struct kb_queue q;
	uint64_t target;
	struct buf bufs[MAXBUF];
	int next;
	/* run */
	long frame, presented, discarded, commits, no_free, gpu_errors, release_n;
	uint64_t t_start, t_first_present, t_last_present;
	uint64_t refresh_ns;
	struct hist cbh, preh;       /* frame callback times, presented times */
	struct stat_acc cb_iv, pre_iv, commit_rel, render_cpu, wait_free, present_lat;
	long sec_frames, sec_present; uint64_t sec_t0;
	long errors;
};
static struct app A;
static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void err(const char *what, const char *detail)
{
	A.errors++;
	jb("error"); jstr("what", what); jstr("detail", detail ? detail : ""); jend();
}

/* ---------------------------------------------------------------- colour */
static void hsv(double h, uint8_t *r, uint8_t *g, uint8_t *b)
{
	h = fmod(h, 360.0) / 60.0;
	int i = (int)h;
	double f = h - i, q = 1 - f, v = 1;
	double R = 0, G = 0, B = 0;
	switch (i) { case 0: R = v; G = f; break; case 1: R = q; G = v; break; case 2: G = v; B = f; break;
		case 3: G = q; B = v; break; case 4: R = f; B = v; break; default: R = v; B = q; break; }
	*r = (uint8_t)(R * 255); *g = (uint8_t)(G * 255); *b = (uint8_t)(B * 255);
}
static void colour_of(long n, uint8_t *r, uint8_t *g, uint8_t *b)
{
	hsv((double)(n % 720) * 0.5, r, g, b);
	*r = (uint8_t)((*r & 0xf8) | (n & 7));
	*g = (uint8_t)((*g & 0xf8) | ((n >> 3) & 7));
	*b = (uint8_t)((*b & 0xf8) | ((n >> 6) & 7));
}

/* ---------------------------------------------------------------- GPU */
/* Submit one clear of buffer i to colour c. Returns fence fd (or -1 on error; *rc set). */
static int gpu_clear(int i, uint8_t r, uint8_t g, uint8_t b, int *rc)
{
	struct buf *bf = &A.bufs[i];
	uint8_t *cpu = (uint8_t *)A.desc.cpu + (size_t)i * DESC_STRIDE;
	uint64_t gva = A.desc.gpu_va + (uint64_t)i * DESC_STRIDE;
	struct kb_fb_params p;
	memset(&p, 0, sizeof p);
	p.width = A.w; p.height = A.h; p.rt_gpu_va = bf->bo.gpu_va; p.row_stride = bf->stride;
	p.clear_r = r; p.clear_g = g; p.clear_b = b; p.clear_a = 0xff;
	*rc = kb_fb_pack(cpu, &p);
	if (*rc) return -1;
	__sync_synchronize();
	uint64_t target = A.target + 1;
	int fence = -1;
	if ((*rc = kb_kcpu_cqs_wait_gt64(A.ctx, A.sync.gpu_va, target - 1)) || (*rc = kb_kcpu_fence_signal(A.ctx, &fence))) return -1;
	if (!A.no_sync && !A.cpu_wait) {
		*rc = kb_dmabuf_import_sync_file(bf->fd, KB_DMA_BUF_SYNC_WRITE, fence);
		if (*rc) { close(fence); return -1; }
	}
	struct kb_cs_buf cb;
	cb.n = 0;
	kb_cs_emit(&cb, kb_cs_nop());
	kb_cs_emit(&cb, kb_cs_move48(40, kb_fb_pointer(gva, 1)));
	kb_cs_emit(&cb, kb_cs_move32(42, kb_fb_bbox_min(0, 0)));
	kb_cs_emit(&cb, kb_cs_move32(43, kb_fb_bbox_max(A.w - 1, A.h - 1)));
	kb_cs_emit(&cb, kb_cs_move32(72, 0));
	kb_cs_emit(&cb, kb_cs_flush_cache2(3, 3, 2, 72));
	kb_cs_emit(&cb, kb_cs_req_res(KB_CS_RES_FRAGMENT));
	kb_cs_emit(&cb, kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, 3));
	kb_cs_emit(&cb, kb_cs_run_fragment(0));
	kb_cs_emit(&cb, kb_cs_wait(1u << 3));
	kb_cs_emit(&cb, kb_cs_req_res(0));
	kb_cs_emit(&cb, kb_cs_move48(68, A.sync.gpu_va));
	kb_cs_emit(&cb, kb_cs_move48(70, 1));
	kb_cs_emit(&cb, kb_cs_sync_add64(68, 70, KB_CS_SCOPE_SYSTEM));
	*rc = kb_queue_submit(&A.q, cb.ins, cb.n, KB_KICK_IOCTL);
	if (*rc) { close(fence); return -1; }
	A.target = target;
	return fence;
}

/* ---------------------------------------------------------------- wayland listeners */
static void buf_release(void *d, struct wl_buffer *wb)
{
	(void)d;
	for (int i = 0; i < A.nbuf; i++)
		if (A.bufs[i].wlb == wb) {
			struct buf *b = &A.bufs[i];
			if (b->busy) {
				st_add(&A.commit_rel, (double)(now_ns() - b->commit_ns) / 1e6);
				A.release_n++;
			}
			b->busy = 0;
			return;
		}
}
static const struct wl_buffer_listener buf_l = { buf_release };

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
	A.configured = 1;
}
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st)
{
	(void)d; (void)t; (void)st;
	if (w > 0 && h > 0) { A.cfg_w = w; A.cfg_h = h; }
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; A.closed = 1; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

static void dmabuf_format(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f)
{
	(void)d; (void)z;
	if (f == FMT_XB24) A.have_xb24 = 1;
	if (f == FMT_AB24) A.have_ab24 = 1;
}
static void dmabuf_modifier(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f, uint32_t hi, uint32_t lo)
{
	(void)d; (void)z;
	A.nmods++;
	if (hi == 0 && lo == 0) {
		if (f == FMT_XB24) A.have_linear_xb24 = 1;
		if (f == FMT_AB24) A.have_linear_ab24 = 1;
	}
}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_l = { dmabuf_format, dmabuf_modifier };

static void pres_clock(void *d, struct wp_presentation *p, uint32_t id) { (void)d; (void)p; (void)id; }
static const struct wp_presentation_listener pres_l = { pres_clock };

static void fb_sync_output(void *d, struct wp_presentation_feedback *f, struct wl_output *o) { (void)d; (void)f; (void)o; }
static void fb_presented(void *d, struct wp_presentation_feedback *f, uint32_t sh, uint32_t sl, uint32_t ns, uint32_t refresh,
                         uint32_t qh, uint32_t ql, uint32_t flags)
{
	(void)qh; (void)ql; (void)flags;
	long fno = (long)(intptr_t)d;
	uint64_t t = ((uint64_t)sh << 32 | sl) * 1000000000ull + ns;
	if (refresh) A.refresh_ns = refresh;
	A.presented++; A.sec_present++;
	if (!A.t_first_present) A.t_first_present = t;
	A.t_last_present = t;
	hist_add(&A.preh, (double)t / 1e6, &A.pre_iv);
	(void)fno;
	wp_presentation_feedback_destroy(f);
}
static void fb_discarded(void *d, struct wp_presentation_feedback *f)
{
	(void)d;
	A.discarded++;
	wp_presentation_feedback_destroy(f);
}
static const struct wp_presentation_feedback_listener fb_l = { fb_sync_output, fb_presented, fb_discarded };

static void cb_done(void *d, struct wl_callback *cb, uint32_t ms)
{
	(void)d;
	wl_callback_destroy(cb);
	A.cb = NULL;
	A.frame_cb_ready = 1;
	hist_add(&A.cbh, (double)ms, &A.cb_iv);
}
static const struct wl_callback_listener cb_l = { cb_done };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	(void)d;
	size_t l = strlen(A.globals);
	snprintf(A.globals + l, sizeof A.globals - l, "%s%s/%u", l ? " " : "", iface, ver);
	if (!strcmp(iface, "wl_compositor")) A.comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
	else if (!strcmp(iface, "xdg_wm_base")) { A.wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 2 ? ver : 2); xdg_wm_base_add_listener(A.wm, &wm_l, NULL); }
	else if (!strcmp(iface, "zwp_linux_dmabuf_v1") && ver >= 3) {
		A.dmabuf_ver = ver < 3 ? ver : 3;
		A.dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, A.dmabuf_ver);
		zwp_linux_dmabuf_v1_add_listener(A.dmabuf, &dmabuf_l, NULL);
	} else if (!strcmp(iface, "wp_presentation")) { A.pres = wl_registry_bind(r, name, &wp_presentation_interface, 1); wp_presentation_add_listener(A.pres, &pres_l, NULL); }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

/* ---------------------------------------------------------------- buffers */
static int make_buffers(void)
{
	for (int i = 0; i < A.nbuf; i++) {
		struct buf *b = &A.bufs[i];
		b->stride = (A.w * 4 + 63u) & ~63u;
		size_t sz = ((size_t)b->stride * A.h + 4095) & ~(size_t)4095;
		b->fd = kb_dmaheap_alloc(KB_HEAP_SYSTEM, sz);
		if (b->fd < 0) { err("dmaheap_alloc", strerror(-b->fd)); return -1; }
		int r = kb_bo_import_dmabuf(A.ctx, b->fd, sz, KB_BO_GPU_RD | KB_BO_GPU_WR, &b->bo);
		if (r) { err("bo_import_dmabuf", kb_last_error()); return -1; }
		struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(A.dmabuf);
		zwp_linux_buffer_params_v1_add(p, b->fd, 0, 0, b->stride, 0, 0);
		b->wlb = zwp_linux_buffer_params_v1_create_immed(p, (int32_t)A.w, (int32_t)A.h, A.fmt, 0);
		zwp_linux_buffer_params_v1_destroy(p);
		wl_buffer_add_listener(b->wlb, &buf_l, NULL);
	}
	wl_display_flush(A.dpy);
	return 0;
}

/* One-time CPU-waited check that the GPU clear reaches the pixels of buffer 0 (all pixels, all 4 bytes). */
static int selfcheck(void)
{
	uint8_t r = 0x12, g = 0xa4, b = 0x56;
	int rc;
	struct buf *bf = &A.bufs[0];
	int save = A.no_sync;
	A.no_sync = 1; /* no attach: we wait on the fence on the CPU */
	int fence = gpu_clear(0, r, g, b, &rc);
	A.no_sync = save;
	if (fence < 0) { err("selfcheck_submit", kb_last_error()); return -1; }
	int wr = kb_fence_wait(A.ctx, fence, 3000);
	close(fence);
	if (wr) { err("selfcheck_fence", kb_last_error()); return -1; }
	uint32_t exp = (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | 0xff000000u;
	kb_bo_dmabuf_sync(&bf->bo, false, false);
	const volatile uint32_t *px = bf->bo.cpu;
	long bad = 0;
	for (unsigned y = 0; y < A.h; y++)
		for (unsigned x = 0; x < A.w; x++)
			if (px[(size_t)y * bf->stride / 4 + x] != exp) bad++;
	kb_bo_dmabuf_sync(&bf->bo, true, false);
	jb("selfcheck"); jbool("ok", bad == 0); ji("mismatches", bad); ji("pixels", (long long)A.w * A.h); jend();
	return bad ? -1 : 0;
}

static void draw_frame(void)
{
	uint64_t t0 = now_ns();
	int idx = -1;
	/* acquire: next free buffer, round robin; wait for a release if none */
	for (int k = 0; k < A.nbuf; k++) {
		int j = (A.next + k) % A.nbuf;
		if (!A.bufs[j].busy) { idx = j; break; }
	}
	if (idx < 0) {
		A.no_free++;
		uint64_t w0 = now_ns();
		while (idx < 0 && !g_stop && !A.closed) {
			if (wl_display_dispatch(A.dpy) < 0) { err("dispatch", strerror(errno)); g_stop = 1; return; }
			for (int k = 0; k < A.nbuf; k++) {
				int j = (A.next + k) % A.nbuf;
				if (!A.bufs[j].busy) { idx = j; break; }
			}
		}
		if (idx < 0) return;
		st_add(&A.wait_free, (double)(now_ns() - w0) / 1e6);
	} else
		st_add(&A.wait_free, 0);
	A.next = (idx + 1) % A.nbuf;
	struct buf *b = &A.bufs[idx];
	uint8_t r, g, bl;
	colour_of(A.frame, &r, &g, &bl);
	int rc;
	int fence = gpu_clear(idx, r, g, bl, &rc);
	if (fence < 0) { A.gpu_errors++; err("gpu_clear", kb_last_error()); g_stop = 1; return; }
	if (A.cpu_wait) {
		if (kb_fence_wait(A.ctx, fence, 3000)) { err("cpu_wait_fence", kb_last_error()); g_stop = 1; }
	}
	close(fence); /* the dma-buf holds its own reference when attached */
	/* commit */
	A.cb = wl_surface_frame(A.surf);
	wl_callback_add_listener(A.cb, &cb_l, NULL);
	if (A.pres) {
		struct wp_presentation_feedback *f = wp_presentation_feedback(A.pres, A.surf);
		wp_presentation_feedback_add_listener(f, &fb_l, (void *)(intptr_t)A.frame);
	}
	wl_surface_attach(A.surf, b->wlb, 0, 0);
	wl_surface_damage_buffer(A.surf, 0, 0, (int32_t)A.w, (int32_t)A.h);
	wl_surface_commit(A.surf);
	wl_display_flush(A.dpy);
	b->busy = 1;
	b->commit_ns = now_ns();
	b->frame = A.frame;
	st_add(&A.render_cpu, (double)(b->commit_ns - t0) / 1e6);
	if (A.frame % 30 == 0) {
		jb("colour"); ji("frame", A.frame); ji("r", r); ji("g", g); ji("b", bl);
		jd("t_ms", (double)(b->commit_ns - A.t_start) / 1e6); jend();
	}
	A.frame++; A.commits++; A.sec_frames++;
	A.frame_cb_ready = 0;
}

static void usage(void)
{
	fprintf(stderr, "t_present [--frames N] [--seconds S] [--buffers N] [--size WxH] [--no-sync] [--cpu-wait] [--format xb24|ab24]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	A.nbuf = 4; A.seconds = 0; A.fmt_pref = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) A.frames = atol(argv[++i]);
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) A.seconds = atof(argv[++i]);
		else if (!strcmp(argv[i], "--buffers") && i + 1 < argc) A.nbuf = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--size") && i + 1 < argc) { if (sscanf(argv[++i], "%ux%u", &A.w, &A.h) != 2) usage(); }
		else if (!strcmp(argv[i], "--no-sync")) A.no_sync = 1;
		else if (!strcmp(argv[i], "--cpu-wait")) A.cpu_wait = 1;
		else if (!strcmp(argv[i], "--format") && i + 1 < argc) { i++; A.fmt_pref = !strcmp(argv[i], "ab24") ? FMT_AB24 : FMT_XB24; }
		else usage();
	}
	if (A.nbuf < 2 || A.nbuf > MAXBUF) usage();
	if (!A.frames && A.seconds <= 0) A.seconds = 20;
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
	A.t_start = now_ns();

	A.dpy = wl_display_connect(NULL);
	if (!A.dpy) { jb("setup"); jbool("ok", 0); jstr("error", "wl_display_connect failed (WAYLAND_DISPLAY/XDG_RUNTIME_DIR?)"); jend(); return 1; }
	A.reg = wl_display_get_registry(A.dpy);
	wl_registry_add_listener(A.reg, &reg_l, NULL);
	wl_display_roundtrip(A.dpy);
	wl_display_roundtrip(A.dpy);
	jb("globals"); jstr("list", A.globals); jend();
	if (!A.comp || !A.wm || !A.dmabuf) { jb("setup"); jbool("ok", 0); jstr("error", "missing wl_compositor/xdg_wm_base/zwp_linux_dmabuf_v1"); jend(); return 1; }
	A.fmt = A.fmt_pref ? A.fmt_pref : (A.have_xb24 ? FMT_XB24 : FMT_AB24);
	if (!(A.fmt == FMT_XB24 ? A.have_linear_xb24 || A.have_xb24 : A.have_linear_ab24 || A.have_ab24)) err("format", "format not offered by the compositor");
	jb("dmabuf"); ji("version", A.dmabuf_ver); jbool("xb24", A.have_xb24); jbool("ab24", A.have_ab24);
	jbool("xb24_linear", A.have_linear_xb24); jbool("ab24_linear", A.have_linear_ab24); ji("modifier_events", A.nmods); jend();

	A.surf = wl_compositor_create_surface(A.comp);
	A.xs = xdg_wm_base_get_xdg_surface(A.wm, A.surf);
	xdg_surface_add_listener(A.xs, &xs_l, NULL);
	A.top = xdg_surface_get_toplevel(A.xs);
	xdg_toplevel_add_listener(A.top, &top_l, NULL);
	xdg_toplevel_set_title(A.top, "t_present");
	xdg_toplevel_set_app_id(A.top, "t_present");
	xdg_toplevel_set_fullscreen(A.top, NULL);
	wl_surface_commit(A.surf);
	for (int i = 0; i < 20 && !A.configured; i++) wl_display_roundtrip(A.dpy);
	if (!A.w || !A.h) {
		A.w = A.cfg_w ? (unsigned)A.cfg_w : 1280;
		A.h = A.cfg_h ? (unsigned)A.cfg_h : 720;
	}
	jb("surface"); jbool("configured", A.configured); ji("cfg_w", A.cfg_w); ji("cfg_h", A.cfg_h); ji("w", A.w); ji("h", A.h);
	jstr("format", A.fmt == FMT_XB24 ? "XB24 (R,G,B,X bytes)" : "AB24 (R,G,B,A bytes)"); ji("buffers", A.nbuf);
	jbool("presentation_time", A.pres != NULL); jbool("no_sync", A.no_sync); jbool("cpu_wait", A.cpu_wait); jend();

	int r = kb_open(&A.ctx, NULL);
	if (r) { jb("setup"); jbool("ok", 0); jstr("error", kb_last_error()); jend(); return 1; }
	struct kb_queue_opts qo = { .ring_size = 16384, .group_priority = 0 };
	if (kb_bo_alloc(A.ctx, (size_t)DESC_STRIDE * MAXBUF, KB_BO_GPU_RD | KB_BO_GPU_WR, &A.desc) ||
	    kb_bo_alloc(A.ctx, 4096, KB_BO_CSF_EVENT | KB_BO_GPU_RD | KB_BO_GPU_WR, &A.sync)) { err("bo_alloc", kb_last_error()); return 1; }
	memset(A.sync.cpu, 0, 4096);
	if (kb_queue_create(A.ctx, &qo, &A.q)) { err("queue_create", kb_last_error()); return 1; }
	if (make_buffers()) return 1;
	if (selfcheck()) goto out;
	jb("setup"); jbool("ok", 1); jend();

	A.sec_t0 = now_ns();
	uint64_t t_loop = now_ns();
	draw_frame(); /* first commit starts the callback chain */
	while (!g_stop && !A.closed) {
		if (A.frames && A.frame >= A.frames) break;
		if (A.seconds > 0 && (double)(now_ns() - t_loop) / 1e9 >= A.seconds) break;
		while (wl_display_prepare_read(A.dpy) != 0) wl_display_dispatch_pending(A.dpy);
		wl_display_flush(A.dpy);
		struct pollfd pf = { wl_display_get_fd(A.dpy), POLLIN, 0 };
		int pr = poll(&pf, 1, 500);
		if (pr > 0) wl_display_read_events(A.dpy); else wl_display_cancel_read(A.dpy);
		if (wl_display_dispatch_pending(A.dpy) < 0) { err("dispatch", strerror(errno)); break; }
		if (pr == 0 && !A.frame_cb_ready) {
			static int stall;
			if (++stall <= 5) { jb("stall"); jstr("detail", "no frame callback for 500 ms"); ji("frame", A.frame); jend(); }
		}
		if (A.frame_cb_ready) draw_frame();
		if (kb_queue_faulted(&A.q)) { err("queue_faulted", "CSF group fault"); break; }
		uint64_t n = now_ns();
		if (n - A.sec_t0 >= 1000000000ull) {
			jb("second"); jd("t", (double)(n - t_loop) / 1e9); ji("commits", A.sec_frames); ji("presented", A.sec_present);
			ji("frame", A.frame); jend();
			A.sec_frames = A.sec_present = 0; A.sec_t0 = n;
		}
	}
	{
		uint64_t t_end = now_ns();
		double dur = (double)(t_end - t_loop) / 1e9;
		/* drain remaining feedback briefly */
		wl_display_roundtrip(A.dpy);
		jb("summary");
		jd("duration_s", dur); ji("frames_committed", A.commits); ji("presented", A.presented); ji("discarded", A.discarded);
		jd("commit_fps", (double)A.commits / dur);
		double pdur = A.presented > 1 ? (double)(A.t_last_present - A.t_first_present) / 1e9 : 0;
		jd("present_fps", pdur > 0 ? (double)(A.presented - 1) / pdur : 0);
		jd("refresh_hz_reported", A.refresh_ns ? 1e9 / (double)A.refresh_ns : 0);
		hist_report(&A.preh, "hist_presented_interval");
		hist_report(&A.cbh, "hist_frame_callback_interval");
		st_report(&A.pre_iv, "presented_interval_ms");
		st_report(&A.cb_iv, "callback_interval_ms");
		st_report(&A.commit_rel, "commit_to_release_ms");
		st_report(&A.wait_free, "acquire_wait_ms");
		st_report(&A.render_cpu, "callback_to_commit_cpu_ms");
		ji("no_free_buffer_events", A.no_free); ji("gpu_errors", A.gpu_errors); ji("errors", A.errors);
		jbool("no_sync", A.no_sync); jbool("cpu_wait", A.cpu_wait); ji("buffers", A.nbuf);
		jend();
	}
out:
	for (int i = 0; i < A.nbuf; i++) {
		if (A.bufs[i].wlb) wl_buffer_destroy(A.bufs[i].wlb);
	}
	wl_display_roundtrip(A.dpy);
	for (int i = 0; i < A.nbuf; i++) {
		if (A.bufs[i].bo.ctx) kb_bo_free(&A.bufs[i].bo);
		if (A.bufs[i].fd > 0) close(A.bufs[i].fd);
	}
	kb_queue_destroy(&A.q);
	kb_bo_free(&A.desc); kb_bo_free(&A.sync);
	kb_close(A.ctx);
	wl_display_disconnect(A.dpy);
	return A.errors ? 1 : 0;
}
