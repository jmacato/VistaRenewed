/* SPDX-License-Identifier: MIT
 * Exercise production descriptor upgrading with guarded legacy allocations.
 * Real WDK table assignments are additionally compiled in both PE builds. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#define APIENTRY
#define VOID void
#define TRUE 1
#define E_INVALIDARG -1
typedef uint32_t UINT;typedef size_t SIZE_T;
typedef struct {void *pDrvPrivate;} Handle;
typedef Handle D3D10DDI_HDEVICE,D3D10DDI_HRESOURCE,D3D10DDI_HRTRESOURCE,D3D10DDI_HSHADERRESOURCEVIEW,D3D10DDI_HRTSHADERRESOURCEVIEW,D3D10DDI_HDEPTHSTENCILVIEW,D3D10DDI_HRTDEPTHSTENCILVIEW,D3D10DDI_HBLENDSTATE,D3D10DDI_HRTBLENDSTATE,D3D10DDI_HRENDERTARGETVIEW,D3D11DDI_HUNORDEREDACCESSVIEW,D3D10DDI_HSHADER,D3D10DDI_HRTSHADER;
typedef void *PTRITON_DEVICE;
typedef struct {UINT a;} TRITON_RESOURCE,TRITON_SRVIEW,TRITON_DSVIEW,TRITON_BLENDSTATE,TRITON_SHADER,D3D10DDI_DEVICEFUNCS,D3D10_1DDI_DEVICEFUNCS,D3D10DDIARG_STAGE_IO_SIGNATURES;
enum {D3D10DDIRESOURCE_BUFFER,D3D10DDIRESOURCE_TEXTURE1D,D3D10DDIRESOURCE_TEXTURE2D,D3D10DDIRESOURCE_TEXTURE3D,D3D10DDIRESOURCE_TEXTURECUBE};
typedef struct {UINT Count,Quality;} Sample;
#define RESOURCE_FIELDS const void *pMipInfoList,*pInitialDataUP;UINT ResourceDimension,Usage,BindFlags,MapFlags,MiscFlags,Format;Sample SampleDesc;UINT MipLevels,ArraySize;const void *pPrimaryDesc
 typedef struct {RESOURCE_FIELDS;} D3D10DDIARG_CREATERESOURCE;
 typedef struct {RESOURCE_FIELDS;UINT ByteStride;} D3D11DDIARG_CREATERESOURCE;
typedef struct {UINT FirstElement,NumElements;} Buffer;
typedef struct {UINT MostDetailedMip,MipLevels,FirstArraySlice,ArraySize;} Tex;
typedef struct {UINT MostDetailedMip,MipLevels;} Cube10;
typedef struct {UINT MostDetailedMip,MipLevels,First2DArrayFace,NumCubes;} Cube;
#define VIEW_COMMON Handle hDrvResource;UINT Format,ResourceDimension
#define VIEWS Buffer Buffer;Tex Tex1D,Tex2D,Tex3D
 typedef struct {VIEW_COMMON;union {VIEWS;Cube10 TexCube;};} D3D10DDIARG_CREATESHADERRESOURCEVIEW;
 typedef struct {VIEW_COMMON;union {VIEWS;Cube TexCube;};} D3D10_1DDIARG_CREATESHADERRESOURCEVIEW,D3D11DDIARG_CREATESHADERRESOURCEVIEW;
 typedef struct {VIEW_COMMON;union {Tex Tex1D,Tex2D,TexCube;};} D3D10DDIARG_CREATEDEPTHSTENCILVIEW;
 typedef struct {VIEW_COMMON;UINT Flags;union {Tex Tex1D,Tex2D,TexCube;};} D3D11DDIARG_CREATEDEPTHSTENCILVIEW;
#define FACTORS UINT SrcBlend,DestBlend,BlendOp,SrcBlendAlpha,DestBlendAlpha,BlendOpAlpha
 typedef struct {UINT AlphaToCoverageEnable,BlendEnable[8];FACTORS;UINT RenderTargetWriteMask[8];} D3D10_DDI_BLEND_DESC;
 typedef struct {UINT BlendEnable;FACTORS;UINT RenderTargetWriteMask;} BlendRT;
 typedef struct {UINT AlphaToCoverageEnable,IndependentBlendEnable;BlendRT RenderTarget[8];} D3D10_1_DDI_BLEND_DESC;
typedef struct {UINT OutputSlot,RegisterIndex,RegisterMask;} D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY;
typedef struct {UINT Stream,OutputSlot,RegisterIndex,RegisterMask;} D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY;
typedef struct {const UINT *pShaderCode;const D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *pOutputStreamDecl;UINT NumEntries,StreamOutputStrideInBytes;} D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT;
typedef struct {const UINT *pShaderCode;const D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *pOutputStreamDecl;UINT NumEntries;const UINT *BufferStridesInBytes;UINT NumStrides,RasterizedStream;} D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT;
static int error;static UINT calls;
static void tritonSetError(PTRITON_DEVICE p,int e){(void)p;error=e;}
static D3D11DDIARG_CREATERESOURCE resource;
static D3D11DDIARG_CREATESHADERRESOURCEVIEW srv;
static D3D11DDIARG_CREATEDEPTHSTENCILVIEW dsv;
static D3D10_1_DDI_BLEND_DESC blend;
static D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT so;
static D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY entries[128];static UINT strides[4],rtCount,rtClear;
// PRODUCTION_ADAPTER_IMPLEMENTATION
void tritonCreateResource(Handle d,const D3D11DDIARG_CREATERESOURCE *a,Handle h,Handle rt){(void)d;(void)h;(void)rt;resource=*a;++calls;}
void tritonCreateSRV(Handle d,const D3D11DDIARG_CREATESHADERRESOURCEVIEW *a,Handle h,Handle rt){(void)d;(void)h;(void)rt;srv=*a;++calls;}
void tritonCreateDSV(Handle d,const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *a,Handle h,Handle rt){(void)d;(void)h;(void)rt;dsv=*a;++calls;}
void tritonCreateBlendState_10(Handle d,const D3D10_1_DDI_BLEND_DESC *a,Handle h,Handle rt){(void)d;(void)h;(void)rt;blend=*a;++calls;}
void tritonCreateGSWithSO_11(Handle d,const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *a,Handle h,Handle rt,const void *sig){(void)d;(void)h;(void)rt;(void)sig;so=*a;memcpy(entries,a->pOutputStreamDecl,a->NumEntries*sizeof(*entries));memcpy(strides,a->BufferStridesInBytes,a->NumStrides*4);++calls;}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL adapter %d: %s\n",__LINE__,#c);exit(1);}}while(0)
void tritonSetRenderTargets(Handle d,const Handle *v,UINT n,UINT clear,Handle dep,const Handle *u,const UINT *counts,UINT a,UINT b,UINT c,UINT e){(void)d;(void)v;(void)dep;rtCount=n;rtClear=clear;CHECK(!u && !counts && !a && !b && !c && !e);}
int main(void){
 Handle h={0};D3D10DDIARG_CREATERESOURCE *r=malloc(sizeof(*r));memset(r,0xa5,sizeof(*r));create_resource(h,r,h,h);
 CHECK(!memcmp(&resource,r,sizeof(*r)) && resource.ByteStride==0);free(r);
 D3D10DDIARG_CREATESHADERRESOURCEVIEW *v=calloc(1,sizeof(*v));v->ResourceDimension=D3D10DDIRESOURCE_TEXTURECUBE;v->TexCube.MipLevels=4;v->TexCube.MostDetailedMip=2;
 create_srv(h,v,h,h);CHECK(srv.TexCube.NumCubes==1 && !srv.TexCube.First2DArrayFace && srv.TexCube.MipLevels==4 && srv.TexCube.MostDetailedMip==2);
 v->ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;v->Tex2D=(Tex){1,3,7,2};create_srv(h,v,h,h);CHECK(!memcmp(&srv.Tex2D,&v->Tex2D,sizeof(Tex)));
 UINT before=calls;v->ResourceDimension=999;create_srv(h,v,h,h);CHECK(error==E_INVALIDARG && calls==before);free(v);
 D3D10_1DDIARG_CREATESHADERRESOURCEVIEW v1={0};v1.ResourceDimension=D3D10DDIRESOURCE_TEXTURECUBE;v1.TexCube=(Cube){1,3,6,2};create_srv_1(h,&v1,h,h);CHECK(!memcmp(&srv.TexCube,&v1.TexCube,sizeof(Cube)));
 D3D10DDIARG_CREATEDEPTHSTENCILVIEW depth={0};depth.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;depth.Tex2D=(Tex){3,4,5,6};create_dsv(h,&depth,h,h);CHECK(!dsv.Flags && !memcmp(&dsv.Tex2D,&depth.Tex2D,sizeof(Tex)));
 D3D10_DDI_BLEND_DESC b={0};b.SrcBlend=2;b.DestBlend=3;b.BlendOp=4;b.SrcBlendAlpha=5;b.DestBlendAlpha=6;b.BlendOpAlpha=7;
 for(UINT i=0;i<8;++i){b.BlendEnable[i]=i&1;b.RenderTargetWriteMask[i]=i;}create_blend(h,&b,h,h);CHECK(blend.IndependentBlendEnable);
 for(UINT i=0;i<8;++i)CHECK(blend.RenderTarget[i].BlendEnable==(i&1) && blend.RenderTarget[i].RenderTargetWriteMask==i && blend.RenderTarget[i].SrcBlend==2 && blend.RenderTarget[i].BlendOpAlpha==7);
 D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY decl[]={{0,1,15},{3,2,3}};D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT a={NULL,decl,2,32};create_so(h,&a,h,h,NULL);
 CHECK(so.NumStrides==4 && strides[0]==16 && strides[3]==8 && !strides[1] && entries[1].OutputSlot==3 && !entries[1].Stream);
 a.NumEntries=1;create_so(h,&a,h,h,NULL);CHECK(so.NumStrides==1 && strides[0]==32);
 decl[0].OutputSlot=4;before=calls;error=0;create_so(h,&a,h,h,NULL);CHECK(calls==before && error==E_INVALIDARG);
 set_targets(h,NULL,3,5,h);CHECK(rtCount==3 && rtClear==5);
 puts("D3D10 descriptor adapters passed");return 0;
}
