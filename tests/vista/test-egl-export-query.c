#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
typedef int EGLint;
typedef uint64_t EGLuint64KHR;
typedef void *EGLImageKHR;
typedef void *EGLClientBuffer;
#define EGL_NO_IMAGE_KHR NULL
#define EGL_GL_TEXTURE_2D_KHR 1
#define EGL_MESA_IMAGE_DMA_BUF_EXPORT 1
#define has_bit(a,b) ((a)&(b))
struct virgl_egl {int extension_bits; void *egl_display;};
struct virgl_renderer_export_query {
 uint32_t in_export_fds,out_num_fds,out_fourcc;
 uint64_t out_modifier;
 int out_fds[4];
 uint32_t out_strides[4],out_offsets[4];
};
static int planes,create_ok,query_ok,export_ok,destroyed,exported;
static int input_fds[4],strides[4],offsets[4],closed[32];
static uint64_t modifiers[4];
static void *eglGetCurrentContext(void){return (void *)1;}
static void *eglCreateImageKHR(void*d,void*c,int t,void*x,void*a){
 (void)d;(void)c;(void)t;(void)x;(void)a;return create_ok?(void*)1:NULL;
}
static int eglExportDMABUFImageQueryMESA(void*d,void*i,int*f,int*p,uint64_t*m){
 (void)d;(void)i;*f=123;*p=planes;memcpy(m,modifiers,sizeof(modifiers));return query_ok;
}
static int eglExportDMABUFImageMESA(void*d,void*i,int*f,int*s,int*o){
 (void)d;(void)i;exported++;memcpy(f,input_fds,sizeof(input_fds));
 memcpy(s,strides,sizeof(strides));memcpy(o,offsets,sizeof(offsets));return export_ok;
}
static void eglDestroyImageKHR(void*d,void*i){(void)d;(void)i;destroyed++;}
static int close(int fd){assert(fd>=0&&fd<32);closed[fd]++;return 0;}
/* SOURCE_UNDER_TEST */
static struct virgl_renderer_export_query reset(void){
 planes=1;create_ok=query_ok=export_ok=1;destroyed=exported=0;
 memset(closed,0,sizeof(closed));
 for(int i=0;i<4;i++){input_fds[i]=-1;strides[i]=5120;offsets[i]=i*4096;modifiers[i]=2;}
 input_fds[0]=7;
 struct virgl_renderer_export_query q={0};
 for(int i=0;i<4;i++)q.out_fds[i]=-1;
 return q;
}
int main(void){
 struct virgl_egl egl={1,NULL};
 struct virgl_renderer_export_query q=reset();
 assert(virgl_egl_export_texture_query(&egl,4,&q)==0);
 assert(q.out_num_fds==1&&q.out_fourcc==123&&q.out_modifier==2);
 assert(q.out_strides[0]==5120&&q.out_fds[0]==-1&&closed[7]==1&&destroyed==1);
 q=reset();q.in_export_fds=1;
 assert(virgl_egl_export_texture_query(&egl,4,&q)==0);
 assert(q.out_fds[0]==7&&closed[7]==0&&destroyed==1);
 q=reset();planes=3;input_fds[2]=9;q.in_export_fds=1;
 assert(virgl_egl_export_texture_query(&egl,4,&q)==0);
 assert(q.out_num_fds==2&&q.out_fds[0]==7&&q.out_fds[1]==9&&q.out_fds[2]==-1);
 assert(q.out_offsets[2]==8192&&closed[7]==0&&closed[9]==0);
 q=reset();planes=3;input_fds[2]=9;
 assert(virgl_egl_export_texture_query(&egl,4,&q)==0);
 assert(closed[7]==1&&closed[9]==1&&q.out_num_fds==2);
 q=reset();export_ok=0;input_fds[1]=9;
 assert(virgl_egl_export_texture_query(&egl,4,&q)==-EINVAL);
 assert(closed[7]==1&&closed[9]==1&&destroyed==1&&q.out_num_fds==0);
 q=reset();planes=2;modifiers[1]=3;
 assert(virgl_egl_export_texture_query(&egl,4,&q)==-EINVAL&&closed[7]==1);
 q=reset();strides[0]=0;
 assert(virgl_egl_export_texture_query(&egl,4,&q)==-EINVAL&&closed[7]==1);
 q=reset();offsets[0]=-1;
 assert(virgl_egl_export_texture_query(&egl,4,&q)==-EINVAL&&closed[7]==1);
 q=reset();planes=5;
 assert(virgl_egl_export_texture_query(&egl,4,&q)==-EINVAL&&exported==0&&destroyed==1);
 q=reset();query_ok=0;
 assert(virgl_egl_export_texture_query(&egl,4,&q)==-EINVAL&&exported==0&&destroyed==1);
 q=reset();create_ok=0;
 assert(virgl_egl_export_texture_query(&egl,4,&q)==-EINVAL&&destroyed==0);
 q=reset();egl.extension_bits=0;
 assert(virgl_egl_export_texture_query(&egl,4,&q)==-EINVAL&&destroyed==0);
 puts("PASS EGL export layout, fd packing/ownership, metadata-only cleanup, and failures");
}
