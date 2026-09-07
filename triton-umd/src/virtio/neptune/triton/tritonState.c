/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Blend / depth-stencil / rasterizer / sampler / element-layout
 * create+destroy+set handlers, viewports / scissors / topology,
 * IA vertex/index buffers, draw/dispatch/stream-output entry points.
 *
 * Element-layout creation is lazy: the DDI passes the input element
 * list but no VS bytecode, and CreateInputLayout needs a VS to validate
 * against. We stash the elements at create time and create the
 * ID3D11InputLayout on the first IaSetInputLayout once a VS is bound.
 */

#include "triton.h"
#include "triton_log.h"

/* D3D10/11_1 DDI enums share the underlying integer values with their
 * D3D11 counterparts where d3d10umddi.h says "HW consumes values
 * directly". Casts make the intent visible. */

static D3D11_BLEND tritonBlend(D3D10_DDI_BLEND b)        { return (D3D11_BLEND)b; }
static D3D11_BLEND_OP tritonBlendOp(D3D10_DDI_BLEND_OP o){ return (D3D11_BLEND_OP)o; }

/* ---------- Blend (D3D11_1 layout) ---------- */

SIZE_T APIENTRY
tritonCalcPrivateBlendStateSize(D3D10DDI_HDEVICE hDev, const D3D11_1_DDI_BLEND_DESC *pArgs)
{
    return sizeof(TRITON_BLENDSTATE);
}

void APIENTRY
tritonCreateBlendState(D3D10DDI_HDEVICE hDevice,
                       const D3D11_1_DDI_BLEND_DESC *pArgs,
                       D3D10DDI_HBLENDSTATE hState,
                       D3D10DDI_HRTBLENDSTATE hRTState)
{
    PTRITON_DEVICE     pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_BLENDSTATE s  = (PTRITON_BLENDSTATE)(hState.pDrvPrivate);
    if (!pD || !s) return;
    s->pState = NULL;

    D3D11_BLEND_DESC1 d = {};
    d.AlphaToCoverageEnable  = pArgs->AlphaToCoverageEnable;
    d.IndependentBlendEnable = pArgs->IndependentBlendEnable;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        const D3D11_1_DDI_RENDER_TARGET_BLEND_DESC *src = &pArgs->RenderTarget[i];
        D3D11_RENDER_TARGET_BLEND_DESC1            *dst = &d.RenderTarget[i];
        dst->BlendEnable           = src->BlendEnable;
        dst->LogicOpEnable         = src->LogicOpEnable;
        dst->SrcBlend              = tritonBlend(src->SrcBlend);
        dst->DestBlend             = tritonBlend(src->DestBlend);
        dst->BlendOp               = tritonBlendOp(src->BlendOp);
        dst->SrcBlendAlpha         = tritonBlend(src->SrcBlendAlpha);
        dst->DestBlendAlpha        = tritonBlend(src->DestBlendAlpha);
        dst->BlendOpAlpha          = tritonBlendOp(src->BlendOpAlpha);
        dst->LogicOp               = (D3D11_LOGIC_OP)src->LogicOp;
        dst->RenderTargetWriteMask = src->RenderTargetWriteMask;
    }
    HRESULT hr = ID3D11Device1_CreateBlendState1(pD->pDev1, &d, &s->pState);
    if (FAILED(hr)) {
        TR_LOG("CreateBlendState: failed 0x%08lx", hr);
        s->pState = NULL;
        tritonSetError(pD, hr);
    }
}

void APIENTRY
tritonDestroyBlendState(D3D10DDI_HDEVICE hDev, D3D10DDI_HBLENDSTATE hState)
{
    PTRITON_BLENDSTATE s = (PTRITON_BLENDSTATE)(hState.pDrvPrivate);
    if (s && s->pState) { ID3D11BlendState1_Release(s->pState); s->pState = NULL; }
}

/* ---------- Blend (11.0 device table — D3D10.1 desc) ----------
 * The D3D11DDI_DEVICEFUNCS table installs a PFND3D10_1DDI_CREATEBLENDSTATE
 * slot, so the desc handed in here is D3D10_1_DDI_BLEND_DESC (per-RT
 * entries minus LogicOp/LogicOpEnable). Upgrade to the 11.1 form and
 * delegate. */

SIZE_T APIENTRY
tritonCalcPrivateBlendStateSize_10(D3D10DDI_HDEVICE hDev, const D3D10_1_DDI_BLEND_DESC *pArgs)
{
    return sizeof(TRITON_BLENDSTATE);
}

