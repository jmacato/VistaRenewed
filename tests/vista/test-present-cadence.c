#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
/* Diagnostic: continuously change the whole frame, with a unique RGB frame ID.
 * Host presentation samples and redraw statistics are required; guest FPS alone
 * cannot establish visible refresh. */
static LRESULT CALLBACK window_proc(HWND w,UINT m,WPARAM a,LPARAM b) {
 if(m==WM_ERASEBKGND)return 1;
 if(m==WM_PAINT){PAINTSTRUCT p;BeginPaint(w,&p);EndPaint(w,&p);return 0;}
 return DefWindowProc(w,m,a,b);
}
int main(int argc,char **argv) {
 int fullscreen=argc>1&&!strcmp(argv[1],"--fullscreen");
 int ex=argc>1&&!strcmp(argv[1],"--ex");
 if(argc>2 || (argc==2 && !fullscreen && !ex))return 2;
 WNDCLASSA wc={0};wc.lpfnWndProc=window_proc;wc.hInstance=GetModuleHandle(NULL);wc.lpszClassName="TritonCadence";RegisterClassA(&wc);
 int width=fullscreen?1280:640, height=fullscreen?720:480;
 DWORD style=fullscreen?WS_POPUP:WS_OVERLAPPEDWINDOW;
 RECT bounds={0,0,width,height};AdjustWindowRect(&bounds,style,FALSE);
 HWND w=CreateWindowExA(0,wc.lpszClassName,"Triton presentation cadence",style|WS_VISIBLE,
     fullscreen?0:100,fullscreen?0:100,bounds.right-bounds.left,bounds.bottom-bounds.top,
     NULL,NULL,GetModuleHandle(NULL),NULL);
 IDirect3D9 *a=NULL;IDirect3D9Ex *ax=NULL;
 IDirect3DDevice9 *d=NULL;IDirect3DDevice9Ex *dx=NULL;
 if(ex){if(FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION,&ax)))return 1;a=(IDirect3D9 *)ax;}
 else a=Direct3DCreate9(D3D_SDK_VERSION);
 if(!w||!a)return 1;
 ShowWindow(w,SW_SHOW);UpdateWindow(w);SetForegroundWindow(w);
 D3DPRESENT_PARAMETERS p={0};p.Windowed=!fullscreen;p.BackBufferWidth=width;p.BackBufferHeight=height;p.BackBufferFormat=D3DFMT_X8R8G8B8;p.BackBufferCount=1;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.hDeviceWindow=w;p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 HRESULT hr;
 if(ex){hr=IDirect3D9Ex_CreateDeviceEx(ax,0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING,&p,NULL,&dx);d=(IDirect3DDevice9 *)dx;}
 else hr=IDirect3D9_CreateDevice(a,0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING,&p,&d);
 if(FAILED(hr)){printf("CREATE %08lx\n",(unsigned long)hr);return 1;}
 printf("CREATED fullscreen=%d\n",fullscreen);fflush(stdout);
 printf("BEFORE CLOCK\n");fflush(stdout);
 LARGE_INTEGER freq,start,now;QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&start);now=start;unsigned frame=0,occluded=0;
 do {
  if(frame<3){printf("FRAME %u before messages\n",frame);fflush(stdout);}
  MSG m;for(unsigned i=0;i<32 && PeekMessage(&m,NULL,0,0,PM_REMOVE);i++){TranslateMessage(&m);DispatchMessage(&m);}
  if(frame<3){printf("FRAME %u before clear\n",frame);fflush(stdout);}
  DWORD color=0xff000000|((frame*73)&255)<<16|((frame*151)&255)<<8|((frame*199)&255);
  hr=IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET,color,1,0);if(FAILED(hr))break;
  if(frame<3){printf("FRAME %u before present\n",frame);fflush(stdout);}
  hr=ex?IDirect3DDevice9Ex_PresentEx(dx,NULL,NULL,NULL,NULL,0):IDirect3DDevice9_Present(d,NULL,NULL,NULL,NULL);if(FAILED(hr))break;if(hr==S_PRESENT_OCCLUDED)occluded++;
  if(frame<3){printf("FRAME %u completed hr=%08lx\n",frame,(unsigned long)hr);fflush(stdout);}
  frame++;QueryPerformanceCounter(&now);
 }while((now.QuadPart-start.QuadPart)<5*freq.QuadPart);
 QueryPerformanceCounter(&now);
 int loaded=GetModuleHandleA(sizeof(void*)==8?"neptune_d3d9.dll":"neptune_d3d9_wow.dll")!=NULL;
 printf("DRIVER ex=%d bits=%u loaded=%d\n",ex,(unsigned)sizeof(void*)*8,loaded);
 printf("CADENCE fullscreen=%d frames=%u seconds=%.3f guest_fps=%.2f occluded=%u hr=%08lx\n",fullscreen,frame,(double)(now.QuadPart-start.QuadPart)/freq.QuadPart,frame*(double)freq.QuadPart/(now.QuadPart-start.QuadPart),occluded,(unsigned long)hr);
 IDirect3DDevice9_Release(d);IDirect3D9_Release(a);DestroyWindow(w);return FAILED(hr)||frame<10||!loaded?1:0;
}
