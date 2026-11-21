/*
 * gl-ladder.c — a minimal imitation of what wined3d does for one present.
 *
 * wined3d's present path for a D3D11 game under Proton is:
 *   swapchain_blit()          src/wined3d/swapchain.c:487
 *     -> glBlitFramebuffer()  src/wined3d/texture_gl.c:883
 *     -> p_wglSwapBuffers()   src/wined3d/swapchain.c:657
 * i.e. the game renders into an offscreen FBO, wined3d blits that FBO into the
 * window's back buffer and then swaps. This program does only that, nothing
 * else, so that a black window can be attributed to the path or to wined3d's
 * state (formats, attachments, GL version it picks).
 *
 * Steps (LADDER_STEP, default 1):
 *   0  no FBO at all: glClearColor + glClear straight into the window's back
 *      buffer, readback of back, swap, readback of front (the two controls)
 *   1  clear an RGBA8 FBO texture to a known colour, blit it to the window, swap
 *   2  step 1 + draw a triangle into the FBO with a simple GLSL shader
 *      (fixed-function fallback if GLSL fails), blit, swap
 *   3  step 2 + glEnable(GL_FRAMEBUFFER_SRGB) with an SRGB8_ALPHA8 FBO
 *   4  step 3, but the blit goes into the back buffer of a SECOND window
 *   5  step 1, but the FBO texture is drawn into the back buffer as a textured
 *      quad instead of glBlitFramebuffer (splits "blit" from "swapchain")
 *      (a shared context, like a second back buffer)
 *
 * Each stage prints one line: FBO completeness, GL error, what the window shows.
 * Three readbacks per run localise a black window: readback=fbo (the texture
 * itself, before the blit), readback=back (after the blit, before the swap) and
 * readback=front (after the swap).
 * After every blit it reads the window's back buffer back with glReadPixels and
 * prints its mean and a few pixels, so "the window is black" is a number and not
 * an opinion. LADDER_DUMP=file.ppm also saves that readback.
 *
 * No GL headers and no libGL at link time: the GL/GLX entry points are loaded at
 * runtime from libGL.so.1 through glXGetProcAddress, the way any loader does it.
 * That is also what lets this file cross-build against the guest sysroot, which
 * ships X11 but no GL.
 *
 * Build:  gcc -O1 -o gl-ladder gl-ladder.c -lX11 -ldl          (host)
 *         tools/gl-ladder.sh --cross                           (aarch64, phone)
 * Run:    LADDER_STEP=n ./gl-ladder
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
    X(GLint,         glGetUniformLocation,  (GLuint, const GLchar *))

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
        fprintf(stderr, "gl-ladder: cannot dlopen libGL.so.1: %s\n", dlerror());
        return 0;
    }
#define LOAD1(ret, name, args)                                                   \
    do {                                                                         \
        void *p_ = dlsym(libgl, #name);                                          \
        if (!p_) p_ = gsym(#name);                                               \
        *(void **)(&name) = p_;                                                   \
        if (!p_) { fprintf(stderr, "gl-ladder: no %s\n", #name); missing_syms++; }\
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
    if (!out->cfg) { fprintf(stderr, "gl-ladder: no usable FBConfig\n"); return 0; }

    vi = glXGetVisualFromFBConfig(dpy, out->cfg);
    if (!vi) { fprintf(stderr, "gl-ladder: no XVisualInfo\n"); return 0; }
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
    if (!w) { fprintf(stderr, "gl-ladder: XCreateWindow failed\n"); return 0; }
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
    if (!ctx) { fprintf(stderr, "gl-ladder: no GLX context\n"); return 0; }

    out->w = w;
    out->ctx = ctx;
    out->w_px = WIDTH;
    out->h_px = HEIGHT;
    out->srgb_capable = srgb;
    XSync(dpy, False);
    return 1;
}

/* ---- shader / triangle ------------------------------------------------- */

static GLuint prog;
static int shader_ok;

