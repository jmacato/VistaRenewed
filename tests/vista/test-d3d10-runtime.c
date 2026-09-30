/* SPDX-License-Identifier: MIT
 * Public Microsoft D3D10/10.1 runtime oracle. No software/reference fallback.
 * Compile shaders with Vista's D3D10CompileShader and read GPU staging data. */
#define COBJMACROS
#include <windows.h>
#include <d3d10_1.h>
#include <d3d9.h>
#include <d3d10shader.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static unsigned failures, checks;
#define EXPECT(c, name) do {++checks;if(!(c)){++failures;printf("FAIL %s line=%u\n",name,(unsigned)__LINE__);}}while(0)
#define HR(c) do {HRESULT h_=(c);++checks;if(FAILED(h_)){++failures;printf("FAIL %s hr=%08lx line=%u\n",#c,(unsigned long)h_,(unsigned)__LINE__);goto done;}}while(0)
#define RELEASE(p) do {if(p){IUnknown_Release((IUnknown*)(p));(p)=NULL;}}while(0)
typedef HRESULT (WINAPI *CompileFn)(LPCSTR,SIZE_T,LPCSTR,const D3D10_SHADER_MACRO*,ID3D10Include*,LPCSTR,LPCSTR,UINT,ID3D10Blob**,ID3D10Blob**);
static CompileFn compileShader;

static ID3D10Blob *compile(const char *source,const char *entry,const char *profile)
{
    ID3D10Blob *code=NULL,*errors=NULL;
    HRESULT hr=compileShader(source,strlen(source),NULL,NULL,NULL,entry,profile,0,&code,&errors);
    if(FAILED(hr)){++failures;printf("FAIL shader %s %s hr=%08lx %s\n",entry,profile,(unsigned long)hr,
        errors?(const char*)errors->lpVtbl->GetBufferPointer(errors):"");}
    RELEASE(errors);return code;
}
static void pixel(ID3D10Device *d,ID3D10Texture2D *render,ID3D10Texture2D *read,
                  UINT x,UINT y,UINT expected,const char *label)
{
    D3D10_MAPPED_TEXTURE2D map;
    ID3D10Device_CopyResource(d,(ID3D10Resource*)read,(ID3D10Resource*)render);
    HR(ID3D10Texture2D_Map(read,0,D3D10_MAP_READ,0,&map));
    UINT got=*(UINT*)((BYTE*)map.pData+y*map.RowPitch+x*4);
    ID3D10Texture2D_Unmap(read,0);
    printf("PIXEL %s x=%u y=%u got=%08x expected=%08x\n",label,x,y,got,expected);
    EXPECT(got==expected,label);
done:;
}
static HRESULT query_wait_flags(ID3D10Query *q,void *data,UINT size,UINT flags)
{
    DWORD start=GetTickCount();HRESULT hr;
    do {hr=ID3D10Query_GetData(q,data,size,flags);if(hr!=S_FALSE)return hr;Sleep(1);}while(GetTickCount()-start<10000);
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}
static HRESULT query_wait(ID3D10Query *q,void *data,UINT size)
{ return query_wait_flags(q,data,size,0); }

