/*
 * fencesync.c — the fence half of the GPU-composited Steam menu's first error.
 *
 * On x8 (2026-11-22 01:53) CEF's ANGLE-GL backend logged, first:
 *   WARNING:angle_platform_impl.cc(49)] Debug.cpp:186 (insertMessage):
 *   GL error: HIGH: Error: 0x00000505, in .../renderer/gl/SyncGL.cpp, set:42.
 *   Internal error: 0x00000505: glFenceSync failed to create a GLsync object.
 * and then lost the context (Skia OOM) and never painted. The GL under ANGLE
 * there was zink on our PanVK. This program is the refuter: a minimal GL
 * context on zink, drawing a triangle and doing glFenceSync + glFlush +
 * glClientWaitSync(100 ms) + glDeleteSync 1000 times, printing the GL error
 * after every call. Variant B (FENCE_VARIANT=egl) does the same through
 * EGL_KHR_fence_sync (eglCreateSyncKHR), which is what Chromium creates around
 * a composite.
 *
 * The window and context code is taken verbatim from tools/gl-ladder.c
 * (branch lane/gl-ladder): X11/GLX, entry points resolved at runtime, X errors
 * counted instead of fatal -- neither aarch64 sysroot ships GL headers.
 *
 * Build:  tools/fencesync.sh            (host)      -> /tmp/fencesync
 *         tools/fencesync.sh --cross    (aarch64)   -> tools/fencesync-a64
 * Run:    FENCE_VARIANT=gl|egl FENCE_ITERS=1000 FENCE_TIMEOUT_MS=100 ./fencesync
 *         with MESA_LOADER_DRIVER_OVERRIDE=zink VK_DRIVER_FILES=<our icd>
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stddef.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* ---- GL / GLX / X11 types (no GL headers on the guest sysroot) ---------- */
typedef unsigned int   GLenum;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLboolean;
typedef unsigned char  GLubyte;
typedef float          GLfloat;
typedef unsigned int   GLbitfield;
typedef char           GLchar;
typedef ptrdiff_t      GLsizeiptr;
typedef ptrdiff_t      GLintptr;
typedef ptrdiff_t      GLintptrARB;

typedef struct __GLXFBConfigRec *GLXFBConfig;
typedef struct __GLXcontextRec  *GLXContext;
typedef struct __GLsync        *GLsync;
typedef XID GLXDrawable;

#define GL_NO_ERROR                            0
#define GL_INVALID_ENUM                        0x0500
#define GL_INVALID_VALUE                       0x0501
#define GL_INVALID_OPERATION                   0x0502
#define GL_OUT_OF_MEMORY                       0x0505
#define GL_INVALID_FRAMEBUFFER_OPERATION       0x0506
#define GL_FRAMEBUFFER_COMPLETE                0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT   0x8CD6
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#define GL_FRAMEBUFFER_UNSUPPORTED             0x8CDD
#define GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER  0x8CDB
#define GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER  0x8CDC
#define GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE  0x8D56
#define GL_FRAMEBUFFER_UNDEFINED               0x8219
#define GL_FRAMEBUFFER                         0x8D40
#define GL_READ_FRAMEBUFFER                    0x8CA8
#define GL_DRAW_FRAMEBUFFER                    0x8CA9
#define GL_COLOR_ATTACHMENT0                   0x8CE0
#define GL_TEXTURE_2D                          0x0DE1
#define GL_RGBA                                0x1908
#define GL_RGBA8                               0x8058
#define GL_SRGB8_ALPHA8                        0x8C43
#define GL_UNSIGNED_BYTE                       0x1401
#define GL_TEXTURE_MIN_FILTER                  0x2801
#define GL_TEXTURE_MAG_FILTER                  0x2800
#define GL_NEAREST                             0x2600
#define GL_FRAMEBUFFER_SRGB                    0x8DB9
#define GL_COLOR_BUFFER_BIT                    0x00004000
#define GL_BACK                                0x0405
#define GL_FRONT                               0x0404
#define GL_RGB                                 0x1907
#define GL_PACK_ALIGNMENT                      0x0D05
#define GL_VENDOR                              0x1F00
#define GL_RENDERER                            0x1F01
#define GL_VERSION                             0x1F02
#define GL_MAJOR_VERSION                       0x821B
#define GL_MINOR_VERSION                       0x821C
#define GL_TRIANGLES                           0x0004
#define GL_FLOAT                               0x1406
#define GL_FALSE                               0
#define GL_TRUE                                1
#define GL_VERTEX_SHADER                       0x8B31
#define GL_FRAGMENT_SHADER                     0x8B30
#define GL_COMPILE_STATUS                      0x8B81
#define GL_LINK_STATUS                         0x8B82
#define GL_ARRAY_BUFFER                        0x8892
#define GL_STATIC_DRAW                         0x88E4
#define GL_PROJECTION                          0x1701
#define GL_MODELVIEW                           0x1700
#define GL_VERTEX_ARRAY                        0x8074
#define GL_TEXTURE_COORD_ARRAY                 0x8078
#define GL_TEXTURE_2D_ENABLE_                 0x0DE1

