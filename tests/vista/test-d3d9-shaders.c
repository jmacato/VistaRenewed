/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 * Public D3D9 runtime tests: do not call private driver entry points. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static IDirect3DDevice9 *device;
static IDirect3DSurface9 *target, *readback;
static unsigned checks, failures;
#define HR(expr) do { HRESULT hr_=(expr); ++checks; if(FAILED(hr_)) { \
    printf("FAIL line %u %s HRESULT=%08lx\n",__LINE__,#expr,(unsigned long)hr_); \
    ++failures; goto done; } } while(0)
static DWORD floatbits(float x) { DWORD r;memcpy(&r,&x,4);return r; }
static void identity(D3DMATRIX *m) { memset(m,0,sizeof(*m));for(unsigned i=0;i<4;++i)m->m[i][i]=1; }
static void checkpixel(const char *name, DWORD expected, int tolerance) {
    D3DLOCKED_RECT map;DWORD actual=0;int good=1;
    HR(IDirect3DDevice9_GetRenderTargetData(device,target,readback));
    HR(IDirect3DSurface9_LockRect(readback,&map,NULL,D3DLOCK_READONLY));
    memcpy(&actual,(BYTE *)map.pBits+16*map.Pitch+16*4,4);
    HR(IDirect3DSurface9_UnlockRect(readback));
    for(unsigned s=0;s<32;s+=8)if(abs((int)((actual>>s)&255)-(int)((expected>>s)&255))>tolerance)good=0;
    ++checks;failures+=!good;
    printf("PIXEL %s actual=%08lx expected=%08lx %s\n",name,(unsigned long)actual,(unsigned long)expected,good?"PASS":"FAIL");
 done:;
}
struct Vertex { float x,y,z,nx,ny,nz;DWORD diffuse,specular;float u,v; };
static struct Vertex vertices[3]={
    {-1,-1,.5f,0,0,-1,0x663399cc,0x991a330d,.25f,.75f},
    {0,1,.5f,0,0,-1,0x663399cc,0x991a330d,.25f,.75f},
    {1,-1,.5f,0,0,-1,0x663399cc,0x991a330d,.25f,.75f}};
