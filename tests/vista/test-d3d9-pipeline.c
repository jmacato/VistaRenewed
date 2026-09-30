/* SPDX-License-Identifier: MIT
 * Public-runtime oracle for query payloads and independent MRT write masks. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

static unsigned failures, checks;
static IDirect3DDevice9 *device;
#define EXPECT(c) do {++checks;if(!(c)){++failures;printf("FAIL line=%u %s\n",(unsigned)__LINE__,#c);}} while(0)
#define HR(c) do {HRESULT result_=(c);++checks;if(FAILED(result_)){++failures;printf("FAIL line=%u %s hr=%08lx\n",(unsigned)__LINE__,#c,(unsigned long)result_);goto done;}} while(0)
#define RELEASE(p) do {if(p){IUnknown_Release((IUnknown *)(p));(p)=NULL;}} while(0)

static HRESULT poll(IDirect3DQuery9 *query, void *data, DWORD size)
{
    DWORD start=GetTickCount();HRESULT hr;
    do {
        hr=IDirect3DQuery9_GetData(query,data,size,D3DGETDATA_FLUSH);
        if(hr!=S_FALSE)return hr;
        Sleep(1);
    } while(GetTickCount()-start<15000);
    printf("FAIL query timeout type=%u\n",(unsigned)IDirect3DQuery9_GetType(query));
    return E_FAIL;
}

static void queries(void)
{
    IDirect3DQuery9 *event=NULL,*frequency=NULL,*disjoint=NULL,*first=NULL,*last=NULL;
    struct {BOOL value;DWORD canary;} boolean={FALSE,0xabcdef12};
    UINT64 hz=0,start=0,end=0;
    HR(IDirect3DDevice9_CreateQuery(device,D3DQUERYTYPE_EVENT,&event));
    HR(IDirect3DDevice9_CreateQuery(device,D3DQUERYTYPE_TIMESTAMPFREQ,&frequency));
    HR(IDirect3DDevice9_CreateQuery(device,D3DQUERYTYPE_TIMESTAMPDISJOINT,&disjoint));
    HR(IDirect3DDevice9_CreateQuery(device,D3DQUERYTYPE_TIMESTAMP,&first));
    HR(IDirect3DDevice9_CreateQuery(device,D3DQUERYTYPE_TIMESTAMP,&last));
    EXPECT(IDirect3DQuery9_GetDataSize(event)==sizeof(BOOL));
    EXPECT(IDirect3DQuery9_GetDataSize(frequency)==sizeof(UINT64));
    EXPECT(IDirect3DQuery9_GetDataSize(disjoint)==sizeof(BOOL));
    HR(IDirect3DQuery9_Issue(disjoint,D3DISSUE_BEGIN));
    HR(IDirect3DQuery9_Issue(first,D3DISSUE_END));
    HR(IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0xff123456,1,0));
    HR(IDirect3DQuery9_Issue(last,D3DISSUE_END));
    HR(IDirect3DQuery9_Issue(disjoint,D3DISSUE_END));
    HR(IDirect3DQuery9_Issue(frequency,D3DISSUE_END));
    HR(IDirect3DQuery9_Issue(event,D3DISSUE_END));
    HR(poll(event,NULL,0));
    HR(poll(event,&boolean.value,sizeof(BOOL)));
    EXPECT(boolean.value && boolean.canary==0xabcdef12);
    HR(poll(frequency,&hz,sizeof(hz)));EXPECT(hz!=0);
    HR(poll(first,&start,sizeof(start)));HR(poll(last,&end,sizeof(end)));
    HR(poll(disjoint,&boolean.value,sizeof(BOOL)));
    EXPECT(boolean.canary==0xabcdef12);
    if(!boolean.value)EXPECT(end>=start);
    printf("QUERY hz=%llu first=%llu last=%llu disjoint=%u\n",
           (unsigned long long)hz,(unsigned long long)start,
           (unsigned long long)end,(unsigned)boolean.value);
    /* Reissue completed queries: the first completion cannot satisfy a later
     * instance or corrupt the runtime's BOOL-sized output slot. */
    for(UINT i=0;i<8;++i) {
        boolean.value=FALSE;
        HR(IDirect3DQuery9_Issue(event,D3DISSUE_END));
        HR(poll(event,&boolean.value,sizeof(BOOL)));
        EXPECT(boolean.value && boolean.canary==0xabcdef12);
    }