#define GLX_USE_GL                             1
#define GLX_RGBA                               4
#define GLX_RGBA_BIT                           0x00000001
#define GLX_WINDOW_BIT                         0x00000001
#define GLX_DOUBLEBUFFER                       5
#define GLX_RED_SIZE                           8
#define GLX_GREEN_SIZE                         9
#define GLX_BLUE_SIZE                          10
#define GLX_ALPHA_SIZE                         11
#define GLX_DEPTH_SIZE                         12
#define GLX_X_VISUAL_TYPE                      0x22
#define GLX_TRUE_COLOR                         0x8002
#define GLX_X_RENDERABLE                       0x8012
#define GLX_DRAWABLE_TYPE                      0x8010
#define GLX_RENDER_TYPE                        0x8011
#define GLX_VISUAL_ID                          0x800B
#define GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB       0x20B2
#define GLX_CONTEXT_MAJOR_VERSION_ARB          0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB          0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB           0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB       0x00000001
#define GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002

/* ---- the entry points we need, resolved at runtime --------------------- */
#ifndef GL_SAMPLES_PASSED
#define GL_SAMPLES_PASSED 0x8914
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif

#define GL_FUNCS(X)                                                                   \
    X(GLenum,        glGetError,            (void))                                   \
    X(const GLubyte *, glGetString,         (GLenum))                                 \
    X(void,          glGetIntegerv,         (GLenum, GLint *))                        \
    X(void,          glClear,               (GLbitfield))                             \
    X(void,          glClearColor,          (GLfloat, GLfloat, GLfloat, GLfloat))     \
    X(void,          glViewport,            (GLint, GLint, GLsizei, GLsizei))         \
    X(void,          glEnable,              (GLenum))                                 \
    X(void,          glDisable,             (GLenum))                                 \
    X(GLboolean,     glIsEnabled,           (GLenum))                                 \
    X(void,          glFinish,              (void))                                   \
    X(void,          glFlush,               (void))                                   \
    X(void,          glGenTextures,         (GLsizei, GLuint *))                      \
    X(void,          glBindTexture,         (GLenum, GLuint))                         \
    X(void,          glTexImage2D,          (GLenum, GLint, GLint, GLsizei, GLsizei,  \
                                             GLint, GLenum, GLenum, const void *))    \
    X(void,          glTexParameteri,       (GLenum, GLenum, GLint))                  \
    X(void,          glDeleteTextures,      (GLsizei, const GLuint *))                \
    X(void,          glReadPixels,          (GLint, GLint, GLsizei, GLsizei, GLenum,  \
                                             GLenum, void *))                         \
    X(void,          glReadBuffer,          (GLenum))                                 \
    X(void,          glDrawBuffer,          (GLenum))                                 \
    X(void,          glPixelStorei,         (GLenum, GLint))                          \
    X(void,          glGenFramebuffers,     (GLsizei, GLuint *))                      \
    X(void,          glBindFramebuffer,     (GLenum, GLuint))                         \
    X(void,          glFramebufferTexture2D,(GLenum, GLenum, GLenum, GLuint, GLint))  \
    X(GLenum,        glCheckFramebufferStatus, (GLenum))                              \
    X(void,          glDeleteFramebuffers,  (GLsizei, const GLuint *))                \
    X(void,          glBlitFramebuffer,     (GLint, GLint, GLint, GLint, GLint,       \
                                             GLint, GLint, GLint, GLbitfield, GLenum))\
    X(void,          glGenQueries,          (GLsizei, GLuint *))                       \
    X(void,          glBeginQuery,          (GLenum, GLuint))                          \
    X(void,          glEndQuery,            (GLenum))                                  \
    X(void,          glGetQueryObjectuiv,   (GLuint, GLenum, GLuint *))                \
    X(GLuint,        glCreateShader,        (GLenum))                                 \
    X(void,          glShaderSource,        (GLuint, GLsizei, const GLchar *const *,  \
                                             const GLint *))                          \
    X(void,          glCompileShader,       (GLuint))                                 \
    X(void,          glGetShaderiv,         (GLuint, GLenum, GLint *))                \
    X(void,          glGetShaderInfoLog,    (GLuint, GLsizei, GLsizei *, GLchar *))   \
    X(void,          glDeleteShader,        (GLuint))                                 \
    X(GLuint,        glCreateProgram,       (void))                                   \
    X(void,          glAttachShader,        (GLuint, GLuint))                         \
    X(void,          glBindAttribLocation,  (GLuint, GLuint, const GLchar *))         \
    X(void,          glLinkProgram,         (GLuint))                                 \
    X(void,          glGetProgramiv,        (GLuint, GLenum, GLint *))                \
    X(void,          glGetProgramInfoLog,   (GLuint, GLsizei, GLsizei *, GLchar *))   \
    X(void,          glUseProgram,          (GLuint))                                 \
    X(void,          glDeleteProgram,       (GLuint))                                 \
    X(void,          glEnableVertexAttribArray, (GLuint))                             \
    X(void,          glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei,\
                                             const void *))                           \
    X(void,          glDrawArrays,          (GLenum, GLint, GLsizei))                 \
    X(void,          glMatrixMode,          (GLenum))                                 \
    X(void,          glLoadIdentity,        (void))                                   \
    X(void,          glBegin,               (GLenum))                                 \
    X(void,          glEnd,                 (void))                                   \
    X(void,          glVertex2f,            (GLfloat, GLfloat))                       \
    X(void,          glColor4f,             (GLfloat, GLfloat, GLfloat, GLfloat))     \
    X(void,          glEnableClientState,   (GLenum))                                 \
    X(void,          glDisableClientState,  (GLenum))                                 \
    X(void,          glVertexPointer,       (GLint, GLenum, GLsizei, const void *))   \
    X(void,          glTexCoordPointer,     (GLint, GLenum, GLsizei, const void *))    \
    X(void,          glUniform1i,           (GLint, GLint))                            \
    X(GLint,         glGetUniformLocation,  (GLuint, const GLchar *))                 \
    X(GLsync,        glFenceSync,           (GLenum, GLbitfield))                     \
    X(GLenum,        glClientWaitSync,      (GLsync, GLbitfield, unsigned long long))  \
    X(void,          glDeleteSync,          (GLsync))

