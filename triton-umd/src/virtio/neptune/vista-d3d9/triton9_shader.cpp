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
};

/* These shaders are not exposed as D3D9 handles.  They only cover the small
 * null-stage path used while bringing up the Vista compositor. */
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

static UINT
triton9LegacyInstructionLength(UINT token, UINT version)
{
    const UINT opcode = token & D3DSI_OPCODE_MASK;

    if (opcode == D3DSIO_COMMENT)
        return ((token & D3DSI_COMMENTSIZE_MASK) >>
                D3DSI_COMMENTSIZE_SHIFT) + 1;
    if (opcode == D3DSIO_DEF || opcode == D3DSIO_DEFI)
        return 6;
    if (opcode == D3DSIO_DEFB)
        return 3;
    if ((version & 0xffff0000u) == 0xffff0000u ||
        version >= D3DVS_VERSION(2, 0))
        return ((token & D3DSI_INSTLENGTH_MASK) >>
                D3DSI_INSTLENGTH_SHIFT) + 1;

    /* Shader model 1 vertex instructions do not carry a reliable encoded
     * length.  Keep this table equal to ShaderConverter's parser. */
    switch (opcode) {
    case D3DSIO_DCL:
        return 3;
    case D3DSIO_END:
    case D3DSIO_NOP:
        return 1;
    case D3DSIO_EXP:
    case D3DSIO_EXPP:
    case D3DSIO_FRC:
    case D3DSIO_LIT:
    case D3DSIO_LOG:
    case D3DSIO_LOGP:
    case D3DSIO_MOV:
    case D3DSIO_RCP:
    case D3DSIO_RSQ:
        return 3;
    case D3DSIO_ADD:
    case D3DSIO_DP3:
    case D3DSIO_DP4:
    case D3DSIO_DST:
    case D3DSIO_M4x4:
    case D3DSIO_M4x3:
    case D3DSIO_M3x4:
    case D3DSIO_M3x3:
    case D3DSIO_M3x2:
    case D3DSIO_MAX:
    case D3DSIO_MIN:
    case D3DSIO_MUL:
    case D3DSIO_SGE:
    case D3DSIO_SLT:
        return 4;
    case D3DSIO_MAD:
        return 5;
    default:
        return 0;
    }
}