static ID3D10Device *create_device(BOOL v101);
static const char *sharingPeer;
static void shared_pattern(BYTE *data,UINT bytes,unsigned phase)
{ for(UINT i=0;i<64*bytes;++i)data[i]=(BYTE)(i*13+phase); }
static HRESULT shared_complete(ID3D10Device *d)
{
    D3D10_QUERY_DESC desc={D3D10_QUERY_EVENT,0};ID3D10Query *q=NULL;BOOL ready=FALSE;
    HRESULT hr=ID3D10Device_CreateQuery(d,&desc,&q);
    if(SUCCEEDED(hr)){ID3D10Query_End(q);ID3D10Device_Flush(d);hr=query_wait(q,&ready,sizeof(ready));if(hr==S_OK&&!ready)hr=E_FAIL;}
    RELEASE(q);return hr;
}
static void shared_read(ID3D10Device *d,ID3D10Texture2D *texture,const BYTE *expected,UINT bytes)
{
    ID3D10Texture2D *read=NULL;D3D10_TEXTURE2D_DESC desc;ID3D10Texture2D_GetDesc(texture,&desc);
    desc.Usage=D3D10_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D10_CPU_ACCESS_READ;
    HR(ID3D10Device_CreateTexture2D(d,&desc,NULL,&read));ID3D10Device_CopyResource(d,(ID3D10Resource*)read,(ID3D10Resource*)texture);
    D3D10_MAPPED_TEXTURE2D mapped;HR(ID3D10Texture2D_Map(read,0,D3D10_MAP_READ,0,&mapped));
    for(UINT y=0;y<8;++y)EXPECT(!memcmp((BYTE*)mapped.pData+y*mapped.RowPitch,expected+y*8*bytes,8*bytes),"shared device/process row bytes");
    ID3D10Texture2D_Unmap(read,0);
done:RELEASE(read);
}
static int sharing_child(BOOL api,HANDLE handle,DXGI_FORMAT format)
{
    ID3D10Device *d=create_device(api);ID3D10Texture2D *texture=NULL;BYTE pattern[512];UINT bytes=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
    if(!d)return 1;
    HR(ID3D10Device_OpenSharedResource(d,handle,&IID_ID3D10Texture2D,(void**)&texture));
    D3D10_TEXTURE2D_DESC desc;ID3D10Texture2D_GetDesc(texture,&desc);EXPECT(desc.Width==8&&desc.Height==8&&desc.Format==format,"cross-process shared description");
    shared_pattern(pattern,bytes,7);shared_read(d,texture,pattern,bytes);
    shared_pattern(pattern,bytes,19);ID3D10Device_UpdateSubresource(d,(ID3D10Resource*)texture,0,NULL,pattern,8*bytes,0);HR(shared_complete(d));
done:RELEASE(texture);RELEASE(d);printf("D3D10 SHARED CHILD %s checks=%u failures=%u\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
static void sharing_process(ID3D10Device *producer,ID3D10Texture2D *texture,HANDLE handle,DXGI_FORMAT format,BOOL api)
{
    char path[MAX_PATH],command[2*MAX_PATH+160];BYTE pattern[512];UINT bytes=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;PROCESS_INFORMATION pi={0};STARTUPINFOA si={0};si.cb=sizeof(si);
    if(sharingPeer){if(strlen(sharingPeer)>=sizeof(path)){EXPECT(FALSE,"shared peer path length");return;}strcpy(path,sharingPeer);}
    else if(!GetModuleFileNameA(NULL,path,sizeof(path))){EXPECT(FALSE,"shared peer executable path");return;}
    shared_pattern(pattern,bytes,7);ID3D10Device_UpdateSubresource(producer,(ID3D10Resource*)texture,0,NULL,pattern,8*bytes,0);HR(shared_complete(producer));
    snprintf(command,sizeof(command),"\"%s\" --sharing-child %u 0x%llx %u",path,(unsigned)api,(unsigned long long)(uintptr_t)handle,(unsigned)format);
    if(!CreateProcessA(path,command,NULL,NULL,TRUE,0,NULL,NULL,&si,&pi)){printf("FAIL shared child launch error=%lu\n",GetLastError());EXPECT(FALSE,"shared child launch");goto done;}
    DWORD wait=WaitForSingleObject(pi.hProcess,60000),code=1;
    if(wait!=WAIT_OBJECT_0){TerminateProcess(pi.hProcess,ERROR_TIMEOUT);WaitForSingleObject(pi.hProcess,5000);}
    EXPECT(wait==WAIT_OBJECT_0,"shared child completion");GetExitCodeProcess(pi.hProcess,&code);EXPECT(code==0,"shared child result");
    if(wait==WAIT_OBJECT_0&&code==0){shared_pattern(pattern,bytes,19);shared_read(producer,texture,pattern,bytes);}
    printf("DATA shared child format=%u api=%u exit=%lu\n",(unsigned)format,(unsigned)api,code);
done:if(pi.hThread)CloseHandle(pi.hThread);if(pi.hProcess)CloseHandle(pi.hProcess);
}
static void sharing_devices(ID3D10Device *producer,ID3D10Device *consumer,BOOL api)
{
    const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8X8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT};
    for(UINT i=0;i<sizeof(formats)/sizeof(formats[0]);++i){
        ID3D10Texture2D *texture=NULL,*opened=NULL;IDXGIResource *dxgi=NULL;HANDLE handle=NULL;BYTE pattern[512];UINT bytes=formats[i]==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
        D3D10_TEXTURE2D_DESC desc={8,8,1,1,formats[i],{1,0},D3D10_USAGE_DEFAULT,D3D10_BIND_RENDER_TARGET|D3D10_BIND_SHADER_RESOURCE,0,D3D10_RESOURCE_MISC_SHARED};
        HR(ID3D10Device_CreateTexture2D(producer,&desc,NULL,&texture));HR(ID3D10Texture2D_QueryInterface(texture,&IID_IDXGIResource,(void**)&dxgi));HR(IDXGIResource_GetSharedHandle(dxgi,&handle));
        HR(ID3D10Device_OpenSharedResource(consumer,handle,&IID_ID3D10Texture2D,(void**)&opened));D3D10_TEXTURE2D_DESC actual;ID3D10Texture2D_GetDesc(opened,&actual);EXPECT(actual.Format==desc.Format,"shared typed format identity");
        shared_pattern(pattern,bytes,7);ID3D10Device_UpdateSubresource(producer,(ID3D10Resource*)texture,0,NULL,pattern,8*bytes,0);HR(shared_complete(producer));shared_read(consumer,opened,pattern,bytes);
        shared_pattern(pattern,bytes,19);ID3D10Device_UpdateSubresource(consumer,(ID3D10Resource*)opened,0,NULL,pattern,8*bytes,0);HR(shared_complete(consumer));shared_read(producer,texture,pattern,bytes);
        sharing_process(producer,texture,handle,formats[i],api);
        RELEASE(dxgi);RELEASE(texture);shared_pattern(pattern,bytes,19);shared_read(consumer,opened,pattern,bytes);
        printf("DATA shared devices format=%u api=%u\n",(unsigned)formats[i],(unsigned)api);
    done:RELEASE(opened);RELEASE(dxgi);RELEASE(texture);
    }
}
static HRESULT shared9_complete(IDirect3DDevice9Ex *d)
{
    IDirect3DQuery9 *q=NULL;HRESULT hr=IDirect3DDevice9Ex_CreateQuery(d,D3DQUERYTYPE_EVENT,&q);BOOL ready=FALSE;
    if(SUCCEEDED(hr)){hr=IDirect3DQuery9_Issue(q,D3DISSUE_END);DWORD start=GetTickCount();if(SUCCEEDED(hr))do{hr=IDirect3DQuery9_GetData(q,&ready,sizeof(ready),D3DGETDATA_FLUSH);if(hr!=S_FALSE)break;Sleep(1);}while(GetTickCount()-start<10000);if(hr==S_FALSE)hr=HRESULT_FROM_WIN32(ERROR_TIMEOUT);}
    RELEASE(q);return hr;
}
static void sharing_d3d9(ID3D10Device *d)
{
    typedef HRESULT (WINAPI *Create9Ex)(UINT,IDirect3D9Ex**);HMODULE dll=LoadLibraryA("d3d9.dll");Create9Ex create=dll?(Create9Ex)(void*)GetProcAddress(dll,"Direct3DCreate9Ex"):NULL;
    IDirect3D9Ex *api=NULL;IDirect3DDevice9Ex *dev=NULL;IDirect3DSurface9 *original=NULL;HWND window=NULL;
    if(!create){EXPECT(FALSE,"D3D9Ex shared runtime entry");goto cleanup;}
    if(FAILED(create(D3D_SDK_VERSION,&api))){EXPECT(FALSE,"D3D9Ex shared create");goto cleanup;}window=CreateWindowA("STATIC","Shared resource oracle",WS_OVERLAPPED,0,0,64,64,NULL,NULL,GetModuleHandle(NULL),NULL);EXPECT(window!=NULL,"shared D3D9 window");if(!window)goto cleanup;
    D3DPRESENT_PARAMETERS pp;ZeroMemory(&pp,sizeof(pp));pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;pp.BackBufferWidth=pp.BackBufferHeight=16;
    if(FAILED(IDirect3D9Ex_CreateDeviceEx(api,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,NULL,&dev))||FAILED(IDirect3DDevice9Ex_GetRenderTarget(dev,0,&original))){EXPECT(FALSE,"D3D9Ex shared device");goto cleanup;}
    const D3DFORMAT formats9[]={D3DFMT_A8B8G8R8,D3DFMT_A2B10G10R10,D3DFMT_A16B16G16R16F};const DXGI_FORMAT formats10[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT};
    /* Vista requires D3D9-origin shared surfaces. Both APIs still write and
     * read the same allocation below, including after creator release.
     * https://learn.microsoft.com/windows/win32/api/d3d10/nf-d3d10-id3d10device-opensharedresource */
    for(UINT f=0;f<3;++f){
        IDirect3DTexture9 *texture9=NULL;IDirect3DSurface9 *surface=NULL,*read9=NULL;ID3D10Texture2D *texture10=NULL;ID3D10RenderTargetView *rt=NULL;HANDLE handle=NULL;
        UINT bytes=f==2?8:4;BYTE expected[512];UINT red32=f==1?0xc00003ffu:0xff0000ffu,green32=f==1?0xc00ffc00u:0xff00ff00u;WORD red16[4]={0x3c00,0,0,0x3c00},green16[4]={0,0x3c00,0,0x3c00};
        HR(IDirect3DDevice9Ex_CreateTexture(dev,8,8,1,D3DUSAGE_RENDERTARGET,formats9[f],D3DPOOL_DEFAULT,&texture9,&handle));
        HR(ID3D10Device_OpenSharedResource(d,handle,&IID_ID3D10Texture2D,(void**)&texture10));
        HR(IDirect3DTexture9_GetSurfaceLevel(texture9,0,&surface));HR(IDirect3DDevice9Ex_SetRenderTarget(dev,0,surface));HR(IDirect3DDevice9Ex_Clear(dev,0,NULL,D3DCLEAR_TARGET,0xffff0000,1,0));HR(shared9_complete(dev));
        for(UINT i=0;i<64;++i){memcpy(expected+i*bytes,f==2?(void*)red16:(void*)&red32,bytes);}
        shared_read(d,texture10,expected,bytes);
        HR(ID3D10Device_CreateRenderTargetView(d,(ID3D10Resource*)texture10,NULL,&rt));const float green[4]={0,1,0,1};ID3D10Device_ClearRenderTargetView(d,rt,green);HR(shared_complete(d));
        HR(IDirect3DDevice9Ex_CreateOffscreenPlainSurface(dev,8,8,formats9[f],D3DPOOL_SYSTEMMEM,&read9,NULL));HR(IDirect3DDevice9Ex_GetRenderTargetData(dev,surface,read9));D3DLOCKED_RECT locked;HR(IDirect3DSurface9_LockRect(read9,&locked,NULL,D3DLOCK_READONLY));
        for(UINT i=0;i<64;++i)memcpy(expected+i*bytes,f==2?(void*)green16:(void*)&green32,bytes);
        for(UINT y=0;y<8;++y){EXPECT(!memcmp((BYTE*)locked.pBits+y*locked.Pitch,expected+y*8*bytes,8*bytes),"D3D10 to D3D9 shared contents");}
        IDirect3DSurface9_UnlockRect(read9);
        HR(IDirect3DDevice9Ex_SetRenderTarget(dev,0,original));RELEASE(surface);RELEASE(texture9);shared_read(d,texture10,expected,bytes);
        printf("DATA D3D9/D3D10 shared format=%u origin=D3D9 bidirectional=1\n",(unsigned)formats10[f]);
    done:IDirect3DDevice9Ex_SetRenderTarget(dev,0,original);RELEASE(rt);RELEASE(read9);RELEASE(surface);RELEASE(texture9);RELEASE(texture10);
    }
cleanup:RELEASE(original);RELEASE(dev);RELEASE(api);if(window)DestroyWindow(window);if(dll)FreeLibrary(dll);
}

static void resources(ID3D10Device *d,BOOL v101)
{
    ID3D10Buffer *a=NULL,*b=NULL,*dynamic=NULL;
    ID3D10Texture2D *texture=NULL,*read=NULL,*cube=NULL;
    ID3D10ShaderResourceView *view=NULL;
    ID3D10ShaderResourceView1 *view1=NULL;
    ID3D10Device1 *d1=NULL;
    UINT pattern[64],other[64];for(UINT i=0;i<64;++i){pattern[i]=0x10203000+i;other[i]=0xaabbcc00+i;}
    D3D10_BUFFER_DESC bd={sizeof(pattern),D3D10_USAGE_DEFAULT,D3D10_BIND_VERTEX_BUFFER,0,0};
    D3D10_SUBRESOURCE_DATA init={pattern,0,0};void *mapped=NULL;
    HR(ID3D10Device_CreateBuffer(d,&bd,&init,&a));
    bd.Usage=D3D10_USAGE_STAGING;bd.BindFlags=0;bd.CPUAccessFlags=D3D10_CPU_ACCESS_READ;
    HR(ID3D10Device_CreateBuffer(d,&bd,NULL,&b));
    ID3D10Device_CopyResource(d,(ID3D10Resource*)b,(ID3D10Resource*)a);
    HR(ID3D10Buffer_Map(b,D3D10_MAP_READ,0,&mapped));EXPECT(!memcmp(mapped,pattern,sizeof(pattern)),"buffer initial upload/copy/read");ID3D10Buffer_Unmap(b);
    bd.Usage=D3D10_USAGE_DYNAMIC;bd.BindFlags=D3D10_BIND_VERTEX_BUFFER;bd.CPUAccessFlags=D3D10_CPU_ACCESS_WRITE;
    HR(ID3D10Device_CreateBuffer(d,&bd,NULL,&dynamic));
    HR(ID3D10Buffer_Map(dynamic,D3D10_MAP_WRITE_DISCARD,0,&mapped));memcpy(mapped,other,sizeof(other));ID3D10Buffer_Unmap(dynamic);
    HR(ID3D10Buffer_Map(dynamic,D3D10_MAP_WRITE_NO_OVERWRITE,0,&mapped));((UINT*)mapped)[63]=0xfeedface;ID3D10Buffer_Unmap(dynamic);other[63]=0xfeedface;
    ID3D10Device_CopyResource(d,(ID3D10Resource*)b,(ID3D10Resource*)dynamic);
    HR(ID3D10Buffer_Map(b,D3D10_MAP_READ,0,&mapped));EXPECT(!memcmp(mapped,other,sizeof(other)),"discard/nooverwrite content");ID3D10Buffer_Unmap(b);
    D3D10_TEXTURE2D_DESC td={8,8,2,2,DXGI_FORMAT_R8G8B8A8_TYPELESS,{1,0},D3D10_USAGE_DEFAULT,D3D10_BIND_SHADER_RESOURCE|D3D10_BIND_RENDER_TARGET,0,0};
    HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&texture));
    ID3D10Device_UpdateSubresource(d,(ID3D10Resource*)texture,2,NULL,pattern,8*4,0);
    D3D10_SHADER_RESOURCE_VIEW_DESC sd;memset(&sd,0,sizeof(sd));sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.ViewDimension=D3D10_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels=2;sd.Texture2DArray.ArraySize=2;
    HR(ID3D10Device_CreateShaderResourceView(d,(ID3D10Resource*)texture,&sd,&view));
    td.Usage=D3D10_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D10_CPU_ACCESS_READ;
    HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&read));
    D3D10_BOX box={0,0,0,8,8,1};ID3D10Device_CopySubresourceRegion(d,(ID3D10Resource*)read,2,0,0,0,(ID3D10Resource*)texture,2,&box);
    D3D10_MAPPED_TEXTURE2D map;HR(ID3D10Texture2D_Map(read,2,D3D10_MAP_READ,0,&map));
    for(UINT y=0;y<8;++y) { EXPECT(!memcmp((BYTE*)map.pData+y*map.RowPitch,pattern+y*8,32),"texture array subresource row"); }
    ID3D10Texture2D_Unmap(read,2);
    td.ArraySize=v101?12:6;td.MipLevels=1;td.Usage=D3D10_USAGE_DEFAULT;td.BindFlags=D3D10_BIND_SHADER_RESOURCE;td.CPUAccessFlags=0;td.MiscFlags=D3D10_RESOURCE_MISC_TEXTURECUBE;
    HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&cube));RELEASE(view);
    if(v101){
        HR(ID3D10Device_QueryInterface(d,&IID_ID3D10Device1,(void**)&d1));
        D3D10_SHADER_RESOURCE_VIEW_DESC1 desc;memset(&desc,0,sizeof(desc));desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.ViewDimension=D3D10_1_SRV_DIMENSION_TEXTURECUBEARRAY;
        desc.TextureCubeArray.MipLevels=1;desc.TextureCubeArray.First2DArrayFace=6;desc.TextureCubeArray.NumCubes=1;
        HR(ID3D10Device1_CreateShaderResourceView1(d1,(ID3D10Resource*)cube,&desc,&view1));
        D3D10_SHADER_RESOURCE_VIEW_DESC1 got;ID3D10ShaderResourceView1_GetDesc1(view1,&got);
        EXPECT(got.TextureCubeArray.First2DArrayFace==6 && got.TextureCubeArray.NumCubes==1,"10.1 cube view fields");
    }else{
        memset(&sd,0,sizeof(sd));sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.ViewDimension=D3D10_SRV_DIMENSION_TEXTURECUBE;sd.TextureCube.MipLevels=1;
        HR(ID3D10Device_CreateShaderResourceView(d,(ID3D10Resource*)cube,&sd,&view));
    }
    printf("DATA resources checked api=%s\n",v101?"10.1":"10.0");