void APIENTRY
tritonCreateBlendState_10(D3D10DDI_HDEVICE hDevice,
                          const D3D10_1_DDI_BLEND_DESC *pArgs,
                          D3D10DDI_HBLENDSTATE hState,
                          D3D10DDI_HRTBLENDSTATE hRTState)
{
    D3D11_1_DDI_BLEND_DESC up = {};
    up.AlphaToCoverageEnable  = pArgs->AlphaToCoverageEnable;
    up.IndependentBlendEnable = pArgs->IndependentBlendEnable;
    for (UINT i = 0; i < D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        const D3D10_DDI_RENDER_TARGET_BLEND_DESC1 *src = &pArgs->RenderTarget[i];
        D3D11_1_DDI_RENDER_TARGET_BLEND_DESC      *dst = &up.RenderTarget[i];
        dst->BlendEnable           = src->BlendEnable;
        dst->LogicOpEnable         = FALSE;
        dst->SrcBlend              = src->SrcBlend;
        dst->DestBlend             = src->DestBlend;
        dst->BlendOp               = src->BlendOp;
        dst->SrcBlendAlpha         = src->SrcBlendAlpha;
        dst->DestBlendAlpha        = src->DestBlendAlpha;
        dst->BlendOpAlpha          = src->BlendOpAlpha;
        dst->LogicOp               = D3D11_1_DDI_LOGIC_OP_CLEAR;
        dst->RenderTargetWriteMask = src->RenderTargetWriteMask;
    }
    tritonCreateBlendState(hDevice, &up, hState, hRTState);
}

void APIENTRY
tritonSetBlendState(D3D10DDI_HDEVICE hDevice,
                    D3D10DDI_HBLENDSTATE hState,
                    const FLOAT BlendFactor[4],
                    UINT SampleMask)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    PTRITON_BLENDSTATE s = (PTRITON_BLENDSTATE)(hState.pDrvPrivate);
    ID3D11DeviceContext1_OMSetBlendState(
        pD->pCtx1, s ? (ID3D11BlendState *)s->pState : NULL, BlendFactor, SampleMask);
}

/* ---------- Depth-stencil ---------- */

SIZE_T APIENTRY
tritonCalcPrivateDepthStencilStateSize(D3D10DDI_HDEVICE hDev, const D3D10_DDI_DEPTH_STENCIL_DESC *pArgs)
{
    return sizeof(TRITON_DEPTHSTENCILSTATE);
}

void APIENTRY
tritonCreateDepthStencilState(D3D10DDI_HDEVICE hDevice,
                              const D3D10_DDI_DEPTH_STENCIL_DESC *pArgs,
                              D3D10DDI_HDEPTHSTENCILSTATE hState,
                              D3D10DDI_HRTDEPTHSTENCILSTATE hRTState)
{
    PTRITON_DEVICE             pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_DEPTHSTENCILSTATE  s  = (PTRITON_DEPTHSTENCILSTATE)(hState.pDrvPrivate);
    if (!pD || !s) return;
    s->pState = NULL;

    D3D11_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable    = pArgs->DepthEnable;
    d.DepthWriteMask = (D3D11_DEPTH_WRITE_MASK)pArgs->DepthWriteMask;
    d.DepthFunc      = (D3D11_COMPARISON_FUNC)pArgs->DepthFunc;
    d.StencilEnable  = pArgs->StencilEnable;
    d.StencilReadMask  = pArgs->StencilReadMask;
    d.StencilWriteMask = pArgs->StencilWriteMask;
    d.FrontFace.StencilFailOp      = (D3D11_STENCIL_OP)pArgs->FrontFace.StencilFailOp;
    d.FrontFace.StencilDepthFailOp = (D3D11_STENCIL_OP)pArgs->FrontFace.StencilDepthFailOp;
    d.FrontFace.StencilPassOp      = (D3D11_STENCIL_OP)pArgs->FrontFace.StencilPassOp;
    d.FrontFace.StencilFunc        = (D3D11_COMPARISON_FUNC)pArgs->FrontFace.StencilFunc;
    d.BackFace.StencilFailOp       = (D3D11_STENCIL_OP)pArgs->BackFace.StencilFailOp;
    d.BackFace.StencilDepthFailOp  = (D3D11_STENCIL_OP)pArgs->BackFace.StencilDepthFailOp;
    d.BackFace.StencilPassOp       = (D3D11_STENCIL_OP)pArgs->BackFace.StencilPassOp;
    d.BackFace.StencilFunc         = (D3D11_COMPARISON_FUNC)pArgs->BackFace.StencilFunc;
    /* D3D11_DEPTH_STENCIL_DESC has a single StencilEnable; the DDI adds
     * per-face FrontEnable/BackEnable that can differ. A face that is
     * individually disabled is expressed as the canonical "do nothing"
     * stencil (Func=ALWAYS, all ops=KEEP). */
    if (pArgs->StencilEnable && !pArgs->FrontEnable) {
        d.FrontFace.StencilFailOp = d.FrontFace.StencilDepthFailOp =
            d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        d.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    }
    if (pArgs->StencilEnable && !pArgs->BackEnable) {
        d.BackFace.StencilFailOp = d.BackFace.StencilDepthFailOp =
            d.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        d.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    }
    HRESULT hr = ID3D11Device1_CreateDepthStencilState(pD->pDev1, &d, &s->pState);
    if (FAILED(hr)) {
        TR_LOG("CreateDepthStencilState: failed 0x%08lx", hr);
        s->pState = NULL;
        tritonSetError(pD, hr);
    }
}