done:
    RELEASE(last);RELEASE(first);RELEASE(disjoint);RELEASE(frequency);RELEASE(event);
}

static void mrt(void)
{
    static const DWORD shader[]={0xffff0200,
        0x02000001,0x800f0800,0xa0e40000,
        0x02000001,0x800f0801,0xa0e40001,
        0x02000001,0x800f0802,0xa0e40002,
        0x02000001,0x800f0803,0xa0e40003,0x0000ffff};
    static const D3DRENDERSTATETYPE masks[]={D3DRS_COLORWRITEENABLE,
        D3DRS_COLORWRITEENABLE1,D3DRS_COLORWRITEENABLE2,D3DRS_COLORWRITEENABLE3};
    static const DWORD expected[]={0x11333344,0x11226644,0x11223399,0xcc223344};
    const float colors[16]={.2f,.4f,.6f,.8f,.2f,.4f,.6f,.8f,
                           .2f,.4f,.6f,.8f,.2f,.4f,.6f,.8f};
    const struct {float x,y,z,w;} vertices[]={{-.5f,-.5f,.5f,1},
        {63.5f,-.5f,.5f,1},{-.5f,63.5f,.5f,1}};
    IDirect3DSurface9 *targets[4]={0},*cpu=NULL,*old=NULL;
    IDirect3DPixelShader9 *ps=NULL;IDirect3DStateBlock9 *states=NULL;
    IDirect3DQuery9 *occlusion=NULL;
    RECT scissor={4,4,12,12};D3DVIEWPORT9 viewport={0,0,32,32,0,1};
    HR(IDirect3DDevice9_CreateStateBlock(device,D3DSBT_ALL,&states));
    HR(IDirect3DDevice9_GetRenderTarget(device,0,&old));
    HR(IDirect3DDevice9_SetDepthStencilSurface(device,NULL));
    for(UINT i=0;i<4;++i) {
        HR(IDirect3DDevice9_CreateRenderTarget(device,32,32,D3DFMT_A8R8G8B8,
            D3DMULTISAMPLE_NONE,0,FALSE,&targets[i],NULL));
        HR(IDirect3DDevice9_SetRenderTarget(device,i,targets[i]));
        HR(IDirect3DDevice9_SetRenderState(device,masks[i],1u<<i));
    }
    HR(IDirect3DDevice9_SetViewport(device,&viewport));
    HR(IDirect3DDevice9_CreateOffscreenPlainSurface(device,32,32,D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM,&cpu,NULL));
    HR(IDirect3DDevice9_CreatePixelShader(device,shader,&ps));
    HR(IDirect3DDevice9_SetPixelShader(device,ps));
    HR(IDirect3DDevice9_SetPixelShaderConstantF(device,0,colors,4));
    HR(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZRHW));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_CULLMODE,D3DCULL_NONE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHABLENDENABLE,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHATESTENABLE,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_LIGHTING,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_SCISSORTESTENABLE,FALSE));
    HR(IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0x11223344,1,0));
    HR(IDirect3DDevice9_SetScissorRect(device,&scissor));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_SCISSORTESTENABLE,TRUE));
    HR(IDirect3DDevice9_CreateQuery(device,D3DQUERYTYPE_OCCLUSION,&occlusion));
    HR(IDirect3DQuery9_Issue(occlusion,D3DISSUE_BEGIN));
    HR(IDirect3DDevice9_BeginScene(device));
    HRESULT draw=IDirect3DDevice9_DrawPrimitiveUP(device,D3DPT_TRIANGLELIST,1,
        vertices,sizeof(vertices[0]));
    HRESULT end=IDirect3DDevice9_EndScene(device);HR(draw);HR(end);
    HR(IDirect3DQuery9_Issue(occlusion,D3DISSUE_END));
    DWORD samples=0;HR(poll(occlusion,&samples,sizeof(samples)));
    printf("OCCLUSION samples=%lu expected=64\n",(unsigned long)samples);EXPECT(samples==64);
    for(UINT i=0;i<4;++i) {
        D3DLOCKED_RECT lock;
        HR(IDirect3DDevice9_GetRenderTargetData(device,targets[i],cpu));
        HR(IDirect3DSurface9_LockRect(cpu,&lock,NULL,D3DLOCK_READONLY));
        DWORD inside=*(DWORD *)((BYTE *)lock.pBits+8*lock.Pitch+8*4);
        DWORD outside=*(DWORD *)((BYTE *)lock.pBits+lock.Pitch+4);
        UINT changed=0;
        for(UINT y=0;y<32;++y)for(UINT x=0;x<32;++x)
            changed+=((DWORD*)((BYTE*)lock.pBits+y*lock.Pitch))[x]!=0x11223344;
        printf("MRT changed=%u\n",changed);EXPECT(changed==64);
        HR(IDirect3DSurface9_UnlockRect(cpu));
        printf("MRT index=%u inside=%08lx outside=%08lx\n",i,(unsigned long)inside,(unsigned long)outside);
        EXPECT(inside==expected[i]);EXPECT(outside==0x11223344);
    }