static bool
triton9ValidateLegacyShader(const UINT *tokens, UINT byteCount,
                            Triton9ShaderStage stage)
{
    const UINT expectedType = stage == Triton9ShaderStage::Vertex
                                  ? 0xfffe0000u : 0xffff0000u;
    const UINT words = byteCount / sizeof(*tokens);
    const UINT version = tokens ? tokens[0] : 0;
    const UINT major = (version >> 8) & 0xffu;
    const UINT minor = version & 0xffu;

    if (!tokens || byteCount < 2 * sizeof(*tokens) ||
        (byteCount & (sizeof(*tokens) - 1)) || byteCount > (4u << 20) ||
        (version & 0xffff0000u) != expectedType)
        return false;
    if (stage == Triton9ShaderStage::Vertex) {
        if (!((major == 1 && minor == 1) ||
              (major == 2 && minor == 0) ||
              (major == 3 && minor == 0)))
            return false;
    } else if (!((major == 1 && minor >= 1 && minor <= 4) ||
                 (major == 2 && minor == 0) ||
                 (major == 3 && minor == 0))) {
        return false;
    }

    for (UINT index = 1; index < words;) {
        const UINT token = tokens[index];
        UINT length;

        if (token == 0x0000ffffu)
            return index + 1 == words;
        if ((token & D3DSI_OPCODE_MASK) == D3DSIO_END)
            return false;
        length = triton9LegacyInstructionLength(token, version);
        if (!length || length > words - index)
            return false;
        index += length;
    }
    return false;
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
    ShaderConv::ConvertShaderArgs args(
        9, ShaderConv::AnythingTimes0Equals0, shader->rasterStates);
    ShaderConv::VSInputDecls convertedInputs(ShaderConv::MAX_VS_INPUT_REGS);
    ShaderConv::VSOutputDecls convertedOutputs;
    ShaderConv::VSOutputDecls emptyOutputs;
    std::vector<TRITON_DXBC_SIGNATURE> inputSignatures;
    std::vector<TRITON_DXBC_SIGNATURE> outputSignatures;
    ShaderConv::ByteCode converted;
    HRESULT hr;

    if (!device || !device->hostDevice || !shader || shader->legacyTokens.empty())
        return E_INVALIDARG;
    if (shader->stage == Triton9ShaderStage::Pixel && linkedVertexShader &&
        linkedVertexShader->stage != Triton9ShaderStage::Vertex)
        return E_INVALIDARG;

    hr = triton9RestoreShaderVariant(shader, declaration, linkedVertexShader);
    if (hr != S_FALSE)
        return hr;

    if (declaration)
        convertedInputs = declaration->inputDecls;
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
        shader->inputDecls = convertedInputs;
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
    ShaderConv::VSOutputDecls convertedOutputs;
    std::vector<TRITON_DXBC_SIGNATURE> inputSignatures;
    std::vector<TRITON_DXBC_SIGNATURE> outputSignatures;
    ShaderConv::ByteCode converted;
    ShaderConv::ConvertTLShaderArgs args(
        9, ShaderConv::AnythingTimes0Equals0, declaration->inputDecls,
        convertedOutputs);
    HRESULT hr;

    if (!device || !device->hostDevice || !shader || !declaration ||
        shader->stage != Triton9ShaderStage::Vertex ||
        !declaration->hasTransformedPosition)
        return E_INVALIDARG;

    try {
        hr = shader->converter.ConvertTLShader(args);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    if (FAILED(hr) || !args.convertedByteCode.m_pByteCode ||
        !args.convertedByteCode.m_byteCodeSize)
        return FAILED(hr) ? hr : E_FAIL;

    converted = args.convertedByteCode;
    hr = triton9BuildVsInputSignatures(declaration, declaration->inputDecls,
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

    shader->inputDecls = declaration->inputDecls;
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
    UINT value;

    if (!device || !slotClass || !stepRate || stream >= TRITON9_MAX_VERTEX_STREAMS)
        return E_INVALIDARG;
    frequency = device->streamSourceFrequencies[stream];
    if (frequency == 1) {
        *slotClass = D3D11_INPUT_PER_VERTEX_DATA;
        *stepRate = 0;
        return S_OK;
    }
    value = frequency & kTriton9StreamFrequencyValueMask;
    if (stream == 0 && (frequency & D3DSTREAMSOURCE_INDEXEDDATA) &&
        !(frequency & D3DSTREAMSOURCE_INSTANCEDATA) && value) {
        *slotClass = D3D11_INPUT_PER_VERTEX_DATA;
        *stepRate = 0;
        return S_OK;
    }
    if (stream != 0 && (frequency & D3DSTREAMSOURCE_INSTANCEDATA) &&
        !(frequency & D3DSTREAMSOURCE_INDEXEDDATA) && value) {
        *slotClass = D3D11_INPUT_PER_INSTANCE_DATA;
        *stepRate = value;
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
    /* D3D11 advances a per-instance element after each step-rate group. */
    *firstElement = 0;
    *elementCount = (instanceCount - 1u) / stepRate + 1u;
    return S_OK;
}

/* The caller holds shaderLock. */
static HRESULT
triton9GetInstanceCount(const TRITON9_DEVICE *device,
                        const Triton9Shader *shader,
                        const Triton9VertexDeclaration *declaration,
                        UINT *instanceCount)
{
    UINT streamZeroFrequency;

    if (!device || !shader || !declaration || !instanceCount)
        return E_INVALIDARG;
    streamZeroFrequency = device->streamSourceFrequencies[0];
    if (streamZeroFrequency == 1) {
        *instanceCount = 1;
    } else if ((streamZeroFrequency & D3DSTREAMSOURCE_INDEXEDDATA) &&
               !(streamZeroFrequency & D3DSTREAMSOURCE_INSTANCEDATA) &&
               (streamZeroFrequency & kTriton9StreamFrequencyValueMask)) {
        *instanceCount = streamZeroFrequency & kTriton9StreamFrequencyValueMask;
    } else {
        return D3DDDIERR_INVALIDCALL;
    }
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
    }
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

static HRESULT
triton9PrepareVertexShader(TRITON9_DEVICE *device, Triton9Shader *shader,
                           Triton9VertexDeclaration *declaration)
{
    const UINT64 serial = declaration ? declaration->serial : 0;
    HRESULT hr;

    if (!shader || shader->stage != Triton9ShaderStage::Vertex)
        return E_INVALIDARG;
    if (shader->conversionDeclarationSerial != serial || !shader->vertexShader) {
        hr = triton9ConvertShader(device, shader, declaration, nullptr);
        if (FAILED(hr))
            return hr;
    }
    if (!declaration)
        return S_OK;
    return triton9EnsureInputLayout(device, shader, declaration);
}

static HRESULT
triton9PreparePixelShader(TRITON9_DEVICE *device, Triton9Shader *shader,
                          const Triton9Shader *vertexShader)
{
    const BOOL alphaTest = device &&
        device->renderStates[D3DDDIRS_ALPHATESTENABLE] != 0;
    const UINT alphaFunc = alphaTest
        ? device->renderStates[D3DDDIRS_ALPHAFUNC] : D3DCMP_ALWAYS;
    const BOOL fogEnable = device &&
        device->renderStates[D3DDDIRS_FOGENABLE] != 0;
    BOOL rasterStateChanged;

    if (!shader || shader->stage != Triton9ShaderStage::Pixel)
        return E_INVALIDARG;
    rasterStateChanged = shader->rasterStates.AlphaTestEnable !=
                             (UINT)alphaTest ||
                         shader->rasterStates.AlphaFunc != alphaFunc ||
                         shader->rasterStates.FogEnable != (UINT)fogEnable ||
                         shader->rasterStates.FogTableMode != D3DFOG_NONE ||
                         shader->rasterStates.WFogEnable != 0;
    shader->rasterStates.AlphaTestEnable = alphaTest;
    shader->rasterStates.AlphaFunc = alphaFunc;
    shader->rasterStates.FogEnable = fogEnable;
    shader->rasterStates.FogTableMode = D3DFOG_NONE;
    shader->rasterStates.WFogEnable = FALSE;
    if (!vertexShader)
        return S_OK;
    if (rasterStateChanged || shader->linkedVertexShader != vertexShader ||
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
    /* ShaderConverter reserves the remaining slots for state extensions.
     * Keep zero-filled buffers bound until the corresponding state path is
     * implemented. This avoids an unbound cbuffer for ordinary shaders. */
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
            UINT firstScalar;
            UINT scalarCount;

            if (!triton9_shader_constant_scalar_range(
                    constant.RegIndex, components, &firstScalar, &scalarCount) ||
                firstScalar > UINT_MAX - scalarCount)
                return UINT_MAX;
            inlineScalarCount = std::max(inlineScalarCount,
                                         firstScalar + scalarCount);
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
        UINT firstScalar;
        UINT ignoredScalarCount;

        if (!triton9_shader_constant_scalar_range(
                constant.RegIndex, scalarCount, &firstScalar,
                &ignoredScalarCount) ||
            firstScalar > UINT_MAX / bytesPerScalar ||
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
    BYTE inlineData[4096];
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
            depthRange <= 0.0f)
            return D3DDDIERR_INVALIDCALL;
        extension.vViewPortScale[0] = 1.0f / viewport.Width;
        extension.vViewPortScale[1] = -1.0f / viewport.Height;
        extension.vScreenToClipOffset[0] =
            0.5f - (viewport.Width * 0.5f + viewport.TopLeftX);
        extension.vScreenToClipOffset[1] =
            0.5f - (viewport.Height * 0.5f + viewport.TopLeftY);
        extension.vScreenToClipOffset[2] = -viewport.MinDepth;
        extension.vScreenToClipScale[0] = 2.0f / viewport.Width;
        extension.vScreenToClipScale[1] = -2.0f / viewport.Height;
        extension.vScreenToClipScale[2] = 1.0f / depthRange;
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
        const UINT capacity = triton9ConstantBufferSize(vertex, slot);
        if (needed > capacity)
            return D3DDDIERR_INVALIDCALL;
        hr = triton9EnsureConstantBuffer(device, buffer, capacity);
        if (FAILED(hr))
            return hr;
        if (slot < 3 && !shader->inlineConstants[slot].empty()) {
            /* DEF/DEFI/DEFB belong to this shader, not to the runtime's
             * saved constant registers. Overlay an upload copy so a later
             * shader can recover the runtime values without another SetConst. */
            if (capacity > sizeof(inlineData))
                return E_FAIL;
            memcpy(inlineData, buffer->data, capacity);
            TRITON9_CONSTANT_BUFFER upload = *buffer;
            upload.data = inlineData;
            hr = triton9ApplyInlineConstants(&upload, shader, slot);
            if (FAILED(hr))
                return hr;
            device->hostContext->UpdateSubresource(buffer->hostBuffer, 0,
                                                    nullptr, inlineData, 0, 0);
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
        device->hostContext->VSSetConstantBuffers(0, count, buffers);
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
    Triton9Shader *shader;
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
        shader = static_cast<Triton9Shader *>(vertex ? device->activeVertexShader :
                                                       device->activePixelShader);
        /* Creation-time constants are D3D9 state, just like a null shader
         * binding.  Keep them dirty until a draw acquires the proxy; binding
         * them here would re-enter the runtime during CreateDeviceEx. */
        if (shader && device->hostContext)
            hr = triton9BindStageConstants(device, shader, vertex);
    }
    if (SUCCEEDED(hr) && device->hostContext)
        hr = triton9CheckHostDevice(device);
    LeaveCriticalSection(&device->shaderLock);
    return triton9MapDeviceFailure(device, hr);
}

static UINT
triton9FixedInstruction(UINT opcode, UINT parameterCount, UINT control = 0)
{
    return opcode | (parameterCount << D3DSI_INSTLENGTH_SHIFT) | control;
}

static UINT
triton9FixedRegisterType(D3DSHADER_PARAM_REGISTER_TYPE type)
{
    return ((UINT)type << D3DSP_REGTYPE_SHIFT & D3DSP_REGTYPE_MASK) |
           ((UINT)type << D3DSP_REGTYPE_SHIFT2 & D3DSP_REGTYPE_MASK2);
}

static UINT
triton9FixedDestination(D3DSHADER_PARAM_REGISTER_TYPE type, UINT index,
                        UINT writeMask = D3DSP_WRITEMASK_ALL)
{
    return 0x80000000u | triton9FixedRegisterType(type) |
           (index & D3DSP_REGNUM_MASK) | writeMask;
}

static UINT
triton9FixedSource(D3DSHADER_PARAM_REGISTER_TYPE type, UINT index,
                   UINT swizzle = D3DSP_NOSWIZZLE)
{
    return 0x80000000u | triton9FixedRegisterType(type) |
           (index & D3DSP_REGNUM_MASK) | swizzle;
}

static UINT
triton9FixedNegatedSource(D3DSHADER_PARAM_REGISTER_TYPE type, UINT index,
                          UINT swizzle = D3DSP_NOSWIZZLE)
{
    return triton9FixedSource(type, index, swizzle) |
           D3DSPSM_NEG;
}

static void
triton9FixedDcl(std::vector<UINT> *tokens, UINT usage, UINT usageIndex,
                D3DSHADER_PARAM_REGISTER_TYPE type, UINT registerIndex)
{
    tokens->push_back(triton9FixedInstruction(D3DSIO_DCL, 2));
    tokens->push_back(triton9_sm2_dcl_semantic(usage, usageIndex));
    tokens->push_back(triton9FixedDestination(type, registerIndex));
}

static void
triton9FixedDef(std::vector<UINT> *tokens, UINT registerIndex,
                UINT x, UINT y, UINT z, UINT w)
{
    tokens->push_back(D3DSIO_DEF);
    tokens->push_back(triton9FixedDestination(D3DSPR_CONST, registerIndex));
    tokens->push_back(x);
    tokens->push_back(y);
    tokens->push_back(z);
    tokens->push_back(w);
}

static const ShaderConv::VSInputDecl *
triton9FixedInput(const Triton9VertexDeclaration *declaration, UINT usage,
                  UINT usageIndex)
{
    return declaration ? declaration->inputDecls.FindInputDecl(usage, usageIndex)
                       : nullptr;
}

static bool
triton9FixedVertexOutput(const Triton9Shader *shader, UINT usage,
                         UINT usageIndex)
{
    return shader && shader->outputDecls.FindOutputDecl(usage, usageIndex);
}

static UINT64
triton9FixedVertexStateKey(const TRITON9_DEVICE *device)
{
    if (!device || !device->renderStates[D3DDDIRS_FOGENABLE])
        return 0;
    return UINT64_C(0x100000000) |
           device->renderStates[D3DDDIRS_FOGVERTEXMODE];
}

static HRESULT
triton9BuildFixedVertexTokens(const TRITON9_DEVICE *device,
                              const Triton9VertexDeclaration *declaration,
                              std::vector<UINT> *tokens)
{
    const ShaderConv::VSInputDecl *position;
    const ShaderConv::VSInputDecl *diffuse;
    const ShaderConv::VSInputDecl *specular;
    const ShaderConv::VSInputDecl *texcoords[TRITON9_FIXED_TEXTURE_STAGES] = {};
    const D3DDDIVERTEXELEMENT *positionElement;
    const UINT fogMode = device && device->renderStates[D3DDDIRS_FOGENABLE]
        ? device->renderStates[D3DDDIRS_FOGVERTEXMODE] : D3DFOG_NONE;

    if (!declaration || !tokens || declaration->hasTransformedPosition)
        return E_INVALIDARG;
    position = triton9FixedInput(declaration, D3DDECLUSAGE_POSITION, 0);
    positionElement = triton9FindDeclarationElement(declaration,
                                                    D3DDECLUSAGE_POSITION, 0);
    if (!position || !positionElement ||
        (positionElement->Type != D3DDECLTYPE_FLOAT3 &&
         positionElement->Type != D3DDECLTYPE_FLOAT4))
        return D3DDDIERR_NOTAVAILABLE;
    for (const D3DDDIVERTEXELEMENT &element : declaration->elements) {
        if (element.Usage == D3DDECLUSAGE_POSITION && element.UsageIndex == 0)
            continue;
        if (element.Usage == D3DDECLUSAGE_COLOR && element.UsageIndex <= 1)
            continue;
        if (element.Usage == D3DDECLUSAGE_TEXCOORD &&
            element.UsageIndex < TRITON9_FIXED_TEXTURE_STAGES)
            continue;
        return D3DDDIERR_NOTAVAILABLE;
    }

    diffuse = triton9FixedInput(declaration, D3DDECLUSAGE_COLOR, 0);
    specular = triton9FixedInput(declaration, D3DDECLUSAGE_COLOR, 1);
    for (UINT stage = 0; stage < TRITON9_FIXED_TEXTURE_STAGES; ++stage)
        texcoords[stage] = triton9FixedInput(declaration, D3DDECLUSAGE_TEXCOORD, stage);
    try {
        tokens->clear();
        tokens->push_back(D3DVS_VERSION(2, 0));
        triton9FixedDcl(tokens, D3DDECLUSAGE_POSITION, 0, D3DSPR_INPUT,
                        position->RegIndex);
        if (diffuse)
            triton9FixedDcl(tokens, D3DDECLUSAGE_COLOR, 0, D3DSPR_INPUT,
                            diffuse->RegIndex);
        if (specular)
            triton9FixedDcl(tokens, D3DDECLUSAGE_COLOR, 1, D3DSPR_INPUT,
                            specular->RegIndex);
        for (UINT stage = 0; stage < TRITON9_FIXED_TEXTURE_STAGES; ++stage) {
            if (texcoords[stage])
                triton9FixedDcl(tokens, D3DDECLUSAGE_TEXCOORD, stage,
                                D3DSPR_INPUT, texcoords[stage]->RegIndex);
        }
        tokens->push_back(triton9FixedInstruction(D3DSIO_M4x4, 3));
        tokens->push_back(triton9FixedDestination(D3DSPR_RASTOUT,
                                                   D3DSRO_POSITION));
        tokens->push_back(triton9FixedSource(D3DSPR_INPUT, position->RegIndex));
        tokens->push_back(triton9FixedSource(D3DSPR_CONST, 0));
        if (fogMode != D3DFOG_NONE) {
            const UINT replicateX = D3DVS_X_X | D3DVS_Y_X |
                                    D3DVS_Z_X | D3DVS_W_X;
            const UINT replicateY = D3DVS_X_Y | D3DVS_Y_Y |
                                    D3DVS_Z_Y | D3DVS_W_Y;
            const UINT replicateZ = D3DVS_X_Z | D3DVS_Y_Z |
                                    D3DVS_Z_Z | D3DVS_W_Z;

            tokens->push_back(triton9FixedInstruction(D3DSIO_M4x4, 3));
            tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 0));
            tokens->push_back(triton9FixedSource(D3DSPR_INPUT,
                                                  position->RegIndex));
            tokens->push_back(triton9FixedSource(D3DSPR_CONST, 4));
            tokens->push_back(triton9FixedInstruction(D3DSIO_ABS, 2));
            tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 0,
                                                       D3DSP_WRITEMASK_0));
            tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 0, replicateZ));
            if (fogMode == D3DFOG_LINEAR) {
                tokens->push_back(triton9FixedInstruction(D3DSIO_ADD, 3));
                tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 0,
                                                           D3DSP_WRITEMASK_0));
                tokens->push_back(triton9FixedSource(D3DSPR_CONST, 8,
                                                      replicateY));
                tokens->push_back(triton9FixedNegatedSource(D3DSPR_TEMP, 0,
                                                             replicateX));
                tokens->push_back(triton9FixedInstruction(D3DSIO_MUL, 3));
                tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 0,
                                                           D3DSP_WRITEMASK_0));
                tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 0,
                                                      replicateX));
                tokens->push_back(triton9FixedSource(D3DSPR_CONST, 8,
                                                      replicateZ));
            } else if (fogMode == D3DFOG_EXP2) {
                tokens->push_back(triton9FixedInstruction(D3DSIO_MUL, 3));
                tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 0,
                                                           D3DSP_WRITEMASK_0));
                tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 0,
                                                      replicateX));
                tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 0,
                                                      replicateX));
                tokens->push_back(triton9FixedInstruction(D3DSIO_MUL, 3));
                tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 0,
                                                           D3DSP_WRITEMASK_0));
                tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 0,
                                                      replicateX));
                tokens->push_back(triton9FixedSource(D3DSPR_CONST, 8,
                                                      replicateX));
            } else if (fogMode == D3DFOG_EXP) {
                tokens->push_back(triton9FixedInstruction(D3DSIO_MUL, 3));
                tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 0,
                                                           D3DSP_WRITEMASK_0));
                tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 0,
                                                      replicateX));
                tokens->push_back(triton9FixedSource(D3DSPR_CONST, 8,
                                                      replicateX));
            } else {
                return D3DDDIERR_NOTAVAILABLE;
            }
            if (fogMode == D3DFOG_EXP || fogMode == D3DFOG_EXP2) {
                tokens->push_back(triton9FixedInstruction(D3DSIO_EXPP, 2));
                tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 0,
                                                           D3DSP_WRITEMASK_0));
                tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 0,
                                                      replicateX));
            }
            tokens->push_back(triton9FixedInstruction(D3DSIO_MOV, 2));
            tokens->push_back(triton9FixedDestination(D3DSPR_RASTOUT,
                                                       D3DSRO_FOG,
                                                       D3DSP_WRITEMASK_0) |
                              D3DSPDM_SATURATE);
            tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 0,
                                                  replicateX));
        }
        if (diffuse) {
            tokens->push_back(triton9FixedInstruction(D3DSIO_MOV, 2));
            tokens->push_back(triton9FixedDestination(D3DSPR_ATTROUT, 0));
            tokens->push_back(triton9FixedSource(D3DSPR_INPUT, diffuse->RegIndex));
        }
        if (specular) {
            tokens->push_back(triton9FixedInstruction(D3DSIO_MOV, 2));
            tokens->push_back(triton9FixedDestination(D3DSPR_ATTROUT, 1));
            tokens->push_back(triton9FixedSource(D3DSPR_INPUT, specular->RegIndex));
        }
        for (UINT stage = 0; stage < TRITON9_FIXED_TEXTURE_STAGES; ++stage) {
            if (!texcoords[stage])
                continue;
            tokens->push_back(triton9FixedInstruction(D3DSIO_MOV, 2));
            tokens->push_back(triton9FixedDestination(D3DSPR_TEXCRDOUT, stage));
            tokens->push_back(triton9FixedSource(D3DSPR_INPUT, texcoords[stage]->RegIndex));
        }
        tokens->push_back(D3DVS_END());
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

