/*
 * Host-side regression for the exact fixed-function shader pair used by the
 * first public DrawPrimitiveUP probe.  This executes Microsoft's converter;
 * it does not merely inspect the legacy tokens.
 */

#include <d3d9.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>
#include "../triton9_fixed.h"

#ifndef D3DHAL_SAMPLER_MAXSAMP
#define D3DHAL_SAMPLER_MAXSAMP 16
#endif
#include <ShaderConv.h>
#include <ShaderValidation.h>

#include "../../triton/tritonDxbc.h"
#include "../../triton/tritonDxbcSignature.h"
#include "../triton9_shader_token_contract.h"

extern "C" uint32_t
npt_host_workaround_flags(void)
{
    return 0;
}

namespace {

template <typename T>
void releaseObject(T *&object)
{
    if (object)
        object->Release();
    object = nullptr;
}

UINT instruction(UINT opcode, UINT parameterCount)
{
    return opcode | (parameterCount << D3DSI_INSTLENGTH_SHIFT);
}

UINT registerType(D3DSHADER_PARAM_REGISTER_TYPE type)
{
    return ((UINT)type << D3DSP_REGTYPE_SHIFT & D3DSP_REGTYPE_MASK) |
           ((UINT)type << D3DSP_REGTYPE_SHIFT2 & D3DSP_REGTYPE_MASK2);
}

UINT destination(D3DSHADER_PARAM_REGISTER_TYPE type, UINT index,
                 UINT mask = D3DSP_WRITEMASK_ALL)
{
    return 0x80000000u | registerType(type) |
           (index & D3DSP_REGNUM_MASK) | mask;
}

UINT source(D3DSHADER_PARAM_REGISTER_TYPE type, UINT index,
            UINT swizzle = D3DSP_NOSWIZZLE)
{
    return 0x80000000u | registerType(type) |
           (index & D3DSP_REGNUM_MASK) | swizzle;
}

UINT negatedSource(D3DSHADER_PARAM_REGISTER_TYPE type, UINT index,
                   UINT swizzle = D3DSP_NOSWIZZLE)
{
    return source(type, index, swizzle) | D3DSPSM_NEG;
}

void declaration(std::vector<UINT> &tokens, UINT usage, UINT usageIndex,
                 D3DSHADER_PARAM_REGISTER_TYPE type, UINT index)
{
    tokens.push_back(instruction(D3DSIO_DCL, 2));
    tokens.push_back(triton9_sm2_dcl_semantic(usage, usageIndex));
    tokens.push_back(destination(type, index));
}

void constant(std::vector<UINT> &tokens, UINT index, UINT value)
{
    tokens.push_back(D3DSIO_DEF);
    tokens.push_back(destination(D3DSPR_CONST, index));
    tokens.insert(tokens.end(), 4, value);
}

void constant4(std::vector<UINT> &tokens, UINT index,
               UINT x, UINT y, UINT z, UINT w)
{
    /* This is the full VS 2.0 position form used by the Vista-era VBox
     * WDDM test: an explicitly sized DEF writes oPos.w from c0.x. */
    tokens.push_back(instruction(D3DSIO_DEF, 5));
    tokens.push_back(destination(D3DSPR_CONST, index));
    tokens.push_back(x);
    tokens.push_back(y);
    tokens.push_back(z);
    tokens.push_back(w);
}

std::vector<UINT> fixedVertexShader()
{
    std::vector<UINT> tokens;
    tokens.push_back(D3DVS_VERSION(2, 0));
    declaration(tokens, D3DDECLUSAGE_POSITION, 0, D3DSPR_INPUT, 0);
    declaration(tokens, D3DDECLUSAGE_COLOR, 0, D3DSPR_INPUT, 1);
    tokens.push_back(instruction(D3DSIO_M4x4, 3));
    tokens.push_back(destination(D3DSPR_RASTOUT, D3DSRO_POSITION));
    tokens.push_back(source(D3DSPR_INPUT, 0));
    tokens.push_back(source(D3DSPR_CONST, 0));
    tokens.push_back(instruction(D3DSIO_MOV, 2));
    tokens.push_back(destination(D3DSPR_ATTROUT, 0));
    tokens.push_back(source(D3DSPR_INPUT, 1));
    tokens.push_back(D3DVS_END());
    return tokens;
}

std::vector<UINT> fixedPixelShader()
{
    std::vector<UINT> tokens;
    tokens.push_back(D3DPS_VERSION(3, 0));
    declaration(tokens, D3DDECLUSAGE_COLOR, 0, D3DSPR_INPUT, 0);
    constant(tokens, 2, 0x3f800000u);
    constant(tokens, 3, 0xbf800000u);
    constant(tokens, 4, 0x00000000u);
    tokens.push_back(instruction(D3DSIO_MOV, 2));
    tokens.push_back(destination(D3DSPR_TEMP, 0));
    tokens.push_back(source(D3DSPR_INPUT, 0));
    tokens.push_back(instruction(D3DSIO_MOV, 2));
    tokens.push_back(destination(D3DSPR_TEMP, 0,
                                 D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                                 D3DSP_WRITEMASK_2));
    tokens.push_back(source(D3DSPR_INPUT, 0));
    tokens.push_back(instruction(D3DSIO_MOV, 2));
    tokens.push_back(destination(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_3));
    tokens.push_back(source(D3DSPR_INPUT, 0));
    tokens.push_back(instruction(D3DSIO_MOV, 2));
    tokens.push_back(destination(D3DSPR_COLOROUT, 0));
    tokens.push_back(source(D3DSPR_TEMP, 0));
    tokens.push_back(D3DPS_END());
    return tokens;
}

std::vector<UINT> fixedFogVertexShader(UINT fogMode)
{
    const UINT replicateX = D3DVS_X_X | D3DVS_Y_X | D3DVS_Z_X | D3DVS_W_X;
    const UINT replicateY = D3DVS_X_Y | D3DVS_Y_Y | D3DVS_Z_Y | D3DVS_W_Y;
    const UINT replicateZ = D3DVS_X_Z | D3DVS_Y_Z | D3DVS_Z_Z | D3DVS_W_Z;
    std::vector<UINT> tokens;

    tokens.push_back(D3DVS_VERSION(2, 0));
    declaration(tokens, D3DDECLUSAGE_POSITION, 0, D3DSPR_INPUT, 0);
    declaration(tokens, D3DDECLUSAGE_COLOR, 0, D3DSPR_INPUT, 1);
    tokens.push_back(instruction(D3DSIO_M4x4, 3));
    tokens.push_back(destination(D3DSPR_RASTOUT, D3DSRO_POSITION));
    tokens.push_back(source(D3DSPR_INPUT, 0));
    tokens.push_back(source(D3DSPR_CONST, 0));
    tokens.push_back(instruction(D3DSIO_M4x4, 3));
    tokens.push_back(destination(D3DSPR_TEMP, 0));
    tokens.push_back(source(D3DSPR_INPUT, 0));
    tokens.push_back(source(D3DSPR_CONST, 4));
    tokens.push_back(instruction(D3DSIO_ABS, 2));
    tokens.push_back(destination(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_0));
    tokens.push_back(source(D3DSPR_TEMP, 0, replicateZ));
    if (fogMode == D3DFOG_LINEAR) {
        tokens.push_back(instruction(D3DSIO_ADD, 3));
        tokens.push_back(destination(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_0));
        tokens.push_back(source(D3DSPR_CONST, 8, replicateY));
        tokens.push_back(negatedSource(D3DSPR_TEMP, 0, replicateX));
        tokens.push_back(instruction(D3DSIO_MUL, 3));
        tokens.push_back(destination(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_0));
        tokens.push_back(source(D3DSPR_TEMP, 0, replicateX));
        tokens.push_back(source(D3DSPR_CONST, 8, replicateZ));
    } else {
        if (fogMode == D3DFOG_EXP2) {
            tokens.push_back(instruction(D3DSIO_MUL, 3));
            tokens.push_back(destination(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_0));
            tokens.push_back(source(D3DSPR_TEMP, 0, replicateX));
            tokens.push_back(source(D3DSPR_TEMP, 0, replicateX));
        }
        tokens.push_back(instruction(D3DSIO_MUL, 3));
        tokens.push_back(destination(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_0));
        tokens.push_back(source(D3DSPR_TEMP, 0, replicateX));
        tokens.push_back(source(D3DSPR_CONST, 8, replicateX));
        tokens.push_back(instruction(D3DSIO_EXPP, 2));
        tokens.push_back(destination(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_0));
        tokens.push_back(source(D3DSPR_TEMP, 0, replicateX));
    }
    tokens.push_back(instruction(D3DSIO_MOV, 2));
    tokens.push_back(destination(D3DSPR_RASTOUT, D3DSRO_FOG,
                                 D3DSP_WRITEMASK_0) | D3DSPDM_SATURATE);
    tokens.push_back(source(D3DSPR_TEMP, 0, replicateX));
    tokens.push_back(instruction(D3DSIO_MOV, 2));
    tokens.push_back(destination(D3DSPR_ATTROUT, 0));
    tokens.push_back(source(D3DSPR_INPUT, 1));
    tokens.push_back(D3DVS_END());
    return tokens;
}

std::vector<UINT> fixedFogPixelShader()
{
    const UINT replicateX = D3DVS_X_X | D3DVS_Y_X | D3DVS_Z_X | D3DVS_W_X;
    std::vector<UINT> tokens;

    tokens.push_back(D3DPS_VERSION(3, 0));
    declaration(tokens, D3DDECLUSAGE_COLOR, 0, D3DSPR_INPUT, 0);
    declaration(tokens, D3DDECLUSAGE_FOG, 0, D3DSPR_INPUT, 4);
    tokens.push_back(instruction(D3DSIO_MOV, 2));
    tokens.push_back(destination(D3DSPR_TEMP, 0));
    tokens.push_back(source(D3DSPR_INPUT, 0));
    tokens.push_back(instruction(D3DSIO_ADD, 3));
    tokens.push_back(destination(D3DSPR_TEMP, 3,
                                 D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                                 D3DSP_WRITEMASK_2));
    tokens.push_back(source(D3DSPR_TEMP, 0));
    tokens.push_back(negatedSource(D3DSPR_CONST, 5));
    tokens.push_back(instruction(D3DSIO_MAD, 4));
    tokens.push_back(destination(D3DSPR_TEMP, 0,
                                 D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                                 D3DSP_WRITEMASK_2));
    tokens.push_back(source(D3DSPR_TEMP, 3));
    tokens.push_back(source(D3DSPR_INPUT, 4, replicateX));
    tokens.push_back(source(D3DSPR_CONST, 5));
    tokens.push_back(instruction(D3DSIO_MOV, 2));
    tokens.push_back(destination(D3DSPR_COLOROUT, 0));
    tokens.push_back(source(D3DSPR_TEMP, 0));
    tokens.push_back(D3DPS_END());
    return tokens;
}

enum class PublicPositionForm {
    FullInput,
    SplitInput,
    SplitInlineConstant,
    SplitInlineConstantC2,
};

std::vector<UINT> publicProbeVertexShader(PublicPositionForm form)
{
    std::vector<UINT> tokens;
    tokens.push_back(D3DVS_VERSION(2, 0));
    if (form == PublicPositionForm::SplitInlineConstant ||
        form == PublicPositionForm::SplitInlineConstantC2)
        constant4(tokens,
                  form == PublicPositionForm::SplitInlineConstant ? 0 : 2,
                  0x3f800000u, 0u, 0u, 0u);
    declaration(tokens, D3DDECLUSAGE_POSITION, 0, D3DSPR_INPUT, 0);
    declaration(tokens, D3DDECLUSAGE_COLOR, 0, D3DSPR_INPUT, 1);
    if (form == PublicPositionForm::FullInput) {
        tokens.push_back(instruction(D3DSIO_MOV, 2));
        tokens.push_back(destination(D3DSPR_RASTOUT, D3DSRO_POSITION));
        tokens.push_back(source(D3DSPR_INPUT, 0));
    } else {
        tokens.push_back(instruction(D3DSIO_MOV, 2));
        tokens.push_back(destination(D3DSPR_RASTOUT, D3DSRO_POSITION,
                                     D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                                     D3DSP_WRITEMASK_2));
        tokens.push_back(source(D3DSPR_INPUT, 0));
        tokens.push_back(instruction(D3DSIO_MOV, 2));
        tokens.push_back(destination(D3DSPR_RASTOUT, D3DSRO_POSITION,
                                     D3DSP_WRITEMASK_3));
        tokens.push_back(source(
            form == PublicPositionForm::SplitInput ? D3DSPR_INPUT
                                                   : D3DSPR_CONST,
            form == PublicPositionForm::SplitInlineConstantC2 ? 2 : 0,
            form == PublicPositionForm::SplitInput ? D3DSP_NOSWIZZLE : 0));
    }
    tokens.push_back(instruction(D3DSIO_MOV, 2));
    tokens.push_back(destination(D3DSPR_ATTROUT, 0));
    tokens.push_back(source(D3DSPR_INPUT, 1));
    tokens.push_back(D3DVS_END());
    return tokens;
}

std::vector<UINT> publicProbePixelShader()
{
    std::vector<UINT> tokens;
    tokens.push_back(D3DPS_VERSION(2, 0));
    tokens.push_back(instruction(D3DSIO_DCL, 2));
    tokens.push_back(triton9_sm2_dcl_semantic(0, 0));
    tokens.push_back(destination(D3DSPR_INPUT, 0));
    tokens.push_back(instruction(D3DSIO_MOV, 2));
    tokens.push_back(destination(D3DSPR_COLOROUT, 0));
    tokens.push_back(source(D3DSPR_INPUT, 0));
    tokens.push_back(D3DPS_END());
    return tokens;
}

const char *semanticName(UINT usage)
{
    switch (usage) {
    case D3DDECLUSAGE_POSITION: return "SV_Position";
    case D3DDECLUSAGE_COLOR: return "COLOR";
    case D3DDECLUSAGE_FOG: return "FOG";
    default: return "ATTRIB";
    }
}

UINT semanticSystemValue(UINT usage)
{
    return usage == D3DDECLUSAGE_POSITION ? D3D_NAME_POSITION
                                          : D3D_NAME_UNDEFINED;
}

std::vector<TRITON_DXBC_SIGNATURE>
vertexInputSignatures()
{
    std::vector<TRITON_DXBC_SIGNATURE> signatures(2);
    signatures[0].semanticName = "POSITION";
    signatures[0].registerIdx = 0;
    signatures[0].mask = 0x0f;
    signatures[0].componentType = 3;
    signatures[1].semanticName = "COLOR";
    signatures[1].registerIdx = 1;
    signatures[1].mask = 0x0f;
    signatures[1].componentType = 3;
    return signatures;
}

std::vector<TRITON_DXBC_SIGNATURE>
linkageSignatures(const ShaderConv::VSOutputDecls &outputs)
{
    std::vector<TRITON_DXBC_SIGNATURE> signatures(outputs.GetSize());
    for (UINT index = 0; index < outputs.GetSize(); ++index) {
        const ShaderConv::VSOutputDecl &output = outputs[index];
        signatures[index].semanticName = semanticName(output.Usage);
        signatures[index].semanticIndex = output.UsageIndex;
        signatures[index].systemValue = semanticSystemValue(output.Usage);
        signatures[index].registerIdx = output.RegIndex;
        signatures[index].mask = (BYTE)(output.WriteMask ? output.WriteMask : 0x0f);
        signatures[index].componentType = 3;
    }
    return signatures;
}

bool applyInlineFloatConstants(std::vector<float> &data,
                               const ShaderConv::ShaderConsts &constants)
{
    for (const ShaderConv::ShaderConst &constant : constants) {
        const uint32_t firstScalar = constant.RegIndex;
        const uint32_t scalarCount = 4;

        if (firstScalar > data.size() || scalarCount > data.size() - firstScalar)
            return false;
        std::memcpy(data.data() + firstScalar, constant.Value,
                    scalarCount * sizeof(float));
    }
    return true;
}

int validateHostShaders(const ShaderConv::ByteCode &vertexCode,
                        const ShaderConv::ByteCode &pixelCode,
                        const ShaderConv::VSOutputDecls &outputs,
                        const ShaderConv::ShaderConsts &vertexInlineFloat,
                        const ShaderConv::ShaderConsts &pixelInlineFloat,
                        const char *label, bool expectFog)
{
    std::vector<TRITON_DXBC_SIGNATURE> vertexInputs = vertexInputSignatures();
    std::vector<TRITON_DXBC_SIGNATURE> linkage = linkageSignatures(outputs);
    SIZE_T vertexDxbcSize = 0;
    SIZE_T pixelDxbcSize = 0;
    void *vertexDxbc = tritonBuildDxbc(
        static_cast<const UINT *>(vertexCode.m_pByteCode),
        vertexCode.m_byteCodeSize, vertexInputs.data(),
        (UINT)vertexInputs.size(), linkage.data(), (UINT)linkage.size(),
        nullptr, 0, sizeof(TRITON_DXBC_SIGNATURE), &vertexDxbcSize);
    void *pixelDxbc = tritonBuildDxbc(
        static_cast<const UINT *>(pixelCode.m_pByteCode),
        pixelCode.m_byteCodeSize, linkage.data(), (UINT)linkage.size(),
        nullptr, 0, nullptr, 0, sizeof(TRITON_DXBC_SIGNATURE),
        &pixelDxbcSize);
    if (!vertexDxbc || !pixelDxbc) {
        std::fprintf(stderr, "DXBC wrapping failed: VS=%p PS=%p\n",
                     vertexDxbc, pixelDxbc);
        HeapFree(GetProcessHeap(), 0, vertexDxbc);
        HeapFree(GetProcessHeap(), 0, pixelDxbc);
        return 3;
    }

    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    ID3D11VertexShader *vertexShader = nullptr;
    ID3D11PixelShader *pixelShader = nullptr;
    ID3D11InputLayout *inputLayout = nullptr;
    ID3D11Buffer *vertexBuffer = nullptr;
    ID3D11Texture2D *renderTexture = nullptr;
    ID3D11Texture2D *stagingTexture = nullptr;
    ID3D11RenderTargetView *renderTarget = nullptr;
    ID3D11BlendState *blendState = nullptr;
    ID3D11DepthStencilState *depthState = nullptr;
    ID3D11RasterizerState *rasterState = nullptr;
    ID3D11RasterizerState *scissorState = nullptr;
    ID3D11RasterizerState *cullClockwiseState = nullptr;
    ID3D11RasterizerState *cullCounterClockwiseState = nullptr;
    std::vector<ID3D11Buffer *> vertexConstants(4, nullptr);
    std::vector<ID3D11Buffer *> pixelConstants(8, nullptr);
    std::vector<ID3D11SamplerState *> samplers(16, nullptr);
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_9_1;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
        (UINT)_countof(levels), D3D11_SDK_VERSION, &device,
        &featureLevel, &context);
    if (SUCCEEDED(hr))
        hr = device->CreateVertexShader(vertexDxbc, vertexDxbcSize, nullptr,
                                        &vertexShader);
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (SUCCEEDED(hr))
        hr = device->CreateInputLayout(elements, (UINT)_countof(elements),
                                       vertexDxbc, vertexDxbcSize, &inputLayout);
    if (SUCCEEDED(hr))
        hr = device->CreatePixelShader(pixelDxbc, pixelDxbcSize, nullptr,
                                       &pixelShader);

