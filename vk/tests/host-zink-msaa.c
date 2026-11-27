#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s EGL=%x GL=%x\n", #x, eglGetError(), glGetError()); return 1; } } while (0)
int main(void) {
 EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY); CHECK(eglInitialize(d,0,0));
 CHECK(eglBindAPI(EGL_OPENGL_ES_API));
 EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_NONE}, n;
 EGLConfig cfg; CHECK(eglChooseConfig(d,ca,&cfg,1,&n)&&n);
 EGLint pa[]={EGL_WIDTH,64,EGL_HEIGHT,64,EGL_NONE}, ctxa[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
 EGLSurface s=eglCreatePbufferSurface(d,cfg,pa); EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,ctxa);
 CHECK(s!=EGL_NO_SURFACE && c!=EGL_NO_CONTEXT && eglMakeCurrent(d,s,s,c));
 fprintf(stderr,"renderer=%s version=%s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
 GLuint fb,rb[2],dst,tex; glGenFramebuffers(1,&fb); glBindFramebuffer(GL_FRAMEBUFFER,fb);
 glGenRenderbuffers(2,rb); glBindRenderbuffer(GL_RENDERBUFFER,rb[0]);
 glRenderbufferStorageMultisample(GL_RENDERBUFFER,4,GL_RGBA8,64,64);
 glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,rb[0]);
 glBindRenderbuffer(GL_RENDERBUFFER,rb[1]); glRenderbufferStorageMultisample(GL_RENDERBUFFER,4,GL_DEPTH24_STENCIL8,64,64);
 glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,rb[1]);
 CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
 const char *vs="#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0,1);}";
 const char *fs="#version 300 es\nprecision mediump float;out vec4 o;void main(){o=vec4(1,0,0,1);}";
 GLuint v=glCreateShader(GL_VERTEX_SHADER),f=glCreateShader(GL_FRAGMENT_SHADER),p=glCreateProgram();
 glShaderSource(v,1,&vs,0);glCompileShader(v);glShaderSource(f,1,&fs,0);glCompileShader(f);
 GLint ok;glGetShaderiv(v,GL_COMPILE_STATUS,&ok);CHECK(ok);glGetShaderiv(f,GL_COMPILE_STATUS,&ok);CHECK(ok);
 glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);glGetProgramiv(p,GL_LINK_STATUS,&ok);CHECK(ok);glUseProgram(p);
 glViewport(0,0,64,64);glEnable(GL_STENCIL_TEST);glStencilMask(255);glClearStencil(0);
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
 glStencilFunc(GL_ALWAYS,1,255);glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);glDrawArrays(GL_TRIANGLES,0,3);
 glFlush();glFinish();
 glStencilFunc(GL_EQUAL,1,255);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);glDrawArrays(GL_TRIANGLES,0,3);
 glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,64,64);
 glGenFramebuffers(1,&dst);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,dst);
 glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
 CHECK(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
 glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);glBlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);
 glFinish(); fprintf(stderr,"final GL error=%x\n",glGetError()); return 0;
}