done:
    RELEASE(d1);RELEASE(view1);RELEASE(view);RELEASE(cube);RELEASE(read);RELEASE(texture);RELEASE(dynamic);RELEASE(b);RELEASE(a);
}

static void more_resources(ID3D10Device *d,BOOL v101)
{
    ID3D10Texture1D *line=NULL,*lineRead=NULL;ID3D10Texture3D *volume=NULL,*volumeRead=NULL;
    ID3D10Texture2D *shared=NULL,*sharedOpen=NULL,*sharedRead=NULL,*blocks=NULL,*compressed=NULL,*compressedRead=NULL;
    IDXGIResource *dxgi=NULL;UINT pattern[64];for(UINT i=0;i<64;++i)pattern[i]=0x12345600+i;
    D3D10_SUBRESOURCE_DATA init={pattern,16,64};
    D3D10_TEXTURE1D_DESC one={4,1,1,DXGI_FORMAT_R32_UINT,D3D10_USAGE_DEFAULT,D3D10_BIND_SHADER_RESOURCE,0,0};
    HR(ID3D10Device_CreateTexture1D(d,&one,&init,&line));one.Usage=D3D10_USAGE_STAGING;one.BindFlags=0;one.CPUAccessFlags=D3D10_CPU_ACCESS_READ;
    HR(ID3D10Device_CreateTexture1D(d,&one,NULL,&lineRead));ID3D10Device_CopyResource(d,(ID3D10Resource*)lineRead,(ID3D10Resource*)line);
    void *mapped=NULL;HR(ID3D10Texture1D_Map(lineRead,0,D3D10_MAP_READ,0,&mapped));EXPECT(!memcmp(mapped,pattern,16),"1D texture contents");ID3D10Texture1D_Unmap(lineRead,0);
    D3D10_TEXTURE3D_DESC three={4,4,4,1,DXGI_FORMAT_R32_UINT,D3D10_USAGE_DEFAULT,D3D10_BIND_SHADER_RESOURCE,0,0};
    HR(ID3D10Device_CreateTexture3D(d,&three,&init,&volume));three.Usage=D3D10_USAGE_STAGING;three.BindFlags=0;three.CPUAccessFlags=D3D10_CPU_ACCESS_READ;
    HR(ID3D10Device_CreateTexture3D(d,&three,NULL,&volumeRead));ID3D10Device_CopyResource(d,(ID3D10Resource*)volumeRead,(ID3D10Resource*)volume);
    D3D10_MAPPED_TEXTURE3D map3;HR(ID3D10Texture3D_Map(volumeRead,0,D3D10_MAP_READ,0,&map3));
    for(UINT z=0;z<4;++z)for(UINT y=0;y<4;++y) {EXPECT(!memcmp((BYTE*)map3.pData+z*map3.DepthPitch+y*map3.RowPitch,pattern+z*16+y*4,16),"3D row/depth contents");}
    ID3D10Texture3D_Unmap(volumeRead,0);
    D3D10_TEXTURE2D_DESC td={8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D10_USAGE_DEFAULT,D3D10_BIND_SHADER_RESOURCE|D3D10_BIND_RENDER_TARGET,0,D3D10_RESOURCE_MISC_SHARED};
    HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&shared));HR(ID3D10Texture2D_QueryInterface(shared,&IID_IDXGIResource,(void**)&dxgi));
    HANDLE handle=NULL;HR(IDXGIResource_GetSharedHandle(dxgi,&handle));EXPECT(handle!=NULL,"shared kernel handle");
    HR(ID3D10Device_OpenSharedResource(d,handle,&IID_ID3D10Texture2D,(void**)&sharedOpen));
    printf("STAGE shared update api=%s\n",v101?"10.1":"10.0");
    ID3D10Device_UpdateSubresource(d,(ID3D10Resource*)shared,0,NULL,pattern,32,0);
    td.Usage=D3D10_USAGE_STAGING;td.BindFlags=td.MiscFlags=0;td.CPUAccessFlags=D3D10_CPU_ACCESS_READ;HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&sharedRead));
    ID3D10Device_CopyResource(d,(ID3D10Resource*)sharedRead,(ID3D10Resource*)sharedOpen);D3D10_MAPPED_TEXTURE2D map;
    HR(ID3D10Texture2D_Map(sharedRead,0,D3D10_MAP_READ,0,&map));
    printf("STAGE shared mapped api=%s\n",v101?"10.1":"10.0");
    for(UINT y=0;y<8;++y) {EXPECT(!memcmp((BYTE*)map.pData+y*map.RowPitch,pattern+y*8,32),"shared alias data");}
    ID3D10Texture2D_Unmap(sharedRead,0);
    if(v101){
        puts("STAGE 10.1 BC reinterpretation create");
        td.Width=td.Height=4;td.Format=DXGI_FORMAT_R32G32B32A32_UINT;td.Usage=D3D10_USAGE_DEFAULT;td.BindFlags=D3D10_BIND_SHADER_RESOURCE;td.CPUAccessFlags=0;init.SysMemPitch=64;
        HR(ID3D10Device_CreateTexture2D(d,&td,&init,&blocks));td.Width=td.Height=16;td.Format=DXGI_FORMAT_BC3_UNORM;
        HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&compressed));puts("STAGE 10.1 BC reinterpretation copy");ID3D10Device_CopyResource(d,(ID3D10Resource*)compressed,(ID3D10Resource*)blocks);
        td.Usage=D3D10_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D10_CPU_ACCESS_READ;HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&compressedRead));
        ID3D10Device_CopyResource(d,(ID3D10Resource*)compressedRead,(ID3D10Resource*)compressed);puts("STAGE 10.1 BC reinterpretation map");HR(ID3D10Texture2D_Map(compressedRead,0,D3D10_MAP_READ,0,&map));
        for(UINT row=0;row<4;++row) {EXPECT(!memcmp((BYTE*)map.pData+row*map.RowPitch,pattern+row*16,64),"10.1 BC3 bit reinterpretation");}
        ID3D10Texture2D_Unmap(compressedRead,0);
    }
    printf("DATA 1D/3D/shared/conversion checked api=%s\n",v101?"10.1":"10.0");
