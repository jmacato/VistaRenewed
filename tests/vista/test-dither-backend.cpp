/* SPDX-License-Identifier: MIT
 * Real native DXVK context/control and low-bit rendering regression. */
#define TRITON_LINUX_D3D11_NO_MAIN
#include "../linux-d3d11-test.cpp"
#include "../../triton-dxvk/src/d3d11/d3d11_dither_control.h"
#include <array>
#include <set>

static const GUID capsGuid = TRITON_DITHER_CAPS_GUID;
static const GUID stateGuid = TRITON_DITHER_STATE_GUID;
static bool dropPixelEnable = false;
static HRESULT setDither(ID3D11DeviceContext* ctx, UINT enabled) {
  if (dropPixelEnable) enabled = 0;
  TritonDitherState state = {TRITON_DITHER_VERSION, enabled};
  return ctx->SetPrivateData(stateGuid, sizeof(state), &state);
}
static UINT getDither(ID3D11DeviceContext* ctx) {
  TritonDitherState state = {}; UINT size = sizeof(state);
  if (ctx->GetPrivateData(stateGuid, &size, &state) != S_OK ||
      size != sizeof(state) || state.version != TRITON_DITHER_VERSION) return ~0u;
  return state.enabled;
}
static void controls(Ctx& c, bool unsupported) {
  const char* T = "dither_control";
  UINT size = 0;
  CHECK_HR(T, c.ctx->GetPrivateData(capsGuid, &size, nullptr));
  CHECK(T, size == sizeof(TritonDitherCaps));
  TritonDitherCaps caps = {99,99}; size = 1;
  CHECK(T, c.ctx->GetPrivateData(capsGuid, &size, &caps) == DXGI_ERROR_MORE_DATA);
  CHECK(T, size == sizeof(caps) && caps.version == 99 && caps.flags == 99);
  CHECK_HR(T, c.ctx->GetPrivateData(capsGuid, &size, &caps));
  CHECK(T, caps.version == TRITON_DITHER_VERSION);
  CHECK(T, caps.flags == (unsupported ? 0u : TRITON_DITHER_NATIVE));
  CHECK(T, c.ctx->GetPrivateData(capsGuid, nullptr, &caps) == E_INVALIDARG);
  CHECK(T, c.ctx->SetPrivateData(capsGuid, sizeof(caps), &caps) == E_INVALIDARG);
  CHECK(T, c.ctx->SetPrivateDataInterface(capsGuid, nullptr) == E_INVALIDARG);
  CHECK(T, c.ctx->SetPrivateDataInterface(stateGuid, c.device) == E_INVALIDARG);
  CHECK(T, getDither(c.ctx) == 0u);
  TritonDitherState state = {2,1};
  CHECK(T, c.ctx->SetPrivateData(stateGuid, sizeof(state), &state) == E_INVALIDARG);
  state.version = 1; state.enabled = 2;
  CHECK(T, c.ctx->SetPrivateData(stateGuid, sizeof(state), &state) == E_INVALIDARG);
  state.enabled = 1;
  CHECK(T, c.ctx->SetPrivateData(stateGuid, sizeof(state)-1, &state) == E_INVALIDARG);
  CHECK(T, c.ctx->SetPrivateData(stateGuid, sizeof(state), nullptr) == E_INVALIDARG);
  CHECK(T, c.ctx->SetPrivateData(stateGuid, 0, nullptr) == E_INVALIDARG);
  CHECK(T, getDither(c.ctx) == 0u);
  CHECK(T, setDither(c.ctx, 1) == (unsupported ? DXGI_ERROR_UNSUPPORTED : S_OK));
  CHECK(T, getDither(c.ctx) == (unsupported ? 0u : 1u));
  if (!unsupported) {
    CHECK(T, c.ctx->SetPrivateData(stateGuid, 0, nullptr) == E_INVALIDARG);
    CHECK(T, getDither(c.ctx) == 1u);
    CHECK_HR(T, setDither(c.ctx, 1));
  }
  GUID ordinary = {0x9abc1234, 0x9876, 0x2345, {1,2,3,4,5,6,7,8}};
  UINT value = 0xabcdef01, fetched = 0; size = sizeof(value);
  CHECK_HR(T, c.ctx->SetPrivateData(ordinary, sizeof(value), &value));
  CHECK_HR(T, c.ctx->GetPrivateData(ordinary, &size, &fetched));
  CHECK(T, fetched == value);
  // Reserved context GUIDs remain ordinary data on non-context objects.
  D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 16;
  Com<ID3D11Buffer> buffer; CHECK_HR(T, c.device->CreateBuffer(&bd, nullptr, &buffer));
  CHECK_HR(T, buffer->SetPrivateData(stateGuid, sizeof(value), &value));
  fetched = 0; size = sizeof(fetched);
  CHECK_HR(T, buffer->GetPrivateData(stateGuid, &size, &fetched));
  CHECK(T, fetched == value);
  CHECK_HR(T, setDither(c.ctx, 0));
  if (unsupported) return;
  Com<ID3D11DeviceContext> deferred; CHECK_HR(T, c.device->CreateDeferredContext(0, &deferred));
  CHECK_HR(T, setDither(deferred, 1));
  CHECK(T, getDither(c.ctx) == 0 && getDither(deferred) == 1);
  Com<ID3D11CommandList> list; CHECK_HR(T, deferred->FinishCommandList(TRUE, &list));
  CHECK(T, getDither(deferred) == 1);
  c.ctx->ExecuteCommandList(list, TRUE);
  CHECK(T, getDither(c.ctx) == 0);
  CHECK_HR(T, setDither(c.ctx, 1));
  c.ctx->ExecuteCommandList(list, FALSE);
  CHECK(T, getDither(c.ctx) == 0);
  Com<ID3D11CommandList> second; CHECK_HR(T, deferred->FinishCommandList(FALSE, &second));
  CHECK(T, getDither(deferred) == 0);
  CHECK_HR(T, setDither(c.ctx, 1)); c.ctx->ClearState();
  CHECK(T, getDither(c.ctx) == 0);
  Com<ID3D11Device1> device1; Com<ID3D11DeviceContext1> ctx1;
  CHECK_HR(T, c.device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1)));
  CHECK_HR(T, c.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)));
  Com<ID3DDeviceContextState> clean, previous, previousClean;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0, selected;
  CHECK_HR(T, device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
      __uuidof(ID3D11Device), &selected, &clean));
  CHECK_HR(T, setDither(c.ctx, 1));
  ctx1->SwapDeviceContextState(clean, &previous);
  CHECK(T, getDither(c.ctx) == 0);
  ctx1->SwapDeviceContextState(previous, &previousClean);
  CHECK(T, getDither(c.ctx) == 1);
  c.ctx->ClearState();
}

