/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D10 uses smaller descriptors and function tables than D3D11. Upgrade
 * values explicitly, then share the actual rendering implementation. */
#include "tritonD3D10.h"
#include "tritonDxbc.h"

extern void APIENTRY tritonCreateResource(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATERESOURCE *, D3D10DDI_HRESOURCE, D3D10DDI_HRTRESOURCE);
extern void APIENTRY tritonCreateSRV(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *, D3D10DDI_HSHADERRESOURCEVIEW, D3D10DDI_HRTSHADERRESOURCEVIEW);
extern void APIENTRY tritonCreateDSV(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *, D3D10DDI_HDEPTHSTENCILVIEW, D3D10DDI_HRTDEPTHSTENCILVIEW);
extern void APIENTRY tritonCreateBlendState_10(D3D10DDI_HDEVICE, const D3D10_1_DDI_BLEND_DESC *, D3D10DDI_HBLENDSTATE, D3D10DDI_HRTBLENDSTATE);
extern void APIENTRY tritonSetRenderTargets(D3D10DDI_HDEVICE, const D3D10DDI_HRENDERTARGETVIEW *, UINT, UINT, D3D10DDI_HDEPTHSTENCILVIEW, const D3D11DDI_HUNORDEREDACCESSVIEW *, const UINT *, UINT, UINT, UINT, UINT);
extern void APIENTRY tritonCreateGSWithSO_11(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);

static SIZE_T APIENTRY resource_size(D3D10DDI_HDEVICE device, const D3D10DDIARG_CREATERESOURCE *args)
{
    (void)device; (void)args;
    return sizeof(TRITON_RESOURCE);
}
static void APIENTRY create_resource(D3D10DDI_HDEVICE device,
    const D3D10DDIARG_CREATERESOURCE *args, D3D10DDI_HRESOURCE resource,
    D3D10DDI_HRTRESOURCE runtime)
{
    D3D11DDIARG_CREATERESOURCE up = {0};
    up.pMipInfoList = args->pMipInfoList;
    up.pInitialDataUP = args->pInitialDataUP;
    up.ResourceDimension = args->ResourceDimension;
    up.Usage = args->Usage;
    up.BindFlags = args->BindFlags;
    up.MapFlags = args->MapFlags;
    up.MiscFlags = args->MiscFlags;
    up.Format = args->Format;
    up.SampleDesc = args->SampleDesc;
    up.MipLevels = args->MipLevels;
    up.ArraySize = args->ArraySize;
    up.pPrimaryDesc = args->pPrimaryDesc;
    tritonCreateResource(device, &up, resource, runtime);
}
static SIZE_T APIENTRY srv_size(D3D10DDI_HDEVICE device, const D3D10DDIARG_CREATESHADERRESOURCEVIEW *args)
{ (void)device; (void)args; return sizeof(TRITON_SRVIEW); }
static SIZE_T APIENTRY srv_size_1(D3D10DDI_HDEVICE device, const D3D10_1DDIARG_CREATESHADERRESOURCEVIEW *args)
{ (void)device; (void)args; return sizeof(TRITON_SRVIEW); }

