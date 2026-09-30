/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D9 shader lifetime and the bridge from SM1-3 token programs to Neptune's
 * host D3D11 proxy. ShaderConverter emits raw SM4/5 tokens. Triton's DXBC
 * builder then creates a signed DXBC container with D3D9 declaration-derived
 * signatures for ID3D11Device::Create*Shader and CreateInputLayout.
 */

#include "triton9.h"
#include "triton9_draw_contract.h"
#include "triton9_shader_token_contract.h"
#include "triton9_fixed.h"

#include "../triton/tritonDxbc.h"
#include "../triton/tritonDxbcSignature.h"

#include <algorithm>
#include <cstring>
#include <limits.h>
#include <new>
#include <vector>

/* ShaderConv normally gets this from its private PCH. The bridge includes
 * the public API directly, and the Windows SDK value is fixed at 16. */
#ifndef D3DHAL_SAMPLER_MAXSAMP
#define D3DHAL_SAMPLER_MAXSAMP 16
#endif
#include "ShaderConv.h"
#include "ShaderValidation.h"

namespace {

enum class Triton9ShaderStage {
    Vertex,
    Pixel,
};

static volatile LONG gTriton9ShaderSerial;

/* A diagnostic run shares the UMD with DWM.  Keep the callback trace useful
 * for the short-lived probe process without letting a compositing process
 * append one record per frame to the persistent Vista debug log.  This is a
 * process-local budget, so a probe always has its own complete initial
 * binding/draw sequence. */
static volatile LONG gTriton9DdiTraceBudget = 48;

static void
triton9TraceDdiU32(const char *tag, DWORD value)
{
    if (InterlockedDecrement(&gTriton9DdiTraceBudget) >= 0)
        triton9DiagU32(tag, value);
}

static void
triton9TraceDdi(const char *message)
{
    if (InterlockedDecrement(&gTriton9DdiTraceBudget) >= 0)
        triton9Diag(message);
}

struct Triton9Shader;

struct Triton9VertexDeclaration {
    Triton9VertexDeclaration() : inputDecls(ShaderConv::MAX_VS_INPUT_REGS) {}

    ~Triton9VertexDeclaration();

    std::vector<D3DDDIVERTEXELEMENT> elements;
    ShaderConv::VSInputDecls inputDecls;
    ID3D11InputLayout *inputLayout = nullptr;
    Triton9Shader *layoutShader = nullptr;
    UINT64 layoutShaderInstanceSerial = 0;
    Triton9Shader *transformedVertexShader = nullptr;
    UINT64 layoutGeneration = 0;
    UINT64 layoutFrequencyGeneration = 0;
    UINT64 serial = 0;
    BOOL hasTransformedPosition = FALSE;
};

struct Triton9ShaderVariant {
    explicit Triton9ShaderVariant()
        : inputDecls(ShaderConv::MAX_VS_INPUT_REGS) {}

    ~Triton9ShaderVariant()
    {
        if (vertexShader)
            vertexShader->Release();
        if (pixelShader)
            pixelShader->Release();
        if (dxbc)
            HeapFree(GetProcessHeap(), 0, dxbc);
    }

    const Triton9VertexDeclaration *conversionDeclaration = nullptr;
    UINT64 conversionDeclarationSerial = 0;
    const Triton9Shader *linkedVertexShader = nullptr;
    UINT64 linkedVertexInstanceSerial = 0;
    UINT64 linkedVertexGeneration = 0;
    ShaderConv::RasterStates rasterStates;
    ShaderConv::VSInputDecls inputDecls;
    ShaderConv::VSOutputDecls outputDecls;
    void *dxbc = nullptr;
    SIZE_T dxbcSize = 0;
    ID3D11VertexShader *vertexShader = nullptr;
    ID3D11PixelShader *pixelShader = nullptr;
    UINT maxFloatConstants = 0;
    UINT maxIntConstants = 0;
    UINT maxBoolConstants = 0;
    ShaderConv::ShaderConsts inlineConstants[3];
    UINT64 generation = 0;
};

struct Triton9Shader {
    explicit Triton9Shader(Triton9ShaderStage stage) : stage(stage),
        inputDecls(ShaderConv::MAX_VS_INPUT_REGS),
        instanceSerial((UINT64)(ULONG)InterlockedIncrement(&gTriton9ShaderSerial)) {
        /* The accepted TCI state is identity. Zero maps every SM1/2 pixel
         * input to TEXCOORD0, collapsing DWM's eight-tap blur to one sample. */
        rasterStates.TCIMapping = ShaderConv::TCIMASK_PASSTHRU;
    }

    ~Triton9Shader()
    {
        if (vertexShader)
            vertexShader->Release();
        if (pixelShader)
            pixelShader->Release();
        if (dxbc)
            HeapFree(GetProcessHeap(), 0, dxbc);
        for (Triton9ShaderVariant *variant : variants)
            delete variant;
    }