static GLuint mk_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        log[0] = '\0';
        glGetShaderInfoLog(s, sizeof log - 1, NULL, log);
        fprintf(stderr, "gl-ladder: shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int build_triangle(void)
{
    static const char *vs =
        "#version 120\n"
        "attribute vec2 pos;\n"
        "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char *fs =
        "#version 120\n"
        "void main() { gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0); }\n";
    static const float verts[] = {
        -0.75f, -0.6f,   0.75f, -0.6f,   0.0f, 0.7f
    };
    GLuint v = mk_shader(GL_VERTEX_SHADER, vs);
    GLuint f = v ? mk_shader(GL_FRAGMENT_SHADER, fs) : 0;
    if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }

    prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    {
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            log[0] = '\0';
            glGetProgramInfoLog(prog, sizeof log - 1, NULL, log);
            fprintf(stderr, "gl-ladder: program link failed: %s\n", log);
            glDeleteProgram(prog);
            prog = 0;
            return 0;
        }
    }
    /* client-side vertex array (no VAO, no VBO: works on compat and on GLSL-less) */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
    while (glGetError() != GL_NO_ERROR) { }
    return 1;
}

static void draw_triangle(void)
{
    static const float verts[] = {
        -0.75f, -0.6f,   0.75f, -0.6f,   0.0f, 0.7f
    };
    if (shader_ok) {
        glUseProgram(prog);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUseProgram(0);
    } else {
        /* fixed-function fallback: same triangle, same colour */
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_TRIANGLES);
        glVertex2f(-0.75f, -0.6f);
        glVertex2f(0.75f, -0.6f);
        glVertex2f(0.0f, 0.7f);
        glEnd();
    }
}

/* ---- step 5: the same FBO, but sampled into the back buffer as a quad --
 * No glBlitFramebuffer anywhere: if this paints and step 1 does not, the loss
 * is in the blit and not in how the FBO/swapchain images are allocated. */
static GLuint qprog, qtex_uniform;
static int qshader_ok;

static int build_quad(void)
{
    static const char *vs =
        "#version 120\n"
        "attribute vec2 pos;\n"
        "attribute vec2 uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char *fs =
        "#version 120\n"
        "uniform sampler2D tex;\n"
        "varying vec2 v_uv;\n"
        "void main() { gl_FragColor = texture2D(tex, v_uv); }\n";
    GLuint v = mk_shader(GL_VERTEX_SHADER, vs);
    GLuint f = v ? mk_shader(GL_FRAGMENT_SHADER, fs) : 0;
    GLint ok = 0;
    if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }
    qprog = glCreateProgram();
    glAttachShader(qprog, v);
    glAttachShader(qprog, f);
    glBindAttribLocation(qprog, 0, "pos");
    glBindAttribLocation(qprog, 1, "uv");
    glLinkProgram(qprog);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(qprog, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(qprog); qprog = 0; return 0; }
    qtex_uniform = glGetUniformLocation(qprog, "tex");
    return 1;
}

