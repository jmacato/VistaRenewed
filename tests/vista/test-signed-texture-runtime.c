#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(call) do { HRESULT hr_=(call); if(FAILED(hr_)) { \
    printf("FAIL %s hr=%08lx\n",#call,(unsigned long)hr_); ++failures; goto done; } } while(0)

/* ps_2_0: sample s0, map signed [-1,1] into observable [0,1], output.
 * An ARGB render target keeps the fourth signed channel observable as well. */
static DWORD shader[] = {
    0xffff0200, 0x0200001f,0x80000000,0xb00f0000,
    0x0200001f,0x90000000,0xa00f0800,
    0x03000042,0x800f0000,0xb0e40000,0xa0e40800,
    0x04000004,0x800f0000,0x80e40000,0xa0e40000,0xa0e40000,
    0x02000001,0x800f0800,0x80e40000,0x0000ffff
};

static void sample(IDirect3DDevice9 *d, IDirect3DSurface9 *cpu,
                   const char *name, float u, float v, DWORD filter, DWORD expected)
{
    struct { float x,y,z,w,u,v; } vertices[] = {
        {-.5f,-.5f,.5f,1,u,v},{63.5f,-.5f,.5f,1,u,v},
        {-.5f,63.5f,.5f,1,u,v},{63.5f,63.5f,.5f,1,u,v}
    };
    IDirect3DSurface9 *rt=NULL;
    D3DLOCKED_RECT lock;
    CHECK(IDirect3DDevice9_SetSamplerState(d,0,D3DSAMP_MINFILTER,filter));
    CHECK(IDirect3DDevice9_SetSamplerState(d,0,D3DSAMP_MAGFILTER,filter));
    CHECK(IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET,0xff123456,1,0));
    CHECK(IDirect3DDevice9_BeginScene(d));
    HRESULT hr=IDirect3DDevice9_DrawPrimitiveUP(d,D3DPT_TRIANGLESTRIP,2,vertices,sizeof(vertices[0]));
    HRESULT end=IDirect3DDevice9_EndScene(d);
    CHECK(hr); CHECK(end);
    CHECK(IDirect3DDevice9_GetRenderTarget(d,0,&rt));
    CHECK(IDirect3DDevice9_GetRenderTargetData(d,rt,cpu));
    CHECK(IDirect3DSurface9_LockRect(cpu,&lock,NULL,D3DLOCK_READONLY));
    DWORD got=*(DWORD *)((BYTE *)lock.pBits+32*lock.Pitch+32*4);
    CHECK(IDirect3DSurface9_UnlockRect(cpu));
    int pass=1;
    for(unsigned shift=0;shift<32;shift+=8) {
        int delta=(int)((got>>shift)&255)-(int)((expected>>shift)&255);
        if(delta < -1 || delta > 1) pass=0;
    }
    printf("PIXEL %s got=%08lx expected=%08lx %s\n",name,
           (unsigned long)got,(unsigned long)expected,pass?"PASS":"FAIL");
    if(!pass) ++failures;
done:
    if(rt) IDirect3DSurface9_Release(rt);
}

