from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'triton-qemu/hw/display/virtio-gpu-virgl.c').read_text()
def extract(a,b):return s[s.index(a):s.index(b,s.index(a))]
base=(Path(__file__).with_name('test-blob-readback.c')).read_text().split('/* BEGIN_EXTRACTED_HELPER:')[0]
base=base.replace('uint64_t blob_size;', 'uint64_t blob_size; uint32_t width,height,format;').replace('uint32_t bytes_pp, width, height, stride, offset;', 'uint32_t format, bytes_pp, width, height, stride, offset;').replace('bool is_blob;', 'bool is_blob; struct VirtIOGPUExternalCopy *external_copy;')
base=base.replace('int is_blob;', 'int is_blob; struct VirtIOGPUExternalCopy *external_copy;')
code=base+'''\n#include <assert.h>
#include <sys/uio.h>
typedef struct VirtIOGPU { int unused; } VirtIOGPU;
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
struct virtio_gpu_ctrl_hdr { uint32_t dummy[6]; };
struct virtio_gpu_box {uint32_t x,y,z,w,h,d;};
struct virgl_box;
static uint32_t output[8*6];
static int writes,write_error,live_surfaces;
static uint32_t virtio_gpu_get_pixman_format(uint32_t f){return f;}
#define QEMU_CLOCK_REALTIME 0
static int64_t qemu_clock_get_ns(int clock){(void)clock;return 0;}
static void trace_virtio_gpu_neptune_present_cost(uint32_t res,uint64_t bytes,int64_t snapshot,int64_t upload){(void)res;(void)bytes;(void)snapshot;(void)upload;}
#ifdef CONFIG_LINUX
#define VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP 1
struct virgl_renderer_resource_info { unsigned tex_id, flags, width, height; };
static bool gpu_enabled, gpu_required, gpu_flip, gpu_opaque;
static int gpu_status, gpu_calls;
static int target_info_error;
static unsigned gpu_x, gpu_y;
static int virgl_renderer_resource_get_info(uint32_t id,struct virgl_renderer_resource_info *info){assert(id==2);*info=(struct virgl_renderer_resource_info){gpu_enabled?42:0,gpu_flip?1:0,8,6};return target_info_error;}
static int virtio_gpu_neptune_copy_resource(VirtIOGPU *g,const struct virtio_gpu_virgl_resource *res,const struct virtio_gpu_framebuffer *fb,const struct virtio_gpu_rect *source,const struct virtio_gpu_rect *rect,unsigned texture,uint32_t x,uint32_t y,bool flip,struct VirtIOGPUExternalCopy **copy,uint32_t width,uint32_t height,bool opaque,bool *required){
 (void)res;(void)fb;(void)source;(void)rect;
 if(!gpu_enabled){*required=false;return -ENOTSUP;}
 assert(!g&&copy);if(!target_info_error)assert(texture==42&&width==8&&height==6&&flip==gpu_flip);
 ++gpu_calls;gpu_x=x;gpu_y=y;gpu_opaque=opaque;*required=gpu_required;return gpu_status;
}
#endif
'''+extract('struct virtio_gpu_triton_present_blt {','QEMU_BUILD_BUG_ON')+extract('static int virtio_gpu_neptune_readback_blob(','static int virtio_gpu_neptune_copy_resource(')+'''
static void qemu_free_displaysurface(DisplaySurface *s){free(s->data);free(s);--live_surfaces;}
static DisplaySurface *virtio_gpu_neptune_create_surface(uint32_t id,const struct virtio_gpu_virgl_resource*r,const struct virtio_gpu_framebuffer*f,const struct virtio_gpu_rect*box,uint32_t format,int*status){
 (void)id;(void)format;DisplaySurface*s=calloc(1,sizeof(*s));++live_surfaces;
 s->width=box->width;s->height=box->height;s->stride=s->width*4;s->bytes_pp=4;s->data=calloc(s->stride,s->height);
 *status=virtio_gpu_neptune_readback_blob(r,f,box,box->x,box->y,box->width,box->height,s);
 if(*status){qemu_free_displaysurface(s);return NULL;}return s;
}
static int virgl_renderer_transfer_write_iov(uint32_t id,int ctx,int level,uint32_t stride,uint32_t layer,struct virgl_box*raw,uint64_t offset,const struct iovec*iov,int count){
 const struct virtio_gpu_box*b=(void*)raw;
 assert(id==2&&ctx==0&&level==0&&count==1&&offset==0&&layer==iov->iov_len);
 ++writes;if(write_error)return write_error;
 for(uint32_t y=0;y<b->h;y++)memcpy(&output[(b->y+y)*8+b->x],(char*)iov->iov_base+y*stride,b->w*4);
 return 0;
}
'''+extract('static int virtio_gpu_neptune_present_blt(','static void virgl_cmd_triton_present_blt(')+'''
int main(void){
 char name[]="/tmp/triton-present-XXXXXX";exported_fd=mkstemp(name);assert(exported_fd>=0);unlink(name);
 uint32_t input[2+10*6];for(unsigned i=0;i<62;i++)input[i]=1000+i;
 assert(write(exported_fd,input,sizeof(input))==sizeof(input));
 struct virtio_gpu_virgl_resource src={.base={.resource_id=1,.blob_size=sizeof(input)},.is_blob=1};
 struct virtio_gpu_virgl_resource dst={.base={.resource_id=2,.width=8,.height=6,.format=2}};
 struct virtio_gpu_triton_present_blt b={.source_resource_id=1,.destination_resource_id=2,.source_width=8,.source_height=6,.source_format=2,.source_stride=40,.source_offset=8,.source_rect={2,1,3,2},.destination_x=4,.destination_y=3};
 for(unsigned i=0;i<48;i++)output[i]=0xdeadbeef;
 assert(!virtio_gpu_neptune_present_blt(&b,&src,&dst));assert(writes==1&&!live_surfaces);
 for(unsigned y=0;y<6;y++)for(unsigned x=0;x<8;x++)assert(output[y*8+x]==((x>=4&&x<7&&y>=3&&y<5)?input[2+(y-3+1)*10+x-4+2]:0xdeadbeef));
 b.source_rect=(struct virtio_gpu_rect){0,0,1,1};b.destination_x=0;b.destination_y=0;
 assert(!virtio_gpu_neptune_present_blt(&b,&src,&dst));assert(output[0]==input[2]&&output[3*8+4]==input[14]);
 int before=writes;b.source_rect.x=UINT32_MAX;assert(virtio_gpu_neptune_present_blt(&b,&src,&dst)==-EINVAL&&writes==before);
 b.source_rect.x=0;b.destination_x=8;assert(virtio_gpu_neptune_present_blt(&b,&src,&dst)==-EINVAL);
 b.destination_x=0;b.source_offset=sizeof(input);assert(virtio_gpu_neptune_present_blt(&b,&src,&dst)==-EINVAL&&!live_surfaces);
 b.source_offset=8;write_error=-EIO;assert(virtio_gpu_neptune_present_blt(&b,&src,&dst)==-EIO&&!live_surfaces);
 write_error=0;exported_type=0;assert(virtio_gpu_neptune_present_blt(&b,&src,&dst)==-EINVAL&&!live_surfaces);
 #ifdef CONFIG_LINUX
 exported_type=VIRGL_RENDERER_BLOB_FD_TYPE_SHM;
 gpu_enabled=true;gpu_required=true;before=writes;
 b.source_rect=(struct virtio_gpu_rect){2,1,3,2};b.destination_x=4;b.destination_y=3;
 assert(!virtio_gpu_neptune_present_blt(NULL,&b,&src,&dst));
 assert(gpu_calls==1&&writes==before&&gpu_x==4&&gpu_y==3&&gpu_opaque);
 gpu_flip=true;dst.base.format=VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
 assert(!virtio_gpu_neptune_present_blt(NULL,&b,&src,&dst));
 assert(gpu_y==3&&!gpu_opaque&&writes==before);
 gpu_status=-EIO;assert(virtio_gpu_neptune_present_blt(NULL,&b,&src,&dst)==-EIO&&writes==before);
 gpu_status=-EOWNERDEAD;assert(virtio_gpu_neptune_present_blt(NULL,&b,&src,&dst)==-EOWNERDEAD&&writes==before);
 target_info_error=-ENOENT;assert(virtio_gpu_neptune_present_blt(NULL,&b,&src,&dst)==-ENOENT&&writes==before);
 gpu_required=false;gpu_status=-ENOTSUP;
 assert(!virtio_gpu_neptune_present_blt(NULL,&b,&src,&dst)&&writes==before+1);
 target_info_error=0;
 #endif
 close(exported_fd);puts("PASS translated/padded/offset rectangle, retained pixels, disjoint updates, overflow, bounds, short SHM, upload failure, handle type, GPU routing, failure without CPU fallback, cleanup");
}
'''
code=code.replace('virtio_gpu_neptune_present_blt(&b', 'virtio_gpu_neptune_present_blt(NULL,&b')
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(code);exe=Path(d)/'test'
 subprocess.run(['clang','-std=c11','-D_DARWIN_C_SOURCE','-Wall','-Wextra','-Werror','-Wno-unused-parameter',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
 # This mutation reintroduces the metadata-error route to a CPU snapshot.
 mutant=code.replace('if (gpu_required) {', 'if (gpu_required && !target_status) {')
 assert mutant!=code
 p.write_text(mutant)
 subprocess.run(['clang','-std=c11','-D_DARWIN_C_SOURCE','-Wall','-Wextra','-Werror','-Wno-unused-parameter',str(p),'-o',str(exe)],check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True)
 assert result.returncode!=0 and 'Assertion' in result.stderr
 print('PASS rejected missing-destination-metadata CPU-fallback mutant')