    Triton9ShaderStage stage;
    std::vector<UINT> legacyTokens;
    void *dxbc = nullptr;
    SIZE_T dxbcSize = 0;
    ID3D11VertexShader *vertexShader = nullptr;
    ID3D11PixelShader *pixelShader = nullptr;
    ShaderConv::VSInputDecls inputDecls;
    ShaderConv::VSOutputDecls outputDecls;
    ShaderConv::RasterStates rasterStates;
    ShaderConv::ShaderConverterAPI converter;
    const Triton9VertexDeclaration *conversionDeclaration = nullptr;
    UINT64 conversionDeclarationSerial = 0;
    UINT64 generation = 0;
    UINT64 instanceSerial = 0;
    const Triton9Shader *linkedVertexShader = nullptr;
    UINT64 linkedVertexInstanceSerial = 0;
    UINT64 linkedVertexGeneration = 0;
    UINT maxFloatConstants = 0;
    UINT maxIntConstants = 0;
    UINT maxBoolConstants = 0;
    ShaderConv::ShaderConsts inlineConstants[3];
    UINT64 conversionEpoch = 0;
    std::vector<Triton9ShaderVariant *> variants;
    BOOL transformedFixedFunction = FALSE;
    BOOL internalFixedFunction = FALSE;
};

/* Internal shader objects implement the D3D9 fixed-function stages. Their
 * program and constant state is isolated from application shader handles. */
struct Triton9FixedFunctionShaders {
    Triton9Shader *vertexShader = nullptr;
    Triton9Shader *pixelShader = nullptr;
    const Triton9VertexDeclaration *vertexDeclaration = nullptr;
    UINT64 vertexDeclarationSerial = 0;
    UINT64 vertexStateKey = 0;
    UINT64 pixelStateKey = 0;
    const Triton9Shader *pixelVertexShader = nullptr;
    UINT64 pixelVertexInstanceSerial = 0;
    UINT64 pixelVertexGeneration = 0;
    ID3D11GeometryShader *geometryShader = nullptr;
    Triton9Shader *geometryLinkage = nullptr;
    const Triton9Shader *geometryVertex = nullptr;
    UINT64 geometryInstanceSerial = 0;
    UINT64 geometryGeneration = 0;
    ShaderConv::RasterStates geometryRaster;
    Triton9Fixed::State *vertexSnapshot = nullptr;
    Triton9Fixed::State *pixelSnapshot = nullptr;
};

Triton9VertexDeclaration::~Triton9VertexDeclaration()
{
    if (inputLayout)
        inputLayout->Release();
    delete transformedVertexShader;
}

struct Triton9InputElement {
    D3D11_INPUT_ELEMENT_DESC desc;
    UINT registerIndex;
};

static volatile LONG gTriton9DeclarationSerial;

static bool
triton9ValidateLegacyShader(const UINT *tokens, UINT byteCount,
                            Triton9ShaderStage stage)
{
    return ShaderConv::ValidateLegacyShader(tokens, byteCount,
                                            stage == Triton9ShaderStage::Vertex);
}

static DXGI_FORMAT
triton9DeclarationFormat(UCHAR type)
{
    switch (type) {
    case D3DDECLTYPE_FLOAT1:    return DXGI_FORMAT_R32_FLOAT;
    case D3DDECLTYPE_FLOAT2:    return DXGI_FORMAT_R32G32_FLOAT;
    case D3DDECLTYPE_FLOAT3:    return DXGI_FORMAT_R32G32B32_FLOAT;
    case D3DDECLTYPE_FLOAT4:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case D3DDECLTYPE_D3DCOLOR:  return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DDECLTYPE_UBYTE4:    return DXGI_FORMAT_R8G8B8A8_UINT;
    case D3DDECLTYPE_SHORT2:    return DXGI_FORMAT_R16G16_SINT;
    case D3DDECLTYPE_SHORT4:    return DXGI_FORMAT_R16G16B16A16_SINT;
    case D3DDECLTYPE_UBYTE4N:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case D3DDECLTYPE_SHORT2N:   return DXGI_FORMAT_R16G16_SNORM;
    case D3DDECLTYPE_SHORT4N:   return DXGI_FORMAT_R16G16B16A16_SNORM;
    case D3DDECLTYPE_USHORT2N:  return DXGI_FORMAT_R16G16_UNORM;
    case D3DDECLTYPE_USHORT4N:  return DXGI_FORMAT_R16G16B16A16_UNORM;
    case D3DDECLTYPE_UDEC3:     return DXGI_FORMAT_R10G10B10A2_UINT;
    case D3DDECLTYPE_DEC3N:     return DXGI_FORMAT_R10G10B10A2_UINT;
    case D3DDECLTYPE_FLOAT16_2: return DXGI_FORMAT_R16G16_FLOAT;
    case D3DDECLTYPE_FLOAT16_4: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                     return DXGI_FORMAT_UNKNOWN;
    }
}

static UINT
triton9DeclarationTypeSize(UCHAR type)
{
    switch (type) {
    case D3DDECLTYPE_FLOAT1:
    case D3DDECLTYPE_D3DCOLOR:
    case D3DDECLTYPE_UBYTE4:
    case D3DDECLTYPE_UBYTE4N:
    case D3DDECLTYPE_SHORT2:
    case D3DDECLTYPE_SHORT2N:
    case D3DDECLTYPE_USHORT2N:
    case D3DDECLTYPE_UDEC3:
    case D3DDECLTYPE_DEC3N:
    case D3DDECLTYPE_FLOAT16_2:
        return 4;
    case D3DDECLTYPE_FLOAT2:
    case D3DDECLTYPE_SHORT4:
    case D3DDECLTYPE_SHORT4N:
    case D3DDECLTYPE_USHORT4N:
    case D3DDECLTYPE_FLOAT16_4:
        return 8;
    case D3DDECLTYPE_FLOAT3:
        return 12;
    case D3DDECLTYPE_FLOAT4:
        return 16;
    default:
        return 0;
    }
}

static UINT
triton9DeclarationConversion(UCHAR type)
{
    switch (type) {
    case D3DDECLTYPE_UDEC3:
        return ShaderConv::VSInputDecl::UDEC3;
    case D3DDECLTYPE_DEC3N:
        return ShaderConv::VSInputDecl::DEC3N;
    case D3DDECLTYPE_UBYTE4:
    case D3DDECLTYPE_SHORT2:
    case D3DDECLTYPE_SHORT4:
        return ShaderConv::VSInputDecl::NeedsIntToFloatConversion;
    default:
        return ShaderConv::VSInputDecl::None;
    }
}

static UINT
triton9DeclarationComponentType(UCHAR type)
{
    switch (type) {
    case D3DDECLTYPE_UBYTE4:
    case D3DDECLTYPE_UDEC3:
        return 1; /* D3D_REGISTER_COMPONENT_UINT32 */
    case D3DDECLTYPE_SHORT2:
    case D3DDECLTYPE_SHORT4:
        return 2; /* D3D_REGISTER_COMPONENT_SINT32 */
    default:
        return 3; /* D3D_REGISTER_COMPONENT_FLOAT32 */
    }
}

static const char *
triton9SemanticName(UINT usage)
{
    switch (usage) {
    case D3DDECLUSAGE_POSITION:     return "POSITION";
    case D3DDECLUSAGE_BLENDWEIGHT:  return "BLENDWEIGHT";
    case D3DDECLUSAGE_BLENDINDICES: return "BLENDINDICES";
    case D3DDECLUSAGE_NORMAL:       return "NORMAL";
    case D3DDECLUSAGE_PSIZE:        return "PSIZE";
    case D3DDECLUSAGE_TEXCOORD:     return "TEXCOORD";
    case D3DDECLUSAGE_TANGENT:      return "TANGENT";
    case D3DDECLUSAGE_BINORMAL:     return "BINORMAL";
    case D3DDECLUSAGE_TESSFACTOR:   return "TESSFACTOR";
    case D3DDECLUSAGE_COLOR:        return "COLOR";
    case D3DDECLUSAGE_FOG:          return "FOG";
    case D3DDECLUSAGE_DEPTH:        return "DEPTH";
    case D3DDECLUSAGE_SAMPLE:       return "SAMPLE";
    case D3DDECLUSAGE_VPOS:         return "SV_Position";
    case D3DDECLUSAGE_VFACE:        return "SV_IsFrontFace";
    case D3DDECLUSAGE_CLIPDISTANCE: return "SV_ClipDistance";
    case D3DDECLUSAGE_POINTSPRITE:  return "POINTSPRITE";
    default:                         return "ATTRIB";
    }
}

static const char *
triton9LinkageSemanticName(UINT usage)
{
    switch (usage) {
    case D3DDECLUSAGE_POSITION:
    case D3DDECLUSAGE_VPOS:
        return "SV_Position";
    case D3DDECLUSAGE_VFACE:
        return "SV_IsFrontFace";
    case D3DDECLUSAGE_CLIPDISTANCE:
        return "SV_ClipDistance";
    default:
        return triton9SemanticName(usage);
    }
}

static UINT
triton9SemanticSystemValue(UINT usage)
{
    switch (usage) {
    case D3DDECLUSAGE_POSITION:
    case D3DDECLUSAGE_VPOS:
        return D3D_NAME_POSITION;
    case D3DDECLUSAGE_VFACE:
        return D3D_NAME_IS_FRONT_FACE;
    case D3DDECLUSAGE_CLIPDISTANCE:
        return D3D_NAME_CLIP_DISTANCE;
    default:
        return D3D_NAME_UNDEFINED;
    }
}

static const D3DDDIVERTEXELEMENT *
triton9FindDeclarationElement(const Triton9VertexDeclaration *declaration,
                              UINT usage, UINT usageIndex)
{
    if (!declaration)
        return nullptr;
    for (const D3DDDIVERTEXELEMENT &element : declaration->elements) {
        if (element.Usage == usage && element.UsageIndex == usageIndex)
            return &element;
    }
    return nullptr;
}

static HRESULT
triton9BuildVsInputSignatures(const Triton9VertexDeclaration *declaration,
                              const ShaderConv::VSInputDecls &inputDecls,
                              std::vector<TRITON_DXBC_SIGNATURE> *signatures)
{
    if (!signatures)
        return E_INVALIDARG;
    signatures->clear();
    if (!declaration)
        return S_OK;
    try {
        signatures->reserve(inputDecls.GetSize());
        for (UINT i = 0; i < inputDecls.GetSize(); ++i) {
            const ShaderConv::VSInputDecl &input = inputDecls[i];
            const D3DDDIVERTEXELEMENT *element = triton9FindDeclarationElement(
                declaration, input.Usage, input.UsageIndex);
            if (!element)
                return D3DDDIERR_NOTAVAILABLE;
            TRITON_DXBC_SIGNATURE signature = {};
            signature.semanticName = triton9SemanticName(input.Usage);
            signature.semanticIndex = input.UsageIndex;
            signature.systemValue = D3D_NAME_UNDEFINED;
            signature.registerIdx = input.RegIndex;
            signature.mask = 0x0f;
            signature.componentType = triton9DeclarationComponentType(element->Type);
            signatures->push_back(signature);
        }
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

static HRESULT
triton9BuildVsOutputSignatures(const ShaderConv::VSOutputDecls &outputDecls,
                               std::vector<TRITON_DXBC_SIGNATURE> *signatures)
{
    if (!signatures)
        return E_INVALIDARG;
    signatures->clear();
    try {
        signatures->reserve(outputDecls.GetSize());
        for (UINT i = 0; i < outputDecls.GetSize(); ++i) {
            const ShaderConv::VSOutputDecl &output = outputDecls[i];
            TRITON_DXBC_SIGNATURE signature = {};
            signature.semanticName = triton9LinkageSemanticName(output.Usage);
            signature.semanticIndex = output.UsageIndex;
            signature.systemValue = triton9SemanticSystemValue(output.Usage);
            signature.registerIdx = output.RegIndex;
            signature.mask = (BYTE)(output.WriteMask ? output.WriteMask : 0x0f);
            signature.componentType = output.Usage == D3DDECLUSAGE_VFACE ? 1 : 3;
            signatures->push_back(signature);
        }
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

static void
triton9DropConvertedShader(Triton9Shader *shader)
{
    if (!shader)
        return;
    if (shader->vertexShader) {
        shader->vertexShader->Release();
        shader->vertexShader = nullptr;
    }
    if (shader->pixelShader) {
        shader->pixelShader->Release();
        shader->pixelShader = nullptr;
    }
    if (shader->dxbc) {
        HeapFree(GetProcessHeap(), 0, shader->dxbc);
        shader->dxbc = nullptr;
    }
    shader->dxbcSize = 0;
}

static bool
triton9VariantMatches(const Triton9ShaderVariant *variant,
                      const Triton9Shader *shader,
                      const Triton9VertexDeclaration *declaration,
                      const Triton9Shader *linkedVertexShader)
{
    if (!variant || !shader ||
        memcmp(&variant->rasterStates, &shader->rasterStates,
               sizeof(shader->rasterStates)))
        return false;
    if (shader->stage == Triton9ShaderStage::Vertex)
        return variant->conversionDeclaration == declaration &&
               variant->conversionDeclarationSerial ==
                   (declaration ? declaration->serial : 0);
    return variant->linkedVertexShader == linkedVertexShader &&
           variant->linkedVertexInstanceSerial ==
               (linkedVertexShader ? linkedVertexShader->instanceSerial : 0) &&
           variant->linkedVertexGeneration ==
               (linkedVertexShader ? linkedVertexShader->generation : 0);
}

/* The caller uses this result only while shaderLock serializes shader use. */
static HRESULT
triton9RestoreShaderVariant(Triton9Shader *shader,
                            const Triton9VertexDeclaration *declaration,
                            const Triton9Shader *linkedVertexShader)
{
    Triton9ShaderVariant *match = nullptr;

    if (!shader)
        return E_INVALIDARG;
    for (Triton9ShaderVariant *variant : shader->variants) {
        if (triton9VariantMatches(variant, shader, declaration, linkedVertexShader)) {
            match = variant;
            break;
        }
    }
    if (!match)
        return S_FALSE;
    if (!match->dxbc || !match->dxbcSize ||
        (shader->stage == Triton9ShaderStage::Vertex && !match->vertexShader) ||
        (shader->stage == Triton9ShaderStage::Pixel && !match->pixelShader))
        return E_FAIL;

    ShaderConv::VSInputDecls inputDecls(ShaderConv::MAX_VS_INPUT_REGS);
    ShaderConv::VSOutputDecls outputDecls;
    ShaderConv::ShaderConsts inlineConstants[3];
    void *dxbc = HeapAlloc(GetProcessHeap(), 0, match->dxbcSize);
    if (!dxbc)
        return E_OUTOFMEMORY;
    try {
        inputDecls = match->inputDecls;
        outputDecls = match->outputDecls;
        for (UINT slot = 0; slot < 3; ++slot)
            inlineConstants[slot] = match->inlineConstants[slot];
    } catch (...) {
        HeapFree(GetProcessHeap(), 0, dxbc);
        return E_OUTOFMEMORY;
    }
    memcpy(dxbc, match->dxbc, match->dxbcSize);
    triton9DropConvertedShader(shader);
    shader->dxbc = dxbc;
    shader->dxbcSize = match->dxbcSize;
    shader->inputDecls = std::move(inputDecls);
    shader->outputDecls = std::move(outputDecls);
    for (UINT slot = 0; slot < 3; ++slot)
        shader->inlineConstants[slot] = std::move(inlineConstants[slot]);
    shader->conversionDeclaration = match->conversionDeclaration;
    shader->conversionDeclarationSerial = match->conversionDeclarationSerial;
    shader->linkedVertexShader = match->linkedVertexShader;
    shader->linkedVertexInstanceSerial = match->linkedVertexInstanceSerial;
    shader->linkedVertexGeneration = match->linkedVertexGeneration;
    shader->maxFloatConstants = match->maxFloatConstants;
    shader->maxIntConstants = match->maxIntConstants;
    shader->maxBoolConstants = match->maxBoolConstants;
    shader->generation = match->generation;
    if (shader->stage == Triton9ShaderStage::Vertex) {
        shader->vertexShader = match->vertexShader;
        shader->vertexShader->AddRef();
    } else {
        shader->pixelShader = match->pixelShader;
        shader->pixelShader->AddRef();
    }
    return S_OK;
}

static void
triton9StoreShaderVariant(Triton9Shader *shader)
{
    constexpr size_t kMaxShaderVariants = 8;
    Triton9ShaderVariant *variant;

    if (!shader || !shader->dxbc || !shader->dxbcSize ||
        (shader->stage == Triton9ShaderStage::Vertex && !shader->vertexShader) ||
        (shader->stage == Triton9ShaderStage::Pixel && !shader->pixelShader))
        return;
    variant = new (std::nothrow) Triton9ShaderVariant;
    if (!variant)
        return;
    variant->dxbc = HeapAlloc(GetProcessHeap(), 0, shader->dxbcSize);
    if (!variant->dxbc) {
        delete variant;
        return;
    }
    try {
        variant->inputDecls = shader->inputDecls;
        variant->outputDecls = shader->outputDecls;
        for (UINT slot = 0; slot < 3; ++slot)
            variant->inlineConstants[slot] = shader->inlineConstants[slot];
    } catch (...) {
        delete variant;
        return;
    }
    memcpy(variant->dxbc, shader->dxbc, shader->dxbcSize);
    variant->dxbcSize = shader->dxbcSize;
    variant->conversionDeclaration = shader->conversionDeclaration;
    variant->conversionDeclarationSerial = shader->conversionDeclarationSerial;
    variant->linkedVertexShader = shader->linkedVertexShader;
    variant->linkedVertexInstanceSerial = shader->linkedVertexShader
                                              ? shader->linkedVertexShader->instanceSerial
                                              : 0;
    variant->linkedVertexGeneration = shader->linkedVertexGeneration;
    variant->rasterStates = shader->rasterStates;
    variant->maxFloatConstants = shader->maxFloatConstants;
    variant->maxIntConstants = shader->maxIntConstants;
    variant->maxBoolConstants = shader->maxBoolConstants;
    variant->generation = shader->generation;
    if (shader->stage == Triton9ShaderStage::Vertex) {
        variant->vertexShader = shader->vertexShader;
        variant->vertexShader->AddRef();
    } else {
        variant->pixelShader = shader->pixelShader;
        variant->pixelShader->AddRef();
    }
    try {
        if (shader->variants.size() == kMaxShaderVariants) {
            delete shader->variants.front();
            shader->variants.erase(shader->variants.begin());
        }
        shader->variants.push_back(variant);
    } catch (...) {
        delete variant;
    }
}

static HRESULT
triton9ConvertShader(TRITON9_DEVICE *device, Triton9Shader *shader,
                     const Triton9VertexDeclaration *declaration,
                     const Triton9Shader *linkedVertexShader)
{
    if (!device || !device->hostDevice || !shader || shader->legacyTokens.empty())
        return E_INVALIDARG;
    ShaderConv::ConvertShaderArgs args(
        9, ShaderConv::AnythingTimes0Equals0 |
           (shader->internalFixedFunction ? ShaderConv::InternalFixedFunction : 0),
        shader->rasterStates);
    ShaderConv::VSInputDecls convertedInputs(ShaderConv::MAX_VS_INPUT_REGS);
    ShaderConv::VSOutputDecls convertedOutputs;
    ShaderConv::VSOutputDecls emptyOutputs;
    std::vector<TRITON_DXBC_SIGNATURE> inputSignatures;
    std::vector<TRITON_DXBC_SIGNATURE> outputSignatures;
    ShaderConv::ByteCode converted;
    HRESULT hr;

    if (shader->stage == Triton9ShaderStage::Pixel && linkedVertexShader &&
        linkedVertexShader->stage != Triton9ShaderStage::Vertex)
        return E_INVALIDARG;

    hr = triton9RestoreShaderVariant(shader, declaration, linkedVertexShader);
    if (hr != S_FALSE)
        return hr;

    try {
        if (declaration)
            convertedInputs = declaration->inputDecls;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    args.type = shader->stage == Triton9ShaderStage::Vertex
                    ? ShaderConv::ConvertShaderArgs::SHADER_TYPE::SHADER_TYPE_VERTEX
                    : ShaderConv::ConvertShaderArgs::SHADER_TYPE::SHADER_TYPE_PIXEL;
    args.legacyByteCode.m_pByteCode = shader->legacyTokens.data();
    args.legacyByteCode.m_byteCodeSize =
        shader->legacyTokens.size() * sizeof(shader->legacyTokens[0]);
    if (shader->stage == Triton9ShaderStage::Vertex) {
        args.pPsInputDecl = nullptr;
        args.pVsInputDecl = &convertedInputs;
        args.pVsOutputDecl = &convertedOutputs;
    } else {
        args.pPsInputDecl = linkedVertexShader
                                ? const_cast<ShaderConv::VSOutputDecls *>(
                                      &linkedVertexShader->outputDecls)
                                : &emptyOutputs;
        args.pVsInputDecl = nullptr;
        args.pVsOutputDecl = nullptr;
    }

    try {
        hr = shader->converter.ConvertShader(args);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    if (FAILED(hr) || !args.convertedByteCode.m_pByteCode ||
        !args.convertedByteCode.m_byteCodeSize)
        return FAILED(hr) ? hr : E_FAIL;

    converted = args.convertedByteCode;
    if (shader->stage == Triton9ShaderStage::Vertex) {
        hr = triton9BuildVsInputSignatures(declaration, convertedInputs,
                                           &inputSignatures);
        if (SUCCEEDED(hr))
            hr = triton9BuildVsOutputSignatures(convertedOutputs, &outputSignatures);
        if (FAILED(hr)) {
            ShaderConv::ShaderConverterAPI::CleanUpConvertedShader(converted);
            return hr;
        }
    } else if (linkedVertexShader) {
        hr = triton9BuildVsOutputSignatures(linkedVertexShader->outputDecls,
                                            &inputSignatures);
        if (FAILED(hr)) {
            ShaderConv::ShaderConverterAPI::CleanUpConvertedShader(converted);
            return hr;
        }
    }

    if (shader->stage == Triton9ShaderStage::Pixel) {
        try {
            for (const auto &semantic : args.AddedSystemSemantics) {
                TRITON_DXBC_SIGNATURE signature = {};
                signature.semanticName = triton9LinkageSemanticName(semantic.Usage);
                signature.semanticIndex = semantic.UsageIndex;
                signature.systemValue = triton9SemanticSystemValue(semantic.Usage);
                signature.registerIdx = semantic.RegIndex;
                signature.mask = semantic.WriteMask;
                signature.componentType = semantic.Usage == D3DDECLUSAGE_VFACE ? 1 : 3;
                inputSignatures.push_back(signature);
            }
        } catch (...) {
            ShaderConv::ShaderConverterAPI::CleanUpConvertedShader(converted);
            return E_OUTOFMEMORY;
        }
    }
    triton9DropConvertedShader(shader);
    shader->dxbc = tritonBuildDxbc(
        static_cast<const UINT *>(converted.m_pByteCode),
        converted.m_byteCodeSize,
        inputSignatures.empty() ? nullptr : inputSignatures.data(),
        (UINT)inputSignatures.size(),
        outputSignatures.empty() ? nullptr : outputSignatures.data(),
        (UINT)outputSignatures.size(), nullptr, 0,
        sizeof(TRITON_DXBC_SIGNATURE), &shader->dxbcSize);
    ShaderConv::ShaderConverterAPI::CleanUpConvertedShader(converted);
    if (!shader->dxbc || !shader->dxbcSize)
        return E_FAIL;

    if (shader->stage == Triton9ShaderStage::Vertex) {
        hr = device->hostDevice->CreateVertexShader(shader->dxbc, shader->dxbcSize,
                                                     nullptr, &shader->vertexShader);
    } else {
        hr = device->hostDevice->CreatePixelShader(shader->dxbc, shader->dxbcSize,
                                                    nullptr, &shader->pixelShader);
    }
    if (FAILED(hr)) {
        triton9DropConvertedShader(shader);
        return hr;
    }

    shader->maxFloatConstants = args.maxFloatConstsUsed;
    shader->maxIntConstants = args.maxIntConstsUsed;
    shader->maxBoolConstants = args.maxBoolConstsUsed;
    for (UINT slot = 0; slot < 3; ++slot)
        shader->inlineConstants[slot] = std::move(args.m_inlineConsts[slot]);
    if (shader->stage == Triton9ShaderStage::Vertex) {
        shader->inputDecls = std::move(convertedInputs);
        shader->outputDecls = convertedOutputs;
        shader->conversionDeclaration = declaration;
        shader->conversionDeclarationSerial = declaration ? declaration->serial : 0;
    } else {
        shader->linkedVertexShader = linkedVertexShader;
        shader->linkedVertexInstanceSerial = linkedVertexShader
                                                  ? linkedVertexShader->instanceSerial : 0;
        shader->linkedVertexGeneration = linkedVertexShader
                                              ? linkedVertexShader->generation : 0;
    }
    ++shader->conversionEpoch;
    if (!shader->conversionEpoch)
        ++shader->conversionEpoch;
    shader->generation = shader->conversionEpoch;
    triton9StoreShaderVariant(shader);
    return S_OK;
}

/* ConvertTLShader is the bounded null-VS path for a declaration containing
 * POSITIONT.  The declaration rewrites POSITIONT to POSITION while retaining
 * IsTransformedPosition in ShaderConverter's input metadata; this is the
 * form expected by the MIT converter. */
static HRESULT
triton9ConvertTransformedVertexShader(TRITON9_DEVICE *device,
                                      Triton9Shader *shader,
                                      Triton9VertexDeclaration *declaration)
{
    if (!device || !device->hostDevice || !shader || !declaration ||
        shader->stage != Triton9ShaderStage::Vertex ||
        !declaration->hasTransformedPosition)
        return E_INVALIDARG;

    ShaderConv::VSInputDecls convertedInputs(ShaderConv::MAX_VS_INPUT_REGS);
    ShaderConv::VSOutputDecls convertedOutputs;
    std::vector<TRITON_DXBC_SIGNATURE> inputSignatures;
    std::vector<TRITON_DXBC_SIGNATURE> outputSignatures;
    ShaderConv::ByteCode converted;
    ShaderConv::ConvertTLShaderArgs args(
        9, ShaderConv::AnythingTimes0Equals0, convertedInputs,
        convertedOutputs);
    HRESULT hr;

    try {
        convertedInputs = declaration->inputDecls;
        hr = shader->converter.ConvertTLShader(args);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    if (FAILED(hr) || !args.convertedByteCode.m_pByteCode ||
        !args.convertedByteCode.m_byteCodeSize)
        return FAILED(hr) ? hr : E_FAIL;

    converted = args.convertedByteCode;
    hr = triton9BuildVsInputSignatures(declaration, convertedInputs,
                                       &inputSignatures);
    if (SUCCEEDED(hr))
        hr = triton9BuildVsOutputSignatures(convertedOutputs, &outputSignatures);
    if (FAILED(hr)) {
        ShaderConv::ShaderConverterAPI::CleanUpConvertedShader(converted);
        return hr;
    }

    triton9DropConvertedShader(shader);
    shader->dxbc = tritonBuildDxbc(
        static_cast<const UINT *>(converted.m_pByteCode),
        converted.m_byteCodeSize,
        inputSignatures.empty() ? nullptr : inputSignatures.data(),
        (UINT)inputSignatures.size(),
        outputSignatures.empty() ? nullptr : outputSignatures.data(),
        (UINT)outputSignatures.size(), nullptr, 0,
        sizeof(TRITON_DXBC_SIGNATURE), &shader->dxbcSize);
    ShaderConv::ShaderConverterAPI::CleanUpConvertedShader(converted);
    if (!shader->dxbc || !shader->dxbcSize)
        return E_FAIL;

    hr = device->hostDevice->CreateVertexShader(shader->dxbc, shader->dxbcSize,
                                                  nullptr, &shader->vertexShader);
    if (FAILED(hr) || !shader->vertexShader) {
        triton9DropConvertedShader(shader);
        return FAILED(hr) ? hr : E_FAIL;
    }

    shader->inputDecls = std::move(convertedInputs);
    shader->outputDecls = convertedOutputs;
    shader->conversionDeclaration = declaration;
    shader->conversionDeclarationSerial = declaration->serial;
    shader->transformedFixedFunction = TRUE;
    shader->maxFloatConstants = args.maxFloatConstsUsed;
    shader->maxIntConstants = args.maxIntConstsUsed;
    shader->maxBoolConstants = args.maxBoolConstsUsed;
    ++shader->conversionEpoch;
    if (!shader->conversionEpoch)
        ++shader->conversionEpoch;
    shader->generation = shader->conversionEpoch;
    return S_OK;
}

/* The high two bits select D3D9 instancing mode.  The remaining bits are an
 * instance count for stream zero, or a per-instance step rate for another
 * stream.  Vista's headers define the flag values but not this useful mask. */
static const UINT kTriton9StreamFrequencyValueMask = 0x3fffffffu;

/* The caller holds shaderLock. */
static HRESULT
triton9GetInputSlotFrequency(const TRITON9_DEVICE *device, UINT stream,
                             D3D11_INPUT_CLASSIFICATION *slotClass,
                             UINT *stepRate)
{
    UINT frequency;

    if (!device || !slotClass || !stepRate || stream >= TRITON9_MAX_VERTEX_STREAMS)
        return E_INVALIDARG;
    frequency = device->streamSourceFrequencies[stream];
    if (!(frequency & D3DSTREAMSOURCE_INSTANCEDATA)) {
        *slotClass = D3D11_INPUT_PER_VERTEX_DATA;
        *stepRate = 0;
        return S_OK;
    }
    if (stream != 0 && (frequency & D3DSTREAMSOURCE_INSTANCEDATA) &&
        !(frequency & D3DSTREAMSOURCE_INDEXEDDATA)) {
        *slotClass = D3D11_INPUT_PER_INSTANCE_DATA;
        *stepRate = frequency & kTriton9StreamFrequencyValueMask;
        return S_OK;
    }
    return D3DDDIERR_INVALIDCALL;
}

/* The caller holds shaderLock. */
static HRESULT
triton9GetStreamElementRange(const TRITON9_DEVICE *device, UINT stream,
                             UINT firstVertex, UINT vertexCount,
                             UINT instanceCount, UINT *firstElement,
                             UINT *elementCount)
{
    D3D11_INPUT_CLASSIFICATION slotClass;
    UINT stepRate;
    HRESULT hr;

    if (!firstElement || !elementCount || !vertexCount || !instanceCount)
        return E_INVALIDARG;
    hr = triton9GetInputSlotFrequency(device, stream, &slotClass, &stepRate);
    if (FAILED(hr))
        return hr;
    if (slotClass == D3D11_INPUT_PER_VERTEX_DATA) {
        if (firstVertex > UINT_MAX - vertexCount)
            return D3DDDIERR_INVALIDCALL;
        *firstElement = firstVertex;
        *elementCount = vertexCount;
        return S_OK;
    }
    /* A zero per-instance step rate always reads the first element. */
    *firstElement = 0;
    *elementCount = stepRate ? (instanceCount - 1u) / stepRate + 1u : 1u;
    return S_OK;
}

/* The caller holds shaderLock. */
static HRESULT
triton9GetInstanceCount(const TRITON9_DEVICE *device,
                        const Triton9Shader *shader,
                        const Triton9VertexDeclaration *declaration,
                        UINT *instanceCount)
{
    BOOL hasInstanceInput = FALSE;

    if (!device || !shader || !declaration || !instanceCount)
        return E_INVALIDARG;
    for (UINT i = 0; i < shader->inputDecls.GetSize(); ++i) {
        const ShaderConv::VSInputDecl &input = shader->inputDecls[i];
        const D3DDDIVERTEXELEMENT *element = triton9FindDeclarationElement(
            declaration, input.Usage, input.UsageIndex);
        D3D11_INPUT_CLASSIFICATION slotClass;
        UINT stepRate;
        HRESULT hr;

        if (!element)
            return D3DDDIERR_INVALIDCALL;
        hr = triton9GetInputSlotFrequency(device, element->Stream, &slotClass,
                                           &stepRate);
        if (FAILED(hr))
            return hr;
        hasInstanceInput |= slotClass == D3D11_INPUT_PER_INSTANCE_DATA;
    }
    *instanceCount = hasInstanceInput ?
        device->streamSourceFrequencies[0] & kTriton9StreamFrequencyValueMask : 1u;
    if (!*instanceCount)
        *instanceCount = 1;
    return S_OK;
}

static HRESULT
triton9EnsureInputLayout(TRITON9_DEVICE *device, Triton9Shader *shader,
                         Triton9VertexDeclaration *declaration)
{
    std::vector<Triton9InputElement> elements;
    HRESULT hr;

    if (!device || !shader || !declaration || !shader->vertexShader ||
        !shader->dxbc || !shader->dxbcSize)
        return E_INVALIDARG;
    if (declaration->inputLayout && declaration->layoutShader == shader &&
        declaration->layoutShaderInstanceSerial == shader->instanceSerial &&
        declaration->layoutGeneration == shader->generation &&
        declaration->layoutFrequencyGeneration ==
            device->streamFrequencyGeneration)
        return S_OK;
    if (declaration->inputLayout) {
        declaration->inputLayout->Release();
        declaration->inputLayout = nullptr;
    }
    declaration->layoutShader = nullptr;
    declaration->layoutShaderInstanceSerial = 0;
    declaration->layoutGeneration = 0;
    declaration->layoutFrequencyGeneration = 0;

    try {
        elements.reserve(shader->inputDecls.GetSize());
        for (UINT i = 0; i < shader->inputDecls.GetSize(); ++i) {
            const ShaderConv::VSInputDecl &input = shader->inputDecls[i];
            const D3DDDIVERTEXELEMENT *source = triton9FindDeclarationElement(
                declaration, input.Usage, input.UsageIndex);
            const DXGI_FORMAT format = source ? triton9DeclarationFormat(source->Type)
                                              : DXGI_FORMAT_UNKNOWN;
            if (!source || format == DXGI_FORMAT_UNKNOWN)
                return D3DDDIERR_NOTAVAILABLE;
            Triton9InputElement element = {};
            element.registerIndex = input.RegIndex;
            element.desc.SemanticName = triton9SemanticName(input.Usage);
            element.desc.SemanticIndex = input.UsageIndex;
            element.desc.Format = format;
            element.desc.InputSlot = source->Stream;
            element.desc.AlignedByteOffset = source->Offset;
            hr = triton9GetInputSlotFrequency(device, source->Stream,
                                               &element.desc.InputSlotClass,
                                               &element.desc.InstanceDataStepRate);
            if (FAILED(hr))
                return hr;
            elements.push_back(element);
        }
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    if (elements.empty())
        return S_OK;
    std::sort(elements.begin(), elements.end(),
              [](const Triton9InputElement &left, const Triton9InputElement &right) {
                  return left.registerIndex < right.registerIndex;
              });
    std::vector<D3D11_INPUT_ELEMENT_DESC> descriptors;
    try {
        descriptors.reserve(elements.size());
        for (const Triton9InputElement &element : elements)
            descriptors.push_back(element.desc);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    hr = device->hostDevice->CreateInputLayout(descriptors.data(),
                                               (UINT)descriptors.size(),
                                               shader->dxbc, shader->dxbcSize,
                                               &declaration->inputLayout);
    if (FAILED(hr) || !declaration->inputLayout) {
        declaration->inputLayout = nullptr;
        return FAILED(hr) ? hr : E_FAIL;
    }
    declaration->layoutShader = shader;
    declaration->layoutShaderInstanceSerial = shader->instanceSerial;
    declaration->layoutGeneration = shader->generation;
    declaration->layoutFrequencyGeneration = device->streamFrequencyGeneration;
    return S_OK;
}

static HRESULT
triton9EnsureTransformedVertexShader(TRITON9_DEVICE *device,
                                     Triton9VertexDeclaration *declaration,
                                     Triton9Shader **outShader)
{
    Triton9Shader *shader;
    HRESULT hr;

    if (!device || !declaration || !outShader ||
        !declaration->hasTransformedPosition)
        return E_INVALIDARG;
    shader = declaration->transformedVertexShader;
    if (!shader) {
        try {
            shader = new (std::nothrow) Triton9Shader(Triton9ShaderStage::Vertex);
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        if (!shader)
            return E_OUTOFMEMORY;
        hr = triton9ConvertTransformedVertexShader(device, shader, declaration);
        if (FAILED(hr)) {
            delete shader;
            return hr;
        }
        declaration->transformedVertexShader = shader;
    }
    hr = triton9EnsureInputLayout(device, shader, declaration);
    if (FAILED(hr))
        return hr;
    *outShader = shader;
    return S_OK;
}

static BYTE
triton9SamplerSwizzle(const TRITON9_RESOURCE *resource)
{
    const auto *owner = (resource && resource->textureOwner ? resource->textureOwner : resource);
    if (!owner)
        return ShaderConv::SAMPLER_SWIZZLE_NONE;
    switch (UINT(owner->format)) {
    case D3DFMT_L8: case D3DFMT_L16:
        return ShaderConv::SAMPLER_SWIZZLE_RRRA;
    case D3DFMT_A8L8:
        return ShaderConv::SAMPLER_SWIZZLE_RRRG;
    case D3DFMT_X8B8G8R8: case D3DFMT_X1R5G5B5: case D3DFMT_X4R4G4B4:
        return ShaderConv::SAMPLER_SWIZZLE_RGB1;
    case D3DFMT_R16F: case D3DFMT_R32F:
        return ShaderConv::SAMPLER_SWIZZLE_RAAA;
    case D3DFMT_V8U8: case D3DFMT_V16U16:
    case D3DFMT_G16R16: case D3DFMT_G16R16F: case D3DFMT_G32R32F:
        return ShaderConv::SAMPLER_SWIZZLE_RGAA;
    default:
        return ShaderConv::SAMPLER_SWIZZLE_NONE;
    }
}

static ShaderConv::RasterStates
triton9RasterStates(const TRITON9_DEVICE *device, UINT primitive, BOOL transformed)
{
    ShaderConv::RasterStates raster;
    raster.TCIMapping = ShaderConv::TCIMASK_PASSTHRU;
    raster.AlphaTestEnable = device->renderStates[D3DDDIRS_ALPHATESTENABLE] != 0;
    raster.AlphaFunc = raster.AlphaTestEnable
        ? device->renderStates[D3DDDIRS_ALPHAFUNC] : D3DCMP_ALWAYS;
    raster.FogEnable = device->renderStates[D3DDDIRS_FOGENABLE] != 0;
    raster.FogTableMode = device->renderStates[D3DDDIRS_FOGTABLEMODE];
    raster.WFogEnable = device->wFogEnable;
    raster.UserClipPlanes = transformed ? 0 : device->renderStates[D3DDDIRS_CLIPPLANEENABLE] & 63;
    raster.FillMode = device->renderStates[D3DDDIRS_FILLMODE];
    raster.ShadeMode = device->renderStates[D3DDDIRS_SHADEMODE];
    raster.PrimitiveType = primitive;
    raster.PointSizeEnable = primitive == D3DPT_POINTLIST || raster.FillMode == D3DFILL_POINT;
    raster.PointSpriteEnable = device->renderStates[D3DDDIRS_POINTSPRITEENABLE] != 0;
    raster.HasTLVertices = transformed;
    for (UINT stage = 0; stage < ShaderConv::MAX_PS_SAMPLER_REGS; ++stage) {
        const auto *resource = triton9ResourceRoot(device->textures[stage]);
        raster.PSSamplers[stage].TextureType = resource && resource->isCube
            ? ShaderConv::TEXTURETYPE_CUBE : resource && resource->isVolume
            ? ShaderConv::TEXTURETYPE_VOLUME : ShaderConv::TEXTURETYPE_2D;
        UINT wrapState = stage < 8 ? D3DDDIRS_WRAP0 + stage : D3DDDIRS_WRAP8 + stage - 8;
        raster.PSSamplers[stage].TexCoordWrap = device->renderStates[wrapState] & 15;
        raster.PSSamplerSwizzles[stage] = triton9SamplerSwizzle(resource);
    }
    for (UINT stage = 0; stage < ShaderConv::MAX_VS_SAMPLER_REGS; ++stage)
        raster.VSSamplerSwizzles[stage] = triton9SamplerSwizzle(
            device->textures[TRITON9_VERTEX_SAMPLER_BASE + stage]);
    return raster;
}

static HRESULT
triton9PrepareVertexShader(TRITON9_DEVICE *device, Triton9Shader *shader,
                           Triton9VertexDeclaration *declaration,
                           const ShaderConv::RasterStates &raster)
{
    const UINT64 serial = declaration ? declaration->serial : 0;
    HRESULT hr;
    if (!shader || shader->stage != Triton9ShaderStage::Vertex)
        return E_INVALIDARG;
    const bool changed = memcmp(&shader->rasterStates, &raster, sizeof(raster));
    shader->rasterStates = raster;
    // ConvertTLShader has no raster-dependent instructions. Its clip/point
    // work is performed by the geometry shader and the viewport extension.
    if (!shader->transformedFixedFunction && (changed ||
        shader->conversionDeclarationSerial != serial || !shader->vertexShader)) {
        hr = triton9ConvertShader(device, shader, declaration, nullptr);
        if (FAILED(hr))
            return hr;
    }
    return declaration ? triton9EnsureInputLayout(device, shader, declaration) : S_OK;
}

static HRESULT
triton9PreparePixelShader(TRITON9_DEVICE *device, Triton9Shader *shader,
                          const Triton9Shader *vertexShader,
                          ShaderConv::RasterStates raster)
{
    if (!shader || shader->stage != Triton9ShaderStage::Pixel || !vertexShader)
        return E_INVALIDARG;
    raster.FixedFunctionPixel = shader->internalFixedFunction;
    const bool changed = memcmp(&shader->rasterStates, &raster, sizeof(raster));
    shader->rasterStates = raster;
    if (changed || shader->linkedVertexShader != vertexShader ||
        shader->linkedVertexInstanceSerial != vertexShader->instanceSerial ||
        shader->linkedVertexGeneration != vertexShader->generation ||
        !shader->pixelShader)
        return triton9ConvertShader(device, shader, nullptr, vertexShader);
    return S_OK;
}

static UINT triton9ConstantBufferSize(BOOL vertex, UINT slot);
static HRESULT triton9EnsureConstantData(TRITON9_CONSTANT_BUFFER *buffer,
                                         UINT byteCount);

static HRESULT
triton9UploadPixelExtensionConstants(TRITON9_DEVICE *device)
{
    ShaderConv::PSCBExtension extension = {};
    TRITON9_CONSTANT_BUFFER *constants;
    HRESULT hr;

    if (!device)
        return E_INVALIDARG;
    extension.fAlphaRef =
        (FLOAT)(device->renderStates[D3DDDIRS_ALPHAREF] & 0xffu) / 255.0f;
    {
        const UINT color = device->renderStates[D3DDDIRS_FOGCOLOR];
        const DWORD startBits = device->renderStates[D3DDDIRS_FOGSTART];
        const DWORD endBits = device->renderStates[D3DDDIRS_FOGEND];
        const DWORD densityBits = device->renderStates[D3DDDIRS_FOGDENSITY];
        FLOAT distance;

        extension.vFogColor[0] = (FLOAT)((color >> 16) & 0xffu) / 255.0f;
        extension.vFogColor[1] = (FLOAT)((color >> 8) & 0xffu) / 255.0f;
        extension.vFogColor[2] = (FLOAT)(color & 0xffu) / 255.0f;
        memcpy(&extension.fFogStart, &startBits, sizeof(extension.fFogStart));
        memcpy(&extension.fFogEnd, &endBits, sizeof(extension.fFogEnd));
        memcpy(&extension.fFogDensity, &densityBits,
               sizeof(extension.fFogDensity));
        distance = extension.fFogEnd - extension.fFogStart;
        extension.fFogDistInv = distance != 0.0f ? 1.0f / distance : 0.0f;
    }
    constants = &device->pixelConstants[ShaderConv::CB_PS_EXT];
    hr = triton9EnsureConstantData(
        constants, triton9ConstantBufferSize(FALSE, ShaderConv::CB_PS_EXT));
    if (FAILED(hr))
        return hr;
    if (memcmp(constants->data, &extension, sizeof(extension))) {
        memcpy(constants->data, &extension, sizeof(extension));
        constants->dirty = TRUE;
    }
    ShaderConv::PSCBExtension2 bump = {};
    const UINT matrixStates[] = {D3DDDITSS_BUMPENVMAT00, D3DDDITSS_BUMPENVMAT01,
                                 D3DDDITSS_BUMPENVMAT10, D3DDDITSS_BUMPENVMAT11};
    for (UINT stage = 0; stage < 8; ++stage) {
        const UINT *values = device->textureStageStates[stage];
        for (UINT component = 0; component < 4; ++component)
            memcpy(&bump.vBumpEnvMat[stage][component],
                   &values[matrixStates[component]], sizeof(FLOAT));
        memcpy(&bump.vBumpEnvL[stage][0], &values[D3DDDITSS_BUMPENVLSCALE], sizeof(FLOAT));
        memcpy(&bump.vBumpEnvL[stage][1], &values[D3DDDITSS_BUMPENVLOFFSET], sizeof(FLOAT));
    }
    constants = &device->pixelConstants[ShaderConv::CB_PS_EXT2];
    hr = triton9EnsureConstantData(
        constants, triton9ConstantBufferSize(FALSE, ShaderConv::CB_PS_EXT2));
    if (FAILED(hr))
        return hr;
    if (memcmp(constants->data, &bump, sizeof(bump))) {
        memcpy(constants->data, &bump, sizeof(bump));
        constants->dirty = TRUE;
    }
    return S_OK;
}

static UINT
triton9ConstantBufferSize(BOOL vertex, UINT slot)
{
    if (slot == 0)
        return vertex ? 256u * 4u * sizeof(FLOAT) : 224u * 4u * sizeof(FLOAT);
    if (slot == 1)
        return 16u * 4u * sizeof(INT);
    if (slot == 2)
        return 16u * sizeof(BOOL);
    /* ShaderConverter reserves the remaining slots for viewport, fog, alpha,
     * bump and color-key extensions. Unused extension bytes remain zero. */
    return 4096;
}

static HRESULT
triton9EnsureConstantData(TRITON9_CONSTANT_BUFFER *buffer, UINT byteCount)
{
    if (!buffer || !byteCount)
        return E_INVALIDARG;
    if (buffer->byteCount && buffer->byteCount != byteCount)
        return E_FAIL;
    if (!buffer->data) {
        buffer->data = static_cast<BYTE *>(HeapAlloc(GetProcessHeap(),
                                                      HEAP_ZERO_MEMORY, byteCount));
        if (!buffer->data)
            return E_OUTOFMEMORY;
        buffer->byteCount = byteCount;
        buffer->dirty = TRUE;
    }
    return S_OK;
}

static HRESULT
triton9EnsureConstantBuffer(TRITON9_DEVICE *device,
                            TRITON9_CONSTANT_BUFFER *buffer, UINT byteCount)
{
    D3D11_BUFFER_DESC desc = {};
    HRESULT hr;

    if (!device || !device->hostDevice || !buffer)
        return E_INVALIDARG;
    hr = triton9EnsureConstantData(buffer, byteCount);
    if (FAILED(hr))
        return hr;
    if (buffer->hostBuffer)
        return S_OK;
    desc.ByteWidth = byteCount;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device->hostDevice->CreateBuffer(&desc, nullptr, &buffer->hostBuffer);
    if (FAILED(hr) || !buffer->hostBuffer)
        return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    buffer->dirty = TRUE;
    return S_OK;
}

static UINT
triton9RequiredConstantBytes(const Triton9Shader *shader, BOOL vertex, UINT slot)
{
    UINT scalarCount;
    UINT inlineScalarCount = 0;

    if (!shader)
        return 0;
    if (slot < 3) {
        const UINT components = slot == ShaderConv::CB_BOOL ? 1 : 4;
        for (const ShaderConv::ShaderConst &constant : shader->inlineConstants[slot]) {
            /* ShaderConverter stores inline offsets in scalar units. */
            const UINT firstScalar = constant.RegIndex;
            if (firstScalar > UINT_MAX - components)
                return UINT_MAX;
            inlineScalarCount = std::max(inlineScalarCount,
                                         firstScalar + components);
        }
    }
    switch (slot) {
    case 0:
        scalarCount = std::max(shader->maxFloatConstants, inlineScalarCount);
        if (scalarCount > UINT_MAX - 3u)
            return UINT_MAX;
        return ((scalarCount + 3u) & ~3u) * sizeof(FLOAT);
    case 1:
        scalarCount = std::max(shader->maxIntConstants, inlineScalarCount);
        if (scalarCount > UINT_MAX - 3u)
            return UINT_MAX;
        return ((scalarCount + 3u) & ~3u) * sizeof(INT);
    case 2:
        scalarCount = std::max(shader->maxBoolConstants, inlineScalarCount);
        if (scalarCount > UINT_MAX - 3u)
            return UINT_MAX;
        return ((scalarCount + 3u) & ~3u) * sizeof(BOOL);
    default:
        (void)vertex;
        return triton9ConstantBufferSize(vertex, slot);
    }
}

static HRESULT
triton9ApplyInlineConstants(TRITON9_CONSTANT_BUFFER *buffer,
                            const Triton9Shader *shader, UINT slot)
{
    UINT bytesPerScalar;
    UINT scalarCount;

    if (!buffer || !buffer->data || !shader || slot >= 3)
        return E_INVALIDARG;
    bytesPerScalar = slot == ShaderConv::CB_FLOAT ? sizeof(FLOAT) :
                     slot == ShaderConv::CB_INT ? sizeof(INT) : sizeof(BOOL);
    scalarCount = slot == ShaderConv::CB_BOOL ? 1 : 4;
    for (const ShaderConv::ShaderConst &constant : shader->inlineConstants[slot]) {
        UINT byteOffset;
        UINT byteCount;
        const UINT firstScalar = constant.RegIndex;

        if (firstScalar > UINT_MAX / bytesPerScalar ||
            scalarCount > UINT_MAX / bytesPerScalar)
            return D3DDDIERR_INVALIDCALL;
        byteOffset = firstScalar * bytesPerScalar;
        byteCount = scalarCount * bytesPerScalar;
        if (byteOffset > buffer->byteCount || byteCount > buffer->byteCount - byteOffset)
            return D3DDDIERR_INVALIDCALL;
        if (memcmp(buffer->data + byteOffset, constant.Value, byteCount)) {
            memcpy(buffer->data + byteOffset, constant.Value, byteCount);
            buffer->dirty = TRUE;
        }
    }
    return S_OK;
}

/* The caller holds shaderLock. */
static HRESULT
triton9BindStageConstants(TRITON9_DEVICE *device, const Triton9Shader *shader,
                          BOOL vertex)
{
    TRITON9_CONSTANT_BUFFER *constants;
    ID3D11Buffer *buffers[TRITON9_MAX_CONSTANT_BUFFERS] = {};
    std::vector<BYTE> inlineData;
    const UINT count = vertex ? 4 : TRITON9_MAX_CONSTANT_BUFFERS;
    HRESULT hr;

    if (!device || !shader || !device->hostContext ||
        shader->stage != (vertex ? Triton9ShaderStage::Vertex :
                                   Triton9ShaderStage::Pixel))
        return E_INVALIDARG;
    constants = vertex ? device->vertexConstants : device->pixelConstants;
    if (vertex && device->renderTarget) {
        D3D11_VIEWPORT viewport;
        ShaderConv::VSCBExtension extension = {};
        TRITON9_CONSTANT_BUFFER *extensionBuffer = &constants[3];
        FLOAT depthRange;

        if (device->viewportSet) {
            viewport = device->viewport;
        } else {
            viewport.TopLeftX = 0.0f;
            viewport.TopLeftY = 0.0f;
            viewport.Width = (FLOAT)device->renderTarget->width;
            viewport.Height = (FLOAT)device->renderTarget->height;
            viewport.MinDepth = device->zRangeSet ? device->viewport.MinDepth : 0.0f;
            viewport.MaxDepth = device->zRangeSet ? device->viewport.MaxDepth : 1.0f;
        }
        depthRange = viewport.MaxDepth - viewport.MinDepth;
        if (viewport.Width <= 0.0f || viewport.Height <= 0.0f ||
            depthRange < 0.0f)
            return D3DDDIERR_INVALIDCALL;
        memcpy(&extension.vPointSize[0], &device->renderStates[D3DDDIRS_POINTSIZE], sizeof(FLOAT));
        memcpy(&extension.vPointSize[1], &device->renderStates[D3DDDIRS_POINTSIZE_MIN], sizeof(FLOAT));
        memcpy(&extension.vPointSize[2], &device->renderStates[D3DDDIRS_POINTSIZE_MAX], sizeof(FLOAT));
        extension.vPointSize[0] = std::max(extension.vPointSize[0], 1.0f);
        extension.vPointSize[1] = std::max(extension.vPointSize[1], 1.0f);
        if (extension.vPointSize[2] <= 0)
            extension.vPointSize[2] = 64.0f;
        memcpy(extension.vClipPlanes, device->clipPlanes, sizeof(extension.vClipPlanes));
        if (shader->internalFixedFunction && shader->rasterStates.UserClipPlanes) {
            D3DMATRIX viewProjection, inverse;
            Triton9Fixed::multiply(device->viewTransform, device->projectionTransform,
                                   viewProjection);
            if (!Triton9Fixed::inverse(viewProjection, inverse))
                return D3DDDIERR_INVALIDCALL;
            for (UINT plane = 0; plane < 6; ++plane) {
                for (UINT row = 0; row < 4; ++row) {
                    extension.vClipPlanes[plane][row] = 0;
                    for (UINT column = 0; column < 4; ++column)
                        extension.vClipPlanes[plane][row] +=
                            inverse.m[row][column] * device->clipPlanes[plane][column];
                }
            }
        }
        extension.vViewPortScale[0] = 1.0f / viewport.Width;
        extension.vViewPortScale[1] = -1.0f / viewport.Height;
        extension.vScreenToClipOffset[0] =
            0.5f - (viewport.Width * 0.5f + viewport.TopLeftX);
        extension.vScreenToClipOffset[1] =
            0.5f - (viewport.Height * 0.5f + viewport.TopLeftY);
        extension.vScreenToClipOffset[2] = -viewport.MinDepth;
        extension.vScreenToClipScale[0] = 2.0f / viewport.Width;
        extension.vScreenToClipScale[1] = -2.0f / viewport.Height;
        extension.vScreenToClipScale[2] = depthRange ? 1.0f / depthRange : 0.0f;
        extension.vScreenToClipScale[3] = 1.0f;
        hr = triton9EnsureConstantData(extensionBuffer,
                                       triton9ConstantBufferSize(TRUE, 3));
        if (FAILED(hr))
            return hr;
        if (memcmp(extensionBuffer->data, &extension, sizeof(extension))) {
            memcpy(extensionBuffer->data, &extension, sizeof(extension));
            extensionBuffer->dirty = TRUE;
        }
    }
    for (UINT slot = 0; slot < count; ++slot) {
        TRITON9_CONSTANT_BUFFER *buffer = &constants[slot];
        const auto *fixed = static_cast<const Triton9FixedFunctionShaders *>(
            device->fixedFunctionShaders);
        if (slot == 0 && fixed && shader ==
            (vertex ? fixed->vertexShader : fixed->pixelShader))
            buffer = vertex ? &device->fixedVertexFloatConstants :
                              &device->fixedPixelFloatConstants;
        const UINT needed = triton9RequiredConstantBytes(shader, vertex, slot);
        if (!needed)
            continue;
        const UINT capacity = slot == 0 && vertex && shader->internalFixedFunction
            ? buffer->byteCount :
              triton9ConstantBufferSize(vertex, slot);
        if (needed > capacity)
            return D3DDDIERR_INVALIDCALL;
        hr = triton9EnsureConstantBuffer(device, buffer, capacity);
        if (FAILED(hr))
            return hr;
        if (slot < 3 && !shader->inlineConstants[slot].empty()) {
            /* DEF/DEFI/DEFB belong to this shader, not to the runtime's
             * saved constant registers. Overlay an upload copy so a later
             * shader can recover the runtime values without another SetConst. */
            try {
                inlineData.assign(buffer->data, buffer->data + capacity);
            } catch (...) {
                return E_OUTOFMEMORY;
            }
            TRITON9_CONSTANT_BUFFER upload = *buffer;
            upload.data = inlineData.data();
            hr = triton9ApplyInlineConstants(&upload, shader, slot);
            if (FAILED(hr))
                return hr;
            device->hostContext->UpdateSubresource(buffer->hostBuffer, 0,
                                                    nullptr, inlineData.data(), 0, 0);
            /* Host bytes now differ from the canonical runtime registers. */
            buffer->dirty = TRUE;
        } else if (buffer->dirty) {
            device->hostContext->UpdateSubresource(buffer->hostBuffer, 0,
                                                    nullptr, buffer->data,
                                                    0, 0);
            buffer->dirty = FALSE;
        }
        buffers[slot] = buffer->hostBuffer;
    }
    if (vertex)
    {
        device->hostContext->VSSetConstantBuffers(0, count, buffers);
        device->hostContext->GSSetConstantBuffers(3, 1, &buffers[3]);
    }
    else
        device->hostContext->PSSetConstantBuffers(0, count, buffers);
    return S_OK;
}

static HRESULT
triton9SetConstants(TRITON9_DEVICE *device, BOOL vertex, UINT slot,
                    UINT registerIndex, UINT count, const void *data,
                    UINT maxRegisters, UINT bytesPerRegister)
{
    TRITON9_CONSTANT_BUFFER *constants;
    UINT byteCount;
    HRESULT hr;

    if (!device || !device->shaderLockInitialized || slot >= TRITON9_MAX_CONSTANT_BUFFERS ||
        (count && !data) || registerIndex > maxRegisters ||
        count > maxRegisters - registerIndex)
        return E_INVALIDARG;
    if (!count)
        return S_OK;
    if (bytesPerRegister > UINT_MAX / count ||
        registerIndex > UINT_MAX / bytesPerRegister)
        return E_INVALIDARG;
    byteCount = count * bytesPerRegister;
    constants = vertex ? device->vertexConstants : device->pixelConstants;
    EnterCriticalSection(&device->shaderLock);
    hr = triton9EnsureConstantData(&constants[slot],
                                   triton9ConstantBufferSize(vertex, slot));
    if (SUCCEEDED(hr)) {
        memcpy(constants[slot].data + registerIndex * bytesPerRegister,
               data, byteCount);
        constants[slot].dirty = TRUE;
        /* Draw preparation uploads these canonical runtime registers and
         * overlays that draw's shader-local DEF values. Deferring the host
         * binding coalesces consecutive SetConstants calls and also avoids
         * re-entering the runtime while CreateDeviceEx initializes state. */
    }
    if (SUCCEEDED(hr) && device->hostContext)
        hr = triton9CheckHostDevice(device);
    LeaveCriticalSection(&device->shaderLock);
    return triton9MapDeviceFailure(device, hr);
}

static UINT64
triton9FixedTokenKey(const std::vector<UINT> &tokens)
{
    UINT64 key = UINT64_C(1469598103934665603);
    for (UINT token : tokens) {
        key ^= token;
        key *= UINT64_C(1099511628211);
    }
    return key;
}

static void
triton9FixedSnapshot(const TRITON9_DEVICE *device,
                     const Triton9VertexDeclaration *declaration,
                     const Triton9Shader *vertexShader, Triton9Fixed::State &state)
{
    static_assert(sizeof(state.render) >= sizeof(device->renderStates),
                  "Fixed render-state snapshot must include all DDI states");
    memcpy(state.render, device->renderStates, sizeof(device->renderStates));
    memcpy(state.world, device->worldTransforms, sizeof(state.world));
    state.view = device->viewTransform;
    state.projection = device->projectionTransform;
    memcpy(state.texture, device->textureTransforms, sizeof(state.texture));
    static_assert(sizeof(state.material) == sizeof(device->material),
                  "D3D9 material layout");
    memcpy(&state.material, &device->material, sizeof(state.material));
    state.viewportHeight = device->viewportSet ? device->viewport.Height :
        device->renderTarget ? FLOAT(device->renderTarget->height) : 1.0f;
    std::vector<const TRITON9_LIGHT *> lights;
    for (const auto *light = device->lights; light; light = light->next)
        if (light->enabled)
            lights.push_back(light);
    std::sort(lights.begin(), lights.end(), [](const TRITON9_LIGHT *a,
                                             const TRITON9_LIGHT *b) {
        return a->index < b->index;
    });
    for (const auto *light : lights) {
        D3DLIGHT9 value;
        static_assert(sizeof(value) == sizeof(light->data), "D3D9 light layout");
        memcpy(&value, &light->data, sizeof(value));
        state.lights.push_back(value);
    }
    if (declaration) {
        for (UINT i = 0; i < declaration->inputDecls.GetSize(); ++i) {
            const auto &input = declaration->inputDecls[i];
            const auto *element = triton9FindDeclarationElement(
                declaration, input.Usage, input.UsageIndex);
            UINT components = element && element->Type <= D3DDECLTYPE_FLOAT4
                ? element->Type - D3DDECLTYPE_FLOAT1 + 1 : 4;
            if (input.Usage == D3DDECLUSAGE_BLENDINDICES && element)
                state.normalizedBlendIndices = element->Type == D3DDECLTYPE_D3DCOLOR ||
                                               element->Type == D3DDECLTYPE_UBYTE4N;
            state.inputs.push_back({input.Usage, input.UsageIndex,
                                    input.RegIndex, components});
        }
    }
    if (vertexShader) {
        state.transformed = vertexShader->transformedFixedFunction;
        state.diffuse = vertexShader->outputDecls.FindOutputDecl(D3DDECLUSAGE_COLOR, 0);
        state.specular = vertexShader->outputDecls.FindOutputDecl(D3DDECLUSAGE_COLOR, 1);
        state.texcoords = vertexShader->outputDecls.TexCoords;
    }
    for (UINT i = 0; i < Triton9Fixed::StageCount; ++i) {
        const UINT *values = device->textureStageStates[i];
        auto &stage = state.stages[i];
        stage.colorOp = values[D3DDDITSS_COLOROP];
        stage.alphaOp = values[D3DDDITSS_ALPHAOP];
        stage.colorArg[0] = values[D3DDDITSS_COLORARG0];
        stage.colorArg[1] = values[D3DDDITSS_COLORARG1];
        stage.colorArg[2] = values[D3DDDITSS_COLORARG2];
        stage.alphaArg[0] = values[D3DDDITSS_ALPHAARG0];
        stage.alphaArg[1] = values[D3DDDITSS_ALPHAARG1];
        stage.alphaArg[2] = values[D3DDDITSS_ALPHAARG2];
        stage.result = values[D3DDDITSS_RESULTARG];
        stage.texcoord = values[D3DDDITSS_TEXCOORDINDEX];
        stage.transform = values[D3DDDITSS_TEXTURETRANSFORMFLAGS];
        if (vertexShader) {
            // FFP VS already applied TEXCOORDINDEX; a programmable VS always
            // supplies TEXCOORD[stage]. POSITIONT bypasses texture transforms.
            if (!vertexShader->transformedFixedFunction)
                stage.texcoord = i;
            else
                stage.transform = D3DTTFF_DISABLE;
        }
        stage.constant = values[D3DDDITSS_CONSTANT];
        stage.bound = device->textures[i] != nullptr;
        const auto *texture = triton9ResourceRoot(device->textures[i]);
        stage.textureType = texture && texture->isCube ? D3DSTT_CUBE :
            texture && texture->isVolume ? D3DSTT_VOLUME : D3DSTT_2D;
        const UINT bumpStates[] = {D3DDDITSS_BUMPENVMAT00, D3DDDITSS_BUMPENVMAT01,
            D3DDDITSS_BUMPENVMAT10, D3DDDITSS_BUMPENVMAT11};
        for (UINT j = 0; j < 4; ++j)
            memcpy(&stage.bump[j], &values[bumpStates[j]], sizeof(FLOAT));
        memcpy(&stage.bumpScale, &values[D3DDDITSS_BUMPENVLSCALE], sizeof(FLOAT));
        memcpy(&stage.bumpOffset, &values[D3DDDITSS_BUMPENVLOFFSET], sizeof(FLOAT));
    }
}

static HRESULT
triton9UploadFixedProgram(TRITON9_DEVICE *device, BOOL vertex,
                          const Triton9Fixed::Program &program)
{
    auto *buffer = vertex ? &device->fixedVertexFloatConstants :
                           &device->fixedPixelFloatConstants;
    const UINT size = vertex ? (UINT)program.constants.size() * 16 :
                              triton9ConstantBufferSize(FALSE, 0);
    if (buffer->byteCount && buffer->byteCount != size) {
        if (buffer->hostBuffer)
            buffer->hostBuffer->Release();
        HeapFree(GetProcessHeap(), 0, buffer->data);
        *buffer = {};
    }
    HRESULT hr = triton9EnsureConstantData(buffer, size);
    if (FAILED(hr))
        return hr;
    const size_t bytes = program.constants.size() * sizeof(Triton9Fixed::Constant);
    if (bytes > size)
        return E_INVALIDARG;
    if (memcmp(buffer->data, program.constants.data(), bytes)) {
        memcpy(buffer->data, program.constants.data(), bytes);
        buffer->dirty = TRUE;
    }
    return S_OK;
}

static Triton9FixedFunctionShaders *
triton9FixedShaders(TRITON9_DEVICE *device)
{
    Triton9FixedFunctionShaders *shaders;

    if (!device)
        return nullptr;
    shaders = static_cast<Triton9FixedFunctionShaders *>(device->fixedFunctionShaders);
    if (!shaders) {
        shaders = new (std::nothrow) Triton9FixedFunctionShaders;
        if (!shaders)
            return nullptr;
        device->fixedFunctionShaders = shaders;
    }
    return shaders;
}

static HRESULT
triton9RememberFixedState(Triton9Fixed::State **snapshot,
                          Triton9Fixed::State &&state)
{
    try {
        if (!*snapshot)
            *snapshot = new Triton9Fixed::State;
        **snapshot = std::move(state);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

static HRESULT
triton9EnsureFixedVertexShader(TRITON9_DEVICE *device,
                               Triton9VertexDeclaration *declaration,
                               Triton9Shader **outShader)
{
    Triton9FixedFunctionShaders *shaders;
    Triton9Shader *shader;
    Triton9Fixed::State state;
    Triton9Fixed::Program program;
    UINT64 stateKey;
    HRESULT hr;

    if (!device || !declaration || !outShader)
        return E_INVALIDARG;
    if (declaration->hasTransformedPosition)
        return triton9EnsureTransformedVertexShader(device, declaration, outShader);
    try {
        triton9FixedSnapshot(device, declaration, nullptr, state);
        shaders = triton9FixedShaders(device);
        if (!shaders)
            return E_OUTOFMEMORY;
        if (shaders->vertexShader && shaders->vertexSnapshot &&
            shaders->vertexDeclaration == declaration &&
            shaders->vertexDeclarationSerial == declaration->serial &&
            Triton9Fixed::equal(*shaders->vertexSnapshot, state)) {
            hr = triton9EnsureInputLayout(device, shaders->vertexShader, declaration);
            if (SUCCEEDED(hr))
                *outShader = shaders->vertexShader;
            return hr;
        }
        hr = Triton9Fixed::vertex(state, program);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    if (FAILED(hr))
        return hr;
    delete shaders->vertexSnapshot;
    shaders->vertexSnapshot = nullptr;
    hr = triton9UploadFixedProgram(device, TRUE, program);
    if (FAILED(hr))
        return hr;
    stateKey = triton9FixedTokenKey(program.tokens);
    shaders = triton9FixedShaders(device);
    if (!shaders)
        return E_OUTOFMEMORY;
    if (shaders->vertexShader && shaders->vertexDeclaration == declaration &&
        shaders->vertexDeclarationSerial == declaration->serial &&
        shaders->vertexStateKey == stateKey &&
        shaders->vertexShader->legacyTokens == program.tokens) {
        hr = triton9EnsureInputLayout(device, shaders->vertexShader, declaration);
        if (SUCCEEDED(hr))
            *outShader = shaders->vertexShader;
        if (SUCCEEDED(hr))
            hr = triton9RememberFixedState(&shaders->vertexSnapshot, std::move(state));
        return hr;
    }
    shader = new (std::nothrow) Triton9Shader(Triton9ShaderStage::Vertex);
    if (!shader)
        return E_OUTOFMEMORY;
    try {
        shader->legacyTokens = std::move(program.tokens);
        shader->internalFixedFunction = TRUE;
    } catch (...) {
        delete shader;
        return E_OUTOFMEMORY;
    }
    hr = triton9ConvertShader(device, shader, declaration, nullptr);
    if (SUCCEEDED(hr))
        hr = triton9EnsureInputLayout(device, shader, declaration);
    if (FAILED(hr)) {
        delete shader;
        return hr;
    }
    delete shaders->vertexShader;
    shaders->vertexShader = shader;
    shaders->vertexDeclaration = declaration;
    shaders->vertexDeclarationSerial = declaration->serial;
    shaders->vertexStateKey = stateKey;
    *outShader = shader;
    return triton9RememberFixedState(&shaders->vertexSnapshot, std::move(state));
}

static HRESULT
triton9EnsureFixedPixelShader(TRITON9_DEVICE *device,
                              const Triton9Shader *vertexShader,
                              Triton9Shader **outShader)
{
    Triton9FixedFunctionShaders *shaders;
    Triton9Shader *shader;
    Triton9Fixed::State state;
    Triton9Fixed::Program program;
    UINT64 stateKey;
    HRESULT hr;

    if (!device || !vertexShader || !outShader)
        return E_INVALIDARG;
    try {
        triton9FixedSnapshot(device, nullptr, vertexShader, state);
        shaders = triton9FixedShaders(device);
        if (!shaders)
            return E_OUTOFMEMORY;
        if (shaders->pixelShader && shaders->pixelSnapshot &&
            shaders->pixelVertexShader == vertexShader &&
            shaders->pixelVertexInstanceSerial == vertexShader->instanceSerial &&
            shaders->pixelVertexGeneration == vertexShader->generation &&
            Triton9Fixed::equal(*shaders->pixelSnapshot, state)) {
            *outShader = shaders->pixelShader;
            return S_OK;
        }
        hr = Triton9Fixed::pixel(state, program);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    if (FAILED(hr))
        return hr;
    delete shaders->pixelSnapshot;
    shaders->pixelSnapshot = nullptr;
    hr = triton9UploadFixedProgram(device, FALSE, program);
    if (FAILED(hr))
        return hr;
    stateKey = triton9FixedTokenKey(program.tokens);
    shaders = triton9FixedShaders(device);
    if (!shaders)
        return E_OUTOFMEMORY;
    if (shaders->pixelShader && shaders->pixelStateKey == stateKey &&
        shaders->pixelShader->legacyTokens == program.tokens &&
        shaders->pixelVertexShader == vertexShader &&
        shaders->pixelVertexInstanceSerial == vertexShader->instanceSerial &&
        shaders->pixelVertexGeneration == vertexShader->generation) {
        *outShader = shaders->pixelShader;
        return triton9RememberFixedState(&shaders->pixelSnapshot, std::move(state));
    }
    shader = new (std::nothrow) Triton9Shader(Triton9ShaderStage::Pixel);
    if (!shader)
        return E_OUTOFMEMORY;
    try {
        shader->legacyTokens = std::move(program.tokens);
        shader->internalFixedFunction = TRUE;
    } catch (...) {
        delete shader;
        return E_OUTOFMEMORY;
    }
    hr = triton9ConvertShader(device, shader, nullptr, vertexShader);
    if (FAILED(hr)) {
        delete shader;
        return hr;
    }
    delete shaders->pixelShader;
    shaders->pixelShader = shader;
    shaders->pixelStateKey = stateKey;
    shaders->pixelVertexShader = vertexShader;
    shaders->pixelVertexInstanceSerial = vertexShader->instanceSerial;
    shaders->pixelVertexGeneration = vertexShader->generation;
    *outShader = shader;
    return triton9RememberFixedState(&shaders->pixelSnapshot, std::move(state));
}

static HRESULT
triton9RejectFixedFunction(HRESULT hr, const char *message)
{
    if (hr == D3DDDIERR_NOTAVAILABLE && message)
        OutputDebugStringA(message);
    return hr;
}

static HRESULT
triton9PrimitiveTopology(D3DPRIMITIVETYPE primitiveType, UINT primitiveCount,
                         D3D11_PRIMITIVE_TOPOLOGY *topology, UINT *vertexCount)
{
    UINT sourceCount;
    UINT drawCount;
    int expansion;

    if (!topology || !vertexCount)
        return E_INVALIDARG;
    if (!triton9_draw_primitive_counts((UINT)primitiveType, primitiveCount,
                                       &sourceCount, &drawCount, &expansion))
        return D3DDDIERR_INVALIDCALL;
    switch (primitiveType) {
    case D3DPT_POINTLIST:
        *topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        break;
    case D3DPT_LINELIST:
        *topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
    case D3DPT_LINESTRIP:
        *topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
    case D3DPT_TRIANGLELIST:
        *topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
    case D3DPT_TRIANGLESTRIP:
        *topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
    case D3DPT_TRIANGLEFAN:
        *topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
    default:
        return D3DDDIERR_NOTAVAILABLE;
    }
    (void)drawCount;
    (void)expansion;
    *vertexCount = sourceCount;
    return S_OK;
}

/* Validate every input before refreshing the consumed SYSTEMMEM ranges.
 * The caller holds shaderLock. */
static HRESULT
triton9ValidateStreamRange(TRITON9_DEVICE *device, const Triton9Shader *shader,
                           const Triton9VertexDeclaration *declaration,
                           UINT firstVertex, UINT vertexCount,
                           UINT instanceCount)
{
    UINT rangeFirst[TRITON9_MAX_VERTEX_STREAMS];
    UINT rangeEnd[TRITON9_MAX_VERTEX_STREAMS] = {};

    if (!device || !shader || !declaration)
        return E_INVALIDARG;
    for (UINT stream = 0; stream < TRITON9_MAX_VERTEX_STREAMS; ++stream)
        rangeFirst[stream] = UINT_MAX;
    for (UINT i = 0; i < shader->inputDecls.GetSize(); ++i) {
        const ShaderConv::VSInputDecl &input = shader->inputDecls[i];
        const D3DDDIVERTEXELEMENT *element = triton9FindDeclarationElement(
            declaration, input.Usage, input.UsageIndex);
        TRITON9_RESOURCE *resource;
        UINT stride;
        UINT firstElement;
        UINT elementCount;
        UINT elementSize;
        UINT64 lastElement;
        UINT64 base;
        UINT64 end;

        if (!element || element->Stream >= TRITON9_MAX_VERTEX_STREAMS)
            return D3DDDIERR_INVALIDCALL;
        resource = device->streamResources[element->Stream];
        stride = device->streamStrides[element->Stream];
        elementSize = triton9DeclarationTypeSize(element->Type);
        if (!stride || !elementSize || element->Offset > stride ||
            elementSize > stride - element->Offset)
            return D3DDDIERR_INVALIDCALL;
        {
            HRESULT hr = triton9GetStreamElementRange(
                device, element->Stream, firstVertex, vertexCount, instanceCount,
                &firstElement, &elementCount);
            if (FAILED(hr))
                return hr;
        }
        if (device->upVertexData[element->Stream]) {
            if (!device->upVertexBuffers[element->Stream] ||
                !device->drawStreamOffsetOverrides[element->Stream])
                return D3DDDIERR_INVALIDCALL;
            base = (UINT64)device->drawStreamOffsets[element->Stream] +
                   element->Offset;
            if (!elementCount)
                return D3DDDIERR_INVALIDCALL;
            lastElement = (UINT64)firstElement + elementCount - 1u;
            if (lastElement > (UINT64_MAX - base - elementSize) / stride)
                return D3DDDIERR_INVALIDCALL;
            end = base + lastElement * stride + elementSize;
            if (end > device->upVertexBytes[element->Stream])
                return D3DDDIERR_INVALIDCALL;
            continue;
        }
        if (!resource || !resource->isBuffer)
            return D3DDDIERR_INVALIDCALL;
        base = (UINT64)(device->drawStreamOffsetOverrides[element->Stream]
                            ? device->drawStreamOffsets[element->Stream]
                            : device->streamOffsets[element->Stream]) +
               element->Offset;
        if (!elementCount)
            return D3DDDIERR_INVALIDCALL;
        lastElement = (UINT64)firstElement + elementCount - 1u;
        if (lastElement > (UINT64_MAX - base - elementSize) / stride)
            return D3DDDIERR_INVALIDCALL;
        end = base + lastElement * stride + elementSize;
        if (end > resource->width)
            return D3DDDIERR_INVALIDCALL;
        /* Include the gaps between elements, but do not scan unused portions
         * of a large buffer. Coalesce declarations sharing the same stream. */
        UINT first = (UINT)(base + (UINT64)firstElement * stride);
        if (first < rangeFirst[element->Stream])
            rangeFirst[element->Stream] = first;
        if (end > rangeEnd[element->Stream])
            rangeEnd[element->Stream] = (UINT)end;
    }
    for (UINT stream = 0; stream < TRITON9_MAX_VERTEX_STREAMS; ++stream) {
        if (rangeEnd[stream]) {
            HRESULT hr = triton9PrepareBufferRangeForHostRead(
                device, device->streamResources[stream], rangeFirst[stream],
                rangeEnd[stream]);
            if (FAILED(hr))
                return hr;
        }
    }
    return S_OK;
}

/* Converter geometry expansion is required for point size/sprites, point fill,
 * and wrapped interpolation. Flat color interpolation is emitted by the PS. */
static HRESULT
triton9PrepareGeometryShader(TRITON9_DEVICE *device, Triton9Shader *vertex,
                             const ShaderConv::RasterStates &raster,
                             const Triton9Shader **pixelLinkage)
{
    bool needed = raster.PrimitiveType == D3DPT_POINTLIST ||
                  raster.FillMode == D3DFILL_POINT;
    if (raster.PrimitiveType != D3DPT_POINTLIST)
        for (UINT i = 0; i < ShaderConv::MAX_PS_SAMPLER_REGS; ++i)
            needed |= (vertex->outputDecls.TexCoords & (1u << i)) &&
                      raster.PSSamplers[i].TexCoordWrap;
    if (!needed) {
        device->hostContext->GSSetShader(nullptr, nullptr, 0);
        return S_OK;
    }
    auto *cache = triton9FixedShaders(device);
    if (!cache)
        return E_OUTOFMEMORY;
    if (!cache->geometryShader || cache->geometryVertex != vertex ||
        cache->geometryInstanceSerial != vertex->instanceSerial ||
        cache->geometryGeneration != vertex->generation ||
        memcmp(&cache->geometryRaster, &raster, sizeof(raster))) {
        auto *linkage = new (std::nothrow) Triton9Shader(Triton9ShaderStage::Vertex);
        if (!linkage)
            return E_OUTOFMEMORY;
        ShaderConv::CreateGeometryShaderArgs args(9,
            ShaderConv::AnythingTimes0Equals0, vertex->outputDecls,
            &linkage->outputDecls, raster);
        std::vector<TRITON_DXBC_SIGNATURE> inputSignatures, outputSignatures;
        HRESULT hr;
        try {
            hr = vertex->converter.CreateGeometryShader(args);
        } catch (...) {
            hr = E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr))
            hr = triton9BuildVsOutputSignatures(vertex->outputDecls, &inputSignatures);
        if (SUCCEEDED(hr))
            hr = triton9BuildVsOutputSignatures(linkage->outputDecls, &outputSignatures);
        void *dxbc = nullptr;
        SIZE_T dxbcSize = 0;
        if (SUCCEEDED(hr)) {
            dxbc = tritonBuildDxbc(static_cast<const UINT *>(args.m_GSByteCode.m_pByteCode),
                args.m_GSByteCode.m_byteCodeSize, inputSignatures.data(),
                (UINT)inputSignatures.size(), outputSignatures.data(),
                (UINT)outputSignatures.size(), nullptr, 0,
                sizeof(TRITON_DXBC_SIGNATURE), &dxbcSize);
            if (!dxbc)
                hr = E_OUTOFMEMORY;
        }
        ID3D11GeometryShader *geometry = nullptr;
        if (SUCCEEDED(hr))
            hr = device->hostDevice->CreateGeometryShader(dxbc, dxbcSize,
                                                          nullptr, &geometry);
        if (dxbc)
            HeapFree(GetProcessHeap(), 0, dxbc);
        ShaderConv::ShaderConverterAPI::CleanUpConvertedShader(args.m_GSByteCode);
        if (FAILED(hr)) {
            delete linkage;
            return hr;
        }
        linkage->generation = 1;
        linkage->transformedFixedFunction = vertex->transformedFixedFunction;
        delete cache->geometryLinkage;
        if (cache->geometryShader)
            cache->geometryShader->Release();
        cache->geometryShader = geometry;
        cache->geometryLinkage = linkage;
        cache->geometryVertex = vertex;
        cache->geometryInstanceSerial = vertex->instanceSerial;
        cache->geometryGeneration = vertex->generation;
        cache->geometryRaster = raster;
    }
    device->hostContext->GSSetShader(cache->geometryShader, nullptr, 0);
    *pixelLinkage = cache->geometryLinkage;
    return S_OK;
}

/* The caller holds shaderLock. */
static HRESULT
triton9PrepareDrawFailure(UINT stage, HRESULT hr)
{
    triton9DiagU32("TRITON9-DRAW-PREP-FAIL-STAGE", stage);
    triton9DiagU32("TRITON9-DRAW-PREP-FAIL-HR", (DWORD)hr);
    return hr;
}

/* The caller holds shaderLock. */
static HRESULT
triton9PrepareDraw(TRITON9_DEVICE *device, UINT primitive)
{
    Triton9Shader *vertexShader;
    Triton9Shader *pixelShader;
    Triton9VertexDeclaration *declaration;
    HRESULT hr;

    hr = triton9EnsureHostDevice(device);
    if (FAILED(hr))
        return triton9PrepareDrawFailure(1, hr);

    if (!device || !device->renderTarget)
        return triton9PrepareDrawFailure(2, D3DDDIERR_INVALIDCALL);
    hr = triton9SynchronizeLockedBuffers(device);
    if (FAILED(hr))
        return triton9PrepareDrawFailure(3, hr);
    device->drawVertexShader = nullptr;
    device->drawVertexDeclaration = nullptr;
    vertexShader = static_cast<Triton9Shader *>(device->activeVertexShader);
    pixelShader = static_cast<Triton9Shader *>(device->activePixelShader);
    declaration = static_cast<Triton9VertexDeclaration *>(
        device->activeVertexDeclaration);
    if (!declaration && device->streamResources[0])
        declaration = static_cast<Triton9VertexDeclaration *>(
            device->streamResources[0]->fvfDeclaration);
    if (!declaration)
        return triton9PrepareDrawFailure(4, D3DDDIERR_INVALIDCALL);
    /* POSITIONT bypasses all vertex processing, including a shader left bound
     * by an earlier draw. Keep the application binding intact so switching
     * back to an ordinary declaration restores it. */
    if (!vertexShader || declaration->hasTransformedPosition) {
        hr = triton9EnsureFixedVertexShader(device, declaration, &vertexShader);
        if (FAILED(hr))
            return triton9PrepareDrawFailure(5, triton9RejectFixedFunction(
                hr, "neptune_d3d9: rejected unsupported fixed-function vertex state.\n"));
    }
    const auto raster = triton9RasterStates(device, primitive,
                                             declaration->hasTransformedPosition);
    hr = triton9PrepareVertexShader(device, vertexShader, declaration, raster);
    if (FAILED(hr))
        return triton9PrepareDrawFailure(7, hr);
    const Triton9Shader *pixelLinkage = vertexShader;
    hr = triton9PrepareGeometryShader(device, vertexShader, raster, &pixelLinkage);
    if (FAILED(hr))
        return triton9PrepareDrawFailure(15, hr);
    if (!pixelShader) {
        hr = triton9EnsureFixedPixelShader(device, pixelLinkage, &pixelShader);
        if (FAILED(hr))
            return triton9PrepareDrawFailure(6, triton9RejectFixedFunction(
                hr, "neptune_d3d9: rejected invalid fixed-function pixel state.\n"));
    }
    hr = triton9PreparePixelShader(device, pixelShader, pixelLinkage, raster);
    if (FAILED(hr))
        return triton9PrepareDrawFailure(8, hr);
    hr = triton9UploadPixelExtensionConstants(device);
    if (FAILED(hr))
        return triton9PrepareDrawFailure(11, hr);
    hr = triton9BindStageConstants(device, vertexShader, TRUE);
    if (FAILED(hr))
        return triton9PrepareDrawFailure(12, hr);
    hr = triton9BindStageConstants(device, pixelShader, FALSE);
    if (FAILED(hr))
        return triton9PrepareDrawFailure(13, hr);
    hr = triton9PreparePipelineState(device);
    if (FAILED(hr))
        return triton9PrepareDrawFailure(14, hr);
    device->hostContext->VSSetShader(vertexShader->vertexShader, nullptr, 0);
    device->hostContext->PSSetShader(pixelShader->pixelShader, nullptr, 0);
    device->hostContext->IASetInputLayout(declaration->inputLayout);
    device->drawVertexShader = vertexShader;
    device->drawVertexDeclaration = declaration;
    return S_OK;
}

static HRESULT
triton9CreateShader(TRITON9_DEVICE *device, Triton9ShaderStage stage,
                    UINT byteCount, const UINT *tokens, HANDLE *outShader)
{
    Triton9Shader *shader = nullptr;

    if (!device || !outShader || !triton9ValidateLegacyShader(tokens, byteCount, stage))
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    *outShader = nullptr;

    try {
        shader = new (std::nothrow) Triton9Shader(stage);
        if (!shader)
            return E_OUTOFMEMORY;
        shader->legacyTokens.assign(tokens, tokens + byteCount / sizeof(*tokens));
    } catch (...) {
        delete shader;
        return E_OUTOFMEMORY;
    }
    /* ShaderConverter needs the active vertex declaration for a VS and the
     * converted VS outputs for a PS. D3D9 permits shaders, declarations, and
     * bindings to be created in any order, so conversion belongs at draw
     * preparation, when both pieces are known. */
    *outShader = static_cast<HANDLE>(shader);
    return S_OK;
}

static HRESULT
triton9SetShader(TRITON9_DEVICE *device, Triton9ShaderStage stage, HANDLE handle)
{
    Triton9Shader *shader = static_cast<Triton9Shader *>(handle);

    if (!device || !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (shader && shader->stage != stage)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;

    /* A null binding is only local D3D9 state; so is every intermediate
     * non-null binding. D3D9 setters describe pending state, not a complete
     * pipeline. The runtime commonly replaces a fixed-function declaration,
     * VS, and PS in separate calls. Compiling the old VS against the incoming
     * declaration here rejects that valid transition before the new VS
     * arrives. Keep every setter local and assemble the coherent
     * declaration/VS/PS tuple only in triton9PrepareDraw(). */
    EnterCriticalSection(&device->shaderLock);
    if (stage == Triton9ShaderStage::Vertex) {
        device->activeVertexShader = shader;
        device->drawVertexShader = nullptr;
        device->drawVertexDeclaration = nullptr;
    } else {
        device->activePixelShader = shader;
    }
    LeaveCriticalSection(&device->shaderLock);
    return S_OK;
}

static HRESULT
triton9DeleteShader(TRITON9_DEVICE *device, Triton9ShaderStage stage, HANDLE handle)
{
    Triton9Shader *shader = static_cast<Triton9Shader *>(handle);

    if (!device || !shader || shader->stage != stage ||
        !device->shaderLockInitialized)
        return E_INVALIDARG;

    EnterCriticalSection(&device->shaderLock);
    if (device->drawVertexShader == shader)
        device->drawVertexShader = nullptr;
    if (stage == Triton9ShaderStage::Vertex && device->activeVertexShader == shader) {
        if (device->hostContext) {
            device->hostContext->VSSetShader(nullptr, nullptr, 0);
            device->hostContext->IASetInputLayout(nullptr);
        }
        device->activeVertexShader = nullptr;
    } else if (stage == Triton9ShaderStage::Pixel && device->activePixelShader == shader) {
        if (device->hostContext)
            device->hostContext->PSSetShader(nullptr, nullptr, 0);
        device->activePixelShader = nullptr;
    }
    LeaveCriticalSection(&device->shaderLock);
    delete shader;
    return S_OK;
}

static HRESULT
triton9GetBuffer(TRITON9_RESOURCE *resource, UINT requiredBind,
                 ID3D11Buffer **outBuffer)
{
    void *object = nullptr;
    HRESULT hr;

    if (!resource || !outBuffer || !resource->isBuffer || !resource->hostResource ||
        !(resource->hostBindFlags & requiredBind))
        return D3DDDIERR_INVALIDCALL;
    *outBuffer = nullptr;
    hr = resource->hostResource->QueryInterface(IID_ID3D11Buffer, &object);
    if (FAILED(hr) || !object)
        return FAILED(hr) ? hr : E_FAIL;
    *outBuffer = static_cast<ID3D11Buffer *>(object);
    return S_OK;
}

/* The Vista DDI gives UP callbacks a pointer but no byte count.  The matching
 * draw callback supplies the count and byte offsets, so upload only there.
 * The caller holds shaderLock for all helpers below. */
static HRESULT
triton9EnsureUpBuffer(TRITON9_DEVICE *device, ID3D11Buffer **buffer,
                      UINT *capacity, UINT requiredBytes, UINT bindFlags)
{
    D3D11_BUFFER_DESC desc = {};
    ID3D11Buffer *newBuffer = nullptr;
    UINT newCapacity;
    HRESULT hr;

    if (!device || !device->hostDevice || !buffer || !capacity || !requiredBytes)
        return E_INVALIDARG;
    if (*buffer && *capacity >= requiredBytes)
        return S_OK;

    newCapacity = *capacity;
    if (newCapacity < 4096u)
        newCapacity = 4096u;
    while (newCapacity < requiredBytes && newCapacity <= UINT_MAX / 2u)
        newCapacity *= 2u;
    if (newCapacity < requiredBytes)
        newCapacity = requiredBytes;

    desc.ByteWidth = newCapacity;
    /* Neptune already has a bounded RESOURCE_UPDATE path for DEFAULT buffers.
     * Using it here also avoids making Vista user pointers depend on the
     * proxy's Map/rename implementation. */
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bindFlags;
    hr = device->hostDevice->CreateBuffer(&desc, nullptr, &newBuffer);
    if (FAILED(hr))
        return triton9MapDeviceFailure(device, hr);
    if (*buffer)
        (*buffer)->Release();
    *buffer = newBuffer;
    *capacity = newCapacity;
    return S_OK;
}

static HRESULT
triton9UploadUpBuffer(TRITON9_DEVICE *device, ID3D11Buffer **buffer,
                      UINT *capacity, UINT bindFlags, const VOID *data,
                      UINT bytes)
{
    D3D11_BOX box = {};
    HRESULT hr;

    if (!data || !bytes)
        return E_INVALIDARG;
    hr = triton9EnsureUpBuffer(device, buffer, capacity, bytes, bindFlags);
    if (FAILED(hr))
        return hr;
    box.right = bytes;
    box.bottom = 1;
    box.back = 1;
    device->hostContext->UpdateSubresource(*buffer, 0, &box, data, bytes, bytes);
    return S_OK;
}

static HRESULT
triton9BindUpVertexStreams(TRITON9_DEVICE *device,
                          const Triton9Shader *shader,
                          const Triton9VertexDeclaration *declaration,
                          UINT firstVertex, UINT vertexCount,
                          UINT instanceCount)
{
    UINT streamMask = 0;

    if (!device || !shader || !declaration)
        return E_INVALIDARG;
    for (UINT i = 0; i < shader->inputDecls.GetSize(); ++i) {
        const ShaderConv::VSInputDecl &input = shader->inputDecls[i];
        const D3DDDIVERTEXELEMENT *element = triton9FindDeclarationElement(
            declaration, input.Usage, input.UsageIndex);
        UINT stream;
        UINT stride;
        UINT firstElement;
        UINT elementCount;
        UINT bytes;

        if (!element || element->Stream >= TRITON9_MAX_VERTEX_STREAMS)
            return D3DDDIERR_INVALIDCALL;
        stream = element->Stream;
        if (streamMask & (1u << stream))
            continue;
        streamMask |= 1u << stream;
        stride = device->streamStrides[stream];
        if (!stride)
            return D3DDDIERR_INVALIDCALL;

        {
            HRESULT hr = triton9GetStreamElementRange(
                device, stream, firstVertex, vertexCount, instanceCount,
                &firstElement, &elementCount);
            if (FAILED(hr))
                return hr;
        }

        if (!device->upVertexData[stream])
            continue;
        if (!triton9_draw_um_prefix_bytes(firstElement, elementCount, stride,
                                          &bytes))
            return D3DDDIERR_INVALIDCALL;
        {
            ID3D11Buffer *buffer;
            UINT offset = 0;
            HRESULT hr = triton9UploadUpBuffer(
                device, &device->upVertexBuffers[stream],
                &device->upVertexCapacities[stream], D3D11_BIND_VERTEX_BUFFER,
                device->upVertexData[stream], bytes);

            if (FAILED(hr))
                return hr;
            buffer = device->upVertexBuffers[stream];
            device->hostContext->IASetVertexBuffers(stream, 1, &buffer, &stride,
                                                     &offset);
            device->upVertexBytes[stream] = bytes;
            device->drawStreamOffsets[stream] = 0;
            device->drawStreamOffsetOverrides[stream] = TRUE;
        }
    }
    return S_OK;
}

static HRESULT
triton9RequireDraw2StreamZero(const Triton9Shader *shader,
                              const Triton9VertexDeclaration *declaration)
{
    if (!shader || !declaration)
        return E_INVALIDARG;
    for (UINT i = 0; i < shader->inputDecls.GetSize(); ++i) {
        const ShaderConv::VSInputDecl &input = shader->inputDecls[i];
        const D3DDDIVERTEXELEMENT *element = triton9FindDeclarationElement(
            declaration, input.Usage, input.UsageIndex);

        if (!element)
            return D3DDDIERR_INVALIDCALL;
        if (element->Stream != 0)
            return D3DDDIERR_NOTAVAILABLE;
    }
    return S_OK;
}

static HRESULT
triton9BindDraw2VertexStreamZero(TRITON9_DEVICE *device,
                                 const Triton9Shader *shader,
                                 const Triton9VertexDeclaration *declaration,
                                 UINT64 firstByte, UINT byteCount)
{
    ID3D11Buffer *buffer = nullptr;
    UINT stride;
    UINT offset;
    HRESULT hr;

    if (!device || !byteCount)
        return E_INVALIDARG;
    hr = triton9RequireDraw2StreamZero(shader, declaration);
    if (FAILED(hr))
        return hr;
    stride = device->streamStrides[0];
    if (!stride)
        return D3DDDIERR_INVALIDCALL;

    if (device->upVertexData[0]) {
        const UINT64 maxPointerOffset = (UINT64)(SIZE_T)-1;

        if (firstByte > maxPointerOffset ||
            byteCount > maxPointerOffset - firstByte)
            return D3DDDIERR_INVALIDCALL;
        hr = triton9UploadUpBuffer(
            device, &device->upVertexBuffers[0],
            &device->upVertexCapacities[0], D3D11_BIND_VERTEX_BUFFER,
            static_cast<const BYTE *>(device->upVertexData[0]) +
                (SIZE_T)firstByte,
            byteCount);
        if (FAILED(hr))
            return hr;
        buffer = device->upVertexBuffers[0];
        offset = 0;
        device->upVertexBytes[0] = byteCount;
    } else {
        UINT64 hostOffset = (UINT64)device->streamOffsets[0] + firstByte;

        if (hostOffset > UINT_MAX)
            return D3DDDIERR_INVALIDCALL;
        hr = triton9GetBuffer(device->streamResources[0],
                              D3D11_BIND_VERTEX_BUFFER, &buffer);
        if (FAILED(hr))
            return hr;
        offset = (UINT)hostOffset;
    }
    device->hostContext->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
    if (!device->upVertexData[0])
        buffer->Release();
    device->drawStreamOffsets[0] = offset;
    device->drawStreamOffsetOverrides[0] = TRUE;
    return S_OK;
}

static void
triton9RestoreTemporaryVertexStreams(TRITON9_DEVICE *device)
{
    if (!device || !device->hostContext)
        return;
    for (UINT stream = 0; stream < TRITON9_MAX_VERTEX_STREAMS; ++stream) {
        ID3D11Buffer *buffer = nullptr;
        UINT stride = 0;
        UINT offset = 0;

        if (!device->drawStreamOffsetOverrides[stream])
            continue;
        if (device->upVertexData[stream]) {
            device->hostContext->IASetVertexBuffers(stream, 1, &buffer, &stride,
                                                     &offset);
            device->upVertexBytes[stream] = 0;
        } else if (device->streamResources[stream] &&
                   SUCCEEDED(triton9GetBuffer(device->streamResources[stream],
                                               D3D11_BIND_VERTEX_BUFFER, &buffer))) {
            stride = device->streamStrides[stream];
            offset = device->streamOffsets[stream];
            device->hostContext->IASetVertexBuffers(stream, 1, &buffer, &stride,
                                                     &offset);
            buffer->Release();
        }
        device->drawStreamOffsets[stream] = 0;
        device->drawStreamOffsetOverrides[stream] = FALSE;
    }
}

static HRESULT
triton9BindUpIndexSlice(TRITON9_DEVICE *device, UINT stride,
                       const VOID *data, UINT64 startOffset,
                       UINT indexCount, UINT *outStartIndex)
{
    ID3D11Buffer *buffer;
    DXGI_FORMAT format;
    UINT64 bytes;
    const UINT64 maxPointerOffset = (UINT64)(SIZE_T)-1;
    HRESULT hr;

    if (!device || !outStartIndex)
        return E_INVALIDARG;
    if (!data || (stride != 2 && stride != 4) || startOffset % stride)
        return D3DDDIERR_INVALIDCALL;
    bytes = (UINT64)indexCount * stride;
    if (!bytes || bytes > UINT_MAX || startOffset > maxPointerOffset ||
        bytes > maxPointerOffset - startOffset)
        return D3DDDIERR_INVALIDCALL;
    hr = triton9UploadUpBuffer(device, &device->upIndexBuffer,
                               &device->upIndexCapacity,
                               D3D11_BIND_INDEX_BUFFER,
                               static_cast<const BYTE *>(data) +
                                   (SIZE_T)startOffset,
                               (UINT)bytes);
    if (FAILED(hr))
        return hr;
    buffer = device->upIndexBuffer;
    format = stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    device->hostContext->IASetIndexBuffer(buffer, format, 0);
    *outStartIndex = 0;
    return S_OK;
}

static void
triton9RestoreIndexBuffer(TRITON9_DEVICE *device)
{
    ID3D11Buffer *buffer = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    if (!device || !device->hostContext)
        return;
    if (device->indexResource &&
        SUCCEEDED(triton9GetBuffer(device->indexResource,
                                   D3D11_BIND_INDEX_BUFFER, &buffer)))
        format = device->indexStride == 2 ? DXGI_FORMAT_R16_UINT
                                          : DXGI_FORMAT_R32_UINT;
    device->hostContext->IASetIndexBuffer(buffer, format, 0);
    if (buffer)
        buffer->Release();
}

} /* namespace */

extern "C" HRESULT APIENTRY
triton9CreateVertexShaderFunc(HANDLE hDevice,
                              D3DDDIARG_CREATEVERTEXSHADERFUNC *args,
                              const UINT *tokens)
{
    HRESULT hr;

    if (!args)
        return E_INVALIDARG;
    triton9DiagU32("TRITON9-CREATE-VS-SIZE", args->Size);
    if (tokens && args->Size >= sizeof(*tokens))
        triton9DiagU32("TRITON9-CREATE-VS-VERSION", tokens[0]);
    hr = triton9CreateShader(static_cast<TRITON9_DEVICE *>(hDevice),
                             Triton9ShaderStage::Vertex, args->Size, tokens,
                             &args->ShaderHandle);
    triton9DiagU32("TRITON9-CREATE-VS-HR", (DWORD)hr);
    return hr;
}

extern "C" HRESULT APIENTRY
triton9DeleteVertexShaderFunc(HANDLE hDevice, HANDLE shader)
{
    return triton9DeleteShader(static_cast<TRITON9_DEVICE *>(hDevice),
                               Triton9ShaderStage::Vertex, shader);
}

extern "C" HRESULT APIENTRY
triton9SetVertexShaderFunc(HANDLE hDevice, HANDLE shader)
{
    HRESULT hr;

    triton9TraceDdiU32("TRITON9-DDI-SET-VS-PID", GetCurrentProcessId());
    hr = triton9SetShader(static_cast<TRITON9_DEVICE *>(hDevice),
                          Triton9ShaderStage::Vertex, shader);
    triton9TraceDdiU32("TRITON9-DDI-SET-VS-HR", (DWORD)hr);
    return hr;
}

static HRESULT
triton9BuildVertexDeclaration(const D3DDDIVERTEXELEMENT *elements,
                              UINT elementCount,
                              Triton9VertexDeclaration **outDeclaration)
{
    Triton9VertexDeclaration *declaration = nullptr;

    if (!elements || !elementCount ||
        elementCount > ShaderConv::MAX_VS_INPUT_REGS || !outDeclaration)
        return E_INVALIDARG;
    *outDeclaration = nullptr;
    try {
        declaration = new (std::nothrow) Triton9VertexDeclaration;
        if (!declaration)
            return E_OUTOFMEMORY;
        declaration->elements.reserve(elementCount);
        for (UINT i = 0; i < elementCount; ++i) {
            D3DDDIVERTEXELEMENT element = elements[i];
            if (element.Stream >= TRITON9_MAX_VERTEX_STREAMS ||
                element.Method != D3DDECLMETHOD_DEFAULT ||
                element.Usage > MAXD3DDECLUSAGE ||
                element.UsageIndex > MAXD3DDECLUSAGEINDEX ||
                triton9DeclarationFormat(element.Type) == DXGI_FORMAT_UNKNOWN) {
                delete declaration;
                return D3DDDIERR_NOTAVAILABLE;
            }
            BOOL transformedPosition = FALSE;
            if (element.Usage == D3DDECLUSAGE_POSITIONT && element.UsageIndex == 0) {
                element.Usage = D3DDECLUSAGE_POSITION;
                transformedPosition = TRUE;
                declaration->hasTransformedPosition = TRUE;
            }
            for (const D3DDDIVERTEXELEMENT &existing : declaration->elements) {
                if (existing.Usage == element.Usage &&
                    existing.UsageIndex == element.UsageIndex) {
                    delete declaration;
                    return D3DDDIERR_INVALIDCALL;
                }
            }
            declaration->inputDecls.AddDecl(element.Usage, element.UsageIndex, i,
                                            transformedPosition,
                                            triton9DeclarationConversion(element.Type));
            declaration->elements.push_back(element);
        }
    } catch (...) {
        delete declaration;
        return E_OUTOFMEMORY;
    }
    declaration->serial = (UINT64)(ULONG)InterlockedIncrement(&gTriton9DeclarationSerial);
    if (!declaration->serial)
        declaration->serial = 1;
    *outDeclaration = declaration;
    return S_OK;
}

static UINT
triton9FvfTypeSize(UCHAR type)
{
    return triton9DeclarationTypeSize(type);
}

static HRESULT
triton9AppendFvfElement(D3DDDIVERTEXELEMENT *elements, UINT *elementCount,
                        UINT *offset, UCHAR type, UCHAR usage, UCHAR usageIndex)
{
    UINT size;

    if (!elements || !elementCount || !offset ||
        *elementCount >= ShaderConv::MAX_VS_INPUT_REGS || *offset > USHRT_MAX)
        return D3DDDIERR_NOTAVAILABLE;
    size = triton9FvfTypeSize(type);
    if (!size || size > USHRT_MAX - *offset)
        return D3DDDIERR_NOTAVAILABLE;
    D3DDDIVERTEXELEMENT &element = elements[*elementCount];
    element.Stream = 0;
    element.Offset = (USHORT)*offset;
    element.Type = type;
    element.Method = D3DDECLMETHOD_DEFAULT;
    element.Usage = usage;
    element.UsageIndex = usageIndex;
    *offset += size;
    ++*elementCount;
    return S_OK;
}

/* Create the declaration that the Vista runtime normally synthesizes from an
 * FVF.  Keep it with the vertex buffer as a fallback for callers that leave
 * the DDI declaration null. */
static HRESULT
triton9BuildFvfDeclaration(UINT fvf, Triton9VertexDeclaration **outDeclaration)
{
    D3DDDIVERTEXELEMENT elements[16] = {};
    const UINT position = fvf & D3DFVF_POSITION_MASK;
    const UINT textureCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    const UINT textureFormatMask = 0xffff0000u;
    const UINT allowed = D3DFVF_POSITION_MASK | D3DFVF_NORMAL | D3DFVF_PSIZE |
                         D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEXCOUNT_MASK |
                         D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR |
                         textureFormatMask;
    UINT elementCount = 0;
    UINT offset = 0;
    UINT blendWeights = 0;
    HRESULT hr;

    if (!outDeclaration || !position || textureCount > 8 || (fvf & ~allowed) ||
        ((fvf & D3DFVF_LASTBETA_UBYTE4) &&
         (fvf & D3DFVF_LASTBETA_D3DCOLOR)))
        return D3DDDIERR_INVALIDCALL;
    if ((fvf & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR)) &&
        position != D3DFVF_XYZB5)
        return D3DDDIERR_INVALIDCALL;
    if (textureCount < 8) {
        const UINT activeTextureFormatMask =
            ((1u << (textureCount * 2u)) - 1u) << 16;
        if (fvf & (textureFormatMask & ~activeTextureFormatMask))
            return D3DDDIERR_INVALIDCALL;
    }

    switch (position) {
    case D3DFVF_XYZ:
        hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                     D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0);
        break;
    case D3DFVF_XYZW:
        hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                     D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0);
        break;
    case D3DFVF_XYZRHW:
        if (fvf & (D3DFVF_NORMAL | D3DFVF_LASTBETA_UBYTE4 |
                   D3DFVF_LASTBETA_D3DCOLOR))
            return D3DDDIERR_INVALIDCALL;
        hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                     D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0);
        break;
    case D3DFVF_XYZB1:
    case D3DFVF_XYZB2:
    case D3DFVF_XYZB3:
    case D3DFVF_XYZB4:
    case D3DFVF_XYZB5:
        hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                     D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0);
        blendWeights = (position - D3DFVF_XYZB1) / 2u + 1u;
        if (fvf & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR)) {
            if (position != D3DFVF_XYZB5)
                return D3DDDIERR_INVALIDCALL;
            --blendWeights;
        }
        if (SUCCEEDED(hr) && blendWeights <= 4)
            hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                         (UCHAR)(D3DDECLTYPE_FLOAT1 + blendWeights - 1u),
                                         D3DDECLUSAGE_BLENDWEIGHT, 0);
        if (SUCCEEDED(hr) && blendWeights == 5) {
            hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                         D3DDECLTYPE_FLOAT4,
                                         D3DDECLUSAGE_BLENDWEIGHT, 0);
            if (SUCCEEDED(hr))
                hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                             D3DDECLTYPE_FLOAT1,
                                             D3DDECLUSAGE_BLENDWEIGHT, 1);
        }
        if (SUCCEEDED(hr) && (fvf & D3DFVF_LASTBETA_UBYTE4))
            hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                         D3DDECLTYPE_UBYTE4,
                                         D3DDECLUSAGE_BLENDINDICES, 0);
        if (SUCCEEDED(hr) && (fvf & D3DFVF_LASTBETA_D3DCOLOR))
            hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                         D3DDECLTYPE_D3DCOLOR,
                                         D3DDECLUSAGE_BLENDINDICES, 0);
        break;
    default:
        return D3DDDIERR_INVALIDCALL;
    }
    if (FAILED(hr))
        return hr;
    if (fvf & D3DFVF_NORMAL) {
        hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                     D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0);
        if (FAILED(hr))
            return hr;
    }
    if (fvf & D3DFVF_PSIZE) {
        hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                     D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0);
        if (FAILED(hr))
            return hr;
    }
    if (fvf & D3DFVF_DIFFUSE) {
        hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                     D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0);
        if (FAILED(hr))
            return hr;
    }
    if (fvf & D3DFVF_SPECULAR) {
        hr = triton9AppendFvfElement(elements, &elementCount, &offset,
                                     D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1);
        if (FAILED(hr))
            return hr;
    }
    for (UINT texture = 0; texture < textureCount; ++texture) {
        const UINT dimensions = (fvf >> (16u + texture * 2u)) & 3u;
        const UCHAR type = dimensions == D3DFVF_TEXTUREFORMAT1
            ? D3DDECLTYPE_FLOAT1 : dimensions == D3DFVF_TEXTUREFORMAT3
            ? D3DDECLTYPE_FLOAT3 : dimensions == D3DFVF_TEXTUREFORMAT4
            ? D3DDECLTYPE_FLOAT4 : D3DDECLTYPE_FLOAT2;
        hr = triton9AppendFvfElement(elements, &elementCount, &offset, type,
                                     D3DDECLUSAGE_TEXCOORD, (UCHAR)texture);
        if (FAILED(hr))
            return hr;
    }
    return triton9BuildVertexDeclaration(elements, elementCount, outDeclaration);
}

