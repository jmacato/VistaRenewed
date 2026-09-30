/* SPDX-License-Identifier: MIT */
#define D3D10_PRESENT_PROBE
#include "test-d3d10-runtime.c"
static void pump(void) {MSG message;while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    BOOL fullscreen=FALSE,v101=FALSE;UINT samples=1;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--fullscreen"))fullscreen=TRUE;
        else if(!strcmp(argv[i],"--msaa4"))samples=4;
        else if(!strcmp(argv[i],"--10.1"))v101=TRUE;
        else if(!strcmp(argv[i],"--10"))v101=FALSE;
        else {printf("FAIL unknown option %s\n",argv[i]);return 1;}
    }
    ID3D10Device *d=create_device(v101);IDXGIDevice *dxgi=NULL;IDXGIAdapter *adapter=NULL;IDXGIFactory *factory=NULL;
    IDXGISwapChain *swap=NULL;ID3D10Texture2D *buffer=NULL;ID3D10RenderTargetView *view=NULL;
    HWND window=CreateWindowA("STATIC","Triton D3D10 presentation pixel oracle",WS_OVERLAPPEDWINDOW|WS_VISIBLE,80,80,360,280,NULL,NULL,GetModuleHandleW(NULL),NULL);
    if(!d || !window){EXPECT(FALSE,"present prerequisites");goto done;}
    HR(ID3D10Device_QueryInterface(d,&IID_IDXGIDevice,(void**)&dxgi));HR(IDXGIDevice_GetAdapter(dxgi,&adapter));HR(IDXGIAdapter_GetParent(adapter,&IID_IDXGIFactory,(void**)&factory));
    DXGI_SWAP_CHAIN_DESC desc;memset(&desc,0,sizeof(desc));desc.BufferDesc.Width=160;desc.BufferDesc.Height=120;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=samples;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=window;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    HR(IDXGIFactory_CreateSwapChain(factory,(IUnknown*)d,&desc,&swap));
    HR(IDXGIFactory_MakeWindowAssociation(factory,window,DXGI_MWA_NO_ALT_ENTER));
    if(fullscreen)HR(IDXGISwapChain_SetFullscreenState(swap,TRUE,NULL));
    for(UINT size=0;size<2;++size){
        if(size){ID3D10Device_OMSetRenderTargets(d,0,NULL,NULL);RELEASE(view);RELEASE(buffer);HR(IDXGISwapChain_ResizeBuffers(swap,1,256,192,DXGI_FORMAT_R8G8B8A8_UNORM,0));}
        HR(IDXGISwapChain_GetBuffer(swap,0,&IID_ID3D10Texture2D,(void**)&buffer));HR(ID3D10Device_CreateRenderTargetView(d,(ID3D10Resource*)buffer,NULL,&view));
        D3D10_TEXTURE2D_DESC actual;ID3D10Texture2D_GetDesc(buffer,&actual);
        EXPECT(actual.SampleDesc.Count==samples,"backbuffer sample count preserved");
        for(UINT frame=0;frame<4;++frame){
            float colour[4]={frame&1?0.f:1.f,frame&1?1.f:0.f,0,1};
            ID3D10Device_ClearRenderTargetView(d,view,colour);HR(IDXGISwapChain_Present(swap,frame&1,0));pump();Sleep(150);pump();
            RECT client;GetClientRect(window,&client);POINT middle={(client.right-client.left)/2,(client.bottom-client.top)/2};ClientToScreen(window,&middle);
            HDC screen=GetDC(NULL);COLORREF got=GetPixel(screen,middle.x,middle.y);ReleaseDC(NULL,screen);
            COLORREF expected=frame&1?RGB(0,255,0):RGB(255,0,0);
            printf("SCREEN mode=%s samples=%u size=%u sync=%u x=%ld y=%ld got=%08lx expected=%08lx\n",fullscreen?"fullscreen":"windowed",samples,size,frame&1,middle.x,middle.y,(unsigned long)got,(unsigned long)expected);
            EXPECT(got==expected,"visible presentation colour");
        }
    }
    Sleep(1000);
done:
    if(swap)IDXGISwapChain_SetFullscreenState(swap,FALSE,NULL);
    if(d)ID3D10Device_ClearState(d);
    RELEASE(view);RELEASE(buffer);RELEASE(swap);RELEASE(factory);RELEASE(adapter);RELEASE(dxgi);RELEASE(d);if(window)DestroyWindow(window);
    printf("D3D10 PRESENT %s checks=%u failures=%u\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