struct Render {
  Ctx& c; UINT bytes; DXGI_FORMAT format;
  Com<ID3D11Texture2D> texture;
  Com<ID3D11RenderTargetView> view;
  Com<ID3D11Buffer> vertices;
  Com<ID3D11VertexShader> vs;
  Com<ID3D11PixelShader> ps;
  Com<ID3D11InputLayout> layout;
  Com<ID3D11RasterizerState> raster;
  bool valid = false;
  Render(Ctx& context, DXGI_FORMAT f, UINT b, UINT samples=1): c(context), bytes(b), format(f) {
    const char* T = "dither_setup";
    D3D11_TEXTURE2D_DESC td = {};
    td.Width=td.Height=32; td.MipLevels=td.ArraySize=1; td.SampleDesc.Count=samples;
    td.Format=f; td.BindFlags=D3D11_BIND_RENDER_TARGET;
    CHECK_HR(T,c.device->CreateTexture2D(&td,nullptr,&texture));
    CHECK_HR(T,c.device->CreateRenderTargetView(texture,nullptr,&view));
    CHECK_HR(T,c.device->CreateVertexShader(dxbc_vs_pass,sizeof(dxbc_vs_pass),nullptr,&vs));
    CHECK_HR(T,c.device->CreatePixelShader(dxbc_ps_color,sizeof(dxbc_ps_color),nullptr,&ps));
    CHECK_HR(T,c.device->CreateInputLayout(kLayout,3,dxbc_vs_pass,sizeof(dxbc_vs_pass),&layout));
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth=sizeof(kQuadFull); bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    CHECK_HR(T,c.device->CreateBuffer(&bd,nullptr,&vertices));
    D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.ScissorEnable=TRUE;
    CHECK_HR(T,c.device->CreateRasterizerState(&rd,&raster));
    valid = true;
  }
  void bind(ID3D11DeviceContext* ctx = nullptr) {
    if (!ctx) ctx=c.ctx;
    ctx->OMSetRenderTargets(1,&view,nullptr);
    D3D11_VIEWPORT vp={0,0,32,32,0,1}; ctx->RSSetViewports(1,&vp);
    RECT rect={0,0,32,32};ctx->RSSetScissorRects(1,&rect);ctx->RSSetState(raster);
    UINT stride=sizeof(Vertex),offset=0;ctx->IASetVertexBuffers(0,1,&vertices,&stride,&offset);
    ctx->IASetInputLayout(layout);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs,nullptr,0);ctx->PSSetShader(ps,nullptr,0);
  }
  void color(float r,float g,float b,float a=0.43f) {
    Vertex v[6];memcpy(v,kQuadFull,sizeof(v));for(auto& p:v){p.color[0]=r;p.color[1]=g;p.color[2]=b;p.color[3]=a;}
    c.ctx->UpdateSubresource(vertices,0,nullptr,v,0,0);
  }
  std::vector<uint8_t> read() {
    D3D11_TEXTURE2D_DESC td;texture->GetDesc(&td);
    if(td.SampleDesc.Count==1)return readback_tex2d(c,texture,0,bytes);
    td.SampleDesc.Count=1;Com<ID3D11Texture2D> resolved;
    if(FAILED(c.device->CreateTexture2D(&td,nullptr,&resolved)))return {};
    c.ctx->ResolveSubresource(resolved,0,texture,0,format);
    return readback_tex2d(c,resolved,0,bytes);
  }
};

