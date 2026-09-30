#if defined(TRITON9_TEST_STATE)
#include <d3d9.h>
#include <d3d11.h>
#include <cassert>
#include <initializer_list>
#include <cmath>
#include <climits>
#include <cstring>
#include <cstdio>
#define TRITON9_MAX_TEXTURE_STAGES 21u
#define TRITON9_MAX_PIXEL_SAMPLERS 16u
#define TRITON9_MAX_VERTEX_SAMPLERS 4u
#define TRITON9_VERTEX_SAMPLER_BASE 17u
#define D3DDDIERR_NOTAVAILABLE D3DERR_NOTAVAILABLE
#define D3DDDIFMT_D16 D3DFMT_D16
#define D3DDDIFMT_D16_LOCKABLE D3DFMT_D16_LOCKABLE
#define D3DDDIFMT_D24S8 D3DFMT_D24S8
#ifndef ZeroMemory
#define ZeroMemory(p,n) std::memset(p,0,n)
#endif
// DDI_ENUMS
static const UINT kDefaultColorWriteMask=15;
static const UINT kD3dDdiMaxVertexShaderInstructions=196;
static const UINT kD3dDdiMaxPixelShaderInstructions=197;
static const UINT kD3dInfiniteInstructions=UINT_MAX;
#define TRITON9_RENDER_STATE_COUNT 210u
#define D3DDDIERR_INVALIDCALL D3DERR_INVALIDCALL
struct D3DDDIARG_RENDERSTATE { D3DDDIRENDERSTATETYPE State; UINT Value; };
struct TRITON9_RESOURCE { D3DFORMAT format=D3DFMT_D24S8; };
struct TRITON9_DEVICE {
    ID3D11Device *hostDevice=nullptr;
    UINT renderStates[210]={};
    UINT textureStageStates[21][35]={};
    BOOL blendStateDirty=TRUE,depthStencilStateDirty=TRUE,rasterizerStateDirty=TRUE;
    BOOL samplerStatesDirty[21]={};
    ID3D11BlendState *blendState=nullptr;
    ID3D11DepthStencilState *depthStencilState=nullptr;
    ID3D11RasterizerState *rasterizerState=nullptr;
    ID3D11SamplerState *samplerStates[21]={};
    TRITON9_RESOURCE *depthStencil=nullptr;
    TRITON9_RESOURCE *renderTargets[4]={};
};
static HRESULT triton9MapDeviceFailure(TRITON9_DEVICE *,HRESULT hr) { return hr; }
// PRODUCTION_STATE_IMPLEMENTATION
static UINT bits(float f) { UINT u;std::memcpy(&u,&f,4);return u; }
#include "tritonBlitShaders.h"
static UINT render(TRITON9_DEVICE &d,ID3D11DeviceContext *c,DXGI_FORMAT fmt, bool swap,bool border,ID3D11BlendState *bs,ID3D11SamplerState *ss) {
 ID3D11Texture2D *source,*target,*read;ID3D11ShaderResourceView *srv;ID3D11RenderTargetView *rtv;ID3D11VertexShader *vs;ID3D11PixelShader *ps;ID3D11Buffer *cb;
 D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=4;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 UINT white[16];for(auto &p:white)p=0xff4080ff;D3D11_SUBRESOURCE_DATA data={white,16,64};assert(SUCCEEDED(d.hostDevice->CreateTexture2D(&td,&data,&source)));assert(SUCCEEDED(d.hostDevice->CreateShaderResourceView(source,nullptr,&srv)));
 td.Format=fmt;td.BindFlags=D3D11_BIND_RENDER_TARGET;assert(SUCCEEDED(d.hostDevice->CreateTexture2D(&td,nullptr,&target)));assert(SUCCEEDED(d.hostDevice->CreateRenderTargetView(target,nullptr,&rtv)));
 td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;assert(SUCCEEDED(d.hostDevice->CreateTexture2D(&td,nullptr,&read)));
 assert(SUCCEEDED(d.hostDevice->CreateVertexShader(g_tritonBlitVS,sizeof(g_tritonBlitVS),nullptr,&vs)));assert(SUCCEEDED(d.hostDevice->CreatePixelShader(swap?g_tritonBlitSwapRBPS:g_tritonBlitPS,swap?sizeof(g_tritonBlitSwapRBPS):sizeof(g_tritonBlitPS),nullptr,&ps)));
 float st[4]={1,1,0,0};if(border){st[0]=st[1]=0;st[2]=st[3]=2;};D3D11_BUFFER_DESC bd={};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;data={st,16,16};assert(SUCCEEDED(d.hostDevice->CreateBuffer(&bd,&data,&cb)));
 float zero[4]={0,0,0,0};c->ClearState();c->ClearRenderTargetView(rtv,zero);c->OMSetRenderTargets(1,&rtv,nullptr);c->OMSetBlendState(bs,nullptr,~0u);D3D11_VIEWPORT vp={0,0,4,4,0,1};c->RSSetViewports(1,&vp);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(vs,nullptr,0);c->PSSetShader(ps,nullptr,0);c->PSSetShaderResources(0,1,&srv);c->PSSetSamplers(0,1,&ss);c->PSSetConstantBuffers(0,1,&cb);c->Draw(3,0);c->ClearState();c->CopyResource(read,target);D3D11_MAPPED_SUBRESOURCE map;assert(SUCCEEDED(c->Map(read,0,D3D11_MAP_READ,0,&map)));UINT value=((UINT*)map.pData)[0];c->Unmap(read,0);source->Release();target->Release();read->Release();srv->Release();rtv->Release();vs->Release();ps->Release();cb->Release();return value;
}