extern "C" HRESULT APIENTRY
triton9CreateVertexShaderDecl(HANDLE hDevice,
                              D3DDDIARG_CREATEVERTEXSHADERDECL *args,
                              const D3DDDIVERTEXELEMENT *elements)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    Triton9VertexDeclaration *declaration;

    triton9TraceDdiU32("TRITON9-DDI-DECL-PID", GetCurrentProcessId());
    if (!device || !args || !elements || !args->NumVertexElements ||
        args->NumVertexElements > ShaderConv::MAX_VS_INPUT_REGS)
        return E_INVALIDARG;
    args->ShaderHandle = nullptr;
    HRESULT hr = triton9BuildVertexDeclaration(elements, args->NumVertexElements,
                                                &declaration);
    if (FAILED(hr))
        return hr;
    args->ShaderHandle = static_cast<HANDLE>(declaration);
    triton9TraceDdiU32("TRITON9-DDI-DECL-OK", args->NumVertexElements);
    return S_OK;
}

extern "C" HRESULT
triton9CreateFvfDeclaration(TRITON9_DEVICE *device, UINT fvf,
                            void **outDeclaration)
{
    Triton9VertexDeclaration *declaration;
    HRESULT hr;

    if (!device || !outDeclaration)
        return E_INVALIDARG;
    *outDeclaration = nullptr;
    hr = triton9BuildFvfDeclaration(fvf, &declaration);
    if (FAILED(hr))
        return hr;
    *outDeclaration = declaration;
    return S_OK;
}