    /* Reproduce Triton's exact first-draw object mix, including DEFAULT
     * buffers updated through a partial box (the Neptune UP upload path). */
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.ByteWidth = 4096;
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (SUCCEEDED(hr))
        hr = device->CreateBuffer(&bufferDesc, nullptr, &vertexBuffer);
    struct Vertex {
        float x, y, z;
        uint32_t color;
    };
    const Vertex triangle[] = {
        {-0.75f, -0.75f, 0.5f, 0xffff0000u},
        { 0.00f,  0.75f, 0.5f, 0xffff0000u},
        { 0.75f, -0.75f, 0.5f, 0xffff0000u},
    };
    D3D11_BOX vertexBox = {};
    vertexBox.right = sizeof(triangle);
    vertexBox.bottom = 1;
    vertexBox.back = 1;
    if (SUCCEEDED(hr))
        context->UpdateSubresource(vertexBuffer, 0, &vertexBox, triangle,
                                   sizeof(triangle), sizeof(triangle));

    auto createConstantBuffer = [&](UINT bytes, ID3D11Buffer **buffer,
                                    const void *data) {
        if (FAILED(hr))
            return;
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = bytes;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device->CreateBuffer(&desc, nullptr, buffer);
        if (SUCCEEDED(hr) && data)
            context->UpdateSubresource(*buffer, 0, nullptr, data, 0, 0);
    };
    std::vector<float> vs0(4096 / sizeof(float), 0.0f);
    vs0[0] = vs0[5] = vs0[10] = vs0[15] = 1.0f;
    vs0[16] = vs0[21] = vs0[26] = vs0[31] = 1.0f;
    vs0[33] = 1.0f;
    vs0[34] = 1.0f;
    vs0[32] = -1.4426950408889634f;
    if (!applyInlineFloatConstants(vs0, vertexInlineFloat))
        hr = E_FAIL;
    createConstantBuffer(4096, &vertexConstants[0], vs0.data());
    ShaderConv::VSCBExtension vsExtension = {};
    vsExtension.vViewPortScale[0] = 1.0f / 128.0f;
    vsExtension.vViewPortScale[1] = -1.0f / 128.0f;
    vsExtension.vScreenToClipOffset[0] = 0.5f - 64.0f;
    vsExtension.vScreenToClipOffset[1] = 0.5f - 64.0f;
    vsExtension.vScreenToClipScale[0] = 2.0f / 128.0f;
    vsExtension.vScreenToClipScale[1] = -2.0f / 128.0f;
    vsExtension.vScreenToClipScale[2] = 1.0f;
    vsExtension.vScreenToClipScale[3] = 1.0f;
    std::vector<uint8_t> vs3(4096, 0);
    std::memcpy(vs3.data(), &vsExtension, sizeof(vsExtension));
    createConstantBuffer(4096, &vertexConstants[3], vs3.data());

    std::vector<float> ps0((224 * 4 * sizeof(float)) / sizeof(float), 0.0f);
    ps0[0] = ps0[1] = ps0[2] = ps0[3] = 1.0f;
    ps0[8] = ps0[9] = ps0[10] = ps0[11] = 1.0f;
    ps0[12] = ps0[13] = ps0[14] = ps0[15] = -1.0f;
    ps0[22] = 1.0f;
    if (!applyInlineFloatConstants(ps0, pixelInlineFloat))
        hr = E_FAIL;
    createConstantBuffer(224 * 4 * sizeof(float), &pixelConstants[0], ps0.data());
    std::vector<uint8_t> zeroConstants(4096, 0);
    for (UINT slot = 3; slot < pixelConstants.size(); ++slot)
        createConstantBuffer(4096, &pixelConstants[slot], zeroConstants.data());

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = 128;
    textureDesc.Height = 128;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(hr))
        hr = device->CreateTexture2D(&textureDesc, nullptr, &renderTexture);
    if (SUCCEEDED(hr))
        hr = device->CreateRenderTargetView(renderTexture, nullptr,
                                            &renderTarget);
    textureDesc.Usage = D3D11_USAGE_STAGING;
    textureDesc.BindFlags = 0;
    textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr))
        hr = device->CreateTexture2D(&textureDesc, nullptr, &stagingTexture);

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr))
        hr = device->CreateBlendState(&blendDesc, &blendState);
    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (SUCCEEDED(hr))
        hr = device->CreateDepthStencilState(&depthDesc, &depthState);
    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr))
        hr = device->CreateRasterizerState(&rasterDesc, &rasterState);
    rasterDesc.ScissorEnable = TRUE;
    if (SUCCEEDED(hr))
        hr = device->CreateRasterizerState(&rasterDesc, &scissorState);
    rasterDesc.ScissorEnable = FALSE;
    rasterDesc.CullMode = D3D11_CULL_BACK;
    rasterDesc.FrontCounterClockwise = FALSE;
    if (SUCCEEDED(hr))
        hr = device->CreateRasterizerState(&rasterDesc, &cullClockwiseState);
    rasterDesc.FrontCounterClockwise = TRUE;
    if (SUCCEEDED(hr))
        hr = device->CreateRasterizerState(&rasterDesc,
                                            &cullCounterClockwiseState);
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    for (UINT stage = 0; SUCCEEDED(hr) && stage < samplers.size(); ++stage)
        hr = device->CreateSamplerState(&samplerDesc, &samplers[stage]);

    uint32_t center = 0;
    uint32_t outside = 0;
    uint32_t clockwiseCenter = 0;
    uint32_t counterClockwiseCenter = 0;
    uint32_t viewportCenter = 0;
    uint32_t viewportInside = 0;
    if (SUCCEEDED(hr)) {
        const FLOAT clear[] = {0.02f, 0.04f, 0.08f, 1.0f};
        const FLOAT blendFactor[] = {1.0f, 1.0f, 1.0f, 1.0f};
        D3D11_VIEWPORT viewport = {};
        UINT stride = sizeof(Vertex);
        UINT offset = 0;

        viewport.Width = 128.0f;
        viewport.Height = 128.0f;
        viewport.MaxDepth = 1.0f;
        context->OMSetRenderTargets(1, &renderTarget, nullptr);
        context->OMSetBlendState(blendState, blendFactor, 0xffffffffu);
        context->OMSetDepthStencilState(depthState, 0);
        context->VSSetShader(vertexShader, nullptr, 0);
        context->PSSetShader(pixelShader, nullptr, 0);
        context->VSSetConstantBuffers(0, (UINT)vertexConstants.size(),
                                      vertexConstants.data());
        context->PSSetConstantBuffers(0, (UINT)pixelConstants.size(),
                                      pixelConstants.data());
        context->PSSetSamplers(0, (UINT)samplers.size(), samplers.data());
        context->IASetInputLayout(inputLayout);
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        auto renderAndRead = [&](ID3D11RasterizerState *state,
                                 D3D11_PRIMITIVE_TOPOLOGY primitiveTopology,
                                 const D3D11_VIEWPORT &drawViewport,
                                 const D3D11_RECT *scissor,
                                 uint32_t *centerPixel,
                                 uint32_t *outsidePixel,
                                 uint32_t *rightPixel) {
            if (FAILED(hr))
                return;
            context->ClearRenderTargetView(renderTarget, clear);
            context->RSSetState(state);
            context->RSSetViewports(1, &drawViewport);
            if (scissor)
                context->RSSetScissorRects(1, scissor);
            context->IASetPrimitiveTopology(primitiveTopology);
            context->Draw(3, 0);
            context->CopyResource(stagingTexture, renderTexture);
            context->Flush();
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            hr = context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(hr) || !mapped.pData) {
                if (SUCCEEDED(hr))
                    hr = E_FAIL;
                return;
            }
            const uint8_t *pixels = static_cast<const uint8_t *>(mapped.pData);
            if (centerPixel)
                std::memcpy(centerPixel, pixels + 64 * mapped.RowPitch +
                            64 * sizeof(*centerPixel), sizeof(*centerPixel));
            if (outsidePixel)
                std::memcpy(outsidePixel, pixels + 5 * mapped.RowPitch +
                            5 * sizeof(*outsidePixel), sizeof(*outsidePixel));
            if (rightPixel)
                std::memcpy(rightPixel, pixels + 64 * mapped.RowPitch +
                            112 * sizeof(*rightPixel), sizeof(*rightPixel));
            context->Unmap(stagingTexture, 0);
        };
        auto isRed = [](uint32_t pixel) {
            return (pixel & 0x00ff0000u) >= 0x00c00000u &&
                   (pixel & 0x0000ffffu) <= 0x00004040u;
        };
        auto isFogBlend = [](uint32_t pixel) {
            const uint32_t red = (pixel >> 16) & 0xffu;
            const uint32_t green = (pixel >> 8) & 0xffu;
            const uint32_t blue = pixel & 0xffu;
            return red >= 0x30u && red <= 0xe0u && green <= 0x20u &&
                   blue >= 0x30u && blue <= 0xd0u;
        };
        auto isDrawColor = [&](uint32_t pixel) {
            return expectFog ? isFogBlend(pixel) : isRed(pixel);
        };

        renderAndRead(rasterState, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                      viewport, nullptr, &center, &outside, nullptr);
        if (SUCCEEDED(hr) && (!isDrawColor(center) || isDrawColor(outside)))
            hr = E_FAIL;
        const Vertex clippedTriangle[] = {
            {-0.75f, -0.75f, 0.5f, 0xffff0000u},
            { 0.00f,  2.00f, 0.5f, 0xffff0000u},
            { 0.75f, -0.75f, 0.5f, 0xffff0000u},
        };
        uint32_t clippedCenter = 0;
        if (SUCCEEDED(hr))
            context->UpdateSubresource(vertexBuffer, 0, &vertexBox,
                                       clippedTriangle,
                                       sizeof(clippedTriangle),
                                       sizeof(clippedTriangle));
        renderAndRead(rasterState, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                      viewport, nullptr, &clippedCenter, nullptr, nullptr);
        if (SUCCEEDED(hr) && !isDrawColor(clippedCenter))
            hr = E_FAIL;
        if (SUCCEEDED(hr))
            context->UpdateSubresource(vertexBuffer, 0, &vertexBox, triangle,
                                       sizeof(triangle), sizeof(triangle));
        renderAndRead(cullClockwiseState,
                      D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, viewport,
                      nullptr, &clockwiseCenter, nullptr, nullptr);
        renderAndRead(cullCounterClockwiseState,
                      D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, viewport,
                      nullptr, &counterClockwiseCenter, nullptr, nullptr);
        if (SUCCEEDED(hr) &&
            isDrawColor(clockwiseCenter) == isDrawColor(counterClockwiseCenter))
            hr = E_FAIL;
        D3D11_RECT narrowScissor = {0, 0, 32, 128};
        uint32_t scissoredCenter = 0;
        renderAndRead(scissorState, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                      viewport, &narrowScissor, &scissoredCenter, nullptr,
                      nullptr);
        if (SUCCEEDED(hr) && isDrawColor(scissoredCenter))
            hr = E_FAIL;
        D3D11_RECT fullScissor = {0, 0, 128, 128};
        uint32_t restoredCenter = 0;
        renderAndRead(scissorState, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
                      viewport, &fullScissor, &restoredCenter, nullptr,
                      nullptr);
        if (SUCCEEDED(hr) && !isDrawColor(restoredCenter))
            hr = E_FAIL;
        D3D11_VIEWPORT rightViewport = viewport;
        rightViewport.TopLeftX = 96.0f;
        rightViewport.Width = 32.0f;
        renderAndRead(rasterState, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                      rightViewport, nullptr, &viewportCenter, nullptr,
                      &viewportInside);
        if (SUCCEEDED(hr) &&
            (isDrawColor(viewportCenter) || !isDrawColor(viewportInside)))
            hr = E_FAIL;
        if (SUCCEEDED(hr))
            hr = device->GetDeviceRemovedReason();
    }

    for (ID3D11SamplerState *&sampler : samplers)
        releaseObject(sampler);
    for (ID3D11Buffer *&buffer : pixelConstants)
        releaseObject(buffer);
    for (ID3D11Buffer *&buffer : vertexConstants)
        releaseObject(buffer);
    releaseObject(cullCounterClockwiseState);
    releaseObject(cullClockwiseState);
    releaseObject(scissorState);
    releaseObject(rasterState);
    releaseObject(depthState);
    releaseObject(blendState);
    releaseObject(renderTarget);
    releaseObject(stagingTexture);
    releaseObject(renderTexture);
    releaseObject(vertexBuffer);
    releaseObject(pixelShader);
    releaseObject(inputLayout);
    releaseObject(vertexShader);
    releaseObject(context);
    releaseObject(device);
    HeapFree(GetProcessHeap(), 0, vertexDxbc);
    HeapFree(GetProcessHeap(), 0, pixelDxbc);
    if (FAILED(hr)) {
        std::fprintf(stderr,
                     "DXMT %s pipeline/render failed: 0x%08x center=0x%08x\n",
                     label, (UINT)hr, center);
        return 4;
    }
    std::printf("DXMT rendered %s center=0x%08x outside=0x%08x "
                "winding=(0x%08x,0x%08x) viewport=0x%08x at FL 0x%x\n",
                label, center, outside, clockwiseCenter,
                counterClockwiseCenter, viewportInside, (UINT)featureLevel);
    return 0;
}

int convertAndRenderPair(const char *label, std::vector<UINT> vertex,
                         std::vector<UINT> pixel, bool expectFog = false)
{
    ShaderConv::ShaderConverterAPI converter;
    ShaderConv::RasterStates raster;
    ShaderConv::VSInputDecls inputs(ShaderConv::MAX_VS_INPUT_REGS);
    ShaderConv::VSOutputDecls outputs;
    inputs.AddDecl(D3DDECLUSAGE_POSITION, 0, 0);
    inputs.AddDecl(D3DDECLUSAGE_COLOR, 0, 1);

    ShaderConv::ConvertShaderArgs vertexArgs(
        9, ShaderConv::AnythingTimes0Equals0, raster);
    vertexArgs.type = ShaderConv::ConvertShaderArgs::SHADER_TYPE::SHADER_TYPE_VERTEX;
    vertexArgs.pPsInputDecl = nullptr;
    vertexArgs.pVsInputDecl = &inputs;
    vertexArgs.pVsOutputDecl = &outputs;
    vertexArgs.legacyByteCode.m_pByteCode = vertex.data();
    vertexArgs.legacyByteCode.m_byteCodeSize = vertex.size() * sizeof(UINT);
    HRESULT hr = converter.ConvertShader(vertexArgs);
    if (FAILED(hr) || !vertexArgs.convertedByteCode.m_pByteCode ||
        !vertexArgs.convertedByteCode.m_byteCodeSize) {
        std::fprintf(stderr, "%s VS conversion failed: 0x%08x\n", label,
                     (UINT)hr);
        return 1;
    }
    std::printf("%s VS: %zu legacy bytes -> %zu SM4 bytes, %u outputs\n",
                label, vertex.size() * sizeof(UINT),
                vertexArgs.convertedByteCode.m_byteCodeSize, outputs.GetSize());
    ShaderConv::ConvertShaderArgs pixelArgs(
        9, ShaderConv::AnythingTimes0Equals0, raster);
    pixelArgs.type = ShaderConv::ConvertShaderArgs::SHADER_TYPE::SHADER_TYPE_PIXEL;
    pixelArgs.pPsInputDecl = &outputs;
    pixelArgs.pVsInputDecl = nullptr;
    pixelArgs.pVsOutputDecl = nullptr;
    pixelArgs.legacyByteCode.m_pByteCode = pixel.data();
    pixelArgs.legacyByteCode.m_byteCodeSize = pixel.size() * sizeof(UINT);
    hr = converter.ConvertShader(pixelArgs);
    if (FAILED(hr) || !pixelArgs.convertedByteCode.m_pByteCode ||
        !pixelArgs.convertedByteCode.m_byteCodeSize) {
        std::fprintf(stderr, "%s PS conversion failed: 0x%08x\n", label,
                     (UINT)hr);
        ShaderConv::ShaderConverterAPI::CleanUpConvertedShader(
            vertexArgs.convertedByteCode);
        return 2;
    }
    std::printf("%s PS: %zu legacy bytes -> %zu SM4 bytes\n",
                label, pixel.size() * sizeof(UINT),
                pixelArgs.convertedByteCode.m_byteCodeSize);
    int hostResult = validateHostShaders(
        vertexArgs.convertedByteCode, pixelArgs.convertedByteCode, outputs,
        vertexArgs.m_inlineConsts[ShaderConv::CB_FLOAT],
        pixelArgs.m_inlineConsts[ShaderConv::CB_FLOAT], label, expectFog);
    ShaderConv::ShaderConverterAPI::CleanUpConvertedShader(
        vertexArgs.convertedByteCode);
    ShaderConv::ShaderConverterAPI::CleanUpConvertedShader(
        pixelArgs.convertedByteCode);
    return hostResult;
}

} // namespace