enum class Triton9FixedSource {
    Current,
    Diffuse,
    Specular,
    Texture,
    TextureFactor,
    White,
    Zero,
};

static HRESULT
triton9FixedSourceFromArgument(UINT argument, BOOL hasDiffuse, BOOL hasSpecular,
                               BOOL hasTexture, UINT stage,
                               Triton9FixedSource *source)
{
    if (!source || stage >= TRITON9_FIXED_TEXTURE_STAGES || (argument & ~D3DTA_SELECTMASK))
        return D3DDDIERR_NOTAVAILABLE;
    switch (argument & D3DTA_SELECTMASK) {
    case D3DTA_DIFFUSE:
        *source = hasDiffuse ? Triton9FixedSource::Diffuse
                             : Triton9FixedSource::White;
        return S_OK;
    case D3DTA_SPECULAR:
        /* FVF COLOR1 is the D3D9 fixed-function specular input.  It is
         * explicitly exercised by Vista MIL's Level-1 device test; an
         * absent COLOR1 has the documented black default rather than being
         * silently treated as diffuse. */
        *source = hasSpecular ? Triton9FixedSource::Specular
                              : Triton9FixedSource::Zero;
        return S_OK;
    case D3DTA_CURRENT:
        *source = Triton9FixedSource::Current;
        return S_OK;
    case D3DTA_TEXTURE:
        *source = !hasTexture ? Triton9FixedSource::Zero
                              : Triton9FixedSource::Texture;
        return S_OK;
    case D3DTA_TFACTOR:
        *source = Triton9FixedSource::TextureFactor;
        return S_OK;
    default:
        return D3DDDIERR_NOTAVAILABLE;
    }
}