extern "C" void
triton9DestroyFvfDeclaration(TRITON9_DEVICE *device, void *handle)
{
    Triton9VertexDeclaration *declaration =
        static_cast<Triton9VertexDeclaration *>(handle);

    if (!declaration)
        return;
    if (device && device->shaderLockInitialized) {
        EnterCriticalSection(&device->shaderLock);
        if (device->activeVertexDeclaration == declaration) {
            if (device->hostContext)
                device->hostContext->IASetInputLayout(nullptr);
            device->activeVertexDeclaration = nullptr;
        }
        if (device->drawVertexDeclaration == declaration)
            device->drawVertexDeclaration = nullptr;
        if (device->fixedFunctionShaders) {
            Triton9FixedFunctionShaders *shaders =
                static_cast<Triton9FixedFunctionShaders *>(device->fixedFunctionShaders);
            if (shaders->vertexDeclaration == declaration)
                triton9ReleaseFixedFunctionShaders(device);
        }
        LeaveCriticalSection(&device->shaderLock);
    }
    delete declaration;
}

extern "C" HRESULT APIENTRY
triton9DeleteVertexShaderDecl(HANDLE hDevice, HANDLE handle)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    Triton9VertexDeclaration *declaration =
        static_cast<Triton9VertexDeclaration *>(handle);

    if (!device || !declaration || !device->shaderLockInitialized)
        return E_INVALIDARG;
    EnterCriticalSection(&device->shaderLock);
    if (device->activeVertexDeclaration == declaration) {
        if (device->hostContext)
            device->hostContext->IASetInputLayout(nullptr);
        device->activeVertexDeclaration = nullptr;
    }
    if (device->drawVertexDeclaration == declaration)
        device->drawVertexDeclaration = nullptr;
    if (device->fixedFunctionShaders) {
        Triton9FixedFunctionShaders *shaders =
            static_cast<Triton9FixedFunctionShaders *>(device->fixedFunctionShaders);
        if (shaders->vertexDeclaration == declaration)
            triton9ReleaseFixedFunctionShaders(device);
    }
    LeaveCriticalSection(&device->shaderLock);
    delete declaration;
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9SetVertexShaderDecl(HANDLE hDevice, HANDLE handle)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    Triton9VertexDeclaration *declaration =
        static_cast<Triton9VertexDeclaration *>(handle);

    triton9TraceDdiU32("TRITON9-DDI-SET-DECL-PID", GetCurrentProcessId());
    if (!device || !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    EnterCriticalSection(&device->shaderLock);
    /* See triton9SetShader(): declaration/shader setters are allowed to
     * describe an intermediate, intentionally incompatible state. */
    device->activeVertexDeclaration = declaration;
    device->drawVertexDeclaration = nullptr;
    LeaveCriticalSection(&device->shaderLock);
    triton9TraceDdiU32("TRITON9-DDI-SET-DECL-HR", S_OK);
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9SetStreamSource(HANDLE hDevice, const D3DDDIARG_SETSTREAMSOURCE *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    TRITON9_RESOURCE *resource;
    ID3D11Buffer *buffer = nullptr;
    UINT stride;
    UINT offset;
    HRESULT hr;

    if (!device || !args || args->Stream >= TRITON9_MAX_VERTEX_STREAMS)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    resource = static_cast<TRITON9_RESOURCE *>(args->hVertexBuffer);
    if (resource) {
        if (!triton9ResourceBelongsToDevice(device, resource))
            return D3DDDIERR_INVALIDCALL;
        if (!args->Stride || args->Offset >= resource->width)
            return D3DDDIERR_INVALIDCALL;
        hr = triton9EnsureResourceHost(device, resource);
        if (FAILED(hr))
            return hr;
        hr = triton9GetBuffer(resource, D3D11_BIND_VERTEX_BUFFER, &buffer);
        if (FAILED(hr))
            return hr;
    }
    stride = resource ? args->Stride : 0;
    offset = resource ? args->Offset : 0;
    EnterCriticalSection(&device->shaderLock);
    if (device->hostContext)
        device->hostContext->IASetVertexBuffers(args->Stream, 1, &buffer, &stride,
                                                 &offset);
    device->streamResources[args->Stream] = resource;
    device->streamOffsets[args->Stream] = offset;
    device->streamStrides[args->Stream] = stride;
    device->upVertexData[args->Stream] = nullptr;
    device->upVertexBytes[args->Stream] = 0;
    device->drawStreamOffsetOverrides[args->Stream] = FALSE;
    LeaveCriticalSection(&device->shaderLock);
    if (buffer)
        buffer->Release();
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9SetStreamSourceUm(HANDLE hDevice,
                         const D3DDDIARG_SETSTREAMSOURCEUM *args,
                         const VOID *data)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    ID3D11Buffer *buffer = nullptr;
    UINT stride = 0;
    UINT offset = 0;

    triton9TraceDdiU32("TRITON9-DDI-SET-UP-PID", GetCurrentProcessId());
    if (!device || !args || args->Stream >= TRITON9_MAX_VERTEX_STREAMS)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (data && (!args->Stride || (args->Stride & 3u) ||
                 ((ULONG_PTR)data & 3u)))
        return D3DDDIERR_INVALIDCALL;

    EnterCriticalSection(&device->shaderLock);
    if (data) {
        for (UINT stream = 0; stream < TRITON9_MAX_VERTEX_STREAMS; ++stream) {
            if (stream != args->Stream && device->upVertexData[stream]) {
                LeaveCriticalSection(&device->shaderLock);
                return D3DDDIERR_INVALIDCALL;
            }
        }
    }
    if (data) {
        stride = args->Stride;
        device->upVertexData[args->Stream] = data;
        device->upVertexBytes[args->Stream] = 0;
    } else {
        device->upVertexData[args->Stream] = nullptr;
        device->upVertexBytes[args->Stream] = 0;
    }
    device->streamResources[args->Stream] = nullptr;
    device->streamOffsets[args->Stream] = 0;
    device->streamStrides[args->Stream] = stride;
    device->drawStreamOffsets[args->Stream] = 0;
    device->drawStreamOffsetOverrides[args->Stream] = FALSE;
    if (device->hostContext)
        device->hostContext->IASetVertexBuffers(args->Stream, 1, &buffer, &stride,
                                                 &offset);
    LeaveCriticalSection(&device->shaderLock);
    triton9TraceDdiU32("TRITON9-DDI-SET-UP-STRIDE", stride);
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9SetStreamSourceFreq(HANDLE hDevice,
                           const D3DDDIARG_SETSTREAMSOURCEFREQ *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    const UINT flagsMask = D3DSTREAMSOURCE_INDEXEDDATA |
                           D3DSTREAMSOURCE_INSTANCEDATA;
    UINT frequency;
    UINT flags;

    if (!device || !args || args->Stream >= TRITON9_MAX_VERTEX_STREAMS ||
        !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    frequency = args->Divider;
    flags = frequency & flagsMask;
    if (!frequency || flags == flagsMask ||
        (args->Stream == 0 && (flags & D3DSTREAMSOURCE_INSTANCEDATA)))
        return D3DDDIERR_INVALIDCALL;

    EnterCriticalSection(&device->shaderLock);
    if (device->streamSourceFrequencies[args->Stream] != frequency) {
        device->streamSourceFrequencies[args->Stream] = frequency;
        ++device->streamFrequencyGeneration;
        if (!device->streamFrequencyGeneration)
            ++device->streamFrequencyGeneration;
        /* The old layout has different input-slot classifications.  It will
         * be rebuilt before the next draw, so do not leave it bound. */
        if (device->hostContext)
            device->hostContext->IASetInputLayout(nullptr);
    }
    LeaveCriticalSection(&device->shaderLock);
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9SetIndices(HANDLE hDevice, const D3DDDIARG_SETINDICES *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    TRITON9_RESOURCE *resource;
    ID3D11Buffer *buffer = nullptr;
    DXGI_FORMAT format;
    HRESULT hr;

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    resource = static_cast<TRITON9_RESOURCE *>(args->hIndexBuffer);
    if (resource) {
        if (!triton9ResourceBelongsToDevice(device, resource))
            return D3DDDIERR_INVALIDCALL;
        if ((args->Stride != 2 && args->Stride != 4) ||
            (args->Stride == 2 && resource->format != D3DDDIFMT_INDEX16) ||
            (args->Stride == 4 && resource->format != D3DDDIFMT_INDEX32))
            return D3DDDIERR_INVALIDCALL;
        hr = triton9EnsureResourceHost(device, resource);
        if (FAILED(hr))
            return hr;
        hr = triton9GetBuffer(resource, D3D11_BIND_INDEX_BUFFER, &buffer);
        if (FAILED(hr))
            return hr;
        format = args->Stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    } else {
        format = DXGI_FORMAT_UNKNOWN;
    }
    EnterCriticalSection(&device->shaderLock);
    if (device->hostContext)
        device->hostContext->IASetIndexBuffer(buffer, format, 0);
    device->indexResource = resource;
    device->indexStride = resource ? args->Stride : 0;
    device->upIndexData = nullptr;
    device->upIndexStride = 0;
    LeaveCriticalSection(&device->shaderLock);
    if (buffer)
        buffer->Release();
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9SetIndicesUm(HANDLE hDevice, UINT stride, const VOID *data)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);

    if (!device)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (data && stride != 2 && stride != 4)
        return D3DDDIERR_INVALIDCALL;

    EnterCriticalSection(&device->shaderLock);
    if (device->hostContext)
        device->hostContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    device->indexResource = nullptr;
    device->indexStride = 0;
    device->upIndexData = data;
    device->upIndexStride = data ? stride : 0;
    LeaveCriticalSection(&device->shaderLock);
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9SetVertexShaderConst(HANDLE hDevice,
                            const D3DDDIARG_SETVERTEXSHADERCONST *args,
                            const VOID *data)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    return triton9SetConstants(device, TRUE, 0, args->Register, args->Count,
                               data, 256, 4 * sizeof(FLOAT));
}

extern "C" HRESULT APIENTRY
triton9SetVertexShaderConstI(HANDLE hDevice,
                             const D3DDDIARG_SETVERTEXSHADERCONSTI *args,
                             const INT *data)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    return triton9SetConstants(device, TRUE, 1, args->Register, args->Count,
                               data, 16, 4 * sizeof(INT));
}

extern "C" HRESULT APIENTRY
triton9SetVertexShaderConstB(HANDLE hDevice,
                             const D3DDDIARG_SETVERTEXSHADERCONSTB *args,
                             const BOOL *data)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    return triton9SetConstants(device, TRUE, 2, args->Register, args->Count,
                               data, 16, sizeof(BOOL));
}

extern "C" HRESULT APIENTRY
triton9SetPixelShaderConst(HANDLE hDevice,
                           const D3DDDIARG_SETPIXELSHADERCONST *args,
                           const FLOAT *data)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    return triton9SetConstants(device, FALSE, 0, args->Register, args->Count,
                               data, 224, 4 * sizeof(FLOAT));
}