done:
    RELEASE(compressedRead);RELEASE(compressed);RELEASE(blocks);RELEASE(sharedRead);RELEASE(sharedOpen);RELEASE(dxgi);RELEASE(shared);
    RELEASE(volumeRead);RELEASE(volume);RELEASE(lineRead);RELEASE(line);
}

static void rendering(ID3D10Device *d)
{
    const char *hlsl="cbuffer C : register(b0) {float4 colour;} struct V {float4 pos:SV_Position;}; V vs(float4 p:POSITION){V o;o.pos=p;return o;} float4 ps(V i):SV_Target{return colour;} Texture2D tex:register(t0); SamplerState samp:register(s0); float4 samplePS(V i):SV_Target{return tex.Sample(samp,i.pos.xy/64.0);}";
    ID3D10Blob *vsCode=NULL,*psCode=NULL,*sampleCode=NULL;
    ID3D10VertexShader *vs=NULL;ID3D10PixelShader *ps=NULL,*samplePS=NULL;ID3D10GeometryShader *so=NULL;
    ID3D10SamplerState *sampler=NULL;ID3D10ShaderResourceView *sampleView=NULL;
    ID3D10Texture2D *sampleTexture=NULL,*mipRead=NULL,*msaa=NULL;ID3D10RenderTargetView *msaaRT=NULL;
    ID3D10InputLayout *layout=NULL;ID3D10RasterizerState *raster=NULL;
    ID3D10DepthStencilState *depthState=NULL;ID3D10BlendState *blend=NULL;
    ID3D10Texture2D *target=NULL,*read=NULL,*depth=NULL;ID3D10RenderTargetView *rt=NULL;ID3D10DepthStencilView *ds=NULL;
    ID3D10Buffer *vertices=NULL,*indices=NULL,*constant=NULL,*output=NULL,*soRead=NULL;
    ID3D10Query *query=NULL,*stats=NULL,*event=NULL,*timestamp=NULL;ID3D10Predicate *predicate=NULL;
    vsCode=compile(hlsl,"vs","vs_4_0");psCode=compile(hlsl,"ps","ps_4_0");if(!vsCode||!psCode)goto done;
    HR(ID3D10Device_CreateVertexShader(d,vsCode->lpVtbl->GetBufferPointer(vsCode),vsCode->lpVtbl->GetBufferSize(vsCode),&vs));
    HR(ID3D10Device_CreatePixelShader(d,psCode->lpVtbl->GetBufferPointer(psCode),psCode->lpVtbl->GetBufferSize(psCode),&ps));
    D3D10_INPUT_ELEMENT_DESC ie={"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D10_INPUT_PER_VERTEX_DATA,0};
    HR(ID3D10Device_CreateInputLayout(d,&ie,1,vsCode->lpVtbl->GetBufferPointer(vsCode),vsCode->lpVtbl->GetBufferSize(vsCode),&layout));
    float v[3][4]={{-1,-1,.5f,1},{-1,3,.5f,1},{3,-1,.5f,1}};UINT ix[3]={0,1,2};
    D3D10_BUFFER_DESC bd={sizeof(v),D3D10_USAGE_DEFAULT,D3D10_BIND_VERTEX_BUFFER,0,0};D3D10_SUBRESOURCE_DATA initial={v,0,0};
    HR(ID3D10Device_CreateBuffer(d,&bd,&initial,&vertices));bd.ByteWidth=sizeof(ix);bd.BindFlags=D3D10_BIND_INDEX_BUFFER;initial.pSysMem=ix;HR(ID3D10Device_CreateBuffer(d,&bd,&initial,&indices));
    bd.ByteWidth=16;bd.BindFlags=D3D10_BIND_CONSTANT_BUFFER;HR(ID3D10Device_CreateBuffer(d,&bd,NULL,&constant));
    D3D10_TEXTURE2D_DESC td={64,64,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D10_USAGE_DEFAULT,D3D10_BIND_RENDER_TARGET,0,0};
    HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&target));HR(ID3D10Device_CreateRenderTargetView(d,(ID3D10Resource*)target,NULL,&rt));
    td.Usage=D3D10_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D10_CPU_ACCESS_READ;HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&read));
    td.Usage=D3D10_USAGE_DEFAULT;td.BindFlags=D3D10_BIND_DEPTH_STENCIL;td.CPUAccessFlags=0;td.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&depth));
    HR(ID3D10Device_CreateDepthStencilView(d,(ID3D10Resource*)depth,NULL,&ds));
    D3D10_RASTERIZER_DESC rd;memset(&rd,0,sizeof(rd));rd.FillMode=D3D10_FILL_SOLID;rd.CullMode=D3D10_CULL_NONE;rd.DepthClipEnable=TRUE;rd.ScissorEnable=TRUE;
    HR(ID3D10Device_CreateRasterizerState(d,&rd,&raster));
    D3D10_DEPTH_STENCIL_DESC dd;memset(&dd,0,sizeof(dd));dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D10_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D10_COMPARISON_LESS;
    dd.StencilReadMask=dd.StencilWriteMask=255;dd.FrontFace.StencilFailOp=dd.FrontFace.StencilDepthFailOp=dd.FrontFace.StencilPassOp=D3D10_STENCIL_OP_KEEP;dd.FrontFace.StencilFunc=D3D10_COMPARISON_ALWAYS;dd.BackFace=dd.FrontFace;
    HR(ID3D10Device_CreateDepthStencilState(d,&dd,&depthState));
    D3D10_VIEWPORT vp={0,0,64,64,0,1};D3D10_RECT rect={0,0,32,64};UINT stride=16,offset=0;
    ID3D10Device_IASetInputLayout(d,layout);ID3D10Device_IASetVertexBuffers(d,0,1,&vertices,&stride,&offset);ID3D10Device_IASetIndexBuffer(d,indices,DXGI_FORMAT_R32_UINT,0);
    ID3D10Device_IASetPrimitiveTopology(d,D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ID3D10Device_VSSetShader(d,vs);ID3D10Device_PSSetShader(d,ps);ID3D10Device_PSSetConstantBuffers(d,0,1,&constant);
    ID3D10Device_RSSetState(d,raster);ID3D10Device_RSSetViewports(d,1,&vp);ID3D10Device_RSSetScissorRects(d,1,&rect);ID3D10Device_OMSetRenderTargets(d,1,&rt,ds);ID3D10Device_OMSetDepthStencilState(d,depthState,0);
    float red[4]={1,0,0,1},green[4]={0,1,0,1},black[4]={0,0,0,1};
    ID3D10Device_ClearRenderTargetView(d,rt,black);ID3D10Device_ClearDepthStencilView(d,ds,D3D10_CLEAR_DEPTH|D3D10_CLEAR_STENCIL,.25f,0);
    ID3D10Device_UpdateSubresource(d,(ID3D10Resource*)constant,0,NULL,red,0,0);ID3D10Device_DrawIndexedInstanced(d,3,1,0,0,0);
    pixel(d,target,read,16,32,0xff000000,"depth reject");
    ID3D10Device_ClearDepthStencilView(d,ds,D3D10_CLEAR_DEPTH,1,0);ID3D10Device_DrawIndexed(d,3,0,0);
    pixel(d,target,read,16,32,0xff0000ff,"indexed red");pixel(d,target,read,48,32,0xff000000,"scissor preserve");
    D3D10_QUERY_DESC qd={D3D10_QUERY_OCCLUSION,0};HR(ID3D10Device_CreateQuery(d,&qd,&query));
    qd.Query=D3D10_QUERY_PIPELINE_STATISTICS;HR(ID3D10Device_CreateQuery(d,&qd,&stats));qd.Query=D3D10_QUERY_EVENT;HR(ID3D10Device_CreateQuery(d,&qd,&event));
    qd.Query=D3D10_QUERY_OCCLUSION_PREDICATE;HR(ID3D10Device_CreatePredicate(d,&qd,&predicate));
    ID3D10Device_ClearDepthStencilView(d,ds,D3D10_CLEAR_DEPTH,1,0);ID3D10Device_UpdateSubresource(d,(ID3D10Resource*)constant,0,NULL,green,0,0);
    ID3D10Query_Begin(query);ID3D10Query_Begin(stats);ID3D10Predicate_Begin(predicate);ID3D10Device_DrawInstanced(d,3,1,0,0);ID3D10Predicate_End(predicate);ID3D10Query_End(stats);ID3D10Query_End(query);ID3D10Query_End(event);
    UINT64 samples=0;HR(query_wait(query,&samples,sizeof(samples)));EXPECT(samples>0,"occlusion samples");
    struct {UINT64 before;D3D10_QUERY_DATA_PIPELINE_STATISTICS data;UINT64 after;} pipeline;memset(&pipeline,0xca,sizeof(pipeline));
    HR(query_wait(stats,&pipeline.data,sizeof(pipeline.data)));EXPECT(pipeline.data.IAVertices==3 && pipeline.data.IAPrimitives==1,"pipeline counters");EXPECT(pipeline.before==0xcacacacacacacacaULL && pipeline.after==pipeline.before,"pipeline canaries");
    BOOL ready=FALSE;HR(query_wait(event,&ready,sizeof(ready)));EXPECT(ready,"event complete");
    pixel(d,target,read,16,32,0xff00ff00,"instanced green");
    qd.Query=D3D10_QUERY_TIMESTAMP;HR(ID3D10Device_CreateQuery(d,&qd,&timestamp));
    UINT64 previous_timestamp=0;
    for(UINT cycle=0;cycle<3;++cycle){
        Sleep(2);ID3D10Device_ClearDepthStencilView(d,ds,D3D10_CLEAR_DEPTH,1,0);
        ID3D10Query_Begin(query);ID3D10Query_Begin(stats);
        ID3D10Device_DrawInstanced(d,3,cycle,0,0);
        ID3D10Query_End(stats);ID3D10Query_End(query);ID3D10Query_End(event);ID3D10Query_End(timestamp);
        ID3D10Device_Flush(d);
        samples=UINT64_MAX;memset(&pipeline.data,0xca,sizeof(pipeline.data));ready=FALSE;
        HR(query_wait_flags(query,&samples,sizeof(samples),D3D10_ASYNC_GETDATA_DONOTFLUSH));
        EXPECT(cycle?samples>0:samples==0,"reissued occlusion result");
        HR(query_wait_flags(stats,&pipeline.data,sizeof(pipeline.data),D3D10_ASYNC_GETDATA_DONOTFLUSH));
        EXPECT(pipeline.data.IAVertices==3*cycle && pipeline.data.IAPrimitives==cycle,"reissued pipeline result");
        HR(query_wait_flags(event,&ready,sizeof(ready),D3D10_ASYNC_GETDATA_DONOTFLUSH));EXPECT(ready,"reissued event complete");
        UINT64 stamp=0;HR(query_wait_flags(timestamp,&stamp,sizeof(stamp),D3D10_ASYNC_GETDATA_DONOTFLUSH));
        EXPECT(stamp>previous_timestamp,"reissued timestamp advances");previous_timestamp=stamp;
    }
    ID3D10Device_ClearDepthStencilView(d,ds,D3D10_CLEAR_DEPTH,1,0);ID3D10Device_UpdateSubresource(d,(ID3D10Resource*)constant,0,NULL,red,0,0);ID3D10Device_SetPredication(d,predicate,TRUE);ID3D10Device_Draw(d,3,0);
    ID3D10Device_SetPredication(d,NULL,FALSE);pixel(d,target,read,16,32,0xff00ff00,"true predicate skips");
    D3D10_SO_DECLARATION_ENTRY decl={"SV_Position",0,0,4,0};
    HR(ID3D10Device_CreateGeometryShaderWithStreamOutput(d,vsCode->lpVtbl->GetBufferPointer(vsCode),vsCode->lpVtbl->GetBufferSize(vsCode),&decl,1,16,&so));
    bd.ByteWidth=sizeof(v);bd.BindFlags=D3D10_BIND_STREAM_OUTPUT;HR(ID3D10Device_CreateBuffer(d,&bd,NULL,&output));bd.Usage=D3D10_USAGE_STAGING;bd.BindFlags=0;bd.CPUAccessFlags=D3D10_CPU_ACCESS_READ;HR(ID3D10Device_CreateBuffer(d,&bd,NULL,&soRead));
    ID3D10Device_OMSetRenderTargets(d,0,NULL,NULL);ID3D10Device_GSSetShader(d,so);ID3D10Device_SOSetTargets(d,1,&output,&offset);ID3D10Device_Draw(d,3,0);ID3D10Device_SOSetTargets(d,0,NULL,NULL);ID3D10Device_GSSetShader(d,NULL);
    ID3D10Device_CopyResource(d,(ID3D10Resource*)soRead,(ID3D10Resource*)output);void *mapped=NULL;HR(ID3D10Buffer_Map(soRead,D3D10_MAP_READ,0,&mapped));
    for(UINT vertex=0;vertex<3;++vertex){const float *got=(const float*)mapped+vertex*4;printf("SO vertex=%u got=%g,%g,%g,%g expected=%g,%g,%g,%g\n",vertex,got[0],got[1],got[2],got[3],v[vertex][0],v[vertex][1],v[vertex][2],v[vertex][3]);}
    EXPECT(!memcmp(mapped,v,sizeof(v)),"VS stream output passthrough");ID3D10Buffer_Unmap(soRead);
    D3D10_BLEND_DESC blendDesc;memset(&blendDesc,0,sizeof(blendDesc));blendDesc.BlendEnable[0]=TRUE;blendDesc.SrcBlend=D3D10_BLEND_ONE;blendDesc.DestBlend=D3D10_BLEND_ONE;blendDesc.BlendOp=D3D10_BLEND_OP_ADD;blendDesc.SrcBlendAlpha=D3D10_BLEND_ONE;blendDesc.DestBlendAlpha=D3D10_BLEND_ZERO;blendDesc.BlendOpAlpha=D3D10_BLEND_OP_ADD;blendDesc.RenderTargetWriteMask[0]=15;
    ID3D10Device_OMSetRenderTargets(d,1,&rt,ds);HR(ID3D10Device_CreateBlendState(d,&blendDesc,&blend));ID3D10Device_OMSetBlendState(d,blend,NULL,~0u);ID3D10Device_ClearRenderTargetView(d,rt,green);ID3D10Device_ClearDepthStencilView(d,ds,D3D10_CLEAR_DEPTH,1,0);ID3D10Device_Draw(d,3,0);pixel(d,target,read,16,32,0xff00ffff,"additive yellow");
    /* Texture sampling, generated mip data and MSAA resolve use independent
     * known colours; successful creation alone is not the oracle. */
    ID3D10Device_OMSetBlendState(d,NULL,NULL,~0u);ID3D10Device_OMSetRenderTargets(d,1,&rt,NULL);
    rect.right=64;ID3D10Device_RSSetScissorRects(d,1,&rect);
    sampleCode=compile(hlsl,"samplePS","ps_4_0");if(!sampleCode)goto done;
    HR(ID3D10Device_CreatePixelShader(d,sampleCode->lpVtbl->GetBufferPointer(sampleCode),sampleCode->lpVtbl->GetBufferSize(sampleCode),&samplePS));
    D3D10_SAMPLER_DESC samplerDesc;memset(&samplerDesc,0,sizeof(samplerDesc));samplerDesc.Filter=D3D10_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D10_TEXTURE_ADDRESS_CLAMP;samplerDesc.ComparisonFunc=D3D10_COMPARISON_NEVER;samplerDesc.MaxLOD=100;
    HR(ID3D10Device_CreateSamplerState(d,&samplerDesc,&sampler));
    td.Width=td.Height=4;td.MipLevels=3;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D10_BIND_SHADER_RESOURCE|D3D10_BIND_RENDER_TARGET;td.MiscFlags=D3D10_RESOURCE_MISC_GENERATE_MIPS;
    HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&sampleTexture));HR(ID3D10Device_CreateShaderResourceView(d,(ID3D10Resource*)sampleTexture,NULL,&sampleView));
    UINT colours[16];for(UINT y=0;y<4;++y)for(UINT x=0;x<4;++x)colours[y*4+x]=y<2?(x<2?0xff0000ff:0xff00ff00):(x<2?0xffff0000:0xffffffff);
    ID3D10Device_UpdateSubresource(d,(ID3D10Resource*)sampleTexture,0,NULL,colours,16,0);
    ID3D10Device_PSSetShader(d,samplePS);ID3D10Device_PSSetShaderResources(d,0,1,&sampleView);ID3D10Device_PSSetSamplers(d,0,1,&sampler);ID3D10Device_Draw(d,3,0);
    pixel(d,target,read,16,16,0xff0000ff,"sample red");pixel(d,target,read,48,16,0xff00ff00,"sample green");pixel(d,target,read,16,48,0xffff0000,"sample blue");pixel(d,target,read,48,48,0xffffffff,"sample white");
    ID3D10ShaderResourceView *nullView=NULL;ID3D10Device_PSSetShaderResources(d,0,1,&nullView);
    for(UINT i=0;i<16;++i)colours[i]=0xffffff00;
    ID3D10Device_UpdateSubresource(d,(ID3D10Resource*)sampleTexture,0,NULL,colours,16,0);ID3D10Device_GenerateMips(d,sampleView);
    td.Usage=D3D10_USAGE_STAGING;td.BindFlags=td.MiscFlags=0;td.CPUAccessFlags=D3D10_CPU_ACCESS_READ;HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&mipRead));
    ID3D10Device_CopyResource(d,(ID3D10Resource*)mipRead,(ID3D10Resource*)sampleTexture);D3D10_MAPPED_TEXTURE2D mipMap;
    HR(ID3D10Texture2D_Map(mipRead,2,D3D10_MAP_READ,0,&mipMap));EXPECT(*(UINT*)mipMap.pData==0xffffff00,"generated last mip cyan");ID3D10Texture2D_Unmap(mipRead,2);
    UINT quality=0;HR(ID3D10Device_CheckMultisampleQualityLevels(d,DXGI_FORMAT_R8G8B8A8_UNORM,4,&quality));EXPECT(quality>0,"required RGBA8 MSAA4 support");
    if(quality){td.Width=td.Height=64;td.MipLevels=1;td.SampleDesc.Count=4;td.Usage=D3D10_USAGE_DEFAULT;td.BindFlags=D3D10_BIND_RENDER_TARGET;td.CPUAccessFlags=0;
        HR(ID3D10Device_CreateTexture2D(d,&td,NULL,&msaa));HR(ID3D10Device_CreateRenderTargetView(d,(ID3D10Resource*)msaa,NULL,&msaaRT));float yellow[4]={1,1,0,1};
        ID3D10Device_ClearRenderTargetView(d,msaaRT,yellow);ID3D10Device_OMSetRenderTargets(d,0,NULL,NULL);ID3D10Device_ResolveSubresource(d,(ID3D10Resource*)target,0,(ID3D10Resource*)msaa,0,DXGI_FORMAT_R8G8B8A8_UNORM);
        pixel(d,target,read,32,32,0xff00ffff,"MSAA resolve yellow");}
    EXPECT(SUCCEEDED(ID3D10Device_GetDeviceRemovedReason(d)),"device healthy after rendering");
    printf("DATA rendering/shader/query/SO checked\n");