void APIENTRY
tritonDestroyDepthStencilState(D3D10DDI_HDEVICE hDev, D3D10DDI_HDEPTHSTENCILSTATE hState)
{
    PTRITON_DEPTHSTENCILSTATE s = (PTRITON_DEPTHSTENCILSTATE)(hState.pDrvPrivate);
    if (s && s->pState) { ID3D11DepthStencilState_Release(s->pState); s->pState = NULL; }
}

void APIENTRY
tritonSetDepthStencilState(D3D10DDI_HDEVICE hDevice,
                           D3D10DDI_HDEPTHSTENCILSTATE hState,
                           UINT StencilRef)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    PTRITON_DEPTHSTENCILSTATE s = (PTRITON_DEPTHSTENCILSTATE)(hState.pDrvPrivate);
    ID3D11DeviceContext1_OMSetDepthStencilState(
        pD->pCtx1, s ? s->pState : NULL, StencilRef);
}

/* ---------- Rasterizer (D3D11_1 layout) ---------- */

SIZE_T APIENTRY
tritonCalcPrivateRasterizerStateSize(D3D10DDI_HDEVICE hDev, const D3D11_1_DDI_RASTERIZER_DESC *pArgs)
{
    return sizeof(TRITON_RASTERIZERSTATE);
}

void APIENTRY
tritonCreateRasterizerState(D3D10DDI_HDEVICE hDevice,
                            const D3D11_1_DDI_RASTERIZER_DESC *pArgs,
                            D3D10DDI_HRASTERIZERSTATE hState,
                            D3D10DDI_HRTRASTERIZERSTATE hRTState)
{
    PTRITON_DEVICE          pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RASTERIZERSTATE s  = (PTRITON_RASTERIZERSTATE)(hState.pDrvPrivate);
    if (!pD || !s) return;
    s->pState = NULL;

    D3D11_RASTERIZER_DESC1 d = {};
    d.FillMode              = (D3D11_FILL_MODE)pArgs->FillMode;
    d.CullMode              = (D3D11_CULL_MODE)pArgs->CullMode;
    d.FrontCounterClockwise = pArgs->FrontCounterClockwise;
    d.DepthBias             = pArgs->DepthBias;
    d.DepthBiasClamp        = pArgs->DepthBiasClamp;
    d.SlopeScaledDepthBias  = pArgs->SlopeScaledDepthBias;
    d.DepthClipEnable       = pArgs->DepthClipEnable;
    d.ScissorEnable         = pArgs->ScissorEnable;
    d.MultisampleEnable     = pArgs->MultisampleEnable;
    d.AntialiasedLineEnable = pArgs->AntialiasedLineEnable;
    d.ForcedSampleCount     = pArgs->ForcedSampleCount;
    HRESULT hr = ID3D11Device1_CreateRasterizerState1(pD->pDev1, &d, &s->pState);
    if (FAILED(hr)) {
        TR_LOG("CreateRasterizerState: failed 0x%08lx", hr);
        s->pState = NULL;
        tritonSetError(pD, hr);
    }
}

void APIENTRY
tritonDestroyRasterizerState(D3D10DDI_HDEVICE hDev, D3D10DDI_HRASTERIZERSTATE hState)
{
    PTRITON_RASTERIZERSTATE s = (PTRITON_RASTERIZERSTATE)(hState.pDrvPrivate);
    if (s && s->pState) { ID3D11RasterizerState1_Release(s->pState); s->pState = NULL; }
}

/* ---------- Rasterizer (11.0 device table — D3D10.0 desc) ----------
 * The 11.0 desc has no ForcedSampleCount. Upgrade to the 11.1 form. */

