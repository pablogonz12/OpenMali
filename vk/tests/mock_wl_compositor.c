/* SPDX-License-Identifier: MIT */
/* Mock Wayland compositor for host tests of the Wayland WSI (t_vk_present on the fake backend, wl_shm path). No GPU, no display.
 *   mock_wl_compositor <socket-name> <mode> [nformats]     (socket lives in $XDG_RUNTIME_DIR)
 *   mode: normal     frame callbacks fire and every buffer is released on the next commit (a well-behaved compositor)
 *         noframe    wl_surface.frame callbacks are never answered (hidden window / minimised, guest compositors that do not draw)
 *         norelease  buffers are never released (a compositor that holds on to them)
 *         stall      noframe + norelease
 *   nformats: how many wl_shm formats to advertise (default 4, max 30; 30 gives >16 VkSurfaceFormats and reproduces the vkGetPhysicalDeviceSurfaceFormatsKHR count a real
 *             compositor (gamescope) gives, which overflowed t_vk_present's fixed 16-entry array)
 * Stops after 60 s or on SIGTERM. Prints "READY" on stdout once the socket is listening and "COMMITS n" on exit. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server.h>

#include "xdg-shell-server-protocol.h"

static int g_frame = 1, g_release = 1, g_nfmt = 4;
static unsigned g_commits;
static struct wl_display *g_dpy;

struct surf {
   struct wl_resource *res, *buf, *pending_buf;
   struct wl_list frames;   /* pending wl_callback resources (wl_resource_get_link) */
   struct wl_resource *xs;
   int configured;
};

static void noop(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static void destroy_res(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }

/* ---- wl_buffer / wl_shm ---- */
static const struct wl_buffer_interface buffer_impl = {destroy_res};
static void pool_create_buffer(struct wl_client *c, struct wl_resource *pool, uint32_t id, int32_t off, int32_t w, int32_t h, int32_t stride, uint32_t fmt)
{
   (void)off; (void)w; (void)h; (void)stride; (void)fmt;
   struct wl_resource *b = wl_resource_create(c, &wl_buffer_interface, 1, id);
   wl_resource_set_implementation(b, &buffer_impl, NULL, NULL);
}
static void pool_resize(struct wl_client *c, struct wl_resource *r, int32_t s) { (void)c; (void)r; (void)s; }
static const struct wl_shm_pool_interface pool_impl = {pool_create_buffer, destroy_res, pool_resize};
static void shm_create_pool(struct wl_client *c, struct wl_resource *shm, uint32_t id, int32_t fd, int32_t size)
{
   (void)size;
   close(fd);
   struct wl_resource *p = wl_resource_create(c, &wl_shm_pool_interface, 1, id);
   wl_resource_set_implementation(p, &pool_impl, NULL, NULL);
}
static const struct wl_shm_interface shm_impl = {shm_create_pool};
static void shm_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id)
{
   (void)d;
   struct wl_resource *r = wl_resource_create(c, &wl_shm_interface, 1, id);
   wl_resource_set_implementation(r, &shm_impl, NULL, NULL);
   /* the real format list: the two mandatory ones first, then whatever else a compositor offers */
   /* DRM fourcc codes (wl_shm uses them, except ARGB8888/XRGB8888 = 0/1); everything wsi_common_wayland can map to a VkFormat */
#define FCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
   static const uint32_t f[] = {WL_SHM_FORMAT_ARGB8888, WL_SHM_FORMAT_XRGB8888, FCC('A','B','2','4'), FCC('X','B','2','4'),
      FCC('A','R','3','0'), FCC('X','R','3','0'), FCC('A','B','3','0'), FCC('X','B','3','0'), FCC('R','G','1','6'), FCC('B','G','1','6'),
      FCC('A','R','1','2'), FCC('X','R','1','2'), FCC('A','B','1','2'), FCC('X','B','1','2'), FCC('R','A','1','2'), FCC('R','X','1','2'),
      FCC('B','A','1','2'), FCC('B','X','1','2'), FCC('A','R','1','5'), FCC('X','R','1','5'), FCC('R','A','1','5'), FCC('R','X','1','5'),
      FCC('B','A','1','5'), FCC('B','X','1','5'), FCC('A','B','4','H'), FCC('X','B','4','H'), FCC('A','B','4','8'), FCC('X','B','4','8'),
      FCC('B','G','2','4'), FCC('R','G','2','4')};
   for (int i = 0; i < g_nfmt && i < (int)(sizeof f / sizeof *f); i++) wl_shm_send_format(r, f[i]);
}