#define GLX_FUNCS(X)                                                                  \
    X(GLXFBConfig *, glXChooseFBConfig,     (Display *, int, const int *, int *))     \
    X(XVisualInfo *, glXGetVisualFromFBConfig, (Display *, GLXFBConfig))              \
    X(GLXContext,    glXCreateContext,      (Display *, XVisualInfo *, GLXContext, Bool))\
    X(Bool,          glXMakeCurrent,        (Display *, GLXDrawable, GLXContext))     \
    X(void,          glXSwapBuffers,        (Display *, GLXDrawable))                 \
    X(void,          glXDestroyContext,     (Display *, GLXContext))                  \
    X(const char *,  glXQueryExtensionsString, (Display *, int))                      \
    X(int,           glXGetFBConfigAttrib,  (Display *, GLXFBConfig, int, int *))     \
    X(void *,        glXGetProcAddressARB,  (const GLubyte *))

#define DECL(ret, name, args) static ret (*name) args;
GL_FUNCS(DECL)
GLX_FUNCS(DECL)
#undef DECL

static void *libgl;
static int missing_syms;

static void *gsym(const char *n)
{
    void *p = NULL;
    if (glXGetProcAddressARB) p = glXGetProcAddressARB((const GLubyte *)n);
    if (!p && libgl) p = dlsym(libgl, n);
    return p;
}

static int load_gl(void)
{
    libgl = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!libgl) libgl = dlopen("libGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!libgl) {
        fprintf(stderr, "fencesync: cannot dlopen libGL.so.1: %s\n", dlerror());
        return 0;
    }