namespace fixed_test {
using Color = Triton9Fixed::Constant;
using State = Triton9Fixed::State;
using Program = Triton9Fixed::Program;
struct Vertex { Color reg[16]{}; };
const char *name(unsigned usage) {
    switch(usage) {
    case D3DDECLUSAGE_POSITION: return "POSITION";
    case D3DDECLUSAGE_NORMAL: return "NORMAL";
    case D3DDECLUSAGE_COLOR: return "COLOR";
    case D3DDECLUSAGE_TEXCOORD: return "TEXCOORD";
    case D3DDECLUSAGE_BLENDWEIGHT: return "BLENDWEIGHT";
    case D3DDECLUSAGE_BLENDINDICES: return "BLENDINDICES";
    case D3DDECLUSAGE_PSIZE: return "PSIZE";
    case D3DDECLUSAGE_FOG: return "FOG";
    case D3DDECLUSAGE_CLIPDISTANCE: return "SV_ClipDistance";
    case D3DDECLUSAGE_POINTSPRITE: return "POINTSPRITE";
    default: return "ATTRIB";
    }
}
std::vector<TRITON_DXBC_SIGNATURE> outputs(const ShaderConv::VSOutputDecls &decls) {
    std::vector<TRITON_DXBC_SIGNATURE> result;
    for(unsigned i=0;i<decls.GetSize();++i) {
        auto d=decls[i]; TRITON_DXBC_SIGNATURE s{};
        s.semanticName=d.Usage==D3DDECLUSAGE_POSITION ? "SV_Position" : name(d.Usage);
        s.semanticIndex=d.UsageIndex; s.registerIdx=d.RegIndex; s.mask=d.WriteMask;
        s.componentType=3; s.systemValue=d.Usage==D3DDECLUSAGE_POSITION ? D3D_NAME_POSITION :
            d.Usage==D3DDECLUSAGE_CLIPDISTANCE ? D3D_NAME_CLIP_DISTANCE : D3D_NAME_UNDEFINED;
        result.push_back(s);
    }
    return result;
}
struct Render {
    ID3D11Device *device=nullptr;
    ID3D11DeviceContext *context=nullptr;
    unsigned cases=0;
    Render() {
        auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
        if(FAILED(hr)) throw hr;
    }
    ~Render() { releaseObject(context); releaseObject(device); }
    Color draw(const State &state, Program vertex, Program pixel,
               const Vertex (&vertices)[3], ShaderConv::RasterStates raster={},
               Color texture={.65f,.35f,.1f,.8f},
               ShaderConv::VSCBExtension extension={}, unsigned x=16, unsigned y=16, bool gradient=false, unsigned dimension=2, bool mipChain=false,
               float depthLimit=-1, unsigned targetIndex=0, float *depthValue=nullptr) {
        context->ClearState();
        raster.TCIMapping=ShaderConv::TCIMASK_PASSTHRU;
        raster.FixedFunctionPixel=pixel.fixedFunction;
        ShaderConv::ShaderConverterAPI converter;
        ShaderConv::VSInputDecls inputs(16); ShaderConv::VSOutputDecls out, gsOut;
        std::vector<TRITON_DXBC_SIGNATURE> inputSig;
        std::vector<D3D11_INPUT_ELEMENT_DESC> layout;
        for(const auto &i : state.inputs) {
            inputs.AddDecl(i.usage,i.index,i.reg);
            TRITON_DXBC_SIGNATURE sig{}; sig.semanticName=name(i.usage); sig.semanticIndex=i.index;
            sig.registerIdx=i.reg; sig.mask=15; sig.componentType=3; inputSig.push_back(sig);
            D3D11_INPUT_ELEMENT_DESC e{}; e.SemanticName=name(i.usage); e.SemanticIndex=i.index;
            e.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; e.AlignedByteOffset=i.reg*16; layout.push_back(e);
        }
        ShaderConv::ConvertShaderArgs vs(9,ShaderConv::AnythingTimes0Equals0 | (vertex.fixedFunction ? ShaderConv::InternalFixedFunction : 0),raster);
        vs.type=ShaderConv::ConvertShaderArgs::SHADER_TYPE::SHADER_TYPE_VERTEX;
        vs.legacyByteCode.m_pByteCode=vertex.tokens.data(); vs.legacyByteCode.m_byteCodeSize=vertex.tokens.size()*4;
        vs.pVsInputDecl=&inputs; vs.pVsOutputDecl=&out;
        HRESULT hr=converter.ConvertShader(vs); if(FAILED(hr)) throw hr;
        auto outSig=outputs(out);
        std::vector<IUnknown *> objects;
        struct Cleanup {std::vector<IUnknown *> &o; ~Cleanup(){for(auto *v:o)v->Release();}} cleanup{objects};
        auto checked=[&](HRESULT result,IUnknown *object){if(FAILED(result)||!object)throw FAILED(result)?result:E_FAIL;objects.push_back(object);};
        auto build=[&](ShaderConv::ByteCode &code,const std::vector<TRITON_DXBC_SIGNATURE> &inSig,const std::vector<TRITON_DXBC_SIGNATURE> &outSig,size_t &size) {
            void *blob=tritonBuildDxbc((UINT *)code.m_pByteCode,code.m_byteCodeSize,inSig.data(),inSig.size(),outSig.data(),outSig.size(),nullptr,0,sizeof(TRITON_DXBC_SIGNATURE),&size);
            ShaderConv::ShaderConverterAPI::CleanUpConvertedShader(code);
            if(!blob)throw E_OUTOFMEMORY;return blob;
        };
        size_t size; void *blob=build(vs.convertedByteCode,inputSig,outSig,size);
        ID3D11VertexShader *vsHost=nullptr;hr=device->CreateVertexShader(blob,size,nullptr,&vsHost); checked(hr,vsHost);
        ID3D11InputLayout *il=nullptr;hr=device->CreateInputLayout(layout.data(),layout.size(),blob,size,&il);
        HeapFree(GetProcessHeap(),0,blob);checked(hr,il);
        context->VSSetShader(vsHost,nullptr,0);context->IASetInputLayout(il);
        ShaderConv::VSOutputDecls *linkage=&out;
        bool gs=raster.PrimitiveType==D3DPT_POINTLIST || raster.FillMode==D3DFILL_POINT;
        for(unsigned i=0;i<16;++i)gs|=raster.PSSamplers[i].TexCoordWrap!=0;
        if(gs) {
            ShaderConv::CreateGeometryShaderArgs args(9,ShaderConv::AnythingTimes0Equals0,out,&gsOut,raster);
            hr=converter.CreateGeometryShader(args);if(FAILED(hr))throw hr;
            auto sig=outputs(gsOut);blob=build(args.m_GSByteCode,outSig,sig,size);
            ID3D11GeometryShader *shader=nullptr;hr=device->CreateGeometryShader(blob,size,nullptr,&shader);
            HeapFree(GetProcessHeap(),0,blob);checked(hr,shader);context->GSSetShader(shader,nullptr,0);linkage=&gsOut;
        }
        ShaderConv::ConvertShaderArgs ps(9,ShaderConv::AnythingTimes0Equals0,raster);
        ps.type=ShaderConv::ConvertShaderArgs::SHADER_TYPE::SHADER_TYPE_PIXEL;
        ps.legacyByteCode.m_pByteCode=pixel.tokens.data();ps.legacyByteCode.m_byteCodeSize=pixel.tokens.size()*4;ps.pPsInputDecl=linkage;
        hr=converter.ConvertShader(ps);if(FAILED(hr))throw hr;
        auto psSig=outputs(*linkage);
        for(auto d:ps.AddedSystemSemantics){TRITON_DXBC_SIGNATURE sig{};sig.semanticName=d.Usage==D3DDECLUSAGE_VFACE?"SV_IsFrontFace":"SV_Position";
            sig.registerIdx=d.RegIndex;sig.mask=d.WriteMask;sig.componentType=d.Usage==D3DDECLUSAGE_VFACE?1:3;
            sig.systemValue=d.Usage==D3DDECLUSAGE_VFACE?D3D_NAME_IS_FRONT_FACE:D3D_NAME_POSITION;psSig.push_back(sig);}
        blob=build(ps.convertedByteCode,psSig,{},size);ID3D11PixelShader *psHost=nullptr;
        hr=device->CreatePixelShader(blob,size,nullptr,&psHost);HeapFree(GetProcessHeap(),0,blob);checked(hr,psHost);context->PSSetShader(psHost,nullptr,0);
        auto cb=[&](const void *data,unsigned bytes){ID3D11Buffer *buffer=nullptr;D3D11_BUFFER_DESC desc{};desc.ByteWidth=(bytes+15)&~15u;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data;auto result=device->CreateBuffer(&desc,&initial,&buffer);checked(result,buffer);return buffer;};
        vertex.constants.resize(std::max<size_t>(vertex.constants.size(),1920));pixel.constants.resize(std::max<size_t>(pixel.constants.size(),224));
        for(auto k:vs.m_inlineConsts[0])std::memcpy((float *)vertex.constants.data()+k.RegIndex,k.Value,16);
        for(auto k:ps.m_inlineConsts[0])std::memcpy((float *)pixel.constants.data()+k.RegIndex,k.Value,16);
        auto *vbuf=cb(vertex.constants.data(),vertex.constants.size()*16);context->VSSetConstantBuffers(0,1,&vbuf);
        auto *pbuf=cb(pixel.constants.data(),pixel.constants.size()*16);context->PSSetConstantBuffers(0,1,&pbuf);
        for(unsigned stage=0;stage<2;++stage) for(unsigned slot=1;slot<3;++slot) {
            UINT data[64]{};auto &arg=stage ? ps : vs;
            for(const auto &k:arg.m_inlineConsts[slot])std::memcpy(data+k.RegIndex,k.Value,slot==1?16:4);
            auto *buffer=cb(data,slot==1?256:64);
            if(stage)context->PSSetConstantBuffers(slot,1,&buffer);else context->VSSetConstantBuffers(slot,1,&buffer);
        }
        extension.vViewPortScale[0]=1.f/32;extension.vViewPortScale[1]=-1.f/32;
        if(extension.vPointSize[0]==0)extension.vPointSize[0]=1;
        if(extension.vPointSize[1]==0)extension.vPointSize[1]=1;
        if(extension.vPointSize[2]==0)extension.vPointSize[2]=64;
        auto *ext=cb(&extension,sizeof(extension));context->VSSetConstantBuffers(3,1,&ext);context->GSSetConstantBuffers(3,1,&ext);
        ShaderConv::PSCBExtension pe{};pe.fAlphaRef=float(state.render[D3DRS_ALPHAREF])/255;
        pe.vFogColor[2]=1;pe.fFogStart=0;pe.fFogEnd=1;pe.fFogDistInv=1;pe.fFogDensity=1;
        auto *pex=cb(&pe,sizeof(pe));context->PSSetConstantBuffers(3,1,&pex);
        ShaderConv::PSCBExtension2 bump{};
        for(unsigned i=0;i<8;++i){std::memcpy(bump.vBumpEnvMat[i],state.stages[i].bump,16);bump.vBumpEnvL[i][0]=state.stages[i].bumpScale;bump.vBumpEnvL[i][1]=state.stages[i].bumpOffset;}
        auto *bumpBuffer=cb(&bump,sizeof(bump));context->PSSetConstantBuffers(4,1,&bumpBuffer);
        D3D11_BUFFER_DESC vd{};vd.ByteWidth=sizeof(vertices);vd.Usage=D3D11_USAGE_DEFAULT;vd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};init.pSysMem=vertices;ID3D11Buffer *vb=nullptr;hr=device->CreateBuffer(&vd,&init,&vb);checked(hr,vb);
        UINT stride=sizeof(Vertex),offset=0;context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
        context->IASetPrimitiveTopology(raster.PrimitiveType==D3DPT_POINTLIST ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=32;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D *targets[4]={};ID3D11RenderTargetView *views[4]={};
        if(targetIndex>=4)throw E_INVALIDARG;
        Color clear={-.25f,-.25f,-.25f,-.25f};
        for(unsigned i=0;i<=targetIndex;++i){hr=device->CreateTexture2D(&td,nullptr,&targets[i]);checked(hr,targets[i]);
            hr=device->CreateRenderTargetView(targets[i],nullptr,&views[i]);checked(hr,views[i]);context->ClearRenderTargetView(views[i],clear.data());}
        ID3D11Texture2D *target=targets[targetIndex],*depth=nullptr;ID3D11DepthStencilView *depthView=nullptr;
        if(depthLimit>=0||depthValue){D3D11_TEXTURE2D_DESC depthDesc=td;depthDesc.Format=DXGI_FORMAT_D32_FLOAT;depthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
            hr=device->CreateTexture2D(&depthDesc,nullptr,&depth);checked(hr,depth);
            hr=device->CreateDepthStencilView(depth,nullptr,&depthView);checked(hr,depthView);context->ClearDepthStencilView(depthView,D3D11_CLEAR_DEPTH,depthLimit>=0?depthLimit:1,0);}
        context->OMSetRenderTargets(targetIndex+1,views,depthView);
        td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D *staging=nullptr;hr=device->CreateTexture2D(&td,nullptr,&staging);checked(hr,staging);
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
        ID3D11RasterizerState *rs=nullptr;hr=device->CreateRasterizerState(&rd,&rs);checked(hr,rs);context->RSSetState(rs);
        D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthFunc=depthLimit>=0?D3D11_COMPARISON_LESS:D3D11_COMPARISON_ALWAYS;
        dd.DepthEnable=depthLimit>=0||depthValue;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ID3D11DepthStencilState *ds=nullptr;
        hr=device->CreateDepthStencilState(&dd,&ds);checked(hr,ds);context->OMSetDepthStencilState(ds,0);
        D3D11_VIEWPORT vp{};vp.Width=vp.Height=32;vp.MaxDepth=1;context->RSSetViewports(1,&vp);
        td.Width=td.Height=1;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        Color pattern[16];
        if(gradient){td.Width=td.Height=4;for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)pattern[y*4+x]={x/3.f,y/3.f,.1f,.8f};}
        init.pSysMem=gradient ? pattern : (const void *)texture.data();init.SysMemPitch=gradient?64:16;
        ID3D11Resource *tex=nullptr;
        if(dimension==3) {
            D3D11_TEXTURE3D_DESC volume{};volume.Width=volume.Height=volume.Depth=volume.MipLevels=1;
            volume.Format=td.Format;volume.Usage=D3D11_USAGE_DEFAULT;volume.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            Color voxels[64];
            if(gradient){volume.Width=volume.Height=volume.Depth=4;
                for(unsigned z=0;z<4;++z)for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)voxels[z*16+y*4+x]={x/3.f,y/3.f,z/3.f,.8f};
                init.pSysMem=voxels;init.SysMemPitch=64;init.SysMemSlicePitch=256;
            }else init.SysMemSlicePitch=16;
            ID3D11Texture3D *object=nullptr;hr=device->CreateTexture3D(&volume,&init,&object);tex=object;
        } else {
            D3D11_SUBRESOURCE_DATA faces[6];for(auto &face:faces)face=init;
            if(dimension==4){td.ArraySize=6;td.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;}
            Color level0[16],level1[4],level2[1]={{.9f,.7f,.5f,.3f}};D3D11_SUBRESOURCE_DATA levels[3]{};
            if(mipChain){td.Width=td.Height=4;td.MipLevels=3;for(auto &c:level0)c=texture;for(auto &c:level1)c={.2f,.4f,.6f,.8f};
                levels[0]={level0,64,256};levels[1]={level1,32,64};levels[2]={level2,16,16};}
            ID3D11Texture2D *object=nullptr;hr=device->CreateTexture2D(&td,mipChain?levels:dimension==4?faces:&init,&object);tex=object;
        }
        checked(hr,tex);ID3D11ShaderResourceView *view=nullptr;hr=device->CreateShaderResourceView(tex,nullptr,&view);checked(hr,view);
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        ID3D11SamplerState *sampler=nullptr;hr=device->CreateSamplerState(&sd,&sampler);checked(hr,sampler);
        for(unsigned i=0;i<16;++i){context->PSSetShaderResources(i,1,&view);context->PSSetSamplers(i,1,&sampler);}
        for(unsigned i=0;i<4;++i){context->VSSetShaderResources(i,1,&view);context->VSSetSamplers(i,1,&sampler);}
        context->Draw(raster.PrimitiveType==D3DPT_POINTLIST ? 1 : 3,0);context->CopyResource(staging,target);
        D3D11_MAPPED_SUBRESOURCE mapped{};hr=context->Map(staging,0,D3D11_MAP_READ,0,&mapped);if(FAILED(hr))throw hr;
        Color actual;std::memcpy(actual.data(),(char *)mapped.pData+y*mapped.RowPitch+x*16,16);context->Unmap(staging,0);
        if(depthValue){D3D11_TEXTURE2D_DESC desc{};depth->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D *readback=nullptr;hr=device->CreateTexture2D(&desc,nullptr,&readback);checked(hr,readback);context->CopyResource(readback,depth);
            hr=context->Map(readback,0,D3D11_MAP_READ,0,&mapped);if(FAILED(hr))throw hr;
            std::memcpy(depthValue,(char *)mapped.pData+y*mapped.RowPitch+x*4,4);context->Unmap(readback,0);}
        ++cases;return actual;
    }
};
void identity(D3DMATRIX &m){m={};for(unsigned i=0;i<4;++i)m.m[i][i]=1;}
State base(Vertex (&v)[3]) {
    State s;for(auto &w:s.world)identity(w);identity(s.view);identity(s.projection);for(auto &t:s.texture)identity(t);
    s.inputs={{D3DDECLUSAGE_POSITION,0,0,4},{D3DDECLUSAGE_COLOR,0,1,4},{D3DDECLUSAGE_COLOR,1,2,4},{D3DDECLUSAGE_NORMAL,0,3,3},{D3DDECLUSAGE_TEXCOORD,0,4,2}};
    const float positions[3][2]={{-1,-1},{0,1},{1,-1}};
    for(unsigned i=0;i<3;++i){v[i].reg[0]={positions[i][0],positions[i][1],.5f,1};v[i].reg[1]={.3f,.6f,.8f,.4f};v[i].reg[2]={.1f,.2f,.05f,.7f};v[i].reg[3]={0,0,-1,0};v[i].reg[4]={.25f,.75f,0,1};}
    s.render[D3DRS_POINTSIZE]=0x3f800000;s.render[D3DRS_POINTSIZE_MIN]=0x3f800000;s.render[D3DRS_POINTSIZE_MAX]=0x42800000;
    s.render[D3DRS_COLORVERTEX]=TRUE;s.render[D3DRS_TEXTUREFACTOR]=0xb3804d33;
    s.viewportHeight=32;for(unsigned i=0;i<8;++i)s.stages[i].texcoord=i;
    return s;
}
Color rgba(unsigned v){return {float((v>>16)&255)/255,float((v>>8)&255)/255,float(v&255)/255,float(v>>24)/255};}
float clamp(float x){return std::max(0.f,std::min(1.f,x));}
void expect(const char *label,Color actual,Color wanted,float tolerance=.003f) {
    for(unsigned i=0;i<4;++i)if(!std::isfinite(actual[i])||std::abs(actual[i]-wanted[i])>tolerance){
        std::fprintf(stderr,"%s channel %u got %.6f expected %.6f (RGBA %.5f %.5f %.5f %.5f)\n",label,i,actual[i],wanted[i],actual[0],actual[1],actual[2],actual[3]);throw E_FAIL;}
}
Color draw(Render &r,State &s,Vertex (&v)[3],ShaderConv::RasterStates raster={}) {
    Program vs,ps;HRESULT hr=Triton9Fixed::vertex(s,vs);if(FAILED(hr))throw hr;
    hr=Triton9Fixed::pixel(s,ps);if(FAILED(hr))throw hr;return r.draw(s,vs,ps,v,raster);
}
int run() {
    try {
        Render renderer;Vertex v[3];State s=base(v);
        expect("production unlit",draw(renderer,s,v),v[0].reg[1]);
        const Color a=v[0].reg[1],b=rgba(s.render[D3DRS_TEXTUREFACTOR]),a0=rgba(0x99553377);
        for(unsigned op=2;op<=26;++op) {
            if(op==D3DTOP_BUMPENVMAP || op==D3DTOP_BUMPENVMAPLUMINANCE)continue;
            s=base(v);auto &stage=s.stages[0];stage.colorOp=op;stage.alphaOp=D3DTOP_SELECTARG1;
            stage.colorArg[0]=D3DTA_CONSTANT;stage.colorArg[1]=D3DTA_DIFFUSE;stage.colorArg[2]=D3DTA_TFACTOR;
            stage.alphaArg[1]=D3DTA_DIFFUSE;stage.constant=0x99553377;stage.bound=true;
            Color wanted=a;
            for(unsigned i=0;i<3;++i) {
                switch(op) {
                case D3DTOP_SELECTARG1:case D3DTOP_PREMODULATE:wanted[i]=a[i];break;
                case D3DTOP_SELECTARG2:wanted[i]=b[i];break;
                case D3DTOP_MODULATE:wanted[i]=a[i]*b[i];break;
                case D3DTOP_MODULATE2X:wanted[i]=2*a[i]*b[i];break;
                case D3DTOP_MODULATE4X:wanted[i]=4*a[i]*b[i];break;
                case D3DTOP_ADD:wanted[i]=a[i]+b[i];break;
                case D3DTOP_ADDSIGNED:wanted[i]=a[i]+b[i]-.5f;break;
                case D3DTOP_ADDSIGNED2X:wanted[i]=2*(a[i]+b[i]-.5f);break;
                case D3DTOP_SUBTRACT:wanted[i]=a[i]-b[i];break;
                case D3DTOP_ADDSMOOTH:wanted[i]=a[i]+b[i]-a[i]*b[i];break;
                case D3DTOP_BLENDDIFFUSEALPHA:case D3DTOP_BLENDCURRENTALPHA:wanted[i]=a[i]*a[3]+b[i]*(1-a[3]);break;
                case D3DTOP_BLENDTEXTUREALPHA:wanted[i]=a[i]*.8f+b[i]*.2f;break;
                case D3DTOP_BLENDFACTORALPHA:wanted[i]=a[i]*b[3]+b[i]*(1-b[3]);break;
                case D3DTOP_BLENDTEXTUREALPHAPM:wanted[i]=a[i]+b[i]*.2f;break;
                case D3DTOP_MODULATEALPHA_ADDCOLOR:wanted[i]=a[i]+a[3]*b[i];break;
                case D3DTOP_MODULATECOLOR_ADDALPHA:wanted[i]=a[i]*b[i]+a[3];break;
                case D3DTOP_MODULATEINVALPHA_ADDCOLOR:wanted[i]=a[i]+(1-a[3])*b[i];break;
                case D3DTOP_MODULATEINVCOLOR_ADDALPHA:wanted[i]=(1-a[i])*b[i]+a[3];break;
                case D3DTOP_DOTPRODUCT3:wanted[i]=4*((a[0]-.5f)*(b[0]-.5f)+(a[1]-.5f)*(b[1]-.5f)+(a[2]-.5f)*(b[2]-.5f));break;
                case D3DTOP_MULTIPLYADD:wanted[i]=a0[i]+a[i]*b[i];break;
                case D3DTOP_LERP:wanted[i]=a0[i]*a[i]+(1-a0[i])*b[i];break;
                }
                wanted[i]=clamp(wanted[i]);
            }
            char label[80];std::snprintf(label,sizeof(label),"production texture operation %u",op);
            expect(label,draw(renderer,s,v),wanted);
        }
        for(unsigned modifier: {0u,unsigned(D3DTA_COMPLEMENT),unsigned(D3DTA_ALPHAREPLICATE),unsigned(D3DTA_COMPLEMENT|D3DTA_ALPHAREPLICATE)}) {
            s=base(v);auto &t=s.stages[0];t.colorOp=t.alphaOp=D3DTOP_SELECTARG1;t.colorArg[1]=t.alphaArg[1]=D3DTA_CONSTANT|modifier;t.constant=0x99553377;
            Color wanted=rgba(t.constant);for(auto &x:wanted)if(modifier&D3DTA_COMPLEMENT)x=1-x;if(modifier&D3DTA_ALPHAREPLICATE)wanted.fill(wanted[3]);
            expect("argument modifiers",draw(renderer,s,v),wanted);
        }
        // An absent texture changes ALPHAARG1's texture input to diffuse;
        // constants used for RGB must still survive source-alpha blending.
        for(unsigned stage: {0u,1u}) for(bool bound: {false,true})
        for(unsigned modifier: {0u,unsigned(D3DTA_COMPLEMENT)}) {
            s=base(v);
            s.stages[0].colorOp=s.stages[0].alphaOp=D3DTOP_SELECTARG1;
            s.stages[0].colorArg[1]=s.stages[0].alphaArg[1]=D3DTA_CONSTANT;
            s.stages[0].constant=0xe6336699;
            auto &t=s.stages[stage];
            t.colorOp=t.alphaOp=D3DTOP_SELECTARG1;
            t.colorArg[1]=D3DTA_CONSTANT;t.constant=0x80a1b2c3;
            t.alphaArg[1]=D3DTA_TEXTURE|modifier;t.bound=bound;
            Color expected=rgba(t.constant);
            expected[3]=bound ? .8f : v[0].reg[1][3];
            if(modifier&D3DTA_COMPLEMENT)expected[3]=1-expected[3];
            expect("absent texture alpha argument",draw(renderer,s,v),expected);
        }
        // Result TEMP persists independently of CURRENT; stage constants are
        // read from each stage, and PREMODULATE modifies the following CURRENT.
        s=base(v);s.stages[0].colorOp=s.stages[0].alphaOp=D3DTOP_SELECTARG1;
        s.stages[0].colorArg[1]=s.stages[0].alphaArg[1]=D3DTA_CONSTANT;
        s.stages[0].constant=0x99553377;s.stages[0].result=D3DTA_TEMP;
        s.stages[1].colorOp=s.stages[1].alphaOp=D3DTOP_SELECTARG1;
        s.stages[1].colorArg[1]=s.stages[1].alphaArg[1]=D3DTA_TEMP;
        expect("TEMP result and argument",draw(renderer,s,v),rgba(0x99553377));
        s=base(v);s.stages[0].colorOp=s.stages[0].alphaOp=D3DTOP_PREMODULATE;
        s.stages[0].colorArg[1]=s.stages[0].alphaArg[1]=D3DTA_DIFFUSE;
        s.stages[1].colorOp=s.stages[1].alphaOp=D3DTOP_SELECTARG1;
        s.stages[1].colorArg[1]=s.stages[1].alphaArg[1]=D3DTA_CURRENT;s.stages[1].bound=true;
        Color wanted=v[0].reg[1],tex={.65f,.35f,.1f,.8f};for(unsigned i=0;i<4;++i)wanted[i]*=tex[i];
        expect("PREMODULATE next CURRENT",draw(renderer,s,v),wanted);
        for(unsigned swizzle=1;swizzle<=6;++swizzle) {
            s=base(v);s.stages[0].colorOp=s.stages[0].alphaOp=D3DTOP_SELECTARG1;
            s.stages[0].colorArg[1]=s.stages[0].alphaArg[1]=D3DTA_TEXTURE;s.stages[0].bound=true;
            ShaderConv::RasterStates raster;raster.PSSamplerSwizzles[0]=swizzle;
            wanted=tex;
            switch(swizzle) {
            case ShaderConv::SAMPLER_SWIZZLE_RRRA:wanted={tex[0],tex[0],tex[0],tex[3]};break;
            case ShaderConv::SAMPLER_SWIZZLE_RAAA:wanted={tex[0],tex[3],tex[3],tex[3]};break;
            case ShaderConv::SAMPLER_SWIZZLE_RGAA:wanted={tex[0],tex[1],tex[3],tex[3]};break;
            case ShaderConv::SAMPLER_SWIZZLE_RRRG:wanted={tex[0],tex[0],tex[0],tex[1]};break;
            case ShaderConv::SAMPLER_SWIZZLE_BGRA:wanted={tex[2],tex[1],tex[0],tex[3]};break;
            case ShaderConv::SAMPLER_SWIZZLE_RGB1:wanted={tex[0],tex[1],tex[2],1};break;
            }
            expect("PS format swizzle",draw(renderer,s,v,raster),wanted);
        }
        s=base(v);ShaderConv::RasterStates swap;swap.SwapRBOnOutputMask=1;
        expect("converter output swizzle control",draw(renderer,s,v,swap),{.8f,.6f,.3f,.4f});
        for(unsigned minor=1;minor<=4;++minor) {
            s=base(v);Program vs,ps;Triton9Fixed::vertex(s,vs);
            ps.tokens={D3DPS_VERSION(1,minor),D3DSIO_MOV,destination(D3DSPR_TEMP,0),source(D3DSPR_INPUT,0),D3DPS_END()};
            if(!ShaderConv::ValidateLegacyShader(ps.tokens.data(),ps.tokens.size()*4,false))throw E_FAIL;
            for(size_t bytes=0;bytes<ps.tokens.size()*4;bytes+=4)
                if(ShaderConv::ValidateLegacyShader(ps.tokens.data(),bytes,false))throw E_FAIL;
            expect("PS1 legacy MOV",renderer.draw(s,vs,ps,v),v[0].reg[1]);
            if(minor==4)ps.tokens={D3DPS_VERSION(1,4),D3DSIO_TEX,destination(D3DSPR_TEMP,0),source(D3DSPR_TEXTURE,0),D3DPS_END()};
            else ps.tokens={D3DPS_VERSION(1,minor),D3DSIO_TEX,destination(D3DSPR_TEXTURE,0),D3DSIO_MOV,destination(D3DSPR_TEMP,0),source(D3DSPR_TEXTURE,0),D3DPS_END()};
            expect("PS1 legacy texture",renderer.draw(s,vs,ps,v),tex);
        }
        // Lights use independent equations with a uniform -Z normal and +Z
        // direction: N.L is exactly one for all vertices, so interpolation
        // cannot hide a bad material/light term.
        for(unsigned lights: {0u,1u,8u}) {
            s=base(v);s.render[D3DRS_LIGHTING]=TRUE;s.render[D3DRS_COLORVERTEX]=FALSE;
            s.material.Diffuse={.5f,.4f,.3f,.6f};s.material.Ambient={.2f,.3f,.4f,1};s.material.Emissive={.01f,.02f,.03f,1};
            s.render[D3DRS_AMBIENT]=0x00402010;
            for(unsigned i=0;i<lights;++i){D3DLIGHT9 l{};l.Type=D3DLIGHT_DIRECTIONAL;l.Direction.z=1;l.Diffuse={.05f,.04f,.03f,1};l.Ambient={.02f,.01f,.03f,1};s.lights.push_back(l);}
            wanted={.01f+.2f*(64.f/255+lights*.02f)+lights*.5f*.05f,
                    .02f+.3f*(32.f/255+lights*.01f)+lights*.4f*.04f,
                    .03f+.4f*(16.f/255+lights*.03f)+lights*.3f*.03f,.6f};
            expect("directional lights and material",draw(renderer,s,v),wanted);
        }
        for(unsigned materialSource=0;materialSource<=2;++materialSource) {
            s=base(v);s.render[D3DRS_LIGHTING]=TRUE;s.render[D3DRS_COLORVERTEX]=TRUE;
            s.render[D3DRS_DIFFUSEMATERIALSOURCE]=materialSource;s.material.Diffuse={.2f,.4f,.6f,.8f};
            D3DLIGHT9 l{};l.Type=D3DLIGHT_DIRECTIONAL;l.Direction.z=1;l.Diffuse={1,1,1,1};s.lights.push_back(l);
            wanted=materialSource==0 ? Color{.2f,.4f,.6f,.8f} : v[0].reg[materialSource];
            expect("diffuse material source",draw(renderer,s,v),wanted);
        }
        s=base(v);s.inputs.push_back({D3DDECLUSAGE_BLENDWEIGHT,0,5,3});s.inputs.push_back({D3DDECLUSAGE_BLENDINDICES,0,6,4});
        s.render[D3DRS_VERTEXBLEND]=D3DVBF_1WEIGHTS;s.render[D3DRS_INDEXEDVERTEXBLENDENABLE]=TRUE;
        // Opposing translations cancel only for the correct nonzero palette
        // entries and implicit final weight. An out-of-range cbuffer reads zero.
        s.world[217].m[3][0]=1.5f;s.world[253].m[3][0]=-.5f;
        for(auto &vert:v){vert.reg[5]={.25f,0,0,0};vert.reg[6]={217,253,0,0};}
        expect("indexed palette 217/253",draw(renderer,s,v),v[0].reg[1]);
        for(unsigned fog: {unsigned(D3DFOG_LINEAR),unsigned(D3DFOG_EXP),unsigned(D3DFOG_EXP2)}) {
            s=base(v);s.render[D3DRS_FOGENABLE]=TRUE;s.render[D3DRS_FOGVERTEXMODE]=fog;s.render[D3DRS_FOGEND]=0x3f800000;s.render[D3DRS_FOGDENSITY]=0x3f800000;
            ShaderConv::RasterStates raster;raster.FogEnable=TRUE;raster.FogTableMode=D3DFOG_NONE;
            float factor=fog==D3DFOG_LINEAR?.5f:std::exp(fog==D3DFOG_EXP?-.5f:-.25f);
            wanted=v[0].reg[1];for(unsigned i=0;i<3;++i)wanted[i]=wanted[i]*factor+(i==2?1-factor:0);
            expect("production vertex fog",draw(renderer,s,v,raster),wanted);
        }
        // User clip distances and GS point expansion execute actual converter
        // output, including a negative coverage control outside the point.
        s=base(v);Program vs,ps;Triton9Fixed::vertex(s,vs);Triton9Fixed::pixel(s,ps);
        ShaderConv::RasterStates raster;raster.UserClipPlanes=1;ShaderConv::VSCBExtension extension{};extension.vClipPlanes[0][0]=1;
        expect("user clip outside",renderer.draw(s,vs,ps,v,raster,tex,extension,12,20),{-.25f,-.25f,-.25f,-.25f});
        expect("user clip inside",renderer.draw(s,vs,ps,v,raster,tex,extension,20,20),v[0].reg[1]);
        s=base(v);v[0].reg[0]={0,0,.5f,1};s.render[D3DRS_POINTSIZE]=0x41000000;Triton9Fixed::vertex(s,vs);Triton9Fixed::pixel(s,ps);
        raster={};raster.PrimitiveType=D3DPT_POINTLIST;raster.PointSizeEnable=TRUE;raster.FillMode=D3DFILL_SOLID;
        expect("point center",renderer.draw(s,vs,ps,v,raster),v[0].reg[1]);
        expect("point enlarged footprint",renderer.draw(s,vs,ps,v,raster,tex,{},18,16),v[0].reg[1]);
        expect("point outside",renderer.draw(s,vs,ps,v,raster,tex,{},24,16),{-.25f,-.25f,-.25f,-.25f});
        // Directly read the production VS texture output through a minimal PS;
        // this is an independent oracle for transforms and generated coords.
        auto coordinatePixel=[](bool project=false) {
            Program p;p.tokens={D3DPS_VERSION(3,0)};
            declaration(p.tokens,D3DDECLUSAGE_TEXCOORD,0,D3DSPR_INPUT,0);
            if(project) {
                p.tokens.insert(p.tokens.end(),{instruction(D3DSIO_RCP,2),destination(D3DSPR_TEMP,0),source(D3DSPR_INPUT,0,0x00ff0000u),
                    instruction(D3DSIO_MUL,3),destination(D3DSPR_COLOROUT,0),source(D3DSPR_INPUT,0),source(D3DSPR_TEMP,0)});
            } else p.tokens.insert(p.tokens.end(),{instruction(D3DSIO_MOV,2),destination(D3DSPR_COLOROUT,0),source(D3DSPR_INPUT,0)});
            p.tokens.push_back(D3DPS_END());return p;
        };
        for(unsigned count=1;count<=4;++count) {
            s=base(v);s.stages[0].transform=count;
            s.texture[0].m[0][0]=2;s.texture[0].m[1][1]=.5f;s.texture[0].m[2][0]=.1f;s.texture[0].m[2][3]=2;
            Triton9Fixed::vertex(s,vs);ps=coordinatePixel();
            Color coordExpected={.6f,.375f,1,2};for(unsigned i=count;i<4;++i)coordExpected[i]=0;
            expect("texture matrix homogeneous append",renderer.draw(s,vs,ps,v),coordExpected);
            if(count>=2) {
                s.stages[0].transform=count|D3DTTFF_PROJECTED;Triton9Fixed::vertex(s,vs);ps=coordinatePixel(true);
                float denominator=count==2?.375f:count==3?1:2;
                expect("projected texture coordinates",renderer.draw(s,vs,ps,v),{.6f/denominator,.375f/denominator,count==2?0:1/denominator,1});
            }
        }
        s=base(v);s.stages[0].texcoord=D3DTSS_TCI_CAMERASPACENORMAL;
        for(auto &vert:v)vert.reg[3]={.2f,.4f,.6f,0};Triton9Fixed::vertex(s,vs);ps=coordinatePixel();
        expect("camera-space normal texgen",renderer.draw(s,vs,ps,v),{.2f,.4f,.6f,0});
        for(unsigned mode:{unsigned(D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR),unsigned(D3DTSS_TCI_SPHEREMAP)}) {
            s=base(v);s.stages[0].texcoord=mode;s.render[D3DRS_NORMALIZENORMALS]=TRUE;
            // Choose each normal analytically so reflection of its normalized
            // eye-to-vertex vector is the same +Y direction at all vertices.
            for(auto &vert:v){float length=std::sqrt(vert.reg[0][0]*vert.reg[0][0]+vert.reg[0][1]*vert.reg[0][1]+.25f);
                Color normal={vert.reg[0][0]/length,vert.reg[0][1]/length-1,.5f/length,0};float n=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
                for(unsigned i=0;i<3;++i)normal[i]/=n;vert.reg[3]=normal;}
            Triton9Fixed::vertex(s,vs);ps=coordinatePixel();
            expect("reflection/sphere texgen",renderer.draw(s,vs,ps,v),mode==D3DTSS_TCI_SPHEREMAP?Color{.5f,.5f+.5f/std::sqrt(2.f),0,0}:Color{0,1,0,0});
        }
        for(unsigned bump:{unsigned(D3DTOP_BUMPENVMAP),unsigned(D3DTOP_BUMPENVMAPLUMINANCE)}) {
            s=base(v);s.stages[0].colorOp=bump;s.stages[0].alphaOp=D3DTOP_SELECTARG1;s.stages[0].alphaArg[1]=D3DTA_DIFFUSE;
            s.stages[0].bound=true;s.stages[0].bump[0]=.75f;s.stages[0].bumpScale=2;s.stages[0].bumpOffset=.1f;
            s.stages[1].texcoord=0;s.stages[1].bound=true;s.stages[1].colorOp=s.stages[1].alphaOp=D3DTOP_SELECTARG1;
            s.stages[1].colorArg[1]=s.stages[1].alphaArg[1]=D3DTA_TEXTURE;
            Triton9Fixed::vertex(s,vs);Triton9Fixed::pixel(s,ps);
            wanted={2.f/3,1,.1f,.8f};if(bump==D3DTOP_BUMPENVMAPLUMINANCE)for(auto &x:wanted)x*=.3f;
            expect("bump matrix and luminance",renderer.draw(s,vs,ps,v,{},tex,{},16,16,true),wanted);
        }
        s=base(v);v[0].reg[4][0]=.9f;v[1].reg[4][0]=.1f;v[2].reg[4][0]=.9f;
        for(auto &vert:v)vert.reg[4][1]=.25f;
        s.stages[0].colorOp=s.stages[0].alphaOp=D3DTOP_SELECTARG1;s.stages[0].colorArg[1]=s.stages[0].alphaArg[1]=D3DTA_TEXTURE;s.stages[0].bound=true;
        Triton9Fixed::vertex(s,vs);Triton9Fixed::pixel(s,ps);raster={};raster.PrimitiveType=D3DPT_TRIANGLELIST;raster.PSSamplers[0].TexCoordWrap=1;
        Color wrapped=renderer.draw(s,vs,ps,v,raster,tex,{},16,16,true);raster.PSSamplers[0].TexCoordWrap=0;
        Color unwrapped=renderer.draw(s,vs,ps,v,raster,tex,{},16,16,true);
        if(wrapped[0]<.9f || unwrapped[0]>.7f)throw E_FAIL;
        std::puts("wrapped interpolation positive/negative controls passed");
        // Vertex texture fetch uses independent sampler fixup state.
        s=base(v);ps.tokens=publicProbePixelShader();ps.fixedFunction=false;
        for(unsigned swizzle=0;swizzle<=6;++swizzle) {
            vs.tokens={D3DVS_VERSION(3,0)};vs.constants.clear();vs.fixedFunction=false;
            declaration(vs.tokens,D3DDECLUSAGE_POSITION,0,D3DSPR_INPUT,0);
            declaration(vs.tokens,D3DDECLUSAGE_TEXCOORD,0,D3DSPR_INPUT,4);
            declaration(vs.tokens,D3DDECLUSAGE_POSITION,0,D3DSPR_TEXCRDOUT,0);
            declaration(vs.tokens,D3DDECLUSAGE_COLOR,0,D3DSPR_TEXCRDOUT,1);
            vs.tokens.insert(vs.tokens.end(),{instruction(D3DSIO_DCL,2),0x80000000u|D3DSTT_2D,destination(D3DSPR_SAMPLER,0),
                instruction(D3DSIO_MOV,2),destination(D3DSPR_TEXCRDOUT,0),source(D3DSPR_INPUT,0),
                instruction(D3DSIO_TEXLDL,3),destination(D3DSPR_TEXCRDOUT,1),source(D3DSPR_INPUT,4),source(D3DSPR_SAMPLER,0),D3DVS_END()});
            raster={};raster.VSSamplerSwizzles[0]=swizzle;wanted=tex;
            switch(swizzle){case 1:wanted={tex[0],tex[0],tex[0],tex[3]};break;case 2:wanted={tex[0],tex[3],tex[3],tex[3]};break;
                case 3:wanted={tex[0],tex[1],tex[3],tex[3]};break;case 4:wanted={tex[0],tex[0],tex[0],tex[1]};break;
                case 5:wanted={tex[2],tex[1],tex[0],tex[3]};break;case 6:wanted={tex[0],tex[1],tex[2],1};break;}
            expect("VS texture format swizzle",renderer.draw(s,vs,ps,v,raster),wanted);
        }
        // Programmable ALU and control flow use constant-color independent
        // CPU references, with real converter output executed by DXVK.
        s=base(v);Triton9Fixed::vertex(s,vs);
        auto programmable=[](){Program p;p.tokens={D3DPS_VERSION(3,0)};declaration(p.tokens,D3DDECLUSAGE_COLOR,0,D3DSPR_INPUT,0);
            p.constants={{.25f,.5f,.75f,1},{.5f,.25f,.125f,.5f},{.1f,.2f,.3f,.4f}};return p;};
        auto emit=[](Program &p,unsigned opcode,std::initializer_list<UINT> operands){p.tokens.push_back(instruction(opcode,operands.size()));p.tokens.insert(p.tokens.end(),operands.begin(),operands.end());};
        auto finish=[&](Program &p){emit(p,D3DSIO_MOV,{destination(D3DSPR_COLOROUT,0),source(D3DSPR_TEMP,0)});p.tokens.push_back(D3DPS_END());};
        for(unsigned opcode:{unsigned(D3DSIO_MOV),unsigned(D3DSIO_ADD),unsigned(D3DSIO_SUB),unsigned(D3DSIO_MUL),unsigned(D3DSIO_MAD),
                unsigned(D3DSIO_MIN),unsigned(D3DSIO_MAX),unsigned(D3DSIO_SLT),unsigned(D3DSIO_SGE),unsigned(D3DSIO_DP3),unsigned(D3DSIO_DP4),
                unsigned(D3DSIO_FRC),unsigned(D3DSIO_ABS),unsigned(D3DSIO_RCP),unsigned(D3DSIO_RSQ),unsigned(D3DSIO_EXP),unsigned(D3DSIO_LOG),
                unsigned(D3DSIO_POW),unsigned(D3DSIO_LRP),unsigned(D3DSIO_CMP),unsigned(D3DSIO_DP2ADD)}) {
            ps=programmable();const Color a=ps.constants[0],b=ps.constants[1],q=ps.constants[2];wanted={};
            unsigned aToken=source(D3DSPR_CONST,0),bToken=source(D3DSPR_CONST,1),qToken=source(D3DSPR_CONST,2);
            bool unary=opcode==D3DSIO_MOV||opcode==D3DSIO_FRC||opcode==D3DSIO_ABS||opcode==D3DSIO_RCP||opcode==D3DSIO_RSQ||opcode==D3DSIO_EXP||opcode==D3DSIO_LOG;
            bool ternary=opcode==D3DSIO_MAD||opcode==D3DSIO_LRP||opcode==D3DSIO_CMP||opcode==D3DSIO_DP2ADD;
            if(opcode==D3DSIO_RCP||opcode==D3DSIO_RSQ||opcode==D3DSIO_EXP||opcode==D3DSIO_LOG||opcode==D3DSIO_POW)aToken=source(D3DSPR_CONST,0,0);
            if(opcode==D3DSIO_DP2ADD)qToken=source(D3DSPR_CONST,2,0);
            if(opcode==D3DSIO_POW)bToken=source(D3DSPR_CONST,1,0);
            if(opcode==D3DSIO_ABS)aToken|=D3DSPSM_NEG;
            if(unary)emit(ps,opcode,{destination(D3DSPR_TEMP,0),aToken});
            else if(ternary)emit(ps,opcode,{destination(D3DSPR_TEMP,0),aToken,bToken,qToken});
            else emit(ps,opcode,{destination(D3DSPR_TEMP,0),aToken,bToken});
            finish(ps);
            for(unsigned i=0;i<4;++i)switch(opcode){
            case D3DSIO_MOV:case D3DSIO_ABS:wanted[i]=a[i];break;
            case D3DSIO_ADD:wanted[i]=a[i]+b[i];break;case D3DSIO_SUB:wanted[i]=a[i]-b[i];break;
            case D3DSIO_MUL:wanted[i]=a[i]*b[i];break;case D3DSIO_MAD:wanted[i]=a[i]*b[i]+q[i];break;
            case D3DSIO_MIN:wanted[i]=std::min(a[i],b[i]);break;case D3DSIO_MAX:wanted[i]=std::max(a[i],b[i]);break;
            case D3DSIO_SLT:wanted[i]=a[i]<b[i];break;case D3DSIO_SGE:wanted[i]=a[i]>=b[i];break;
            case D3DSIO_DP3:wanted[i]=a[0]*b[0]+a[1]*b[1]+a[2]*b[2];break;
            case D3DSIO_DP4:wanted[i]=a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];break;
            case D3DSIO_FRC:wanted[i]=a[i]-std::floor(a[i]);break;
            case D3DSIO_RCP:wanted[i]=1/a[0];break;case D3DSIO_RSQ:wanted[i]=1/std::sqrt(a[0]);break;
            case D3DSIO_EXP:wanted[i]=std::exp2(a[0]);break;case D3DSIO_LOG:wanted[i]=std::log2(a[0]);break;
            case D3DSIO_POW:wanted[i]=std::pow(a[0],b[0]);break;case D3DSIO_LRP:wanted[i]=a[i]*b[i]+(1-a[i])*q[i];break;
            case D3DSIO_CMP:wanted[i]=a[i]>=0?b[i]:q[i];break;
            case D3DSIO_DP2ADD:wanted[i]=a[0]*b[0]+a[1]*b[1]+q[0];break;
            }
            char label[64];std::snprintf(label,sizeof(label),"programmable opcode %u",opcode);
            expect(label,renderer.draw(s,vs,ps,v),wanted);
        }
        for(bool condition:{false,true}) {
            ps=programmable();emit(ps,D3DSIO_IFC|(D3DSPC_GT<<16),{source(D3DSPR_CONST,condition?1:0,0),source(D3DSPR_CONST,condition?0:1,0)});
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0)});emit(ps,D3DSIO_ELSE,{});
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,1)});emit(ps,D3DSIO_ENDIF,{});finish(ps);
            expect("dynamic IFC/ELSE",renderer.draw(s,vs,ps,v),ps.constants[condition?0:1]);
        }
        ps=programmable();emit(ps,D3DSIO_DEFI,{destination(D3DSPR_CONSTINT,0),3,0,1,0});
        emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,2)});
        emit(ps,D3DSIO_REP,{source(D3DSPR_CONSTINT,0)});emit(ps,D3DSIO_ADD,{destination(D3DSPR_TEMP,0),source(D3DSPR_TEMP,0),source(D3DSPR_CONST,1)});
        emit(ps,D3DSIO_ENDREP,{});finish(ps);wanted=ps.constants[2];for(unsigned i=0;i<4;++i)wanted[i]+=3*ps.constants[1][i];
        expect("REP integer constants",renderer.draw(s,vs,ps,v),wanted);
        ps=programmable();emit(ps,D3DSIO_DEFB,{destination(D3DSPR_CONSTBOOL,0),1});
        emit(ps,D3DSIO_IF,{source(D3DSPR_CONSTBOOL,0)});emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,1)});
        emit(ps,D3DSIO_ELSE,{});emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,2)});emit(ps,D3DSIO_ENDIF,{});finish(ps);
        expect("IF boolean constants",renderer.draw(s,vs,ps,v),ps.constants[1]);
        ps=programmable();emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0)});emit(ps,D3DSIO_CALL,{source(D3DSPR_LABEL,0)});
        emit(ps,D3DSIO_MOV,{destination(D3DSPR_COLOROUT,0),source(D3DSPR_TEMP,0)});emit(ps,D3DSIO_RET,{});
        emit(ps,D3DSIO_LABEL,{source(D3DSPR_LABEL,0)});emit(ps,D3DSIO_MUL,{destination(D3DSPR_TEMP,0),source(D3DSPR_TEMP,0),source(D3DSPR_CONST,1)});emit(ps,D3DSIO_RET,{});ps.tokens.push_back(D3DPS_END());
        wanted=ps.constants[0];for(unsigned i=0;i<4;++i)wanted[i]*=ps.constants[1][i];expect("CALL/RET",renderer.draw(s,vs,ps,v),wanted);
        for(unsigned derivative:{unsigned(D3DSIO_DSX),unsigned(D3DSIO_DSY)}) {
            ps=programmable();ps.tokens={D3DPS_VERSION(3,0)};declaration(ps.tokens,0,0,D3DSPR_MISCTYPE,0);
            emit(ps,derivative,{destination(D3DSPR_TEMP,0),source(D3DSPR_MISCTYPE,0)});finish(ps);
            expect("pixel derivatives",renderer.draw(s,vs,ps,v),derivative==D3DSIO_DSX?Color{1,0,0,0}:Color{0,1,0,0});
        }
        // Flat interpolation applies to both diffuse and specular. A varying
        // positive control distinguishes it from Gouraud interpolation.
        s=base(v);s.render[D3DRS_SPECULARENABLE]=TRUE;
        v[0].reg[1]={.2f,.3f,.4f,.5f};v[0].reg[2]={.1f,.2f,.3f,.1f};
        v[1].reg[1]={.8f,.1f,.2f,.9f};v[1].reg[2]={.1f,.1f,.1f,.1f};
        v[2].reg[1]={.1f,.8f,.1f,.2f};v[2].reg[2]={.2f,.1f,.1f,.1f};
        raster={};raster.ShadeMode=D3DSHADE_FLAT;
        expect("flat diffuse and specular",draw(renderer,s,v,raster),{.3f,.5f,.7f,.5f});
        for(unsigned fog:{unsigned(D3DFOG_LINEAR),unsigned(D3DFOG_EXP),unsigned(D3DFOG_EXP2)}) {
            s=base(v);raster={};raster.FogEnable=TRUE;raster.FogTableMode=fog;
            float factor=fog==D3DFOG_LINEAR?.5f:std::exp(fog==D3DFOG_EXP?-.5f:-.25f);
            wanted=v[0].reg[1];for(unsigned i=0;i<3;++i)wanted[i]=wanted[i]*factor+(i==2?1-factor:0);
            expect("table Z fog",draw(renderer,s,v,raster),wanted);
            // Direct3D SV_Position.w retains clip W, here two.
            s.projection.m[3][3]=2;raster.WFogEnable=TRUE;
            factor=fog==D3DFOG_LINEAR?0:std::exp(fog==D3DFOG_EXP?-2.f:-4.f);
            wanted=v[0].reg[1];for(unsigned i=0;i<3;++i)wanted[i]=wanted[i]*factor+(i==2?1-factor:0);
            expect("table W fog",draw(renderer,s,v,raster),wanted);
        }
        for(unsigned comparison=D3DCMP_NEVER;comparison<=D3DCMP_ALWAYS;++comparison) {
            s=base(v);s.render[D3DRS_ALPHAREF]=102;raster={};raster.AlphaTestEnable=TRUE;raster.AlphaFunc=comparison;
            bool keep=comparison==D3DCMP_EQUAL||comparison==D3DCMP_LESSEQUAL||comparison==D3DCMP_GREATEREQUAL||comparison==D3DCMP_ALWAYS;
            expect("alpha comparison",draw(renderer,s,v,raster),keep?v[0].reg[1]:Color{-.25f,-.25f,-.25f,-.25f});
        }
        for(unsigned weights=0;weights<=3;++weights) {
            s=base(v);s.inputs.push_back({D3DDECLUSAGE_BLENDWEIGHT,0,5,3});
            s.render[D3DRS_VERTEXBLEND]=weights;
            for(auto &vert:v)vert.reg[5]={.25f,.25f,.25f,0};
            // Each used matrix preserves geometry. Different color/light
            // state is irrelevant; this catches shader/register failures.
            expect("unindexed vertex blend",draw(renderer,s,v),v[0].reg[1]);
        }
        s=base(v);s.inputs.push_back({D3DDECLUSAGE_BLENDINDICES,0,6,4});
        s.render[D3DRS_VERTEXBLEND]=D3DVBF_0WEIGHTS;s.render[D3DRS_INDEXEDVERTEXBLENDENABLE]=TRUE;
        s.normalizedBlendIndices=true;for(auto &vert:v)vert.reg[6]={217.f/255,0,0,0};
        s.world[0].m[3][0]=10;expect("D3DCOLOR palette indices",draw(renderer,s,v),v[0].reg[1]);
        s=base(v);s.inputs.push_back({D3DDECLUSAGE_POSITION,1,7,4});s.render[D3DRS_VERTEXBLEND]=D3DVBF_TWEENING;s.render[D3DRS_TWEENFACTOR]=0x3e800000;
        for(auto &vert:v){vert.reg[7]=vert.reg[0];vert.reg[0][0]-=.25f;vert.reg[7][0]+=.75f;}
        expect("tween vertex positions",draw(renderer,s,v),v[0].reg[1]);
        // Zero reciprocal behavior and masked destination aliasing.
        for(unsigned opcode:{unsigned(D3DSIO_RCP),unsigned(D3DSIO_RSQ),unsigned(D3DSIO_LOG)}) {
            s=base(v);Triton9Fixed::vertex(s,vs);ps=programmable();ps.constants[0]={0,0,0,0};
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0)});
            emit(ps,opcode,{destination(D3DSPR_TEMP,0,D3DSP_WRITEMASK_0),source(D3DSPR_TEMP,0,0)});finish(ps);
            Color actual=renderer.draw(s,vs,ps,v);
            if(opcode==D3DSIO_LOG){if(actual[0]!=-std::numeric_limits<float>::max())throw E_FAIL;}
            else if(!std::isinf(actual[0])||actual[0]<0)throw E_FAIL;
            if(actual[1]!=0||actual[2]!=0||actual[3]!=0)throw E_FAIL;
        }
        // A single expanded point gives an exact lighting oracle: all light
        // vectors and distances are evaluated at (0,0,.5), without triangle
        // interpolation hiding errors in attenuation, range or cone cutoff.
        for(unsigned kind:{unsigned(D3DLIGHT_POINT),unsigned(D3DLIGHT_SPOT)})
        for(unsigned scenario=0;scenario<5;++scenario) {
            s=base(v);v[0].reg[0]={0,0,.5f,1};s.render[D3DRS_POINTSIZE]=0x41000000;
            s.render[D3DRS_LIGHTING]=TRUE;s.render[D3DRS_COLORVERTEX]=FALSE;
            s.render[D3DRS_SPECULARENABLE]=TRUE;s.render[D3DRS_LOCALVIEWER]=scenario&1;
            s.material.Diffuse={.4f,.5f,.6f,.7f};s.material.Specular={.1f,.2f,.3f,1};
            s.material.Ambient={.2f,.2f,.2f,1};s.material.Emissive={.01f,.01f,.01f,1};s.material.Power=8;
            D3DLIGHT9 light{};light.Type=D3DLIGHTTYPE(kind);light.Direction.z=scenario==2?-1:1;
            light.Diffuse={1,1,1,1};light.Specular={1,1,1,1};light.Ambient={.1f,.1f,.1f,1};
            light.Range=scenario==1?.25f:10;light.Attenuation0=2;light.Attenuation1=2;light.Attenuation2=4;
            light.Theta=.5f;light.Phi=1;light.Falloff=scenario==2?0:2;
            s.lights.push_back(light);raster={};raster.PrimitiveType=D3DPT_POINTLIST;raster.PointSizeEnable=TRUE;raster.FillMode=D3DFILL_SOLID;
            if(scenario==3){for(auto &vert:v)vert.reg[3]={0,0,-2,0};s.render[D3DRS_NORMALIZENORMALS]=TRUE;}
            if(scenario==4){s.world[0].m[2][2]=2;s.projection.m[2][2]=.5f;light.Attenuation0=4;light.Attenuation1=0;light.Attenuation2=0;s.lights[0]=light;s.render[D3DRS_NORMALIZENORMALS]=TRUE;}
            float attenuation=scenario==1 || (kind==D3DLIGHT_SPOT && scenario==2) ? 0:.25f;
            wanted={.01f+(.4f+.1f+.02f)*attenuation,.01f+(.5f+.2f+.02f)*attenuation,.01f+(.6f+.3f+.02f)*attenuation,.7f};
            expect("point/spot range attenuation normal viewer",draw(renderer,s,v,raster),wanted);
        }
        // Sequential REP instructions must reuse nesting temporaries instead
        // of wrapping a BYTE register counter after 255 loop occurrences.
        s=base(v);Triton9Fixed::vertex(s,vs);ps=programmable();ps.constants[0]={.001f,.001f,.001f,.001f};ps.constants[1]={0,0,0,0};
        emit(ps,D3DSIO_DEFI,{destination(D3DSPR_CONSTINT,0),1,0,0,0});
        emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,1)});
        for(unsigned i=0;i<260;++i){emit(ps,D3DSIO_REP,{source(D3DSPR_CONSTINT,0)});
            emit(ps,D3DSIO_ADD,{destination(D3DSPR_TEMP,0),source(D3DSPR_TEMP,0),source(D3DSPR_CONST,0)});emit(ps,D3DSIO_ENDREP,{});}
        finish(ps);expect("260 sequential loops",renderer.draw(s,vs,ps,v),{.26f,.26f,.26f,.26f});
        // Last public vertex constant and relative address translation.
        s=base(v);vs={};vs.tokens={D3DVS_VERSION(2,0)};vs.constants.resize(256);
        declaration(vs.tokens,D3DDECLUSAGE_POSITION,0,D3DSPR_INPUT,0);
        vs.constants[0]={255,255,255,255};vs.constants[255]={.13f,.37f,.59f,.83f};
        emit(vs,D3DSIO_MOV,{destination(D3DSPR_RASTOUT,0),source(D3DSPR_INPUT,0)});
        emit(vs,D3DSIO_MOVA,{destination(D3DSPR_ADDR,0,D3DSP_WRITEMASK_0),source(D3DSPR_CONST,0,0)});
        emit(vs,D3DSIO_MOV,{destination(D3DSPR_ATTROUT,0),source(D3DSPR_CONST,0)|D3DSHADER_ADDRESSMODE_MASK,source(D3DSPR_ADDR,0,0)});
        vs.tokens.push_back(D3DVS_END());ps={};ps.tokens=publicProbePixelShader();
        expect("relative public c255",renderer.draw(s,vs,ps,v),vs.constants[255]);
        // Legacy MOV and later MOVA choose different relative constants for
        // negative fractions as well as positive fractions.
        for(bool legacy: {true,false}) for(float address: {-2.4f,-1.6f,-.4f,.4f,1.6f,2.4f}) {
            s=base(v);vs={};vs.tokens={legacy ? D3DVS_VERSION(1,1) : D3DVS_VERSION(2,0)};
            vs.constants.resize(8);
            for(unsigned i=0;i<7;++i)vs.constants[i]={.1f*(i+1),.05f*(i+1),.08f*(i+1),1};
            vs.constants[7]={address,0,0,0};
            auto append=[&](unsigned op,std::initializer_list<UINT> operands) {
                vs.tokens.push_back(legacy ? op : instruction(op,operands.size()));
                vs.tokens.insert(vs.tokens.end(),operands.begin(),operands.end());
            };
            append(D3DSIO_DCL,{triton9_sm2_dcl_semantic(D3DDECLUSAGE_POSITION,0),destination(D3DSPR_INPUT,0)});
            append(D3DSIO_MOV,{destination(D3DSPR_RASTOUT,0),source(D3DSPR_INPUT,0)});
            append(legacy ? D3DSIO_MOV : D3DSIO_MOVA,{destination(D3DSPR_ADDR,0,D3DSP_WRITEMASK_0),source(D3DSPR_CONST,7,0)});
            if(legacy)append(D3DSIO_MOV,{destination(D3DSPR_ATTROUT,0),source(D3DSPR_CONST,3)|D3DSHADER_ADDRESSMODE_MASK});
            else append(D3DSIO_MOV,{destination(D3DSPR_ATTROUT,0),source(D3DSPR_CONST,3)|D3DSHADER_ADDRESSMODE_MASK,source(D3DSPR_ADDR,0,0)});
            vs.tokens.push_back(D3DVS_END());ps={};ps.tokens=publicProbePixelShader();
            const int index=3+int(legacy ? std::floor(address) : std::round(address));
            expect("MOV/MOVA relative rounding",renderer.draw(s,vs,ps,v),vs.constants[index]);
        }
        // Four nested loops are legal; a fifth must fail before conversion.
        for(unsigned depth:{4u,5u}) {
            ps=programmable();emit(ps,D3DSIO_DEFI,{destination(D3DSPR_CONSTINT,0),1,0,0,0});
            for(unsigned i=0;i<depth;++i)emit(ps,D3DSIO_REP,{source(D3DSPR_CONSTINT,0)});
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0)});
            for(unsigned i=0;i<depth;++i)emit(ps,D3DSIO_ENDREP,{});finish(ps);
            bool valid=ShaderConv::ValidateLegacyShader(ps.tokens.data(),ps.tokens.size()*4,false);
            if(valid!=(depth==4))throw E_FAIL;
            if(valid)expect("four nested loops",renderer.draw(s,vs,ps,v),ps.constants[0]);
        }
        for(unsigned dimension:{2u,3u,4u}) for(unsigned opcode:{unsigned(D3DSIO_TEX),unsigned(D3DSIO_TEXLDL),unsigned(D3DSIO_TEXLDD)}) {
            s=base(v);Triton9Fixed::vertex(s,vs);ps={};ps.tokens={D3DPS_VERSION(3,0)};ps.constants={{.25f,.75f,.5f,0},{0,0,0,0}};
            unsigned type=dimension==2?D3DSTT_2D:dimension==3?D3DSTT_VOLUME:D3DSTT_CUBE;
            ps.tokens.insert(ps.tokens.end(),{instruction(D3DSIO_DCL,2),0x80000000u|type,destination(D3DSPR_SAMPLER,15)});
            if(opcode==D3DSIO_TEXLDD)emit(ps,opcode,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0),source(D3DSPR_SAMPLER,15),source(D3DSPR_CONST,1),source(D3DSPR_CONST,1)});
            else emit(ps,opcode,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0),source(D3DSPR_SAMPLER,15)});
            finish(ps);expect("sampler15 dimensional/LOD/gradient",renderer.draw(s,vs,ps,v,{},tex,{},16,16,false,dimension),tex);
        }
        // Wrap states eight through fifteen address semantic indices, not
        // fixed-function stage count. Exercise the last index through GS.
        for(unsigned index:{8u,15u}) {
            s=base(v);v[0].reg[4][0]=.9f;v[1].reg[4][0]=.1f;v[2].reg[4][0]=.9f;
            vs={};vs.tokens={D3DVS_VERSION(3,0)};
            declaration(vs.tokens,D3DDECLUSAGE_POSITION,0,D3DSPR_INPUT,0);declaration(vs.tokens,D3DDECLUSAGE_TEXCOORD,0,D3DSPR_INPUT,4);
            declaration(vs.tokens,D3DDECLUSAGE_POSITION,0,D3DSPR_TEXCRDOUT,0);declaration(vs.tokens,D3DDECLUSAGE_TEXCOORD,index,D3DSPR_TEXCRDOUT,1);
            emit(vs,D3DSIO_MOV,{destination(D3DSPR_TEXCRDOUT,0),source(D3DSPR_INPUT,0)});emit(vs,D3DSIO_MOV,{destination(D3DSPR_TEXCRDOUT,1),source(D3DSPR_INPUT,4)});vs.tokens.push_back(D3DVS_END());
            ps={};ps.tokens={D3DPS_VERSION(3,0)};declaration(ps.tokens,D3DDECLUSAGE_TEXCOORD,index,D3DSPR_INPUT,0);
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_COLOROUT,0),source(D3DSPR_INPUT,0)});ps.tokens.push_back(D3DPS_END());
            raster={};raster.PrimitiveType=D3DPT_TRIANGLELIST;raster.PSSamplers[index].TexCoordWrap=1;
            Color result=renderer.draw(s,vs,ps,v,raster);if(result[0]<.9f)throw E_FAIL;
            raster.PSSamplers[index].TexCoordWrap=0;result=renderer.draw(s,vs,ps,v,raster);if(result[0]>.7f)throw E_FAIL;
        }
        s=base(v);v[0].reg[0]={0,0,.5f,1};s.render[D3DRS_POINTSIZE]=0x41000000;
        Triton9Fixed::vertex(s,vs);ps=coordinatePixel();raster={};raster.PrimitiveType=D3DPT_POINTLIST;
        raster.PointSizeEnable=TRUE;raster.PointSpriteEnable=TRUE;raster.FillMode=D3DFILL_SOLID;
        expect("point sprite generated coordinate",renderer.draw(s,vs,ps,v,raster),{.5f,.5f,0,1});
        s.render[D3DRS_POINTSIZE]=0x3f800000;s.render[D3DRS_POINTSCALEENABLE]=TRUE;s.render[D3DRS_POINTSCALE_A]=0x41800000;
        Triton9Fixed::vertex(s,vs);Triton9Fixed::pixel(s,ps);raster.PointSpriteEnable=FALSE;
        expect("distance scaled point inside",renderer.draw(s,vs,ps,v,raster,tex,{},18,16),v[0].reg[1]);
        expect("distance scaled point outside",renderer.draw(s,vs,ps,v,raster,tex,{},24,16),{-.25f,-.25f,-.25f,-.25f});
        s.render[D3DRS_POINTSCALEENABLE]=FALSE;s.render[D3DRS_POINTSIZE_MIN]=0x41000000;
        Triton9Fixed::vertex(s,vs);expect("point minimum clamp",renderer.draw(s,vs,ps,v,raster,tex,{},18,16),v[0].reg[1]);
        s.render[D3DRS_POINTSIZE_MIN]=0x3f800000;s.render[D3DRS_POINTSIZE]=0x41000000;s.render[D3DRS_POINTSIZE_MAX]=0x40800000;
        Triton9Fixed::vertex(s,vs);expect("point maximum clamp",renderer.draw(s,vs,ps,v,raster,tex,{},19,16),{-.25f,-.25f,-.25f,-.25f});
        s=base(v);for(auto &vert:v){vert.reg[0][0]*=.5f;vert.reg[0][1]*=.5f;}s.render[D3DRS_POINTSIZE]=0x40800000;
        Triton9Fixed::vertex(s,vs);Triton9Fixed::pixel(s,ps);raster={};raster.PrimitiveType=D3DPT_TRIANGLELIST;raster.FillMode=D3DFILL_POINT;raster.PointSizeEnable=TRUE;
        expect("point fill rejects triangle interior",renderer.draw(s,vs,ps,v,raster),{-.25f,-.25f,-.25f,-.25f});
        expect("point fill vertex footprint",renderer.draw(s,vs,ps,v,raster,tex,{},8,24),v[0].reg[1]);
        s=base(v);Triton9Fixed::vertex(s,vs);ps={};
        ps.tokens={D3DPS_VERSION(1,1),D3DSIO_MOV,destination(D3DSPR_TEMP,0,D3DSP_WRITEMASK_0|D3DSP_WRITEMASK_1|D3DSP_WRITEMASK_2),source(D3DSPR_INPUT,0),
            D3DSIO_MOV|D3DSI_COISSUE,destination(D3DSPR_TEMP,0,D3DSP_WRITEMASK_3),source(D3DSPR_INPUT,1),D3DPS_END()};
        expect("SM1 coissue RGB alpha",renderer.draw(s,vs,ps,v),{.3f,.6f,.8f,.7f});
        ps.tokens={D3DPS_VERSION(1,4),D3DSIO_TEX,destination(D3DSPR_TEMP,0),source(D3DSPR_TEXTURE,0),D3DSIO_PHASE,
            D3DSIO_MOV,destination(D3DSPR_TEMP,0),source(D3DSPR_INPUT,0),D3DPS_END()};raster={};raster.PSSamplers[0].TextureType=ShaderConv::TEXTURETYPE_2D;
        expect("SM1.4 phase",renderer.draw(s,vs,ps,v,raster),v[0].reg[1]);
        for(unsigned opcode:{unsigned(D3DSIO_TEXREG2AR),unsigned(D3DSIO_TEXREG2GB),unsigned(D3DSIO_TEXREG2RGB)}) {
            ps.tokens={D3DPS_VERSION(1,3),D3DSIO_TEX,destination(D3DSPR_TEXTURE,0),opcode,destination(D3DSPR_TEXTURE,1),source(D3DSPR_TEXTURE,0),
                D3DSIO_MOV,destination(D3DSPR_TEMP,0),source(D3DSPR_TEXTURE,1),D3DPS_END()};
            raster.PSSamplers[1].TextureType=ShaderConv::TEXTURETYPE_2D;
            wanted=opcode==D3DSIO_TEXREG2AR?Color{1,1.f/3,.1f,.8f}:opcode==D3DSIO_TEXREG2GB?Color{1,0,.1f,.8f}:Color{1.f/3,1,.1f,.8f};
            expect("SM1 dependent texture read",renderer.draw(s,vs,ps,v,raster,tex,{},16,16,true),wanted);
        }
        s=base(v);Triton9Fixed::vertex(s,vs);ps=programmable();ps.constants[0]={.1f,.1f,.1f,.1f};ps.constants[1]={0,0,0,0};
        emit(ps,D3DSIO_DEFI,{destination(D3DSPR_CONSTINT,0),3,0,0,0});
        emit(ps,D3DSIO_DEFI,{destination(D3DSPR_CONSTINT,1),2,0,0,0});
        emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,1)});
        emit(ps,D3DSIO_REP,{source(D3DSPR_CONSTINT,0)});emit(ps,D3DSIO_CALL,{source(D3DSPR_LABEL,7)});emit(ps,D3DSIO_ENDREP,{});
        emit(ps,D3DSIO_MOV,{destination(D3DSPR_COLOROUT,0),source(D3DSPR_TEMP,0)});emit(ps,D3DSIO_RET,{});
        emit(ps,D3DSIO_LABEL,{source(D3DSPR_LABEL,7)});emit(ps,D3DSIO_REP,{source(D3DSPR_CONSTINT,1)});
        emit(ps,D3DSIO_ADD,{destination(D3DSPR_TEMP,0),source(D3DSPR_TEMP,0),source(D3DSPR_CONST,0)});
        emit(ps,D3DSIO_ENDREP,{});emit(ps,D3DSIO_RET,{});ps.tokens.push_back(D3DPS_END());
        expect("caller and callee loop temporaries",renderer.draw(s,vs,ps,v),{.6f,.6f,.6f,.6f});
        s=base(v);vs={};vs.tokens={D3DVS_VERSION(2,0)};vs.constants.resize(13);
        vs.constants[5]={.1f,.1f,.1f,.1f};vs.constants[6]={.2f,.2f,.2f,.2f};vs.constants[12]={.05f,.05f,.05f,.05f};
        declaration(vs.tokens,D3DDECLUSAGE_POSITION,0,D3DSPR_INPUT,0);
        emit(vs,D3DSIO_DEFI,{destination(D3DSPR_CONSTINT,0),2,5,1,0});emit(vs,D3DSIO_DEFI,{destination(D3DSPR_CONSTINT,1),1,12,1,0});
        emit(vs,D3DSIO_MOV,{destination(D3DSPR_RASTOUT,0),source(D3DSPR_INPUT,0)});
        emit(vs,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0)});
        emit(vs,D3DSIO_LOOP,{source(D3DSPR_LOOP,0),source(D3DSPR_CONSTINT,0)});emit(vs,D3DSIO_CALL,{source(D3DSPR_LABEL,7)});
        emit(vs,D3DSIO_ADD,{destination(D3DSPR_TEMP,0),source(D3DSPR_TEMP,0),source(D3DSPR_CONST,0)|D3DSHADER_ADDRESSMODE_MASK,source(D3DSPR_LOOP,0,0)});
        emit(vs,D3DSIO_ENDLOOP,{});emit(vs,D3DSIO_MOV,{destination(D3DSPR_ATTROUT,0),source(D3DSPR_TEMP,0)});emit(vs,D3DSIO_RET,{});
        emit(vs,D3DSIO_LABEL,{source(D3DSPR_LABEL,7)});emit(vs,D3DSIO_LOOP,{source(D3DSPR_LOOP,0),source(D3DSPR_CONSTINT,1)});
        emit(vs,D3DSIO_ADD,{destination(D3DSPR_TEMP,0),source(D3DSPR_TEMP,0),source(D3DSPR_CONST,0)|D3DSHADER_ADDRESSMODE_MASK,source(D3DSPR_LOOP,0,0)});
        emit(vs,D3DSIO_ENDLOOP,{});emit(vs,D3DSIO_RET,{});vs.tokens.push_back(D3DVS_END());ps={};ps.tokens=publicProbePixelShader();
        expect("CALL restores caller aL relative address",renderer.draw(s,vs,ps,v),{.4f,.4f,.4f,.4f});
        for(unsigned version:{D3DPS_VERSION(2,0),D3DPS_VERSION(2,1),D3DPS_VERSION(3,0)})
        for(unsigned negate=0;negate<2;++negate)for(unsigned condition=0;condition<2;++condition) {
            s=base(v);Triton9Fixed::vertex(s,vs);ps=programmable();ps.tokens[0]=version;
            emit(ps,D3DSIO_SETP|(D3DSPC_GT<<16),{destination(D3DSPR_PREDICATE,0),source(D3DSPR_CONST,condition?1:0,0),source(D3DSPR_CONST,condition?0:1,0)});
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,1)});
            emit(ps,D3DSIO_MOV|D3DSHADER_INSTRUCTION_PREDICATED,{destination(D3DSPR_TEMP,0,D3DSP_WRITEMASK_0),source(D3DSPR_PREDICATE,0,0)|(negate?D3DSPSM_NOT:0),source(D3DSPR_CONST,0)});finish(ps);
            wanted=ps.constants[1];if(condition!=negate)wanted[0]=ps.constants[0][0];
            expect("predicated masked destination",renderer.draw(s,vs,ps,v),wanted);
        }
        for(unsigned opcode:{unsigned(D3DSIO_BREAK),unsigned(D3DSIO_BREAKC),unsigned(D3DSIO_BREAKP)})for(unsigned condition=0;condition<2;++condition) {
            s=base(v);Triton9Fixed::vertex(s,vs);ps=programmable();ps.constants[0]={.1f,.1f,.1f,.1f};ps.constants[1]={0,0,0,0};
            emit(ps,D3DSIO_DEFI,{destination(D3DSPR_CONSTINT,0),3,0,0,0});
            emit(ps,D3DSIO_SETP|(D3DSPC_GT<<16),{destination(D3DSPR_PREDICATE,0),source(D3DSPR_CONST,condition?0:1,0),source(D3DSPR_CONST,condition?1:0,0)});
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,1)});emit(ps,D3DSIO_REP,{source(D3DSPR_CONSTINT,0)});
            emit(ps,D3DSIO_ADD,{destination(D3DSPR_TEMP,0),source(D3DSPR_TEMP,0),source(D3DSPR_CONST,0)});
            if(opcode==D3DSIO_BREAK)emit(ps,opcode,{});
            if(opcode==D3DSIO_BREAKC)emit(ps,opcode|(D3DSPC_GT<<16),{source(D3DSPR_CONST,condition?0:1,0),source(D3DSPR_CONST,condition?1:0,0)});
            if(opcode==D3DSIO_BREAKP)emit(ps,opcode,{source(D3DSPR_PREDICATE,0,0)});
            emit(ps,D3DSIO_ENDREP,{});finish(ps);float value=condition||opcode==D3DSIO_BREAK?.1f:.3f;
            expect("conditional loop break",renderer.draw(s,vs,ps,v),{value,value,value,value});
        }
        for(unsigned target=0;target<4;++target) {
            s=base(v);Triton9Fixed::vertex(s,vs);ps=programmable();ps.constants.push_back({.8f,.6f,.4f,.2f});
            for(unsigned i=0;i<4;++i)emit(ps,D3DSIO_MOV,{destination(D3DSPR_COLOROUT,i),source(D3DSPR_CONST,i)});ps.tokens.push_back(D3DPS_END());
            expect("MRT distinct outputs",renderer.draw(s,vs,ps,v,{},tex,{},16,16,false,2,false,-1,target),ps.constants[target]);
        }
        for(float depth:{.25f,.75f}) {
            s=base(v);Triton9Fixed::vertex(s,vs);ps=programmable();ps.constants[1]={depth,depth,depth,depth};
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_COLOROUT,0),source(D3DSPR_CONST,0)});
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_DEPTHOUT,0),source(D3DSPR_CONST,1,0)});ps.tokens.push_back(D3DPS_END());
            expect("pixel depth output comparison",renderer.draw(s,vs,ps,v,{},tex,{},16,16,false,2,false,.5f),depth<.5f?ps.constants[0]:Color{-.25f,-.25f,-.25f,-.25f});
        }
        for(unsigned lod=0;lod<3;++lod) {
            s=base(v);Triton9Fixed::vertex(s,vs);ps={};ps.tokens={D3DPS_VERSION(3,0)};ps.constants={{.25f,.75f,0,float(lod)}};
            ps.tokens.insert(ps.tokens.end(),{instruction(D3DSIO_DCL,2),0x80000000u|D3DSTT_2D,destination(D3DSPR_SAMPLER,15)});
            emit(ps,D3DSIO_TEXLDL,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0),source(D3DSPR_SAMPLER,15)});finish(ps);
            wanted=lod==0?tex:lod==1?Color{.2f,.4f,.6f,.8f}:Color{.9f,.7f,.5f,.3f};
            expect("PS explicit mip selection",renderer.draw(s,vs,ps,v,{},tex,{},16,16,false,2,true),wanted);
        }
        for(unsigned lod=0;lod<3;++lod) {
            s=base(v);vs={};vs.tokens={D3DVS_VERSION(3,0)};vs.constants={{.25f,.75f,0,float(lod)}};
            declaration(vs.tokens,D3DDECLUSAGE_POSITION,0,D3DSPR_INPUT,0);
            declaration(vs.tokens,D3DDECLUSAGE_POSITION,0,D3DSPR_TEXCRDOUT,0);declaration(vs.tokens,D3DDECLUSAGE_COLOR,0,D3DSPR_TEXCRDOUT,1);
            vs.tokens.insert(vs.tokens.end(),{instruction(D3DSIO_DCL,2),0x80000000u|D3DSTT_2D,destination(D3DSPR_SAMPLER,3)});
            emit(vs,D3DSIO_MOV,{destination(D3DSPR_TEXCRDOUT,0),source(D3DSPR_INPUT,0)});
            emit(vs,D3DSIO_TEXLDL,{destination(D3DSPR_TEXCRDOUT,1),source(D3DSPR_CONST,0),source(D3DSPR_SAMPLER,3)});vs.tokens.push_back(D3DVS_END());
            ps={};ps.tokens=publicProbePixelShader();wanted=lod==0?tex:lod==1?Color{.2f,.4f,.6f,.8f}:Color{.9f,.7f,.5f,.3f};
            expect("VS sampler3 explicit mip selection",renderer.draw(s,vs,ps,v,{},tex,{},16,16,false,2,true),wanted);
        }
        for(unsigned lod=1;lod<3;++lod) {
            s=base(v);Triton9Fixed::vertex(s,vs);ps={};ps.tokens={D3DPS_VERSION(3,0)};
            float derivative=lod==1?.5f:1;ps.constants={{.25f,.75f,0,0},{derivative,0,0,0},{0,derivative,0,0}};
            ps.tokens.insert(ps.tokens.end(),{instruction(D3DSIO_DCL,2),0x80000000u|D3DSTT_2D,destination(D3DSPR_SAMPLER,0)});
            emit(ps,D3DSIO_TEXLDD,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0),source(D3DSPR_SAMPLER,0),source(D3DSPR_CONST,1),source(D3DSPR_CONST,2)});finish(ps);
            wanted=lod==1?Color{.2f,.4f,.6f,.8f}:Color{.9f,.7f,.5f,.3f};
            expect("gradient selects mip",renderer.draw(s,vs,ps,v,{},tex,{},16,16,false,2,true),wanted);
        }
        s=base(v);s.inputs[4].components=4;for(auto &vert:v)vert.reg[4]={(vert.reg[0][0]+1)*.5f,(1-vert.reg[0][1])*.5f,0,4};
        Triton9Fixed::vertex(s,vs);ps={};ps.tokens={D3DPS_VERSION(3,0)};declaration(ps.tokens,D3DDECLUSAGE_TEXCOORD,0,D3DSPR_INPUT,0);
        ps.tokens.insert(ps.tokens.end(),{instruction(D3DSIO_DCL,2),0x80000000u|D3DSTT_2D,destination(D3DSPR_SAMPLER,0)});
        emit(ps,D3DSIO_TEX|D3DSI_TEXLD_BIAS,{destination(D3DSPR_TEMP,0),source(D3DSPR_INPUT,0),source(D3DSPR_SAMPLER,0)});finish(ps);
        expect("sample bias selects mip",renderer.draw(s,vs,ps,v,{},tex,{},16,16,false,2,true),{.2f,.4f,.6f,.8f});
        s=base(v);Triton9Fixed::vertex(s,vs);ps={};ps.tokens={D3DPS_VERSION(3,0)};ps.constants={{.25f,.75f,0,2}};
        ps.tokens.insert(ps.tokens.end(),{instruction(D3DSIO_DCL,2),0x80000000u|D3DSTT_2D,destination(D3DSPR_SAMPLER,0)});
        emit(ps,D3DSIO_TEX|D3DSI_TEXLD_PROJECT,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0),source(D3DSPR_SAMPLER,0)});finish(ps);
        expect("projected sample instruction",renderer.draw(s,vs,ps,v,{},tex,{},16,16,true),{0,1.f/3,.1f,.8f});
        for(unsigned opcode:{unsigned(D3DSIO_TEXBEM),unsigned(D3DSIO_TEXBEML)}) {
            s=base(v);s.stages[1].texcoord=0;s.stages[1].bump[0]=.75f;s.stages[1].bumpScale=2;s.stages[1].bumpOffset=.1f;
            Triton9Fixed::vertex(s,vs);ps={};ps.tokens={D3DPS_VERSION(1,3),D3DSIO_TEX,destination(D3DSPR_TEXTURE,0),opcode,destination(D3DSPR_TEXTURE,1),source(D3DSPR_TEXTURE,0),
                D3DSIO_MOV,destination(D3DSPR_TEMP,0),source(D3DSPR_TEXTURE,1),D3DPS_END()};raster={};raster.PSSamplers[0].TextureType=raster.PSSamplers[1].TextureType=ShaderConv::TEXTURETYPE_2D;
            wanted={2.f/3,1,.1f,.8f};if(opcode==D3DSIO_TEXBEML)for(auto &component:wanted)component*=.3f;
            expect("SM1 bump/luminance instruction",renderer.draw(s,vs,ps,v,raster,tex,{},16,16,true),wanted);
            s.stages[1].bump[0]=0;s.stages[1].bump[1]=-.6f;s.stages[1].bump[2]=.3f;
            wanted={2.f/3,2.f/3,.1f,.8f};if(opcode==D3DSIO_TEXBEML)for(auto &component:wanted)component*=.3f;
            expect("SM1 off-diagonal bump matrix",renderer.draw(s,vs,ps,v,raster,tex,{},16,16,true),wanted);
        }
        for(unsigned dimensions=2;dimensions<=3;++dimensions) {
            s=base(v);s.inputs.push_back({D3DDECLUSAGE_TEXCOORD,1,5,3});s.inputs.push_back({D3DDECLUSAGE_TEXCOORD,2,6,3});s.inputs.push_back({D3DDECLUSAGE_TEXCOORD,3,7,3});
            for(auto &vert:v){vert.reg[5]={1,0,0,0};vert.reg[6]={0,1,0,0};vert.reg[7]={0,0,1,0};}
            Triton9Fixed::vertex(s,vs);ps={};ps.tokens={D3DPS_VERSION(1,3),D3DSIO_TEX,destination(D3DSPR_TEXTURE,0)};
            ps.tokens.insert(ps.tokens.end(),{dimensions==2?D3DSIO_TEXM3x2PAD:D3DSIO_TEXM3x3PAD,destination(D3DSPR_TEXTURE,1),source(D3DSPR_TEXTURE,0)});
            if(dimensions==3)ps.tokens.insert(ps.tokens.end(),{D3DSIO_TEXM3x3PAD,destination(D3DSPR_TEXTURE,2),source(D3DSPR_TEXTURE,0)});
            ps.tokens.insert(ps.tokens.end(),{dimensions==2?D3DSIO_TEXM3x2TEX:D3DSIO_TEXM3x3TEX,destination(D3DSPR_TEXTURE,dimensions),source(D3DSPR_TEXTURE,0),
                D3DSIO_MOV,destination(D3DSPR_TEMP,0),source(D3DSPR_TEXTURE,dimensions),D3DPS_END()});raster={};for(unsigned i=0;i<4;++i)raster.PSSamplers[i].TextureType=ShaderConv::TEXTURETYPE_2D;
            expect("SM1 matrix texture instructions",renderer.draw(s,vs,ps,v,raster,tex,{},16,16,true),{1.f/3,1,.1f,.8f});
        }
        s=base(v);v[0].reg[0]={.375f,0,.5f,1};s.render[D3DRS_POINTSIZE]=0x41000000;
        s.render[D3DRS_FOGENABLE]=TRUE;s.render[D3DRS_FOGVERTEXMODE]=D3DFOG_LINEAR;s.render[D3DRS_RANGEFOGENABLE]=TRUE;s.render[D3DRS_FOGEND]=0x3f800000;
        Triton9Fixed::vertex(s,vs);Triton9Fixed::pixel(s,ps);raster={};raster.PrimitiveType=D3DPT_POINTLIST;raster.PointSizeEnable=TRUE;raster.FogEnable=TRUE;raster.FillMode=D3DFILL_SOLID;
        wanted=v[0].reg[1];for(unsigned i=0;i<3;++i)wanted[i]=wanted[i]*.375f+(i==2?.625f:0);
        expect("range fog differs from z fog",renderer.draw(s,vs,ps,v,raster,tex,{},22,16),wanted);
        s=base(v);s.stages[0].texcoord=D3DTSS_TCI_CAMERASPACEPOSITION;
        for(auto &vert:v){vert.reg[0][0]=0;vert.reg[0][1]=0;}s.render[D3DRS_POINTSIZE]=0x41000000;
        Triton9Fixed::vertex(s,vs);ps=coordinatePixel();raster={};raster.PrimitiveType=D3DPT_POINTLIST;raster.PointSizeEnable=TRUE;raster.FillMode=D3DFILL_SOLID;
        expect("camera position texgen",renderer.draw(s,vs,ps,v,raster),{0,0,.5f,0});
        for(unsigned negate=0;negate<2;++negate)for(unsigned condition=0;condition<2;++condition) {
            s=base(v);vs={};vs.tokens={D3DVS_VERSION(3,0)};vs.constants={{.2f,.3f,.4f,.5f},{.8f,.7f,.6f,.9f}};
            declaration(vs.tokens,D3DDECLUSAGE_POSITION,0,D3DSPR_INPUT,0);
            declaration(vs.tokens,D3DDECLUSAGE_POSITION,0,D3DSPR_TEXCRDOUT,0);declaration(vs.tokens,D3DDECLUSAGE_COLOR,0,D3DSPR_TEXCRDOUT,1);
            emit(vs,D3DSIO_MOV,{destination(D3DSPR_TEXCRDOUT,0),source(D3DSPR_INPUT,0)});
            emit(vs,D3DSIO_SETP|(D3DSPC_GT<<16),{destination(D3DSPR_PREDICATE,0),source(D3DSPR_CONST,condition?1:0,0),source(D3DSPR_CONST,condition?0:1,0)});
            emit(vs,D3DSIO_MOV,{destination(D3DSPR_TEXCRDOUT,1),source(D3DSPR_CONST,1)});
            emit(vs,D3DSIO_MOV|D3DSHADER_INSTRUCTION_PREDICATED,{destination(D3DSPR_TEXCRDOUT,1,D3DSP_WRITEMASK_0),source(D3DSPR_PREDICATE,0,0)|(negate?D3DSPSM_NOT:0),source(D3DSPR_CONST,0)});
            vs.tokens.push_back(D3DVS_END());ps={};ps.tokens=publicProbePixelShader();wanted=vs.constants[1];if(condition!=negate)wanted[0]=vs.constants[0][0];
            expect("VS predicated masked output",renderer.draw(s,vs,ps,v),wanted);
        }
        for(unsigned predicate=0;predicate<2;++predicate)for(unsigned negate=0;negate<2;++negate)for(unsigned condition=0;condition<2;++condition) {
            s=base(v);Triton9Fixed::vertex(s,vs);ps=programmable();
            if(predicate)emit(ps,D3DSIO_SETP|(D3DSPC_GT<<16),{destination(D3DSPR_PREDICATE,0),source(D3DSPR_CONST,condition?1:0,0),source(D3DSPR_CONST,condition?0:1,0)});
            else emit(ps,D3DSIO_DEFB,{destination(D3DSPR_CONSTBOOL,0),condition});
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0)});
            emit(ps,D3DSIO_CALLNZ,{source(D3DSPR_LABEL,7),source(predicate?D3DSPR_PREDICATE:D3DSPR_CONSTBOOL,0,0)|(negate?D3DSPSM_NOT:0)});
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_COLOROUT,0),source(D3DSPR_TEMP,0)});emit(ps,D3DSIO_RET,{});
            emit(ps,D3DSIO_LABEL,{source(D3DSPR_LABEL,7)});emit(ps,D3DSIO_MUL,{destination(D3DSPR_TEMP,0),source(D3DSPR_TEMP,0),source(D3DSPR_CONST,1)});
            emit(ps,D3DSIO_RET,{});ps.tokens.push_back(D3DPS_END());wanted=ps.constants[0];if(condition!=negate)for(unsigned i=0;i<4;++i)wanted[i]*=ps.constants[1][i];
            expect("CALLNZ boolean and predicate branches",renderer.draw(s,vs,ps,v),wanted);
        }
        for(unsigned scenario=0;scenario<3;++scenario) {
            s=base(v);for(auto &vert:v)vert.reg[1]={scenario==0?.25f:.75f,scenario==2?0.f:1.f,0,1};
            Triton9Fixed::vertex(s,vs);ps={};ps.constants={{.2f,.4f,.6f,.8f}};
            ps.tokens={D3DPS_VERSION(1,4),D3DSIO_MOV,destination(D3DSPR_TEMP,5),source(D3DSPR_INPUT,0),D3DSIO_PHASE,
                D3DSIO_TEXDEPTH,destination(D3DSPR_TEMP,5),D3DSIO_MOV,destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0),D3DPS_END()};
            expect("SM1.4 texture depth and zero divisor",renderer.draw(s,vs,ps,v,{},tex,{},16,16,false,2,false,.5f),scenario==0?ps.constants[0]:Color{-.25f,-.25f,-.25f,-.25f});
        }
        for(unsigned opcode:{unsigned(D3DSIO_TEXDP3),unsigned(D3DSIO_TEXDP3TEX)}) {
            s=base(v);s.inputs.push_back({D3DDECLUSAGE_TEXCOORD,1,5,3});for(auto &vert:v)vert.reg[5]={.5f,.25f,0,0};
            Triton9Fixed::vertex(s,vs);ps={};ps.tokens={D3DPS_VERSION(1,3),D3DSIO_TEXCOORD,destination(D3DSPR_TEXTURE,0),
                opcode,destination(D3DSPR_TEXTURE,1),source(D3DSPR_TEXTURE,0),D3DSIO_MOV,destination(D3DSPR_TEMP,0),source(D3DSPR_TEXTURE,1),D3DPS_END()};
            raster={};raster.PSSamplers[1].TextureType=ShaderConv::TEXTURETYPE_2D;
            wanted=opcode==D3DSIO_TEXDP3?Color{.3125f,.3125f,.3125f,.3125f}:Color{1.f/3,0,.1f,.8f};
            expect("SM1 dot-product texture instruction",renderer.draw(s,vs,ps,v,raster,tex,{},16,16,true),wanted);
        }
        // SPEC reflection must divide by N.N. Cube addressing hides a
        // uniform scale error, so use a spatially patterned volume texture.
        for(unsigned opcode:{unsigned(D3DSIO_TEXM3x3SPEC),unsigned(D3DSIO_TEXM3x3VSPEC)}) {
            s=base(v);s.inputs[4].components=3;
            s.inputs.push_back({D3DDECLUSAGE_TEXCOORD,1,5,4});s.inputs.push_back({D3DDECLUSAGE_TEXCOORD,2,6,4});s.inputs.push_back({D3DDECLUSAGE_TEXCOORD,3,7,4});
            for(auto &vert:v){vert.reg[4]={.25f,.5f,.25f,1};vert.reg[5]={1,0,0,.1f};vert.reg[6]={0,1,0,.2f};vert.reg[7]={0,0,1,.3f};}
            Triton9Fixed::vertex(s,vs);ps={};ps.constants={{.1f,.2f,.3f,0}};
            ps.tokens={D3DPS_VERSION(1,3),D3DSIO_TEXCOORD,destination(D3DSPR_TEXTURE,0),
                D3DSIO_TEXM3x3PAD,destination(D3DSPR_TEXTURE,1),source(D3DSPR_TEXTURE,0),
                D3DSIO_TEXM3x3PAD,destination(D3DSPR_TEXTURE,2),source(D3DSPR_TEXTURE,0),
                opcode,destination(D3DSPR_TEXTURE,3),source(D3DSPR_TEXTURE,0)};
            if(opcode==D3DSIO_TEXM3x3SPEC)ps.tokens.push_back(source(D3DSPR_CONST,0));
            ps.tokens.insert(ps.tokens.end(),{D3DSIO_MOV,destination(D3DSPR_TEMP,0),source(D3DSPR_TEXTURE,3),D3DPS_END()});
            raster={};raster.PSSamplers[3].TextureType=ShaderConv::TEXTURETYPE_VOLUME;
            expect("SM1 specular volume reflection",renderer.draw(s,vs,ps,v,raster,tex,{},16,16,true,3),{0,1.f/3,0,.8f});
        }
        // Compare actual depth-buffer contents, including division by a
        // nonunit denominator and the specified zero-denominator result.
        for(unsigned scenario=0;scenario<4;++scenario) {
            s=base(v);s.inputs[4].components=3;
            s.inputs.push_back({D3DDECLUSAGE_TEXCOORD,1,5,3});s.inputs.push_back({D3DDECLUSAGE_TEXCOORD,2,6,3});
            for(auto &vert:v){vert.reg[4]={1,.5f,.25f,1};
                vert.reg[5]={scenario==1?.6f:.1f,.2f,.2f,0};
                vert.reg[6]=scenario==3?Color{0,0,0,0}:scenario==2?Color{.25f,.25f,.5f,0}:Color{.5f,.5f,1,0};}
            Triton9Fixed::vertex(s,vs);ps={};ps.constants={{.2f,.4f,.6f,.8f}};
            ps.tokens={D3DPS_VERSION(1,3),D3DSIO_TEXCOORD,destination(D3DSPR_TEXTURE,0),
                D3DSIO_TEXM3x2PAD,destination(D3DSPR_TEXTURE,1),source(D3DSPR_TEXTURE,0),
                D3DSIO_TEXM3x2DEPTH,destination(D3DSPR_TEXTURE,2),source(D3DSPR_TEXTURE,0),
                D3DSIO_MOV,destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,0),D3DPS_END()};
            float depth=-1;expect("SM1 matrix depth color",renderer.draw(s,vs,ps,v,{},tex,{},16,16,false,2,false,-1,0,&depth),ps.constants[0]);
            const float wantedDepth=scenario==0?.25f:scenario==1?.75f:scenario==2?.5f:1;
            expect("SM1 matrix depth readback",{depth,0,0,0},{wantedDepth,0,0,0},.0001f);
        }
        // BEM chooses the bump matrix by destination register and must
        // preserve the unwritten B/A channels, including source aliasing.
        for(unsigned dst:{1u,4u})for(unsigned alias=0;alias<2;++alias) {
            s=base(v);s.stages[dst].bump[0]=.5f;s.stages[dst].bump[1]=-.25f;
            s.stages[dst].bump[2]=.125f;s.stages[dst].bump[3]=.75f;
            Triton9Fixed::vertex(s,vs);ps={};ps.constants={{.2f,.3f,.4f,.5f},{.4f,.6f,.8f,1}};
            ps.tokens={D3DPS_VERSION(1,4),D3DSIO_MOV,destination(D3DSPR_TEMP,dst),source(D3DSPR_CONST,0),
                D3DSIO_BEM,destination(D3DSPR_TEMP,dst,D3DSP_WRITEMASK_0|D3DSP_WRITEMASK_1),
                source(D3DSPR_TEMP,dst),source(alias?D3DSPR_TEMP:D3DSPR_CONST,alias?dst:1),
                D3DSIO_MOV,destination(D3DSPR_TEMP,0),source(D3DSPR_TEMP,dst),D3DPS_END()};
            wanted=ps.constants[0];const Color input=ps.constants[alias?0:1];
            wanted[0]+=.5f*input[0]+.125f*input[1];wanted[1]+=-.25f*input[0]+.75f*input[1];
            expect("SM1.4 BEM matrix and alias readback",renderer.draw(s,vs,ps,v),wanted);
        }
        // Every PS1 source arithmetic modifier crossed with each legal
        // destination scale and saturation. Bias/sign/negation are observed
        // through a final scale+offset so output clamping cannot hide errors.
        for(unsigned minor:{1u,4u})for(unsigned mod=0;mod<=(minor==4?8u:6u);++mod)
        for(unsigned shift:{0u,1u,2u,3u,13u,14u,15u})for(unsigned saturate=0;saturate<2;++saturate)
        for(unsigned alpha=0;alpha<2;++alpha) {
            if(minor!=4&&(shift==3||shift==13||shift==14))continue;
            s=base(v);Triton9Fixed::vertex(s,vs);ps={};ps.constants={{.125f,.25f,.375f,.5f},{.05f,.05f,.05f,.05f},{.5f,.5f,.5f,.5f}};
            ps.tokens={D3DPS_VERSION(1,minor),D3DSIO_MOV,destination(D3DSPR_TEMP,1),source(D3DSPR_CONST,0),
                D3DSIO_MOV,destination(D3DSPR_TEMP,1)|(shift<<D3DSP_DSTSHIFT_SHIFT)|(saturate?D3DSPDM_SATURATE:0),
                source(D3DSPR_TEMP,1,alpha?D3DSP_SWIZZLE_MASK:D3DSP_NOSWIZZLE)|(mod<<D3DSP_SRCMOD_SHIFT),
                D3DSIO_MAD,destination(D3DSPR_TEMP,0),source(D3DSPR_TEMP,1),source(D3DSPR_CONST,1),source(D3DSPR_CONST,2),D3DPS_END()};
            wanted=ps.constants[0];if(alpha)wanted.fill(wanted[3]);
            const float scale=shift<4?float(1u<<shift):1.f/float(1u<<(16-shift));
            for(auto &component:wanted){switch(mod){case 1:component=-component;break;case 2:component-=.5f;break;
                case 3:component=.5f-component;break;case 4:component=2*component-1;break;case 5:component=1-2*component;break;
                case 6:component=1-component;break;case 7:component*=2;break;case 8:component*=-2;break;}
                component*=scale;if(saturate)component=clamp(component);component=component*.05f+.5f;}
            char label[96];std::snprintf(label,sizeof(label),"PS1.%u modifier %u shift %u sat %u alpha %u",minor,mod,shift,saturate,alpha);
            expect(label,renderer.draw(s,vs,ps,v),wanted);
        }
        for(unsigned major:{2u,3u})for(unsigned mod:{0u,1u,11u,12u})
        for(unsigned mask=1;mask<16;++mask)for(unsigned dstMod:{0u,unsigned(D3DSPDM_SATURATE),unsigned(D3DSPDM_PARTIALPRECISION),unsigned(D3DSPDM_SATURATE|D3DSPDM_PARTIALPRECISION)}) {
            if(major==2&&mod>=11)continue;
            s=base(v);Triton9Fixed::vertex(s,vs);ps={};ps.tokens={D3DPS_VERSION(major,0)};
            ps.constants={{-.25f,.5f,-.75f,1.25f},{.1f,.2f,.3f,.4f}};
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,1),source(D3DSPR_CONST,0)});
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,1)});
            const unsigned swizzle=0x93u<<D3DSP_SWIZZLE_SHIFT;
            emit(ps,D3DSIO_MOV,{destination(D3DSPR_TEMP,0,mask<<16)|dstMod,source(D3DSPR_TEMP,1,swizzle)|(mod<<D3DSP_SRCMOD_SHIFT)});finish(ps);
            wanted=ps.constants[1];for(unsigned channel=0;channel<4;++channel)if(mask&(1u<<channel)){
                float value=ps.constants[0][channel==0?3:channel-1];if(mod>=11)value=std::abs(value);if(mod==1||mod==12)value=-value;
                wanted[channel]=dstMod&D3DSPDM_SATURATE?clamp(value):value;}
            char label[96];std::snprintf(label,sizeof(label),"PS%u modifier %u mask %u dstmod %u",major,mod,mask,dstMod);
            expect(label,renderer.draw(s,vs,ps,v),wanted);
        }
        // PS1.4 projective source modifiers have a defined (1,1) result
        // for a zero divisor. Read texcrd directly and texld through a
        // patterned texture so neither path can hide non-finite values.
        for(unsigned mode=0;mode<3;++mode)for(unsigned zero=0;zero<3;++zero) {
            s=base(v);s.inputs[4].components=4;
            for(auto &vert:v)vert.reg[4]={zero==2?0.f:.25f,zero==2?-.25f:.75f,zero?0.f:2.f,zero?0.f:2.f};
            Triton9Fixed::vertex(s,vs);ps={};ps.tokens={D3DPS_VERSION(1,4)};ps.constants={{.3f,.4f,.5f,.6f},{.25f,.25f,.25f,.25f},{.125f,.125f,.125f,.125f}};
            const unsigned xyw=0xf4u<<D3DSP_SWIZZLE_SHIFT;
            if(mode==2)ps.tokens.insert(ps.tokens.end(),{D3DSIO_TEXCOORD,destination(D3DSPR_TEMP,0,D3DSP_WRITEMASK_0|D3DSP_WRITEMASK_1|D3DSP_WRITEMASK_2),source(D3DSPR_TEXTURE,0),D3DSIO_PHASE,
                D3DSIO_TEX,destination(D3DSPR_TEMP,0),source(D3DSPR_TEMP,0)|D3DSPSM_DZ});
            else ps.tokens.insert(ps.tokens.end(),{mode==0?D3DSIO_TEXCOORD:D3DSIO_TEX,
                destination(D3DSPR_TEMP,0,mode==0?D3DSP_WRITEMASK_0|D3DSP_WRITEMASK_1:D3DSP_WRITEMASK_ALL),
                source(D3DSPR_TEXTURE,0,xyw)|D3DSPSM_DW});
            if(mode==0)ps.tokens.insert(ps.tokens.end(),{D3DSIO_MOV,destination(D3DSPR_TEMP,0,D3DSP_WRITEMASK_2|D3DSP_WRITEMASK_3),source(D3DSPR_CONST,0),
                D3DSIO_MAD,destination(D3DSPR_TEMP,0),source(D3DSPR_TEMP,0),source(D3DSPR_CONST,1),source(D3DSPR_CONST,2)});
            ps.tokens.push_back(D3DPS_END());raster={};raster.PSSamplers[0].TextureType=ShaderConv::TEXTURETYPE_2D;
            wanted=mode==0?Color{zero?1.f:.125f,zero?1.f:.375f,.5f,.6f}:zero?Color{1,1,.1f,.8f}:Color{0,1.f/3,.1f,.8f};
            if(mode==0)for(auto &component:wanted)component=component*.25f+.125f;
            char label[80];std::snprintf(label,sizeof(label),"PS1.4 projective mode %u zero %u",mode,zero);
            expect(label,renderer.draw(s,vs,ps,v,raster,tex,{},16,16,true),wanted);
        }
        // Malformed programs must be rejected before the converter touches a
        // declaration, source operand, or nested control-flow stack.
        unsigned badPrograms[][8]={{D3DPS_VERSION(3,0),instruction(D3DSIO_MOV,2),destination(D3DSPR_TEMP,32),source(D3DSPR_CONST,0),D3DPS_END()},
            {D3DPS_VERSION(3,0),instruction(D3DSIO_DCL,2),0x80000000,destination(D3DSPR_SAMPLER,16),D3DPS_END()},
            {D3DPS_VERSION(3,0),instruction(D3DSIO_ELSE,0),D3DPS_END()},
            {D3DPS_VERSION(3,0),instruction(D3DSIO_MOV,2),destination(D3DSPR_TEMP,0),source(D3DSPR_CONST,224),D3DPS_END()}};
        for(const auto &bad:badPrograms)if(ShaderConv::ValidateLegacyShader(bad,(bad[1]==instruction(D3DSIO_ELSE,0)?3:5)*4,false))throw E_FAIL;
        std::printf("production fixed-function rendered checks: %u passed\n",renderer.cases);
    } catch(HRESULT hr) { std::fprintf(stderr,"production fixed-function failure: 0x%08x\n",unsigned(hr)); return 1; }
    return 0;
}
}