int main() {
    TRITON9_DEVICE d;
    ID3D11DeviceContext *context=nullptr;
    D3D_FEATURE_LEVEL level;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
        nullptr,0,D3D11_SDK_VERSION,&d.hostDevice,&level,&context)));
    for (UINT i=0;i<16;++i) assert(triton9SamplerIndex(i)==i);
    for (UINT i=0;i<4;++i) assert(triton9SamplerIndex(257+i)==17+i);
    assert(triton9SamplerIndex(256)==16);
    assert(triton9SamplerIndex(16)==UINT_MAX);
    assert(triton9SamplerIndex(261)==UINT_MAX);
    d.renderStates[D3DDDIRS_SRCBLEND]=D3DBLEND_SRCCOLOR;
    d.renderStates[D3DDDIRS_DESTBLEND]=D3DBLEND_INVSRCCOLOR;
    d.renderStates[D3DDDIRS_BLENDOP]=D3DBLENDOP_ADD;
    d.renderStates[D3DDDIRS_ALPHABLENDENABLE]=TRUE;
    d.renderStates[D3DDDIRS_COLORWRITEENABLE]=1;
    d.renderStates[D3DDDIRS_COLORWRITEENABLE1]=2;
    d.renderStates[D3DDDIRS_COLORWRITEENABLE2]=4;
    d.renderStates[D3DDDIRS_COLORWRITEENABLE3]=8;
    assert(triton9CreateBlendState(&d)==S_OK);
    D3D11_BLEND_DESC blend;d.blendState->GetDesc(&blend);
    assert(blend.IndependentBlendEnable);
    for (UINT i=0;i<4;++i) {
        assert(blend.RenderTarget[i].RenderTargetWriteMask==(1u<<i));
        assert(blend.RenderTarget[i].BlendEnable);
        assert(blend.RenderTarget[i].SrcBlendAlpha==D3D11_BLEND_SRC_ALPHA);
        assert(blend.RenderTarget[i].DestBlendAlpha==D3D11_BLEND_INV_SRC_ALPHA);
    }
    for (UINT mode : {UINT(D3DBLEND_BOTHSRCALPHA), UINT(D3DBLEND_BOTHINVSRCALPHA)}) {
        D3DDDIARG_RENDERSTATE state = {D3DDDIRS_SRCBLEND, mode};
        assert(triton9ValidateRenderState(&state) == S_OK);
        state.State = D3DDDIRS_DESTBLEND;
        assert(FAILED(triton9ValidateRenderState(&state)));
        state.State = D3DDDIRS_SRCBLENDALPHA;
        assert(FAILED(triton9ValidateRenderState(&state)));
        d.renderStates[D3DDDIRS_SRCBLEND] = mode;
        d.renderStates[D3DDDIRS_DESTBLEND] = D3DBLEND_ZERO;
        for (UINT separate = 0; separate < 2; ++separate) {
            d.renderStates[D3DDDIRS_SEPARATEALPHABLENDENABLE] = separate;
            d.renderStates[D3DDDIRS_SRCBLENDALPHA] = D3DBLEND_ONE;
            d.renderStates[D3DDDIRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
            d.renderStates[D3DDDIRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
            d.blendStateDirty = TRUE;
            assert(triton9CreateBlendState(&d) == S_OK);
            d.blendState->GetDesc(&blend);
            const auto src = mode == D3DBLEND_BOTHSRCALPHA
                ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_INV_SRC_ALPHA;
            const auto dst = mode == D3DBLEND_BOTHSRCALPHA
                ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_SRC_ALPHA;
            assert(blend.RenderTarget[0].SrcBlend == src);
            assert(blend.RenderTarget[0].DestBlend == dst);
            assert(blend.RenderTarget[0].SrcBlendAlpha == (separate ? D3D11_BLEND_ONE : src));
            assert(blend.RenderTarget[0].DestBlendAlpha == (separate ? D3D11_BLEND_ZERO : dst));
        }
    }
    d.renderStates[D3DDDIRS_SEPARATEALPHABLENDENABLE] = FALSE;
    TRITON9_RESOURCE outputs[4];
    const D3DFORMAT opaqueFormats[]={D3DFMT_X8R8G8B8,D3DFMT_X8B8G8R8,
        D3DFMT_X1R5G5B5,D3DFMT_X4R4G4B4,D3DFMT_R5G6B5};
    for (D3DFORMAT format:opaqueFormats) {
        outputs[0].format=format;d.renderTargets[0]=&outputs[0];
        outputs[1].format=D3DFMT_A8R8G8B8;d.renderTargets[1]=&outputs[1];
        d.renderStates[D3DDDIRS_SRCBLEND]=D3DBLEND_DESTALPHA;
        d.renderStates[D3DDDIRS_DESTBLEND]=D3DBLEND_INVDESTALPHA;
        d.blendStateDirty=TRUE;assert(triton9CreateBlendState(&d)==S_OK);
        d.blendState->GetDesc(&blend);
        assert(blend.RenderTarget[0].SrcBlend==D3D11_BLEND_ONE);
        assert(blend.RenderTarget[0].DestBlend==D3D11_BLEND_ZERO);
        assert(blend.RenderTarget[1].SrcBlend==D3D11_BLEND_DEST_ALPHA);
        assert(blend.RenderTarget[1].DestBlend==D3D11_BLEND_INV_DEST_ALPHA);
        d.renderStates[D3DDDIRS_SRCBLEND]=D3DBLEND_SRCALPHASAT;
        d.blendStateDirty=TRUE;assert(triton9CreateBlendState(&d)==S_OK);
        d.blendState->GetDesc(&blend);
        assert(blend.RenderTarget[0].SrcBlend==D3D11_BLEND_ZERO);
        assert(blend.RenderTarget[0].SrcBlendAlpha==D3D11_BLEND_ONE);
        assert(blend.RenderTarget[1].SrcBlend==D3D11_BLEND_SRC_ALPHA_SAT);
    }
    d.renderStates[D3DDDIRS_FILLMODE]=D3DFILL_SOLID;
    d.renderStates[D3DDDIRS_CULLMODE]=D3DCULL_CCW;
    d.renderStates[D3DDDIRS_CLIPPING]=FALSE;
    d.renderStates[D3DDDIRS_MULTISAMPLEANTIALIAS]=TRUE;
    d.renderStates[D3DDDIRS_ANTIALIASEDLINEENABLE]=TRUE;
    d.renderStates[D3DDDIRS_DEPTHBIAS]=bits(1.0f/65536);
    d.renderStates[D3DDDIRS_SLOPESCALEDEPTHBIAS]=bits(2.25f);
    TRITON9_RESOURCE depth;depth.format=D3DFMT_D16;d.depthStencil=&depth;
    assert(triton9CreateRasterizerState(&d)==S_OK);
    D3D11_RASTERIZER_DESC raster;d.rasterizerState->GetDesc(&raster);
    assert(raster.DepthBias==1 && raster.SlopeScaledDepthBias==2.25f);
    assert(!raster.DepthClipEnable && raster.MultisampleEnable && raster.AntialiasedLineEnable);
    depth.format=D3DFMT_D16_LOCKABLE;d.rasterizerStateDirty=TRUE;
    assert(triton9CreateRasterizerState(&d)==S_OK);
    d.rasterizerState->GetDesc(&raster);assert(raster.DepthBias==1);
    depth.format=D3DFMT_D24S8;d.rasterizerStateDirty=TRUE;
    assert(triton9CreateRasterizerState(&d)==S_OK);
    d.rasterizerState->GetDesc(&raster);assert(raster.DepthBias==256);
    d.renderStates[D3DDDIRS_DEPTHBIAS]=bits(-1.0f/16777216);d.rasterizerStateDirty=TRUE;
    assert(triton9CreateRasterizerState(&d)==S_OK);
    d.rasterizerState->GetDesc(&raster);assert(raster.DepthBias==-1);
    d.renderStates[D3DDDIRS_FILLMODE]=D3DFILL_POINT;d.rasterizerStateDirty=TRUE;
    assert(triton9CreateRasterizerState(&d)==S_OK);
    d.rasterizerState->GetDesc(&raster);assert(raster.FillMode==D3D11_FILL_SOLID);
    UINT *s=d.textureStageStates[17];
    s[D3DDDITSS_ADDRESSU]=s[D3DDDITSS_ADDRESSV]=s[D3DDDITSS_ADDRESSW]=D3DTADDRESS_CLAMP;
    s[D3DDDITSS_MINFILTER]=s[D3DDDITSS_MAGFILTER]=D3DTEXF_ANISOTROPIC;
    s[D3DDDITSS_MIPFILTER]=D3DTEXF_NONE;
    s[D3DDDITSS_MAXANISOTROPY]=16;s[D3DDDITSS_MAXMIPLEVEL]=3;
    s[D3DDDITSS_MIPMAPLODBIAS]=bits(-0.5f);
    assert(triton9CreateSamplerState(&d,17)==S_OK);
    D3D11_SAMPLER_DESC sampler;d.samplerStates[17]->GetDesc(&sampler);
    assert(sampler.Filter==D3D11_FILTER_ANISOTROPIC && sampler.MaxAnisotropy==16);
    assert(sampler.MinLOD==3 && sampler.MaxLOD==3 && sampler.MipLODBias==-0.5f);
    // Render through the extracted production blend state. Destination storage
    // starts with alpha zero, so a missing X-format substitution renders black.
    d.renderStates[D3DDDIRS_SRCBLEND]=D3DBLEND_DESTALPHA;
    d.renderStates[D3DDDIRS_DESTBLEND]=D3DBLEND_ZERO;
    d.renderStates[D3DDDIRS_COLORWRITEENABLE]=15;
    outputs[0].format=D3DFMT_X8B8G8R8;d.blendStateDirty=TRUE;
    assert(triton9CreateBlendState(&d)==S_OK);
    s[D3DDDITSS_MAXMIPLEVEL]=0;s[D3DDDITSS_MIPMAPLODBIAS]=0;
    d.samplerStatesDirty[17]=TRUE;assert(triton9CreateSamplerState(&d,17)==S_OK);
    UINT opaquePixel=render(d,context,DXGI_FORMAT_R8G8B8A8_UNORM,false,false,
        d.blendState,d.samplerStates[17]);
    assert((opaquePixel&0xffffffu)==0x4080ffu);
    outputs[0].format=D3DFMT_A8B8G8R8;d.blendStateDirty=TRUE;
    assert(triton9CreateBlendState(&d)==S_OK);
    UINT alphaPixel=render(d,context,DXGI_FORMAT_R8G8B8A8_UNORM,false,false,
        d.blendState,d.samplerStates[17]);
    assert((alphaPixel&0xffffffu)==0);
    s[D3DDDITSS_MAXANISOTROPY]=0;d.samplerStatesDirty[17]=TRUE;
    assert(triton9CreateSamplerState(&d,17)==D3DDDIERR_NOTAVAILABLE);
    s[D3DDDITSS_MAXANISOTROPY]=17;
    assert(triton9CreateSamplerState(&d,17)==D3DDDIERR_NOTAVAILABLE);
    s[D3DDDITSS_MAXANISOTROPY]=1;s[D3DDDITSS_MIPMAPLODBIAS]=0x7fc00000;
    assert(triton9CreateSamplerState(&d,17)==D3DDDIERR_NOTAVAILABLE);
    for (UINT op=D3DTOP_DISABLE;op<=D3DTOP_LERP;++op)
        assert(triton9ValidFixedOperation(op));
    assert(!triton9ValidFixedOperation(0) && !triton9ValidFixedOperation(27));
    for (UINT argument=0;argument<=D3DTA_CONSTANT;++argument)
        assert(triton9ValidFixedArgument(argument|D3DTA_COMPLEMENT|D3DTA_ALPHAREPLICATE));
    assert(!triton9ValidFixedArgument(7) && !triton9ValidFixedArgument(64));
    const struct {D3DDDIRENDERSTATETYPE state;UINT value;bool valid;} values[]={
        {D3DDDIRS_FILLMODE,D3DFILL_POINT,true},
        {D3DDDIRS_SHADEMODE,D3DSHADE_FLAT,true},
        {D3DDDIRS_SHADEMODE,0,false},
        {D3DDDIRS_POINTSPRITEENABLE,1,true},
        {D3DDDIRS_POINTSCALEENABLE,1,true},
        {D3DDDIRS_NORMALIZENORMALS,1,true},
        {D3DDDIRS_INDEXEDVERTEXBLENDENABLE,1,true},
        {D3DDDIRS_RANGEFOGENABLE,1,true},
        {D3DDDIRS_FOGTABLEMODE,D3DFOG_EXP2,true},
        {D3DDDIRS_FOGTABLEMODE,4,false},
        {D3DDDIRS_CLIPPLANEENABLE,63,true},
        {D3DDDIRS_CLIPPLANEENABLE,64,false},
        {D3DDDIRS_VERTEXBLEND,D3DVBF_3WEIGHTS,true},
        {D3DDDIRS_VERTEXBLEND,D3DVBF_0WEIGHTS,true},
        {D3DDDIRS_VERTEXBLEND,D3DVBF_TWEENING,true},
        {D3DDDIRS_VERTEXBLEND,4,false},
        {D3DDDIRS_SPECULARMATERIALSOURCE,D3DMCS_COLOR2,true},
        {D3DDDIRS_AMBIENTMATERIALSOURCE,D3DMCS_COLOR1,true},
        {D3DDDIRS_DIFFUSEMATERIALSOURCE,3,false},
        {D3DDDIRS_WRAP15,15,true},
        {D3DDDIRS_WRAP0,16,false},
        {D3DDDIRS_TWEENFACTOR,bits(0.5f),true},
        {D3DDDIRS_TWEENFACTOR,0x7fc00000,false},
        {D3DDDIRS_POINTSIZE,bits(-1.0f),false},
        {D3DDDIRS_POINTSIZE_MAX,0,true},
        {D3DDDIRS_MULTISAMPLEMASK,0xaaaaaaaa,true},
        {D3DDDIRS_COLORWRITEENABLE3,8,true},
        {D3DDDIRS_COLORWRITEENABLE2,16,false},
    };
    for (const auto &v:values) {
        D3DDDIARG_RENDERSTATE state={v.state,v.value};
        assert(SUCCEEDED(triton9ValidateRenderState(&state))==v.valid);
    }
    d.samplerStates[17]->Release();d.blendState->Release();d.rasterizerState->Release();
    context->Release();d.hostDevice->Release();
    std::puts("D3D9 native state behavior passed");
}
#else
/* SPDX-License-Identifier: MIT
 * Compile the production query implementation with a deterministic host.
 * Data sent to the runtime, callback ordering and failure publication are
 * observed independently of the translation's internal state.
 */
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
using UINT = uint32_t;
using DWORD = uint32_t;
using UINT64 = uint64_t;
using SIZE_T = size_t;
using BOOL = int32_t;
using HRESULT = int32_t;
using HANDLE = void *;
#define APIENTRY
#define TRUE 1
#define FALSE 0
#define S_OK 0
#define S_FALSE 1
#define E_FAIL HRESULT(0x80004005u)
#define E_INVALIDARG HRESULT(0x80070057u)
#define E_OUTOFMEMORY HRESULT(0x8007000eu)
#define D3DDDIERR_NOTAVAILABLE HRESULT(0x8876086au)
#define D3DDDIERR_INVALIDCALL HRESULT(0x8876086cu)
#define D3DDDIERR_DEVICEREMOVED HRESULT(0x88760870u)
#define FAILED(hr) ((hr) < 0)
#define SUCCEEDED(hr) ((hr) >= 0)
#define HEAP_ZERO_MEMORY 1
#ifndef ZeroMemory
#define ZeroMemory(p,n) std::memset(p,0,n)
#endif
#define CopyMemory(p,q,n) std::memcpy(p,q,n)
static int allocations, hostStarts;
static bool failAllocation;
static void *GetProcessHeap() { return nullptr; }
static void *HeapAlloc(void *, UINT, SIZE_T n) {
    if (failAllocation) return nullptr;
    void *p = std::calloc(1,n);
    if (p) ++allocations;
    return p;
}
static void HeapFree(void *, UINT, void *p) {
    if (p) --allocations;
    std::free(p);
}
static void EnterCriticalSection(int *) {}
static void LeaveCriticalSection(int *) {}
enum D3DDDIQUERYTYPE : int32_t {
    D3DDDIQUERYTYPE_EVENT=8, D3DDDIQUERYTYPE_OCCLUSION=9,
    D3DDDIQUERYTYPE_TIMESTAMP=10, D3DDDIQUERYTYPE_TIMESTAMPDISJOINT=11,
    D3DDDIQUERYTYPE_TIMESTAMPFREQ=12
};
enum D3D11_QUERY : int32_t {
    D3D11_QUERY_EVENT=0, D3D11_QUERY_OCCLUSION=1,
    D3D11_QUERY_TIMESTAMP=2, D3D11_QUERY_TIMESTAMP_DISJOINT=3
};
struct D3D11_QUERY_DESC { D3D11_QUERY Query; UINT MiscFlags; };
struct D3D11_QUERY_DATA_TIMESTAMP_DISJOINT { UINT64 Frequency; BOOL Disjoint; };
struct D3DDDIARG_CREATEQUERY { D3DDDIQUERYTYPE QueryType; HANDLE hQuery; };
struct QueryFlags {
    union { struct { UINT End:1; UINT Begin:1; }; UINT Value; };
};
struct D3DDDIARG_ISSUEQUERY { HANDLE hQuery; QueryFlags Flags; };
struct D3DDDIARG_GETQUERYDATA { HANDLE hQuery; void *pData; };
struct ID3D11Query {
    D3D11_QUERY type;
    static int live;
    explicit ID3D11Query(D3D11_QUERY t):type(t) { ++live; }
    void Release() { --live; delete this; }
};
int ID3D11Query::live;
struct HostDevice {
    HRESULT createResult=S_OK;
    HRESULT CreateQuery(const D3D11_QUERY_DESC *d, ID3D11Query **q) {
        if (FAILED(createResult)) return createResult;
        *q = new ID3D11Query(d->Query);
        return S_OK;
    }
};
struct HostContext {
    std::vector<char> commands;
    HRESULT readResult=S_OK;
    BOOL event=TRUE, disjoint=FALSE;
    UINT64 ticks=0x1234567887654321ULL, frequency=1000000;
    void Begin(ID3D11Query *) { commands.push_back('B'); }
    void End(ID3D11Query *) { commands.push_back('E'); }
    HRESULT GetData(ID3D11Query *q, void *p, UINT n, UINT flags) {
        assert(flags==0);
        if (readResult!=S_OK) return readResult;
        if (q->type==D3D11_QUERY_EVENT) {
            assert(n==sizeof(BOOL)); std::memcpy(p,&event,n);
        } else if (q->type==D3D11_QUERY_TIMESTAMP_DISJOINT) {
            assert(n==sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT));
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d={frequency,disjoint};
            std::memcpy(p,&d,n);
        } else {
            assert(n==sizeof(UINT64)); std::memcpy(p,&ticks,n);
        }
        return S_OK;
    }
};
struct TRITON9_DEVICE {
    BOOL shaderLockInitialized=TRUE, deviceLost=FALSE;
    int shaderLock=0;
    HostDevice *hostDevice;
    HostContext *hostContext;
    void *queries=nullptr;
    HRESULT status=S_OK;
};
static HRESULT triton9EnsureHostDevice(TRITON9_DEVICE *) { ++hostStarts; return S_OK; }
static HRESULT triton9CheckHostDevice(TRITON9_DEVICE *d) { return d->status; }
static HRESULT triton9MapDeviceFailure(TRITON9_DEVICE *, HRESULT hr) { return hr; }
// PRODUCTION_QUERY_IMPLEMENTATION
static HANDLE create(TRITON9_DEVICE *d,D3DDDIQUERYTYPE type) {
    D3DDDIARG_CREATEQUERY a={type,nullptr};
    assert(triton9CreateQuery(d,&a)==S_OK && a.hQuery);
    return a.hQuery;
}
static HRESULT issue(TRITON9_DEVICE *d,HANDLE q,UINT flags) {
    D3DDDIARG_ISSUEQUERY a={q,{}}; a.Flags.Value=flags;
    return triton9IssueQuery(d,&a);
}
static HRESULT read(TRITON9_DEVICE *d,HANDLE q,void *p) {
    D3DDDIARG_GETQUERYDATA a={q,p};return triton9GetQueryData(d,&a);
}
int main() {
    HostDevice host;HostContext ctx;TRITON9_DEVICE d;
    d.hostDevice=&host;d.hostContext=&ctx;
    D3DDDIARG_CREATEQUERY bad={static_cast<D3DDDIQUERYTYPE>(123),nullptr};
    assert(triton9CreateQuery(&d,&bad)==D3DDDIERR_NOTAVAILABLE && !hostStarts);
    failAllocation=true;
    bad.QueryType=D3DDDIQUERYTYPE_EVENT;
    assert(triton9CreateQuery(&d,&bad)==E_OUTOFMEMORY);
    failAllocation=false;
    host.createResult=E_FAIL;
    assert(triton9CreateQuery(&d,&bad)==E_FAIL && !allocations && !ID3D11Query::live);
    host.createResult=S_OK;
    HANDLE event=create(&d,D3DDDIQUERYTYPE_EVENT);
    assert(issue(&d,event,2)==D3DDDIERR_INVALIDCALL);
    assert(issue(&d,event,3)==D3DDDIERR_INVALIDCALL);
    assert(issue(&d,event,1)==S_OK);
    UINT64 output=~UINT64(0);
    ctx.event=FALSE;
    assert(read(&d,event,&output)==S_FALSE && output==~UINT64(0));
    ctx.event=TRUE;
    assert(read(&d,event,&output)==S_OK && output==0xffffffff00000001ULL);
    HANDLE stamp=create(&d,D3DDDIQUERYTYPE_TIMESTAMP);
    assert(issue(&d,stamp,2)==D3DDDIERR_INVALIDCALL);
    assert(issue(&d,stamp,1)==S_OK);
    assert(read(&d,stamp,&output)==S_OK && output==ctx.ticks);
    assert(read(&d,event,nullptr)==S_OK);
    assert(read(&d,stamp,nullptr)==S_OK);
    HANDLE freq=create(&d,D3DDDIQUERYTYPE_TIMESTAMPFREQ);
    ctx.commands.clear();
    assert(issue(&d,freq,1)==S_OK && ctx.commands==std::vector<char>({'B','E'}));
    assert(read(&d,freq,&output)==S_OK && output==ctx.frequency);
    ctx.frequency=0;output=99;
    assert(read(&d,freq,&output)==E_FAIL && output==99);
    ctx.frequency=1000000;
    HANDLE disjoint=create(&d,D3DDDIQUERYTYPE_TIMESTAMPDISJOINT);
    assert(issue(&d,disjoint,1)==D3DDDIERR_INVALIDCALL);
    assert(issue(&d,disjoint,2)==S_OK);
    assert(issue(&d,disjoint,2)==D3DDDIERR_INVALIDCALL);
    assert(issue(&d,disjoint,1)==S_OK);
    ctx.disjoint=TRUE;output=~UINT64(0);
    assert(read(&d,disjoint,&output)==S_OK && output==0xffffffff00000001ULL);
    HANDLE occ=create(&d,D3DDDIQUERYTYPE_OCCLUSION);
    assert(issue(&d,occ,2)==S_OK && issue(&d,occ,1)==S_OK);
    output=0;
    assert(read(&d,occ,&output)==S_OK && output==UINT32_MAX);
    ctx.readResult=S_FALSE;output=99;
    assert(read(&d,stamp,&output)==S_FALSE && output==99);
    assert(read(&d,stamp,nullptr)==S_FALSE);
    ctx.readResult=E_FAIL;
    assert(read(&d,stamp,&output)==E_FAIL && output==99);
    ctx.readResult=S_OK;d.status=E_FAIL;
    assert(read(&d,stamp,&output)==E_FAIL && output==99);
    d.status=S_OK;
    assert(triton9DestroyQuery(&d,stamp)==S_OK);
    assert(issue(&d,stamp,1)==D3DDDIERR_INVALIDCALL);
    triton9DestroyAllQueries(&d);
    assert(!allocations && !ID3D11Query::live && !d.queries);
    std::puts("D3D9 query behavior passed");
}

#endif