done:
    ID3D10Device_ClearState(d);ID3D10Device_Flush(d);
    RELEASE(msaaRT);RELEASE(msaa);RELEASE(mipRead);RELEASE(sampleView);RELEASE(sampleTexture);RELEASE(sampler);RELEASE(samplePS);RELEASE(sampleCode);
    RELEASE(timestamp);RELEASE(predicate);RELEASE(event);RELEASE(stats);RELEASE(query);RELEASE(soRead);RELEASE(output);RELEASE(constant);RELEASE(indices);RELEASE(vertices);
    RELEASE(blend);RELEASE(depthState);RELEASE(raster);RELEASE(layout);RELEASE(ds);RELEASE(rt);RELEASE(depth);RELEASE(read);RELEASE(target);RELEASE(so);RELEASE(ps);RELEASE(vs);RELEASE(psCode);RELEASE(vsCode);
}

static ID3D10Device *create_device_flags(BOOL v101,D3D10_FEATURE_LEVEL1 level,UINT flags)
{
    typedef HRESULT (WINAPI *Create10)(IDXGIAdapter*,D3D10_DRIVER_TYPE,HMODULE,UINT,UINT,ID3D10Device**);
    typedef HRESULT (WINAPI *Create101)(IDXGIAdapter*,D3D10_DRIVER_TYPE,HMODULE,UINT,D3D10_FEATURE_LEVEL1,UINT,ID3D10Device1**);
    ID3D10Device *d=NULL;HMODULE api=LoadLibraryA(v101?"d3d10_1.dll":"d3d10.dll");
    if(!api){++failures;printf("FAIL runtime missing %lu\n",GetLastError());return NULL;}
    HRESULT hr;
    if(v101){Create101 f=(Create101)(void*)GetProcAddress(api,"D3D10CreateDevice1");hr=f?f(NULL,D3D10_DRIVER_TYPE_HARDWARE,NULL,flags,level,D3D10_1_SDK_VERSION,(ID3D10Device1**)&d):E_NOINTERFACE;}
    else {Create10 f=(Create10)(void*)GetProcAddress(api,"D3D10CreateDevice");hr=f?f(NULL,D3D10_DRIVER_TYPE_HARDWARE,NULL,flags,D3D10_SDK_VERSION,&d):E_NOINTERFACE;}
    printf("DEVICE api=%s level=%04x flags=%x hr=%08lx arch=%u\n",v101?"10.1":"10.0",level,flags,(unsigned long)hr,(unsigned)(8*sizeof(void*)));
    EXPECT(SUCCEEDED(hr)&&d,"hardware device creation");
    if(d){
        HMODULE umd=GetModuleHandleA("neptune_d3d10.dll");if(!umd)umd=GetModuleHandleA("neptune_d3d10_wow.dll");EXPECT(umd,"Triton D3D10 module loaded");
        IDXGIDevice *dxgi=NULL;IDXGIAdapter *adapter=NULL;DXGI_ADAPTER_DESC desc;
        if(SUCCEEDED(ID3D10Device_QueryInterface(d,&IID_IDXGIDevice,(void**)&dxgi)) && SUCCEEDED(IDXGIDevice_GetAdapter(dxgi,&adapter)) && SUCCEEDED(IDXGIAdapter_GetDesc(adapter,&desc))) {
            printf("ADAPTER vendor=%04x device=%04x\n",desc.VendorId,desc.DeviceId);EXPECT(desc.VendorId==0x1af4 && desc.DeviceId==0x1050,"virtio adapter identity");
        }else EXPECT(FALSE,"adapter identity query");RELEASE(adapter);RELEASE(dxgi);
    }
    /* Keep the public runtime loaded through COM object lifetimes. */
    return d;
}
static ID3D10Device *create_device(BOOL v101)
{ return create_device_flags(v101,v101?D3D10_FEATURE_LEVEL_10_1:D3D10_FEATURE_LEVEL_10_0,0); }

