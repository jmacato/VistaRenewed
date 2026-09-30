/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D9 fixed pipeline state for programmable draws.  The Vista UMD keeps the
 * D3D9 values as its source of truth, then builds matching D3D11 objects only
 * when a draw needs them.  This lets callbacks reject unsupported state before
 * the runtime observes a successful draw with different behavior.
 */

#include "triton9.h"
#include "triton9_cpu_layout.h"
#include "../triton/tritonSharedBridge.h"

#include <cstring>
#include <cmath>
#include <climits>

namespace {

static const UINT kDefaultColorWriteMask =
    D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
    D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA;

/* d3dumddi.h deliberately omits these two legacy DDI-only render-state
 * values, but the Vista D3D9 runtime still sends them while seeding a HAL
 * state block.  Their only default is D3DINFINITEINSTRUCTIONS (UINT_MAX). */
static const UINT kD3dDdiMaxVertexShaderInstructions = 196u;
static const UINT kD3dDdiMaxPixelShaderInstructions = 197u;
static const UINT kD3dInfiniteInstructions = 0xffffffffu;

static FLOAT
triton9FloatFromBits(UINT value)
{
    FLOAT result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

static bool
triton9FiniteFloatBits(UINT value)
{
    return (value & 0x7f800000u) != 0x7f800000u;
}

static UINT
triton9SamplerIndex(UINT stage)
{
    /* The DDI keeps the public 256/257..260 displacement/vertex indices. */
    if (stage == D3DDMAPSAMPLER)
        return TRITON9_MAX_PIXEL_SAMPLERS;
    if (stage >= D3DVERTEXTEXTURESAMPLER0 &&
        stage < D3DVERTEXTEXTURESAMPLER0 + TRITON9_MAX_VERTEX_SAMPLERS)
        return TRITON9_VERTEX_SAMPLER_BASE + stage - D3DVERTEXTEXTURESAMPLER0;
    return stage < TRITON9_MAX_PIXEL_SAMPLERS ? stage : UINT_MAX;
}

static bool
triton9MapBlend(UINT value, D3D11_BLEND *result)
{
    if (!result)
        return false;
    switch (value) {
    case D3DBLEND_ZERO:         *result = D3D11_BLEND_ZERO; return true;
    case D3DBLEND_ONE:          *result = D3D11_BLEND_ONE; return true;
    case D3DBLEND_SRCCOLOR:     *result = D3D11_BLEND_SRC_COLOR; return true;
    case D3DBLEND_INVSRCCOLOR:  *result = D3D11_BLEND_INV_SRC_COLOR; return true;
    case D3DBLEND_SRCALPHA:     *result = D3D11_BLEND_SRC_ALPHA; return true;
    case D3DBLEND_INVSRCALPHA:  *result = D3D11_BLEND_INV_SRC_ALPHA; return true;
    case D3DBLEND_DESTALPHA:    *result = D3D11_BLEND_DEST_ALPHA; return true;
    case D3DBLEND_INVDESTALPHA: *result = D3D11_BLEND_INV_DEST_ALPHA; return true;
    case D3DBLEND_DESTCOLOR:    *result = D3D11_BLEND_DEST_COLOR; return true;
    case D3DBLEND_INVDESTCOLOR: *result = D3D11_BLEND_INV_DEST_COLOR; return true;
    case D3DBLEND_SRCALPHASAT:  *result = D3D11_BLEND_SRC_ALPHA_SAT; return true;
    case D3DBLEND_BLENDFACTOR:  *result = D3D11_BLEND_BLEND_FACTOR; return true;
    case D3DBLEND_INVBLENDFACTOR:
        *result = D3D11_BLEND_INV_BLEND_FACTOR;
        return true;
    default:
        return false;
    }
}

static bool
triton9MapBlendOp(UINT value, D3D11_BLEND_OP *result)
{
    if (!result)
        return false;
    switch (value) {
    case D3DBLENDOP_ADD:          *result = D3D11_BLEND_OP_ADD; return true;
    case D3DBLENDOP_SUBTRACT:     *result = D3D11_BLEND_OP_SUBTRACT; return true;
    case D3DBLENDOP_REVSUBTRACT:  *result = D3D11_BLEND_OP_REV_SUBTRACT; return true;
    case D3DBLENDOP_MIN:          *result = D3D11_BLEND_OP_MIN; return true;
    case D3DBLENDOP_MAX:          *result = D3D11_BLEND_OP_MAX; return true;
    default:
        return false;
    }
}

static bool
triton9MapComparison(UINT value, D3D11_COMPARISON_FUNC *result)
{
    if (!result)
        return false;
    switch (value) {
    case D3DCMP_NEVER:        *result = D3D11_COMPARISON_NEVER; return true;
    case D3DCMP_LESS:         *result = D3D11_COMPARISON_LESS; return true;
    case D3DCMP_EQUAL:        *result = D3D11_COMPARISON_EQUAL; return true;
    case D3DCMP_LESSEQUAL:    *result = D3D11_COMPARISON_LESS_EQUAL; return true;
    case D3DCMP_GREATER:      *result = D3D11_COMPARISON_GREATER; return true;
    case D3DCMP_NOTEQUAL:     *result = D3D11_COMPARISON_NOT_EQUAL; return true;
    case D3DCMP_GREATEREQUAL: *result = D3D11_COMPARISON_GREATER_EQUAL; return true;
    case D3DCMP_ALWAYS:       *result = D3D11_COMPARISON_ALWAYS; return true;
    default:
        return false;
    }
}

/* Vista writes the complete two-sided stencil state block while constructing
 * a D3D9Ex device.  Validate every encoding that the conditional D24S8 path
 * can execute. */
static bool
triton9ValidStencilOperation(UINT value)
{
    switch (value) {
    case D3DSTENCILOP_KEEP:
    case D3DSTENCILOP_ZERO:
    case D3DSTENCILOP_REPLACE:
    case D3DSTENCILOP_INCRSAT:
    case D3DSTENCILOP_DECRSAT:
    case D3DSTENCILOP_INVERT:
    case D3DSTENCILOP_INCR:
    case D3DSTENCILOP_DECR:
        return true;
    default:
        return false;
    }
}

static bool
triton9ValidFogMode(UINT value)
{
    return value == D3DFOG_NONE || value == D3DFOG_EXP ||
           value == D3DFOG_EXP2 || value == D3DFOG_LINEAR;
}

static bool
triton9MapStencilOperation(UINT value, D3D11_STENCIL_OP *result)
{
    if (!result)
        return false;
    switch (value) {
    case D3DSTENCILOP_KEEP:    *result = D3D11_STENCIL_OP_KEEP; return true;
    case D3DSTENCILOP_ZERO:    *result = D3D11_STENCIL_OP_ZERO; return true;
    case D3DSTENCILOP_REPLACE: *result = D3D11_STENCIL_OP_REPLACE; return true;
    case D3DSTENCILOP_INCRSAT: *result = D3D11_STENCIL_OP_INCR_SAT; return true;
    case D3DSTENCILOP_DECRSAT: *result = D3D11_STENCIL_OP_DECR_SAT; return true;
    case D3DSTENCILOP_INVERT:  *result = D3D11_STENCIL_OP_INVERT; return true;
    case D3DSTENCILOP_INCR:    *result = D3D11_STENCIL_OP_INCR; return true;
    case D3DSTENCILOP_DECR:    *result = D3D11_STENCIL_OP_DECR; return true;
    default:
        return false;
    }
}

static bool
triton9MapAddress(UINT value, D3D11_TEXTURE_ADDRESS_MODE *result)
{
    if (!result)
        return false;
    switch (value) {
    case D3DTADDRESS_WRAP:       *result = D3D11_TEXTURE_ADDRESS_WRAP; return true;
    case D3DTADDRESS_MIRROR:     *result = D3D11_TEXTURE_ADDRESS_MIRROR; return true;
    case D3DTADDRESS_CLAMP:      *result = D3D11_TEXTURE_ADDRESS_CLAMP; return true;
    case D3DTADDRESS_BORDER:     *result = D3D11_TEXTURE_ADDRESS_BORDER; return true;
    case D3DTADDRESS_MIRRORONCE: *result = D3D11_TEXTURE_ADDRESS_MIRROR_ONCE; return true;
    default:
        return false;
    }
}

static bool
triton9ValidMinMagFilter(UINT value)
{
    return value == D3DTEXF_POINT || value == D3DTEXF_LINEAR ||
           value == D3DTEXF_ANISOTROPIC;
}

static bool
triton9ValidMipFilter(UINT value)
{
    return value == D3DTEXF_NONE || value == D3DTEXF_POINT ||
           value == D3DTEXF_LINEAR;
}

static bool
triton9ValidFixedOperation(UINT value)
{
    return value >= D3DTOP_DISABLE && value <= D3DTOP_LERP;
}

static bool
triton9ValidFixedArgument(UINT value)
{
    if (value & ~(D3DTA_SELECTMASK | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE))
        return false;
    switch (value & D3DTA_SELECTMASK) {
    case D3DTA_DIFFUSE:
    case D3DTA_SPECULAR:
    case D3DTA_CURRENT:
    case D3DTA_TEXTURE:
    case D3DTA_TFACTOR:
    case D3DTA_TEMP:
    case D3DTA_CONSTANT:
        return true;
    default:
        return false;
    }
}

static D3D11_FILTER
triton9MapFilter(UINT minFilter, UINT magFilter, UINT mipFilter)
{
    if (minFilter == D3DTEXF_ANISOTROPIC || magFilter == D3DTEXF_ANISOTROPIC)
        return D3D11_FILTER_ANISOTROPIC;

    const bool minLinear = minFilter == D3DTEXF_LINEAR;
    const bool magLinear = magFilter == D3DTEXF_LINEAR;
    const bool mipLinear = mipFilter == D3DTEXF_LINEAR;
    if (minLinear && magLinear && mipLinear)
        return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (minLinear && magLinear)
        return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    if (minLinear && mipLinear)
        return D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    if (minLinear)
        return D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    if (magLinear && mipLinear)
        return D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    if (magLinear)
        return D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    if (mipLinear)
        return D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    return D3D11_FILTER_MIN_MAG_MIP_POINT;
}

static void
triton9ColorToFloat(UINT color, FLOAT output[4])
{
    output[0] = (FLOAT)((color >> 16) & 0xffu) / 255.0f;
    output[1] = (FLOAT)((color >> 8) & 0xffu) / 255.0f;
    output[2] = (FLOAT)(color & 0xffu) / 255.0f;
    output[3] = (FLOAT)((color >> 24) & 0xffu) / 255.0f;
}

static void
triton9IdentityMatrix(D3DMATRIX *matrix)
{
    if (!matrix)
        return;
    ZeroMemory(matrix, sizeof(*matrix));
    for (UINT index = 0; index < 4; ++index)
        matrix->m[index][index] = 1.0f;
}

static D3DMATRIX *
triton9TransformSlot(TRITON9_DEVICE *device, D3DTRANSFORMSTATETYPE type)
{
    if (!device)
        return nullptr;
    if ((UINT)type == (UINT)D3DTS_WORLD)
        return &device->worldTransform;
    if ((UINT)type > (UINT)D3DTS_WORLD && (UINT)type < (UINT)D3DTS_WORLD + 256)
        return &device->worldTransforms[(UINT)type - (UINT)D3DTS_WORLD];
    if ((UINT)type >= (UINT)D3DTS_TEXTURE0 &&
        (UINT)type < (UINT)D3DTS_TEXTURE0 + TRITON9_FIXED_TEXTURE_STAGES)
        return &device->textureTransforms[(UINT)type - (UINT)D3DTS_TEXTURE0];
    switch (type) {
    case D3DTS_VIEW:
        return &device->viewTransform;
    case D3DTS_PROJECTION:
        return &device->projectionTransform;
    default:
        return nullptr;
    }
}

static void
triton9MultiplyMatrices(const D3DMATRIX &left, const D3DMATRIX &right,
                        D3DMATRIX *result)
{
    D3DMATRIX value = {};

    for (UINT row = 0; row < 4; ++row)
        for (UINT column = 0; column < 4; ++column)
            for (UINT index = 0; index < 4; ++index)
                value.m[row][column] += left.m[row][index] * right.m[index][column];
    *result = value;
}

/* D3D9 applies the RGBA factor's alpha component when separate alpha
 * blending is disabled. D3D11 spells those scalar factors explicitly and
 * rejects *_COLOR in its alpha slots. */
static D3D11_BLEND
triton9AlphaBlendFactor(D3D11_BLEND factor)
{
    switch (factor) {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    case D3D11_BLEND_SRC_ALPHA_SAT: return D3D11_BLEND_ONE;
    default: return factor;
    }
}

/* X formats have logical destination alpha one even when the host storage
 * contains writable alpha bits. Substitute factors independently for each RT. */
static D3D11_BLEND
triton9OpaqueDestinationBlend(D3D11_BLEND factor)
{
    switch (factor) {
    case D3D11_BLEND_DEST_ALPHA: return D3D11_BLEND_ONE;
    case D3D11_BLEND_INV_DEST_ALPHA:
    case D3D11_BLEND_SRC_ALPHA_SAT: return D3D11_BLEND_ZERO;
    default: return factor;
    }
}

static bool
triton9OpaqueRenderTarget(const TRITON9_RESOURCE *resource)
{
    if (!resource)
        return false;
    switch (UINT(resource->format)) {
    case D3DFMT_X8R8G8B8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_R5G6B5:
        return true;
    default:
        return false;
    }
}

static HRESULT
triton9CreateBlendState(TRITON9_DEVICE *device)
{
    D3D11_BLEND_DESC desc;
    D3D11_RENDER_TARGET_BLEND_DESC *target;
    D3D11_BLEND srcColor, dstColor, srcAlpha, dstAlpha;
    D3D11_BLEND_OP colorOp, alphaOp;
    ID3D11BlendState *newState = nullptr;
    HRESULT hr;

    if (!device)
        return E_INVALIDARG;
    if (!device->blendStateDirty && device->blendState)
        return S_OK;
    const UINT sourceBlend = device->renderStates[D3DDDIRS_SRCBLEND];
    if (sourceBlend == D3DBLEND_BOTHSRCALPHA ||
        sourceBlend == D3DBLEND_BOTHINVSRCALPHA) {
        /* Legacy source modes override the destination color factor. */
        srcColor = sourceBlend == D3DBLEND_BOTHSRCALPHA
            ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_INV_SRC_ALPHA;
        dstColor = sourceBlend == D3DBLEND_BOTHSRCALPHA
            ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_SRC_ALPHA;
    } else if (!triton9MapBlend(sourceBlend, &srcColor) ||
               !triton9MapBlend(device->renderStates[D3DDDIRS_DESTBLEND],
                                &dstColor)) {
        return D3DDDIERR_NOTAVAILABLE;
    }
    if (!triton9MapBlendOp(device->renderStates[D3DDDIRS_BLENDOP], &colorOp))
        return D3DDDIERR_NOTAVAILABLE;
    if (device->renderStates[D3DDDIRS_SEPARATEALPHABLENDENABLE]) {
        if (!triton9MapBlend(device->renderStates[D3DDDIRS_SRCBLENDALPHA],
                             &srcAlpha) ||
            !triton9MapBlend(device->renderStates[D3DDDIRS_DESTBLENDALPHA],
                             &dstAlpha) ||
            !triton9MapBlendOp(device->renderStates[D3DDDIRS_BLENDOPALPHA],
                                &alphaOp))
            return D3DDDIERR_NOTAVAILABLE;
    } else {
        srcAlpha = srcColor;
        dstAlpha = dstColor;
        alphaOp = colorOp;
    }

    ZeroMemory(&desc, sizeof(desc));
    target = &desc.RenderTarget[0];
    target->BlendEnable = device->renderStates[D3DDDIRS_ALPHABLENDENABLE] != 0;
    target->SrcBlend = srcColor;
    target->DestBlend = dstColor;
    target->BlendOp = colorOp;
    target->SrcBlendAlpha = triton9AlphaBlendFactor(srcAlpha);
    target->DestBlendAlpha = triton9AlphaBlendFactor(dstAlpha);
    target->BlendOpAlpha = alphaOp;
    target->RenderTargetWriteMask = (UINT8)(
        device->renderStates[D3DDDIRS_COLORWRITEENABLE] & kDefaultColorWriteMask);
    desc.IndependentBlendEnable = TRUE;
    for (UINT index = 1; index < 4; ++index) {
        desc.RenderTarget[index] = *target;
        desc.RenderTarget[index].RenderTargetWriteMask = (UINT8)(
            device->renderStates[D3DDDIRS_COLORWRITEENABLE1 + index - 1] &
            kDefaultColorWriteMask);
    }
    for (UINT index = 0; index < 4; ++index) {
        if (!triton9OpaqueRenderTarget(device->renderTargets[index]))
            continue;
        auto &output = desc.RenderTarget[index];
        output.SrcBlend = triton9OpaqueDestinationBlend(output.SrcBlend);
        output.DestBlend = triton9OpaqueDestinationBlend(output.DestBlend);
        output.SrcBlendAlpha = triton9OpaqueDestinationBlend(output.SrcBlendAlpha);
        output.DestBlendAlpha = triton9OpaqueDestinationBlend(output.DestBlendAlpha);
    }
    hr = device->hostDevice->CreateBlendState(&desc, &newState);
    if (FAILED(hr) || !newState) {
        if (newState)
            newState->Release();
        return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    }
    if (device->blendState)
        device->blendState->Release();
    device->blendState = newState;
    device->blendStateDirty = FALSE;
    return S_OK;
}

static HRESULT
triton9CreateDepthStencilState(TRITON9_DEVICE *device)
{
    D3D11_DEPTH_STENCIL_DESC desc;
    D3D11_COMPARISON_FUNC comparison;
    D3D11_STENCIL_OP stencilFail;
    D3D11_STENCIL_OP stencilZFail;
    D3D11_STENCIL_OP stencilPass;
    D3D11_COMPARISON_FUNC stencilFunc;
    ID3D11DepthStencilState *newState = nullptr;
    HRESULT hr;

    if (!device)
        return E_INVALIDARG;
    if (!device->depthStencilStateDirty && device->depthStencilState)
        return S_OK;
    if (!triton9MapStencilOperation(device->renderStates[D3DDDIRS_STENCILFAIL],
                                    &stencilFail) ||
        !triton9MapStencilOperation(device->renderStates[D3DDDIRS_STENCILZFAIL],
                                    &stencilZFail) ||
        !triton9MapStencilOperation(device->renderStates[D3DDDIRS_STENCILPASS],
                                    &stencilPass) ||
        !triton9MapComparison(device->renderStates[D3DDDIRS_STENCILFUNC],
                               &stencilFunc))
        return D3DDDIERR_NOTAVAILABLE;
    if (!triton9MapComparison(device->renderStates[D3DDDIRS_ZFUNC], &comparison))
        return D3DDDIERR_NOTAVAILABLE;
    ZeroMemory(&desc, sizeof(desc));
    desc.DepthEnable = device->renderStates[D3DDDIRS_ZENABLE] == D3DZB_TRUE;
    desc.DepthWriteMask = device->renderStates[D3DDDIRS_ZWRITEENABLE]
        ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = comparison;
    /* D24X8 shares a host D24S8 image, but its padding is not stencil.
     * Binding a different logical depth format invalidates this state. */
    desc.StencilEnable = device->renderStates[D3DDDIRS_STENCILENABLE] != 0 &&
        device->depthStencil && device->depthStencil->format == D3DDDIFMT_D24S8;
    desc.StencilReadMask = (UINT8)device->renderStates[D3DDDIRS_STENCILMASK];
    desc.StencilWriteMask =
        (UINT8)device->renderStates[D3DDDIRS_STENCILWRITEMASK];
    desc.FrontFace.StencilFailOp = stencilFail;
    desc.FrontFace.StencilDepthFailOp = stencilZFail;
    desc.FrontFace.StencilPassOp = stencilPass;
    desc.FrontFace.StencilFunc = stencilFunc;
    if (device->renderStates[D3DDDIRS_TWOSIDEDSTENCILMODE]) {
        if (!triton9MapStencilOperation(
                device->renderStates[D3DDDIRS_CCW_STENCILFAIL], &stencilFail) ||
            !triton9MapStencilOperation(
                device->renderStates[D3DDDIRS_CCW_STENCILZFAIL], &stencilZFail) ||
            !triton9MapStencilOperation(
                device->renderStates[D3DDDIRS_CCW_STENCILPASS], &stencilPass) ||
            !triton9MapComparison(device->renderStates[D3DDDIRS_CCW_STENCILFUNC],
                                   &stencilFunc))
            return D3DDDIERR_NOTAVAILABLE;
    }
    desc.BackFace.StencilFailOp = stencilFail;
    desc.BackFace.StencilDepthFailOp = stencilZFail;
    desc.BackFace.StencilPassOp = stencilPass;
    desc.BackFace.StencilFunc = stencilFunc;
    hr = device->hostDevice->CreateDepthStencilState(&desc, &newState);
    if (FAILED(hr) || !newState) {
        if (newState)
            newState->Release();
        return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    }
    if (device->depthStencilState)
        device->depthStencilState->Release();
    device->depthStencilState = newState;
    device->depthStencilStateDirty = FALSE;
    return S_OK;
}

static HRESULT
triton9CreateRasterizerState(TRITON9_DEVICE *device)
{
    D3D11_RASTERIZER_DESC desc;
    UINT fillMode;
    UINT cullMode;
    ID3D11RasterizerState *newState = nullptr;
    HRESULT hr;

    if (!device)
        return E_INVALIDARG;
    if (!device->rasterizerStateDirty && device->rasterizerState)
        return S_OK;
    fillMode = device->renderStates[D3DDDIRS_FILLMODE];
    cullMode = device->renderStates[D3DDDIRS_CULLMODE];
    if (fillMode != D3DFILL_POINT && fillMode != D3DFILL_SOLID &&
        fillMode != D3DFILL_WIREFRAME)
        return D3DDDIERR_NOTAVAILABLE;
    if (cullMode != D3DCULL_NONE && cullMode != D3DCULL_CW &&
        cullMode != D3DCULL_CCW)
        return D3DDDIERR_NOTAVAILABLE;
    ZeroMemory(&desc, sizeof(desc));
    desc.FillMode = fillMode == D3DFILL_WIREFRAME
        ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    if (cullMode == D3DCULL_NONE) {
        desc.CullMode = D3D11_CULL_NONE;
    } else if (cullMode == D3DCULL_CW) {
        desc.CullMode = D3D11_CULL_FRONT;
    } else {
        desc.CullMode = D3D11_CULL_BACK;
    }
    /* D3D9 keeps clockwise faces in the front-face stencil lane.  Keep that
     * identity stable when culling changes, so CCW stencil state always maps
     * to D3D11 BackFace. */
    desc.FrontCounterClockwise = FALSE;
    desc.DepthClipEnable = device->renderStates[D3DDDIRS_CLIPPING] != 0;
    const UINT depthBits = device->depthStencil &&
        (device->depthStencil->format == D3DDDIFMT_D16 ||
         device->depthStencil->format == D3DDDIFMT_D16_LOCKABLE) ? 16 : 24;
    const double bias = std::ldexp(static_cast<double>(triton9FloatFromBits(
        device->renderStates[D3DDDIRS_DEPTHBIAS])), depthBits);
    desc.DepthBias = bias >= INT_MAX ? INT_MAX : bias <= INT_MIN ? INT_MIN :
        static_cast<INT>(std::round(bias));
    desc.SlopeScaledDepthBias = triton9FloatFromBits(
        device->renderStates[D3DDDIRS_SLOPESCALEDEPTHBIAS]);
    desc.ScissorEnable = device->renderStates[D3DDDIRS_SCISSORTESTENABLE] != 0;
    desc.MultisampleEnable = device->renderStates[D3DDDIRS_MULTISAMPLEANTIALIAS] != 0;
    desc.AntialiasedLineEnable = device->renderStates[D3DDDIRS_ANTIALIASEDLINEENABLE] != 0;
    hr = device->hostDevice->CreateRasterizerState(&desc, &newState);
    if (FAILED(hr) || !newState) {
        if (newState)
            newState->Release();
        return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    }
    if (device->rasterizerState)
        device->rasterizerState->Release();
    device->rasterizerState = newState;
    device->rasterizerStateDirty = FALSE;
    return S_OK;
}

static HRESULT
triton9CreateSamplerState(TRITON9_DEVICE *device, UINT stage)
{
    const UINT *states;
    D3D11_SAMPLER_DESC desc;
    ID3D11SamplerState *newState = nullptr;
    HRESULT hr;

    if (!device || stage >= TRITON9_MAX_TEXTURE_STAGES)
        return E_INVALIDARG;
    if (!device->samplerStatesDirty[stage] && device->samplerStates[stage])
        return S_OK;
    states = device->textureStageStates[stage];
    if (!triton9ValidMinMagFilter(states[D3DDDITSS_MINFILTER]) ||
        !triton9ValidMinMagFilter(states[D3DDDITSS_MAGFILTER]) ||
        !triton9ValidMipFilter(states[D3DDDITSS_MIPFILTER]) ||
        !triton9FiniteFloatBits(states[D3DDDITSS_MIPMAPLODBIAS]) ||
        !states[D3DDDITSS_MAXANISOTROPY] ||
        states[D3DDDITSS_MAXANISOTROPY] > 16)
        return D3DDDIERR_NOTAVAILABLE;
    ZeroMemory(&desc, sizeof(desc));
    if (!triton9MapAddress(states[D3DDDITSS_ADDRESSU], &desc.AddressU) ||
        !triton9MapAddress(states[D3DDDITSS_ADDRESSV], &desc.AddressV) ||
        !triton9MapAddress(states[D3DDDITSS_ADDRESSW], &desc.AddressW))
        return D3DDDIERR_NOTAVAILABLE;
    desc.Filter = triton9MapFilter(states[D3DDDITSS_MINFILTER],
                                   states[D3DDDITSS_MAGFILTER],
                                   states[D3DDDITSS_MIPFILTER]);
    desc.MipLODBias = triton9FloatFromBits(states[D3DDDITSS_MIPMAPLODBIAS]);
    desc.MaxAnisotropy = states[D3DDDITSS_MAXANISOTROPY];
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    triton9ColorToFloat(states[D3DDDITSS_BORDERCOLOR], desc.BorderColor);
    desc.MinLOD = (FLOAT)states[D3DDDITSS_MAXMIPLEVEL];
    desc.MaxLOD = states[D3DDDITSS_MIPFILTER] == D3DTEXF_NONE
        ? desc.MinLOD : D3D11_FLOAT32_MAX;
    hr = device->hostDevice->CreateSamplerState(&desc, &newState);
    if (FAILED(hr) || !newState) {
        if (newState)
            newState->Release();
        return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    }
    if (device->samplerStates[stage])
        device->samplerStates[stage]->Release();
    device->samplerStates[stage] = newState;
    device->samplerStatesDirty[stage] = FALSE;
    return S_OK;
}

static HRESULT
triton9ValidateViewport(const TRITON9_DEVICE *device, D3D11_VIEWPORT *viewport,
                        RECT *scissor)
{
    UINT64 right;
    UINT64 bottom;

    if (!device || !device->renderTarget || !viewport || !scissor)
        return E_INVALIDARG;
    if (!device->viewportSet) {
        viewport->TopLeftX = 0.0f;
        viewport->TopLeftY = 0.0f;
        viewport->Width = (FLOAT)device->renderTarget->width;
        viewport->Height = (FLOAT)device->renderTarget->height;
        viewport->MinDepth = device->zRangeSet ? device->viewport.MinDepth : 0.0f;
        viewport->MaxDepth = device->zRangeSet ? device->viewport.MaxDepth : 1.0f;
    } else {
        *viewport = device->viewport;
    }
    if (viewport->Width <= 0.0f || viewport->Height <= 0.0f ||
        viewport->MinDepth < 0.0f || viewport->MinDepth > viewport->MaxDepth ||
        viewport->MaxDepth > 1.0f || viewport->TopLeftX < 0.0f ||
        viewport->TopLeftY < 0.0f)
        return D3DDDIERR_INVALIDCALL;
    right = (UINT64)viewport->TopLeftX + (UINT64)viewport->Width;
    bottom = (UINT64)viewport->TopLeftY + (UINT64)viewport->Height;
    if (right > device->renderTarget->width || bottom > device->renderTarget->height)
        return D3DDDIERR_INVALIDCALL;
    if (!device->scissorSet) {
        scissor->left = 0;
        scissor->top = 0;
        scissor->right = (LONG)device->renderTarget->width;
        scissor->bottom = (LONG)device->renderTarget->height;
    } else {
        *scissor = device->scissorRect;
    }
    if (scissor->left < 0 || scissor->top < 0 || scissor->right < scissor->left ||
        scissor->bottom < scissor->top || (UINT)scissor->right > device->renderTarget->width ||
        (UINT)scissor->bottom > device->renderTarget->height)
        return D3DDDIERR_INVALIDCALL;
    return S_OK;
}

static HRESULT
triton9ValidateRenderState(const D3DDDIARG_RENDERSTATE *args)
{
    if (!args || (UINT)args->State >= TRITON9_RENDER_STATE_COUNT)
        return E_INVALIDARG;
    if ((UINT)args->State == kD3dDdiMaxVertexShaderInstructions ||
        (UINT)args->State == kD3dDdiMaxPixelShaderInstructions) {
        /* Instruction limits are a runtime debugging facility, not a shader
         * execution capability.  Triton honors Vista's unlimited default as
         * preserved-only state and rejects finite limits it cannot enforce. */
        return args->Value == kD3dInfiniteInstructions ? S_OK
                                                        : D3DDDIERR_NOTAVAILABLE;
    }
    switch (args->State) {
    case D3DDDIRS_ZENABLE:
        return args->Value == D3DZB_FALSE || args->Value == D3DZB_TRUE
            ? S_OK : D3DDDIERR_NOTAVAILABLE;
    case D3DDDIRS_FILLMODE:
        return args->Value == D3DFILL_POINT || args->Value == D3DFILL_SOLID ||
               args->Value == D3DFILL_WIREFRAME
            ? S_OK : D3DDDIERR_NOTAVAILABLE;
    case D3DDDIRS_SHADEMODE:
        return args->Value == D3DSHADE_GOURAUD || args->Value == D3DSHADE_FLAT
            ? S_OK : D3DDDIERR_NOTAVAILABLE;
    /*
     * Vista sends the complete legacy D3D9 state block while it constructs
     * a HAL device, including several D3D7-era controls that a D3D11-backed
     * compositor will never consume.  Rejecting their neutral defaults turns
     * an otherwise valid CreateDeviceEx into the runtime's unhelpful E_FAIL.
     *
     * This is deliberately an acceptance/preservation lane, not a claim that
     * Triton rasterizes lines, patches, or legacy visibility queries.  It
     * mirrors the important Vista-era VirtualBox DDI property: every legal
     * D3DDDI render-state callback reaches the device layer, while the states
     * that affect our D3D11 pipeline remain validated and mapped below.
     */
    case D3DDDIRS_LINEPATTERN:
    case D3DDDIRS_ZVISIBLE:
    case D3DDDIRS_OLDALPHABLENDENABLE:
    case D3DDDIRS_ZBIAS:
    case D3DDDIRS_TRANSLUCENTSORTINDEPENDENT:
    case D3DDDIRS_STIPPLEPATTERN00:
    case D3DDDIRS_STIPPLEPATTERN01:
    case D3DDDIRS_STIPPLEPATTERN02:
    case D3DDDIRS_STIPPLEPATTERN03:
    case D3DDDIRS_STIPPLEPATTERN04:
    case D3DDDIRS_STIPPLEPATTERN05:
    case D3DDDIRS_STIPPLEPATTERN06:
    case D3DDDIRS_STIPPLEPATTERN07:
    case D3DDDIRS_STIPPLEPATTERN08:
    case D3DDDIRS_STIPPLEPATTERN09:
    case D3DDDIRS_STIPPLEPATTERN10:
    case D3DDDIRS_STIPPLEPATTERN11:
    case D3DDDIRS_STIPPLEPATTERN12:
    case D3DDDIRS_STIPPLEPATTERN13:
    case D3DDDIRS_STIPPLEPATTERN14:
    case D3DDDIRS_STIPPLEPATTERN15:
    case D3DDDIRS_STIPPLEPATTERN16:
    case D3DDDIRS_STIPPLEPATTERN17:
    case D3DDDIRS_STIPPLEPATTERN18:
    case D3DDDIRS_STIPPLEPATTERN19:
    case D3DDDIRS_STIPPLEPATTERN20:
    case D3DDDIRS_STIPPLEPATTERN21:
    case D3DDDIRS_STIPPLEPATTERN22:
    case D3DDDIRS_STIPPLEPATTERN23:
    case D3DDDIRS_STIPPLEPATTERN24:
    case D3DDDIRS_STIPPLEPATTERN25:
    case D3DDDIRS_STIPPLEPATTERN26:
    case D3DDDIRS_STIPPLEPATTERN27:
    case D3DDDIRS_STIPPLEPATTERN28:
    case D3DDDIRS_STIPPLEPATTERN29:
    case D3DDDIRS_STIPPLEPATTERN30:
    case D3DDDIRS_STIPPLEPATTERN31:
    case D3DDDIRS_PATCHEDGESTYLE:
    case D3DDDIRS_PATCHSEGMENTS:
    case D3DDDIRS_DEBUGMONITORTOKEN:
    case D3DDDIRS_DELETERTPATCH:
    case D3DDDIRS_POSITIONDEGREE:
    case D3DDDIRS_NORMALDEGREE:
    case D3DDDIRS_MINTESSELLATIONLEVEL:
    case D3DDDIRS_MAXTESSELLATIONLEVEL:
    case D3DDDIRS_ADAPTIVETESS_X:
    case D3DDDIRS_ADAPTIVETESS_Y:
    case D3DDDIRS_ADAPTIVETESS_Z:
    case D3DDDIRS_ADAPTIVETESS_W:
    case D3DDDIRS_ENABLEADAPTIVETESSELLATION:
        return S_OK;
    case D3DDDIRS_ZWRITEENABLE:
    case D3DDDIRS_ALPHABLENDENABLE:
    case D3DDDIRS_SEPARATEALPHABLENDENABLE:
    case D3DDDIRS_SCISSORTESTENABLE:
    case D3DDDIRS_SRGBWRITEENABLE:
    case D3DDDIRS_ANTIALIASEDLINEENABLE:
    case D3DDDIRS_CLIPPING:
        return args->Value <= 1 ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_SRCBLEND:
        if (args->Value == D3DBLEND_BOTHSRCALPHA ||
            args->Value == D3DBLEND_BOTHINVSRCALPHA)
            return S_OK;
        /* fall through */
    case D3DDDIRS_DESTBLEND:
    case D3DDDIRS_SRCBLENDALPHA:
    case D3DDDIRS_DESTBLENDALPHA: {
        D3D11_BLEND ignored;
        return triton9MapBlend(args->Value, &ignored) ? S_OK : D3DDDIERR_NOTAVAILABLE;
    }
    case D3DDDIRS_BLENDOP:
    case D3DDDIRS_BLENDOPALPHA: {
        D3D11_BLEND_OP ignored;
        return triton9MapBlendOp(args->Value, &ignored) ? S_OK : D3DDDIERR_NOTAVAILABLE;
    }
    case D3DDDIRS_ZFUNC: {
        D3D11_COMPARISON_FUNC ignored;
        return triton9MapComparison(args->Value, &ignored) ? S_OK : D3DDDIERR_NOTAVAILABLE;
    }
    case D3DDDIRS_CULLMODE:
        return args->Value == D3DCULL_NONE || args->Value == D3DCULL_CW ||
               args->Value == D3DCULL_CCW ? S_OK : D3DDDIERR_NOTAVAILABLE;
    case D3DDDIRS_COLORWRITEENABLE:
        return (args->Value & ~kDefaultColorWriteMask) == 0 ? S_OK
                                                             : D3DDDIERR_NOTAVAILABLE;
    case D3DDDIRS_BLENDFACTOR:
    case D3DDDIRS_ALPHAREF:
    case D3DDDIRS_TEXTUREFACTOR:
        return S_OK;
    case D3DDDIRS_ALPHATESTENABLE:
        return args->Value <= 1 ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_ALPHAFUNC: {
        UINT ignored;
        switch (args->Value) {
        case D3DCMP_NEVER: case D3DCMP_LESS: case D3DCMP_EQUAL:
        case D3DCMP_LESSEQUAL: case D3DCMP_GREATER: case D3DCMP_NOTEQUAL:
        case D3DCMP_GREATEREQUAL: case D3DCMP_ALWAYS:
            ignored = args->Value;
            return ignored ? S_OK : D3DDDIERR_NOTAVAILABLE;
        default:
            return D3DDDIERR_NOTAVAILABLE;
        }
    }
    case D3DDDIRS_FOGENABLE:
        /* The fixed-function shader path consumes this preserved value at
         * draw time.  No D3D11 rasterizer object represents vertex fog. */
        return args->Value <= 1 ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_DITHERENABLE:
        /* Preserve the logical state; draw-time application checks the
         * backend's native dithering support. */
        return args->Value <= 1 ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_COLORKEYENABLE:
    case D3DDDIRS_COLORKEYBLENDENABLE:
        return args->Value == 0 ? S_OK : D3DDDIERR_NOTAVAILABLE;
    case D3DDDIRS_STENCILENABLE:
        return args->Value <= 1 ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_SCENECAPTURE:
        /* This is a BeginScene/EndScene marker. It has no raster effect. */
        return S_OK;
    case D3DDDIRS_LASTPIXEL:
        /* MIL's canonical hardware state explicitly disables last-pixel line
         * rasterization.  Triton's composition path does not draw lines, so
         * both legal D3D9 values are state-only and must be accepted. */
        return args->Value <= 1 ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_SPECULARENABLE:
    case D3DDDIRS_RANGEFOGENABLE:
    case D3DDDIRS_NORMALIZENORMALS:
    case D3DDDIRS_INDEXEDVERTEXBLENDENABLE:
    case D3DDDIRS_POINTSPRITEENABLE:
    case D3DDDIRS_POINTSCALEENABLE:
        return args->Value <= 1 ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_EDGEANTIALIAS:
        return args->Value == FALSE ? S_OK : D3DDDIERR_NOTAVAILABLE;
    case D3DDDIRS_LIGHTING:
    case D3DDDIRS_COLORVERTEX:
    case D3DDDIRS_LOCALVIEWER:
    case D3DDDIRS_SOFTWAREVERTEXPROCESSING:
    case D3DDDIRS_MULTISAMPLEANTIALIAS:
        return args->Value <= 1 ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_FOGTABLEMODE:
    case D3DDDIRS_FOGVERTEXMODE:
        return triton9ValidFogMode(args->Value) ? S_OK
                                                : D3DDDIERR_NOTAVAILABLE;
    /* These four values are parameter payloads, not enum values.  Vista's
     * public D3D9 runtime initializes them while constructing every HAL
     * device, including when both fog modes are NONE.  They must therefore
     * be accepted and preserved for the draw-time vertex-fog shader path.
     * Treating zero FOGCOLOR as a stencil opcode makes CreateDeviceEx fail
     * before it reaches any draw callback. */
    case D3DDDIRS_FOGCOLOR:
    case D3DDDIRS_FOGSTART:
    case D3DDDIRS_FOGEND:
    case D3DDDIRS_FOGDENSITY:
        return S_OK;
    case D3DDDIRS_STENCILFAIL:
    case D3DDDIRS_STENCILZFAIL:
    case D3DDDIRS_STENCILPASS:
        return triton9ValidStencilOperation(args->Value) ? S_OK
                                                          : D3DDDIERR_NOTAVAILABLE;
    case D3DDDIRS_STENCILREF:
    case D3DDDIRS_STENCILMASK:
    case D3DDDIRS_STENCILWRITEMASK:
        return S_OK;
    case D3DDDIRS_STENCILFUNC: {
        D3D11_COMPARISON_FUNC ignored;
        return triton9MapComparison(args->Value, &ignored) ? S_OK
                                                            : D3DDDIERR_NOTAVAILABLE;
    }
    case D3DDDIRS_TWOSIDEDSTENCILMODE:
        return args->Value <= 1 ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_CCW_STENCILFAIL:
    case D3DDDIRS_CCW_STENCILZFAIL:
    case D3DDDIRS_CCW_STENCILPASS:
        return triton9ValidStencilOperation(args->Value) ? S_OK
                                                          : D3DDDIERR_NOTAVAILABLE;
    case D3DDDIRS_CCW_STENCILFUNC: {
        D3D11_COMPARISON_FUNC ignored;
        return triton9MapComparison(args->Value, &ignored) ? S_OK
                                                            : D3DDDIERR_NOTAVAILABLE;
    }
    case D3DDDIRS_COLORWRITEENABLE1:
    case D3DDDIRS_COLORWRITEENABLE2:
    case D3DDDIRS_COLORWRITEENABLE3:
        return (args->Value & ~kDefaultColorWriteMask) == 0 ? S_OK
                                                             : D3DDDIERR_NOTAVAILABLE;
    case D3DDDIRS_DIFFUSEMATERIALSOURCE:
    case D3DDDIRS_SPECULARMATERIALSOURCE:
    case D3DDDIRS_EMISSIVEMATERIALSOURCE:
    case D3DDDIRS_AMBIENTMATERIALSOURCE:
        return args->Value <= D3DMCS_COLOR2 ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_AMBIENT:
        return S_OK;
    case D3DDDIRS_CLIPPLANEENABLE:
        return (args->Value & ~63u) == 0 ? S_OK : D3DDDIERR_NOTAVAILABLE;
    case D3DDDIRS_VERTEXBLEND:
        return args->Value <= D3DVBF_3WEIGHTS ||
               args->Value == D3DVBF_0WEIGHTS || args->Value == D3DVBF_TWEENING
            ? S_OK : D3DDDIERR_NOTAVAILABLE;
    case D3DDDIRS_POINTSIZE:
    case D3DDDIRS_POINTSIZE_MIN:
    case D3DDDIRS_POINTSIZE_MAX:
        /* Zero remains valid for Vista's device-limit initialization sentinel. */
        return triton9FiniteFloatBits(args->Value) &&
               triton9FloatFromBits(args->Value) >= 0.0f
            ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_POINTSCALE_A:
    case D3DDDIRS_POINTSCALE_B:
    case D3DDDIRS_POINTSCALE_C:
    case D3DDDIRS_TWEENFACTOR:
        return triton9FiniteFloatBits(args->Value) ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_WRAP0:
    case D3DDDIRS_WRAP1:
    case D3DDDIRS_WRAP2:
    case D3DDDIRS_WRAP3:
    case D3DDDIRS_WRAP4:
    case D3DDDIRS_WRAP5:
    case D3DDDIRS_WRAP6:
    case D3DDDIRS_WRAP7:
    case D3DDDIRS_WRAP8:
    case D3DDDIRS_WRAP9:
    case D3DDDIRS_WRAP10:
    case D3DDDIRS_WRAP11:
    case D3DDDIRS_WRAP12:
    case D3DDDIRS_WRAP13:
    case D3DDDIRS_WRAP14:
    case D3DDDIRS_WRAP15:
        return (args->Value & ~15u) == 0 ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_DEPTHBIAS:
    case D3DDDIRS_SLOPESCALEDEPTHBIAS:
        return triton9FiniteFloatBits(args->Value) ? S_OK : D3DDDIERR_INVALIDCALL;
    case D3DDDIRS_MULTISAMPLEMASK:
        return S_OK;
    default:
        return D3DDDIERR_NOTAVAILABLE;
    }
}

/* Vista reduces a rejected initialization-state callback to the same public
 * CreateDeviceEx E_FAIL it uses for unrelated setup failures. Keep a
 * state-only flight recorder at this boundary; it must never acquire the
 * renderer proxy. */
static HRESULT
triton9RejectTextureStageState(const D3DDDIARG_TEXTURESTAGESTATE *args,
                               HRESULT hr)
{
    if (args) {
        triton9DiagU32("TRITON9-TSS-REJECT-STAGE", args->Stage);
        triton9DiagU32("TRITON9-TSS-REJECT-STATE", args->State);
        triton9DiagU32("TRITON9-TSS-REJECT-VALUE", args->Value);
    }
    triton9DiagU32("TRITON9-TSS-REJECT-HR", (DWORD)hr);
    return hr;
}

} /* namespace */

extern "C" void
triton9InitializePipelineState(TRITON9_DEVICE *device)
{
    UINT stage;

    if (!device)
        return;
    device->renderStates[D3DDDIRS_ZENABLE] = D3DZB_TRUE;
    device->renderStates[D3DDDIRS_FILLMODE] = D3DFILL_SOLID;
    device->renderStates[D3DDDIRS_SHADEMODE] = D3DSHADE_GOURAUD;
    device->renderStates[D3DDDIRS_ZWRITEENABLE] = TRUE;
    device->renderStates[D3DDDIRS_SRCBLEND] = D3DBLEND_ONE;
    device->renderStates[D3DDDIRS_DESTBLEND] = D3DBLEND_ZERO;
    device->renderStates[D3DDDIRS_CULLMODE] = D3DCULL_CCW;
    device->renderStates[D3DDDIRS_ZFUNC] = D3DCMP_LESSEQUAL;
    device->renderStates[D3DDDIRS_STENCILENABLE] = FALSE;
    device->renderStates[D3DDDIRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
    device->renderStates[D3DDDIRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    device->renderStates[D3DDDIRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    device->renderStates[D3DDDIRS_STENCILFUNC] = D3DCMP_ALWAYS;
    device->renderStates[D3DDDIRS_STENCILREF] = 0;
    device->renderStates[D3DDDIRS_STENCILMASK] = 0xffffffffu;
    device->renderStates[D3DDDIRS_STENCILWRITEMASK] = 0xffffffffu;
    device->renderStates[D3DDDIRS_TWOSIDEDSTENCILMODE] = FALSE;
    device->renderStates[D3DDDIRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
    device->renderStates[D3DDDIRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    device->renderStates[D3DDDIRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    device->renderStates[D3DDDIRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    device->renderStates[D3DDDIRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    device->renderStates[D3DDDIRS_COLORWRITEENABLE] = kDefaultColorWriteMask;
    device->renderStates[D3DDDIRS_COLORWRITEENABLE1] = kDefaultColorWriteMask;
    device->renderStates[D3DDDIRS_COLORWRITEENABLE2] = kDefaultColorWriteMask;
    device->renderStates[D3DDDIRS_COLORWRITEENABLE3] = kDefaultColorWriteMask;
    device->renderStates[D3DDDIRS_MULTISAMPLEANTIALIAS] = TRUE;
    device->renderStates[D3DDDIRS_BLENDOP] = D3DBLENDOP_ADD;
    device->renderStates[D3DDDIRS_MULTISAMPLEMASK] = 0xffffffffu;
    device->renderStates[D3DDDIRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    device->renderStates[D3DDDIRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    device->renderStates[D3DDDIRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    device->renderStates[D3DDDIRS_BLENDFACTOR] = 0xffffffffu;
    device->renderStates[D3DDDIRS_TEXTUREFACTOR] = 0xffffffffu;
    device->renderStates[D3DDDIRS_FOGENABLE] = FALSE;
    device->renderStates[D3DDDIRS_FOGCOLOR] = 0;
    device->renderStates[D3DDDIRS_FOGTABLEMODE] = D3DFOG_NONE;
    device->renderStates[D3DDDIRS_FOGSTART] = 0;
    device->renderStates[D3DDDIRS_FOGEND] = 0x3f800000u;
    device->renderStates[D3DDDIRS_FOGDENSITY] = 0x3f800000u;
    device->renderStates[D3DDDIRS_RANGEFOGENABLE] = FALSE;
    device->renderStates[D3DDDIRS_FOGVERTEXMODE] = D3DFOG_NONE;
    device->renderStates[D3DDDIRS_DITHERENABLE] = FALSE;
    device->renderStates[D3DDDIRS_LASTPIXEL] = TRUE;
    device->renderStates[D3DDDIRS_CLIPPING] = TRUE;
    device->renderStates[D3DDDIRS_LIGHTING] = TRUE;
    device->renderStates[D3DDDIRS_COLORVERTEX] = TRUE;
    device->renderStates[D3DDDIRS_LOCALVIEWER] = TRUE;
    device->renderStates[D3DDDIRS_POINTSIZE] = 0x3f800000u;
    device->renderStates[D3DDDIRS_POINTSIZE_MIN] = 0x3f800000u;
    device->renderStates[D3DDDIRS_POINTSIZE_MAX] = 0x42800000u; /* 64 */
    device->renderStates[D3DDDIRS_POINTSCALE_A] = 0x3f800000u;
    device->renderStates[D3DDDIRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    device->renderStates[D3DDDIRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    device->blendStateDirty = TRUE;
    device->depthStencilStateDirty = TRUE;
    device->rasterizerStateDirty = TRUE;
    triton9IdentityMatrix(&device->worldTransform);
    for (UINT index = 0; index < 256; ++index)
        triton9IdentityMatrix(&device->worldTransforms[index]);
    triton9IdentityMatrix(&device->viewTransform);
    triton9IdentityMatrix(&device->projectionTransform);
    for (stage = 0; stage < TRITON9_FIXED_TEXTURE_STAGES; ++stage)
        triton9IdentityMatrix(&device->textureTransforms[stage]);
    for (stage = 0; stage < TRITON9_MAX_TEXTURE_STAGES; ++stage) {
        UINT *states = device->textureStageStates[stage];
        states[D3DDDITSS_COLOROP] = stage == 0 ? D3DTOP_MODULATE
                                               : D3DTOP_DISABLE;
        states[D3DDDITSS_ALPHAOP] = stage == 0 ? D3DTOP_SELECTARG1
                                               : D3DTOP_DISABLE;
        states[D3DDDITSS_COLORARG1] = D3DTA_TEXTURE;
        states[D3DDDITSS_COLORARG2] = D3DTA_CURRENT;
        states[D3DDDITSS_ALPHAARG1] = D3DTA_TEXTURE;
        states[D3DDDITSS_ALPHAARG2] = D3DTA_CURRENT;
        states[D3DDDITSS_TEXCOORDINDEX] = stage;
        states[D3DDDITSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
        states[D3DDDITSS_RESULTARG] = D3DTA_CURRENT;
        states[D3DDDITSS_ADDRESSU] = D3DTADDRESS_WRAP;
        states[D3DDDITSS_ADDRESSV] = D3DTADDRESS_WRAP;
        states[D3DDDITSS_ADDRESSW] = D3DTADDRESS_WRAP;
        states[D3DDDITSS_MAGFILTER] = D3DTEXF_LINEAR;
        states[D3DDDITSS_MINFILTER] = D3DTEXF_LINEAR;
        states[D3DDDITSS_MIPFILTER] = D3DTEXF_POINT;
        states[D3DDDITSS_MAXANISOTROPY] = 1;
        device->samplerStatesDirty[stage] = TRUE;
    }
}

extern "C" void
triton9ReleasePipelineState(TRITON9_DEVICE *device)
{
    UINT stage;

    if (!device)
        return;
    for (stage = 0; stage < TRITON9_MAX_TEXTURE_STAGES; ++stage) {
        if (device->samplerStates[stage]) {
            device->samplerStates[stage]->Release();
            device->samplerStates[stage] = nullptr;
        }
        device->textures[stage] = nullptr;
    }
    if (device->blendState) {
        device->blendState->Release();
        device->blendState = nullptr;
    }
    if (device->depthStencilState) {
        device->depthStencilState->Release();
        device->depthStencilState = nullptr;
    }
    if (device->rasterizerState) {
        device->rasterizerState->Release();
        device->rasterizerState = nullptr;
    }
}

extern "C" HRESULT APIENTRY
triton9SetRenderState(HANDLE hDevice, const D3DDDIARG_RENDERSTATE *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    HRESULT hr;

    if (!device || !args || !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    hr = triton9ValidateRenderState(args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-RS-REJECT-STATE", args->State);
        triton9DiagU32("TRITON9-RS-REJECT-VALUE", args->Value);
        triton9DiagU32("TRITON9-RS-REJECT-HR", (DWORD)hr);
        return hr;
    }
    EnterCriticalSection(&device->shaderLock);
    device->renderStates[args->State] = args->Value;
    switch (args->State) {
    case D3DDDIRS_SRCBLEND:
    case D3DDDIRS_DESTBLEND:
    case D3DDDIRS_ALPHABLENDENABLE:
    case D3DDDIRS_COLORWRITEENABLE:
    case D3DDDIRS_COLORWRITEENABLE1:
    case D3DDDIRS_COLORWRITEENABLE2:
    case D3DDDIRS_COLORWRITEENABLE3:
    case D3DDDIRS_BLENDOP:
    case D3DDDIRS_SEPARATEALPHABLENDENABLE:
    case D3DDDIRS_SRCBLENDALPHA:
    case D3DDDIRS_DESTBLENDALPHA:
    case D3DDDIRS_BLENDOPALPHA:
    case D3DDDIRS_BLENDFACTOR:
        device->blendStateDirty = TRUE;
        break;
    case D3DDDIRS_ZENABLE:
    case D3DDDIRS_ZWRITEENABLE:
    case D3DDDIRS_ZFUNC:
    case D3DDDIRS_STENCILENABLE:
    case D3DDDIRS_STENCILFAIL:
    case D3DDDIRS_STENCILZFAIL:
    case D3DDDIRS_STENCILPASS:
    case D3DDDIRS_STENCILFUNC:
    case D3DDDIRS_STENCILMASK:
    case D3DDDIRS_STENCILWRITEMASK:
    case D3DDDIRS_TWOSIDEDSTENCILMODE:
    case D3DDDIRS_CCW_STENCILFAIL:
    case D3DDDIRS_CCW_STENCILZFAIL:
    case D3DDDIRS_CCW_STENCILPASS:
    case D3DDDIRS_CCW_STENCILFUNC:
        device->depthStencilStateDirty = TRUE;
        break;
    case D3DDDIRS_FILLMODE:
    case D3DDDIRS_CULLMODE:
    case D3DDDIRS_SCISSORTESTENABLE:
    case D3DDDIRS_DEPTHBIAS:
    case D3DDDIRS_SLOPESCALEDEPTHBIAS:
    case D3DDDIRS_CLIPPING:
    case D3DDDIRS_MULTISAMPLEANTIALIAS:
    case D3DDDIRS_ANTIALIASEDLINEENABLE:
        device->rasterizerStateDirty = TRUE;
        break;
    default:
        break;
    }
    LeaveCriticalSection(&device->shaderLock);
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9SetTransform(HANDLE hDevice, const D3DDDIARG_SETTRANSFORM *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    D3DMATRIX *destination;

    if (!device || !args || !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    EnterCriticalSection(&device->shaderLock);
    destination = triton9TransformSlot(device, args->TransformType);
    if (destination)
        *destination = args->Matrix;
    LeaveCriticalSection(&device->shaderLock);
    return destination ? S_OK : D3DDDIERR_NOTAVAILABLE;
}

extern "C" HRESULT APIENTRY
triton9MultiplyTransform(HANDLE hDevice,
                         const D3DDDIARG_MULTIPLYTRANSFORM *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    D3DMATRIX *destination;
    D3DMATRIX current;

    if (!device || !args || !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    EnterCriticalSection(&device->shaderLock);
    destination = triton9TransformSlot(device, args->TransformType);
    if (destination) {
        current = *destination;
        triton9MultiplyMatrices(current, args->Matrix, destination);
    }
    LeaveCriticalSection(&device->shaderLock);
    return destination ? S_OK : D3DDDIERR_NOTAVAILABLE;
}

extern "C" HRESULT APIENTRY
triton9SetTextureStageState(HANDLE hDevice,
                            const D3DDDIARG_TEXTURESTAGESTATE *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    D3DDDIARG_TEXTURESTAGESTATE normalized;

    if (!args)
        return E_INVALIDARG;
    normalized = *args;
    normalized.Stage = triton9SamplerIndex(args->Stage);
    args = &normalized;
    if (!device || !device->shaderLockInitialized ||
        args->Stage >= TRITON9_MAX_TEXTURE_STAGES ||
        args->State >= TRITON9_TEXTURE_STAGE_STATE_COUNT)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    switch (args->State) {
    case D3DDDITSS_COLOROP:
    case D3DDDITSS_ALPHAOP:
        if (!triton9ValidFixedOperation(args->Value) ||
            (args->Stage >= TRITON9_FIXED_TEXTURE_STAGES && args->Value != D3DTOP_DISABLE))
            return triton9RejectTextureStageState(args, D3DDDIERR_NOTAVAILABLE);
        break;
    case D3DDDITSS_COLORARG0:
    case D3DDDITSS_ALPHAARG0:
    case D3DDDITSS_COLORARG1:
    case D3DDDITSS_COLORARG2:
    case D3DDDITSS_ALPHAARG1:
    case D3DDDITSS_ALPHAARG2:
        if (args->Stage >= TRITON9_FIXED_TEXTURE_STAGES || !triton9ValidFixedArgument(args->Value))
            return triton9RejectTextureStageState(args, D3DDDIERR_NOTAVAILABLE);
        break;
    case D3DDDITSS_TEXCOORDINDEX: {
        const UINT coordinates = args->Value & 0xffffu;
        const UINT generation = args->Value & 0xffff0000u;
        if (args->Stage >= TRITON9_FIXED_TEXTURE_STAGES) {
            if (args->Value != args->Stage)
                return triton9RejectTextureStageState(args, D3DDDIERR_NOTAVAILABLE);
        } else if (coordinates >= TRITON9_FIXED_TEXTURE_STAGES ||
                   generation > D3DTSS_TCI_SPHEREMAP) {
            return triton9RejectTextureStageState(args, D3DDDIERR_INVALIDCALL);
        }
        break;
    }
    case D3DDDITSS_TEXTURETRANSFORMFLAGS: {
        const UINT count = args->Value & ~D3DTTFF_PROJECTED;
        if (count > D3DTTFF_COUNT4 ||
            ((args->Value & D3DTTFF_PROJECTED) && count < D3DTTFF_COUNT2) ||
            (args->Stage >= TRITON9_FIXED_TEXTURE_STAGES && args->Value))
            return triton9RejectTextureStageState(args, D3DDDIERR_INVALIDCALL);
        break;
    }
    case D3DDDITSS_RESULTARG:
        if (args->Value != D3DTA_CURRENT && args->Value != D3DTA_TEMP)
            return triton9RejectTextureStageState(args, D3DDDIERR_INVALIDCALL);
        break;
    case D3DDDITSS_TEXTUREMAP:
        /* Resource binding uses pfnSetTexture; preserve this legacy state
         * token only for the runtime's initialization bookkeeping. */
        break;
    case D3DDDITSS_BUMPENVMAT00:
    case D3DDDITSS_BUMPENVMAT01:
    case D3DDDITSS_BUMPENVMAT10:
    case D3DDDITSS_BUMPENVMAT11:
    case D3DDDITSS_BUMPENVLSCALE:
    case D3DDDITSS_BUMPENVLOFFSET:
        if (!triton9FiniteFloatBits(args->Value))
            return triton9RejectTextureStageState(args, D3DDDIERR_INVALIDCALL);
        break;
    case D3DDDITSS_CONSTANT:
        break;
    case D3DDDITSS_ELEMENTINDEX:
    case D3DDDITSS_DMAPOFFSET:
        /* No displacement map is bound.  Vista supplies zero defaults for
         * every stage, including ones that Triton does not sample. */
        if (args->Value != 0)
            return triton9RejectTextureStageState(args, D3DDDIERR_NOTAVAILABLE);
        break;
    case D3DDDITSS_DISABLETEXTURECOLORKEY:
        if (args->Value > 1)
            return triton9RejectTextureStageState(args, D3DDDIERR_INVALIDCALL);
        break;
    case D3DDDITSS_TEXTURECOLORKEYVAL:
        /* Texture color keying is disabled by the matching state above. */
        break;
    case D3DDDITSS_BORDERCOLOR:
        break;
    case D3DDDITSS_ADDRESSU:
    case D3DDDITSS_ADDRESSV:
    case D3DDDITSS_ADDRESSW: {
        D3D11_TEXTURE_ADDRESS_MODE ignored;
        if (!triton9MapAddress(args->Value, &ignored))
            return triton9RejectTextureStageState(args, D3DDDIERR_NOTAVAILABLE);
        break;
    }
    case D3DDDITSS_MAGFILTER:
    case D3DDDITSS_MINFILTER:
        if (!triton9ValidMinMagFilter(args->Value))
            return triton9RejectTextureStageState(args, D3DDDIERR_NOTAVAILABLE);
        break;
    case D3DDDITSS_MIPFILTER:
        if (!triton9ValidMipFilter(args->Value))
            return triton9RejectTextureStageState(args, D3DDDIERR_NOTAVAILABLE);
        break;
    case D3DDDITSS_MIPMAPLODBIAS:
        if (!triton9FiniteFloatBits(args->Value))
            return triton9RejectTextureStageState(args, D3DDDIERR_INVALIDCALL);
        break;
    case D3DDDITSS_MAXANISOTROPY:
        if (!args->Value || args->Value > 16)
            return triton9RejectTextureStageState(args, D3DDDIERR_NOTAVAILABLE);
        break;
    case D3DDDITSS_MAXMIPLEVEL:
        break;
    case D3DDDITSS_SRGBTEXTURE:
        if (args->Value > 1)
            return triton9RejectTextureStageState(args, D3DDDIERR_INVALIDCALL);
        break;
    default:
        return triton9RejectTextureStageState(args, D3DDDIERR_NOTAVAILABLE);
    }
    EnterCriticalSection(&device->shaderLock);
    device->textureStageStates[args->Stage][args->State] = args->Value;
    switch (args->State) {
    case D3DDDITSS_ADDRESSU:
    case D3DDDITSS_ADDRESSV:
    case D3DDDITSS_ADDRESSW:
    case D3DDDITSS_BORDERCOLOR:
    case D3DDDITSS_MAGFILTER:
    case D3DDDITSS_MINFILTER:
    case D3DDDITSS_MIPFILTER:
    case D3DDDITSS_MIPMAPLODBIAS:
    case D3DDDITSS_MAXMIPLEVEL:
    case D3DDDITSS_MAXANISOTROPY:
        device->samplerStatesDirty[args->Stage] = TRUE;
        break;
    default:
        break;
    }
    LeaveCriticalSection(&device->shaderLock);
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9SetTexture(HANDLE hDevice, UINT stage, HANDLE resourceHandle)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    TRITON9_RESOURCE *resource = static_cast<TRITON9_RESOURCE *>(resourceHandle);
    HRESULT hr;

    stage = triton9SamplerIndex(stage);
    if (!device || !device->shaderLockInitialized ||
        stage >= TRITON9_MAX_TEXTURE_STAGES)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (resource) {
        if (stage == TRITON9_MAX_PIXEL_SAMPLERS)
            return D3DDDIERR_NOTAVAILABLE;
        if (!triton9ResourceBelongsToDevice(device, resource))
            return D3DDDIERR_INVALIDCALL;
        hr = triton9PrepareResourceForHostRead(device, resource);
        if (FAILED(hr))
            return hr;
        if (resource->isBuffer ||
            !(resource->hostBindFlags & D3D11_BIND_SHADER_RESOURCE))
            return D3DDDIERR_NOTAVAILABLE;
    }
    EnterCriticalSection(&device->shaderLock);
    /* Vista can submit the next pass's texture before its output binding.
     * Keep D3D9 state here; PreparePipelineState rejects actual feedback at
     * draw time, after all state callbacks have arrived. No SRV is bound to
     * the host by this setter. */
    device->textures[stage] = resource;
    LeaveCriticalSection(&device->shaderLock);
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9SetViewport(HANDLE hDevice, const D3DDDIARG_VIEWPORTINFO *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);

    if (!device || !args || !device->shaderLockInitialized || !args->Width ||
        !args->Height)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    EnterCriticalSection(&device->shaderLock);
    device->viewport.TopLeftX = (FLOAT)args->X;
    device->viewport.TopLeftY = (FLOAT)args->Y;
    device->viewport.Width = (FLOAT)args->Width;
    device->viewport.Height = (FLOAT)args->Height;
    if (!device->viewportSet && !device->zRangeSet) {
        device->viewport.MinDepth = 0.0f;
        device->viewport.MaxDepth = 1.0f;
    }
    device->viewportSet = TRUE;
    LeaveCriticalSection(&device->shaderLock);
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9SetZRange(HANDLE hDevice, const D3DDDIARG_ZRANGE *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);

    if (!device || !args || !device->shaderLockInitialized ||
        args->MinZ != args->MinZ || args->MaxZ != args->MaxZ || args->MinZ < 0.0f ||
        args->MinZ > args->MaxZ || args->MaxZ > 1.0f)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    EnterCriticalSection(&device->shaderLock);
    device->viewport.MinDepth = args->MinZ;
    device->viewport.MaxDepth = args->MaxZ;
    device->zRangeSet = TRUE;
    LeaveCriticalSection(&device->shaderLock);
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9SetScissorRect(HANDLE hDevice, const RECT *rect)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);

    if (!device || !rect || !device->shaderLockInitialized || rect->left < 0 ||
        rect->top < 0 || rect->right < rect->left || rect->bottom < rect->top)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    EnterCriticalSection(&device->shaderLock);
    device->scissorRect = *rect;
    device->scissorSet = TRUE;
    LeaveCriticalSection(&device->shaderLock);
    return S_OK;
}

extern "C" HRESULT
triton9PreparePipelineState(TRITON9_DEVICE *device)
{
    ID3D11ShaderResourceView *views[TRITON9_MAX_TEXTURE_STAGES];
    D3D11_VIEWPORT viewport;
    RECT scissor;
    FLOAT blendFactor[4];
    HRESULT hr;
    UINT stage;

    if (!device || !device->hostContext || !device->hostDevice ||
        !device->renderTarget)
        return E_INVALIDARG;
    /* Setters may arrive in any order while changing passes. Validate the
     * complete attachment set immediately before it can render. */
    for (UINT target = 1; target < 4; ++target) {
        const TRITON9_RESOURCE *resource = device->renderTargets[target];
        if (resource && (resource->width != device->renderTarget->width ||
                         resource->height != device->renderTarget->height ||
                         resource->sampleCount != device->renderTarget->sampleCount ||
                         resource->sampleQuality != device->renderTarget->sampleQuality))
            return D3DDDIERR_INVALIDCALL;
    }
    if (device->depthStencil &&
        (device->depthStencil->width < device->renderTarget->width ||
         device->depthStencil->height < device->renderTarget->height ||
         device->depthStencil->sampleCount != device->renderTarget->sampleCount ||
         device->depthStencil->sampleQuality != device->renderTarget->sampleQuality))
        return D3DDDIERR_INVALIDCALL;
    const BOOL ditherEnabled = device->renderStates[D3DDDIRS_DITHERENABLE] != 0;
    if (ditherEnabled != device->hostDitherEnabled) {
        hr = tritonSharedBridgeSetDither(device->hostContext, ditherEnabled);
        if (FAILED(hr))
            return triton9MapDeviceFailure(device, hr);
        device->hostDitherEnabled = ditherEnabled;
    }
    /* The shader bridge calls this while it owns shaderLock. */
    hr = triton9CreateBlendState(device);
    if (SUCCEEDED(hr))
        hr = triton9CreateDepthStencilState(device);
    if (SUCCEEDED(hr))
        hr = triton9CreateRasterizerState(device);
    if (FAILED(hr))
        return hr;
    for (stage = 0; stage < TRITON9_MAX_TEXTURE_STAGES; ++stage) {
        views[stage] = nullptr;
        if (stage == TRITON9_MAX_PIXEL_SAMPLERS)
            continue;
        if (device->textures[stage]) {
            bool writableAlias = triton9ResourcesShareBacking(
                device->textures[stage], device->depthStencil);
            for (UINT target = 0; target < 4; ++target) {
                writableAlias |= triton9ResourcesShareBacking(
                    device->textures[stage], device->renderTargets[target]);
            }
            /* D3D9 permits a bound, unused sampler to alias an output.
             * Hide the backend view for this draw, retaining logical state
             * so that a later pass restores the original texture binding. */
            if (writableAlias)
                continue;
            hr = triton9GetShaderResourceViewEx(device, device->textures[stage],
                device->textureStageStates[stage][D3DDDITSS_SRGBTEXTURE] != 0,
                &views[stage]);
            if (FAILED(hr))
                return hr;
        }
        hr = triton9CreateSamplerState(device, stage);
        if (FAILED(hr))
            return hr;
    }
    hr = triton9ValidateViewport(device, &viewport, &scissor);
    if (FAILED(hr))
        return hr;
    hr = triton9BindOutputs(device);
    if (FAILED(hr))
        return hr;
    triton9ColorToFloat(device->renderStates[D3DDDIRS_BLENDFACTOR], blendFactor);
    device->hostContext->OMSetBlendState(device->blendState, blendFactor,
        device->renderTarget->nonMaskable ? ~0u :
            device->renderStates[D3DDDIRS_MULTISAMPLEMASK]);
    device->hostContext->OMSetDepthStencilState(device->depthStencilState,
        device->renderStates[D3DDDIRS_STENCILREF]);
    device->hostContext->RSSetState(device->rasterizerState);
    device->hostContext->RSSetViewports(1, &viewport);
    device->hostContext->RSSetScissorRects(1, &scissor);
    device->hostContext->PSSetShaderResources(0, TRITON9_MAX_PIXEL_SAMPLERS, views);
    device->hostContext->PSSetSamplers(0, TRITON9_MAX_PIXEL_SAMPLERS,
                                       device->samplerStates);
    device->hostContext->VSSetShaderResources(0, TRITON9_MAX_VERTEX_SAMPLERS,
        views + TRITON9_VERTEX_SAMPLER_BASE);
    device->hostContext->VSSetSamplers(0, TRITON9_MAX_VERTEX_SAMPLERS,
        device->samplerStates + TRITON9_VERTEX_SAMPLER_BASE);
    return triton9CheckHostDevice(device);
}