static void draw(void) {
    HRESULT drawResult,endResult;
    HR(IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0xff050709,1,0));
    HR(IDirect3DDevice9_BeginScene(device));
    drawResult=IDirect3DDevice9_DrawPrimitiveUP(device,D3DPT_TRIANGLELIST,1,vertices,sizeof(vertices[0]));
    endResult=IDirect3DDevice9_EndScene(device);HR(drawResult);HR(endResult);
 done:;
}
static DWORD pack(const float c[4]) {
    DWORD result=0;const unsigned shifts[4]={16,8,0,24};
    for(unsigned i=0;i<4;++i){float v=c[i]<0?0:c[i]>1?1:c[i];result|=(DWORD)(v*255+.5f)<<shifts[i];}return result;
}
static void operations(void) {
    const float a[4]={.2f,.6f,.8f,.4f},b[4]={128.f/255,77.f/255,.2f,179.f/255},c[4]={85.f/255,.2f,119.f/255,.6f};
    IDirect3DTexture9 *texture=NULL;D3DLOCKED_RECT map;
    HR(IDirect3DDevice9_CreateTexture(device,1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,NULL));
    HR(IDirect3DTexture9_LockRect(texture,0,&map,NULL,0));*(DWORD *)map.pBits=0xccaa551a;HR(IDirect3DTexture9_UnlockRect(texture,0));
    HR(IDirect3DDevice9_SetTexture(device,0,(IDirect3DBaseTexture9 *)texture));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_TEXTUREFACTOR,0xb3804d33));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLORARG0,D3DTA_CONSTANT));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLORARG1,D3DTA_DIFFUSE));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLORARG2,D3DTA_TFACTOR));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_CONSTANT,0x99553377));
    for(unsigned op=2;op<=26;++op) {
        if(op==D3DTOP_BUMPENVMAP || op==D3DTOP_BUMPENVMAPLUMINANCE)continue;
        float out[4]={0,0,0,a[3]};char name[64];
        HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLOROP,op));
        for(unsigned i=0;i<3;++i)switch(op) {
        case D3DTOP_SELECTARG1:case D3DTOP_PREMODULATE:out[i]=a[i];break;
        case D3DTOP_SELECTARG2:out[i]=b[i];break;
        case D3DTOP_MODULATE:out[i]=a[i]*b[i];break;
        case D3DTOP_MODULATE2X:out[i]=2*a[i]*b[i];break;
        case D3DTOP_MODULATE4X:out[i]=4*a[i]*b[i];break;
        case D3DTOP_ADD:out[i]=a[i]+b[i];break;
        case D3DTOP_ADDSIGNED:out[i]=a[i]+b[i]-.5f;break;
        case D3DTOP_ADDSIGNED2X:out[i]=2*(a[i]+b[i]-.5f);break;
        case D3DTOP_SUBTRACT:out[i]=a[i]-b[i];break;
        case D3DTOP_ADDSMOOTH:out[i]=a[i]+b[i]-a[i]*b[i];break;
        case D3DTOP_BLENDDIFFUSEALPHA:case D3DTOP_BLENDCURRENTALPHA:out[i]=a[i]*a[3]+b[i]*(1-a[3]);break;
        case D3DTOP_BLENDTEXTUREALPHA:out[i]=a[i]*.8f+b[i]*.2f;break;
        case D3DTOP_BLENDFACTORALPHA:out[i]=a[i]*b[3]+b[i]*(1-b[3]);break;
        case D3DTOP_BLENDTEXTUREALPHAPM:out[i]=a[i]+b[i]*.2f;break;
        case D3DTOP_MODULATEALPHA_ADDCOLOR:out[i]=a[i]+a[3]*b[i];break;
        case D3DTOP_MODULATECOLOR_ADDALPHA:out[i]=a[i]*b[i]+a[3];break;
        case D3DTOP_MODULATEINVALPHA_ADDCOLOR:out[i]=a[i]+(1-a[3])*b[i];break;
        case D3DTOP_MODULATEINVCOLOR_ADDALPHA:out[i]=(1-a[i])*b[i]+a[3];break;
        case D3DTOP_DOTPRODUCT3:out[i]=4*((a[0]-.5f)*(b[0]-.5f)+(a[1]-.5f)*(b[1]-.5f)+(a[2]-.5f)*(b[2]-.5f));break;
        case D3DTOP_MULTIPLYADD:out[i]=c[i]+a[i]*b[i];break;
        case D3DTOP_LERP:out[i]=c[i]*a[i]+(1-c[i])*b[i];break;
        }
        draw();snprintf(name,sizeof(name),"FFP operation %u",op);checkpixel(name,pack(out),2);
    }
 done:
    IDirect3DDevice9_SetTexture(device,0,NULL);if(texture)IDirect3DTexture9_Release(texture);
}
static void lights(void) {
    D3DMATERIAL9 material;memset(&material,0,sizeof(material));material.Diffuse=(D3DCOLORVALUE){.5f,.4f,.3f,.6f};
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLORARG1,D3DTA_DIFFUSE));
    HR(IDirect3DDevice9_SetMaterial(device,&material));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_COLORVERTEX,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_LIGHTING,TRUE));
    for(unsigned i=0;i<8;++i){D3DLIGHT9 light;memset(&light,0,sizeof(light));light.Type=D3DLIGHT_DIRECTIONAL;light.Direction.z=1;
        light.Diffuse=(D3DCOLORVALUE){.1f,.1f,.1f,1};HR(IDirect3DDevice9_SetLight(device,100+i*17,&light));HR(IDirect3DDevice9_LightEnable(device,100+i*17,TRUE));}
    draw();{float expected[4]={.4f,.32f,.24f,.6f};checkpixel("eight lights arbitrary indices",pack(expected),2);}
    for(unsigned i=0;i<8;++i)HR(IDirect3DDevice9_LightEnable(device,100+i*17,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_LIGHTING,FALSE));
 done:;
}
static void fixed_pipeline(void) {
    D3DMATRIX matrix; IDirect3DTexture9 *texture=NULL; D3DLOCKED_RECT map;
    const float color[4]={.2f,.6f,.8f,.4f};
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_COLORVERTEX,TRUE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_LIGHTING,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_FOGCOLOR,0xff0000ff));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_FOGSTART,floatbits(0)));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_FOGEND,floatbits(1)));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_FOGDENSITY,floatbits(1)));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_FOGENABLE,TRUE));
    for(unsigned table=0;table<2;++table)for(unsigned mode=D3DFOG_EXP;mode<=D3DFOG_LINEAR;++mode) {
        float factor=mode==D3DFOG_LINEAR?.5f:expf(mode==D3DFOG_EXP?-.5f:-.25f), expected[4];char name[64];
        HR(IDirect3DDevice9_SetRenderState(device,D3DRS_FOGVERTEXMODE,table?D3DFOG_NONE:mode));
        HR(IDirect3DDevice9_SetRenderState(device,D3DRS_FOGTABLEMODE,table?mode:D3DFOG_NONE));
        memcpy(expected,color,sizeof(expected));for(unsigned i=0;i<3;++i)expected[i]=color[i]*factor+(i==2?1-factor:0);
        draw();snprintf(name,sizeof(name),"%s fog mode%u",table?"table":"vertex",mode);checkpixel(name,pack(expected),2);
    }
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_FOGENABLE,FALSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_FOGTABLEMODE,D3DFOG_NONE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_FOGVERTEXMODE,D3DFOG_NONE));
    for(unsigned plane=0;plane<6;++plane) {
        float equation[4]={1,0,0,-2};char name[64];
        HR(IDirect3DDevice9_SetClipPlane(device,plane,equation));HR(IDirect3DDevice9_SetRenderState(device,D3DRS_CLIPPLANEENABLE,1u<<plane));
        draw();snprintf(name,sizeof(name),"clip plane%u rejects",plane);checkpixel(name,0xff050709,0);
        equation[3]=2;HR(IDirect3DDevice9_SetClipPlane(device,plane,equation));draw();checkpixel("clip positive control",0x663399cc,1);
    }
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_CLIPPLANEENABLE,0));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHATESTENABLE,TRUE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHAREF,102));
    for(unsigned func=D3DCMP_NEVER;func<=D3DCMP_ALWAYS;++func) {
        int keep=func==D3DCMP_EQUAL||func==D3DCMP_LESSEQUAL||func==D3DCMP_GREATEREQUAL||func==D3DCMP_ALWAYS;
        HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHAFUNC,func));draw();checkpixel("alpha comparison",keep?0x663399cc:0xff050709,1);
    }
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ALPHATESTENABLE,FALSE));
    {D3DVIEWPORT9 viewport={0,0,32,32,.25f,.25f};
        HR(IDirect3DDevice9_SetViewport(device,&viewport));draw();checkpixel("equal viewport depth endpoints",0x663399cc,1);
        viewport.MinZ=0;viewport.MaxZ=1;HR(IDirect3DDevice9_SetViewport(device,&viewport));}
    HR(IDirect3DDevice9_CreateTexture(device,4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,NULL));
    HR(IDirect3DTexture9_LockRect(texture,0,&map,NULL,0));
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)*((DWORD *)((BYTE *)map.pBits+y*map.Pitch)+x)=0xcc00001a|(x*85<<16)|(y*85<<8);
    HR(IDirect3DTexture9_UnlockRect(texture,0));HR(IDirect3DDevice9_SetTexture(device,0,(IDirect3DBaseTexture9 *)texture));
    HR(IDirect3DDevice9_SetSamplerState(device,0,D3DSAMP_MINFILTER,D3DTEXF_POINT));HR(IDirect3DDevice9_SetSamplerState(device,0,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
    HR(IDirect3DDevice9_SetSamplerState(device,0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));HR(IDirect3DDevice9_SetSamplerState(device,0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLORARG1,D3DTA_TEXTURE));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE));
    draw();checkpixel("texture coordinate baseline",0xcc55ff1a,1);
    identity(&matrix);matrix.m[0][0]=2;matrix.m[1][1]=.5f;
    HR(IDirect3DDevice9_SetTransform(device,D3DTS_TEXTURE0,&matrix));HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_COUNT2));
    draw();checkpixel("texture transform COUNT2",0xccaa551a,1);
    identity(&matrix);matrix.m[2][2]=2;HR(IDirect3DDevice9_SetTransform(device,D3DTS_TEXTURE0,&matrix));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_COUNT3|D3DTTFF_PROJECTED));
    draw();checkpixel("projected texture coordinates",0xcc00551a,1);
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_TEXCOORDINDEX,D3DTSS_TCI_CAMERASPACENORMAL));
    draw();checkpixel("camera normal texgen",0xcc00001a,1);
 done:
    IDirect3DDevice9_SetTexture(device,0,NULL);if(texture)IDirect3DTexture9_Release(texture);
    IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_TEXCOORDINDEX,0);
    IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_TEXTURETRANSFORMFLAGS,0);
    IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
}
static void skinning_points(void) {
    struct BlendVertex {float x,y,z,weight;DWORD indices,diffuse;} blend[3];D3DMATRIX matrix;
    for(unsigned i=0;i<3;++i){blend[i]=(struct BlendVertex){vertices[i].x,vertices[i].y,vertices[i].z,.25f,217|(253<<8),0x663399cc};}
    identity(&matrix);matrix.m[3][0]=1.5f;HR(IDirect3DDevice9_SetTransform(device,D3DTS_WORLDMATRIX(217),&matrix));
    matrix.m[3][0]=-.5f;HR(IDirect3DDevice9_SetTransform(device,D3DTS_WORLDMATRIX(253),&matrix));
    HR(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZB2|D3DFVF_LASTBETA_UBYTE4|D3DFVF_DIFFUSE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_VERTEXBLEND,D3DVBF_1WEIGHTS));HR(IDirect3DDevice9_SetRenderState(device,D3DRS_INDEXEDVERTEXBLENDENABLE,TRUE));
    HR(IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0xff050709,1,0));HR(IDirect3DDevice9_BeginScene(device));
    {HRESULT result=IDirect3DDevice9_DrawPrimitiveUP(device,D3DPT_TRIANGLELIST,1,blend,sizeof(blend[0]));HRESULT end=IDirect3DDevice9_EndScene(device);HR(result);HR(end);}
    checkpixel("indexed world palette217/253",0x663399cc,1);
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_VERTEXBLEND,D3DVBF_DISABLE));HR(IDirect3DDevice9_SetRenderState(device,D3DRS_INDEXEDVERTEXBLENDENABLE,FALSE));
    HR(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEX1));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_POINTSIZE,floatbits(8)));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_POINTSIZE_MAX,floatbits(64)));
    {struct Vertex point=vertices[0];point.x=point.y=0;
        HR(IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0xff050709,1,0));HR(IDirect3DDevice9_BeginScene(device));
        HRESULT result=IDirect3DDevice9_DrawPrimitiveUP(device,D3DPT_POINTLIST,1,&point,sizeof(point));HRESULT end=IDirect3DDevice9_EndScene(device);HR(result);HR(end);
        checkpixel("expanded point",0x663399cc,1);}
 done:;
}
static void legacy_bump(void) {
    static const DWORD program[]={0xffff0103,D3DSIO_TEX,0xb00f0000,
        D3DSIO_TEXBEML,0xb00f0001,0xb0e40000,D3DSIO_MOV,0x800f0000,0xb0e40001,0xffff};
    IDirect3DTexture9 *texture=NULL;IDirect3DPixelShader9 *shader=NULL;D3DLOCKED_RECT map;
    HR(IDirect3DDevice9_CreateTexture(device,4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,NULL));
    HR(IDirect3DTexture9_LockRect(texture,0,&map,NULL,0));
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)*((DWORD *)((BYTE *)map.pBits+y*map.Pitch)+x)=0xcc00001a|(x*85<<16)|(y*85<<8);
    HR(IDirect3DTexture9_UnlockRect(texture,0));
    for(unsigned sampler=0;sampler<2;++sampler) {
        HR(IDirect3DDevice9_SetTexture(device,sampler,(IDirect3DBaseTexture9 *)texture));
        HR(IDirect3DDevice9_SetSamplerState(device,sampler,D3DSAMP_MINFILTER,D3DTEXF_POINT));
        HR(IDirect3DDevice9_SetSamplerState(device,sampler,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
        HR(IDirect3DDevice9_SetSamplerState(device,sampler,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
        HR(IDirect3DDevice9_SetSamplerState(device,sampler,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
        HR(IDirect3DDevice9_SetTextureStageState(device,sampler,D3DTSS_TEXCOORDINDEX,0));
    }
    HR(IDirect3DDevice9_SetTextureStageState(device,1,D3DTSS_BUMPENVMAT00,floatbits(.75f)));
    HR(IDirect3DDevice9_SetTextureStageState(device,1,D3DTSS_BUMPENVMAT01,floatbits(0)));
    HR(IDirect3DDevice9_SetTextureStageState(device,1,D3DTSS_BUMPENVMAT10,floatbits(0)));
    HR(IDirect3DDevice9_SetTextureStageState(device,1,D3DTSS_BUMPENVMAT11,floatbits(0)));
    HR(IDirect3DDevice9_SetTextureStageState(device,1,D3DTSS_BUMPENVLSCALE,floatbits(2)));
    HR(IDirect3DDevice9_SetTextureStageState(device,1,D3DTSS_BUMPENVLOFFSET,floatbits(.1f)));
    HR(IDirect3DDevice9_CreatePixelShader(device,program,&shader));HR(IDirect3DDevice9_SetPixelShader(device,shader));
    float luminance=2*(26.f/255)+.1f;float expected[4]={2.f/3*luminance,luminance,26.f/255*luminance,.8f*luminance};
    draw();checkpixel("PS1.3 bump source luminance",pack(expected),2);
    HR(IDirect3DDevice9_SetTextureStageState(device,1,D3DTSS_BUMPENVMAT00,floatbits(0)));
    HR(IDirect3DDevice9_SetTextureStageState(device,1,D3DTSS_BUMPENVMAT01,floatbits(-.6f)));
    HR(IDirect3DDevice9_SetTextureStageState(device,1,D3DTSS_BUMPENVMAT10,floatbits(.3f)));
    expected[1]=2.f/3*luminance;draw();checkpixel("PS1.3 off-diagonal bump matrix",pack(expected),2);
 done:
    IDirect3DDevice9_SetPixelShader(device,NULL);IDirect3DDevice9_SetTexture(device,0,NULL);IDirect3DDevice9_SetTexture(device,1,NULL);
    IDirect3DDevice9_SetTextureStageState(device,1,D3DTSS_TEXCOORDINDEX,1);
    if(shader)IDirect3DPixelShader9_Release(shader);if(texture)IDirect3DTexture9_Release(texture);
}
static void multipass_alias(void) {
    /* Keep both logical bindings while the texture is used as a render target.
     * The first pass ignores texture data; the later PS and VS passes sample
     * without another SetTexture call. */
    static const DWORD vertexTexture[]={0xfffe0300,
        0x0200001f,0x80000000,0x900f0000,
        0x0200001f,0x80000000,0xe00f0000,
        0x0200001f,0x8000000a,0xe00f0001,
        0x0200001f,0x90000000,0xa00f0800,
        0x05000051,0xa00f0000,0x3e800000,0x3f400000,0,0,
        0x02000001,0xe00f0000,0x90e40000,
        0x0300005f,0xe00f0001,0xa0e40000,0xa0e40800,0xffff};
    static const DWORD pixelColor[]={0xffff0300,
        0x0200001f,0x8000000a,0x900f0000,
        0x02000001,0x800f0800,0x90e40000,0xffff};
    D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
    IDirect3DTexture9 *texture=NULL;IDirect3DSurface9 *surface=NULL,*saved=target;
    IDirect3DBaseTexture9 *logical=NULL;IDirect3DVertexShader9 *vs=NULL;IDirect3DPixelShader9 *ps=NULL;
    IDirect3DVertexDeclaration9 *declaration=NULL;
    HR(IDirect3DDevice9_CreateTexture(device,32,32,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&texture,NULL));
    HR(IDirect3DTexture9_GetSurfaceLevel(texture,0,&surface));
    HR(IDirect3DDevice9_SetTexture(device,0,(IDirect3DBaseTexture9 *)texture));
    HR(IDirect3DDevice9_SetTexture(device,D3DVERTEXTEXTURESAMPLER0,(IDirect3DBaseTexture9 *)texture));
    HR(IDirect3DDevice9_SetRenderTarget(device,0,surface));target=surface;
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLORARG1,D3DTA_DIFFUSE));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE));
    draw();checkpixel("unused PS/VS texture aliases writable RT",0x663399cc,1);
    HR(IDirect3DDevice9_GetTexture(device,0,&logical));++checks;
    if(logical!=(IDirect3DBaseTexture9 *)texture){++failures;puts("FAIL PS logical alias binding lost");}
    if(logical){IDirect3DBaseTexture9_Release(logical);logical=NULL;}
    HR(IDirect3DDevice9_GetTexture(device,D3DVERTEXTEXTURESAMPLER0,&logical));++checks;
    if(logical!=(IDirect3DBaseTexture9 *)texture){++failures;puts("FAIL VS logical alias binding lost");}
    if(logical){IDirect3DBaseTexture9_Release(logical);logical=NULL;}
    HR(IDirect3DDevice9_SetRenderTarget(device,0,saved));target=saved;
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLORARG1,D3DTA_TEXTURE));
    HR(IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE));
    draw();checkpixel("PS texture restored after target switch",0x663399cc,1);
    HR(IDirect3DDevice9_CreateVertexDeclaration(device,elements,&declaration));
    HR(IDirect3DDevice9_CreateVertexShader(device,vertexTexture,&vs));
    HR(IDirect3DDevice9_CreatePixelShader(device,pixelColor,&ps));
    HR(IDirect3DDevice9_SetVertexDeclaration(device,declaration));
    HR(IDirect3DDevice9_SetVertexShader(device,vs));HR(IDirect3DDevice9_SetPixelShader(device,ps));
    HR(IDirect3DDevice9_SetSamplerState(device,D3DVERTEXTEXTURESAMPLER0,D3DSAMP_MINFILTER,D3DTEXF_POINT));
    HR(IDirect3DDevice9_SetSamplerState(device,D3DVERTEXTEXTURESAMPLER0,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
    draw();checkpixel("VS texture restored after target switch",0x663399cc,1);
 done:
    target=saved;IDirect3DDevice9_SetRenderTarget(device,0,saved);
    IDirect3DDevice9_SetVertexShader(device,NULL);IDirect3DDevice9_SetPixelShader(device,NULL);
    IDirect3DDevice9_SetTexture(device,0,NULL);IDirect3DDevice9_SetTexture(device,D3DVERTEXTEXTURESAMPLER0,NULL);
    IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
    IDirect3DDevice9_SetFVF(device,D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEX1);
    if(logical)IDirect3DBaseTexture9_Release(logical);if(declaration)IDirect3DVertexDeclaration9_Release(declaration);
    if(vs)IDirect3DVertexShader9_Release(vs);if(ps)IDirect3DPixelShader9_Release(ps);
    if(surface)IDirect3DSurface9_Release(surface);if(texture)IDirect3DTexture9_Release(texture);
}
static void profiles(void) {
    const DWORD vs11[]={0xfffe0101,1,0xc00f0000,0x90e40000,1,0xd00f0000,0x90e40001,0xffff};
    const DWORD vs20[]={0xfffe0200,0x0200001f,0x80000000,0x900f0000,0x0200001f,0x8000000a,0x900f0001,0x02000001,0xc00f0000,0x90e40000,0x02000001,0xd00f0000,0x90e40001,0xffff};
    const DWORD vs30[]={0xfffe0300,0x0200001f,0x80000000,0x900f0000,0x0200001f,0x8000000a,0x900f0001,0x0200001f,0x80000000,0xe00f0000,0x0200001f,0x8000000a,0xe00f0001,0x02000001,0xe00f0000,0x90e40000,0x02000001,0xe00f0001,0x90e40001,0xffff};
    D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,24,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_COLOR,0},D3DDECL_END()};
    IDirect3DVertexDeclaration9 *declaration=NULL;IDirect3DVertexShader9 *vs=NULL;IDirect3DPixelShader9 *ps=NULL;
    HR(IDirect3DDevice9_CreateVertexDeclaration(device,elements,&declaration));HR(IDirect3DDevice9_SetVertexDeclaration(device,declaration));
    for(unsigned version=0;version<3;++version) {
        HR(IDirect3DDevice9_CreateVertexShader(device,version==0?vs11:version==1?vs20:vs30,&vs));HR(IDirect3DDevice9_SetVertexShader(device,vs));
        for(unsigned profile=1;profile<=6;++profile) {
            if ((version==2) != (profile==6)) continue;
            DWORD shader[16];unsigned n=0;
            shader[n++]=profile<=4?D3DPS_VERSION(1,profile):profile==5?D3DPS_VERSION(2,0):D3DPS_VERSION(3,0);
            if(profile>=5){shader[n++]=0x0200001f;shader[n++]=profile==6?0x8000000a:0x80000000;shader[n++]=0x900f0000;}
            shader[n++]=profile<=4?D3DSIO_MOV:0x02000001;shader[n++]=profile<=4?0x800f0000:0x800f0800;shader[n++]=0x90e40000;shader[n++]=0xffff;
            HR(IDirect3DDevice9_CreatePixelShader(device,shader,&ps));HR(IDirect3DDevice9_SetPixelShader(device,ps));draw();
            char name[64];snprintf(name,sizeof(name),"VS%u PS profile%u",version+1,profile);checkpixel(name,0x663399cc,1);
            HR(IDirect3DDevice9_SetPixelShader(device,NULL));IDirect3DPixelShader9_Release(ps);ps=NULL;
        }
        HR(IDirect3DDevice9_SetVertexShader(device,NULL));IDirect3DVertexShader9_Release(vs);vs=NULL;
    }
 done:
    IDirect3DDevice9_SetVertexShader(device,NULL);IDirect3DDevice9_SetPixelShader(device,NULL);IDirect3DDevice9_SetVertexDeclaration(device,NULL);
    if(vs)IDirect3DVertexShader9_Release(vs);if(ps)IDirect3DPixelShader9_Release(ps);if(declaration)IDirect3DVertexDeclaration9_Release(declaration);
}
int main(void) {
    HWND window=CreateWindowA("STATIC","D3D9 shaders",WS_OVERLAPPEDWINDOW,0,0,64,64,NULL,NULL,GetModuleHandle(NULL),NULL);
    IDirect3D9 *api=Direct3DCreate9(D3D_SDK_VERSION);D3DPRESENT_PARAMETERS present={0};D3DMATRIX matrix;
    if(!api || !window){++failures;goto done;}
    present.Windowed=TRUE;present.hDeviceWindow=window;present.SwapEffect=D3DSWAPEFFECT_DISCARD;present.BackBufferWidth=present.BackBufferHeight=32;present.BackBufferFormat=D3DFMT_X8R8G8B8;
    HR(IDirect3D9_CreateDevice(api,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&present,&device));
    ++checks;if(!GetModuleHandleA("neptune_d3d9.dll")){++failures;printf("FAIL wrong UMD\n");goto done;}
    HR(IDirect3DDevice9_CreateRenderTarget(device,32,32,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,NULL));
    HR(IDirect3DDevice9_CreateOffscreenPlainSurface(device,32,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,NULL));
    HR(IDirect3DDevice9_SetRenderTarget(device,0,target));HR(IDirect3DDevice9_SetDepthStencilSurface(device,NULL));
    HR(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_SPECULAR|D3DFVF_TEX1));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,FALSE));HR(IDirect3DDevice9_SetRenderState(device,D3DRS_CULLMODE,D3DCULL_NONE));
    HR(IDirect3DDevice9_SetRenderState(device,D3DRS_LIGHTING,FALSE));identity(&matrix);
    HR(IDirect3DDevice9_SetTransform(device,D3DTS_WORLD,&matrix));HR(IDirect3DDevice9_SetTransform(device,D3DTS_VIEW,&matrix));HR(IDirect3DDevice9_SetTransform(device,D3DTS_PROJECTION,&matrix));
    operations();lights();fixed_pipeline();skinning_points();legacy_bump();multipass_alias();profiles();
 done:
    if(device){IDirect3DDevice9_SetTexture(device,0,NULL);IDirect3DDevice9_SetRenderTarget(device,0,NULL);}
    if(readback)IDirect3DSurface9_Release(readback);if(target)IDirect3DSurface9_Release(target);if(device)IDirect3DDevice9_Release(device);if(api)IDirect3D9_Release(api);if(window)DestroyWindow(window);
    printf("D3D9-SHADERS checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