done:
    for(UINT i=1;i<4;++i)IDirect3DDevice9_SetRenderTarget(device,i,NULL);
    if(old)IDirect3DDevice9_SetRenderTarget(device,0,old);
    if(states)IDirect3DStateBlock9_Apply(states);
    RELEASE(occlusion);RELEASE(states);RELEASE(ps);RELEASE(cpu);RELEASE(old);
    for(UINT i=0;i<4;++i)RELEASE(targets[i]);
}


static void format_blending(void)
{
    static const DWORD shader[]={0xffff0200,
        0x02000001,0x800f0800,0xa0e40000,
        0x02000001,0x800f0801,0xa0e40000,0x0000ffff};
    static const D3DFORMAT formats[]={D3DFMT_X8B8G8R8,D3DFMT_A2R10G10B10};
    const float white[4]={1,1,1,1};
    const struct {float x,y,z,w;} vertices[]={{-.5f,-.5f,.5f,1},
        {63.5f,-.5f,.5f,1},{-.5f,63.5f,.5f,1}};
    IDirect3DSurface9 *targets[2]={0},*cpu[2]={0},*old=NULL;
    IDirect3DPixelShader9 *ps=NULL;IDirect3DStateBlock9 *states=NULL;
    D3DVIEWPORT9 viewport={0,0,32,32,0,1};
    HR(IDirect3DDevice9_CreateStateBlock(device,D3DSBT_ALL,&states));
    HR(IDirect3DDevice9_GetRenderTarget(device,0,&old));
    HR(IDirect3DDevice9_SetDepthStencilSurface(device,NULL));
    for(UINT i=0;i<2;++i) {
        HR(IDirect3DDevice9_CreateRenderTarget(device,32,32,formats[i],
            D3DMULTISAMPLE_NONE,0,FALSE,&targets[i],NULL));
        HR(IDirect3DDevice9_CreateOffscreenPlainSurface(device,32,32,formats[i],
            D3DPOOL_SYSTEMMEM,&cpu[i],NULL));
        HR(IDirect3DDevice9_SetRenderTarget(device,i,targets[i]));
    }
    HR(IDirect3DDevice9_SetViewport(device,&viewport));
    HR(IDirect3DDevice9_CreatePixelShader(device,shader,&ps));
    HR(IDirect3DDevice9_SetPixelShader(device,ps));
    HR(IDirect3DDevice9_SetPixelShaderConstantF(device,0,white,1));
    HR(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZRHW));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_CULLMODE,D3DCULL_NONE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHABLENDENABLE,TRUE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHATESTENABLE,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_SEPARATEALPHABLENDENABLE,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_LIGHTING,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_SCISSORTESTENABLE,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_BLENDOP,D3DBLENDOP_ADD));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_DESTBLEND,D3DBLEND_ZERO));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_BLENDFACTOR,0xff4080c0));
    for(UINT mode=0;mode<3;++mode) {
        HR(IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0,1,0));
        HR(IDirect3DDevice9_SetRenderState(device,D3DRS_SRCBLEND,
            mode==0?D3DBLEND_DESTALPHA:D3DBLEND_BLENDFACTOR));
        HR(IDirect3DDevice9_SetRenderState(device,D3DRS_COLORWRITEENABLE,mode==2?1:15));
        HR(IDirect3DDevice9_SetRenderState(device,D3DRS_COLORWRITEENABLE1,mode==2?1:15));
        HR(IDirect3DDevice9_BeginScene(device));
        HRESULT draw=IDirect3DDevice9_DrawPrimitiveUP(device,D3DPT_TRIANGLELIST,1,
            vertices,sizeof(vertices[0]));
        HRESULT end=IDirect3DDevice9_EndScene(device);HR(draw);HR(end);
        for(UINT i=0;i<2;++i) {
            D3DLOCKED_RECT lock;DWORD pixel;UINT red,green,blue;
            HR(IDirect3DDevice9_GetRenderTargetData(device,targets[i],cpu[i]));
            HR(IDirect3DSurface9_LockRect(cpu[i],&lock,NULL,D3DLOCK_READONLY));
            memcpy(&pixel,(BYTE *)lock.pBits+8*lock.Pitch+8*4,4);
            HR(IDirect3DSurface9_UnlockRect(cpu[i]));
            if(i==0) {red=pixel&255;green=(pixel>>8)&255;blue=(pixel>>16)&255;}
            else {red=(((pixel>>20)&1023)*255+511)/1023;
                  green=(((pixel>>10)&1023)*255+511)/1023;blue=((pixel&1023)*255+511)/1023;}
            printf("FORMAT_BLEND mode=%u target=%u rgb=%u,%u,%u\n",mode,i,red,green,blue);
            if(mode==0) {UINT expected=i==0?255:0;EXPECT(red==expected&&green==expected&&blue==expected);}
            else {EXPECT(red==64);EXPECT(green==(mode==2?0:128));EXPECT(blue==(mode==2?0:192));}
        }
    }
