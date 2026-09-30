/* SPDX-License-Identifier: MIT
 * Exercise the actual VirGL standard-primary storage as a DXVK render target.
 * This does not substitute a separately created D3D texture for the primary.
 */
#define TRITON_LINUX_D3D11_NO_MAIN
#include "linux-d3d11-test.cpp"
#define VIRGL_RENDERER_UNSTABLE_APIS
extern "C" {
#include "virglrenderer.h"
#include "virgl_hw.h"
}
#include <epoxy/gl.h>

static void primary_fence(void *, uint32_t) {}
static int primary_drm(void *) {
  const char *node=getenv("TRITON_TEST_RENDER_NODE");
  return open(node?node:"/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
}
static uint32_t fourcc(char a, char b, char c, char d) {
  return uint32_t(a) | (uint32_t(b)<<8) | (uint32_t(c)<<16) | (uint32_t(d)<<24);
}
static void test_primary_interop(Ctx &c, uint32_t width, uint32_t height, uint32_t id) {
  const char *T = "primary_interop";
  virgl_renderer_resource_create_args args{};
  args.handle=id; args.target=2; args.format=VIRGL_FORMAT_B8G8R8A8_UNORM;
  args.bind=VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW |
      VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT; // Current KMD standard primary.
  args.width=width; args.height=height; args.depth=args.array_size=1;
  CHECK(T, virgl_renderer_resource_create(&args, nullptr, 0) == 0, return);
  struct ResourceGuard { uint32_t id; ~ResourceGuard(){virgl_renderer_resource_unref(id);} } guard{id};
  std::vector<uint32_t> pixels(width*height, 0xffff0000);
  iovec iov{pixels.data(), pixels.size()*4};
  virgl_box box{0,0,0,width,height,1};
  CHECK(T, virgl_renderer_transfer_write_iov(id,0,0,width*4,width*height*4,&box,0,&iov,1)==0, return);
  virgl_renderer_force_ctx_0();
  glFinish();
  virgl_renderer_export_query query{};
  query.hdr.stype=VIRGL_RENDERER_STRUCTURE_TYPE_EXPORT_QUERY;
  query.hdr.size=sizeof(query);
  query.in_resource_id=id;
  CHECK(T, virgl_renderer_execute(&query,sizeof(query))==0, return);
  CHECK(T, query.out_num_fds==1 && query.out_fds[0]==-1, return);
  const auto metadata=query;
  query.in_export_fds=1;
  CHECK(T, virgl_renderer_execute(&query,sizeof(query))==0, return);
  struct FdGuard { virgl_renderer_export_query &q; ~FdGuard(){for(int fd:q.out_fds)if(fd>=0)close(fd);} } fds{query};
  CHECK(T, query.out_fourcc==metadata.out_fourcc &&
      query.out_modifier==metadata.out_modifier &&
      query.out_strides[0]==metadata.out_strides[0] &&
      query.out_offsets[0]==metadata.out_offsets[0]);
  emit("[PRIMARY] %ux%u fourcc=%08x planes=%u modifier=%016llx stride=%u offset=%u",
    width,height,query.out_fourcc,query.out_num_fds,
    (unsigned long long)query.out_modifier,query.out_strides[0],query.out_offsets[0]);
  CHECK(T, query.out_num_fds==1 && query.out_fds[0]>=0, return);
  DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
  if(query.out_fourcc==fourcc('A','R','2','4')) format=DXGI_FORMAT_B8G8R8A8_UNORM;
  if(query.out_fourcc==fourcc('X','R','2','4')) format=DXGI_FORMAT_B8G8R8X8_UNORM;
  if(query.out_fourcc==fourcc('A','B','2','4')) format=DXGI_FORMAT_R8G8B8A8_UNORM;
  CHECK(T, format!=DXGI_FORMAT_UNKNOWN, return);
  auto size=lseek(query.out_fds[0],0,SEEK_END);
  CHECK(T, size>0, return);
  DxvkSharedTextureDescriptor desc{};
  desc.magic=DXVK_SHARED_DESCRIPTOR_TEXTURE; desc.version=DXVK_SHARED_DESCRIPTOR_VERSION;
  desc.structSize=sizeof(desc); desc.fd=query.out_fds[0];
  desc.meta.Width=width;desc.meta.Height=height;
  desc.meta.MipLevels=desc.meta.ArraySize=desc.meta.SampleDesc.Count=1;
  desc.meta.Format=format;desc.meta.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
  desc.meta.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
  desc.drmFormatModifier=query.out_modifier;desc.planeCount=1;desc.allocationSize=size;
  desc.planes[0].offset=query.out_offsets[0];desc.planes[0].pitch=query.out_strides[0];
  Com<ID3D11Texture2D> imported;
  CHECK_HR(T,c.device->OpenSharedResource(&desc,__uuidof(ID3D11Texture2D),(void **)&imported));
  CHECK(T,bool(imported),return);
  auto gpu=readback_tex2d(c,imported,0,4);
  CHECK(T,gpu.size()==width*height*4,return);
  uint32_t pixel=0;memcpy(&pixel,gpu.data()+((height/2)*width+width/2)*4,4);
  uint32_t red=format==DXGI_FORMAT_R8G8B8A8_UNORM?0x000000ff:0x00ff0000;
  emit("[PRIMARY] GL red -> D3D pixel=%08x",pixel);
  CHECK(T,(pixel&0xffffff)==red);
  Com<ID3D11RenderTargetView> rtv;
  CHECK_HR(T,c.device->CreateRenderTargetView(imported,nullptr,&rtv));
  CHECK(T,bool(rtv),return);
  const FLOAT colours[][4]={{0,1,0,1},{0,0,1,1},{1,0,0,1}};
  const uint32_t expected[]={0x0000ff00,0x000000ff,0x00ff0000};
  for(unsigned phase=0;phase<3;phase++) {
    c.ctx->ClearRenderTargetView(rtv,colours[phase]);
    gpu=readback_tex2d(c,imported,0,4); // Complete D3D before VirGL reads.
    CHECK(T,gpu.size()==width*height*4,return);
    std::fill(pixels.begin(),pixels.end(),0);
    virgl_renderer_force_ctx_0();
    CHECK(T,virgl_renderer_transfer_read_iov(id,0,0,width*4,width*height*4,&box,0,&iov,1)==0,return);
    pixel=pixels[(height/2)*width+width/2];
    emit("[PRIMARY] D3D phase=%u -> GL pixel=%08x expected=%08x",phase,pixel,expected[phase]);
    CHECK(T,(pixel&0xffffff)==expected[phase]);
  }

}
int main() {
  g_out=stdout;
  virgl_renderer_callbacks cb{};cb.version=2;cb.write_fence=primary_fence;cb.get_drm_fd=primary_drm;
  int cookie=0;
  if(virgl_renderer_init(&cookie,VIRGL_RENDERER_USE_EGL|VIRGL_RENDERER_USE_SURFACELESS,&cb)) return 2;
  {
    Ctx c;
    if(create_device(c)) {
      test_primary_interop(c,1280,720,1);
      test_primary_interop(c,800,600,2);
    }
  }
  virgl_renderer_cleanup(&cookie);
  emit("[PRIMARY-SUMMARY] passed=%d failed=%d",g_passes,g_fails);
  return g_fails?1:0;
}