SIZE_T APIENTRY
tritonCalcPrivateRasterizerStateSize_10(D3D10DDI_HDEVICE hDev, const D3D10_DDI_RASTERIZER_DESC *pArgs)
{
    return sizeof(TRITON_RASTERIZERSTATE);
}

void APIENTRY
tritonCreateRasterizerState_10(D3D10DDI_HDEVICE hDevice,
                               const D3D10_DDI_RASTERIZER_DESC *pArgs,
                               D3D10DDI_HRASTERIZERSTATE hState,
                               D3D10DDI_HRTRASTERIZERSTATE hRTState)
{
    D3D11_1_DDI_RASTERIZER_DESC up = {};
    up.FillMode              = pArgs->FillMode;
    up.CullMode              = pArgs->CullMode;
    up.FrontCounterClockwise = pArgs->FrontCounterClockwise;
    up.DepthBias             = pArgs->DepthBias;
    up.DepthBiasClamp        = pArgs->DepthBiasClamp;
    up.SlopeScaledDepthBias  = pArgs->SlopeScaledDepthBias;
    up.DepthClipEnable       = pArgs->DepthClipEnable;
    up.ScissorEnable         = pArgs->ScissorEnable;
    up.MultisampleEnable     = pArgs->MultisampleEnable;
    up.AntialiasedLineEnable = pArgs->AntialiasedLineEnable;
    up.ForcedSampleCount     = 0;
    tritonCreateRasterizerState(hDevice, &up, hState, hRTState);
}

void APIENTRY
tritonSetRasterizerState(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRASTERIZERSTATE hState)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    PTRITON_RASTERIZERSTATE s = (PTRITON_RASTERIZERSTATE)(hState.pDrvPrivate);
    ID3D11DeviceContext1_RSSetState(
        pD->pCtx1, s ? (ID3D11RasterizerState *)s->pState : NULL);
}

/* ---------- Rasterizer (WDDM 2.0: adds ConservativeRasterizationMode) ----
 * Forward through ID3D11Device3::CreateRasterizerState2.
 * ID3D11RasterizerState2 derives from RasterizerState1, so the result is
 * stored as the base pointer. Without Device3 the call falls back to the
 * 11.1 path and ConservativeRaster is logged and dropped. */

SIZE_T APIENTRY
tritonCalcPrivateRasterizerStateSize_WDDM2_0(D3D10DDI_HDEVICE hDev,
                                             const D3DWDDM2_0DDI_RASTERIZER_DESC *pArgs)
{
    return sizeof(TRITON_RASTERIZERSTATE);
}

void APIENTRY
tritonCreateRasterizerState_WDDM2_0(D3D10DDI_HDEVICE hDevice,
                                    const D3DWDDM2_0DDI_RASTERIZER_DESC *pArgs,
                                    D3D10DDI_HRASTERIZERSTATE hState,
                                    D3D10DDI_HRTRASTERIZERSTATE hRTState)
{
    PTRITON_DEVICE          pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RASTERIZERSTATE s  = (PTRITON_RASTERIZERSTATE)(hState.pDrvPrivate);
    if (!pD || !s) return;

    if (!pD->pDev3) {
        D3D11_1_DDI_RASTERIZER_DESC up = {};
        up.FillMode              = pArgs->FillMode;
        up.CullMode              = pArgs->CullMode;
        up.FrontCounterClockwise = pArgs->FrontCounterClockwise;
        up.DepthBias             = pArgs->DepthBias;
        up.DepthBiasClamp        = pArgs->DepthBiasClamp;
        up.SlopeScaledDepthBias  = pArgs->SlopeScaledDepthBias;
        up.DepthClipEnable       = pArgs->DepthClipEnable;
        up.ScissorEnable         = pArgs->ScissorEnable;
        up.MultisampleEnable     = pArgs->MultisampleEnable;
        up.AntialiasedLineEnable = pArgs->AntialiasedLineEnable;
        up.ForcedSampleCount     = pArgs->ForcedSampleCount;
        if (pArgs->ConservativeRasterizationMode != D3DWDDM2_0DDI_CONSERVATIVE_RASTERIZATION_OFF)
            TR_LOG("CreateRasterizerState_WDDM2_0: ConservativeRaster requested but no Device3");
        tritonCreateRasterizerState(hDevice, &up, hState, hRTState);
        return;
    }

    s->pState = NULL;

    D3D11_RASTERIZER_DESC2 d = {};
    d.FillMode              = (D3D11_FILL_MODE)pArgs->FillMode;
    d.CullMode              = (D3D11_CULL_MODE)pArgs->CullMode;
    d.FrontCounterClockwise = pArgs->FrontCounterClockwise;
    d.DepthBias             = pArgs->DepthBias;
    d.DepthBiasClamp        = pArgs->DepthBiasClamp;
    d.SlopeScaledDepthBias  = pArgs->SlopeScaledDepthBias;
    d.DepthClipEnable       = pArgs->DepthClipEnable;
    d.ScissorEnable         = pArgs->ScissorEnable;
    d.MultisampleEnable     = pArgs->MultisampleEnable;
    d.AntialiasedLineEnable = pArgs->AntialiasedLineEnable;
    d.ForcedSampleCount     = pArgs->ForcedSampleCount;
    d.ConservativeRaster    = (D3D11_CONSERVATIVE_RASTERIZATION_MODE)
                              pArgs->ConservativeRasterizationMode;

    ID3D11RasterizerState2 *pState2 = NULL;
    HRESULT hr = ID3D11Device3_CreateRasterizerState2(pD->pDev3, &d, &pState2);
    if (FAILED(hr)) {
        TR_LOG("CreateRasterizerState_WDDM2_0: failed 0x%08lx", hr);
        s->pState = NULL;
        tritonSetError(pD, hr);
        return;
    }
    s->pState = (ID3D11RasterizerState1 *)pState2;
}