extern "C" HRESULT APIENTRY
triton9SetPixelShaderConstI(HANDLE hDevice,
                            const D3DDDIARG_SETPIXELSHADERCONSTI *args,
                            const INT *data)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    return triton9SetConstants(device, FALSE, 1, args->Register, args->Count,
                               data, 16, 4 * sizeof(INT));
}

extern "C" HRESULT APIENTRY
triton9SetPixelShaderConstB(HANDLE hDevice,
                            const D3DDDIARG_SETPIXELSHADERCONSTB *args,
                            const BOOL *data)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    return triton9SetConstants(device, FALSE, 2, args->Register, args->Count,
                               data, 16, sizeof(BOOL));
}

static void
triton9DrawResourcesWritten(TRITON9_DEVICE *device)
{
    for (UINT i = 0; i < TRITON9_MAX_RENDER_TARGETS; ++i)
        triton9ResourceWritten(device->renderTargets[i]);
    triton9ResourceWritten(device->depthStencil);
}

extern "C" HRESULT APIENTRY
triton9DrawPrimitive(HANDLE hDevice, const D3DDDIARG_DRAWPRIMITIVE *args,
                     const UINT *flags)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    Triton9Shader *vertexShader;
    Triton9VertexDeclaration *declaration;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertexCount;
    UINT drawCount;
    const UINT instanceCount = 1;
    UINT startIndex = 0;
    int expand;
    std::vector<UINT> expandedIndices;
    HRESULT hr;

    triton9TraceDdiU32("TRITON9-DDI-DRAW-P-PID", GetCurrentProcessId());
    if (args) {
        triton9TraceDdiU32("TRITON9-DDI-DRAW-P-TYPE", args->PrimitiveType);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-P-COUNT", args->PrimitiveCount);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-P-START", args->VStart);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-P-HASFLAGS", flags != nullptr);
    }
    if (!device || !args || !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (!args->PrimitiveCount)
        return S_OK;
    if (flags) {
        triton9TraceDdi("TRITON9-DDI-DRAW-P-FLAGS\n");
        return D3DDDIERR_NOTAVAILABLE;
    }
    hr = triton9PrimitiveTopology(args->PrimitiveType, args->PrimitiveCount,
                                  &topology, &vertexCount);
    if (FAILED(hr))
        return hr;
    if (!triton9_draw_primitive_counts(args->PrimitiveType,
                                       args->PrimitiveCount, &vertexCount,
                                       &drawCount, &expand))
        return D3DDDIERR_INVALIDCALL;
    if (expand) {
        try {
            expandedIndices.resize(drawCount);
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        if (!triton9_draw_expand_fan(nullptr, 0, 0, args->PrimitiveCount,
                                     expandedIndices.data(), drawCount))
            return D3DDDIERR_INVALIDCALL;
    }
    EnterCriticalSection(&device->shaderLock);
    hr = triton9PrepareDraw(device, args->PrimitiveType);
    vertexShader = static_cast<Triton9Shader *>(device->drawVertexShader);
    declaration = static_cast<Triton9VertexDeclaration *>(
        device->drawVertexDeclaration);
    /* INDEXEDDATA repeats indexed draws only.  Instance streams still supply
     * their first element when DrawPrimitive follows an instanced draw. */
    if (SUCCEEDED(hr))
        hr = triton9BindUpVertexStreams(device, vertexShader, declaration,
                                        args->VStart, vertexCount,
                                        instanceCount);
    if (SUCCEEDED(hr))
        hr = triton9ValidateStreamRange(device, vertexShader, declaration,
                                        args->VStart, vertexCount, instanceCount);
    if (SUCCEEDED(hr) && expand)
        hr = triton9BindUpIndexSlice(device, sizeof(UINT),
                                     expandedIndices.data(), 0, drawCount,
                                     &startIndex);
    if (SUCCEEDED(hr)) {
        device->hostContext->IASetPrimitiveTopology(topology);
        if (expand) {
            device->hostContext->DrawIndexed(drawCount, startIndex,
                                             args->VStart);
        } else {
            device->hostContext->Draw(vertexCount, args->VStart);
        }
    }
    triton9RestoreTemporaryVertexStreams(device);
    if (expand)
        triton9RestoreIndexBuffer(device);
    if (SUCCEEDED(hr))
        hr = triton9CheckHostDevice(device);
    if (SUCCEEDED(hr))
        triton9DrawResourcesWritten(device);
    if (FAILED(hr))
        triton9DiagU32("TRITON9-DRAW-PRIMITIVE-HR", (DWORD)hr);
    LeaveCriticalSection(&device->shaderLock);
    triton9TraceDdiU32("TRITON9-DDI-DRAW-P-HR", (DWORD)hr);
    return triton9MapDeviceFailure(device, hr);
}

extern "C" HRESULT APIENTRY
triton9DrawIndexedPrimitive(HANDLE hDevice,
                            const D3DDDIARG_DRAWINDEXEDPRIMITIVE *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    Triton9Shader *vertexShader;
    Triton9VertexDeclaration *declaration;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT indexCount;
    UINT drawCount;
    UINT instanceCount;
    UINT firstVertex;
    UINT drawStartIndex;
    UINT64 indexEnd = 0;
    const BYTE *sourceIndices = nullptr;
    UINT sourceIndexStride = 0;
    int expand;
    std::vector<UINT> expandedIndices;
    BOOL temporaryIndex = FALSE;
    HRESULT hr;

    if (args) {
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP-TYPE", args->PrimitiveType);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP-COUNT", args->PrimitiveCount);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP-START", args->StartIndex);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP-BASE",
                           (DWORD)args->BaseVertexIndex);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP-MIN", args->MinIndex);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP-NUM", args->NumVertices);
    }
    if (!device || !args || !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (!args->PrimitiveCount)
        return S_OK;
    hr = triton9PrimitiveTopology(args->PrimitiveType, args->PrimitiveCount,
                                  &topology, &indexCount);
    if (FAILED(hr))
        return hr;
    if (!triton9_draw_primitive_counts(args->PrimitiveType,
                                       args->PrimitiveCount, &indexCount,
                                       &drawCount, &expand))
        return D3DDDIERR_INVALIDCALL;
    if (!triton9_draw_indexed_vertex_range(args->BaseVertexIndex,
                                           args->MinIndex, args->NumVertices,
                                           &firstVertex))
        return D3DDDIERR_INVALIDCALL;
    EnterCriticalSection(&device->shaderLock);
    drawStartIndex = args->StartIndex;
    if (device->indexResource && device->indexStride &&
        args->StartIndex <= UINT64_MAX / device->indexStride &&
        indexCount <= UINT64_MAX / device->indexStride - args->StartIndex) {
        indexEnd = ((UINT64)args->StartIndex + indexCount) * device->indexStride;
        if (indexEnd > device->indexResource->width ||
            indexEnd > device->indexResource->shadowSize ||
            !device->indexResource->shadow) {
            hr = D3DDDIERR_INVALIDCALL;
        } else {
            sourceIndices = device->indexResource->shadow +
                (SIZE_T)args->StartIndex * device->indexStride;
            sourceIndexStride = device->indexStride;
            hr = S_OK;
        }
    } else if (device->upIndexData &&
               (device->upIndexStride == 2 || device->upIndexStride == 4)) {
        const UINT64 offset = (UINT64)args->StartIndex *
                              device->upIndexStride;

        if (offset > (UINT64)(SIZE_T)-1) {
            hr = D3DDDIERR_INVALIDCALL;
        } else {
            hr = S_OK;
            temporaryIndex = TRUE;
            sourceIndices = static_cast<const BYTE *>(device->upIndexData) +
                            (SIZE_T)offset;
            sourceIndexStride = device->upIndexStride;
        }
    } else {
        hr = D3DDDIERR_INVALIDCALL;
    }
    if (SUCCEEDED(hr))
        hr = triton9PrepareDraw(device, args->PrimitiveType);
    if (SUCCEEDED(hr) && !triton9_draw_validate_indices(
            sourceIndices, sourceIndexStride, indexCount, args->MinIndex,
            args->NumVertices))
        hr = D3DDDIERR_INVALIDCALL;
    if (SUCCEEDED(hr) && expand) {
        try {
            expandedIndices.resize(drawCount);
        } catch (...) {
            hr = E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr) && !triton9_draw_expand_fan(
                sourceIndices, sourceIndexStride, 0, args->PrimitiveCount,
                expandedIndices.data(), drawCount))
            hr = D3DDDIERR_INVALIDCALL;
    }
    vertexShader = static_cast<Triton9Shader *>(device->drawVertexShader);
    declaration = static_cast<Triton9VertexDeclaration *>(
        device->drawVertexDeclaration);
    if (SUCCEEDED(hr))
        hr = triton9GetInstanceCount(device, vertexShader, declaration,
                                     &instanceCount);
    if (SUCCEEDED(hr))
        hr = triton9BindUpVertexStreams(device, vertexShader, declaration,
                                        firstVertex, args->NumVertices,
                                        instanceCount);
    if (SUCCEEDED(hr))
        hr = triton9ValidateStreamRange(device, vertexShader, declaration,
                                        firstVertex, args->NumVertices,
                                        instanceCount);
    if (SUCCEEDED(hr) && expand) {
        hr = triton9BindUpIndexSlice(device, sizeof(UINT),
                                     expandedIndices.data(), 0, drawCount,
                                     &drawStartIndex);
        temporaryIndex = SUCCEEDED(hr);
    } else if (SUCCEEDED(hr) && temporaryIndex) {
        hr = triton9BindUpIndexSlice(
            device, device->upIndexStride, device->upIndexData,
            (UINT64)args->StartIndex * device->upIndexStride, indexCount,
            &drawStartIndex);
    }
    if (SUCCEEDED(hr) && !temporaryIndex)
        hr = triton9PrepareBufferRangeForHostRead(
            device, device->indexResource,
            (UINT)((UINT64)args->StartIndex * device->indexStride),
            (UINT)indexEnd);
    if (SUCCEEDED(hr)) {
        device->hostContext->IASetPrimitiveTopology(topology);
        if (instanceCount == 1)
            device->hostContext->DrawIndexed(expand ? drawCount : indexCount,
                                             drawStartIndex,
                                             args->BaseVertexIndex);
        else
            device->hostContext->DrawIndexedInstanced(
                expand ? drawCount : indexCount, instanceCount, drawStartIndex,
                args->BaseVertexIndex, 0);
    }
    triton9RestoreTemporaryVertexStreams(device);
    if (temporaryIndex)
        triton9RestoreIndexBuffer(device);
    if (SUCCEEDED(hr))
        hr = triton9CheckHostDevice(device);
    if (SUCCEEDED(hr))
        triton9DrawResourcesWritten(device);
    if (FAILED(hr))
        triton9DiagU32("TRITON9-DRAW-INDEXED-HR", (DWORD)hr);
    LeaveCriticalSection(&device->shaderLock);
    triton9TraceDdiU32("TRITON9-DDI-DRAW-IP-HR", (DWORD)hr);
    return triton9MapDeviceFailure(device, hr);
}

