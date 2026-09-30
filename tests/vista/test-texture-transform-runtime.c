#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
static int failures;
static int check(const char *name,HRESULT hr) { if(FAILED(hr)){printf("FAIL %s hr=%08lx\n",name,(unsigned long)hr);failures++;return 0;}return 1; }
#define CHECK(x) do {if(!check(#x,(x)))goto done;}while(0)
struct Vertex {float x,y,z,u,v;};
static int sample(IDirect3DDevice9 *d,IDirect3DSurface9 *readback,const char *name,DWORD expected,BOOL transformed) {
 struct Vertex vertices[4]={{-1,-1,.5f,.25f,.25f},{-1,1,.5f,.25f,.25f},{1,-1,.5f,.25f,.25f},{1,1,.5f,.25f,.25f}};
 struct {float x,y,z,rhw,u,v;} tl[4]={{0,64,.5f,1,.25f,.25f},{0,0,.5f,1,.25f,.25f},{64,64,.5f,1,.25f,.25f},{64,0,.5f,1,.25f,.25f}};
 IDirect3DSurface9 *rt=NULL;D3DLOCKED_RECT lock;HRESULT hr;DWORD got=0;int before=failures;
 CHECK(IDirect3DDevice9_SetFVF(d,(transformed?D3DFVF_XYZRHW:D3DFVF_XYZ)|D3DFVF_TEX1));
 CHECK(IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET,0xff000000,1,0));
 CHECK(IDirect3DDevice9_BeginScene(d));
 hr=IDirect3DDevice9_DrawPrimitiveUP(d,D3DPT_TRIANGLESTRIP,2,transformed?(void*)tl:(void*)vertices,transformed?sizeof(tl[0]):sizeof(vertices[0]));
 IDirect3DDevice9_EndScene(d);CHECK(hr);
 CHECK(IDirect3DDevice9_GetRenderTarget(d,0,&rt));
 CHECK(IDirect3DDevice9_GetRenderTargetData(d,rt,readback));
 CHECK(IDirect3DSurface9_LockRect(readback,&lock,NULL,D3DLOCK_READONLY));
 got=*(DWORD*)((BYTE*)lock.pBits+32*lock.Pitch+32*4);IDirect3DSurface9_UnlockRect(readback);
 printf("PIXEL %s got=%08lx expected=%08lx %s\n",name,(unsigned long)got,(unsigned long)expected,(got&0xffffff)==(expected&0xffffff)?"PASS":"FAIL");
 if((got&0xffffff)!=(expected&0xffffff))failures++;