/* ---- wl_surface ---- */
struct fcb { struct wl_list link; struct wl_resource *res; };
static void send_frames(struct surf *s)
{
   while (!wl_list_empty(&s->frames)) {
      struct fcb *f = wl_container_of(s->frames.next, f, link);
      wl_list_remove(&f->link);
      struct wl_resource *cb = f->res;
      free(f);
      wl_resource_set_user_data(cb, NULL);
      wl_callback_send_done(cb, (uint32_t)(g_commits * 16));
      wl_resource_destroy(cb);
   }
}
static void surf_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *b, int32_t x, int32_t y)
{
   (void)c; (void)x; (void)y;
   ((struct surf *)wl_resource_get_user_data(r))->pending_buf = b;
}
static void cb_destroy(struct wl_resource *r)
{
   struct fcb *f = wl_resource_get_user_data(r);
   if (f) { wl_list_remove(&f->link); free(f); }
}
static void surf_frame(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
   struct surf *s = wl_resource_get_user_data(r);
   struct fcb *f = calloc(1, sizeof *f);
   f->res = wl_resource_create(c, &wl_callback_interface, 1, id);
   wl_resource_set_implementation(f->res, NULL, f, cb_destroy);
   wl_list_insert(s->frames.prev, &f->link);
}
static void surf_commit(struct wl_client *c, struct wl_resource *r)
{
   (void)c;
   struct surf *s = wl_resource_get_user_data(r);
   g_commits++;
   if (s->pending_buf) {
      /* the previous buffer is replaced: a compositor that is done with it releases it */
      if (s->buf && g_release) wl_buffer_send_release(s->buf);
      s->buf = s->pending_buf;
      s->pending_buf = NULL;
      if (g_release) {   /* shm buffers are copied at commit, so the new one can be released at once as well */
         wl_buffer_send_release(s->buf);
         s->buf = NULL;
      }
   }
   if (g_frame) send_frames(s);
}
static void surf_set_buffer_transform(struct wl_client *c, struct wl_resource *r, int32_t t) { (void)c; (void)r; (void)t; }
static void surf_set_buffer_scale(struct wl_client *c, struct wl_resource *r, int32_t t) { (void)c; (void)r; (void)t; }
static void surf_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }
static void surf_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *g) { (void)c; (void)r; (void)g; }
static void surf_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) { (void)c; (void)r; (void)x; (void)y; }
static const struct wl_surface_interface surf_impl = {
   .destroy = destroy_res, .attach = surf_attach, .damage = surf_damage, .frame = surf_frame, .set_opaque_region = surf_region,
   .set_input_region = surf_region, .commit = surf_commit, .set_buffer_transform = surf_set_buffer_transform,
   .set_buffer_scale = surf_set_buffer_scale, .damage_buffer = surf_damage, .offset = surf_offset};
static void surf_free(struct wl_resource *r)
{
   struct surf *s = wl_resource_get_user_data(r);
   while (!wl_list_empty(&s->frames)) {   /* callbacks never sent (noframe/stall): destroy them without `done` before the list head is freed */
      struct fcb *f = wl_container_of(s->frames.next, f, link);
      wl_resource_destroy(f->res);          /* cb_destroy unlinks and frees f */
   }
   free(s);
}

static void region_add(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }
static const struct wl_region_interface region_impl = {destroy_res, region_add, region_add};
static void comp_create_surface(struct wl_client *c, struct wl_resource *comp, uint32_t id)
{
   struct surf *s = calloc(1, sizeof *s);
   wl_list_init(&s->frames);
   s->res = wl_resource_create(c, &wl_surface_interface, 4, id);
   wl_resource_set_implementation(s->res, &surf_impl, s, surf_free);
}
static void comp_create_region(struct wl_client *c, struct wl_resource *comp, uint32_t id)
{
   struct wl_resource *r = wl_resource_create(c, &wl_region_interface, 1, id);
   wl_resource_set_implementation(r, &region_impl, NULL, NULL);
}
static const struct wl_compositor_interface comp_impl = {comp_create_surface, comp_create_region};
static void comp_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id)
{
   (void)d;
   struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, ver < 4 ? ver : 4, id);
   wl_resource_set_implementation(r, &comp_impl, NULL, NULL);
}