static BOOL extended_format_caps_valid(DXGI_FORMAT format,UINT support)
{
    /* R/O/N/A attributes in the extended-format contract. Backbuffer casting
     * is exercised separately; only displayable typed formats may report it.
     * https://learn.microsoft.com/windows-hardware/drivers/display/details-of-the-extended-format */
    UINT required=D3D10_FORMAT_SUPPORT_TEXTURE1D|D3D10_FORMAT_SUPPORT_TEXTURE2D|
        D3D10_FORMAT_SUPPORT_TEXTURE3D|D3D10_FORMAT_SUPPORT_TEXTURECUBE|
        D3D10_FORMAT_SUPPORT_MIP|D3D10_FORMAT_SUPPORT_CPU_LOCKABLE|
        D3D10_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT;
    UINT optional=0;
    switch(format){
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        required|=D3D10_FORMAT_SUPPORT_SHADER_LOAD|D3D10_FORMAT_SUPPORT_SHADER_SAMPLE|
            D3D10_FORMAT_SUPPORT_MIP_AUTOGEN|D3D10_FORMAT_SUPPORT_RENDER_TARGET|
            D3D10_FORMAT_SUPPORT_BLENDABLE|D3D10_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE|
            D3D10_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
        optional=D3D10_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
        if(format==DXGI_FORMAT_B8G8R8A8_UNORM||format==DXGI_FORMAT_B8G8R8X8_UNORM)
            required|=D3D10_FORMAT_SUPPORT_BUFFER|D3D10_FORMAT_SUPPORT_IA_VERTEX_BUFFER;
        if(format==DXGI_FORMAT_B8G8R8A8_UNORM||format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB){
            required|=D3D10_FORMAT_SUPPORT_DISPLAY;
            optional|=D3D10_FORMAT_SUPPORT_BACK_BUFFER_CAST;
        }
        break;
    default:return FALSE;
    }
    return (support&required)==required && !(support&~(required|optional));
}