#define LOAD1(ret, name, args)                                                   \
    do {                                                                         \
        void *p_ = dlsym(libgl, #name);                                          \
        if (!p_) p_ = gsym(#name);                                               \
        *(void **)(&name) = p_;                                                   \
        if (!p_) { fprintf(stderr, "fencesync: no %s\n", #name); missing_syms++; }\
    } while (0);
    GLX_FUNCS(LOAD1)
    if (!glXGetProcAddressARB) return 0;
    GL_FUNCS(LOAD1)
#undef LOAD1
    return 1;
}

/* ---- tiny error/diagnostic helpers ------------------------------------- */

static const char *errname(GLenum e)
{
    switch (e) {
    case GL_NO_ERROR: return "0";
    case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
    default: return "other";
    }
}

/* drain the error queue, report the first error seen (and the count) */
static void drain_errors(char *buf, size_t n)
{
    GLenum first = GL_NO_ERROR, e;
    int count = 0;
    while ((e = glGetError()) != GL_NO_ERROR) {
        if (first == GL_NO_ERROR) first = e;
        count++;
        if (count > 32) break;
    }
    if (count == 0) snprintf(buf, n, "0");
    else snprintf(buf, n, "%s(x%d)", errname(first), count);
}

static const char *completeness(void)
{
    switch (glCheckFramebufferStatus(GL_FRAMEBUFFER)) {
    case GL_FRAMEBUFFER_COMPLETE: return "COMPLETE";
    case GL_FRAMEBUFFER_UNDEFINED: return "UNDEFINED";
    case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: return "INCOMPLETE_ATTACHMENT";
    case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: return "MISSING_ATTACHMENT";
    case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER: return "INCOMPLETE_DRAW_BUFFER";
    case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER: return "INCOMPLETE_READ_BUFFER";
    case GL_FRAMEBUFFER_UNSUPPORTED: return "UNSUPPORTED";
    case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE: return "INCOMPLETE_MULTISAMPLE";
    default: return "unknown-status";
    }
}

/* ---- window / context -------------------------------------------------- */

#define WIDTH  800
#define HEIGHT 600
#define FBO_W  800
#define FBO_H  600

static Display *dpy;
static int scr;

/* GLX failures (a rejected profile, a bad FBConfig) arrive as X protocol errors,
 * and Xlib's default handler exits the process. Keep them countable instead. */
static int x_err;
static int x_error_handler(Display *d, XErrorEvent *e)
{
    (void)d;
    x_err = e->error_code;
    return 0;
}
static int take_x_error(void) { int e = x_err; x_err = 0; return e; }

struct win {
    Window w;
    GLXContext ctx;
    GLXFBConfig cfg;
    XVisualInfo *vi;
    int w_px, h_px;
    int srgb_capable;
};

static int has_glx_ext(const char *name)
{
    const char *p = glXQueryExtensionsString(dpy, scr);
    size_t len = strlen(name);
    for (; p && *p; p = strchr(p, ' ')) {
        while (*p == ' ') p++;
        if (!strncmp(p, name, len) && (p[len] == ' ' || p[len] == '\0'))
            return 1;
        if (!strchr(p, ' ')) break;
    }
    return 0;
}

/* Ask for an FBConfig list and say what happened, including any X protocol
 * error: Xwayland under gamescope answers an unsupported attribute set with
 * GLXBadFBConfig instead of an empty list. */
static GLXFBConfig *try_cfgs(const char *what, int *attrs, int *n_out)
{
    int n = 0;
    GLXFBConfig *c = glXChooseFBConfig(dpy, scr, attrs, &n);
    XSync(dpy, False);
    printf("fbconfig[%s] %s n=%d x_error=%d\n", what, c ? "ok" : "NULL", n, take_x_error());
    *n_out = n;
    return c;
}

static int open_window(struct win *out, const char *title, GLXContext share, int want_srgb)
{
    static int attrs_depth[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 24,
        GLX_DOUBLEBUFFER, True,
        0
    };
    static int attrs_nodepth[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_DOUBLEBUFFER, True,
        0
    };
    int attrs_srgb[64];
    GLXFBConfig *cand;
    int n = 0, i, srgb = 0, have_cfg = 0;
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    Window w;
    GLXContext ctx;

    if (want_srgb && has_glx_ext("GLX_ARB_framebuffer_sRGB")) {
        for (i = 0; attrs_depth[i] != 0; i++) attrs_srgb[i] = attrs_depth[i];
        attrs_srgb[i++] = GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB;
        attrs_srgb[i++] = True;
        attrs_srgb[i++] = 0;
        cand = try_cfgs("rgba8+depth24+srgb", attrs_srgb, &n);
        if (cand && n > 0) { out->cfg = cand[0]; srgb = 1; have_cfg = 1; }
        if (cand) XFree(cand);
    }
    if (!have_cfg) {
        cand = try_cfgs("rgba8+depth24", attrs_depth, &n);
        if (cand && n > 0) { out->cfg = cand[0]; have_cfg = 1; }
        if (cand) XFree(cand);
    }
    if (!have_cfg) {
        cand = try_cfgs("rgba8", attrs_nodepth, &n);
        if (cand && n > 0) { out->cfg = cand[0]; have_cfg = 1; }
        if (cand) XFree(cand);
    }
    {
        int have = 0;
        int id = 0;
        if (glXGetFBConfigAttrib && out->cfg) {
            glXGetFBConfigAttrib(dpy, out->cfg, GLX_VISUAL_ID, &id);
            have = 1;
        }
        printf("fbconfig=chosen visual_id=0x%x srgb=%d have=%d\n", id, srgb, have);
    }
    if (!out->cfg) { fprintf(stderr, "fencesync: no usable FBConfig\n"); return 0; }

    vi = glXGetVisualFromFBConfig(dpy, out->cfg);
    if (!vi) { fprintf(stderr, "fencesync: no XVisualInfo\n"); return 0; }
    out->vi = vi;

    memset(&swa, 0, sizeof swa);
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen),
                                   vi->visual, AllocNone);
    swa.background_pixmap = None;
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask;
    w = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, WIDTH, HEIGHT, 0,
                      vi->depth, InputOutput, vi->visual,
                      CWBorderPixel | CWColormap | CWEventMask, &swa);
    if (!w) { fprintf(stderr, "fencesync: XCreateWindow failed\n"); return 0; }
    XStoreName(dpy, w, title);
    /* glxgears (Mesa demos/glxgears.c) names its window and sets a class: XStoreName alone leaves WM_CLASS
     * unset, and a window manager (gamescope's xwm included) that keys on WM_CLASS puts such a window in
     * no class at all. Same for WM_COMMAND (XSetStandardProperties sets name, class, command and hints). */
    {
        XClassHint ch;
        ch.res_name = (char *)"gl-ladder";
        ch.res_class = (char *)"GL-Ladder";
        XSetClassHint(dpy, w, &ch);
        XSetStandardProperties(dpy, w, title, "GL-Ladder", None, NULL, 0, NULL);
    }
    {
        XSizeHints *h = XAllocSizeHints();
        if (h) {
            h->flags = PMinSize | PMaxSize;
            h->min_width = h->max_width = WIDTH;
            h->min_height = h->max_height = HEIGHT;
            XSetWMNormalHints(dpy, w, h);
            XFree(h);
        }
    }
    XMapWindow(dpy, w);
    XSync(dpy, False);   /* the map is on the server before the first frame is drawn into it */

    /* 3.3 compatibility profile if we can, then core 3.3, then whatever GLX
     * gives us. Xwayland under gamescope answers an unsupported profile with a
     * GLXBadFBConfig protocol error, which Xlib's default handler turns into
     * exit(); x_error_handler() above keeps it a return value we can fall back
     * from, which is exactly what a screenshot on a phone has to distinguish
     * from a black frame. */
    ctx = NULL;
    if (has_glx_ext("GLX_ARB_create_context")) {
        void *p = glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
        if (p) {
            static const int profiles[] = {
                GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
                GLX_CONTEXT_CORE_PROFILE_BIT_ARB
            };
            GLXContext (*create)(Display *, GLXFBConfig, GLXContext, Bool, const int *) = p;
            int pi;
            for (pi = 0; pi < 2 && !ctx; pi++) {
                int cattrs[] = {
                    GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
                    GLX_CONTEXT_MINOR_VERSION_ARB, 3,
                    GLX_CONTEXT_PROFILE_MASK_ARB, profiles[pi],
                    0
                };
                ctx = create(dpy, out->cfg, share, True, cattrs);
                XSync(dpy, False);
                printf("context=%s3.3 %s x_error=%d\n",
                       pi == 0 ? "compat" : "core", ctx ? "ok" : "FAILED",
                       take_x_error());
            }
        }
    }
    if (!ctx) {
        ctx = glXCreateContext(dpy, vi, share, True);
        XSync(dpy, False);
        printf("context=legacy %s x_error=%d\n", ctx ? "ok" : "FAILED", take_x_error());
    }
    if (!ctx) { fprintf(stderr, "fencesync: no GLX context\n"); return 0; }

    out->w = w;
    out->ctx = ctx;
    out->w_px = WIDTH;
    out->h_px = HEIGHT;
    out->srgb_capable = srgb;
    XSync(dpy, False);
    return 1;
}

