// SPDX-License-Identifier: MIT
/*
 * t_present_tri: ladder R2 step 3. Animated triangle (Mesa-compiled shaders, compiled.h) rendered by our libkbase driver straight into
 * linear dma-heap buffers, presented through Wayland (xdg_toplevel + zwp_linux_dmabuf create_immed, XB24/AB24 linear). Runs in the guest.
 * Per frame: 0 CPU copies. GPU job -> KCPU fence signal -> DMA_BUF_IOCTL_IMPORT_SYNC_FILE(write) on the dma-buf -> wl_surface.commit.
 * Pacing: wl_surface.frame callbacks. Buffer 256x256 (the engine's fixed size); the compositor scales it.
 * Options: --seconds S (default 20) --frames N --buffers N (default 4) --appid STR --verify (CPU-verify frame 0 and every 60th, waits on fence)
 *          --format xb24|ab24 --windowed (no fullscreen request)
 * JSON lines on stdout (rung2.h helpers: {"test":...}).
 */
#include <poll.h>
#include <signal.h>
#include <wayland-client.h>

#include "rung2.h"
#include "compiled.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FMT_XB24 FOURCC('X', 'B', '2', '4')
#define FMT_AB24 FOURCC('A', 'B', '2', '4')
#define NB 8

struct pbuf { int fd; struct kb_bo bo; struct wl_buffer *wlb; int busy; uint64_t commit_ns; };

static struct {
	struct wl_display *dpy; struct wl_registry *reg; struct wl_compositor *comp; struct xdg_wm_base *wm;
	struct zwp_linux_dmabuf_v1 *dmabuf; struct wp_presentation *pres; struct wl_surface *surf; struct xdg_surface *xs; struct xdg_toplevel *top;
	struct wl_callback *cb;
	int configured, closed, cb_ready, have_xb, have_ab;
	uint32_t fmt; char globals[1024];
	struct pbuf b[NB]; int nbuf, next;
	long frame, presented, discarded, released, no_free, errors;
	double rel_sum, rel_max; uint64_t t_first, t_last;
	struct fx f;
} S;
static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }
static void perr(const char *what, const char *d) { S.errors++; jb("error"); jstr("what", what); jstr("detail", d ? d : ""); jend(); }

static void buf_rel(void *d, struct wl_buffer *wb)
{
	(void)d;
	for (int i = 0; i < S.nbuf; i++)
		if (S.b[i].wlb == wb && S.b[i].busy) {
			double ms = (double)(now_ns() - S.b[i].commit_ns) / 1e6;
			S.rel_sum += ms; if (ms > S.rel_max) S.rel_max = ms;
			S.released++; S.b[i].busy = 0;
		}
}
static const struct wl_buffer_listener buf_l = { buf_rel };
static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };
static void xs_cfg(void *d, struct xdg_surface *xs, uint32_t serial) { (void)d; xdg_surface_ack_configure(xs, serial); S.configured = 1; }
static const struct xdg_surface_listener xs_l = { xs_cfg };
static void top_cfg(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; S.closed = 1; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_cfg, top_close, top_bounds, top_caps };
static void dm_fmt(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f) { (void)d; (void)z; if (f == FMT_XB24) S.have_xb = 1; if (f == FMT_AB24) S.have_ab = 1; }
static void dm_mod(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f, uint32_t hi, uint32_t lo) { (void)d; (void)z; (void)f; (void)hi; (void)lo; }
static const struct zwp_linux_dmabuf_v1_listener dm_l = { dm_fmt, dm_mod };
static void pr_clock(void *d, struct wp_presentation *p, uint32_t id) { (void)d; (void)p; (void)id; }
static const struct wp_presentation_listener pr_l = { pr_clock };
static void fb_out(void *d, struct wp_presentation_feedback *f, struct wl_output *o) { (void)d; (void)f; (void)o; }
static void fb_pres(void *d, struct wp_presentation_feedback *f, uint32_t sh, uint32_t sl, uint32_t ns, uint32_t refresh, uint32_t qh, uint32_t ql, uint32_t fl)
{
	(void)d; (void)refresh; (void)qh; (void)ql; (void)fl;
	uint64_t t = ((uint64_t)sh << 32 | sl) * 1000000000ull + ns;
	if (!S.t_first) S.t_first = t;
	S.t_last = t; S.presented++;
	wp_presentation_feedback_destroy(f);
}
static void fb_disc(void *d, struct wp_presentation_feedback *f) { (void)d; S.discarded++; wp_presentation_feedback_destroy(f); }
static const struct wp_presentation_feedback_listener fb_l = { fb_out, fb_pres, fb_disc };
static void cb_done(void *d, struct wl_callback *cb, uint32_t ms) { (void)d; (void)ms; wl_callback_destroy(cb); S.cb = NULL; S.cb_ready = 1; }
static const struct wl_callback_listener cb_l = { cb_done };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	(void)d;
	size_t l = strlen(S.globals);
	snprintf(S.globals + l, sizeof S.globals - l, "%s%s/%u", l ? " " : "", iface, ver);
	if (!strcmp(iface, "wl_compositor")) S.comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
	else if (!strcmp(iface, "xdg_wm_base")) { S.wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 2 ? ver : 2); xdg_wm_base_add_listener(S.wm, &wm_l, NULL); }
	else if (!strcmp(iface, "zwp_linux_dmabuf_v1") && ver >= 3) { S.dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, 3); zwp_linux_dmabuf_v1_add_listener(S.dmabuf, &dm_l, NULL); }
	else if (!strcmp(iface, "wp_presentation")) { S.pres = wl_registry_bind(r, name, &wp_presentation_interface, 1); wp_presentation_add_listener(S.pres, &pr_l, NULL); }
}
static void reg_rm(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_rm };

