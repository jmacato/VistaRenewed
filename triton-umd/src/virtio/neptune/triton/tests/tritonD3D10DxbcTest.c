/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <windows.h>
#define TR_LOG(...) ((void)0)
#define NPT_WA_WIDEN_SCALAR_VS_INPUT_MASK 1
#define NPT_WA_LINEARIZE_NOPERSPECTIVE_PS_INPUT 2
static uint32_t npt_host_workaround_flags(void) {return 0;}
// PRODUCTION_DXBC_IMPLEMENTATION
// PRODUCTION_D3D10_SHADER_HELPERS
#include "tritonBlitShaders.h"
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL DXBC %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static const UINT *chunk(const void *data,UINT code,UINT *size) {
 const UINT *h=data;for(UINT i=0;i<h[7];++i){const UINT *p=(const UINT*)((const BYTE*)data+h[8+i]);if(p[0]==code){*size=p[1];return p+2;}}return NULL;
}
static void validate(const void *data,SIZE_T bytes) {
 const UINT *h=data;UINT length;CHECK(h[6]==bytes);
 const UINT *code=chunk(data,DXBC_BLOB_TYPE_SHDR,&length);CHECK(code && (code[0]&65535)==0x40 && code[1]*4==length);
 for(UINT at=2;at<code[1];){UINT n=(code[at]>>24)&127;CHECK(n && n<=code[1]-at && (code[at]&2047)<=106 && !(code[at]&0x80000000u));at+=n;}
 BYTE digest[16];dxbcHash((const BYTE*)data+20,bytes-20,digest);CHECK(!memcmp(digest,(const BYTE*)data+4,16));
}
int main(void) {
 struct {UINT value,reg;BYTE mask,pad[3];} sigs[]={{1,0,15,{0}},{0,2,3,{0}},{0,2,12,{0}}};
 SIZE_T bytes=0;void *alias=tritonD3D10BuildSOAlias(sigs,3,12,&bytes);CHECK(alias);validate(alias,bytes);
 UINT length;const UINT *out=chunk(alias,DXBC_BLOB_TYPE_OSGN,&length);CHECK(out && out[0]==3);free(alias);
 sigs[0].reg=32;CHECK(!tritonD3D10BuildSOAlias(sigs,3,12,&bytes) && !bytes);
 CHECK(!tritonD3D10BuildSOAlias(NULL,0,12,&bytes));
 const BYTE *shaders[]={g_tritonBlitVS,g_tritonBlitPS};SIZE_T sizes[]={sizeof(g_tritonBlitVS),sizeof(g_tritonBlitPS)};
 for(UINT i=0;i<2;++i){void *dxbc=tritonD3D10BlitBytecode(shaders[i],sizes[i],&bytes);CHECK(dxbc);validate(dxbc,bytes);
 const UINT *in=chunk(dxbc,DXBC_BLOB_TYPE_ISGN,&length);out=chunk(dxbc,DXBC_BLOB_TYPE_OSGN,&length);
 CHECK(in && out && in[0]>=1 && out[0]>=1);
 if(!i){const UINT *code=chunk(dxbc,DXBC_BLOB_TYPE_SHDR,&length);UINT shift=0,and=0;
 for(UINT at=2;at<code[1];at+=(code[at]>>24)&127){shift+=(code[at]&2047)==41;and+=(code[at]&2047)==1;}
 CHECK(shift==1 && and==2);CHECK(in[2+2]==6);}
 free(dxbc);CHECK(!tritonD3D10BlitBytecode(shaders[i],31,&bytes));}
 puts("D3D10 SO/SM4 shader behavior passed");return 0;
}
