#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

/* Rendered colour is the independent oracle for depth and stencil behavior;
 * non-lockable depth formats are never inspected through their CPU shadows. */
static int failures;
#define CHECK(call) do { HRESULT h = (call); if (FAILED(h)) { \
    printf("FAIL %s %08lx\n", #call, (unsigned long)h); ++failures; goto done; } } while (0)
struct Vertex { float x, y, z, rhw; DWORD colour; };

static HRESULT quad(IDirect3DDevice9 *d, float z, DWORD colour)
{
    struct Vertex v[] = {{-.5f,-.5f,z,1,colour}, {63.5f,-.5f,z,1,colour},
                        {-.5f,63.5f,z,1,colour}, {63.5f,63.5f,z,1,colour}};
    HRESULT hr = IDirect3DDevice9_BeginScene(d);
    if (FAILED(hr)) return hr;
    hr = IDirect3DDevice9_DrawPrimitiveUP(d,D3DPT_TRIANGLESTRIP,2,v,sizeof(v[0]));
    HRESULT end = IDirect3DDevice9_EndScene(d);
    return FAILED(hr) ? hr : end;
}

static void pixel(IDirect3DDevice9 *d, IDirect3DSurface9 *cpu,
                  const char *name, UINT x, DWORD expected)
{
    IDirect3DSurface9 *rt = NULL;
    D3DLOCKED_RECT lock;
    CHECK(IDirect3DDevice9_GetRenderTarget(d,0,&rt));
    CHECK(IDirect3DDevice9_GetRenderTargetData(d,rt,cpu));
    CHECK(IDirect3DSurface9_LockRect(cpu,&lock,NULL,D3DLOCK_READONLY));
    DWORD got = *(DWORD *)((BYTE *)lock.pBits + 32*lock.Pitch + x*4);
    CHECK(IDirect3DSurface9_UnlockRect(cpu));
    printf("PIXEL %s got=%08lx expected=%08lx\n",name,
           (unsigned long)got,(unsigned long)expected);
    if ((got & 0xffffff) != (expected & 0xffffff)) ++failures;
done:
    if (rt) IDirect3DSurface9_Release(rt);
}

int main(void)
{
    HWND w = CreateWindowA("STATIC","Triton D24X8",WS_OVERLAPPEDWINDOW,
                          0,0,100,100,NULL,NULL,GetModuleHandle(NULL),NULL);
    IDirect3D9 *api = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9 *d = NULL;
    IDirect3DSurface9 *depth = NULL, *stencil = NULL, *cpu = NULL;
    D3DPRESENT_PARAMETERS pp = {0};
    D3DRECT left = {0,0,32,64};
    if (!w || !api) { ++failures; goto done; }
    CHECK(IDirect3D9_CheckDeviceFormat(api,0,D3DDEVTYPE_HAL,D3DFMT_X8R8G8B8,
                                      D3DUSAGE_DEPTHSTENCIL,D3DRTYPE_SURFACE,D3DFMT_D24X8));
    pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=w;
    pp.BackBufferWidth=64; pp.BackBufferHeight=64; pp.BackBufferFormat=D3DFMT_X8R8G8B8;
    pp.EnableAutoDepthStencil=TRUE; pp.AutoDepthStencilFormat=D3DFMT_D24X8;
    CHECK(IDirect3D9_CreateDevice(api,0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&d));
    CHECK(IDirect3DDevice9_GetDepthStencilSurface(d,&depth));
    CHECK(IDirect3DDevice9_CreateDepthStencilSurface(d,64,64,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,&stencil,NULL));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(d,64,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&cpu,NULL));
    CHECK(IDirect3DDevice9_SetFVF(d,D3DFVF_XYZRHW|D3DFVF_DIFFUSE));
    CHECK(IDirect3DDevice9_SetRenderState(d,D3DRS_LIGHTING,FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(d,D3DRS_CULLMODE,D3DCULL_NONE));
    CHECK(IDirect3DDevice9_SetRenderState(d,D3DRS_ZENABLE,TRUE));
    CHECK(IDirect3DDevice9_SetRenderState(d,D3DRS_ZFUNC,D3DCMP_LESS));
    CHECK(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
    CHECK(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLORARG1,D3DTA_DIFFUSE));
    CHECK(IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xff000000,.4f,0));
    CHECK(quad(d,.6f,0xffff0000));
    pixel(d,cpu,"far rejected",16,0xff000000);
    CHECK(quad(d,.2f,0xff00ff00));
    pixel(d,cpu,"near passes",16,0xff00ff00);
    CHECK(IDirect3DDevice9_Clear(d,1,&left,D3DCLEAR_ZBUFFER,0,.8f,0));
    CHECK(quad(d,.6f,0xff0000ff));
    pixel(d,cpu,"rect depth cleared",16,0xff0000ff);
    pixel(d,cpu,"outside depth preserved",48,0xff00ff00);
    CHECK(IDirect3DDevice9_SetRenderState(d,D3DRS_ZENABLE,FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(d,D3DRS_STENCILENABLE,TRUE));
    CHECK(IDirect3DDevice9_SetRenderState(d,D3DRS_STENCILFUNC,D3DCMP_NEVER));
    for (UINT i=0;i<4;++i) {
        CHECK(IDirect3DDevice9_SetDepthStencilSurface(d,(i&1)?depth:stencil));
        CHECK(IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET,0xff000000,1,0));
        CHECK(quad(d,.5f,0xffff0000));
        pixel(d,cpu,(i&1)?"X8 ignores stencil":"S8 stencil rejects",32,
              (i&1)?0xffff0000:0xff000000);
    }
done:
    if (cpu) IDirect3DSurface9_Release(cpu);
    if (stencil) IDirect3DSurface9_Release(stencil);
    if (depth) IDirect3DSurface9_Release(depth);
    if (d) IDirect3DDevice9_Release(d);
    if (api) IDirect3D9_Release(api);
    if (w) DestroyWindow(w);
    printf("D24X8 %s failures=%d\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