/* ---- shader / triangle ------------------------------------------------- */

/* ---- fencesync: the fence loop ---------------------------------------- */

#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT    0x00000001
#define GL_ALREADY_SIGNALED           0x911A
#define GL_TIMEOUT_EXPIRED            0x911B
#define GL_CONDITION_SATISFIED        0x911C
#define GL_WAIT_FAILED                0x911D

static const char *fencename(GLenum e)
{
    switch (e) {
    case GL_ALREADY_SIGNALED:     return "ALREADY_SIGNALED";
    case GL_TIMEOUT_EXPIRED:      return "TIMEOUT_EXPIRED";
    case GL_CONDITION_SATISFIED:  return "CONDITION_SATISFIED";
    case GL_WAIT_FAILED:          return "WAIT_FAILED";
    default:                      return "?";
    }
}

/* one frame: clear, a fixed-function triangle, no shader -- the fence is the
 * subject, the draw only makes the deferred flush carry work */
static void draw_frame(int i)
{
    float c = (i & 1) ? 0.45f : 0.20f;
    glClearColor(0.10f, c, 0.35f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glBegin(GL_TRIANGLES);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glVertex2f(-0.6f, -0.6f);
    glVertex2f(0.6f, -0.6f);
    glVertex2f(0.0f, 0.6f);
    glEnd();
}

/* variant A: GL_ARB_sync, what ANGLE's SyncGL uses */
static int run_gl_fences(int iters, unsigned long long timeout_ns)
{
    int nulls = 0, errs = 0, timeouts = 0, waitfail = 0, signaled = 0;
    char err[64];
    int i;

    for (i = 1; i <= iters; i++) {
        GLsync s;
        GLenum e, r;

        draw_frame(i);
        glFlush();
        e = glGetError();
        if (e != GL_NO_ERROR) {
            errs++;
            printf("FSYNC iter=%d stage=draw err=0x%04x %s\n", i, e, errname(e));
        }

        s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!s) {
            nulls++;
            e = glGetError();
            printf("FSYNC iter=%d stage=glFenceSync SYNC=NULL err=0x%04x %s\n",
                   i, e, errname(e));
            if (nulls >= 5) { printf("FSYNC stop: 5 NULL syncs\n"); break; }
            continue;
        }
        glFlush();
        e = glGetError();
        if (e != GL_NO_ERROR) {
            errs++;
            printf("FSYNC iter=%d stage=flush err=0x%04x %s\n", i, e, errname(e));
        }

        r = glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, timeout_ns);
        switch (r) {
        case GL_ALREADY_SIGNALED:
        case GL_CONDITION_SATISFIED: signaled++; break;
        case GL_TIMEOUT_EXPIRED:
            timeouts++;
            printf("FSYNC iter=%d stage=glClientWaitSync %s\n", i, fencename(r));
            break;
        case GL_WAIT_FAILED:
            waitfail++;
            e = glGetError();
            printf("FSYNC iter=%d stage=glClientWaitSync WAIT_FAILED err=0x%04x %s\n",
                   i, e, errname(e));
            break;
        default:
            errs++;
            printf("FSYNC iter=%d stage=glClientWaitSync r=0x%04x %s\n", i, r, fencename(r));
        }
        if (waitfail >= 5) { printf("FSYNC stop: 5 WAIT_FAILED\n"); break; }

        glDeleteSync(s);
        e = glGetError();
        if (e != GL_NO_ERROR) {
            errs++;
            printf("FSYNC iter=%d stage=glDeleteSync err=0x%04x %s\n", i, e, errname(e));
        }
        if ((i % 100) == 0) {
            drain_errors(err, sizeof err);
            printf("FSYNC progress iter=%d signaled=%d timeouts=%d nulls=%d errs=%d drain=%s\n",
                   i, signaled, timeouts, nulls, errs, err);
        }
    }
    drain_errors(err, sizeof err);
    printf("FSYNC SUMMARY variant=gl iters=%d signaled=%d timeouts=%d nulls=%d waitfail=%d errs=%d drain=%s\n",
           i - 1, signaled, timeouts, nulls, waitfail, errs, err);
    return (nulls || waitfail || errs) ? 1 : 0;
}