static std::array<int,4> unpack(const std::vector<uint8_t>& image, size_t pixel, DXGI_FORMAT f) {
  uint32_t v=0;memcpy(&v,image.data()+pixel*2,2);
  if(f==DXGI_FORMAT_B5G6R5_UNORM)return {int(v>>11&31),int(v>>5&63),int(v&31),0};
  if(f==DXGI_FORMAT_B5G5R5A1_UNORM)return {int(v>>10&31),int(v>>5&31),int(v&31),int(v>>15)};
  return {int(v>>8&15),int(v>>4&15),int(v&15),int(v>>12)};
}
static void pixels(Ctx& c) {
  const char* T="dither_pixels";
  const struct {DXGI_FORMAT f;UINT bytes;bool low;} formats[]={
    {DXGI_FORMAT_B5G6R5_UNORM,2,true},{DXGI_FORMAT_B5G5R5A1_UNORM,2,true},
    {DXGI_FORMAT_B4G4R4A4_UNORM,2,true},{DXGI_FORMAT_B8G8R8A8_UNORM,4,false},
    {DXGI_FORMAT_R8G8B8A8_UNORM,4,false},{DXGI_FORMAT_R10G10B10A2_UNORM,4,false},
    {DXGI_FORMAT_R16G16B16A16_FLOAT,8,false},{DXGI_FORMAT_R32G32B32A32_FLOAT,16,false}};
  unsigned lowChanged=0;
  for(auto f:formats) {
    c.ctx->ClearState();Render rt(c,f.f,f.bytes);CHECK(T,rt.valid,continue);rt.bind();rt.color(.371f,.537f,.693f);
    const float clear[4]={.2f,.4f,.6f,.8f};
    CHECK_HR(T,setDither(c.ctx,0));c.ctx->ClearRenderTargetView(rt.view,clear);auto clearOff=rt.read();
    CHECK_HR(T,setDither(c.ctx,1));c.ctx->ClearRenderTargetView(rt.view,clear);auto clearOn=rt.read();
    CHECK(T,clearOff==clearOn && clearOff.size()==32*32*f.bytes);
    CHECK_HR(T,setDither(c.ctx,0));c.ctx->Draw(6,0);auto off=rt.read();
    CHECK_HR(T,setDither(c.ctx,1));c.ctx->Draw(6,0);auto on=rt.read();
    CHECK_HR(T,setDither(c.ctx,0));c.ctx->Draw(6,0);auto again=rt.read();
    CHECK(T,off==again && on.size()==off.size());
    emit("[PIXEL] format=%u off=%08x on=%08x",f.f,crc32_of(off.data(),off.size()),crc32_of(on.data(),on.size()));
    if(f.low && off.size()==32*32*f.bytes) {
      lowChanged += off!=on;bool bounds=true;std::set<std::array<int,4>> values;
      for(size_t p=0;p<1024;p++){auto a=unpack(off,p,f.f),b=unpack(on,p,f.f);values.insert(b);for(int k=0;k<4;k++)bounds &= std::abs(a[k]-b[k])<=1;}
      CHECK(T,bounds);if(off!=on)CHECK(T,values.size()>1);
      // Blend twice over destination pixels, exercising post-fragment output
      // and preserving masked channels regardless of dithering.
      D3D11_BLEND_DESC bd={};auto& b=bd.RenderTarget[0];b.BlendEnable=TRUE;
      b.SrcBlend=D3D11_BLEND_SRC_ALPHA;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;
      b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_ZERO;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;
      b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
      Com<ID3D11BlendState> blend;CHECK_HR(T,c.device->CreateBlendState(&bd,&blend));c.ctx->OMSetBlendState(blend,nullptr,~0u);
      CHECK_HR(T,setDither(c.ctx,1));c.ctx->ClearRenderTargetView(rt.view,clear);c.ctx->Draw(6,0);c.ctx->Draw(6,0);auto mixed=rt.read();
      bool mask=true,red=true;
      for(size_t p=0;p<1024;p++){auto dst=unpack(clearOn,p,f.f),out=unpack(mixed,p,f.f);mask &= dst[1]==out[1]&&dst[2]==out[2]&&dst[3]==out[3];float scale=f.f==DXGI_FORMAT_B4G4R4A4_UNORM?15.f:31.f;float ideal=(.371f*.43f+(dst[0]/scale)*.57f)*.57f+.371f*.43f;red &= std::abs(out[0]-ideal*scale)<=2.0f;}
      CHECK(T,mask&&red);c.ctx->OMSetBlendState(nullptr,nullptr,~0u);
    }
    // CopyResource must preserve raw bytes while dither is enabled.
    CHECK_HR(T,setDither(c.ctx,1));D3D11_TEXTURE2D_DESC td;rt.texture->GetDesc(&td);
    Com<ID3D11Texture2D> copy;CHECK_HR(T,c.device->CreateTexture2D(&td,nullptr,&copy));
    auto expected=rt.read();c.ctx->CopyResource(copy,rt.texture);
    CHECK(T,readback_tex2d(c,copy,0,f.bytes)==expected);
  }
  CHECK(T,lowChanged>0);
  c.ctx->ClearState();
  // Toggle within a framebuffer, without readback/clear between draws, so a
  // missing render-pass break cannot be hidden by the fixture itself.
  Render rt(c,DXGI_FORMAT_B5G6R5_UNORM,2);CHECK(T,rt.valid,return);rt.bind();rt.color(.371f,.537f,.693f);
  CHECK_HR(T,setDither(c.ctx,0));c.ctx->Draw(6,0);auto off=rt.read();
  CHECK_HR(T,setDither(c.ctx,1));c.ctx->Draw(6,0);auto on=rt.read();
  CHECK_HR(T,setDither(c.ctx,0));RECT left={0,0,16,32};c.ctx->RSSetScissorRects(1,&left);c.ctx->Draw(6,0);
  CHECK_HR(T,setDither(c.ctx,1));RECT right={16,0,32,32};c.ctx->RSSetScissorRects(1,&right);c.ctx->Draw(6,0);auto halves=rt.read();
  bool split=true;for(size_t y=0;y<32;y++)for(size_t x=0;x<32;x++){size_t p=(y*32+x)*2;auto& ref=x<16?off:on;split &= !memcmp(halves.data()+p,ref.data()+p,2);}
  CHECK(T,split);
  // ExecuteCommandList(TRUE) must restore the backend value, not just Get.
  Com<ID3D11DeviceContext> def;CHECK_HR(T,c.device->CreateDeferredContext(0,&def));
  CHECK_HR(T,setDither(def,1));rt.bind(def);def->Draw(6,0);Com<ID3D11CommandList> list;
  CHECK_HR(T,def->FinishCommandList(FALSE,&list));
  CHECK_HR(T,setDither(c.ctx,0));rt.bind();c.ctx->ExecuteCommandList(list,TRUE);c.ctx->Draw(6,0);
  CHECK(T,rt.read()==off);
  CHECK_HR(T,setDither(c.ctx,1));c.ctx->ExecuteCommandList(list,FALSE);rt.bind();c.ctx->Draw(6,0);
  CHECK(T,getDither(c.ctx)==0 && rt.read()==off);
  CHECK_HR(T,setDither(c.ctx,1));c.ctx->ClearState();rt.bind();c.ctx->Draw(6,0);
  CHECK(T,rt.read()==off);
  Com<ID3D11Device1> dev1;Com<ID3D11DeviceContext1> ctx1;
  CHECK_HR(T,c.device->QueryInterface(__uuidof(ID3D11Device1),reinterpret_cast<void**>(&dev1)));
  CHECK_HR(T,c.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1),reinterpret_cast<void**>(&ctx1)));
  Com<ID3DDeviceContextState> fresh,saved,savedFresh;
  D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0,selected;
  CHECK_HR(T,dev1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&selected,&fresh));
  CHECK_HR(T,setDither(c.ctx,1));
  ctx1->SwapDeviceContextState(fresh,&saved);rt.bind();c.ctx->Draw(6,0);
  CHECK(T,rt.read()==off);
  ctx1->SwapDeviceContextState(saved,&savedFresh);c.ctx->Draw(6,0);
  CHECK(T,rt.read()==on);
  // Rectangular clear uses an internal clear path that must not inherit dither.
  const float clearColor[4]={.371f,.537f,.693f,.43f};RECT rect={3,5,19,23};
  CHECK_HR(T,setDither(c.ctx,0));ctx1->ClearView(rt.view,clearColor,&rect,1);auto cleared=rt.read();
  CHECK_HR(T,setDither(c.ctx,1));ctx1->ClearView(rt.view,clearColor,&rect,1);
  CHECK(T,rt.read()==cleared);
  for(UINT samples:{2u,4u}) {
    c.ctx->ClearState();Render ms(c,DXGI_FORMAT_R8G8B8A8_UNORM,4,samples);CHECK(T,ms.valid,continue);
    ms.bind();ms.color(.371f,.537f,.693f);
    CHECK_HR(T,setDither(c.ctx,0));c.ctx->Draw(6,0);auto reference=ms.read();
    CHECK_HR(T,setDither(c.ctx,1));c.ctx->Draw(6,0);
    CHECK(T,reference.size()==4096 && ms.read()==reference);
  }
}
int main(int argc,char** argv) {
  g_out=stdout;Ctx c;bool unsupported=argc>1&&!strcmp(argv[1],"--unsupported");
  if(create_device(c)){controls(c,unsupported);if(!unsupported){dropPixelEnable = argc>1&&!strcmp(argv[1],"--drop-pixel-enable");pixels(c);}}
  emit("[DITHER-SUMMARY] passed=%d failed=%d",g_passes,g_fails);
  return g_fails?1:0;
}
