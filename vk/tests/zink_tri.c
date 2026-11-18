#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
static GLuint sh(GLenum t, const char *s){GLuint o=glCreateShader(t);glShaderSource(o,1,&s,0);glCompileShader(o);GLint ok;glGetShaderiv(o,GL_COMPILE_STATUS,&ok);if(!ok){char l[512];glGetShaderInfoLog(o,512,0,l);printf("shader: %s\n",l);}return o;}
int main(int argc,char**argv){
 int devidx = argc>1?atoi(argv[1]):1;
 PFNEGLQUERYDEVICESEXTPROC qd=(void*)eglGetProcAddress("eglQueryDevicesEXT");
 PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
 EGLDeviceEXT devs[8]; EGLint n=0; qd(8,devs,&n); printf("devices %d\n",n);
 EGLDisplay d=gpd(EGL_PLATFORM_DEVICE_EXT,devs[devidx],0);
 EGLint maj,min; if(!eglInitialize(d,&maj,&min)){printf("eglInitialize failed 0x%x\n",eglGetError());return 1;}
 printf("egl %d.%d vendor %s\n",maj,min,eglQueryString(d,EGL_VENDOR));
 eglBindAPI(EGL_OPENGL_ES_API);
 EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
 EGLConfig cfg; EGLint nc; if(!eglChooseConfig(d,ca,&cfg,1,&nc)||!nc){printf("no config 0x%x\n",eglGetError());return 1;}
 EGLint pa[]={EGL_WIDTH,64,EGL_HEIGHT,64,EGL_NONE};
 EGLSurface s=eglCreatePbufferSurface(d,cfg,pa); if(s==EGL_NO_SURFACE){printf("pbuffer failed 0x%x\n",eglGetError());return 1;}
 EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
 EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,xa); if(c==EGL_NO_CONTEXT){printf("ctx failed 0x%x\n",eglGetError());return 1;}
 if(!eglMakeCurrent(d,s,s,c)){printf("makecurrent failed 0x%x\n",eglGetError());return 1;}
 printf("GL_RENDERER %s\nGL_VERSION %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
 GLuint p=glCreateProgram();
 glAttachShader(p,sh(GL_VERTEX_SHADER,"attribute vec2 a;void main(){gl_Position=vec4(a,0,1);}"));
 glAttachShader(p,sh(GL_FRAGMENT_SHADER,"precision mediump float;void main(){gl_FragColor=vec4(1,0,0,1);}"));
 glBindAttribLocation(p,0,"a");glLinkProgram(p);glUseProgram(p);
 float v[]={-1,-1, 3,-1, -1,3};
 glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,0,0,v);
 glViewport(0,0,64,64);glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
 glDrawArrays(GL_TRIANGLES,0,3);
 unsigned char px[4]={9,9,9,9}; glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
 printf("glGetError 0x%x pixel %u,%u,%u,%u (red expected on hardware; fake backend does not rasterize)\n",glGetError(),px[0],px[1],px[2],px[3]);
 return 0;}