/* ---------- Sampler ---------- */

SIZE_T APIENTRY
tritonCalcPrivateSamplerSize(D3D10DDI_HDEVICE hDev, const D3D10_DDI_SAMPLER_DESC *pArgs)
{
    return sizeof(TRITON_SAMPLER);
}

void APIENTRY
tritonCreateSampler(D3D10DDI_HDEVICE hDevice,
                    const D3D10_DDI_SAMPLER_DESC *pArgs,
                    D3D10DDI_HSAMPLER hState,
                    D3D10DDI_HRTSAMPLER hRTState)
{
    PTRITON_DEVICE  pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SAMPLER s  = (PTRITON_SAMPLER)(hState.pDrvPrivate);
    if (!pD || !s) return;
    s->pState = NULL;

    D3D11_SAMPLER_DESC d = {};
    d.Filter         = (D3D11_FILTER)pArgs->Filter;
    d.AddressU       = (D3D11_TEXTURE_ADDRESS_MODE)pArgs->AddressU;
    d.AddressV       = (D3D11_TEXTURE_ADDRESS_MODE)pArgs->AddressV;
    d.AddressW       = (D3D11_TEXTURE_ADDRESS_MODE)pArgs->AddressW;
    d.MipLODBias     = pArgs->MipLODBias;
    /* D3D11 spec: MaxAnisotropy valid range is 1..16. */
    d.MaxAnisotropy  = pArgs->MaxAnisotropy < 1  ? 1
                     : pArgs->MaxAnisotropy > 16 ? 16
                     : pArgs->MaxAnisotropy;
    d.ComparisonFunc = (D3D11_COMPARISON_FUNC)pArgs->ComparisonFunc;
    for (int i = 0; i < 4; ++i) d.BorderColor[i] = pArgs->BorderColor[i];
    d.MinLOD         = pArgs->MinLOD;
    d.MaxLOD         = pArgs->MaxLOD;
    HRESULT hr = ID3D11Device1_CreateSamplerState(pD->pDev1, &d, &s->pState);
    if (FAILED(hr)) {
        TR_LOG("CreateSampler: failed 0x%08lx", hr);
        s->pState = NULL;
        tritonSetError(pD, hr);
    }
}

void APIENTRY
tritonDestroySampler(D3D10DDI_HDEVICE hDev, D3D10DDI_HSAMPLER hState)
{
    PTRITON_SAMPLER s = (PTRITON_SAMPLER)(hState.pDrvPrivate);
    if (s && s->pState) { ID3D11SamplerState_Release(s->pState); s->pState = NULL; }
}

void APIENTRY
tritonPsSetSamplers(D3D10DDI_HDEVICE hDevice, UINT Start, UINT NumSamplers,
                    const D3D10DDI_HSAMPLER *phSamplers)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    ID3D11SamplerState *aS[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    if (NumSamplers > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) return;
    for (UINT i = 0; i < NumSamplers; ++i) {
        PTRITON_SAMPLER s = phSamplers
            ? (PTRITON_SAMPLER)(phSamplers[i].pDrvPrivate)
            : NULL;
        aS[i] = s ? s->pState : NULL;
    }
    ID3D11DeviceContext1_PSSetSamplers(pD->pCtx1, Start, NumSamplers, aS);
}

