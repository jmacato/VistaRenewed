#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define NPT_BLOB_EXPORT_MAX_PLANES 4
#define NPT_D3D11_BIND_SHADER_RESOURCE 8
#define NPT_D3D11_BIND_RENDER_TARGET 32
#define NPT_D3D11_RESOURCE_MISC_SHARED 2
#define NPT_SHARED_ATTACH_POLL_MS 2
#define NPT_SHARED_ATTACH_WAIT_MS 1000
#define VIRGL_RESOURCE_FD_DMABUF 0
#define NPT_S_OK 0
#define NPT_E_INVALIDARG (-1)
typedef int HRESULT;
struct npt_reply_header { uint32_t cmd_type, cmd_return; };
struct npt_context { int resource_mutex; void *resource_table; };
struct hash_entry { void *data; };
struct npt_resource;
static struct hash_entry entry;
static int locked, polls, attach_after, detach_on_unlock;
static uint32_t npt_shared_dxgi_to_virgl_format(uint32_t f) {
 return (f==87||f==88||f==28)?1:0;
}
static void mtx_lock(int *m) {(void)m;assert(!locked);locked=1;}
static void mtx_unlock(int *m) {
 (void)m;assert(locked);locked=0;
 if(detach_on_unlock)entry.data=NULL;
}
static const struct hash_entry *_mesa_hash_table_search(void *t,const uint32_t *id) {
 (void)t;assert(locked&&*id==17);return polls>=attach_after&&entry.data?&entry:NULL;
}
static int thrd_sleep(const struct timespec *t,struct timespec *r) {
 (void)r;assert(!locked&&t->tv_nsec==2000000);polls++;return 0;
}
/* The lookup body needs the production layout type in its resource record. */
#define RESOURCE_RECORD struct npt_resource { int fd_type; struct {int fd;} u; uint64_t size; struct virgl_attachment_layout layout; };
/* SOURCE_UNDER_TEST */
static struct virgl_attachment_layout good(void) {
 return (struct virgl_attachment_layout){.width=800,.height=600,
  .fourcc=0x34324241,.plane_count=1,.modifier=0x0100000000000002ULL,
  .strides={3584},.offsets={4096}};
}
int main(void) {
 struct virgl_attachment_layout a=good();
 struct npt_cmd_shared_query_layout_reply r={0};
 const uint64_t size=4096+3584*600;
 assert(npt_shared_layout_to_reply(&a,size,&r));
 assert(r.width==800&&r.height==600&&r.format==28);
 assert(r.export_info.planes[0].pitch==3584&&r.export_info.planes[0].offset==4096);
 assert(r.export_info.modifier==a.modifier&&r.export_info.allocation_size==size);
 a.fourcc=0x34325241;assert(npt_shared_layout_to_reply(&a,size,&r)&&r.format==87);
 a.fourcc=0x34325258;assert(npt_shared_layout_to_reply(&a,size,&r)&&r.format==88);
 a.fourcc=0x34324258;assert(npt_shared_layout_to_reply(&a,size,&r)&&r.format==28);
 assert(r.export_info.modifier==a.modifier&&r.export_info.planes[0].pitch==3584);
 assert(r.export_info.planes[0].offset==4096&&r.export_info.allocation_size==size);
 a.fourcc=0;assert(!npt_shared_layout_to_reply(&a,size,&r));
 a=good();a.plane_count=0;assert(!npt_shared_layout_to_reply(&a,size,&r));
 a.plane_count=2;assert(!npt_shared_layout_to_reply(&a,size,&r));
 a=good();a.strides[1]=1;assert(!npt_shared_layout_to_reply(&a,size,&r));
 a=good();a.strides[0]=3199;assert(!npt_shared_layout_to_reply(&a,size,&r));
 a=good();a.offsets[0]=size;assert(!npt_shared_layout_to_reply(&a,size,&r));
 a=good();a.width=0;assert(!npt_shared_layout_to_reply(&a,size,&r));
 a=good();a.height=0;assert(!npt_shared_layout_to_reply(&a,size,&r));
 a=good();assert(!npt_shared_layout_to_reply(&a,0,&r));
 assert(!npt_shared_layout_to_reply(&a,4096+3584*599+3199,&r));
 assert(npt_shared_layout_to_reply(&a,4096+3584*599+3200,&r));
 struct npt_resource res={.fd_type=0,.u.fd=5,.size=size,.layout=good()};
 struct npt_context ctx={0};entry.data=&res;
 attach_after=3;assert(npt_shared_query_layout(&ctx,17,&r)==NPT_S_OK&&polls==3&&!locked);
 polls=0;attach_after=0;detach_on_unlock=1;
 assert(npt_shared_query_layout(&ctx,17,&r)==NPT_S_OK&&!entry.data&&r.format==28);
 detach_on_unlock=0;polls=0;
 assert(npt_shared_query_layout(&ctx,17,&r)==NPT_E_INVALIDARG&&polls==500&&!locked);
 entry.data=&res;res.fd_type=1;polls=0;
 assert(npt_shared_query_layout(&ctx,17,&r)==NPT_E_INVALIDARG&&polls==0);
 res.fd_type=0;res.u.fd=-1;
 assert(npt_shared_query_layout(&ctx,17,&r)==NPT_E_INVALIDARG);
 puts("PASS: primary layout formats, padded tiled rows, bounds, delayed attach, detach and timeout");
}
