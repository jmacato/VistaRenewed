/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Query / Predicate / Counter wrapper metadata and lifecycle.
 */

#include "npt_com.h"
#include "npt_overrides_d3d11_feedback.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_client_id3d11asynchronous.h"
#include "neptune-protocol/npt_protocol_client_id3d11counter.h"
#include "neptune-protocol/npt_protocol_client_id3d11device.h"
#include "neptune-protocol/npt_protocol_client_id3d11predicate.h"
#include "neptune-protocol/npt_protocol_client_id3d11query.h"
#include "neptune-protocol/npt_protocol_defs.h"

/* Zero denotes a query type whose result size is unknown. */
static uint32_t
npt_query_data_size_for_type(D3D11_QUERY type)
{
   switch (type) {
   case D3D11_QUERY_EVENT:
   case D3D11_QUERY_OCCLUSION_PREDICATE:
   case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
   case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
   case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
   case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
   case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3:
      return (uint32_t)sizeof(BOOL);
   case D3D11_QUERY_OCCLUSION:
   case D3D11_QUERY_TIMESTAMP:
      return (uint32_t)sizeof(UINT64);
   case D3D11_QUERY_TIMESTAMP_DISJOINT:
      return (uint32_t)sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT);
   case D3D11_QUERY_PIPELINE_STATISTICS:
      return (uint32_t)sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS);
   case D3D11_QUERY_SO_STATISTICS:
   case D3D11_QUERY_SO_STATISTICS_STREAM0:
   case D3D11_QUERY_SO_STATISTICS_STREAM1:
   case D3D11_QUERY_SO_STATISTICS_STREAM2:
   case D3D11_QUERY_SO_STATISTICS_STREAM3:
      return (uint32_t)sizeof(D3D11_QUERY_DATA_SO_STATISTICS);
   default:
      return 0u;
   }
}

static void query_aux_destroy(void *aux_raw);

static void
query_aux_init(struct npt_com_base *com,
               struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d11_query_aux *aux = com->aux;
   aux->base.com = com;
   aux->base.fb_shmem = NULL;
   aux->base.fb_offset = 0;
   aux->base.registered = false;
   aux->query_data_size = 0;
   atomic_store_explicit(&aux->local_version, 0, memory_order_relaxed);
   com->aux_destroy = query_aux_destroy;
   /* The creation descriptor supplies the result size after aux_init. */
   (void)dev; (void)host_id;
}

static void
query_aux_destroy(void *aux_raw)
{
   struct npt_d3d11_query_aux *aux = aux_raw;
   if (aux->base.registered && aux->base.com && aux->base.com->base.device) {
      struct npt_device *dev = aux->base.com->base.device;
      npt_dispatch_feedback_unregister_query(dev->ring, aux->base.com->base.id);
   }
   if (aux->base.fb_shmem && aux->base.com && aux->base.com->base.device) {
      npt_renderer_shmem_unref(aux->base.com->base.device->renderer,
                               aux->base.fb_shmem);
   }
   free(aux);
}

struct npt_d3d11_query_aux *
npt_d3d11_query_aux_cast(void *async)
{
   if (!async)
      return NULL;
   /* Identity check: family wrappers carry one of these per-tier vtbl
    * pointers (overrides patch slots in place, never swap the storage),
    * so vtbl equality means "this is a query family wrapper". */
   const void **vt = ((struct npt_com_base *)async)->lpVtbl;
   if (vt != (const void **)&npt_id3d11asynchronous_default_vtbl_storage &&
       vt != (const void **)&npt_id3d11query_default_vtbl_storage &&
       vt != (const void **)&npt_id3d11query1_default_vtbl_storage &&
       vt != (const void **)&npt_id3d11predicate_default_vtbl_storage &&
       vt != (const void **)&npt_id3d11counter_default_vtbl_storage)
      return NULL;
   return ((struct npt_com_base *)async)->aux;
}

/* Query results use ordered GetData commands. A shared slot cannot safely
 * distinguish repeated Begin/End cycles without a generation in that same
 * command stream, including queries which are issued with End alone. */
static void
npt_d3d11_query_finalize_create(struct npt_device *dev, void *wrapper,
                                D3D11_QUERY type)
{
   if (!dev || !wrapper)
      return;
   struct npt_com_base *com = wrapper;
   struct npt_d3d11_query_aux *aux = com->aux;
   if (aux)
      aux->query_data_size = npt_query_data_size_for_type(type);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateQuery_override(void *self, const D3D11_QUERY_DESC *pQueryDesc,
                         ID3D11Query **ppQuery)
{
   HRESULT hr = npt_id3d11device_default_CreateQuery(self, pQueryDesc, ppQuery);
   if (NPT_SUCCEEDED(hr) && pQueryDesc && ppQuery && *ppQuery) {
      npt_d3d11_query_finalize_create(npt_com_self_device(self), *ppQuery,
                                      pQueryDesc->Query);
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreatePredicate_override(void *self,
                             const D3D11_QUERY_DESC *pPredicateDesc,
                             ID3D11Predicate **ppPredicate)
{
   HRESULT hr = npt_id3d11device_default_CreatePredicate(
      self, pPredicateDesc, ppPredicate);
   if (NPT_SUCCEEDED(hr) && pPredicateDesc && ppPredicate && *ppPredicate) {
      npt_d3d11_query_finalize_create(npt_com_self_device(self),
                                      *ppPredicate, pPredicateDesc->Query);
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateCounter_override(void *self,
                           const D3D11_COUNTER_DESC *pCounterDesc,
                           ID3D11Counter **ppCounter)
{
   HRESULT hr = npt_id3d11device_default_CreateCounter(
      self, pCounterDesc, ppCounter);
   /* No feedback for Counter: not a D3D11_QUERY type, and Linux
    * D3D11 typically stubs Counter creation with E_NOTIMPL anyway. */
   (void)pCounterDesc;
   (void)ppCounter;
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateQuery1_override(void *self, const D3D11_QUERY_DESC1 *pQueryDesc1,
                           ID3D11Query1 **ppQuery1)
{
   HRESULT hr = npt_id3d11device3_default_CreateQuery1(
      self, pQueryDesc1, ppQuery1);
   if (NPT_SUCCEEDED(hr) && pQueryDesc1 && ppQuery1 && *ppQuery1) {
      /* DESC1::Query is the same D3D11_QUERY enum; ContextType
       * doesn't affect result sizing. */
      npt_d3d11_query_finalize_create(npt_com_self_device(self),
                                      *ppQuery1, pQueryDesc1->Query);
   }
   return hr;
}

/* All tiers derive from ID3D11Asynchronous and share one aux. */
static const GUID *const query_tiers[] = {
   &NPT_IID_ID3D11Asynchronous,
   &NPT_IID_ID3D11Query,
   &NPT_IID_ID3D11Query1,
   &NPT_IID_ID3D11Predicate,
   &NPT_IID_ID3D11Counter,
   NULL,
};

#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE5(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device5, m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE4(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device4, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE5(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device3, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE4(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE2(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device2, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE1(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device1, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE2(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE1(m, f)

void
npt_overrides_d3d11_query_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE(CreateQuery,    dev_CreateQuery_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE(CreatePredicate, dev_CreatePredicate_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE(CreateCounter,  dev_CreateCounter_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(CreateQuery1,  dev3_CreateQuery1_override);

   npt_com_register_family(query_tiers,
                           sizeof(struct npt_d3d11_query_aux),
                           query_aux_init);
}