void APIENTRY
tritonVsSetSamplers(D3D10DDI_HDEVICE hDevice, UINT Start, UINT NumSamplers,
                    const D3D10DDI_HSAMPLER *phSamplers)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    ID3D11SamplerState *aS[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    if (NumSamplers > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) return;
    for (UINT i = 0; i < NumSamplers; ++i) {
        PTRITON_SAMPLER s = phSamplers
            ? (PTRITON_SAMPLER)(phSamplers[i].pDrvPrivate)
            : NULL;
        aS[i] = s ? s->pState : NULL;
    }
    ID3D11DeviceContext1_VSSetSamplers(pD->pCtx1, Start, NumSamplers, aS);
}

#define TR_SAMPLER_SETTER(stage, ctx_method)                                              \
void APIENTRY                                                                  \
triton ## stage ## SetSamplers(D3D10DDI_HDEVICE hDevice, UINT Start, UINT N,              \
                               const D3D10DDI_HSAMPLER *ph)                               \
{                                                                                         \
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);            \
    if (!pD) return;                                                                      \
    ID3D11SamplerState *a[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};                    \
    if (N > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) return;                                \
    for (UINT i = 0; i < N; ++i) {                                                        \
        PTRITON_SAMPLER s = ph ? (PTRITON_SAMPLER)(ph[i].pDrvPrivate) : NULL; \
        a[i] = s ? s->pState : NULL;                                                      \
    }                                                                                     \
    ID3D11DeviceContext1_##ctx_method(pD->pCtx1, Start, N, a);                            \
}
TR_SAMPLER_SETTER(Gs, GSSetSamplers)
TR_SAMPLER_SETTER(Hs, HSSetSamplers)
TR_SAMPLER_SETTER(Ds, DSSetSamplers)
TR_SAMPLER_SETTER(Cs, CSSetSamplers)
#undef TR_SAMPLER_SETTER

/* ---------- Element layout (lazy ID3D11InputLayout creation) ---------- */

SIZE_T APIENTRY
tritonCalcPrivateElementLayoutSize(D3D10DDI_HDEVICE hDev, const D3D10DDIARG_CREATEELEMENTLAYOUT *pArgs)
{
    return sizeof(TRITON_ELEMENTLAYOUT);
}

void APIENTRY
tritonCreateElementLayout(D3D10DDI_HDEVICE hDevice,
                          const D3D10DDIARG_CREATEELEMENTLAYOUT *pArgs,
                          D3D10DDI_HELEMENTLAYOUT hLayout,
                          D3D10DDI_HRTELEMENTLAYOUT hRTLayout)
{
    PTRITON_DEVICE        pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_ELEMENTLAYOUT e  = (PTRITON_ELEMENTLAYOUT)(hLayout.pDrvPrivate);
    if (!e) return;
    e->NumElements     = pArgs->NumElements;
    e->pLayout         = NULL;
    e->LayoutVsCookie  = 0;
    e->pReconVS        = NULL;
    /* The runtime may free pArgs->pVertexElements after this returns. */
    const SIZE_T cb = pArgs->NumElements * sizeof(D3D10DDIARG_INPUT_ELEMENT_DESC);
    e->pElements = (D3D10DDIARG_INPUT_ELEMENT_DESC *)(
        HeapAlloc(GetProcessHeap(), 0, cb));
    if (e->pElements) {
        memcpy(e->pElements, pArgs->pVertexElements, cb);
    } else {
        e->NumElements = 0;
        tritonSetError(pD, E_OUTOFMEMORY);
    }
}

void tritonResolveInputLayout(PTRITON_DEVICE pD);
void tritonUnbindReconVs(PTRITON_DEVICE pD);

void APIENTRY
tritonDestroyElementLayout(D3D10DDI_HDEVICE hDevice, D3D10DDI_HELEMENTLAYOUT hLayout)
{
    PTRITON_DEVICE        pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_ELEMENTLAYOUT e  = (PTRITON_ELEMENTLAYOUT)(hLayout.pDrvPrivate);
    if (!e) return;
    if (pD && pD->pCurrentLayout == e) pD->pCurrentLayout = NULL;
    if (e->pLayout)   { ID3D11InputLayout_Release(e->pLayout); e->pLayout = NULL; }
    if (e->pReconVS)  { ID3D11VertexShader_Release(e->pReconVS); e->pReconVS = NULL; }
    if (e->pElements) { HeapFree(GetProcessHeap(), 0, e->pElements); e->pElements = NULL; }
    e->NumElements      = 0;
    e->LayoutVsCookie   = 0;
}