done:
    IDirect3DDevice9_SetRenderTarget(device,1,NULL);
    if(old)IDirect3DDevice9_SetRenderTarget(device,0,old);
    if(states)IDirect3DStateBlock9_Apply(states);
    RELEASE(states);RELEASE(ps);RELEASE(old);
    for(UINT i=0;i<2;++i){RELEASE(cpu[i]);RELEASE(targets[i]);}
}

static void raster_controls(IDirect3D9 *d3d)
{
    static const DWORD shader[]={0xffff0200,0x02000001,0x800f0800,0xa0e40000,0x0000ffff};
    const float color[4]={0.49f,0.37f,0.23f,1};
    const struct {float x,y,z,w;} vertices[]={{-.5f,-.5f,.5f,1},
        {63.5f,-.5f,.5f,1},{-.5f,63.5f,.5f,1}};
    IDirect3DSurface9 *old=NULL,*target=NULL,*cpu=NULL,*resolved=NULL;
    IDirect3DPixelShader9 *ps=NULL;IDirect3DStateBlock9 *states=NULL;
    D3DVIEWPORT9 viewport={0,0,32,32,0,1};
    WORD reference[32*32];UINT changed=0;DWORD qualities=0;
    HR(IDirect3DDevice9_CreateStateBlock(device,D3DSBT_ALL,&states));
    HR(IDirect3DDevice9_GetRenderTarget(device,0,&old));
    HR(IDirect3DDevice9_SetDepthStencilSurface(device,NULL));
    HR(IDirect3DDevice9_CreatePixelShader(device,shader,&ps));
    HR(IDirect3DDevice9_SetPixelShader(device,ps));
    HR(IDirect3DDevice9_SetPixelShaderConstantF(device,0,color,1));
    HR(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZRHW));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_CULLMODE,D3DCULL_NONE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHABLENDENABLE,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHATESTENABLE,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_SCISSORTESTENABLE,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_COLORWRITEENABLE,15));
    HR(IDirect3DDevice9_CreateRenderTarget(device,32,32,D3DFMT_R5G6B5,
        D3DMULTISAMPLE_NONE,0,FALSE,&target,NULL));
    HR(IDirect3DDevice9_CreateOffscreenPlainSurface(device,32,32,D3DFMT_R5G6B5,
        D3DPOOL_SYSTEMMEM,&cpu,NULL));
    HR(IDirect3DDevice9_SetRenderTarget(device,0,target));
    HR(IDirect3DDevice9_SetViewport(device,&viewport));
    for(UINT pass=0;pass<3;++pass) {
        D3DLOCKED_RECT lock;
        HR(IDirect3DDevice9_SetRenderState(device,D3DRS_DITHERENABLE,pass==1));
        HR(IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0,1,0));
        HR(IDirect3DDevice9_BeginScene(device));
        HRESULT draw=IDirect3DDevice9_DrawPrimitiveUP(device,D3DPT_TRIANGLELIST,1,vertices,sizeof(vertices[0]));
        HRESULT end=IDirect3DDevice9_EndScene(device);HR(draw);HR(end);
        HR(IDirect3DDevice9_GetRenderTargetData(device,target,cpu));
        HR(IDirect3DSurface9_LockRect(cpu,&lock,NULL,D3DLOCK_READONLY));
        for(UINT y=0;y<32;++y)for(UINT x=0;x<32;++x) {
            WORD pixel=((WORD*)((BYTE*)lock.pBits+y*lock.Pitch))[x];
            if(pass==0)reference[y*32+x]=pixel;
            else if(pass==1)changed+=pixel!=reference[y*32+x];
            else EXPECT(pixel==reference[y*32+x]);
        }
        HR(IDirect3DSurface9_UnlockRect(cpu));
    }
    EXPECT(changed>0);printf("DITHER changed=%u pixels\n",changed);
    IDirect3DDevice9_SetRenderTarget(device,0,old);RELEASE(target);RELEASE(cpu);
    HR(IDirect3D9_CheckDeviceMultiSampleType(d3d,0,D3DDEVTYPE_HAL,D3DFMT_A8R8G8B8,
        TRUE,D3DMULTISAMPLE_NONMASKABLE,&qualities));
    EXPECT(qualities==2);
    HR(IDirect3DDevice9_CreateRenderTarget(device,32,32,D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE,0,FALSE,&resolved,NULL));
    HR(IDirect3DDevice9_CreateOffscreenPlainSurface(device,32,32,D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM,&cpu,NULL));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_MULTISAMPLEMASK,0));
    for(DWORD quality=0;quality<qualities;++quality) {
        D3DLOCKED_RECT lock;DWORD pixel;
        HR(IDirect3DDevice9_CreateRenderTarget(device,32,32,D3DFMT_A8R8G8B8,
            D3DMULTISAMPLE_NONMASKABLE,quality,FALSE,&target,NULL));
        HR(IDirect3DDevice9_SetRenderTarget(device,0,target));
        HR(IDirect3DDevice9_SetViewport(device,&viewport));
        HR(IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0,1,0));
        HR(IDirect3DDevice9_BeginScene(device));
        HRESULT draw=IDirect3DDevice9_DrawPrimitiveUP(device,D3DPT_TRIANGLELIST,1,vertices,sizeof(vertices[0]));
        HRESULT end=IDirect3DDevice9_EndScene(device);HR(draw);HR(end);
        HR(IDirect3DDevice9_StretchRect(device,target,NULL,resolved,NULL,D3DTEXF_NONE));
        HR(IDirect3DDevice9_GetRenderTargetData(device,resolved,cpu));
        HR(IDirect3DSurface9_LockRect(cpu,&lock,NULL,D3DLOCK_READONLY));
        pixel=((DWORD*)((BYTE*)lock.pBits+8*lock.Pitch))[8];
        HR(IDirect3DSurface9_UnlockRect(cpu));
        printf("NONMASKABLE quality=%lu pixel=%08lx\n",(unsigned long)quality,(unsigned long)pixel);
        EXPECT((pixel&0xffffff)==0x7d5e3b);
        IDirect3DDevice9_SetRenderTarget(device,0,old);RELEASE(target);
    }