extern "C" HRESULT APIENTRY
triton9DrawPrimitive2(HANDLE hDevice, const D3DDDIARG_DRAWPRIMITIVE2 *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    Triton9Shader *vertexShader;
    Triton9VertexDeclaration *declaration;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertexCount;
    UINT drawCount;
    UINT instanceCount;
    UINT vertexBytes;
    UINT startIndex = 0;
    int expand;
    std::vector<UINT> expandedIndices;
    HRESULT hr;

    triton9TraceDdiU32("TRITON9-DDI-DRAW-P2-PID", GetCurrentProcessId());
    if (args) {
        triton9TraceDdiU32("TRITON9-DDI-DRAW-P2-TYPE", args->PrimitiveType);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-P2-COUNT", args->PrimitiveCount);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-P2-OFFSET",
                           args->FirstVertexOffset);
    }
    if (!device || !args || !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (!args->PrimitiveCount)
        return S_OK;
    hr = triton9PrimitiveTopology(args->PrimitiveType, args->PrimitiveCount,
                                  &topology, &vertexCount);
    if (FAILED(hr))
        return hr;
    if (!triton9_draw_primitive_counts(args->PrimitiveType,
                                       args->PrimitiveCount, &vertexCount,
                                       &drawCount, &expand))
        return D3DDDIERR_INVALIDCALL;
    if (expand) {
        try {
            expandedIndices.resize(drawCount);
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        if (!triton9_draw_expand_fan(nullptr, 0, 0, args->PrimitiveCount,
                                     expandedIndices.data(), drawCount))
            return D3DDDIERR_INVALIDCALL;
    }

    EnterCriticalSection(&device->shaderLock);
    hr = triton9PrepareDraw(device, args->PrimitiveType);
    vertexShader = static_cast<Triton9Shader *>(device->drawVertexShader);
    declaration = static_cast<Triton9VertexDeclaration *>(
        device->drawVertexDeclaration);
    if (SUCCEEDED(hr))
        hr = triton9GetInstanceCount(device, vertexShader, declaration,
                                     &instanceCount);
    if (SUCCEEDED(hr) && instanceCount != 1)
        hr = D3DDDIERR_NOTAVAILABLE;
    if (SUCCEEDED(hr) &&
        !triton9_draw_um_prefix_bytes(0, vertexCount,
                                      device->streamStrides[0], &vertexBytes))
        hr = D3DDDIERR_INVALIDCALL;
    if (SUCCEEDED(hr))
        hr = triton9BindDraw2VertexStreamZero(
            device, vertexShader, declaration, args->FirstVertexOffset,
            vertexBytes);
    if (SUCCEEDED(hr))
        hr = triton9ValidateStreamRange(device, vertexShader, declaration, 0,
                                        vertexCount, 1);
    if (SUCCEEDED(hr) && expand)
        hr = triton9BindUpIndexSlice(device, sizeof(UINT),
                                     expandedIndices.data(), 0, drawCount,
                                     &startIndex);
    if (SUCCEEDED(hr)) {
        device->hostContext->IASetPrimitiveTopology(topology);
        if (expand)
            device->hostContext->DrawIndexed(drawCount, startIndex, 0);
        else
            device->hostContext->Draw(vertexCount, 0);
    }
    triton9RestoreTemporaryVertexStreams(device);
    if (expand)
        triton9RestoreIndexBuffer(device);
    if (SUCCEEDED(hr))
        hr = triton9CheckHostDevice(device);
    if (SUCCEEDED(hr))
        triton9DrawResourcesWritten(device);
    if (FAILED(hr))
        triton9DiagU32("TRITON9-DRAW-PRIMITIVE2-HR", (DWORD)hr);
    LeaveCriticalSection(&device->shaderLock);
    triton9TraceDdiU32("TRITON9-DDI-DRAW-P2-HR", (DWORD)hr);
    return triton9MapDeviceFailure(device, hr);
}

extern "C" HRESULT APIENTRY
triton9DrawIndexedPrimitive2(HANDLE hDevice,
                             const D3DDDIARG_DRAWINDEXEDPRIMITIVE2 *args,
                             UINT stride, const VOID *data,
                             const UINT *flags)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    Triton9Shader *vertexShader;
    Triton9VertexDeclaration *declaration;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT indexCount;
    UINT drawCount;
    UINT instanceCount;
    UINT startIndex = 0;
    UINT vertexBytes;
    UINT indexBytes;
    UINT indexStride;
    UINT64 firstVertexByte;
    const VOID *indexData;
    std::vector<BYTE> normalizedIndices;
    std::vector<UINT> expandedIndices;
    int expand;
    HRESULT hr;

    if (args) {
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP2-TYPE", args->PrimitiveType);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP2-COUNT", args->PrimitiveCount);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP2-START",
                           args->StartIndexOffset);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP2-BASE",
                           (DWORD)args->BaseVertexOffset);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP2-MIN", args->MinIndex);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP2-NUM", args->NumVertices);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP2-STRIDE", stride);
        triton9TraceDdiU32("TRITON9-DDI-DRAW-IP2-HASFLAGS", flags != nullptr);
    }
    if (!device || !args || !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (!args->PrimitiveCount)
        return S_OK;
    if (flags)
        return D3DDDIERR_NOTAVAILABLE;
    if (!args->NumVertices)
        return D3DDDIERR_INVALIDCALL;
    hr = triton9PrimitiveTopology(args->PrimitiveType, args->PrimitiveCount,
                                  &topology, &indexCount);
    if (FAILED(hr))
        return hr;
    if (!triton9_draw_primitive_counts(args->PrimitiveType,
                                       args->PrimitiveCount, &indexCount,
                                       &drawCount, &expand))
        return D3DDDIERR_INVALIDCALL;

    EnterCriticalSection(&device->shaderLock);
    hr = triton9PrepareDraw(device, args->PrimitiveType);
    vertexShader = static_cast<Triton9Shader *>(device->drawVertexShader);
    declaration = static_cast<Triton9VertexDeclaration *>(
        device->drawVertexDeclaration);
    if (SUCCEEDED(hr))
        hr = triton9GetInstanceCount(device, vertexShader, declaration,
                                     &instanceCount);
    if (SUCCEEDED(hr) && instanceCount != 1)
        hr = D3DDDIERR_NOTAVAILABLE;
    if (SUCCEEDED(hr))
        hr = triton9RequireDraw2StreamZero(vertexShader, declaration);
    if (SUCCEEDED(hr) &&
        !triton9_draw2_vertex_window(args->BaseVertexOffset, args->MinIndex,
                                     args->NumVertices,
                                     device->streamStrides[0],
                                     &firstVertexByte, &vertexBytes))
        hr = D3DDDIERR_INVALIDCALL;
    indexData = data ? data : device->upIndexData;
    indexStride = data ? stride : device->upIndexStride;
    indexBytes = 0;
    if (SUCCEEDED(hr) &&
        (indexStride != 2 && indexStride != 4))
        hr = D3DDDIERR_INVALIDCALL;
    if (SUCCEEDED(hr) &&
        ((UINT64)indexCount * indexStride > UINT_MAX ||
         args->StartIndexOffset % indexStride))
        hr = D3DDDIERR_INVALIDCALL;
    if (SUCCEEDED(hr)) {
        const UINT64 maxPointerOffset = (UINT64)(SIZE_T)-1;

        indexBytes = indexCount * indexStride;
        if (!indexData || args->StartIndexOffset > maxPointerOffset ||
            indexBytes > maxPointerOffset - args->StartIndexOffset) {
            hr = D3DDDIERR_INVALIDCALL;
        } else {
            try {
                normalizedIndices.resize(indexBytes);
            } catch (...) {
                hr = E_OUTOFMEMORY;
            }
        }
    }
    if (SUCCEEDED(hr) && !triton9_draw2_normalize_indices(
            static_cast<const BYTE *>(indexData) + args->StartIndexOffset,
            indexStride, indexCount, args->MinIndex, args->NumVertices,
            normalizedIndices.data()))
        hr = D3DDDIERR_INVALIDCALL;
    if (SUCCEEDED(hr) && expand) {
        try {
            expandedIndices.resize(drawCount);
        } catch (...) {
            hr = E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr) && !triton9_draw_expand_fan(
                normalizedIndices.data(), indexStride, 0,
                args->PrimitiveCount, expandedIndices.data(), drawCount))
            hr = D3DDDIERR_INVALIDCALL;
    }
    if (SUCCEEDED(hr))
        hr = triton9BindDraw2VertexStreamZero(
            device, vertexShader, declaration, firstVertexByte, vertexBytes);
    if (SUCCEEDED(hr))
        hr = triton9ValidateStreamRange(device, vertexShader, declaration,
                                        0, args->NumVertices, 1);
    if (SUCCEEDED(hr)) {
        if (expand)
            hr = triton9BindUpIndexSlice(device, sizeof(UINT),
                                         expandedIndices.data(), 0, drawCount,
                                         &startIndex);
        else
            hr = triton9BindUpIndexSlice(device, indexStride,
                                         normalizedIndices.data(), 0,
                                         indexCount, &startIndex);
    }
    if (SUCCEEDED(hr)) {
        device->hostContext->IASetPrimitiveTopology(topology);
        device->hostContext->DrawIndexed(expand ? drawCount : indexCount,
                                         startIndex, 0);
    }
    triton9RestoreTemporaryVertexStreams(device);
    triton9RestoreIndexBuffer(device);
    if (SUCCEEDED(hr))
        hr = triton9CheckHostDevice(device);
    if (SUCCEEDED(hr))
        triton9DrawResourcesWritten(device);
    if (FAILED(hr))
        triton9DiagU32("TRITON9-DRAW-INDEXED2-HR", (DWORD)hr);
    LeaveCriticalSection(&device->shaderLock);
    triton9TraceDdiU32("TRITON9-DDI-DRAW-IP2-HR", (DWORD)hr);
    return triton9MapDeviceFailure(device, hr);
}