/* variant B: EGL_KHR_fence_sync (and its native-fence siblings), which is what
 * Chromium's GPU process creates around a composite -- no GL sync object */
#define EGL_NONE                0x3038
#define EGL_SURFACE_TYPE        0x3033
#define EGL_PBUFFER_BIT         0x0001
#define EGL_RENDERABLE_TYPE     0x3040
#define EGL_OPENGL_BIT          0x0008
#define EGL_RED_SIZE            0x3024
#define EGL_GREEN_SIZE          0x3023
#define EGL_BLUE_SIZE           0x3022
#define EGL_ALPHA_SIZE          0x3021
#define EGL_WIDTH               0x3057
#define EGL_HEIGHT              0x3056
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_OPENGL_API          0x30A2
#define EGL_SYNC_FENCE_KHR            0x30F9
#define EGL_SYNC_FLUSH_COMMANDS_BIT_KHR 0x0001
#define EGL_CONDITION_SATISFIED_KHR   0x30F6
#define EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR 0x30F0

typedef void *EGLDisplay, *EGLContext, *EGLSurface, *EGLSyncKHR, *EGLConfig;
typedef int EGLint;
typedef unsigned int EGLBoolean, EGLenum;

#define EGL_FUNCS(X)                                                                  \
    X(EGLDisplay, eglGetDisplay,          (void *))                                   \
    X(EGLBoolean, eglInitialize,          (EGLDisplay, EGLint *, EGLint *))           \
    X(EGLBoolean, eglChooseConfig,        (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *)) \
    X(EGLBoolean, eglBindAPI,             (EGLenum))                                  \
    X(EGLContext, eglCreateContext,       (EGLDisplay, EGLConfig, EGLContext, const EGLint *)) \
    X(EGLSurface, eglCreatePbufferSurface,(EGLDisplay, EGLConfig, const EGLint *))    \
    X(EGLBoolean, eglMakeCurrent,         (EGLDisplay, EGLSurface, EGLSurface, EGLContext)) \
    X(const char *, eglQueryString,       (EGLDisplay, EGLint))                       \
    X(EGLint,     eglGetError,            (void))