static bool
triton9FixedUsesTexture(UINT argument)
{
    return !(argument & ~D3DTA_SELECTMASK) &&
           (argument & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
}

static bool
triton9FixedOperationUsesArgument(UINT operation, UINT argumentIndex)
{
    if (operation == D3DTOP_MODULATE)
        return true;
    if (operation == D3DTOP_SELECTARG1)
        return argumentIndex == 0;
    if (operation == D3DTOP_SELECTARG2)
        return argumentIndex == 1;
    return false;
}

static UINT
triton9FixedSourceToken(Triton9FixedSource source)
{
    switch (source) {
    case Triton9FixedSource::Current:
        return triton9FixedSource(D3DSPR_TEMP, 0);
    case Triton9FixedSource::Diffuse:
        return triton9FixedSource(D3DSPR_INPUT, 0);
    case Triton9FixedSource::Specular:
        return triton9FixedSource(D3DSPR_INPUT, 1);
    case Triton9FixedSource::Texture:
        return triton9FixedSource(D3DSPR_TEMP, 1);
    case Triton9FixedSource::TextureFactor:
        return triton9FixedSource(D3DSPR_CONST, 0);
    case Triton9FixedSource::White:
        return triton9FixedSource(D3DSPR_CONST, 2);
    case Triton9FixedSource::Zero:
        return triton9FixedSource(D3DSPR_CONST, 4);
    }
    return 0;
}

static HRESULT
triton9FixedOperation(UINT value)
{
    return value == D3DTOP_SELECTARG1 || value == D3DTOP_SELECTARG2 ||
           value == D3DTOP_MODULATE || value == D3DTOP_DISABLE
               ? S_OK : D3DDDIERR_NOTAVAILABLE;
}

static void
triton9FixedEmitOperation(std::vector<UINT> *tokens, UINT operation,
                          UINT writeMask, Triton9FixedSource argument1,
                          Triton9FixedSource argument2)
{
    const UINT source1 = triton9FixedSourceToken(argument1);
    const UINT source2 = triton9FixedSourceToken(argument2);

    if (operation == D3DTOP_SELECTARG2)
        tokens->push_back(triton9FixedInstruction(D3DSIO_MOV, 2));
    else if (operation == D3DTOP_MODULATE)
        tokens->push_back(triton9FixedInstruction(D3DSIO_MUL, 3));
    else
        tokens->push_back(triton9FixedInstruction(D3DSIO_MOV, 2));
    tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 0, writeMask));
    tokens->push_back(operation == D3DTOP_SELECTARG2 ? source2 : source1);
    if (operation == D3DTOP_MODULATE)
        tokens->push_back(source2);
}