static void draw_quad(GLuint tex, int wpix, int hpix)
{
    /* two triangles in clip space, uv 0..1 (texture origin bottom-left) */
    static const GLfloat verts[] = {
        -1.f, -1.f, 0.f, 0.f,   1.f, -1.f, 1.f, 0.f,   1.f, 1.f, 1.f, 1.f,
        -1.f, -1.f, 0.f, 0.f,   1.f,  1.f, 1.f, 1.f,  -1.f, 1.f, 0.f, 1.f
    };
    glViewport(0, 0, wpix, hpix);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (qshader_ok) {
        glUseProgram(qprog);
        glUniform1i(qtex_uniform, 0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                              verts + 2);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glUseProgram(0);
    } else {
        glEnable(GL_TEXTURE_2D);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glColor4f(1.f, 1.f, 1.f, 1.f);
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(2, GL_FLOAT, 4 * sizeof(GLfloat), verts);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 4 * sizeof(GLfloat), verts + 2);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisable(GL_TEXTURE_2D);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* ---- FBO -------------------------------------------------------------- */

struct fbo {
    GLuint fbo, tex;
    GLenum internal;
};

static int make_fbo(struct fbo *f, GLenum internal)
{
    f->internal = internal;
    glGenTextures(1, &f->tex);
    glBindTexture(GL_TEXTURE_2D, f->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, FBO_W, FBO_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &f->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, f->tex, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, FBO_W, FBO_H);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

/* ---- readback: what the window's back buffer really holds -------------- */

static unsigned char *readbacks[2];
static int rb_w, rb_h;

static void readback(struct win *win, const char *dump, GLenum sel, const char *label)
{
    size_t sz = (size_t)win->w_px * win->h_px * 3;
    unsigned char *px = readbacks[sel == GL_FRONT ? 1 : 0];
    double sum = 0.0;
    size_t i;
    int x, y;
    char err[64];

    glFlush();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadBuffer(sel);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, win->w_px, win->h_px, GL_RGB, GL_UNSIGNED_BYTE, px);
    drain_errors(err, sizeof err);   /* GL_FRONT after a swap may be refused; say so, do not fail */
    for (i = 0; i < sz; i++) sum += px[i];
    printf("%s readback=%-5s mean=%.1f/255 pixel_mid=(%d,%d,%d) pixel_low=(%d,%d,%d) err=%s\n",
           label, sel == GL_FRONT ? "front" : "back", sum / (double)sz,
           px[((win->h_px / 2) * win->w_px + win->w_px / 2) * 3 + 0],
           px[((win->h_px / 2) * win->w_px + win->w_px / 2) * 3 + 1],
           px[((win->h_px / 2) * win->w_px + win->w_px / 2) * 3 + 2],
           px[((win->h_px / 5) * win->w_px + win->w_px / 2) * 3 + 0],
           px[((win->h_px / 5) * win->w_px + win->w_px / 2) * 3 + 1],
           px[((win->h_px / 5) * win->w_px + win->w_px / 2) * 3 + 2], err);
    if (dump && *dump) {
        FILE *fp = fopen(dump, "wb");
        if (!fp) {
            fprintf(stderr, "gl-ladder: cannot write %s\n", dump);
        } else {
            fprintf(fp, "P6\n%d %d\n255\n", win->w_px, win->h_px);
            for (y = win->h_px - 1; y >= 0; y--)
                for (x = 0; x < win->w_px * 3; x++)
                    fputc(px[y * win->w_px * 3 + x], fp);
            fclose(fp);
            printf("%s dump=%s\n", label, dump);
        }
    }
    (void)rb_w; (void)rb_h;
}

/* ---- readback of the FBO texture itself (did the draw land at all?) ----- */

static void readback_fbo(GLuint fbo, const char *label)
{
    size_t sz = (size_t)FBO_W * FBO_H * 3;
    unsigned char *px = readbacks[1];
    double sum = 0.0;
    size_t i;
    char err[64];

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glFinish();
    glReadPixels(0, 0, FBO_W, FBO_H, GL_RGB, GL_UNSIGNED_BYTE, px);
    drain_errors(err, sizeof err);
    for (i = 0; i < sz; i++) sum += px[i];
    printf("%s readback=fbo   mean=%.1f/255 pixel_mid=(%d,%d,%d) pixel_low=(%d,%d,%d) err=%s\n",
           label, sum / (double)sz,
           px[((FBO_H / 2) * FBO_W + FBO_W / 2) * 3 + 0],
           px[((FBO_H / 2) * FBO_W + FBO_W / 2) * 3 + 1],
           px[((FBO_H / 2) * FBO_W + FBO_W / 2) * 3 + 2],
           px[((FBO_H / 5) * FBO_W + FBO_W / 2) * 3 + 0],
           px[((FBO_H / 5) * FBO_W + FBO_W / 2) * 3 + 1],
           px[((FBO_H / 5) * FBO_W + FBO_W / 2) * 3 + 2], err);
}

/* ---- what stays on screen: a redraw and a swap every frame ---------------- */
/* glxgears redraws and swaps for as long as it runs. The ladder used to swap once and then sit in an event
 * loop that read and dropped every Expose: a window that never redraws is not what a game does, and a
 * compositor that asks for an Expose answer (or that only keeps presenting windows in its scene) never gets
 * one. So "the ladder window is invisible while glxgears is visible" (PROVEN on the CMF, same build and
 * session) said nothing about the driver until the window behaved like glxgears. The hold below re-issues
 * the step's own commands without readbacks or prints, and swaps, exactly like glxgears' main loop. */
static int g_step, g_srgb;
static struct fbo *g_fbo;
static struct win *g_win, *g_win1;

static void
redraw_frame(void)
{
    glXMakeCurrent(dpy, g_win->w, g_win->ctx);
    if (g_step == 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
        glClearColor(0.90f, 0.10f, 0.60f, 1.0f);   /* the step 0 magenta */
        glClear(GL_COLOR_BUFFER_BIT);
        glXSwapBuffers(dpy, g_win->w);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo->fbo);
    if (g_step == 1 || g_step == 5) {
        glClearColor(0.10f, 0.45f, 0.90f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        if (g_srgb) glEnable(GL_FRAMEBUFFER_SRGB);
        glClearColor(0.10f, 0.20f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_triangle();
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDrawBuffer(GL_BACK);
    glBlitFramebuffer(0, 0, FBO_W, FBO_H, 0, 0, g_win->w_px, g_win->h_px,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glXSwapBuffers(dpy, g_win->w);
    if (g_win1) {   /* step 4: the first window keeps its own grey, swapped too */
        glXMakeCurrent(dpy, g_win1->w, g_win1->ctx);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
        glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glXSwapBuffers(dpy, g_win1->w);
    }
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int step = 1;
    const char *s = getenv("LADDER_STEP");
    const char *dump = getenv("LADDER_DUMP");
    struct win w1, w2;
    int use_srgb_fbo, use_second_window;
    struct fbo f;
    char err[64];
    GLint major = 0, minor = 0;

    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (s && *s) step = atoi(s);
    if (step < 0 || step > 5) {
        fprintf(stderr, "gl-ladder: LADDER_STEP must be 0..5 (got %s)\n", s ? s : "");
        return 2;
    }
    use_srgb_fbo = (step >= 3 && step <= 4);   /* step 5 is step 1 with a quad */
    use_second_window = step == 4;
    memset(&f, 0, sizeof f);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "gl-ladder: cannot open display %s\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 2;
    }
    scr = DefaultScreen(dpy);
    XSetErrorHandler(x_error_handler);
    if (!load_gl()) return 2;
    if (!glGenFramebuffers || !glBlitFramebuffer) {
        fprintf(stderr, "gl-ladder: no glGenFramebuffers/glBlitFramebuffer in libGL\n");
        return 2;
    }
    printf("step=%d libGL=%s missing_symbols=%d\n", step,
           libgl ? "loaded" : "no", missing_syms);

    if (!open_window(&w1, "gl-ladder window 1", NULL, use_srgb_fbo)) return 2;
    if (!glXMakeCurrent(dpy, w1.w, w1.ctx)) {
        fprintf(stderr, "gl-ladder: glXMakeCurrent failed\n");
        return 2;
    }
    if (use_second_window) {
        if (!open_window(&w2, "gl-ladder window 2", w1.ctx, use_srgb_fbo)) return 2;
        glXMakeCurrent(dpy, w1.w, w1.ctx);
    }

    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    while (glGetError() != GL_NO_ERROR) { }
    printf("step=%d vendor=\"%s\" renderer=\"%s\" gl=%d.%d version=\"%s\" "
           "fbconfig_srgb=%d glx_exts_srgb=%d\n",
           step, (const char *)glGetString(GL_VENDOR),
           (const char *)glGetString(GL_RENDERER), major, minor,
           (const char *)glGetString(GL_VERSION), w1.srgb_capable,
           has_glx_ext("GLX_ARB_framebuffer_sRGB"));

    shader_ok = (step >= 2) ? build_triangle() : 0;
    if (step >= 2)
        printf("step=%d triangle=%s\n", step, shader_ok ? "glsl" : "fixed-function");

    readbacks[0] = malloc((size_t)WIDTH * HEIGHT * 3);
    readbacks[1] = malloc((size_t)WIDTH * HEIGHT * 3);
    if (!readbacks[0] || !readbacks[1]) { fprintf(stderr, "gl-ladder: no memory\n"); return 2; }

    if (step >= 1) {
        if (!make_fbo(&f, use_srgb_fbo ? GL_SRGB8_ALPHA8 : GL_RGBA8)) {
            printf("step=%d fbo=%s complete=%s -> STOP\n", step,
                   use_srgb_fbo ? "SRGB8_ALPHA8" : "RGBA8", completeness());
            return 1;
        }

        /* --- 1: render offscreen --------------------------------------- */
        if (step == 1 || step == 5) {
            glClearColor(0.10f, 0.45f, 0.90f, 1.0f);       /* blue */
            glClear(GL_COLOR_BUFFER_BIT);
        } else {
            glClearColor(0.10f, 0.20f, 0.10f, 1.0f);       /* dark green */
            glClear(GL_COLOR_BUFFER_BIT);
            draw_triangle();
        }
        drain_errors(err, sizeof err);
        printf("step=%d fbo=%s complete=%s srgb=%s draw=%s err=%s\n",
               step, use_srgb_fbo ? "SRGB8_ALPHA8" : "RGBA8", completeness(),
               glIsEnabled(GL_FRAMEBUFFER_SRGB) ? "on" : "off",
               (step == 1 || step == 5) ? "clear" : "triangle", err);

        if (step >= 3) {
            glEnable(GL_FRAMEBUFFER_SRGB);
            /* re-render so the sRGB encode is actually applied on the FBO write */
            glClearColor(0.10f, 0.20f, 0.10f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            draw_triangle();
            glFinish();
            drain_errors(err, sizeof err);
            printf("step=%d framebuffer_srgb=on err=%s\n", step, err);
        }

        /* H1: did the clear/draw land in the texture? read the FBO itself */
        readback_fbo(f.fbo, "preblit");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /* --- 0: no FBO at all: clear the back buffer directly (control) ----- */
    if (step == 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
        glClearColor(0.90f, 0.10f, 0.60f, 1.0f);   /* magenta, distinguishable from every step */
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        drain_errors(err, sizeof err);
        printf("step=0 fbo=none target=window1 direct_clear=magenta(0.90,0.10,0.60) err=%s\n", err);
        readback(&w1, dump, GL_BACK, "direct");
        glXSwapBuffers(dpy, w1.w);
        glFinish();
        drain_errors(err, sizeof err);
        printf("step=0 swap=ok err=%s window_shows=magenta (0.90,0.10,0.60)\n", err);
        readback(&w1, NULL, GL_FRONT, "after");
    }

    /* --- 2: blit into the (last) window's back buffer ------------------- */
    if (step >= 1) {
        struct win *target = use_second_window ? &w2 : &w1;
        if (use_second_window) {
            if (!glXMakeCurrent(dpy, w2.w, w2.ctx)) {
                fprintf(stderr, "gl-ladder: current window 2 failed\n");
                return 2;
            }
            printf("step=%d target=window2 shared_context=yes\n", step);
        } else {
            printf("step=%d target=window1\n", step);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, f.fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
        if (step == 5) {
            /* no blit at all: sample the FBO texture into the back buffer */
            qshader_ok = build_quad();
            {   /* FRAGSCTRL: the same query around the step that PAINTS (the control) */
                GLuint q = 0;
                int have_q = glGenQueries && glBeginQuery && glEndQuery && glGetQueryObjectuiv;
                if (have_q) { glGenQueries(1, &q); glBeginQuery(GL_SAMPLES_PASSED, q); }
                draw_quad(f.tex, target->w_px, target->h_px);
                glFinish();
                if (have_q) {
                    GLuint nf = 0xffffffffu;
                    unsigned char px[8 * 8 * 4] = {0};
                    unsigned m[3] = {0, 0, 0};
                    glEndQuery(GL_SAMPLES_PASSED);
                    glGetQueryObjectuiv(q, GL_QUERY_RESULT, &nf);
                    glReadPixels(target->w_px / 2 - 4, target->h_px / 2 - 4, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    for (int i = 0; i < 64; i++) { m[0] += px[i * 4]; m[1] += px[i * 4 + 1]; m[2] += px[i * 4 + 2]; }
                    printf("FRAGSCTRL step=5 n=%u mean=%u,%u,%u\n", nf, m[0] / 64, m[1] / 64, m[2] / 64);
                } else {
                    printf("FRAGSCTRL step=5 n=no-query\n");
                }
            }
            drain_errors(err, sizeof err);
            printf("step=%d quad=textured full-window quad from the FBO texture "
                   "shader=%s err=%s\n", step, qshader_ok ? "glsl" : "fixed-function", err);
            readback(target, dump, GL_BACK, "quad");
        } else {
        /* RTDBG: how many fragments does the blit actually produce? GL_SAMPLES_PASSED (our occlusion query) is
         * the decisive number: a sampling FS over a 100 %-nonzero source cannot write 0,0,0 if it runs. */
        {
            GLuint q = 0;
            int have_q = glGenQueries && glBeginQuery && glEndQuery && glGetQueryObjectuiv;
            if (have_q) { glGenQueries(1, &q); glBeginQuery(GL_SAMPLES_PASSED, q); }
            glBlitFramebuffer(0, 0, FBO_W, FBO_H, 0, 0, target->w_px, target->h_px,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glFinish();
            if (have_q) {
                GLuint nf = 0xffffffffu;
                glEndQuery(GL_SAMPLES_PASSED);
                glGetQueryObjectuiv(q, GL_QUERY_RESULT, &nf);
                unsigned char px[8 * 8 * 4] = {0};
                unsigned m[3] = {0, 0, 0};
                glReadPixels(target->w_px / 2 - 4, target->h_px / 2 - 4, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px);
                for (int i = 0; i < 64; i++) { m[0] += px[i * 4]; m[1] += px[i * 4 + 1]; m[2] += px[i * 4 + 2]; }
                printf("BLITFRAGS n=%u mean=%u,%u,%u\n", nf, m[0] / 64, m[1] / 64, m[2] / 64);
            } else {
                printf("BLITFRAGS n=no-query mean=no-query\n");
            }
        }
        drain_errors(err, sizeof err);
        printf("step=%d blit=glBlitFramebuffer(0,0,%d,%d -> 0,0,%d,%d,COLOR,NEAREST) "
               "err=%s\n", step, FBO_W, FBO_H, target->w_px, target->h_px, err);

        readback(target, dump, GL_BACK, "blit");
        }

        glXSwapBuffers(dpy, target->w);
        glFinish();
        drain_errors(err, sizeof err);
        printf("step=%d swap=ok err=%s window_shows=%s\n", step, err,
               step == 1 ? "blue (0.10,0.45,0.90)"
                         : (step == 2 ? "dark green + white triangle"
                                      : "dark green + white triangle, sRGB-encoded"));
        /* H3: is it only the readback of the window, or the present itself? */
        readback(target, NULL, GL_FRONT, "after");
    }

    if (use_second_window) {
        /* window 1 gets a different colour: proves the two windows are separate */
        glXMakeCurrent(dpy, w1.w, w1.ctx);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        glXSwapBuffers(dpy, w1.w);
        drain_errors(err, sizeof err);
        printf("step=%d window1_shows=grey 0.5 (second buffer untouched by the blit) err=%s\n",
               step, err);
    }

    /* handshake for the screenshot: the window is up and swapped, so a watcher
     * on the host can stop guessing how long the guest takes to boot */
    {
        const char *rf = getenv("LADDER_READY");
        if (rf && *rf) {
            FILE *fp = fopen(rf, "w");
            if (fp) { fprintf(fp, "step=%d\n", step); fclose(fp); }
            printf("ready=%s\n", rf);
        }
    }

    /* keep the window on screen long enough for a screenshot, redrawing and swapping every frame the way
     * glxgears' main loop does; a key press (or the end of the hold) stops it */
    {
        const char *hold = getenv("LADDER_HOLD");
        int ms = hold && *hold ? atoi(hold) : 4000;
        int i, stop = 0;
        XEvent ev;
        g_step = step;
        g_srgb = use_srgb_fbo;
        g_fbo = step >= 1 ? &f : NULL;
        g_win = use_second_window ? &w2 : &w1;
        g_win1 = use_second_window ? &w1 : NULL;
        for (i = 0; i < ms / 20 && !stop; i++) {
            while (XPending(dpy)) {
                XNextEvent(dpy, &ev);
                if (ev.type == KeyPress)
                    stop = 1;
                /* Expose needs no answer here: the frame is redrawn below anyway */
            }
            if (stop)
                break;
            redraw_frame();
            usleep(20000);
        }
    }

    if (f.fbo) { glDeleteFramebuffers(1, &f.fbo); glDeleteTextures(1, &f.tex); }
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, w1.ctx);
    if (use_second_window) glXDestroyContext(dpy, w2.ctx);
    XDestroyWindow(dpy, w1.w);
    if (use_second_window) XDestroyWindow(dpy, w2.w);
    XCloseDisplay(dpy);
    printf("step=%d done\n", step);
    return 0;
}
