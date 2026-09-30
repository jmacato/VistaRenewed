/* SPDX-License-Identifier: MIT
 * Production DDI query functions run against a deterministic host. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define APIENTRY
#define VOID void
typedef uint32_t UINT;
typedef size_t SIZE_T;
typedef int32_t HRESULT;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define S_OK 0
#define S_FALSE 1
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_OUTOFMEMORY ((HRESULT)0x8007000e)
#define DXGI_DDI_ERR_WASSTILLDRAWING ((HRESULT)0x887a000a)
#define FAILED(x) ((HRESULT)(x)<0)
#define TR_LOG(...) ((void)0)
#define D3D10DDI_QUERY_MISCFLAG_PREDICATEHINT 1
#define D3D11_QUERY_MISC_PREDICATEHINT 1
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL query %d: %s\n",__LINE__,#c); exit(1); } } while(0)
typedef enum {D3D10DDI_QUERY_EVENT, D3D10DDI_QUERY_OCCLUSION, D3D10DDI_QUERY_TIMESTAMP,
 D3D10DDI_QUERY_TIMESTAMPDISJOINT,D3D10DDI_QUERY_PIPELINESTATS,D3D10DDI_QUERY_OCCLUSIONPREDICATE,
 D3D10DDI_QUERY_STREAMOUTPUTSTATS,D3D10DDI_QUERY_STREAMOVERFLOWPREDICATE,
 D3D11DDI_QUERY_PIPELINESTATS,D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM0,
 D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM1,D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM2,
 D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM3,D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM0,
 D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM1,D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM2,
 D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM3} D3D10DDI_QUERY;
typedef enum {D3D11_QUERY_EVENT,D3D11_QUERY_OCCLUSION,D3D11_QUERY_TIMESTAMP,
 D3D11_QUERY_TIMESTAMP_DISJOINT,D3D11_QUERY_PIPELINE_STATISTICS,D3D11_QUERY_OCCLUSION_PREDICATE,
 D3D11_QUERY_SO_STATISTICS_STREAM0,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0,
 D3D11_QUERY_SO_STATISTICS_STREAM1,D3D11_QUERY_SO_STATISTICS_STREAM2,D3D11_QUERY_SO_STATISTICS_STREAM3,
 D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2,
 D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3} D3D11_QUERY;
#define D3D11_QUERY_SO_OVERFLOW_PREDICATE D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0
typedef struct {void *pDrvPrivate;} D3D10DDI_HDEVICE, D3D10DDI_HQUERY, D3D10DDI_HRTQUERY;
typedef struct {D3D10DDI_QUERY Query; UINT MiscFlags;} D3D10DDIARG_CREATEQUERY;
typedef struct {D3D11_QUERY Query; UINT MiscFlags;} D3D11_QUERY_DESC;
typedef struct {uint64_t values[8];} D3D10_DDI_QUERY_DATA_PIPELINE_STATISTICS;
typedef struct {uint64_t values[11];} D3D11_QUERY_DATA_PIPELINE_STATISTICS;
typedef struct {int predicate;} ID3D11Query;
typedef ID3D11Query ID3D11Predicate, ID3D11Asynchronous;
typedef struct {ID3D11Query *pQuery; D3D10DDI_QUERY type;} TRITON_QUERY, *PTRITON_QUERY;
typedef struct {void *pDev1,*pCtx1;} TRITON_DEVICE, *PTRITON_DEVICE;
static HRESULT error, createResult, dataResult;
static UINT hostSize,hostFlags,starts,ends,live,createdPredicates;
static BOOL boundValue;
static ID3D11Predicate *boundPredicate;
static void tritonSetError(PTRITON_DEVICE d,HRESULT e) {(void)d;error=e;}
static HRESULT create(const D3D11_QUERY_DESC *d,ID3D11Query **q,BOOL pred) {
 (void)d; if(FAILED(createResult))return createResult;
 *q=calloc(1,sizeof(**q)); CHECK(*q); (*q)->predicate=pred; ++live;
 if(pred)++createdPredicates; return S_OK;
}
#define ID3D11Device1_CreatePredicate(d,p,q) create(p,q,TRUE)
#define ID3D11Device1_CreateQuery(d,p,q) create(p,q,FALSE)
static void ID3D11Query_Release(ID3D11Query *q) {--live;free(q);}
#define ID3D11DeviceContext1_Begin(d,q) (++starts)
#define ID3D11DeviceContext1_End(d,q) (++ends)
static HRESULT get_data(ID3D11Asynchronous *q,void *data,UINT bytes,UINT flags) {
 (void)q;hostSize=bytes;hostFlags=flags;
 if(dataResult!=S_OK)return dataResult;
 if(data && bytes==88) {for(UINT i=0;i<11;++i)((uint64_t*)data)[i]=0x12340000+i;}
 return S_OK;
}
#define ID3D11DeviceContext1_GetData(d,q,p,n,f) get_data(q,p,n,f)
static void predication(ID3D11Predicate *q,BOOL value) {boundPredicate=q;boundValue=value;}
#define ID3D11DeviceContext1_SetPredication(d,q,v) predication(q,v)
// PRODUCTION_QUERY_IMPLEMENTATION
int main(void) {
 TRITON_DEVICE dev={0}; TRITON_QUERY query={0};
 D3D10DDI_HDEVICE d={&dev};D3D10DDI_HQUERY q={&query}; D3D10DDI_HRTQUERY rt={0};
 D3D10DDIARG_CREATEQUERY args={D3D10DDI_QUERY_OCCLUSIONPREDICATE,0};
 tritonCreateQuery(d,&args,q,rt); CHECK(query.pQuery && query.pQuery->predicate); CHECK(createdPredicates==1);
 tritonSetPredication(d,q,TRUE);CHECK(boundPredicate==query.pQuery && boundValue);
 D3D10DDI_HQUERY nullq={0};tritonSetPredication(d,nullq,FALSE);CHECK(!boundPredicate);
 tritonQueryBegin(d,q);tritonQueryEnd(d,q);CHECK(starts==1 && ends==1);
 tritonDestroyQuery(d,q);CHECK(live==0 && !query.pQuery);tritonDestroyQuery(d,q);CHECK(live==0);
 args.Query=D3D10DDI_QUERY_EVENT;createResult=E_OUTOFMEMORY;error=0;
 tritonCreateQuery(d,&args,q,rt);CHECK(error==E_OUTOFMEMORY && !query.pQuery && !live);
 createResult=S_OK;args.Query=(D3D10DDI_QUERY)999;error=0;
 tritonCreateQuery(d,&args,q,rt);CHECK(error==E_INVALIDARG && !live);
 args.Query=D3D10DDI_QUERY_PIPELINESTATS;tritonCreateQuery(d,&args,q,rt);CHECK(live==1);
 struct {uint64_t before,values[8],after;} result;memset(&result,0xab,sizeof(result));
 dataResult=S_FALSE;error=0;tritonQueryGetData(d,q,result.values,64,1);
 CHECK(error==DXGI_DDI_ERR_WASSTILLDRAWING && hostSize==88 && hostFlags==1);
 for(UINT i=0;i<10;++i)CHECK(((uint64_t*)&result)[i]==UINT64_C(0xabababababababab));
 dataResult=S_OK;error=0;tritonQueryGetData(d,q,result.values,64,0);
 CHECK(error==0 && hostSize==88);for(UINT i=0;i<8;++i)CHECK(result.values[i]==0x12340000+i);
 CHECK(result.before==UINT64_C(0xabababababababab) && result.after==result.before);
 error=0;tritonQueryGetData(d,q,result.values,63,0);CHECK(error==E_INVALIDARG);
 error=0;tritonQueryGetData(d,q,NULL,0,1);CHECK(!error && hostSize==0);
 dataResult=E_OUTOFMEMORY;tritonQueryGetData(d,q,NULL,0,0);CHECK(error==E_OUTOFMEMORY);
 tritonDestroyQuery(d,q);error=0;tritonQueryGetData(d,q,NULL,0,0);CHECK(error==E_INVALIDARG && !live);
 puts("D3D10 query behavior passed");return 0;
}