static UINT64
triton9FixedHash(UINT64 hash, UINT value)
{
    return (hash ^ value) * UINT64_C(1099511628211);
}

static HRESULT
triton9FixedAlphaComparison(UINT comparison, UINT *result)
{
    if (!result)
        return E_INVALIDARG;
    switch (comparison) {
    case D3DCMP_NEVER:        *result = D3DSPC_RESERVED0; return S_OK;
    case D3DCMP_LESS:         *result = D3DSPC_LT; return S_OK;
    case D3DCMP_EQUAL:        *result = D3DSPC_EQ; return S_OK;
    case D3DCMP_LESSEQUAL:    *result = D3DSPC_LE; return S_OK;
    case D3DCMP_GREATER:      *result = D3DSPC_GT; return S_OK;
    case D3DCMP_NOTEQUAL:     *result = D3DSPC_NE; return S_OK;
    case D3DCMP_GREATEREQUAL: *result = D3DSPC_GE; return S_OK;
    case D3DCMP_ALWAYS:       *result = D3DSPC_RESERVED1; return S_OK;
    default: return D3DDDIERR_NOTAVAILABLE;
    }
}

static HRESULT
triton9BuildFixedPixelTokens(TRITON9_DEVICE *device,
                             const Triton9Shader *vertexShader,
                             std::vector<UINT> *tokens, UINT64 *stateKey)
{
    const BOOL hasDiffuse = triton9FixedVertexOutput(vertexShader,
                                                      D3DDECLUSAGE_COLOR, 0);
    const BOOL hasSpecular = triton9FixedVertexOutput(vertexShader,
                                                       D3DDECLUSAGE_COLOR, 1);
    const BOOL hasFog = triton9FixedVertexOutput(vertexShader,
                                                  D3DDECLUSAGE_FOG, 0);
    BOOL usesTexture[TRITON9_FIXED_TEXTURE_STAGES] = {};
    BOOL activeStage[TRITON9_FIXED_TEXTURE_STAGES] = {};
    const BOOL fogFromVertexMode = device && hasFog &&
        device->renderStates[D3DDDIRS_FOGVERTEXMODE] != D3DFOG_NONE;
    const BOOL fogFromTransformedSpecular = hasSpecular &&
        vertexShader && vertexShader->transformedFixedFunction;
    const BOOL fogEnabled = device &&
        device->renderStates[D3DDDIRS_FOGENABLE] &&
        (fogFromVertexMode || fogFromTransformedSpecular);
    UINT64 key = UINT64_C(1469598103934665603);
    UINT alphaComparison = D3DSPC_RESERVED1;

    if (!device || !vertexShader || !tokens || !stateKey)
        return E_INVALIDARG;
    key = triton9FixedHash(key, hasDiffuse);
    key = triton9FixedHash(key, hasSpecular);
    key = triton9FixedHash(key, device->renderStates[D3DDDIRS_ALPHATESTENABLE]);
    key = triton9FixedHash(key, device->renderStates[D3DDDIRS_ALPHAFUNC]);
    key = triton9FixedHash(key, fogEnabled);
    for (UINT stage = 0; stage < TRITON9_FIXED_TEXTURE_STAGES; ++stage) {
        const UINT *states = device->textureStageStates[stage];
        const UINT colorOperation = states[D3DDDITSS_COLOROP];
        const UINT alphaOperation = states[D3DDDITSS_ALPHAOP];
        const BOOL hasTexture = device->textures[stage] != nullptr;
        BOOL colorUsesTexture = FALSE;
        BOOL alphaUsesTexture = FALSE;
        HRESULT hr = triton9FixedOperation(colorOperation);

        if (FAILED(hr))
            return hr;
        key = triton9FixedHash(key, colorOperation);
        key = triton9FixedHash(key, hasTexture);
        if (colorOperation == D3DTOP_DISABLE)
            break;
        if (FAILED(triton9FixedOperation(alphaOperation)))
            return D3DDDIERR_NOTAVAILABLE;
        key = triton9FixedHash(key, alphaOperation);
        const UINT argumentStates[] = {
            D3DDDITSS_COLORARG1, D3DDDITSS_COLORARG2,
            D3DDDITSS_ALPHAARG1, D3DDDITSS_ALPHAARG2,
        };
        for (UINT argumentIndex = 0; argumentIndex < 4; ++argumentIndex) {
            const UINT state = argumentStates[argumentIndex];
            const UINT operation = argumentIndex < 2 ? colorOperation
                                                     : alphaOperation;
            key = triton9FixedHash(key, states[state]);
            if (operation != D3DTOP_DISABLE &&
                triton9FixedOperationUsesArgument(operation, argumentIndex & 1u) &&
                triton9FixedUsesTexture(states[state])) {
                if (argumentIndex < 2)
                    colorUsesTexture = TRUE;
                else
                    alphaUsesTexture = TRUE;
            }
        }
        /* Native D3D9 disables this stage and every later stage when an
         * active color argument needs an unbound texture. An alpha-only
         * reference to an unbound texture reads zero. */
        if (colorUsesTexture && !hasTexture)
            break;
        activeStage[stage] = TRUE;
        usesTexture[stage] = hasTexture &&
                             (colorUsesTexture || alphaUsesTexture);
        if (usesTexture[stage] &&
            !triton9FixedVertexOutput(vertexShader, D3DDECLUSAGE_TEXCOORD,
                                      stage))
            return D3DDDIERR_NOTAVAILABLE;
    }
    if (device->renderStates[D3DDDIRS_ALPHATESTENABLE]) {
        HRESULT hr = triton9FixedAlphaComparison(
            device->renderStates[D3DDDIRS_ALPHAFUNC], &alphaComparison);
        if (FAILED(hr))
            return hr;
    }

    try {
        Triton9FixedSource colorArguments[2] = {
            Triton9FixedSource::White, Triton9FixedSource::White,
        };
        Triton9FixedSource alphaArguments[2] = {
            Triton9FixedSource::White, Triton9FixedSource::White,
        };

        tokens->clear();
        tokens->push_back(D3DPS_VERSION(3, 0));
        if (hasDiffuse)
            triton9FixedDcl(tokens, D3DDECLUSAGE_COLOR, 0, D3DSPR_INPUT, 0);
        if (hasSpecular)
            triton9FixedDcl(tokens, D3DDECLUSAGE_COLOR, 1, D3DSPR_INPUT, 1);
        for (UINT stage = 0; stage < TRITON9_FIXED_TEXTURE_STAGES; ++stage) {
            if (!usesTexture[stage])
                continue;
            triton9FixedDcl(tokens, D3DDECLUSAGE_TEXCOORD, stage,
                            D3DSPR_INPUT, stage + 2);
            triton9FixedDcl(tokens, D3DSTT_2D, 0, D3DSPR_SAMPLER, stage);
        }
        if (fogFromVertexMode)
            triton9FixedDcl(tokens, D3DDECLUSAGE_FOG, 0, D3DSPR_INPUT, TRITON9_FIXED_TEXTURE_STAGES + 2);
        triton9FixedDef(tokens, 2, 0x3f800000u, 0x3f800000u,
                        0x3f800000u, 0x3f800000u);
        triton9FixedDef(tokens, 3, 0xbf800000u, 0xbf800000u,
                        0xbf800000u, 0xbf800000u);
        triton9FixedDef(tokens, 4, 0, 0, 0, 0);
        tokens->push_back(triton9FixedInstruction(D3DSIO_MOV, 2));
        tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 0));
        tokens->push_back(triton9FixedSourceToken(hasDiffuse
            ? Triton9FixedSource::Diffuse : Triton9FixedSource::White));
        for (UINT stage = 0; stage < TRITON9_FIXED_TEXTURE_STAGES; ++stage) {
            const UINT *states = device->textureStageStates[stage];
            const UINT colorOperation = states[D3DDDITSS_COLOROP];
            const UINT alphaOperation = states[D3DDDITSS_ALPHAOP];
            const BOOL hasTexture = device->textures[stage] != nullptr;
            HRESULT hr;

            if (!activeStage[stage] || colorOperation == D3DTOP_DISABLE)
                break;
            hr = S_OK;
            if (triton9FixedOperationUsesArgument(colorOperation, 0))
                hr = triton9FixedSourceFromArgument(states[D3DDDITSS_COLORARG1],
                                                    hasDiffuse, hasSpecular,
                                                    hasTexture, stage,
                                                    &colorArguments[0]);
            if (SUCCEEDED(hr) &&
                triton9FixedOperationUsesArgument(colorOperation, 1))
                hr = triton9FixedSourceFromArgument(states[D3DDDITSS_COLORARG2],
                                                    hasDiffuse, hasSpecular,
                                                    hasTexture, stage,
                                                    &colorArguments[1]);
            if (SUCCEEDED(hr) && alphaOperation != D3DTOP_DISABLE &&
                triton9FixedOperationUsesArgument(alphaOperation, 0))
                hr = triton9FixedSourceFromArgument(states[D3DDDITSS_ALPHAARG1],
                                                    hasDiffuse, hasSpecular,
                                                    hasTexture, stage,
                                                    &alphaArguments[0]);
            if (SUCCEEDED(hr) && alphaOperation != D3DTOP_DISABLE &&
                triton9FixedOperationUsesArgument(alphaOperation, 1))
                hr = triton9FixedSourceFromArgument(states[D3DDDITSS_ALPHAARG2],
                                                    hasDiffuse, hasSpecular,
                                                    hasTexture, stage,
                                                    &alphaArguments[1]);
            if (FAILED(hr))
                return hr;
            if (usesTexture[stage]) {
                tokens->push_back(triton9FixedInstruction(D3DSIO_TEX, 3));
                tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 1));
                tokens->push_back(triton9FixedSource(D3DSPR_INPUT, stage + 2));
                tokens->push_back(triton9FixedSource(D3DSPR_SAMPLER, stage));
            }
            triton9FixedEmitOperation(tokens, colorOperation,
                                      D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                                      D3DSP_WRITEMASK_2, colorArguments[0],
                                      colorArguments[1]);
            if (alphaOperation != D3DTOP_DISABLE)
                triton9FixedEmitOperation(tokens, alphaOperation,
                                          D3DSP_WRITEMASK_3, alphaArguments[0],
                                          alphaArguments[1]);
        }
        if (fogEnabled) {
            const UINT replicateX = D3DVS_X_X | D3DVS_Y_X |
                                    D3DVS_Z_X | D3DVS_W_X;

            tokens->push_back(triton9FixedInstruction(D3DSIO_ADD, 3));
            tokens->push_back(triton9FixedDestination(
                D3DSPR_TEMP, 3, D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                                     D3DSP_WRITEMASK_2));
            tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 0));
            tokens->push_back(triton9FixedNegatedSource(D3DSPR_CONST, 5));
            tokens->push_back(triton9FixedInstruction(D3DSIO_MAD, 4));
            tokens->push_back(triton9FixedDestination(
                D3DSPR_TEMP, 0, D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                                     D3DSP_WRITEMASK_2));
            tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 3));
            tokens->push_back(triton9FixedSource(
                D3DSPR_INPUT, fogFromVertexMode ? TRITON9_FIXED_TEXTURE_STAGES + 2 : 1,
                fogFromVertexMode ? replicateX
                                  : (D3DVS_X_W | D3DVS_Y_W |
                                     D3DVS_Z_W | D3DVS_W_W)));
            tokens->push_back(triton9FixedSource(D3DSPR_CONST, 5));
        }
        if (device->renderStates[D3DDDIRS_ALPHATESTENABLE]) {
            if (device->renderStates[D3DDDIRS_ALPHAFUNC] == D3DCMP_NEVER) {
                tokens->push_back(triton9FixedInstruction(D3DSIO_MOV, 2));
                tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 3));
                tokens->push_back(triton9FixedSource(D3DSPR_CONST, 3));
            } else if (device->renderStates[D3DDDIRS_ALPHAFUNC] != D3DCMP_ALWAYS) {
                const UINT alphaSwizzle = D3DVS_X_W | D3DVS_Y_W | D3DVS_Z_W |
                                          D3DVS_W_W;
                tokens->push_back(triton9FixedInstruction(
                    D3DSIO_IFC, 2,
                    alphaComparison << D3DSP_OPCODESPECIFICCONTROL_SHIFT));
                tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 0,
                                                      alphaSwizzle));
                tokens->push_back(triton9FixedSource(D3DSPR_CONST, 1,
                                                      alphaSwizzle));
                tokens->push_back(triton9FixedInstruction(D3DSIO_MOV, 2));
                tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 3));
                tokens->push_back(triton9FixedSource(D3DSPR_CONST, 2));
                tokens->push_back(D3DSIO_ELSE);
                tokens->push_back(triton9FixedInstruction(D3DSIO_MOV, 2));
                tokens->push_back(triton9FixedDestination(D3DSPR_TEMP, 3));
                tokens->push_back(triton9FixedSource(D3DSPR_CONST, 3));
                tokens->push_back(D3DSIO_ENDIF);
            }
            if (device->renderStates[D3DDDIRS_ALPHAFUNC] != D3DCMP_ALWAYS) {
                tokens->push_back(triton9FixedInstruction(D3DSIO_TEXKILL, 1));
                tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 3));
            }
        }
        tokens->push_back(triton9FixedInstruction(D3DSIO_MOV, 2));
        tokens->push_back(triton9FixedDestination(D3DSPR_COLOROUT, 0));
        tokens->push_back(triton9FixedSource(D3DSPR_TEMP, 0));
        tokens->push_back(D3DPS_END());
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    *stateKey = key;
    return S_OK;
}

