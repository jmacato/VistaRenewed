#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
int main(void) {
 IDirect3D9Ex *a=NULL; HRESULT hr=Direct3DCreate9Ex(D3D_SDK_VERSION,&a);
 if(FAILED(hr))return 1;
 HWND w=CreateWindowA("STATIC","Fullscreen mode probe",WS_POPUP|WS_VISIBLE,0,0,1280,720,NULL,NULL,GetModuleHandle(NULL),NULL);
 SetForegroundWindow(w);
 D3DDISPLAYMODEFILTER filter={sizeof(filter),D3DFMT_X8R8G8B8,D3DSCANLINEORDERING_PROGRESSIVE};
 UINT count=IDirect3D9Ex_GetAdapterModeCountEx(a,0,&filter);
 for(UINT i=0;i<count;i++) {D3DDISPLAYMODEEX m={sizeof(m)};hr=IDirect3D9Ex_EnumAdapterModesEx(a,0,&filter,i,&m);printf("MODE hr=%08lx %ux%u @%u order=%u\n",(unsigned long)hr,m.Width,m.Height,m.RefreshRate,m.ScanLineOrdering);}
 UINT rates[]={0,60,300};
 for(unsigned ex=0;ex<2;ex++)for(unsigned r=0;r<3;r++) {
  D3DPRESENT_PARAMETERS p={0};p.BackBufferWidth=1280;p.BackBufferHeight=720;p.BackBufferFormat=D3DFMT_X8R8G8B8;p.BackBufferCount=1;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.hDeviceWindow=w;p.FullScreen_RefreshRateInHz=rates[r];p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
  p.EnableAutoDepthStencil=TRUE;p.AutoDepthStencilFormat=D3DFMT_D24X8;
  D3DDISPLAYMODEEX m={sizeof(m),1280,720,rates[r],D3DFMT_X8R8G8B8,D3DSCANLINEORDERING_PROGRESSIVE};
  IDirect3DDevice9Ex *d=NULL;
  if(ex)hr=IDirect3D9Ex_CreateDeviceEx(a,0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING,&p,&m,&d);
  else hr=IDirect3D9Ex_CreateDevice(a,0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING,&p,(IDirect3DDevice9**)&d);
  printf("CREATE ex=%u refresh=%u hr=%08lx\n",ex,rates[r],(unsigned long)hr);fflush(stdout);
  if(d) {hr=IDirect3DDevice9Ex_Clear(d,0,NULL,D3DCLEAR_TARGET,0xff204060,1,0);if(SUCCEEDED(hr))hr=IDirect3DDevice9Ex_Present(d,NULL,NULL,NULL,NULL);printf("PRESENT hr=%08lx\n",(unsigned long)hr);IDirect3DDevice9Ex_Release(d);}
 }
 DestroyWindow(w);IDirect3D9Ex_Release(a);return 0;
}
