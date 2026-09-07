/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Top-level function dispatch overrides.  Mandatory because top-
 * level functions have no `_self` for the default dispatcher to bind
 * against.  Each override resolves the function pointer from the
 * loaded host D3D library and forwards the decoded args.
 */

#include "npt_context.h"
#include "npt_library.h"
#include "npt_overrides.h"
#include "npt_renderer.h"

#include "neptune-protocol/npt_protocol_host_toplevel.h"

/* ================================================================== */
/* DXGI factory creation                                                */
/* ================================================================== */

/* The generated dispatcher emits object-table registration for the
 * returned ppFactory / ppDevice automatically; no manual register
 * call is needed here. */

static HRESULT
npt_override_CreateDXGIFactory(UNUSED struct npt_dispatch_context *dctx,
                               struct npt_command_CreateDXGIFactory *args)
{
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_CreateDXGIFactory1) {
      args->ret = NPT_E_FAIL;
      return args->ret;
   }

   /* CreateDXGIFactory and CreateDXGIFactory1 both return
    * IDXGIFactory*; collapse onto CreateDXGIFactory1 for a single
    * entry point. */
   args->ret = lib->pfn_CreateDXGIFactory1(args->riid, args->ppFactory);
   return args->ret;
}

static HRESULT
npt_override_CreateDXGIFactory1(UNUSED struct npt_dispatch_context *dctx,
                                struct npt_command_CreateDXGIFactory1 *args)
{
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_CreateDXGIFactory1) {
      args->ret = NPT_E_FAIL;
      return args->ret;
   }
   args->ret = lib->pfn_CreateDXGIFactory1(args->riid, args->ppFactory);
   return args->ret;
}

static HRESULT
npt_override_CreateDXGIFactory2(UNUSED struct npt_dispatch_context *dctx,
                                struct npt_command_CreateDXGIFactory2 *args)
{
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_CreateDXGIFactory1) {
      args->ret = NPT_E_FAIL;
      return args->ret;
   }
   /* Flags ignored: CreateDXGIFactory1 has none and we collapse onto it. */
   args->ret = lib->pfn_CreateDXGIFactory1(args->riid, args->ppFactory);
   return args->ret;
}

/* ================================================================== */
/* D3D11 device creation                                                */
/* ================================================================== */

static HRESULT
npt_override_D3D11CreateDevice(UNUSED struct npt_dispatch_context *dctx,
                               struct npt_command_D3D11CreateDevice *args)
{
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_D3D11CreateDevice) {
      args->ret = NPT_E_FAIL;
      return args->ret;
   }

   args->ret = lib->pfn_D3D11CreateDevice(
      args->pAdapter,
      args->DriverType,
      /* Software (HMODULE): a host-side module handle would be
       * meaningless to the guest. */
      0,
      args->Flags,
      args->pFeatureLevels,
      args->FeatureLevels,
      args->SDKVersion,
      args->ppDevice,
      args->pFeatureLevel,
      args->ppImmediateContext);
   return args->ret;
}

static HRESULT
npt_override_D3D11On12CreateDevice(UNUSED struct npt_dispatch_context *dctx,
                                   struct npt_command_D3D11On12CreateDevice *args)
{
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_D3D11On12CreateDevice) {
      args->ret = NPT_E_FAIL;
      return args->ret;
   }
   args->ret = lib->pfn_D3D11On12CreateDevice(
      args->pDevice, args->Flags, args->pFeatureLevels, args->FeatureLevels,
      args->ppCommandQueues, args->NumQueues, args->NodeMask,
      args->ppDevice, args->ppImmediateContext, args->pChosenFeatureLevel);
   return args->ret;
}

/* ================================================================== */
/* D3D12 device creation                                                */
/* ================================================================== */

static HRESULT
npt_override_D3D12CreateDevice(UNUSED struct npt_dispatch_context *dctx,
                               struct npt_command_D3D12CreateDevice *args)
{
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_D3D12CreateDevice) {
      args->ret = NPT_E_FAIL;
      return args->ret;
   }
   args->ret = lib->pfn_D3D12CreateDevice(args->pAdapter,
                                           args->MinimumFeatureLevel,
                                           args->riid,
                                           args->ppDevice);
   return args->ret;
}

static HRESULT
npt_override_D3D12CreateRootSignatureDeserializer(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_D3D12CreateRootSignatureDeserializer *args)
{
   args->ret = NPT_E_NOTIMPL;
   if (args->ppRootSignatureDeserializer)
      *args->ppRootSignatureDeserializer = NULL;
   return args->ret;
}

static HRESULT
npt_override_D3D12CreateVersionedRootSignatureDeserializer(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_D3D12CreateVersionedRootSignatureDeserializer *args)
{
   args->ret = NPT_E_NOTIMPL;
   if (args->ppRootSignatureDeserializer)
      *args->ppRootSignatureDeserializer = NULL;
   return args->ret;
}

static HRESULT
npt_override_D3D12SerializeRootSignature(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_D3D12SerializeRootSignature *args)
{
   args->ret = NPT_E_NOTIMPL;
   if (args->ppBlob) *args->ppBlob = NULL;
   if (args->ppErrorBlob) *args->ppErrorBlob = NULL;
   return args->ret;
}

static HRESULT
npt_override_D3D12SerializeVersionedRootSignature(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_D3D12SerializeVersionedRootSignature *args)
{
   args->ret = NPT_E_NOTIMPL;
   if (args->ppBlob) *args->ppBlob = NULL;
   if (args->ppErrorBlob) *args->ppErrorBlob = NULL;
   return args->ret;
}

static HRESULT
npt_override_DXGIDeclareAdapterRemovalSupport(
   UNUSED struct npt_dispatch_context *dctx,
   struct npt_command_DXGIDeclareAdapterRemovalSupport *args)
{
   args->ret = NPT_S_OK;
   return args->ret;
}

/* ================================================================== */
/* Override table                                                       */
/* ================================================================== */

struct npt_dispatch_toplevel_overrides npt_toplevel_overrides = {
   .CreateDXGIFactory                              = npt_override_CreateDXGIFactory,
   .CreateDXGIFactory1                             = npt_override_CreateDXGIFactory1,
   .CreateDXGIFactory2                             = npt_override_CreateDXGIFactory2,
   .DXGIDeclareAdapterRemovalSupport               = npt_override_DXGIDeclareAdapterRemovalSupport,
   .D3D11CreateDevice                              = npt_override_D3D11CreateDevice,
   .D3D11On12CreateDevice                          = npt_override_D3D11On12CreateDevice,
   .D3D12CreateDevice                              = npt_override_D3D12CreateDevice,
   .D3D12CreateRootSignatureDeserializer           = npt_override_D3D12CreateRootSignatureDeserializer,
   .D3D12CreateVersionedRootSignatureDeserializer  = npt_override_D3D12CreateVersionedRootSignatureDeserializer,
   .D3D12SerializeRootSignature                    = npt_override_D3D12SerializeRootSignature,
   .D3D12SerializeVersionedRootSignature           = npt_override_D3D12SerializeVersionedRootSignature,
};