static void
triton9FixedMultiplyMatrix(const D3DMATRIX &left, const D3DMATRIX &right,
                           D3DMATRIX *result)
{
    D3DMATRIX value = {};

    for (UINT row = 0; row < 4; ++row)
        for (UINT column = 0; column < 4; ++column)
            for (UINT index = 0; index < 4; ++index)
                value.m[row][column] += left.m[row][index] * right.m[index][column];
    *result = value;
}

static HRESULT
triton9UploadFixedVertexConstants(TRITON9_DEVICE *device)
{
    D3DMATRIX worldView;
    D3DMATRIX worldViewProjection;
    FLOAT constantsData[9][4] = {};
    TRITON9_CONSTANT_BUFFER *constants;
    HRESULT hr;

    if (!device)
        return E_INVALIDARG;
    triton9FixedMultiplyMatrix(device->worldTransform, device->viewTransform,
                               &worldView);
    triton9FixedMultiplyMatrix(worldView, device->projectionTransform,
                               &worldViewProjection);
    /* D3D9 matrices use row-vector notation. The legacy m4x4 instruction
     * computes dot products against c0-c3, so upload the transpose. */
    for (UINT row = 0; row < 4; ++row) {
        for (UINT column = 0; column < 4; ++column) {
            constantsData[row][column] =
                worldViewProjection.m[column][row];
            constantsData[row + 4u][column] = worldView.m[column][row];
        }
    }
    {
        const DWORD startBits = device->renderStates[D3DDDIRS_FOGSTART];
        const DWORD endBits = device->renderStates[D3DDDIRS_FOGEND];
        const DWORD densityBits = device->renderStates[D3DDDIRS_FOGDENSITY];
        FLOAT start;
        FLOAT end;
        FLOAT density;
        FLOAT distance;

        memcpy(&start, &startBits, sizeof(start));
        memcpy(&end, &endBits, sizeof(end));
        memcpy(&density, &densityBits, sizeof(density));
        distance = end - start;
        constantsData[8][0] = -density * 1.4426950408889634f;
        if (device->renderStates[D3DDDIRS_FOGVERTEXMODE] == D3DFOG_EXP2)
            constantsData[8][0] *= density;
        constantsData[8][1] = end;
        constantsData[8][2] = distance != 0.0f ? 1.0f / distance : 0.0f;
    }
    constants = &device->fixedVertexFloatConstants;
    hr = triton9EnsureConstantData(constants,
                                   triton9ConstantBufferSize(TRUE, 0));
    if (FAILED(hr))
        return hr;
    if (memcmp(constants->data, constantsData, sizeof(constantsData))) {
        memcpy(constants->data, constantsData, sizeof(constantsData));
        constants->dirty = TRUE;
    }
    return S_OK;
}