void APIENTRY
tritonIaSetInputLayout(D3D10DDI_HDEVICE hDevice, D3D10DDI_HELEMENTLAYOUT hLayout)
{
    PTRITON_DEVICE        pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_ELEMENTLAYOUT e  = (PTRITON_ELEMENTLAYOUT)(hLayout.pDrvPrivate);
    if (!pD) return;
    pD->pCurrentLayout = e;
    /* Layout unbound: propagate immediately so a previous layout stays
     * unbound; the resolver below early-returns on NULL. */
    if (!e) {
        ID3D11DeviceContext1_IASetInputLayout(pD->pCtx1, NULL);
        tritonUnbindReconVs(pD);
        return;
    }
    /* Resolve eagerly if a VS is already bound; otherwise the next
     * tritonVsSetShader will trigger the resolver. */
    if (pD->pCurrentVS) {
        tritonResolveInputLayout(pD);
    } else {
        ID3D11DeviceContext1_IASetInputLayout(pD->pCtx1, NULL);
        tritonUnbindReconVs(pD);
    }
}

/* ---------- Viewports / scissors / topology ---------- */

void APIENTRY
tritonSetViewports(D3D10DDI_HDEVICE hDevice, UINT NumViewports, UINT ClearViewports,
                   const D3D10_DDI_VIEWPORT *pViewports)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    (void)ClearViewports;
    D3D11_VIEWPORT a[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    if (NumViewports > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) return;
    for (UINT i = 0; i < NumViewports; ++i) {
        a[i].TopLeftX = pViewports[i].TopLeftX;
        a[i].TopLeftY = pViewports[i].TopLeftY;
        a[i].Width    = pViewports[i].Width;
        a[i].Height   = pViewports[i].Height;
        a[i].MinDepth = pViewports[i].MinDepth;
        a[i].MaxDepth = pViewports[i].MaxDepth;
    }
    ID3D11DeviceContext1_RSSetViewports(pD->pCtx1, NumViewports, a);
}

void APIENTRY
tritonSetScissorRects(D3D10DDI_HDEVICE hDevice, UINT NumRects, UINT ClearRects,
                      const D3D10_DDI_RECT *pRects)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    (void)ClearRects;
    D3D11_RECT a[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    if (NumRects > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) return;
    for (UINT i = 0; i < NumRects; ++i) {
        a[i].left   = pRects[i].left;
        a[i].top    = pRects[i].top;
        a[i].right  = pRects[i].right;
        a[i].bottom = pRects[i].bottom;
    }
    ID3D11DeviceContext1_RSSetScissorRects(pD->pCtx1, NumRects, a);
}

void APIENTRY
tritonIaSetTopology(D3D10DDI_HDEVICE hDevice, D3D10_DDI_PRIMITIVE_TOPOLOGY Topology)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    /* The runtime may pass UNDEFINED during teardown; D3D11 rejects it. */
    if (Topology == D3D10_DDI_PRIMITIVE_TOPOLOGY_UNDEFINED) return;
    ID3D11DeviceContext1_IASetPrimitiveTopology(pD->pCtx1, (D3D11_PRIMITIVE_TOPOLOGY)Topology);
}

/* ---------- IA vertex/index buffer binding ---------- */

void APIENTRY
tritonIaSetVertexBuffers(D3D10DDI_HDEVICE hDevice, UINT StartSlot, UINT NumBuffers,
                         const D3D10DDI_HRESOURCE *phBuffers,
                         const UINT *pStrides, const UINT *pOffsets)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    ID3D11Buffer *aBuf[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    if (NumBuffers > D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) return;
    for (UINT i = 0; i < NumBuffers; ++i) {
        PTRITON_RESOURCE r = phBuffers
            ? (PTRITON_RESOURCE)(phBuffers[i].pDrvPrivate)
            : NULL;
        aBuf[i] = r ? (ID3D11Buffer *)r->pResource : NULL;
    }
    ID3D11DeviceContext1_IASetVertexBuffers(
        pD->pCtx1, StartSlot, NumBuffers, aBuf, pStrides, pOffsets);
}

void APIENTRY
tritonIaSetIndexBuffer(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hBuffer,
                       DXGI_FORMAT Format, UINT Offset)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hBuffer.pDrvPrivate);
    if (!pD) return;
    ID3D11DeviceContext1_IASetIndexBuffer(
        pD->pCtx1, r ? (ID3D11Buffer *)r->pResource : NULL, Format, Offset);
}

/* ---------- Draw + dispatch ---------- */

void APIENTRY
tritonDraw(D3D10DDI_HDEVICE hDevice, UINT VertexCount, UINT StartVertexLocation)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    ID3D11DeviceContext1_Draw(pD->pCtx1, VertexCount, StartVertexLocation);
}