static void extended_formats(ID3D10Device *d)
{
    const DXGI_FORMAT formats[]={DXGI_FORMAT_B8G8R8A8_TYPELESS,DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8X8_TYPELESS,
        DXGI_FORMAT_B8G8R8X8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM_SRGB};
    UINT pattern[64];for(UINT i=0;i<64;++i)pattern[i]=0xffff0000u;
    for(UINT f=0;f<6;++f){
        ID3D10Texture1D *one=NULL,*oneRead=NULL;ID3D10Texture3D *three=NULL,*threeRead=NULL;
        ID3D10Texture2D *cube=NULL,*mips=NULL,*mipRead=NULL;ID3D10ShaderResourceView *view=NULL;
        BOOL typeless=f==0||f==3;DXGI_FORMAT typed=typeless?formats[f+1]:formats[f];UINT support=0;
        HR(ID3D10Device_CheckFormatSupport(d,formats[f],&support));
        printf("EXTENDED format=%u support=%08x\n",formats[f],support);
        EXPECT(extended_format_caps_valid(formats[f],support),"extended format mandatory and forbidden capabilities");
        D3D10_SUBRESOURCE_DATA initial={pattern,32,128};
        D3D10_TEXTURE1D_DESC a={8,1,1,formats[f],D3D10_USAGE_DEFAULT,D3D10_BIND_SHADER_RESOURCE,0,0};
        HR(ID3D10Device_CreateTexture1D(d,&a,&initial,&one));a.Usage=D3D10_USAGE_STAGING;a.BindFlags=0;a.CPUAccessFlags=D3D10_CPU_ACCESS_READ;
        HR(ID3D10Device_CreateTexture1D(d,&a,NULL,&oneRead));ID3D10Device_CopyResource(d,(ID3D10Resource*)oneRead,(ID3D10Resource*)one);
        void *data=NULL;HR(ID3D10Texture1D_Map(oneRead,0,D3D10_MAP_READ,0,&data));
        EXPECT(!memcmp(data,pattern,32),"extended 1D initial data and readback");ID3D10Texture1D_Unmap(oneRead,0);
        D3D10_TEXTURE3D_DESC b={8,4,2,1,formats[f],D3D10_USAGE_DEFAULT,D3D10_BIND_SHADER_RESOURCE,0,0};
        HR(ID3D10Device_CreateTexture3D(d,&b,&initial,&three));b.Usage=D3D10_USAGE_STAGING;b.BindFlags=0;b.CPUAccessFlags=D3D10_CPU_ACCESS_READ;
        HR(ID3D10Device_CreateTexture3D(d,&b,NULL,&threeRead));ID3D10Device_CopyResource(d,(ID3D10Resource*)threeRead,(ID3D10Resource*)three);
        D3D10_MAPPED_TEXTURE3D map3;HR(ID3D10Texture3D_Map(threeRead,0,D3D10_MAP_READ,0,&map3));
        for(UINT z=0;z<2;++z)for(UINT y=0;y<4;++y)
            EXPECT(!memcmp((BYTE*)map3.pData+z*map3.DepthPitch+y*map3.RowPitch,pattern,32),"extended 3D slice bytes");
        ID3D10Texture3D_Unmap(threeRead,0);
        D3D10_TEXTURE2D_DESC c={8,8,1,6,formats[f],{1,0},D3D10_USAGE_DEFAULT,D3D10_BIND_SHADER_RESOURCE,0,D3D10_RESOURCE_MISC_TEXTURECUBE};
        HR(ID3D10Device_CreateTexture2D(d,&c,NULL,&cube));D3D10_SHADER_RESOURCE_VIEW_DESC sv={0};sv.Format=typed;sv.ViewDimension=D3D10_SRV_DIMENSION_TEXTURECUBE;sv.TextureCube.MipLevels=1;
        HR(ID3D10Device_CreateShaderResourceView(d,(ID3D10Resource*)cube,&sv,&view));RELEASE(view);
        c.ArraySize=1;c.MipLevels=4;c.BindFlags|=D3D10_BIND_RENDER_TARGET;c.MiscFlags=D3D10_RESOURCE_MISC_GENERATE_MIPS;
        HR(ID3D10Device_CreateTexture2D(d,&c,NULL,&mips));sv.ViewDimension=D3D10_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=4;
        HR(ID3D10Device_CreateShaderResourceView(d,(ID3D10Resource*)mips,&sv,&view));
        ID3D10Device_UpdateSubresource(d,(ID3D10Resource*)mips,0,NULL,pattern,32,0);ID3D10Device_GenerateMips(d,view);
        c.Usage=D3D10_USAGE_STAGING;c.BindFlags=c.MiscFlags=0;c.CPUAccessFlags=D3D10_CPU_ACCESS_READ;
        HR(ID3D10Device_CreateTexture2D(d,&c,NULL,&mipRead));ID3D10Device_CopyResource(d,(ID3D10Resource*)mipRead,(ID3D10Resource*)mips);
        D3D10_MAPPED_TEXTURE2D map2;HR(ID3D10Texture2D_Map(mipRead,3,D3D10_MAP_READ,0,&map2));
        UINT mask=f>=3?0x00ffffffu:UINT32_MAX;
        EXPECT((*(UINT*)map2.pData&mask)==(pattern[0]&mask),"extended mip generation bytes");ID3D10Texture2D_Unmap(mipRead,3);
    done:RELEASE(view);RELEASE(mipRead);RELEASE(mips);RELEASE(cube);RELEASE(threeRead);RELEASE(three);RELEASE(oneRead);RELEASE(one);
    }
    EXPECT(SUCCEEDED(ID3D10Device_GetDeviceRemovedReason(d)),"extended formats device health");
}