/* ---- xdg_shell: toplevel with a 640x480 fullscreen configure ---- */
static void top_set_title(struct wl_client *c, struct wl_resource *r, const char *t) { (void)c; (void)r; (void)t; }
static void top_set_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p) { (void)c; (void)r; (void)p; }
static void top_wh(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c; (void)r; (void)w; (void)h; }
static void top_fs(struct wl_client *c, struct wl_resource *r, struct wl_resource *o) { (void)c; (void)r; (void)o; }
static void top_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, uint32_t n) { (void)c; (void)r; (void)s; (void)n; }
static void top_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, uint32_t n, uint32_t e) { (void)c; (void)r; (void)s; (void)n; (void)e; }
static const struct xdg_toplevel_interface top_impl = {
   .destroy = destroy_res, .set_parent = top_set_parent, .set_title = top_set_title, .set_app_id = top_set_title, .show_window_menu = NULL,
   .move = top_move, .resize = top_resize, .set_max_size = top_wh, .set_min_size = top_wh, .set_maximized = noop, .unset_maximized = noop,
   .set_fullscreen = top_fs, .unset_fullscreen = noop, .set_minimized = noop};
static void xs_get_toplevel(struct wl_client *c, struct wl_resource *xs, uint32_t id)
{
   struct wl_resource *t = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(xs), id);
   wl_resource_set_implementation(t, &top_impl, NULL, NULL);
   struct wl_array st;
   wl_array_init(&st);
   xdg_toplevel_send_configure(t, 640, 480, &st);
   wl_array_release(&st);
   xdg_surface_send_configure(xs, 1);
}
static void xs_ack(struct wl_client *c, struct wl_resource *r, uint32_t s) { (void)c; (void)r; (void)s; }
static void xs_geom(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }
static const struct xdg_surface_interface xs_impl = {destroy_res, xs_get_toplevel, NULL, xs_geom, xs_ack};
static void wm_get_xs(struct wl_client *c, struct wl_resource *wm, uint32_t id, struct wl_resource *surf)
{
   (void)surf;
   struct wl_resource *x = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(wm), id);
   wl_resource_set_implementation(x, &xs_impl, NULL, NULL);
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t s) { (void)c; (void)r; (void)s; }
static void wm_create_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id) { (void)c; (void)r; (void)id; }
static const struct xdg_wm_base_interface wm_impl = {destroy_res, wm_create_positioner, wm_get_xs, wm_pong};
static void wm_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id)
{
   (void)d;
   struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, ver < 2 ? ver : 2, id);
   wl_resource_set_implementation(r, &wm_impl, NULL, NULL);
}

static void on_term(int s) { (void)s; wl_display_terminate(g_dpy); }

int main(int argc, char **argv)
{
   if (argc < 3) { fprintf(stderr, "usage: mock_wl_compositor <socket> normal|noframe|norelease|stall [nformats]\n"); return 2; }
   const char *m = argv[2];
   g_frame = strcmp(m, "noframe") && strcmp(m, "stall");
   g_release = strcmp(m, "norelease") && strcmp(m, "stall");
   if (argc > 3) g_nfmt = atoi(argv[3]);
   g_dpy = wl_display_create();
   if (wl_display_add_socket(g_dpy, argv[1])) { fprintf(stderr, "mock: cannot add socket %s\n", argv[1]); return 1; }
   wl_global_create(g_dpy, &wl_compositor_interface, 4, NULL, comp_bind);
   wl_global_create(g_dpy, &wl_shm_interface, 1, NULL, shm_bind);
   wl_global_create(g_dpy, &xdg_wm_base_interface, 2, NULL, wm_bind);
   signal(SIGTERM, on_term);
   signal(SIGINT, on_term);
   alarm(60);
   signal(SIGALRM, on_term);
   printf("READY\n");
   fflush(stdout);
   wl_display_run(g_dpy);
   printf("COMMITS %u\n", g_commits);
   wl_display_destroy(g_dpy);
   return 0;
}