static int draw(int verify)
{
	int idx = -1;
	for (int k = 0; k < S.nbuf && idx < 0; k++) { int j = (S.next + k) % S.nbuf; if (!S.b[j].busy) idx = j; }
	while (idx < 0 && !g_stop && !S.closed) {
		S.no_free++;
		if (wl_display_dispatch(S.dpy) < 0) { g_stop = 1; return -1; }
		for (int k = 0; k < S.nbuf && idx < 0; k++) { int j = (S.next + k) % S.nbuf; if (!S.b[j].busy) idx = j; }
	}
	if (idx < 0) return -1;
	S.next = (idx + 1) % S.nbuf;
	struct pbuf *b = &S.b[idx];
	int fence = -1; uint64_t t0;
	struct cmp_xf xf = cmp_xf_frame((unsigned)S.frame, 240);
	int rc = cmp_render(&S.f, CMP_ROT, xf, b->bo.gpu_va, &fence, &t0);
	if (rc) { perr("cmp_render", kb_last_error()); g_stop = 1; return -1; }
	if (verify) {
		if (kb_fence_wait(S.f.ctx, fence, 3000)) { perr("fence_wait", kb_last_error()); g_stop = 1; return -1; }
		uint32_t *img = cmp_read(&b->bo);
		struct cmp_res r;
		cmp_verify(img, CMP_ROT, xf, &r);
		free(img);
		jb("verify"); jok(cmp_ok(&r)); ji("frame", S.frame); ji("in_bad", r.in_bad); ji("out_bad", r.out_bad); jd("area_err_pct", r.area_err_pct); jend();
		if (!cmp_ok(&r)) S.errors++;
	} else {
		/* attach the completion fence to the dma-buf: the consumer waits for the GPU (implicit sync), no CPU wait */
		rc = kb_dmabuf_import_sync_file(b->fd, KB_DMA_BUF_SYNC_WRITE, fence);
		if (rc) { perr("import_sync_file", kb_last_error()); g_stop = 1; close(fence); return -1; }
	}
	close(fence);
	S.cb = wl_surface_frame(S.surf);
	wl_callback_add_listener(S.cb, &cb_l, NULL);
	if (S.pres) { struct wp_presentation_feedback *fbk = wp_presentation_feedback(S.pres, S.surf); wp_presentation_feedback_add_listener(fbk, &fb_l, NULL); }
	wl_surface_attach(S.surf, b->wlb, 0, 0);
	wl_surface_damage_buffer(S.surf, 0, 0, W, H);
	wl_surface_commit(S.surf);
	wl_display_flush(S.dpy);
	b->busy = 1; b->commit_ns = now_ns();
	S.frame++; S.cb_ready = 0;
	return 0;
}