extern "C" void
triton9ReleaseConstantBuffers(TRITON9_DEVICE *device)
{
    if (!device)
        return;
    for (auto *fixed : {&device->fixedVertexFloatConstants,
                        &device->fixedPixelFloatConstants}) {
        if (fixed->hostBuffer)
            fixed->hostBuffer->Release();
        if (fixed->data)
            HeapFree(GetProcessHeap(), 0, fixed->data);
        ZeroMemory(fixed, sizeof(*fixed));
    }
    for (UINT slot = 0; slot < TRITON9_MAX_CONSTANT_BUFFERS; ++slot) {
        TRITON9_CONSTANT_BUFFER *vertex = &device->vertexConstants[slot];
        TRITON9_CONSTANT_BUFFER *pixel = &device->pixelConstants[slot];
        if (vertex->hostBuffer)
            vertex->hostBuffer->Release();
        if (pixel->hostBuffer)
            pixel->hostBuffer->Release();
        if (vertex->data)
            HeapFree(GetProcessHeap(), 0, vertex->data);
        if (pixel->data)
            HeapFree(GetProcessHeap(), 0, pixel->data);
        ZeroMemory(vertex, sizeof(*vertex));
        ZeroMemory(pixel, sizeof(*pixel));
    }
}

extern "C" void
triton9ReleaseUpBuffers(TRITON9_DEVICE *device)
{
    if (!device)
        return;
    for (UINT stream = 0; stream < TRITON9_MAX_VERTEX_STREAMS; ++stream) {
        if (device->upVertexBuffers[stream])
            device->upVertexBuffers[stream]->Release();
        device->upVertexBuffers[stream] = nullptr;
        device->upVertexCapacities[stream] = 0;
        device->upVertexBytes[stream] = 0;
        device->upVertexData[stream] = nullptr;
        device->drawStreamOffsets[stream] = 0;
        device->drawStreamOffsetOverrides[stream] = FALSE;
    }
    if (device->upIndexBuffer)
        device->upIndexBuffer->Release();
    device->upIndexBuffer = nullptr;
    device->upIndexCapacity = 0;
    device->upIndexData = nullptr;
    device->upIndexStride = 0;
}