int main(void)
{
    HWND w=CreateWindowA("STATIC","Triton signed texture",WS_OVERLAPPEDWINDOW,
                        0,0,100,100,NULL,NULL,GetModuleHandle(NULL),NULL);
    IDirect3D9 *api=Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9 *d=NULL;
    IDirect3DTexture9 *tex=NULL;
    IDirect3DSurface9 *cpu=NULL,*target=NULL;
    IDirect3DPixelShader9 *ps=NULL;
    D3DPRESENT_PARAMETERS pp={0};
    D3DLOCKED_RECT lock;
    const BYTE pixels[16]={128,0,127,129, 127,127,128,127,
                          127,127,127,127, 0,0,0,0};
    const float half[4]={.5f,.5f,.5f,.5f};
    RECT one={0,0,1,1};
    if(!api || !w) {++failures;goto done;}
    CHECK(IDirect3D9_CheckDeviceFormat(api,0,D3DDEVTYPE_HAL,D3DFMT_X8R8G8B8,
                                      0,D3DRTYPE_TEXTURE,D3DFMT_Q8W8V8U8));
    pp.Windowed=TRUE;pp.hDeviceWindow=w;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth=pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_X8R8G8B8;
    CHECK(IDirect3D9_CreateDevice(api,0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&d));
    CHECK(IDirect3DDevice9_CreateTexture(d,2,2,1,0,D3DFMT_Q8W8V8U8,D3DPOOL_MANAGED,&tex,NULL));
    CHECK(IDirect3DTexture9_LockRect(tex,0,&lock,NULL,0));
    for(int y=0;y<2;++y) memcpy((BYTE *)lock.pBits+y*lock.Pitch,pixels+y*8,8);
    CHECK(IDirect3DTexture9_UnlockRect(tex,0));
    CHECK(IDirect3DDevice9_CreatePixelShader(d,shader,&ps));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(d,64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&cpu,NULL));
    CHECK(IDirect3DDevice9_CreateRenderTarget(d,64,64,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,NULL));
    CHECK(IDirect3DDevice9_SetRenderTarget(d,0,target));
    CHECK(IDirect3DDevice9_SetFVF(d,D3DFVF_XYZRHW|D3DFVF_TEX1));
    CHECK(IDirect3DDevice9_SetRenderState(d,D3DRS_ZENABLE,FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(d,D3DRS_CULLMODE,D3DCULL_NONE));
    CHECK(IDirect3DDevice9_SetTexture(d,0,(IDirect3DBaseTexture9 *)tex));
    CHECK(IDirect3DDevice9_SetPixelShaderConstantF(d,0,half,1));
    CHECK(IDirect3DDevice9_SetPixelShader(d,ps));
    sample(d,cpu,"negative and positive endpoints",.25f,.25f,D3DTEXF_POINT,0x000080ff);
    sample(d,cpu,"opposite endpoints",.75f,.25f,D3DTEXF_POINT,0xffffff00);
    sample(d,cpu,"linear across sign boundary",.5f,.25f,D3DTEXF_LINEAR,0x8080bf80);
    CHECK(IDirect3DDevice9_SetTexture(d,0,NULL));
    CHECK(IDirect3DTexture9_LockRect(tex,0,&lock,NULL,D3DLOCK_READONLY));
    for(int y=0;y<2;++y) if(memcmp((BYTE *)lock.pBits+y*lock.Pitch,pixels+y*8,8)) ++failures;
    CHECK(IDirect3DTexture9_UnlockRect(tex,0));
    CHECK(IDirect3DTexture9_LockRect(tex,0,&lock,&one,0));
    memset(lock.pBits,0,4);
    CHECK(IDirect3DTexture9_UnlockRect(tex,0));
    CHECK(IDirect3DDevice9_SetTexture(d,0,(IDirect3DBaseTexture9 *)tex));
    CHECK(IDirect3DDevice9_SetPixelShader(d,ps));
    sample(d,cpu,"partial update",.25f,.25f,D3DTEXF_POINT,0x80808080);
    sample(d,cpu,"other row preserved",.25f,.75f,D3DTEXF_POINT,0xffffffff);
done:
    if(target) IDirect3DSurface9_Release(target);
    if(ps) IDirect3DPixelShader9_Release(ps);
    if(cpu) IDirect3DSurface9_Release(cpu);
    if(tex) IDirect3DTexture9_Release(tex);
    if(d) IDirect3DDevice9_Release(d);
    if(api) IDirect3D9_Release(api);
    if(w) DestroyWindow(w);
    printf("SIGNED_TEXTURE bits=%u %s failures=%d\n",(unsigned)(8*sizeof(void*)),failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