static void APIENTRY create_srv(D3D10DDI_HDEVICE device,
    const D3D10DDIARG_CREATESHADERRESOURCEVIEW *args,
    D3D10DDI_HSHADERRESOURCEVIEW view, D3D10DDI_HRTSHADERRESOURCEVIEW runtime)
{
    D3D11DDIARG_CREATESHADERRESOURCEVIEW up = {0};
    up.hDrvResource = args->hDrvResource;
    up.Format = args->Format;
    up.ResourceDimension = args->ResourceDimension;
    switch (args->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER: up.Buffer = args->Buffer; break;
    case D3D10DDIRESOURCE_TEXTURE1D: up.Tex1D = args->Tex1D; break;
    case D3D10DDIRESOURCE_TEXTURE2D: up.Tex2D = args->Tex2D; break;
    case D3D10DDIRESOURCE_TEXTURE3D: up.Tex3D = args->Tex3D; break;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        up.TexCube.MostDetailedMip = args->TexCube.MostDetailedMip;
        up.TexCube.MipLevels = args->TexCube.MipLevels;
        up.TexCube.NumCubes = 1;
        break;
    default: tritonSetError((PTRITON_DEVICE)device.pDrvPrivate, E_INVALIDARG); return;
    }
    tritonCreateSRV(device, &up, view, runtime);
}
static void APIENTRY create_srv_1(D3D10DDI_HDEVICE device,
    const D3D10_1DDIARG_CREATESHADERRESOURCEVIEW *args,
    D3D10DDI_HSHADERRESOURCEVIEW view, D3D10DDI_HRTSHADERRESOURCEVIEW runtime)
{
    D3D11DDIARG_CREATESHADERRESOURCEVIEW up = {0};
    up.hDrvResource = args->hDrvResource;
    up.Format = args->Format;
    up.ResourceDimension = args->ResourceDimension;
    switch (args->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER: up.Buffer = args->Buffer; break;
    case D3D10DDIRESOURCE_TEXTURE1D: up.Tex1D = args->Tex1D; break;
    case D3D10DDIRESOURCE_TEXTURE2D: up.Tex2D = args->Tex2D; break;
    case D3D10DDIRESOURCE_TEXTURE3D: up.Tex3D = args->Tex3D; break;
    case D3D10DDIRESOURCE_TEXTURECUBE: up.TexCube = args->TexCube; break;
    default: tritonSetError((PTRITON_DEVICE)device.pDrvPrivate, E_INVALIDARG); return;
    }
    tritonCreateSRV(device, &up, view, runtime);
}
static SIZE_T APIENTRY dsv_size(D3D10DDI_HDEVICE device, const D3D10DDIARG_CREATEDEPTHSTENCILVIEW *args)
{ (void)device; (void)args; return sizeof(TRITON_DSVIEW); }
static void APIENTRY create_dsv(D3D10DDI_HDEVICE device,
    const D3D10DDIARG_CREATEDEPTHSTENCILVIEW *args,
    D3D10DDI_HDEPTHSTENCILVIEW view, D3D10DDI_HRTDEPTHSTENCILVIEW runtime)
{
    D3D11DDIARG_CREATEDEPTHSTENCILVIEW up = {0};
    up.hDrvResource = args->hDrvResource;
    up.Format = args->Format;
    up.ResourceDimension = args->ResourceDimension;
    switch (args->ResourceDimension) {
    case D3D10DDIRESOURCE_TEXTURE1D: up.Tex1D = args->Tex1D; break;
    case D3D10DDIRESOURCE_TEXTURE2D: up.Tex2D = args->Tex2D; break;
    case D3D10DDIRESOURCE_TEXTURECUBE: up.TexCube = args->TexCube; break;
    default: tritonSetError((PTRITON_DEVICE)device.pDrvPrivate, E_INVALIDARG); return;
    }
    tritonCreateDSV(device, &up, view, runtime);
}
static SIZE_T APIENTRY blend_size(D3D10DDI_HDEVICE device, const D3D10_DDI_BLEND_DESC *args)
{ (void)device; (void)args; return sizeof(TRITON_BLENDSTATE); }
static void APIENTRY create_blend(D3D10DDI_HDEVICE device,
    const D3D10_DDI_BLEND_DESC *args, D3D10DDI_HBLENDSTATE state,
    D3D10DDI_HRTBLENDSTATE runtime)
{
    D3D10_1_DDI_BLEND_DESC up = {0};
    up.AlphaToCoverageEnable = args->AlphaToCoverageEnable;
    /* D3D10 has shared factors but independent enable and write masks. */
    up.IndependentBlendEnable = TRUE;
    for (UINT i = 0; i < 8; ++i) {
        up.RenderTarget[i].BlendEnable = args->BlendEnable[i];
        up.RenderTarget[i].SrcBlend = args->SrcBlend;
        up.RenderTarget[i].DestBlend = args->DestBlend;
        up.RenderTarget[i].BlendOp = args->BlendOp;
        up.RenderTarget[i].SrcBlendAlpha = args->SrcBlendAlpha;
        up.RenderTarget[i].DestBlendAlpha = args->DestBlendAlpha;
        up.RenderTarget[i].BlendOpAlpha = args->BlendOpAlpha;
        up.RenderTarget[i].RenderTargetWriteMask = args->RenderTargetWriteMask[i];
    }
    tritonCreateBlendState_10(device, &up, state, runtime);
}
static void APIENTRY set_targets(D3D10DDI_HDEVICE device,
    const D3D10DDI_HRENDERTARGETVIEW *views, UINT count, UINT clear,
    D3D10DDI_HDEPTHSTENCILVIEW depth)
{
    tritonSetRenderTargets(device, views, count, clear, depth, NULL, NULL, 0, 0, 0, 0);
}
static SIZE_T APIENTRY so_size(D3D10DDI_HDEVICE device,
    const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *args,
    const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{ (void)device; (void)args; (void)signatures; return sizeof(TRITON_SHADER); }
static void APIENTRY create_so(D3D10DDI_HDEVICE device,
    const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *args,
    D3D10DDI_HSHADER shader, D3D10DDI_HRTSHADER runtime,
    const D3D10DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY entries[128];
    UINT strides[4] = {0};
    UINT slots = 1;
    if (args->NumEntries > 128 || (args->NumEntries && !args->pOutputStreamDecl)) {
        tritonSetError((PTRITON_DEVICE)device.pDrvPrivate, E_INVALIDARG); return;
    }
    for (UINT i = 0; i < args->NumEntries; ++i) {
        const D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *src = &args->pOutputStreamDecl[i];
        if (src->OutputSlot >= 4) { tritonSetError((PTRITON_DEVICE)device.pDrvPrivate, E_INVALIDARG); return; }
        entries[i].Stream = 0;
        entries[i].OutputSlot = src->OutputSlot;
        entries[i].RegisterIndex = src->RegisterIndex;
        entries[i].RegisterMask = src->RegisterMask;
        if (slots <= src->OutputSlot) slots = src->OutputSlot + 1;
        for (UINT c = 0; c < 4; ++c)
            if (src->RegisterMask & (1u << c)) strides[src->OutputSlot] += 4;
    }
    /* Interleaved output has one explicit stride. Separate output slots
     * use the component sizes derived from their declaration. */
    if (slots == 1) strides[0] = args->StreamOutputStrideInBytes;
    D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT up = {0};
    up.pShaderCode = args->pShaderCode;
    up.pOutputStreamDecl = entries;
    up.NumEntries = args->NumEntries;
    up.BufferStridesInBytes = strides;
    up.NumStrides = slots;
    up.RasterizedStream = 0;
    tritonCreateGSWithSO_11(device, &up, shader, runtime, signatures);
}
static void APIENTRY relocate(D3D10DDI_HDEVICE device, D3D10DDI_DEVICEFUNCS *table)
{ (void)device; (void)table; /* No table address is retained. */ }
static void APIENTRY relocate_1(D3D10DDI_HDEVICE device, D3D10_1DDI_DEVICEFUNCS *table)
{ (void)device; (void)table; }

void tritonFillD3D10DeviceFuncs(D3D10DDI_DEVICEFUNCS *p)
{
    D3D11DDI_DEVICEFUNCS base = {0};
    tritonFillD3D11DeviceFuncs(&base);
    memset(p, 0, sizeof(*p));
    p->pfnDefaultConstantBufferUpdateSubresourceUP = base.pfnDefaultConstantBufferUpdateSubresourceUP;
    p->pfnVsSetConstantBuffers = base.pfnVsSetConstantBuffers;
    p->pfnPsSetShaderResources = base.pfnPsSetShaderResources;
    p->pfnPsSetShader = base.pfnPsSetShader;
    p->pfnPsSetSamplers = base.pfnPsSetSamplers;
    p->pfnVsSetShader = base.pfnVsSetShader;
    p->pfnDrawIndexed = base.pfnDrawIndexed;
    p->pfnDraw = base.pfnDraw;
    p->pfnDynamicIABufferMapNoOverwrite = base.pfnDynamicIABufferMapNoOverwrite;
    p->pfnDynamicIABufferUnmap = base.pfnDynamicIABufferUnmap;
    p->pfnDynamicConstantBufferMapDiscard = base.pfnDynamicConstantBufferMapDiscard;
    p->pfnDynamicIABufferMapDiscard = base.pfnDynamicIABufferMapDiscard;
    p->pfnDynamicConstantBufferUnmap = base.pfnDynamicConstantBufferUnmap;
    p->pfnPsSetConstantBuffers = base.pfnPsSetConstantBuffers;
    p->pfnIaSetInputLayout = base.pfnIaSetInputLayout;
    p->pfnIaSetVertexBuffers = base.pfnIaSetVertexBuffers;
    p->pfnIaSetIndexBuffer = base.pfnIaSetIndexBuffer;
    p->pfnDrawIndexedInstanced = base.pfnDrawIndexedInstanced;
    p->pfnDrawInstanced = base.pfnDrawInstanced;
    p->pfnDynamicResourceMapDiscard = base.pfnDynamicResourceMapDiscard;
    p->pfnDynamicResourceUnmap = base.pfnDynamicResourceUnmap;
    p->pfnGsSetConstantBuffers = base.pfnGsSetConstantBuffers;
    p->pfnGsSetShader = base.pfnGsSetShader;
    p->pfnIaSetTopology = base.pfnIaSetTopology;
    p->pfnStagingResourceMap = base.pfnStagingResourceMap;
    p->pfnStagingResourceUnmap = base.pfnStagingResourceUnmap;
    p->pfnVsSetShaderResources = base.pfnVsSetShaderResources;
    p->pfnVsSetSamplers = base.pfnVsSetSamplers;
    p->pfnGsSetShaderResources = base.pfnGsSetShaderResources;
    p->pfnGsSetSamplers = base.pfnGsSetSamplers;
    p->pfnSetRenderTargets = set_targets;
    p->pfnShaderResourceViewReadAfterWriteHazard = base.pfnShaderResourceViewReadAfterWriteHazard;
    p->pfnResourceReadAfterWriteHazard = base.pfnResourceReadAfterWriteHazard;
    p->pfnSetBlendState = base.pfnSetBlendState;
    p->pfnSetDepthStencilState = base.pfnSetDepthStencilState;
    p->pfnSetRasterizerState = base.pfnSetRasterizerState;
    p->pfnQueryEnd = base.pfnQueryEnd;
    p->pfnQueryBegin = base.pfnQueryBegin;
    p->pfnResourceCopyRegion = base.pfnResourceCopyRegion;
    p->pfnResourceUpdateSubresourceUP = base.pfnResourceUpdateSubresourceUP;
    p->pfnSoSetTargets = base.pfnSoSetTargets;
    p->pfnDrawAuto = base.pfnDrawAuto;
    p->pfnSetViewports = base.pfnSetViewports;
    p->pfnSetScissorRects = base.pfnSetScissorRects;
    p->pfnClearRenderTargetView = base.pfnClearRenderTargetView;
    p->pfnClearDepthStencilView = base.pfnClearDepthStencilView;
    p->pfnSetPredication = base.pfnSetPredication;
    p->pfnQueryGetData = base.pfnQueryGetData;
    p->pfnFlush = base.pfnFlush;
    p->pfnGenMips = base.pfnGenMips;
    p->pfnResourceCopy = base.pfnResourceCopy;
    p->pfnResourceResolveSubresource = base.pfnResourceResolveSubresource;
    p->pfnResourceMap = base.pfnResourceMap;
    p->pfnResourceUnmap = base.pfnResourceUnmap;
    p->pfnResourceIsStagingBusy = base.pfnResourceIsStagingBusy;
    p->pfnRelocateDeviceFuncs = relocate;
    p->pfnCalcPrivateResourceSize = resource_size;
    p->pfnCalcPrivateOpenedResourceSize = base.pfnCalcPrivateOpenedResourceSize;
    p->pfnCreateResource = create_resource;
    p->pfnOpenResource = base.pfnOpenResource;
    p->pfnDestroyResource = base.pfnDestroyResource;
    p->pfnCalcPrivateShaderResourceViewSize = srv_size;
    p->pfnCreateShaderResourceView = create_srv;
    p->pfnDestroyShaderResourceView = base.pfnDestroyShaderResourceView;
    p->pfnCalcPrivateRenderTargetViewSize = base.pfnCalcPrivateRenderTargetViewSize;
    p->pfnCreateRenderTargetView = base.pfnCreateRenderTargetView;
    p->pfnDestroyRenderTargetView = base.pfnDestroyRenderTargetView;
    p->pfnCalcPrivateDepthStencilViewSize = dsv_size;
    p->pfnCreateDepthStencilView = create_dsv;
    p->pfnDestroyDepthStencilView = base.pfnDestroyDepthStencilView;
    p->pfnCalcPrivateElementLayoutSize = base.pfnCalcPrivateElementLayoutSize;
    p->pfnCreateElementLayout = base.pfnCreateElementLayout;
    p->pfnDestroyElementLayout = base.pfnDestroyElementLayout;
    p->pfnCalcPrivateBlendStateSize = blend_size;
    p->pfnCreateBlendState = create_blend;
    p->pfnDestroyBlendState = base.pfnDestroyBlendState;
    p->pfnCalcPrivateDepthStencilStateSize = base.pfnCalcPrivateDepthStencilStateSize;
    p->pfnCreateDepthStencilState = base.pfnCreateDepthStencilState;
    p->pfnDestroyDepthStencilState = base.pfnDestroyDepthStencilState;
    p->pfnCalcPrivateRasterizerStateSize = base.pfnCalcPrivateRasterizerStateSize;
    p->pfnCreateRasterizerState = base.pfnCreateRasterizerState;
    p->pfnDestroyRasterizerState = base.pfnDestroyRasterizerState;
    p->pfnCalcPrivateShaderSize = base.pfnCalcPrivateShaderSize;
    p->pfnCreateVertexShader = base.pfnCreateVertexShader;
    p->pfnCreateGeometryShader = base.pfnCreateGeometryShader;
    p->pfnCreatePixelShader = base.pfnCreatePixelShader;
    p->pfnCalcPrivateGeometryShaderWithStreamOutput = so_size;
    p->pfnCreateGeometryShaderWithStreamOutput = create_so;
    p->pfnDestroyShader = base.pfnDestroyShader;
    p->pfnCalcPrivateSamplerSize = base.pfnCalcPrivateSamplerSize;
    p->pfnCreateSampler = base.pfnCreateSampler;
    p->pfnDestroySampler = base.pfnDestroySampler;
    p->pfnCalcPrivateQuerySize = base.pfnCalcPrivateQuerySize;
    p->pfnCreateQuery = base.pfnCreateQuery;
    p->pfnDestroyQuery = base.pfnDestroyQuery;
    p->pfnCheckFormatSupport = base.pfnCheckFormatSupport;
    p->pfnCheckMultisampleQualityLevels = base.pfnCheckMultisampleQualityLevels;
    p->pfnCheckCounterInfo = base.pfnCheckCounterInfo;
    p->pfnCheckCounter = base.pfnCheckCounter;
    p->pfnDestroyDevice = base.pfnDestroyDevice;
    p->pfnSetTextFilterSize = base.pfnSetTextFilterSize;
}

void tritonFillD3D10_1DeviceFuncs(D3D10_1DDI_DEVICEFUNCS *p)
{
    D3D11DDI_DEVICEFUNCS base = {0};
    tritonFillD3D11DeviceFuncs(&base);
    memset(p, 0, sizeof(*p));
    p->pfnDefaultConstantBufferUpdateSubresourceUP = base.pfnDefaultConstantBufferUpdateSubresourceUP;
    p->pfnVsSetConstantBuffers = base.pfnVsSetConstantBuffers;
    p->pfnPsSetShaderResources = base.pfnPsSetShaderResources;
    p->pfnPsSetShader = base.pfnPsSetShader;
    p->pfnPsSetSamplers = base.pfnPsSetSamplers;
    p->pfnVsSetShader = base.pfnVsSetShader;
    p->pfnDrawIndexed = base.pfnDrawIndexed;
    p->pfnDraw = base.pfnDraw;
    p->pfnDynamicIABufferMapNoOverwrite = base.pfnDynamicIABufferMapNoOverwrite;
    p->pfnDynamicIABufferUnmap = base.pfnDynamicIABufferUnmap;
    p->pfnDynamicConstantBufferMapDiscard = base.pfnDynamicConstantBufferMapDiscard;
    p->pfnDynamicIABufferMapDiscard = base.pfnDynamicIABufferMapDiscard;
    p->pfnDynamicConstantBufferUnmap = base.pfnDynamicConstantBufferUnmap;
    p->pfnPsSetConstantBuffers = base.pfnPsSetConstantBuffers;
    p->pfnIaSetInputLayout = base.pfnIaSetInputLayout;
    p->pfnIaSetVertexBuffers = base.pfnIaSetVertexBuffers;
    p->pfnIaSetIndexBuffer = base.pfnIaSetIndexBuffer;
    p->pfnDrawIndexedInstanced = base.pfnDrawIndexedInstanced;
    p->pfnDrawInstanced = base.pfnDrawInstanced;
    p->pfnDynamicResourceMapDiscard = base.pfnDynamicResourceMapDiscard;
    p->pfnDynamicResourceUnmap = base.pfnDynamicResourceUnmap;
    p->pfnGsSetConstantBuffers = base.pfnGsSetConstantBuffers;
    p->pfnGsSetShader = base.pfnGsSetShader;
    p->pfnIaSetTopology = base.pfnIaSetTopology;
    p->pfnStagingResourceMap = base.pfnStagingResourceMap;
    p->pfnStagingResourceUnmap = base.pfnStagingResourceUnmap;
    p->pfnVsSetShaderResources = base.pfnVsSetShaderResources;
    p->pfnVsSetSamplers = base.pfnVsSetSamplers;
    p->pfnGsSetShaderResources = base.pfnGsSetShaderResources;
    p->pfnGsSetSamplers = base.pfnGsSetSamplers;
    p->pfnSetRenderTargets = set_targets;
    p->pfnShaderResourceViewReadAfterWriteHazard = base.pfnShaderResourceViewReadAfterWriteHazard;
    p->pfnResourceReadAfterWriteHazard = base.pfnResourceReadAfterWriteHazard;
    p->pfnSetBlendState = base.pfnSetBlendState;
    p->pfnSetDepthStencilState = base.pfnSetDepthStencilState;
    p->pfnSetRasterizerState = base.pfnSetRasterizerState;
    p->pfnQueryEnd = base.pfnQueryEnd;
    p->pfnQueryBegin = base.pfnQueryBegin;
    p->pfnResourceCopyRegion = base.pfnResourceCopyRegion;
    p->pfnResourceUpdateSubresourceUP = base.pfnResourceUpdateSubresourceUP;
    p->pfnSoSetTargets = base.pfnSoSetTargets;
    p->pfnDrawAuto = base.pfnDrawAuto;
    p->pfnSetViewports = base.pfnSetViewports;
    p->pfnSetScissorRects = base.pfnSetScissorRects;
    p->pfnClearRenderTargetView = base.pfnClearRenderTargetView;
    p->pfnClearDepthStencilView = base.pfnClearDepthStencilView;
    p->pfnSetPredication = base.pfnSetPredication;
    p->pfnQueryGetData = base.pfnQueryGetData;
    p->pfnFlush = base.pfnFlush;
    p->pfnGenMips = base.pfnGenMips;
    p->pfnResourceCopy = base.pfnResourceCopy;
    p->pfnResourceResolveSubresource = base.pfnResourceResolveSubresource;
    p->pfnResourceMap = base.pfnResourceMap;
    p->pfnResourceUnmap = base.pfnResourceUnmap;
    p->pfnResourceIsStagingBusy = base.pfnResourceIsStagingBusy;
    p->pfnRelocateDeviceFuncs = relocate_1;
    p->pfnCalcPrivateResourceSize = resource_size;
    p->pfnCalcPrivateOpenedResourceSize = base.pfnCalcPrivateOpenedResourceSize;
    p->pfnCreateResource = create_resource;
    p->pfnOpenResource = base.pfnOpenResource;
    p->pfnDestroyResource = base.pfnDestroyResource;
    p->pfnCalcPrivateShaderResourceViewSize = srv_size_1;
    p->pfnCreateShaderResourceView = create_srv_1;
    p->pfnDestroyShaderResourceView = base.pfnDestroyShaderResourceView;
    p->pfnCalcPrivateRenderTargetViewSize = base.pfnCalcPrivateRenderTargetViewSize;
    p->pfnCreateRenderTargetView = base.pfnCreateRenderTargetView;
    p->pfnDestroyRenderTargetView = base.pfnDestroyRenderTargetView;
    p->pfnCalcPrivateDepthStencilViewSize = dsv_size;
    p->pfnCreateDepthStencilView = create_dsv;
    p->pfnDestroyDepthStencilView = base.pfnDestroyDepthStencilView;
    p->pfnCalcPrivateElementLayoutSize = base.pfnCalcPrivateElementLayoutSize;
    p->pfnCreateElementLayout = base.pfnCreateElementLayout;
    p->pfnDestroyElementLayout = base.pfnDestroyElementLayout;
    p->pfnCalcPrivateBlendStateSize = base.pfnCalcPrivateBlendStateSize;
    p->pfnCreateBlendState = base.pfnCreateBlendState;
    p->pfnDestroyBlendState = base.pfnDestroyBlendState;
    p->pfnCalcPrivateDepthStencilStateSize = base.pfnCalcPrivateDepthStencilStateSize;
    p->pfnCreateDepthStencilState = base.pfnCreateDepthStencilState;
    p->pfnDestroyDepthStencilState = base.pfnDestroyDepthStencilState;
    p->pfnCalcPrivateRasterizerStateSize = base.pfnCalcPrivateRasterizerStateSize;
    p->pfnCreateRasterizerState = base.pfnCreateRasterizerState;
    p->pfnDestroyRasterizerState = base.pfnDestroyRasterizerState;
    p->pfnCalcPrivateShaderSize = base.pfnCalcPrivateShaderSize;
    p->pfnCreateVertexShader = base.pfnCreateVertexShader;
    p->pfnCreateGeometryShader = base.pfnCreateGeometryShader;
    p->pfnCreatePixelShader = base.pfnCreatePixelShader;
    p->pfnCalcPrivateGeometryShaderWithStreamOutput = so_size;
    p->pfnCreateGeometryShaderWithStreamOutput = create_so;
    p->pfnDestroyShader = base.pfnDestroyShader;
    p->pfnCalcPrivateSamplerSize = base.pfnCalcPrivateSamplerSize;
    p->pfnCreateSampler = base.pfnCreateSampler;
    p->pfnDestroySampler = base.pfnDestroySampler;
    p->pfnCalcPrivateQuerySize = base.pfnCalcPrivateQuerySize;
    p->pfnCreateQuery = base.pfnCreateQuery;
    p->pfnDestroyQuery = base.pfnDestroyQuery;
    p->pfnCheckFormatSupport = base.pfnCheckFormatSupport;
    p->pfnCheckMultisampleQualityLevels = base.pfnCheckMultisampleQualityLevels;
    p->pfnCheckCounterInfo = base.pfnCheckCounterInfo;
    p->pfnCheckCounter = base.pfnCheckCounter;
    p->pfnDestroyDevice = base.pfnDestroyDevice;
    p->pfnSetTextFilterSize = base.pfnSetTextFilterSize;
    p->pfnResourceConvert = base.pfnResourceConvert;
    p->pfnResourceConvertRegion = base.pfnResourceConvertRegion;
}

/* CreateGeometryShaderWithStreamOutput accepts vertex-shader bytecode to
 * describe the passthrough stage. Its output signature supplies the SO
 * semantics; the VS body is not executed by that API's passthrough GS. */
void *tritonD3D10BuildSOAlias(const void *entries, UINT count, UINT stride, SIZE_T *bytes)
{
    UINT tokens[2 + 32 * 12 + 1], n = 2;
    BYTE masks[32] = {0};
    UINT values[32] = {0};
    *bytes = 0;
    if (!entries || !count || count > 128 || (stride != 12 && stride != 20)) return NULL;
    for (UINT i = 0; i < count; ++i) {
        const BYTE *entry = (const BYTE *)entries + i * stride;
        UINT reg, value;
        memcpy(&value, entry, 4);
        memcpy(&reg, entry + 4, 4);
        if (reg >= 32 || !(entry[8] & 15) || (entry[8] & ~15)) return NULL;
        masks[reg] |= entry[8];
        if (value) values[reg] = value;
    }
    tokens[0] = 0x00010040u; /* vs_4_0 */
    for (UINT r = 0; r < 32; ++r) if (masks[r]) {
        UINT operand = 0x00102002u | ((UINT)masks[r] << 4);
        tokens[n++] = (values[r] ? 0x04000067u : 0x03000065u); /* dcl_output[_siv] */
        tokens[n++] = operand;
        tokens[n++] = r;
        if (values[r]) tokens[n++] = values[r];
        tokens[n++] = 0x08000036u; /* mov oN.mask, l(0,0,0,1) */
        tokens[n++] = operand;
        tokens[n++] = r;
        tokens[n++] = 0x00004002u;
        tokens[n++] = 0;
        tokens[n++] = 0;
        tokens[n++] = 0;
        tokens[n++] = 0x3f800000u;
    }
    tokens[n++] = 0x0100003eu; /* ret */
    tokens[1] = n;
    return tritonBuildDxbc(tokens, n * sizeof(UINT), NULL, 0, entries, count,
                          NULL, 0, stride, bytes);
}

/* Rebuild the embedded blit shaders as SM4. fxc emits one SM5 BFI in
 * VSBlit for (vertexID << 1) & 2: lower that exact expression to SM4 ISHL
 * and AND. Reject other SM5 instructions rather than relabeling bytecode. */
void *tritonD3D10BlitBytecode(const void *data, SIZE_T size, SIZE_T *bytes)
{
    const BYTE *src = data;
    UINT chunks;
    *bytes = 0;
    if (size < 32 || memcmp(src, "DXBC", 4)) return NULL;
    memcpy(&chunks, src + 28, 4);
    if (chunks > (size - 32) / 4) return NULL;
    for (UINT i = 0; i < chunks; ++i) {
        UINT offset, length;
        memcpy(&offset, src + 32 + i * 4, 4);
        if (offset > size - 8) return NULL;
        memcpy(&length, src + offset + 4, 4);
        if (length > size - offset - 8) return NULL;
        if (memcmp(src + offset, "SHEX", 4) && memcmp(src + offset, "SHDR", 4)) continue;
        if (length < 8 || (length & 3)) return NULL;
        UINT *tokens = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)length * 2);
        if (!tokens) return NULL;
        const UINT *input = (const UINT *)(src + offset + 8);
        UINT count = length / 4, out = 2;
        if (input[1] != count) { HeapFree(GetProcessHeap(), 0, tokens); return NULL; }
        tokens[0] = (input[0] & 0xffff0000u) | 0x40u;
        for (UINT at = 2; at < count;) {
            UINT words = (input[at] >> 24) & 127, opcode = input[at] & 2047;
            if (!words || words > count - at) goto invalid;
            if (opcode == 140) {
                static const UINT bfi[] = {0x0b00008c,0x00100012,0,0x4001,1,
                                           0x4001,1,0x0010100a,0,0x4001,0};
                static const UINT lowered[] = {
                    0x07000029,0x00100012,0,0x0010100a,0,0x4001,1,
                    0x07000001,0x00100012,0,0x0010000a,0,0x4001,2};
                if (words != 11 || memcmp(input + at, bfi, sizeof(bfi))) goto invalid;
                memcpy(tokens + out, lowered, sizeof(lowered));
                out += sizeof(lowered) / sizeof(lowered[0]);
            } else if (opcode == 72 && (input[at] & 0x80000000u)) {
                /* fxc SM5 sample_l adds dimension/return-type opcode
                 * extensions. SM4 obtains both from dcl_resource. */
                if (words != 13 || input[at + 1] != 0x800000c2u ||
                    input[at + 2] != 0x00155543u) goto invalid;
                tokens[out++] = 0x0b000048u;
                memcpy(tokens + out, input + at + 3, (words - 3) * sizeof(UINT));
                out += words - 3;
            } else {
                if (opcode > 106 || (input[at] & 0x80000000u)) goto invalid;
                memcpy(tokens + out, input + at, words * sizeof(UINT));
                out += words;
            }
            at += words;
        }
        tokens[1] = out;
        void *result = tritonBuildDxbc(tokens, out * sizeof(UINT), NULL, 0, NULL, 0,
                                      NULL, 0, 12, bytes);
        HeapFree(GetProcessHeap(), 0, tokens);
        return result;
invalid:
        HeapFree(GetProcessHeap(), 0, tokens);
        return NULL;
    }
    return NULL;
}
