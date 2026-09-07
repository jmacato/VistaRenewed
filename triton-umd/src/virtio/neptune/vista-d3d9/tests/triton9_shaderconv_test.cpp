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

#ifndef D3DHAL_SAMPLER_MAXSAMP
#define D3DHAL_SAMPLER_MAXSAMP 16
#endif
#include <ShaderConv.h>

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
        uint32_t firstScalar;
        uint32_t scalarCount;

        if (!triton9_shader_constant_scalar_range(
                constant.RegIndex, 4, &firstScalar, &scalarCount) ||
            firstScalar > data.size() || scalarCount > data.size() - firstScalar)
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
    std::puts("triton9 shader conversion/render: PASS");
    return 0;
}