static void extended_present(ID3D10Device *d,BOOL fullscreen)
{
    const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    const DXGI_FORMAT views[]={DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM};
    IDXGIDevice *dxgi=NULL;IDXGIAdapter *adapter=NULL;IDXGIFactory *factory=NULL;
    HWND window=CreateWindowA("STATIC","Vista extended format oracle",WS_OVERLAPPEDWINDOW|WS_VISIBLE,80,80,320,240,NULL,NULL,GetModuleHandle(NULL),NULL);
    EXPECT(window!=NULL,"extended presentation window");if(!window)return;
    HR(ID3D10Device_QueryInterface(d,&IID_IDXGIDevice,(void**)&dxgi));HR(IDXGIDevice_GetAdapter(dxgi,&adapter));
    HR(IDXGIAdapter_GetParent(adapter,&IID_IDXGIFactory,(void**)&factory));
    HR(IDXGIFactory_MakeWindowAssociation(factory,window,DXGI_MWA_NO_ALT_ENTER));
    for(UINT f=0;f<3;++f){
        IDXGISwapChain *swap=NULL;ID3D10Texture2D *buffer=NULL;ID3D10RenderTargetView *view=NULL;
        DXGI_SWAP_CHAIN_DESC desc={0};desc.BufferDesc.Width=160;desc.BufferDesc.Height=120;desc.BufferDesc.Format=formats[f];
        desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=window;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        HRESULT hr=IDXGIFactory_CreateSwapChain(factory,(IUnknown*)d,&desc,&swap);EXPECT(SUCCEEDED(hr)&&swap,"extended swapchain creation");
        if(FAILED(hr)||!swap)goto next;
        if(fullscreen){hr=IDXGISwapChain_SetFullscreenState(swap,TRUE,NULL);EXPECT(SUCCEEDED(hr),"extended fullscreen state");if(FAILED(hr))goto next;}
        hr=IDXGISwapChain_GetBuffer(swap,0,&IID_ID3D10Texture2D,(void**)&buffer);EXPECT(SUCCEEDED(hr)&&buffer,"extended swapchain buffer");if(FAILED(hr)||!buffer)goto next;
        D3D10_RENDER_TARGET_VIEW_DESC rv={0};rv.Format=views[f];rv.ViewDimension=D3D10_RTV_DIMENSION_TEXTURE2D;
        hr=ID3D10Device_CreateRenderTargetView(d,(ID3D10Resource*)buffer,&rv,&view);EXPECT(SUCCEEDED(hr)&&view,"fully typed backbuffer cast");if(FAILED(hr)||!view)goto next;
        D3D10_RENDER_TARGET_VIEW_DESC actual;ID3D10RenderTargetView_GetDesc(view,&actual);EXPECT(actual.Format==views[f],"backbuffer cast view identity");
        for(UINT frame=0;frame<2;++frame){
            const float red[4]={1,0,0,1},green[4]={0,1,0,1};ID3D10Device_ClearRenderTargetView(d,view,frame?green:red);
            hr=IDXGISwapChain_Present(swap,0,0);EXPECT(SUCCEEDED(hr),"extended format present");
            MSG msg;while(PeekMessage(&msg,NULL,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessage(&msg);}Sleep(150);
            RECT rect;GetClientRect(window,&rect);POINT middle={(rect.right-rect.left)/2,(rect.bottom-rect.top)/2};ClientToScreen(window,&middle);
            HDC screen=GetDC(NULL);COLORREF got=GetPixel(screen,middle.x,middle.y);ReleaseDC(NULL,screen);
            COLORREF expected=frame?RGB(0,255,0):RGB(255,0,0);
            printf("EXTENDED SCREEN format=%u view=%u fullscreen=%u got=%08lx expected=%08lx\n",formats[f],views[f],fullscreen,(unsigned long)got,(unsigned long)expected);
            EXPECT(got==expected,"extended presentation screen bytes");
        }
    next:if(swap)IDXGISwapChain_SetFullscreenState(swap,FALSE,NULL);RELEASE(view);RELEASE(buffer);RELEASE(swap);
    }
done:RELEASE(factory);RELEASE(adapter);RELEASE(dxgi);DestroyWindow(window);
}

#ifndef D3D10_PRESENT_PROBE
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    HMODULE compiler=LoadLibraryA("d3d10.dll");compileShader=compiler?(CompileFn)(void*)GetProcAddress(compiler,"D3D10CompileShader"):NULL;
    if(!compileShader){puts("FAIL D3D10CompileShader unavailable");return 1;}
    if(argc==5&&!strcmp(argv[1],"--sharing-child"))return sharing_child((BOOL)strtoul(argv[2],NULL,0),(HANDLE)(uintptr_t)strtoull(argv[3],NULL,0),(DXGI_FORMAT)strtoul(argv[4],NULL,0));
    BOOL extendedOnly=FALSE,extendedFullscreen=FALSE;
    int first=0,last=1;for(int arg=1;arg<argc;++arg){if(!strcmp(argv[arg],"--10"))last=0;else if(!strcmp(argv[arg],"--10.1"))first=1;else if(!strcmp(argv[arg],"--shared-peer")&&arg+1<argc)sharingPeer=argv[++arg];else if(!strcmp(argv[arg],"--extended-only"))extendedOnly=TRUE;else if(!strcmp(argv[arg],"--extended-fullscreen"))extendedFullscreen=TRUE;}
    for(int api=first;api<=last;++api){
        ID3D10Device *extended=create_device_flags(TRUE,api?D3D10_FEATURE_LEVEL_10_1:D3D10_FEATURE_LEVEL_10_0,0x20);
        if(extended){extended_formats(extended);extended_present(extended,extendedFullscreen);RELEASE(extended);}
        if(extendedOnly)continue;
        ID3D10Device *firstDevice=create_device(api), *survivor=create_device(api);
        /* Both transports must work while both runtimes live, and the second
         * must remain usable after destruction of the original callback owner. */
        if(firstDevice)resources(firstDevice,api);
        if(survivor)resources(survivor,api);
        if(firstDevice&&survivor)sharing_devices(firstDevice,survivor,api);
        RELEASE(firstDevice);
        if(survivor){resources(survivor,api);rendering(survivor);more_resources(survivor,api);sharing_d3d9(survivor);RELEASE(survivor);}
        ID3D10Device *recreated=create_device(api);
        if(recreated){resources(recreated,api);RELEASE(recreated);}
    }
    printf("D3D10 RUNTIME %s checks=%u failures=%u\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
#endif