extern "C" void
triton9ReleaseFixedFunctionShaders(TRITON9_DEVICE *device)
{
    Triton9FixedFunctionShaders *shaders;

    if (!device)
        return;
    shaders = static_cast<Triton9FixedFunctionShaders *>(device->fixedFunctionShaders);
    if (!shaders)
        return;
    if (device->drawVertexShader == shaders->vertexShader)
        device->drawVertexShader = nullptr;
    if (device->drawVertexDeclaration == shaders->vertexDeclaration)
        device->drawVertexDeclaration = nullptr;
    delete shaders->vertexShader;
    delete shaders->pixelShader;
    delete shaders->geometryLinkage;
    delete shaders->vertexSnapshot;
    delete shaders->pixelSnapshot;
    if (shaders->geometryShader)
        shaders->geometryShader->Release();
    delete shaders;
    device->fixedFunctionShaders = nullptr;
}

extern "C" HRESULT APIENTRY
triton9CreatePixelShader(HANDLE hDevice, D3DDDIARG_CREATEPIXELSHADER *args,
                         const UINT *tokens)
{
    HRESULT hr;

    if (!args)
        return E_INVALIDARG;
    triton9DiagU32("TRITON9-CREATE-PS-SIZE", args->CodeSize);
    if (tokens && args->CodeSize >= sizeof(*tokens))
        triton9DiagU32("TRITON9-CREATE-PS-VERSION", tokens[0]);
    hr = triton9CreateShader(static_cast<TRITON9_DEVICE *>(hDevice),
                             Triton9ShaderStage::Pixel, args->CodeSize, tokens,
                             &args->ShaderHandle);
    triton9DiagU32("TRITON9-CREATE-PS-HR", (DWORD)hr);
    return hr;
}

extern "C" HRESULT APIENTRY
triton9DeletePixelShader(HANDLE hDevice, HANDLE shader)
{
    return triton9DeleteShader(static_cast<TRITON9_DEVICE *>(hDevice),
                               Triton9ShaderStage::Pixel, shader);
}

extern "C" HRESULT APIENTRY
triton9SetPixelShader(HANDLE hDevice, HANDLE shader)
{
    HRESULT hr;

    triton9TraceDdiU32("TRITON9-DDI-SET-PS-PID", GetCurrentProcessId());
    hr = triton9SetShader(static_cast<TRITON9_DEVICE *>(hDevice),
                          Triton9ShaderStage::Pixel, shader);
    triton9TraceDdiU32("TRITON9-DDI-SET-PS-HR", (DWORD)hr);
    return hr;
}