#define EGLX_FUNCS(X)                                                                 \
    X(EGLSyncKHR, eglCreateSyncKHR,       (EGLDisplay, EGLenum, const void *))        \
    X(EGLint,     eglClientWaitSyncKHR,   (EGLDisplay, EGLSyncKHR, EGLint, unsigned long long)) \
    X(EGLBoolean, eglDestroySyncKHR,      (EGLDisplay, EGLSyncKHR))

#define DECL2(ret, name, args) static ret (*name) args;
EGL_FUNCS(DECL2)
EGLX_FUNCS(DECL2)
#undef DECL2

static int run_egl_fences(int iters, unsigned long long timeout_ns)
{
    void *libegl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    EGLDisplay edpy;
    EGLConfig cfg;
    EGLint n = 0, major = 0, minor = 0;
    EGLSurface surf;
    EGLContext ectx;
    int nulls = 0, errs = 0, timeouts = 0, i, ok;
    char err[64];
    static const EGLint cattrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    static const EGLint sattrs[] = { EGL_WIDTH, 320, EGL_HEIGHT, 240, EGL_NONE };
    static const EGLint xattrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE
    };
    void *(*gpa)(const char *) = NULL;

    if (!libegl) {
        printf("FSYNC variant=egl SKIP: cannot dlopen libEGL.so.1: %s\n", dlerror());
        return 0;
    }
    gpa = (void *(*)(const char *))dlsym(libegl, "eglGetProcAddress");
