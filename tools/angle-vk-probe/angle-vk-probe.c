/* SPDX-License-Identifier: MIT */
/* angle-vk-probe: does ANGLE's Vulkan backend create a display, a pbuffer and a context on our EGL/driver?
 *
 * One line on stdout, then exit:
 *   PASS renderer=<GL_RENDERER> version=<GL_VERSION> backend=<angle-vulkan|angle-gl|egl-x11-khr|mesa-surfaceless>
 *   FAIL stage=<eglGetPlatformDisplay|eglInitialize|eglChooseConfig|eglCreatePbufferSurface|eglCreateContext|
 *               eglMakeCurrent|dlopen> error=0x<hex>
 * Everything else (per-attempt log, env) goes to stderr.
 *
 * No link-time EGL/GLES/X11: libEGL and libGLESv2 are dlopen()ed from $EGL_DIR (default the steamrt tree), and
 * X11/xcb handles come from dlopen too, so one binary runs against any ICD. The four ANGLE constants and the
 * EGL/GLES prototypes are defined here (the probe must build without ANGLE's SDK headers, which are not on the
 * host). Values are the documented ones, see README.
 *
 * Build (aarch64, repo cross toolchain), from mali-open/:
 *   ../droidtest/tools/cc-aarch64-glibc -std=gnu11 -O2 -Wall -Wextra -Werror -o /tmp/angle-vk-probe \
 *       tools/angle-vk-probe/angle-vk-probe.c -ldl
 */
#define _DEFAULT_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- EGL/GLES types (probe-local; no EGL headers needed) ---------------------------------------- */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;

#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_NONE 0x3038
#define EGL_SUCCESS 0x3000
#define EGL_NOT_INITIALIZED 0x3001
#define EGL_BAD_CONFIG 0x3002
#define EGL_BAD_DISPLAY 0x3008

#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_BIT 0x0008
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_EXTENSIONS 0x3055
#define EGL_VENDOR 0x3053

/* ANGLE platform selection (EGL_ANGLE_platform_angle). */
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE 0x3450
#define EGL_PLATFORM_ANGLE_TYPE_GL_ANGLE 0x3205
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE 0x3209
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE 0x320A
/* Non-ANGLE native platforms, tried after ANGLE only to say what else the EGL in EGL_DIR answers. */
#define EGL_PLATFORM_X11_KHR 0x31D5
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