int main()
{
    int result = convertAndRenderPair("fixed", fixedVertexShader(),
                                      fixedPixelShader());
    if (result)
        return result;
    result = convertAndRenderPair("fixed linear vertex fog",
                                  fixedFogVertexShader(D3DFOG_LINEAR),
                                  fixedFogPixelShader(), true);
    if (result)
        return result;
    result = convertAndRenderPair("fixed exponential vertex fog",
                                  fixedFogVertexShader(D3DFOG_EXP),
                                  fixedFogPixelShader(), true);
    if (result)
        return result;
    result = convertAndRenderPair("fixed squared exponential vertex fog",
                                  fixedFogVertexShader(D3DFOG_EXP2),
                                  fixedFogPixelShader(), true);
    if (result)
        return result;
    result = convertAndRenderPair("public SM2 full oPos",
                                  publicProbeVertexShader(PublicPositionForm::FullInput),
                                  publicProbePixelShader());
    if (result)
        return result;
    result = convertAndRenderPair("public SM2 split input oPos",
                                  publicProbeVertexShader(PublicPositionForm::SplitInput),
                                  publicProbePixelShader());
    if (result)
        return result;
    result = convertAndRenderPair("public SM2 canonical oPos",
                                  publicProbeVertexShader(PublicPositionForm::SplitInlineConstant),
                                  publicProbePixelShader());
    if (result)
        return result;
    result = convertAndRenderPair("public SM2 nonzero DEF c2",
                                  publicProbeVertexShader(
                                      PublicPositionForm::SplitInlineConstantC2),
                                  publicProbePixelShader());
    if (result)
        return result;
    if (fixed_test::run()) return 1;
    std::puts("triton9 shader conversion/render: PASS");
    return 0;
}