#define LOAD2(ret, name, args)                                                   \
    do {                                                                         \
        void *p_ = dlsym(libegl, #name);                                          \
        if (!p_ && gpa) p_ = gpa(#name);                                          \
        *(void **)(&name) = p_;                                                   \
        if (!p_) { printf("FSYNC variant=egl SKIP: no %s\n", #name); return 0; }    \
    } while (0);
    EGL_FUNCS(LOAD2)
    EGLX_FUNCS(LOAD2)
#undef LOAD2

    edpy = eglGetDisplay(NULL);
    if (edpy == NULL || !eglInitialize(edpy, &major, &minor)) {
        printf("FSYNC variant=egl SKIP: eglInitialize failed (0x%x)\n",
               eglGetError ? eglGetError() : -1);
        return 0;
    }
    printf("FSYNC variant=egl EGL %d.%d exts=\"%s\"\n", major, minor,
           eglQueryString(edpy, 0x3055) ? eglQueryString(edpy, 0x3055) : "(none)");
    if (!eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(edpy, cattrs, &cfg, 1, &n) || n < 1) {
        printf("FSYNC variant=egl SKIP: no pbuffer config (n=%d err=0x%x)\n", n, eglGetError());
        return 0;
    }
    surf = eglCreatePbufferSurface(edpy, cfg, sattrs);
    if (!surf) printf("FSYNC variant=egl pbuffer surface failed (err=0x%x)\n", eglGetError());
    ectx = surf ? eglCreateContext(edpy, cfg, NULL, xattrs) : NULL;
    if (!ectx && surf) {
        printf("FSYNC variant=egl context(3.0) failed (err=0x%x), retrying default\n", eglGetError());
        ectx = eglCreateContext(edpy, cfg, NULL, NULL);
    }
    if (!ectx || !eglMakeCurrent(edpy, surf, surf, ectx)) {
        printf("FSYNC variant=egl SKIP: context/surface failed (err=0x%x)\n", eglGetError());
        return 0;
    }
    printf("FSYNC variant=egl renderer=\"%s\"\n",
           glGetString && glGetString(GL_RENDERER) ? (const char *)glGetString(GL_RENDERER) : "?");
    while (glGetError() != GL_NO_ERROR) { }

    ok = 0;
    for (i = 1; i <= iters; i++) {
        EGLSyncKHR s;
        EGLint r;
        draw_frame(i);
        glFlush();
        s = eglCreateSyncKHR(edpy, EGL_SYNC_FENCE_KHR, NULL);
        if (s == NULL) {
            nulls++;
            printf("FSYNC iter=%d stage=eglCreateSyncKHR SYNC=NULL err=0x%x\n", i, eglGetError());
            if (nulls >= 5) break;
            continue;
        }
        r = eglClientWaitSyncKHR(edpy, s, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, timeout_ns);
        if (r == 0x30F5 /* EGL_TIMEOUT_EXPIRED_KHR */) {
            timeouts++;
            printf("FSYNC iter=%d stage=eglClientWaitSyncKHR TIMEOUT\n", i);
        } else if (r == 0x30F7 /* EGL_ALREADY_SIGNALED_KHR */ ||
                   r == EGL_CONDITION_SATISFIED_KHR) {
            ok++;
        } else {
            errs++;
            printf("FSYNC iter=%d stage=eglClientWaitSyncKHR r=0x%x err=0x%x\n", i, r, eglGetError());
        }
        eglDestroySyncKHR(edpy, s);
        if ((i % 100) == 0) printf("FSYNC progress iter=%d ok=%d timeouts=%d nulls=%d errs=%d\n",
                                   i, ok, timeouts, nulls, errs);
    }
    drain_errors(err, sizeof err);
    printf("FSYNC SUMMARY variant=egl iters=%d ok=%d timeouts=%d nulls=%d errs=%d drain=%s\n",
           i - 1, ok, timeouts, nulls, errs, err);
    return (nulls || errs) ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *v = getenv("FENCE_VARIANT"), *it = getenv("FENCE_ITERS");
    const char *to = getenv("FENCE_TIMEOUT_MS");
    int iters = it && *it ? atoi(it) : 1000;
    unsigned long long timeout_ns = (unsigned long long)(to && *to ? atoi(to) : 100) * 1000000ull;
    int egl_variant = v && (!strcmp(v, "egl") || !strcmp(v, "b"));
    struct win w1;
    GLint major = 0, minor = 0;
    char err[64];
    int rc;

    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "fencesync: cannot open display %s\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 2;
    }
    scr = DefaultScreen(dpy);
    XSetErrorHandler(x_error_handler);
    if (!load_gl()) return 2;
    if (!glFenceSync || !glClientWaitSync || !glDeleteSync) {
        printf("FSYNC REFUSED: libGL has no glFenceSync/glClientWaitSync/glDeleteSync\n");
        return 2;
    }
    printf("FSYNC variant=%s iters=%d timeout_ms=%llu libGL=loaded missing_symbols=%d\n",
           egl_variant ? "egl" : "gl", iters, timeout_ns / 1000000ull, missing_syms);

    if (egl_variant) {
        /* variant B runs on its own EGL context: a GLX context current on the
         * same thread first is what EGL answers with EGL_BAD_ACCESS */
        rc = run_egl_fences(iters, timeout_ns);
        drain_errors(err, sizeof err);
        printf("FSYNC end rc=%d trailing_gl_errors=%s\n", rc, err);
        return rc;
    }
    if (!open_window(&w1, "fencesync window", NULL, 0)) return 2;
    if (!glXMakeCurrent(dpy, w1.w, w1.ctx)) {
        fprintf(stderr, "fencesync: glXMakeCurrent failed\n");
        return 2;
    }
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    while (glGetError() != GL_NO_ERROR) { }
    printf("FSYNC vendor=\"%s\" renderer=\"%s\" gl=%d.%d version=\"%s\"\n",
           (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
           major, minor, (const char *)glGetString(GL_VERSION));
    glDrawBuffer(GL_BACK);

    rc = egl_variant ? run_egl_fences(iters, timeout_ns)
                     : run_gl_fences(iters, timeout_ns);
    glFinish();
    drain_errors(err, sizeof err);
    printf("FSYNC end rc=%d trailing_gl_errors=%s\n", rc, err);
    return rc;
}