static HRESULT
triton9UploadFixedPixelConstants(TRITON9_DEVICE *device)
{
    FLOAT constantsData[24] = {};
    TRITON9_CONSTANT_BUFFER *constants;
    UINT color;
    HRESULT hr;

    if (!device)
        return E_INVALIDARG;
    color = device->renderStates[D3DDDIRS_TEXTUREFACTOR];
    constantsData[0] = (FLOAT)((color >> 16) & 0xffu) / 255.0f;
    constantsData[1] = (FLOAT)((color >> 8) & 0xffu) / 255.0f;
    constantsData[2] = (FLOAT)(color & 0xffu) / 255.0f;
    constantsData[3] = (FLOAT)((color >> 24) & 0xffu) / 255.0f;
    constantsData[4] = constantsData[5] = constantsData[6] = constantsData[7] =
        (FLOAT)(device->renderStates[D3DDDIRS_ALPHAREF] & 0xffu) / 255.0f;
    color = device->renderStates[D3DDDIRS_FOGCOLOR];
    constantsData[20] = (FLOAT)((color >> 16) & 0xffu) / 255.0f;
    constantsData[21] = (FLOAT)((color >> 8) & 0xffu) / 255.0f;
    constantsData[22] = (FLOAT)(color & 0xffu) / 255.0f;
    constants = &device->fixedPixelFloatConstants;
    hr = triton9EnsureConstantData(constants,
                                   triton9ConstantBufferSize(FALSE, 0));
    if (FAILED(hr))
        return hr;
    if (memcmp(constants->data, constantsData, sizeof(constantsData))) {
        memcpy(constants->data, constantsData, sizeof(constantsData));
        constants->dirty = TRUE;
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
triton9EnsureFixedVertexShader(TRITON9_DEVICE *device,
                               Triton9VertexDeclaration *declaration,
                               Triton9Shader **outShader)
{
    Triton9FixedFunctionShaders *shaders;
    Triton9Shader *shader;
    std::vector<UINT> tokens;
    const UINT64 stateKey = triton9FixedVertexStateKey(device);
    HRESULT hr;

    if (!device || !declaration || !outShader)
        return E_INVALIDARG;
    if (declaration->hasTransformedPosition)
        return triton9EnsureTransformedVertexShader(device, declaration, outShader);
    if (device->renderStates[D3DDDIRS_LIGHTING])
        return D3DDDIERR_NOTAVAILABLE;
    shaders = triton9FixedShaders(device);
    if (!shaders)
        return E_OUTOFMEMORY;
    if (shaders->vertexShader && shaders->vertexDeclaration == declaration &&
        shaders->vertexDeclarationSerial == declaration->serial &&
        shaders->vertexStateKey == stateKey) {
        hr = triton9EnsureInputLayout(device, shaders->vertexShader, declaration);
        if (SUCCEEDED(hr))
            *outShader = shaders->vertexShader;
        return hr;
    }
    hr = triton9BuildFixedVertexTokens(device, declaration, &tokens);
    if (FAILED(hr))
        return hr;
    shader = new (std::nothrow) Triton9Shader(Triton9ShaderStage::Vertex);
    if (!shader)
        return E_OUTOFMEMORY;
    try {
        shader->legacyTokens = std::move(tokens);
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
    return S_OK;
}

static HRESULT
triton9EnsureFixedPixelShader(TRITON9_DEVICE *device,
                              const Triton9Shader *vertexShader,
                              Triton9Shader **outShader)
{
    Triton9FixedFunctionShaders *shaders;
    Triton9Shader *shader;
    std::vector<UINT> tokens;
    UINT64 stateKey;
    HRESULT hr;

    if (!device || !vertexShader || !outShader)
        return E_INVALIDARG;
    hr = triton9BuildFixedPixelTokens(device, vertexShader, &tokens, &stateKey);
    if (FAILED(hr))
        return hr;
    shaders = triton9FixedShaders(device);
    if (!shaders)
        return E_OUTOFMEMORY;
    if (shaders->pixelShader && shaders->pixelStateKey == stateKey &&
        shaders->pixelVertexShader == vertexShader &&
        shaders->pixelVertexInstanceSerial == vertexShader->instanceSerial &&
        shaders->pixelVertexGeneration == vertexShader->generation) {
        *outShader = shaders->pixelShader;
        return S_OK;
    }
    shader = new (std::nothrow) Triton9Shader(Triton9ShaderStage::Pixel);
    if (!shader)
        return E_OUTOFMEMORY;
    try {
        shader->legacyTokens = std::move(tokens);
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
    return S_OK;
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

/* The caller holds shaderLock. */
static HRESULT
triton9ValidateStreamRange(TRITON9_DEVICE *device, const Triton9Shader *shader,
                           const Triton9VertexDeclaration *declaration,
                           UINT firstVertex, UINT vertexCount,
                           UINT instanceCount)
{
    if (!device || !shader || !declaration)
        return E_INVALIDARG;
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
    }
    return S_OK;
}

/* The caller holds shaderLock. */
static HRESULT
triton9PrepareDrawFailure(UINT stage, HRESULT hr)
{
    triton9DiagU32("TRITON9-DRAW-PREP-STAGE", stage);
    triton9DiagU32("TRITON9-DRAW-PREP-HR", (DWORD)hr);
    return hr;
}

/* The caller holds shaderLock. */
static HRESULT
triton9PrepareDraw(TRITON9_DEVICE *device)
{
    Triton9Shader *vertexShader;
    Triton9Shader *pixelShader;
    Triton9VertexDeclaration *declaration;
    BOOL vertexFallback = FALSE;
    BOOL pixelFallback = FALSE;
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
    if (!vertexShader) {
        hr = triton9EnsureFixedVertexShader(device, declaration, &vertexShader);
        if (FAILED(hr))
            return triton9PrepareDrawFailure(5, triton9RejectFixedFunction(
                hr, "neptune_d3d9: rejected unsupported fixed-function vertex state.\n"));
        vertexFallback = TRUE;
    }
    if (!pixelShader) {
        hr = triton9EnsureFixedPixelShader(device, vertexShader, &pixelShader);
        if (FAILED(hr))
            return triton9PrepareDrawFailure(6, triton9RejectFixedFunction(
                hr, "neptune_d3d9: rejected unsupported fixed-function pixel state.\n"));
        pixelFallback = TRUE;
    }
    if (!vertexFallback) {
        hr = triton9PrepareVertexShader(device, vertexShader, declaration);
        if (FAILED(hr))
            return triton9PrepareDrawFailure(7, hr);
    }
    if (!pixelFallback) {
        hr = triton9PreparePixelShader(device, pixelShader, vertexShader);
        if (FAILED(hr))
            return triton9PrepareDrawFailure(8, hr);
    }
    if (vertexFallback && !declaration->hasTransformedPosition) {
        hr = triton9UploadFixedVertexConstants(device);
        if (FAILED(hr))
            return triton9PrepareDrawFailure(9, hr);
    }
    if (pixelFallback) {
        hr = triton9UploadFixedPixelConstants(device);
        if (FAILED(hr))
            return triton9PrepareDrawFailure(10, hr);
    }
    if (!pixelFallback &&
        (device->renderStates[D3DDDIRS_ALPHATESTENABLE] ||
         device->renderStates[D3DDDIRS_FOGENABLE])) {
        hr = triton9UploadPixelExtensionConstants(device);
        if (FAILED(hr))
            return triton9PrepareDrawFailure(11, hr);
    }
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
    if (tokens)
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
        hr = triton9PrepareResourceForHostRead(device, resource);
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
    UINT value;

    if (!device || !args || args->Stream >= TRITON9_MAX_VERTEX_STREAMS ||
        !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    frequency = args->Divider;
    flags = frequency & flagsMask;
    value = frequency & kTriton9StreamFrequencyValueMask;
    if (frequency != 1 &&
        !((args->Stream == 0 && flags == D3DSTREAMSOURCE_INDEXEDDATA && value) ||
          (args->Stream != 0 && flags == D3DSTREAMSOURCE_INSTANCEDATA && value)))
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
        hr = triton9PrepareResourceForHostRead(device, resource);
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
    UINT instanceCount;
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
    hr = triton9PrepareDraw(device);
    vertexShader = static_cast<Triton9Shader *>(device->drawVertexShader);
    declaration = static_cast<Triton9VertexDeclaration *>(
        device->drawVertexDeclaration);
    if (SUCCEEDED(hr))
        hr = triton9GetInstanceCount(device, vertexShader, declaration,
                                     &instanceCount);
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
            if (instanceCount == 1)
                device->hostContext->DrawIndexed(drawCount, startIndex,
                                                 args->VStart);
            else
                device->hostContext->DrawIndexedInstanced(
                    drawCount, instanceCount, startIndex, args->VStart, 0);
        } else if (instanceCount == 1) {
            device->hostContext->Draw(vertexCount, args->VStart);
        } else {
            device->hostContext->DrawInstanced(vertexCount, instanceCount,
                                               args->VStart, 0);
        }
    }
    triton9RestoreTemporaryVertexStreams(device);
    if (expand)
        triton9RestoreIndexBuffer(device);
    if (SUCCEEDED(hr))
        hr = triton9CheckHostDevice(device);
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
    UINT64 indexEnd;
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
        hr = triton9PrepareDraw(device);
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
    hr = triton9PrepareDraw(device);
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
    hr = triton9PrepareDraw(device);
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
    if (tokens)
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