void APIENTRY
tritonDrawIndexed(D3D10DDI_HDEVICE hDevice, UINT IndexCount,
                  UINT StartIndexLocation, INT BaseVertexLocation)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    ID3D11DeviceContext1_DrawIndexed(pD->pCtx1, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void APIENTRY
tritonDrawInstanced(D3D10DDI_HDEVICE hDevice, UINT VertexCountPerInstance,
                    UINT InstanceCount, UINT StartVertexLocation,
                    UINT StartInstanceLocation)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    ID3D11DeviceContext1_DrawInstanced(pD->pCtx1, VertexCountPerInstance, InstanceCount,
                                       StartVertexLocation, StartInstanceLocation);
}

void APIENTRY
tritonDrawIndexedInstanced(D3D10DDI_HDEVICE hDevice, UINT IndexCountPerInstance,
                           UINT InstanceCount, UINT StartIndexLocation,
                           INT BaseVertexLocation, UINT StartInstanceLocation)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    ID3D11DeviceContext1_DrawIndexedInstanced(pD->pCtx1, IndexCountPerInstance, InstanceCount,
                                              StartIndexLocation, BaseVertexLocation,
                                              StartInstanceLocation);
}

void APIENTRY
tritonDrawAuto(D3D10DDI_HDEVICE hDevice)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    ID3D11DeviceContext1_DrawAuto(pD->pCtx1);
}

void APIENTRY
tritonDrawInstancedIndirect(D3D10DDI_HDEVICE hDevice,
                            D3D10DDI_HRESOURCE hBuf, UINT Offset)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hBuf.pDrvPrivate);
    if (!pD || !r || !r->pResource) return;
    ID3D11DeviceContext1_DrawInstancedIndirect(
        pD->pCtx1, (ID3D11Buffer *)r->pResource, Offset);
}

void APIENTRY
tritonDrawIndexedInstancedIndirect(D3D10DDI_HDEVICE hDevice,
                                   D3D10DDI_HRESOURCE hBuf, UINT Offset)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hBuf.pDrvPrivate);
    if (!pD || !r || !r->pResource) return;
    ID3D11DeviceContext1_DrawIndexedInstancedIndirect(
        pD->pCtx1, (ID3D11Buffer *)r->pResource, Offset);
}

void APIENTRY
tritonDispatch(D3D10DDI_HDEVICE hDevice, UINT X, UINT Y, UINT Z)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    ID3D11DeviceContext1_Dispatch(pD->pCtx1, X, Y, Z);
}

void APIENTRY
tritonDispatchIndirect(D3D10DDI_HDEVICE hDevice,
                       D3D10DDI_HRESOURCE hBuf, UINT Offset)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hBuf.pDrvPrivate);
    if (!pD || !r || !r->pResource) return;
    ID3D11DeviceContext1_DispatchIndirect(
        pD->pCtx1, (ID3D11Buffer *)r->pResource, Offset);
}

/* ---------- Stream output ---------- */

void APIENTRY
tritonSoSetTargets(D3D10DDI_HDEVICE hDevice, UINT NumBuffers, UINT ClearTargets,
                   const D3D10DDI_HRESOURCE *phBuffers, const UINT *pOffsets)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    /* ClearTargets is a hint: how many trailing slots beyond NumBuffers
     * the runtime wants unbound. The handle/offset arrays hold only
     * NumBuffers entries (_In_reads_(NumBuffers)), and SOSetTargets
     * NULLs every slot past the count we pass, so bind exactly
     * NumBuffers. */
    (void)ClearTargets;
    if (NumBuffers > D3D11_SO_BUFFER_SLOT_COUNT) return;
    ID3D11Buffer *aBuf[D3D11_SO_BUFFER_SLOT_COUNT] = {};
    UINT aOff[D3D11_SO_BUFFER_SLOT_COUNT] = {};
    for (UINT i = 0; i < NumBuffers; ++i) {
        PTRITON_RESOURCE r = phBuffers
            ? (PTRITON_RESOURCE)(phBuffers[i].pDrvPrivate)
            : NULL;
        aBuf[i] = r ? (ID3D11Buffer *)r->pResource : NULL;
        aOff[i] = pOffsets ? pOffsets[i] : 0;
    }
    ID3D11DeviceContext1_SOSetTargets(pD->pCtx1, NumBuffers, aBuf, aOff);
}

void APIENTRY
tritonSetResourceMinLOD(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hRes, FLOAT MinLOD)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hRes.pDrvPrivate);
    if (!pD || !r || !r->pResource) return;
    ID3D11DeviceContext1_SetResourceMinLOD(pD->pCtx1, r->pResource, MinLOD);
}