struct egl {
   void *lib;
   void *gles_lib;
   void *(*GetProcAddress)(const char *);
   EGLDisplay (*GetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
   EGLDisplay (*GetDisplay)(void *);
   EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
   EGLBoolean (*BindAPI)(EGLenum);
   EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
   EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
   EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
   EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
   EGLint (*GetError)(void);
   const char *(*QueryString)(EGLDisplay, EGLint);
   const unsigned char *(*glGetString)(unsigned);
};

/* stage numbers, in the order they are reached: the deepest failure is the one reported */
enum stage {
   ST_DLOPEN = 0,
   ST_PLATFORM,
   ST_INIT,
   ST_CONFIG,
   ST_PBUFFER,
   ST_CONTEXT,
   ST_CURRENT,
};

static const char *const stage_name[] = {
   [ST_DLOPEN] = "dlopen",
   [ST_PLATFORM] = "eglGetPlatformDisplay",
   [ST_INIT] = "eglInitialize",
   [ST_CONFIG] = "eglChooseConfig",
   [ST_PBUFFER] = "eglCreatePbufferSurface",
   [ST_CONTEXT] = "eglCreateContext",
   [ST_CURRENT] = "eglMakeCurrent",
};

struct result {
   enum stage stage;
   EGLint err;
   const char *dpy;
   const char *backend;
   char renderer[512];
   char version[512];
   char vendor[512];
};

static const char *env_or(const char *name, const char *def)
{
   const char *v = getenv(name);
   return v && *v ? v : def;
}

/* first of names that loads, from dir (when set) and then the default search path */
static void *dl_first(const char *dir, const char *const *names)
{
   char path[1024];

   for (int i = 0; names[i]; i++) {
      if (dir && *dir) {
         snprintf(path, sizeof path, "%s/%s", dir, names[i]);
         void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
         if (h)
            return h;
      }
      void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
      if (h)
         return h;
   }
   return NULL;
}

static void *egl_sym(struct egl *e, const char *name)
{
   void *p = dlsym(e->lib, name);
   if (!p && e->GetProcAddress)
      p = e->GetProcAddress(name);
   return p;
}

static int load_egl(struct egl *e, const char *dir)
{
   static const char *const egl_names[] = {"libEGL.so.1", "libEGL.so", "libEGL_angle.so", NULL};
   static const char *const gles_names[] = {"libGLESv2.so.2", "libGLESv2.so", "libGLESv2_angle.so", NULL};
   void *p;

   memset(e, 0, sizeof *e);
   e->lib = dl_first(dir, egl_names);
   if (!e->lib)
      return -1;
   e->GetProcAddress = (void *(*)(const char *))dlsym(e->lib, "eglGetProcAddress");

#define LOAD(field, name, required)                                                                    \
   do {                                                                                                \
      p = egl_sym(e, name);                                                                            \
      if (!p) {                                                                                        \
         if (required) {                                                                               \
            fprintf(stderr, "probe: missing %s in the EGL in %s\n", name, dir);                        \
            return -1;                                                                                 \
         }                                                                                             \
      } else {                                                                                         \
         memcpy(&e->field, &p, sizeof p);                                                              \
      }                                                                                                \
   } while (0)

   LOAD(GetPlatformDisplayEXT, "eglGetPlatformDisplayEXT", 1);
   LOAD(Initialize, "eglInitialize", 1);
   LOAD(ChooseConfig, "eglChooseConfig", 1);
   LOAD(CreateContext, "eglCreateContext", 1);
   LOAD(MakeCurrent, "eglMakeCurrent", 1);
   LOAD(GetError, "eglGetError", 1);
   LOAD(GetDisplay, "eglGetDisplay", 0);
   LOAD(BindAPI, "eglBindAPI", 0);
   LOAD(CreatePbufferSurface, "eglCreatePbufferSurface", 0);
   LOAD(QueryString, "eglQueryString", 0);
#undef LOAD

   if (!e->CreatePbufferSurface) {
      fprintf(stderr, "probe: missing eglCreatePbufferSurface in the EGL in %s\n", dir);
      return -1;
   }

   e->gles_lib = dl_first(dir, gles_names);
   if (e->gles_lib) {
      p = dlsym(e->gles_lib, "glGetString");
      if (p)
         memcpy(&e->glGetString, &p, sizeof p);
   }
   if (!e->glGetString)
      fprintf(stderr, "probe: no glGetString in libGLESv2 under %s (PASS will report renderer=unknown)\n", dir);
   return 0;
}

static void clean_string(char *dst, size_t cap, const unsigned char *src)
{
   if (!src) {
      snprintf(dst, cap, "unknown");
      return;
   }
   size_t j = 0;
   for (size_t i = 0; src[i] && j + 1 < cap; i++) {
      char c = (char)src[i];
      dst[j++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
   }
   dst[j] = 0;
}

struct candidate {
   const char *backend; /* what answers, for the log and the PASS line */
   EGLenum platform;    /* EGL_PLATFORM_ANGLE_ANGLE, EGL_PLATFORM_X11_KHR, ... */
   EGLint type;         /* ANGLE device/backend type, 0 when not ANGLE */
   void *ndpy;          /* native display: Display*, xcb_connection_t*, or NULL */
   const char *dpy;     /* name of the native display, for the log */
};

/* One full EGL run. Returns 0 on PASS, -1 on failure with res->stage/err set. */
static int run_candidate(struct egl *e, const struct candidate *c, struct result *res)
{
   EGLint attrs[9];
   int n = 0;
   EGLint major = 0, minor = 0, ncfg = 0;
   EGLDisplay dpy;
   EGLConfig cfg = NULL;
   EGLSurface surf;
   EGLContext ctx;
   EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT | EGL_OPENGL_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                         EGL_NONE};
   EGLint pb_attrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
   EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

   res->backend = c->backend;
   res->dpy = c->dpy;

   if (c->platform == EGL_PLATFORM_ANGLE_ANGLE) {
      attrs[n++] = EGL_PLATFORM_ANGLE_TYPE_ANGLE;
      attrs[n++] = c->type;
      if (c->type == EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE) {
         attrs[n++] = EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE;
         attrs[n++] = EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE;
      }
      attrs[n++] = EGL_NONE;
   }

   res->stage = ST_PLATFORM;
   dpy = e->GetPlatformDisplayEXT(c->platform, c->ndpy, n ? attrs : NULL);
   if (!dpy || dpy == (EGLDisplay)(intptr_t)0) {
      res->err = e->GetError();
      return -1;
   }

   res->stage = ST_INIT;
   if (!e->Initialize(dpy, &major, &minor)) {
      res->err = e->GetError();
      return -1;
   }
   fprintf(stderr, "probe: backend=%s dpy=%s eglInitialize -> %d.%d\n", c->backend, c->dpy, major, minor);
   if (e->QueryString) {
      const char *vend = e->QueryString(dpy, EGL_VENDOR);
      const char *ext = e->QueryString(dpy, EGL_EXTENSIONS);
      fprintf(stderr, "probe: egl vendor=%s\n", vend ? vend : "?");
      if (ext)
         fprintf(stderr, "probe: egl_extensions=%s\n", ext);
   }
   if (e->BindAPI)
      e->BindAPI(EGL_OPENGL_ES_API);

   res->stage = ST_CONFIG;
   if (!e->ChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) {
      res->err = ncfg < 1 ? EGL_BAD_CONFIG : e->GetError();
      return -1;
   }

   res->stage = ST_PBUFFER;
   surf = e->CreatePbufferSurface(dpy, cfg, pb_attrs);
   if (!surf) {
      res->err = e->GetError();
      return -1;
   }

   res->stage = ST_CONTEXT;
   ctx = e->CreateContext(dpy, cfg, NULL, ctx_attrs);
   if (!ctx) {
      res->err = e->GetError();
      return -1;
   }

   res->stage = ST_CURRENT;
   if (!e->MakeCurrent(dpy, surf, surf, ctx)) {
      res->err = e->GetError();
      return -1;
   }

   clean_string(res->renderer, sizeof res->renderer, e->glGetString ? e->glGetString(GL_RENDERER) : NULL);
   clean_string(res->version, sizeof res->version, e->glGetString ? e->glGetString(GL_VERSION) : NULL);
   clean_string(res->vendor, sizeof res->vendor, e->glGetString ? e->glGetString(GL_VENDOR) : NULL);
   res->err = EGL_SUCCESS;
   return 0;
}

int main(void)
{
   const char *egl_dir = env_or("EGL_DIR", "/root/.local/share/Steam/steamrtarm64");
   const char *display_env = getenv("DISPLAY");
   struct egl e;
   struct result best, r;
   void *x11 = NULL, *xcb = NULL;
   int have_attempt = 0, passed = 0, rc = 1;

   memset(&best, 0, sizeof best);
   best.stage = ST_DLOPEN;

   fprintf(stderr, "probe: EGL_DIR=%s DISPLAY=%s VK_ICD_FILENAMES=%s VK_DRIVER_FILES=%s\n", egl_dir,
           display_env && *display_env ? display_env : "(unset)",
           env_or("VK_ICD_FILENAMES", "(unset)"), env_or("VK_DRIVER_FILES", "(unset)"));

   if (load_egl(&e, egl_dir) != 0) {
      fprintf(stderr, "probe: cannot load an EGL from %s: %s\n", egl_dir, dlerror() ? dlerror() : "no symbol");
      printf("FAIL stage=dlopen error=0x0\n");
      return 2;
   }

   /* Native display: X11 first, then a raw xcb connection (the ANGLE X11 backend takes either). */
   {
      static const char *const x11_names[] = {"libX11.so.6", "libX11.so", NULL};
      void *lib = dl_first(NULL, x11_names);
      void *(*open_display)(const char *) = NULL;
      if (lib) {
         void *p = dlsym(lib, "XOpenDisplay");
         if (p)
            memcpy(&open_display, &p, sizeof p);
      }
      if (open_display) {
         x11 = open_display(NULL);
         fprintf(stderr, "probe: XOpenDisplay(%s) -> %p\n", display_env && *display_env ? display_env : "NULL",
                 x11);
      } else {
         fprintf(stderr, "probe: no libX11 (XOpenDisplay not available)\n");
      }
   }
   {
      static const char *const xcb_names[] = {"libxcb.so.1", "libxcb.so", NULL};
      void *lib = dl_first(NULL, xcb_names);
      if (lib) {
         void *(*connect)(const char *, int *) = NULL;
         int (*has_error)(void *) = NULL;
         void *p = dlsym(lib, "xcb_connect");
         if (p)
            memcpy(&connect, &p, sizeof p);
         p = dlsym(lib, "xcb_connection_has_error");
         if (p)
            memcpy(&has_error, &p, sizeof p);
         if (connect) {
            int screen = 0;
            xcb = connect(NULL, &screen);
            if (xcb && has_error && has_error(xcb)) {
               fprintf(stderr, "probe: xcb_connect failed (has_error!=0)\n");
               xcb = NULL;
            } else {
               fprintf(stderr, "probe: xcb_connect -> %p screen=%d\n", xcb, screen);
            }
         }
      } else {
         fprintf(stderr, "probe: no libxcb\n");
      }
   }

   struct candidate cands[8];
   int nc = 0;
   /* ANGLE Vulkan first: that is the question. X11, then xcb, then EGL_DEFAULT_DISPLAY (headless). */
   if (x11)
      cands[nc++] = (struct candidate){"angle-vulkan", EGL_PLATFORM_ANGLE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE, x11, "x11"};
   if (xcb)
      cands[nc++] = (struct candidate){"angle-vulkan", EGL_PLATFORM_ANGLE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE, xcb, "xcb"};
   cands[nc++] = (struct candidate){"angle-vulkan", EGL_PLATFORM_ANGLE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE, NULL, "default"};
   if (x11)
      cands[nc++] = (struct candidate){"angle-gl", EGL_PLATFORM_ANGLE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_GL_ANGLE, x11, "x11"};
   if (xcb)
      cands[nc++] = (struct candidate){"angle-gl", EGL_PLATFORM_ANGLE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_GL_ANGLE, xcb, "xcb"};
   if (x11)
      cands[nc++] = (struct candidate){"egl-x11-khr", EGL_PLATFORM_X11_KHR, 0, x11, "x11"};
   if (xcb)
      cands[nc++] = (struct candidate){"egl-x11-khr", EGL_PLATFORM_X11_KHR, 0, xcb, "xcb"};
   /* Opt-in only: the surfaceless platform proves the EGL/GLES plumbing itself, but it is not an ANGLE answer. */
   if (env_or("PROBE_EXTRA", "")[0] && !strcmp(env_or("PROBE_EXTRA", ""), "surfaceless"))
      cands[nc++] = (struct candidate){"mesa-surfaceless", EGL_PLATFORM_SURFACELESS_MESA, 0, NULL, "surfaceless"};

   for (int i = 0; i < nc; i++) {
      memset(&r, 0, sizeof r);
      have_attempt = 1;
      if (run_candidate(&e, &cands[i], &r) == 0) {
         passed = 1;
         best = r;
         break;
      }
      fprintf(stderr, "probe: attempt backend=%s dpy=%s -> FAIL stage=%s error=0x%04x\n", r.backend, r.dpy,
              stage_name[r.stage], (unsigned)r.err);
      if (i == 0 || r.stage > best.stage)
         best = r;
   }

   if (!passed) {
      if (!have_attempt) {
         printf("FAIL stage=dlopen error=0x0\n");
         return 2;
      }
      printf("FAIL stage=%s error=0x%04x\n", stage_name[best.stage], (unsigned)best.err);
      fprintf(stderr, "probe: deepest failure backend=%s dpy=%s vendor=%s\n", best.backend, best.dpy, best.vendor);
      return 1;
   }

   printf("PASS renderer=%s version=%s backend=%s\n", best.renderer, best.version, best.backend);
   fprintf(stderr, "probe: PASS backend=%s dpy=%s gl_vendor=%s\n", best.backend, best.dpy, best.vendor);
   rc = 0;
   return rc;
}