done: if(rt)IDirect3DSurface9_Release(rt);return before==failures;
}
int main(void) {
 /* vs_2_0: def c0, .75,.75,0,1; dcl_position v0;
  * mov oPos,v0; mov oT0,c0. Ordinary vertices sample yellow, while
  * POSITIONT must ignore this shader and sample the vertex's red UV. */
 const DWORD vs_tokens[]={0xfffe0200,0x05000051,0xa00f0000,0x3f400000,0x3f400000,0,0x3f800000,
  0x0200001f,0x80000000,0x900f0000,0x02000001,0xc00f0000,0x90e40000,
  0x02000001,0xe00f0000,0xa0e40000,0x0000ffff};
 IDirect3DVertexShader9 *vs=NULL,*bound=NULL;
 HWND w=CreateWindowA("STATIC","Triton texture transform pixels",WS_OVERLAPPEDWINDOW,0,0,128,128,NULL,NULL,GetModuleHandle(NULL),NULL);
 IDirect3D9 *api=Direct3DCreate9(D3D_SDK_VERSION);IDirect3DDevice9 *d=NULL;IDirect3DTexture9 *tex=NULL;IDirect3DSurface9 *readback=NULL;D3DLOCKED_RECT lock;
 D3DPRESENT_PARAMETERS pp={0};D3DMATRIX matrix={0};for(int i=0;i<4;i++)matrix.m[i][i]=1;
 printf("TEXTURE_TRANSFORM bits=%u\n",(unsigned)(8*sizeof(void*)));
 if(!w||!api){failures++;goto done;}
 pp.BackBufferWidth=pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferCount=1;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=w;pp.Windowed=TRUE;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 CHECK(IDirect3D9_CreateDevice(api,0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&d));
 CHECK(IDirect3DDevice9_CreateTexture(d,2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex,NULL));
 CHECK(IDirect3DTexture9_LockRect(tex,0,&lock,NULL,0));
 ((DWORD*)lock.pBits)[0]=0xffff0000;((DWORD*)lock.pBits)[1]=0xff00ff00;
 ((DWORD*)((BYTE*)lock.pBits+lock.Pitch))[0]=0xff0000ff;((DWORD*)((BYTE*)lock.pBits+lock.Pitch))[1]=0xffffff00;
 IDirect3DTexture9_UnlockRect(tex,0);
 CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(d,64,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,NULL));
 CHECK(IDirect3DDevice9_SetTexture(d,0,(IDirect3DBaseTexture9*)tex));
 CHECK(IDirect3DDevice9_SetRenderState(d,D3DRS_LIGHTING,FALSE));
 CHECK(IDirect3DDevice9_SetRenderState(d,D3DRS_CULLMODE,D3DCULL_NONE));
 CHECK(IDirect3DDevice9_SetRenderState(d,D3DRS_ZENABLE,FALSE));
 CHECK(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
 CHECK(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_COLORARG1,D3DTA_TEXTURE));
 CHECK(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1));
 CHECK(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE));
 CHECK(IDirect3DDevice9_SetSamplerState(d,0,D3DSAMP_MINFILTER,D3DTEXF_POINT));
 CHECK(IDirect3DDevice9_SetSamplerState(d,0,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
 CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_WORLD,&matrix));
 CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_VIEW,&matrix));
 CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_PROJECTION,&matrix));
 sample(d,readback,"disabled",0xffff0000,FALSE);
 matrix.m[2][0]=.5f;
 CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_TEXTURE0,&matrix));
 CHECK(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_COUNT2));
 sample(d,readback,"COUNT2 translate u",0xff00ff00,FALSE);
 matrix.m[2][0]=0;matrix.m[2][1]=.5f;
 CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_TEXTURE0,&matrix));
 sample(d,readback,"COUNT2 matrix update v",0xff0000ff,FALSE);
 sample(d,readback,"POSITIONT bypass",0xffff0000,TRUE);
 CHECK(IDirect3DDevice9_SetTextureStageState(d,0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE));
 sample(d,readback,"disable after transform",0xffff0000,FALSE);
 CHECK(IDirect3DDevice9_CreateVertexShader(d,vs_tokens,&vs));
 CHECK(IDirect3DDevice9_SetVertexShader(d,vs));
 sample(d,readback,"explicit VS",0xffffff00,FALSE);
 sample(d,readback,"POSITIONT with bound VS",0xffff0000,TRUE);
 CHECK(IDirect3DDevice9_GetVertexShader(d,&bound));
 if(bound!=vs){printf("FAIL POSITIONT lost vertex shader binding\n");failures++;}
 if(bound){IDirect3DVertexShader9_Release(bound);bound=NULL;}
 sample(d,readback,"restore bound VS",0xffffff00,FALSE);
done:
 if(bound)IDirect3DVertexShader9_Release(bound);if(vs)IDirect3DVertexShader9_Release(vs);
 if(readback)IDirect3DSurface9_Release(readback);if(tex)IDirect3DTexture9_Release(tex);if(d)IDirect3DDevice9_Release(d);if(api)IDirect3D9_Release(api);if(w)DestroyWindow(w);
 printf("TEXTURE_TRANSFORM %s failures=%d\n",failures?"FAIL":"PASS",failures);return failures?1:0;
}
