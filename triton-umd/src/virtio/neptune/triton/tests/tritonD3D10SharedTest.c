/* SPDX-License-Identifier: MIT
 * Real native D3D11 textures with the production host export/import functions.
 * The resource table models only the virtio/KMD fd attach boundary. */
#include <d3d11.h>
#include <dxvk_shared_resource.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cinttypes>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include "virtio/virtio-gpu/virgl_hw.h"
#define CHECK(c) do { ++checks; if (!(c)) {fprintf(stderr,"FAIL shared line %u: %s\n",__LINE__,#c);exit(1);} } while(0)
static unsigned checks;
enum virgl_resource_fd_type {VIRGL_RESOURCE_FD_INVALID,VIRGL_RESOURCE_FD_DMABUF,VIRGL_RESOURCE_FD_SHM};
struct npt_resource {virgl_resource_fd_type fd_type;uint64_t size;union {int fd;void *data;} u;};
struct hash_entry {void *data;};
struct hash_table {std::map<uint32_t,hash_entry> entries;};
// HOST_LAYOUT
struct npt_context {
 std::mutex resource_mutex;
 hash_table table;
 hash_table *resource_table=&table;
 std::map<uint64_t,void*> objects;
 virgl_attachment_layout pending_layout={};
 int pending_fd=-1;
 uint64_t pending_size=0;
 uint32_t pending_format=0,ctx_id=1;
};
static const hash_entry *_mesa_hash_table_search(hash_table *table,const uint32_t *id){auto i=table->entries.find(*id);return i==table->entries.end()?nullptr:&i->second;}
#define mtx_lock(m) (m)->lock()
#define mtx_unlock(m) (m)->unlock()
static int thrd_sleep(const timespec *duration,void*){std::this_thread::sleep_for(std::chrono::nanoseconds(duration->tv_nsec));return 0;}
#define NPT_OBJECT_TYPE_IUNKNOWN 0
#define NPT_OBJECT_TYPE_ID3D11TEXTURE2D 1
#define NPT_OBJECT_TYPE_ID3D11DEVICE 2
#define NPT_SHARED_FD_TYPE VIRGL_RESOURCE_FD_DMABUF
#define NPT_BLOB_EXPORT_MAX_PLANES 4
#define NPT_D3D11_BIND_SHADER_RESOURCE 8u
#define NPT_D3D11_BIND_RENDER_TARGET 32u
#define NPT_D3D11_RESOURCE_MISC_SHARED 2u
#define NPT_SHARED_EXPORT_FORMAT(d) ((d)->meta.Format)
#define NPT_SHARED_ATTACH_WAIT_MS 2
#define NPT_SHARED_ATTACH_POLL_MS 1
#define NPT_S_OK S_OK
#define NPT_E_INVALIDARG E_INVALIDARG
#define NPT_E_FAIL E_FAIL
#define NPT_FAILED FAILED
#define npt_log(...) ((void)0)
#define NPT_IID_IDXGIResource __uuidof(IDXGIResource)
#define NPT_IID_ID3D11Texture2D __uuidof(ID3D11Texture2D)
static void *npt_context_lookup_object(npt_context *ctx,void*,uint64_t id,unsigned){auto i=ctx->objects.find(id);return i==ctx->objects.end()?nullptr:i->second;}
static void npt_context_register_object(npt_context *ctx,uint64_t id,void *object,unsigned){ctx->objects.emplace(id,object);}
static HRESULT npt_com_query_interface(void *object,const GUID *iid,void **out){return static_cast<IUnknown*>(object)->QueryInterface(*iid,out);}
static void npt_com_release(void *object){static_cast<IUnknown*>(object)->Release();}
using PFN_IDXGIResource_GetSharedHandle=HRESULT (*)(void*,HANDLE*);
using PFN_ID3D11Device_OpenSharedResource=HRESULT (*)(void*,HANDLE,const GUID*,void**);
static HRESULT PFN_IDXGIResource_GetSharedHandle_call(void *resource,HANDLE *handle){return static_cast<IDXGIResource*>(resource)->GetSharedHandle(handle);}
static HRESULT PFN_ID3D11Device_OpenSharedResource_call(void *device,HANDLE handle,const GUID *iid,void **out){return static_cast<ID3D11Device*>(device)->OpenSharedResource(handle,*iid,out);}
#define NPT_COM_VTBL_FUNC(type,vtbl,slot) type##_call
static bool npt_context_register_pending_blob(npt_context *ctx,uint64_t,virgl_resource_fd_type type,int fd,uint64_t size,uint32_t format,const virgl_attachment_layout *layout){
 CHECK(type==NPT_SHARED_FD_TYPE&&ctx->pending_fd<0);ctx->pending_fd=fd;ctx->pending_size=size;ctx->pending_format=format;CHECK(layout);ctx->pending_layout=*layout;return true;
}
// WIRE_TYPES
// PRODUCTION_HOST_SHARED
static void complete(ID3D11Device *d,ID3D11DeviceContext *c){
 D3D11_QUERY_DESC desc={D3D11_QUERY_EVENT,0};ID3D11Query *q=nullptr;
 CHECK(SUCCEEDED(d->CreateQuery(&desc,&q)));c->End(q);c->Flush();BOOL ready=FALSE;HRESULT hr=S_FALSE;
 for(unsigned i=0;i<1000000&&hr==S_FALSE;++i)hr=c->GetData(q,&ready,sizeof(ready),0);
 CHECK(hr==S_OK&&ready);q->Release();
}
static void check_read(ID3D11Device *d,ID3D11DeviceContext *c,ID3D11Texture2D *texture,const unsigned char *expected,unsigned bytes,unsigned tolerance=0){
 D3D11_TEXTURE2D_DESC desc;texture->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.Usage=D3D11_USAGE_STAGING;
 ID3D11Texture2D *read=nullptr;CHECK(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&read)));c->CopyResource(read,texture);
 D3D11_MAPPED_SUBRESOURCE map;CHECK(SUCCEEDED(c->Map(read,0,D3D11_MAP_READ,0,&map)));
 for(unsigned y=0;y<8;++y){auto actual=static_cast<unsigned char*>(map.pData)+y*map.RowPitch;
  if(!tolerance)CHECK(!memcmp(actual,expected+y*8*bytes,8*bytes));
  else for(unsigned i=0;i<8*bytes;++i){unsigned limit=(i%4==3)?0:tolerance;CHECK(unsigned(std::abs(int(actual[i])-int(expected[y*8*bytes+i])))<=limit);}
 }
 c->Unmap(read,0);read->Release();
}
static void compressed_copy(ID3D11Device *d,ID3D11DeviceContext *c){
 for(auto format:{DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC2_UNORM,DXGI_FORMAT_BC3_UNORM,DXGI_FORMAT_BC4_UNORM,DXGI_FORMAT_BC5_UNORM}){
  unsigned blockBytes=(format==DXGI_FORMAT_BC1_UNORM||format==DXGI_FORMAT_BC4_UNORM)?8:16;
  unsigned char bytes[256];for(unsigned i=0;i<sizeof(bytes);++i)bytes[i]=(unsigned char)(i*13+7);
  D3D11_TEXTURE2D_DESC desc={};desc.Width=desc.Height=4;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=blockBytes==8?DXGI_FORMAT_R32G32_UINT:DXGI_FORMAT_R32G32B32A32_UINT;
  D3D11_SUBRESOURCE_DATA initial={bytes,4*blockBytes,0};ID3D11Texture2D *source=nullptr,*compressed=nullptr,*read=nullptr;
  CHECK(SUCCEEDED(d->CreateTexture2D(&desc,&initial,&source)));desc.Width=desc.Height=16;desc.Format=format;
  CHECK(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&compressed)));c->CopyResource(compressed,source);
  desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  CHECK(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&read)));c->CopyResource(read,compressed);
  D3D11_MAPPED_SUBRESOURCE mapped={};CHECK(SUCCEEDED(c->Map(read,0,D3D11_MAP_READ,0,&mapped)));
  for(unsigned row=0;row<4;++row)CHECK(!memcmp((unsigned char*)mapped.pData+row*mapped.RowPitch,bytes+row*4*blockBytes,4*blockBytes));
  c->Unmap(read,0);read->Release();compressed->Release();source->Release();printf("D3D10.1 native BC reinterpret format=%u passed\n",unsigned(format));
 }
}
int main(){
 ID3D11Device *d[2]={};ID3D11DeviceContext *c[2]={};D3D_FEATURE_LEVEL fl;
 for(unsigned i=0;i<2;++i)CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d[i],&fl,&c[i])));
 compressed_copy(d[0],c[0]);
 for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8X8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT}){
  unsigned bytes=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
  D3D11_TEXTURE2D_DESC desc={};desc.Width=desc.Height=8;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=format;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED|DXVK_D3D11_RESOURCE_MISC_SINGLE_PLANE_EXPORT;
  ID3D11Texture2D *texture=nullptr;CHECK(SUCCEEDED(d[0]->CreateTexture2D(&desc,nullptr,&texture)));
  npt_context producer,consumer;producer.objects[1]=texture;consumer.objects[2]=d[1];
  npt_blob_export_info info={};npt_resource reply={VIRGL_RESOURCE_FD_SHM,sizeof(info),{}};reply.u.data=&info;producer.table.entries[3]={&reply};
  CHECK(npt_shared_export_blob(&producer,1,4,3,0)==S_OK);CHECK(producer.pending_fd>=0&&info.plane_count==1&&info.planes[0].pitch>=8*bytes);
  CHECK(producer.pending_format==npt_shared_dxgi_to_virgl_format(format));
  CHECK(producer.pending_layout.width==8&&producer.pending_layout.height==8);
  CHECK(producer.pending_layout.plane_count==info.plane_count&&producer.pending_layout.modifier==info.modifier);
  CHECK(producer.pending_layout.strides[0]==info.planes[0].pitch&&producer.pending_layout.offsets[0]==info.planes[0].offset);
  auto bad=info;bad.planes[0].pitch=8*bytes-1;
  CHECK(!npt_shared_linear_desc_valid(8,8,format,1,1,1,0,desc.BindFlags,0,2,&bad));
  bad=info;bad.allocation_size=bad.planes[0].offset+7*bad.planes[0].pitch+8*bytes-1;
  CHECK(!npt_shared_linear_desc_valid(8,8,format,1,1,1,0,desc.BindFlags,0,2,&bad));
  bad=info;bad.texture_layout=3;CHECK(!npt_shared_linear_desc_valid(8,8,format,1,1,1,0,desc.BindFlags,0,2,&bad));
  bad=info;bad.planes[1].pitch=1;CHECK(!npt_shared_linear_desc_valid(8,8,format,1,1,1,0,desc.BindFlags,0,2,&bad));
  CHECK(!npt_shared_linear_desc_valid(8,8,DXGI_FORMAT_UNKNOWN,1,1,1,0,desc.BindFlags,0,2,&info));
  CHECK(!npt_shared_linear_desc_valid(8,8,format,1,1,4,0,desc.BindFlags,0,2,&info));
  CHECK(!npt_shared_linear_desc_valid(8,8,format,1,1,1,0,desc.BindFlags,0,0,&info));
  npt_resource attachment={VIRGL_RESOURCE_FD_DMABUF,producer.pending_size,{}};attachment.u.fd=producer.pending_fd;producer.pending_fd=-1;consumer.table.entries[5]={&attachment};
  npt_cmd_shared_open_res command={};command.res_id=5;command.mint_object_id=6;command.width=command.height=8;command.mip_levels=command.array_size=command.sample_count=1;command.format=format;command.bind_flags=desc.BindFlags;command.misc_flags=D3D11_RESOURCE_MISC_SHARED;command.export_info=info;
  CHECK(npt_shared_open_res(&consumer,2,&command)==S_OK);CHECK(npt_shared_open_res(&consumer,2,&command)==E_INVALIDARG);
  auto *opened=static_cast<ID3D11Texture2D*>(consumer.objects[6]);CHECK(opened!=nullptr);
  D3D11_TEXTURE2D_DESC actual;opened->GetDesc(&actual);CHECK(actual.Format==format&&actual.MiscFlags==D3D11_RESOURCE_MISC_SHARED);
  uint16_t pattern[256];for(unsigned i=0;i<256;++i)pattern[i]=uint16_t(0x3800+i);
  c[0]->UpdateSubresource(texture,0,nullptr,pattern,8*bytes,0);complete(d[0],c[0]);check_read(d[1],c[1],opened,(const unsigned char*)pattern,bytes);
  for(unsigned i=0;i<256;++i)pattern[i]=uint16_t(0x3a00+i);
  c[1]->UpdateSubresource(opened,0,nullptr,pattern,8*bytes,0);complete(d[1],c[1]);check_read(d[0],c[0],texture,(const unsigned char*)pattern,bytes);
  if(format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB||format==DXGI_FORMAT_B8G8R8X8_UNORM_SRGB){
   ID3D11RenderTargetView *rt=nullptr;CHECK(SUCCEEDED(d[1]->CreateRenderTargetView(opened,nullptr,&rt)));const float gray[4]={.5f,.5f,.5f,1};c[1]->ClearRenderTargetView(rt,gray);rt->Release();complete(d[1],c[1]);
   /* IEC sRGB transfer: linear 0.5 encodes to round(0.735357*255)=188. */
   for(unsigned pixel=0;pixel<64;++pixel){auto p=(unsigned char*)pattern+pixel*4;p[0]=p[1]=p[2]=188;p[3]=255;}
   check_read(d[0],c[0],texture,(const unsigned char*)pattern,bytes,1);
  }
  /* Neither exporter lifetime nor a transport detach may revoke a completed import. */
  producer.objects.clear();texture->Release();consumer.table.entries.clear();close(attachment.u.fd);
  check_read(d[1],c[1],opened,(const unsigned char*)pattern,bytes,format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB||format==DXGI_FORMAT_B8G8R8X8_UNORM_SRGB?1:0);opened->Release();consumer.objects.erase(6);
  command.mint_object_id=7;CHECK(npt_shared_open_res(&consumer,2,&command)==E_INVALIDARG);
  printf("D3D10 production shared pair format=%u bytes=%u passed\n",unsigned(format),bytes);
 }
 for(unsigned i=0;i<2;++i){c[i]->ClearState();c[i]->Release();d[i]->Release();}
 printf("D3D10 production shared pair %u checks passed\n",checks);return 0;
}