done:
    if(old)IDirect3DDevice9_SetRenderTarget(device,0,old);
    if(states)IDirect3DStateBlock9_Apply(states);
    RELEASE(states);RELEASE(ps);RELEASE(target);RELEASE(cpu);RELEASE(resolved);RELEASE(old);
}

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    IDirect3D9 *d3d=Direct3DCreate9(D3D_SDK_VERSION);D3DCAPS9 caps;
    HWND window=CreateWindowA("STATIC","Triton D3D9 pipeline",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        0,0,320,240,NULL,NULL,GetModuleHandle(NULL),NULL);
    D3DPRESENT_PARAMETERS pp={0};pp.BackBufferWidth=320;pp.BackBufferHeight=240;
    pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferCount=1;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow=window;pp.Windowed=TRUE;
    EXPECT(d3d && window);if(!d3d || !window)goto done;
    HR(IDirect3D9_CreateDevice(d3d,D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device));
    EXPECT(GetModuleHandleA("neptune_d3d9.dll")!=NULL);
    HR(IDirect3DDevice9_GetDeviceCaps(device,&caps));
    EXPECT(caps.NumSimultaneousRTs>=4);
    queries();mrt();format_blending();raster_controls(d3d);
    HR(IDirect3DDevice9_Present(device,NULL,NULL,NULL,NULL));
done:
    RELEASE(device);RELEASE(d3d);if(window)DestroyWindow(window);
    printf("D3D9 PIPELINE checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
