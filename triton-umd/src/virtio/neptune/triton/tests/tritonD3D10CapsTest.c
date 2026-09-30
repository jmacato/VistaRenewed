/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define APIENTRY
#define TRUE 1
#define FALSE 0
#define S_OK 0
#define E_INVALIDARG -1
#define TR_LOG(...) ((void)0)
#define ZeroMemory(p,n) memset(p,0,n)
typedef uint32_t UINT,UINT32;typedef uint64_t UINT64;typedef int HRESULT;typedef void *D3D10DDI_HADAPTER;
enum {D3D11DDICAPS_THREADING,D3D11DDICAPS_SHADER,D3D11DDICAPS_3DPIPELINESUPPORT,D3D11_1DDICAPS_D3D11_OPTIONS,D3D11_1DDICAPS_ARCHITECTURE_INFO,D3D11_1DDICAPS_SHADER_MIN_PRECISION_SUPPORT};
enum {D3D11DDI_3DPIPELINELEVEL_10_0,D3D11DDI_3DPIPELINELEVEL_10_1,D3D11DDI_3DPIPELINELEVEL_11_0,D3D11_1DDI_3DPIPELINELEVEL_11_1};
#define D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(x) (1u<<(x))
#define D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X 1
#define D3D10_0_DDI_SUPPORTED UINT64_C(0x100000000)
#define D3D10_1_DDI_SUPPORTED UINT64_C(0x101000000)
#define D3D10_0_x_vista_DDI_SUPPORTED UINT64_C(0x103000000)
#define D3D10_1_x_vista_DDI_SUPPORTED UINT64_C(0x104000000)
#define D3D11_0_DDI_SUPPORTED UINT64_C(0x110000000)
#define D3D11_1_DDI_SUPPORTED UINT64_C(0x111000000)
typedef struct {UINT Caps;} D3D11DDI_THREADING_CAPS,D3D11DDI_SHADER_CAPS,D3D11DDI_3DPIPELINESUPPORT_CAPS;
typedef struct {UINT OutputMergerLogicOp,AssignDebugBinarySupport;} D3D11_1DDI_D3D11_OPTIONS_DATA;
typedef struct {UINT TileBasedDeferredRenderer;} D3D11_1DDI_ARCHITECTURE_INFO_DATA;
typedef struct {UINT PixelShaderMinPrecision,AllOtherStagesMinPrecision;} D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA;
typedef struct {UINT Type,DataSize;void *pData;} D3D10_2DDIARG_GETCAPS;
#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL caps %d: %s\n",__LINE__,#c);exit(1);}}while(0)
// PRODUCTION_CAPS_IMPLEMENTATION
int main(void){
 UINT data[4]={0x12345678,0xaabbccdd,0xfeedface,0x10101010};
 D3D10_2DDIARG_GETCAPS args={D3D11DDICAPS_THREADING,1,data+1};
 CHECK(tritonGetCaps(NULL,&args)==E_INVALIDARG);
 CHECK(data[0]==0x12345678 && data[1]==0xaabbccdd && data[2]==0xfeedface);
 CHECK(tritonGetCaps(NULL,NULL)==E_INVALIDARG);args.pData=NULL;CHECK(tritonGetCaps(NULL,&args)==E_INVALIDARG);
 args.pData=data+1;args.DataSize=4;CHECK(tritonGetCaps(NULL,&args)==S_OK);CHECK(!data[1] && data[2]==0xfeedface);
 args.Type=D3D11DDICAPS_3DPIPELINESUPPORT;CHECK(tritonGetCaps(NULL,&args)==S_OK);
#ifdef NPT_D3D10_RUNTIME_DDI
 CHECK(data[1]==3);const UINT expected=4;
#else
 CHECK(data[1]==15);const UINT expected=4;
#endif
 UINT count=0;CHECK(tritonGetSupportedVersions(NULL,&count,NULL)==S_OK && count==expected);
 UINT64 versions[6]={0};count=6;CHECK(tritonGetSupportedVersions(NULL,&count,versions)==S_OK && count==expected);
 CHECK(versions[0]==D3D10_0_DDI_SUPPORTED && versions[1]==D3D10_1_DDI_SUPPORTED && versions[expected]==0);
#ifdef NPT_D3D10_RUNTIME_DDI
 CHECK(versions[2]==D3D10_0_x_vista_DDI_SUPPORTED && versions[3]==D3D10_1_x_vista_DDI_SUPPORTED);
#else
 CHECK(versions[2]==D3D11_0_DDI_SUPPORTED && versions[3]==D3D11_1_DDI_SUPPORTED);
#endif
 count=1;versions[1]=0xfeedface;CHECK(tritonGetSupportedVersions(NULL,&count,versions)==S_OK && count==1 && versions[1]==0xfeedface);
 CHECK(tritonGetSupportedVersions(NULL,NULL,NULL)==E_INVALIDARG);
 puts("D3D10/D3D11 adapter caps bounds and versions passed");return 0;
}