int main(int argc, char **argv)
{
	double seconds = 20; long frames = 0; int verify = 0, windowed = 0; const char *appid = "t_present_tri"; uint32_t pref = 0;
	S.nbuf = 4;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
		else if (!strcmp(argv[i], "--buffers") && i + 1 < argc) S.nbuf = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--appid") && i + 1 < argc) appid = argv[++i];
		else if (!strcmp(argv[i], "--verify")) verify = 1;
		else if (!strcmp(argv[i], "--windowed")) windowed = 1;
		else if (!strcmp(argv[i], "--format") && i + 1 < argc) { i++; pref = !strcmp(argv[i], "ab24") ? FMT_AB24 : FMT_XB24; }
		else { fprintf(stderr, "usage: t_present_tri [--seconds S] [--frames N] [--buffers N] [--appid S] [--verify] [--windowed] [--format xb24|ab24]\n"); return 2; }
	}
	if (S.nbuf < 2 || S.nbuf > NB) S.nbuf = 4;
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

	S.dpy = wl_display_connect(NULL);
	if (!S.dpy) { jb("setup"); jok(0); jstr("error", "wl_display_connect failed"); jend(); return 1; }
	S.reg = wl_display_get_registry(S.dpy);
	wl_registry_add_listener(S.reg, &reg_l, NULL);
	wl_display_roundtrip(S.dpy); wl_display_roundtrip(S.dpy);
	jb("globals"); jstr("list", S.globals); jend();
	if (!S.comp || !S.wm || !S.dmabuf) { jb("setup"); jok(0); jstr("error", "missing wl_compositor/xdg_wm_base/zwp_linux_dmabuf_v1"); jend(); return 1; }
	S.fmt = pref ? pref : (S.have_xb ? FMT_XB24 : FMT_AB24);

	S.surf = wl_compositor_create_surface(S.comp);
	S.xs = xdg_wm_base_get_xdg_surface(S.wm, S.surf);
	xdg_surface_add_listener(S.xs, &xs_l, NULL);
	S.top = xdg_surface_get_toplevel(S.xs);
	xdg_toplevel_add_listener(S.top, &top_l, NULL);
	xdg_toplevel_set_title(S.top, "t_present_tri");
	xdg_toplevel_set_app_id(S.top, appid);
	if (!windowed) xdg_toplevel_set_fullscreen(S.top, NULL);
	wl_surface_commit(S.surf);
	for (int i = 0; i < 20 && !S.configured; i++) wl_display_roundtrip(S.dpy);

	cmp_opts_default(&S.f);
	int r = kb_open(&S.f.ctx, NULL);
	if (r) { jb("setup"); jok(0); jstr("error", kb_last_error()); jend(); return 1; }
	r = cmp_setup(&S.f);
	if (r) { jb("setup"); jok(0); ji("rc", r); jstr("error", kb_last_error()); jend(); return 1; }
	size_t sz = (size_t)W * H * 4;
	for (int i = 0; i < S.nbuf; i++) {
		struct pbuf *b = &S.b[i];
		if (alloc_target(&S.f, sz, 0x5a3c1e87u, &b->bo, &b->fd)) { perr("alloc_target", kb_last_error()); return 1; }
		struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(S.dmabuf);
		zwp_linux_buffer_params_v1_add(p, b->fd, 0, 0, W * 4, 0, 0);
		b->wlb = zwp_linux_buffer_params_v1_create_immed(p, W, H, S.fmt, 0);
		zwp_linux_buffer_params_v1_destroy(p);
		wl_buffer_add_listener(b->wlb, &buf_l, NULL);
	}
	wl_display_flush(S.dpy);
	jb("setup"); jok(1); jstr("format", S.fmt == FMT_XB24 ? "XB24" : "AB24"); jbool("configured", S.configured); jbool("presentation_time", S.pres != NULL);
	jstr("appid", appid); ji("buffers", S.nbuf); ji("w", W); ji("h", H); jend();

	uint64_t t_loop = now_ns();
	if (draw(verify)) goto out;
	while (!g_stop && !S.closed) {
		if (frames && S.frame >= frames) break;
		if (!frames && (double)(now_ns() - t_loop) / 1e9 >= seconds) break;
		while (wl_display_prepare_read(S.dpy) != 0) wl_display_dispatch_pending(S.dpy);
		wl_display_flush(S.dpy);
		struct pollfd pf = { wl_display_get_fd(S.dpy), POLLIN, 0 };
		int pr = poll(&pf, 1, 500);
		if (pr > 0) wl_display_read_events(S.dpy); else wl_display_cancel_read(S.dpy);
		if (wl_display_dispatch_pending(S.dpy) < 0) { perr("dispatch", strerror(errno)); break; }
		if (pr == 0 && !S.cb_ready) { static int stall; if (++stall <= 5) { jb("stall"); ji("frame", S.frame); jend(); } }
		if (S.cb_ready) draw(verify && S.frame % 60 == 0);
		if (kb_queue_faulted(&S.f.q)) { perr("queue_faulted", "CSF group fault"); break; }
	}
	{
		double dur = (double)(now_ns() - t_loop) / 1e9;
		wl_display_roundtrip(S.dpy);
		double pd = S.presented > 1 ? (double)(S.t_last - S.t_first) / 1e9 : 0;
		jb("summary"); jok(S.errors == 0); jd("duration_s", dur); ji("frames_committed", S.frame); ji("presented", S.presented); ji("discarded", S.discarded);
		ji("released", S.released); jd("commit_fps", (double)S.frame / dur); jd("present_fps", pd > 0 ? (double)(S.presented - 1) / pd : 0);
		jd("commit_to_release_mean_ms", S.released ? S.rel_sum / (double)S.released : 0); jd("commit_to_release_max_ms", S.rel_max);
		ji("no_free_buffer_events", S.no_free); ji("errors", S.errors);
		ji("cpu_copies_per_frame", 0); jend();
	}
out:
	for (int i = 0; i < S.nbuf; i++) if (S.b[i].wlb) wl_buffer_destroy(S.b[i].wlb);
	wl_display_roundtrip(S.dpy);
	for (int i = 0; i < S.nbuf; i++) { if (S.b[i].bo.ctx) kb_bo_free(&S.b[i].bo); if (S.b[i].fd > 0) close(S.b[i].fd); }
	cmp_teardown(&S.f);
	kb_close(S.f.ctx);
	wl_display_disconnect(S.dpy);
	return S.errors ? 1 : 0;
}
