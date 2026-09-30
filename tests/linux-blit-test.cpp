/* Real GPU test of the production Vista stretch/format-conversion helper. */
#define TRITON_LINUX_D3D11_NO_MAIN
#include "../tests/linux-d3d11-test.cpp"
#include "../triton-umd/src/virtio/neptune/triton/tritonBlitShaders.h"
#include <d3d9.h>
#define D3DDDIFMT_A2R10G10B10 D3DFMT_A2R10G10B10
#define D3DDDIFMT_R16F D3DFMT_R16F
#define D3DDDIFMT_R32F D3DFMT_R32F
#define D3DDDIFMT_G16R16 D3DFMT_G16R16
#define D3DDDIFMT_G16R16F D3DFMT_G16R16F
#define D3DDDIFMT_G32R32F D3DFMT_G32R32F
#define D3DDDIFMT_X8B8G8R8 D3DFMT_X8B8G8R8
#define D3DDDIFMT_X1R5G5B5 D3DFMT_X1R5G5B5
#define D3DDDIFMT_X4R4G4B4 D3DFMT_X4R4G4B4
struct TRITON9_DEVICE {
 void *stretchBlitState = nullptr;
 ID3D11Device *hostDevice;
 ID3D11DeviceContext *hostContext;
};
struct TRITON9_RESOURCE {
 UINT contentWrites = 0;
 UINT subresourceIndex = 0;
 D3DFORMAT format = D3DFMT_UNKNOWN;
 DXGI_FORMAT hostFormat;
 ID3D11Resource *hostResource = nullptr, *stagingResource = nullptr;
 bool system = false, isBuffer = false;
 UINT width=6,height=4,depth=1;
 BYTE *shadow = nullptr;
 UINT pitch=24, slicePitch=96, bytesPerPixel=4;
};
static bool triton9ResourceBelongsToDevice(TRITON9_DEVICE *d, TRITON9_RESOURCE *r) { return d && r; }
static void triton9ResourceWritten(TRITON9_RESOURCE *r) { ++r->contentWrites; }
static bool triton9ResourceIsSystemMemory(TRITON9_RESOURCE *r) { return r->system; }
static HRESULT triton9CheckHostDevice(TRITON9_DEVICE *d) {return d->hostDevice->GetDeviceRemovedReason();}
#define GetProcessHeap() nullptr
#define HEAP_ZERO_MEMORY 0
#define HeapAlloc(h,f,s) calloc(1,s)
#define HeapFree(h,f,p) free(p)
/* PRODUCTION_BLIT */
static uint32_t pixel(DXGI_FORMAT f, int x, int y, bool opaque=false) {
 uint32_t r=0x21+x*7, g=0x43+y*9, b=0x87+x+y;
 uint32_t a=(opaque||f==DXGI_FORMAT_B8G8R8X8_UNORM)?255:0x65+x*3;
 return (f==DXGI_FORMAT_R8G8B8A8_UNORM ? r|(g<<8)|(b<<16) : b|(g<<8)|(r<<16)) | (a<<24);
}
static std::vector<uint8_t> blit_readback(Ctx &c, ID3D11Texture2D *tex, bool staging) {
 if(!staging)return readback_tex2d(c,tex,0,4);
 // Map the destination itself. An extra staging copy can hide completion bugs.
 D3D11_TEXTURE2D_DESC td;tex->GetDesc(&td);
 D3D11_MAPPED_SUBRESOURCE m={};
 if(FAILED(c.ctx->Map(tex,0,D3D11_MAP_READ,0,&m)))return {};
 std::vector<uint8_t> bytes(td.Width*td.Height*4);
 for(UINT y=0;y<td.Height;y++)memcpy(bytes.data()+y*td.Width*4,(BYTE*)m.pData+y*m.RowPitch,td.Width*4);
 c.ctx->Unmap(tex,0);return bytes;
}
static void run(Ctx &c) {
 const char *T="vista_gpu_blit";
 TRITON9_DEVICE d={nullptr,c.device,c.ctx};
 const DXGI_FORMAT fs[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM};
 for(auto sf:fs)for(auto df:fs)for(int mode=0;mode<3;mode++) {
  std::vector<uint32_t> in(24), initial(24,0xeeeeeeeeu);
  for(int y=0;y<4;y++)for(int x=0;x<6;x++)in[y*6+x]=pixel(sf,x,y);
  TRITON9_RESOURCE src,dst;src.hostFormat=sf;dst.hostFormat=df;
  src.system=mode==1;dst.system=mode==2;src.shadow=(BYTE*)in.data();
  D3D11_TEXTURE2D_DESC td={};td.Width=6;td.Height=4;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
  td.Format=sf;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
  D3D11_SUBRESOURCE_DATA init={in.data(),24,96};
  Com<ID3D11Texture2D> a,b;
  CHECK_HR(T,c.device->CreateTexture2D(&td,&init,&a));src.hostResource=a;
  td.Format=df;
  if(dst.system){td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;}
  init.pSysMem=initial.data();CHECK_HR(T,c.device->CreateTexture2D(&td,&init,&b));
  dst.hostResource=dst.system?nullptr:(ID3D11Resource*)b;
  dst.stagingResource=dst.system?(ID3D11Resource*)b:nullptr;
  D3D11_VIEWPORT vp={2,3,17,19,0.2f,0.8f};c.ctx->RSSetViewports(1,&vp);
  RECT from={2,1,5,3},to={1,1,4,3};
  CHECK_HR(T,triton9StretchBlt(&d,&src,&dst,&from,&to,FALSE));
  CHECK(T,dst.contentWrites==1);
  D3D11_VIEWPORT after={};UINT n=1;c.ctx->RSGetViewports(&n,&after);
  CHECK(T,n==1&&memcmp(&after,&vp,sizeof(vp))==0);
  auto bytes=blit_readback(c,b,dst.system);CHECK(T,bytes.size()==96,return);
  bool ok=true;
  for(int y=0;y<4;y++)for(int x=0;x<6;x++) {
   uint32_t got;memcpy(&got,bytes.data()+(y*6+x)*4,4);
   uint32_t expected=(x>=1&&x<4&&y>=1&&y<3)?pixel(df,x+1,y,sf==DXGI_FORMAT_B8G8R8X8_UNORM):0xeeeeeeeeu;
   // BGRX storage's unused alpha byte is not part of its public colour.
   uint32_t mask=df==DXGI_FORMAT_B8G8R8X8_UNORM?0x00ffffff:0xffffffff;
   if((got&mask)!=(expected&mask)){emit("[PIXEL] src=%u dst=%u mode=%d x=%d y=%d got=%08x expected=%08x",sf,df,mode,x,y,got,expected);ok=false;}
  }
  CHECK(T,ok);
  // Reuse the destination after a deferred clear, as fullscreen readback does.
  Com<ID3D11RenderTargetView> clearView;
  CHECK_HR(T,c.device->CreateRenderTargetView(a,nullptr,&clearView));
  const float green[]={0,1,0,1};c.ctx->ClearRenderTargetView(clearView,green);
  std::fill(in.begin(),in.end(),0xff00ff00u);
  CHECK_HR(T,triton9StretchBlt(&d,&src,&dst,&from,&to,FALSE));
  bytes=blit_readback(c,b,dst.system);CHECK(T,bytes.size()==96,return);
  uint32_t reused;memcpy(&reused,bytes.data()+(1*6+2)*4,4);
  CHECK(T,(reused&0xffffff)==0x00ff00);
 }
 triton9ReleaseStretchBlit(&d);
}
int main() {g_out=stdout;Ctx c;if(create_device(c))run(c);emit("[BLIT-SUMMARY] passed=%d failed=%d",g_passes,g_fails);return g_fails?1:0;}
