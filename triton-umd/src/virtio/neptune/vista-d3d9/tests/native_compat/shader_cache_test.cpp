// Actual cache/conversion orchestration and real ShaderConv metadata;
// backend compiler/COM allocation are injected HRESULT endpoints.
#include <d3d9.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>
#include "triton9_fixed.h"
#define D3DHAL_SAMPLER_MAXSAMP 16
#include <ShaderConv.h>

#define CHECK(value, message) do { if (!(value)) { \
    std::fprintf(stderr, "FAIL: %s at %d\n", message, __LINE__); std::exit(1); } } while (0)
static long failNew = -1;
static bool injectedNew;
void *operator new(std::size_t size)
{
    if (failNew == 0) { failNew = -1; injectedNew = true; throw std::bad_alloc(); }
    if (failNew > 0) --failNew;
    void *p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{ try { return ::operator new(size); } catch (...) { return nullptr; } }
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{ try { return ::operator new(size); } catch (...) { return nullptr; } }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
static bool failHeap;
static unsigned heapLive, comLive, conversionCalls;
static void *testHeapAlloc(HANDLE, DWORD flags, size_t size)
{
    if (failHeap) { failHeap = false; return nullptr; }
    void *p = flags & HEAP_ZERO_MEMORY ? std::calloc(1, size) : std::malloc(size);
    if (p) ++heapLive;
    return p;
}
static BOOL testHeapFree(HANDLE, DWORD, void *p)
{ if (p) { CHECK(heapLive, "heap ownership underflow"); --heapLive; std::free(p); } return TRUE; }
#define HeapAlloc testHeapAlloc
#define HeapFree testHeapFree
struct Com {
    unsigned references = 1;
    Com() { ++comLive; }
    void AddRef() { ++references; }
    void Release() { CHECK(references, "COM reference underflow"); if (!--references) { --comLive; delete this; } }
};
using ID3D11VertexShader = Com;
using ID3D11PixelShader = Com;
using ID3D11GeometryShader = Com;
using ID3D11InputLayout = Com;
using D3DDDIVERTEXELEMENT = D3DVERTEXELEMENT9;
enum class Triton9ShaderStage { Vertex, Pixel };
static volatile LONG gTriton9ShaderSerial;
/* CLASSES */
struct TRITON9_CONSTANT_BUFFER { BYTE *data = nullptr; UINT byteCount = 0; BOOL dirty = FALSE; Com *hostBuffer = nullptr; };
#define TRITON9_MAX_CONSTANT_BUFFERS 5
struct Host {
    HRESULT status = S_OK;
    HRESULT CreateVertexShader(const void *, size_t, void *, Com **out) {
        *out = nullptr; if (FAILED(status)) return status;
        *out = new (std::nothrow) Com; return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT CreatePixelShader(const void *p, size_t size, void *link, Com **out)
    { return CreateVertexShader(p, size, link, out); }
};
struct TRITON9_DEVICE {
    Host *hostDevice;
    void *fixedFunctionShaders = nullptr, *drawVertexShader = nullptr, *drawVertexDeclaration = nullptr;
    TRITON9_CONSTANT_BUFFER fixedVertexFloatConstants, fixedPixelFloatConstants;
    TRITON9_CONSTANT_BUFFER vertexConstants[5], pixelConstants[5];
    Triton9Fixed::State testState;
};
struct TRITON_DXBC_SIGNATURE {
    const char *semanticName;
    UINT semanticIndex, systemValue, registerIdx;
    BYTE mask;
    UINT componentType;
};
#define D3D_NAME_POSITION 1
#define D3D_NAME_IS_FRONT_FACE 9
#define D3DDDIERR_NOTAVAILABLE ((HRESULT)0x8876086a)
static HRESULT triton9BuildVsInputSignatures(const Triton9VertexDeclaration *, const ShaderConv::VSInputDecls &, std::vector<TRITON_DXBC_SIGNATURE> *) { return S_OK; }
static HRESULT triton9BuildVsOutputSignatures(const ShaderConv::VSOutputDecls &, std::vector<TRITON_DXBC_SIGNATURE> *) { return S_OK; }
static const char *triton9LinkageSemanticName(UINT) { return "TEST"; }
static UINT triton9SemanticSystemValue(UINT) { return 0; }
static void *tritonBuildDxbc(const UINT *p, size_t size, const TRITON_DXBC_SIGNATURE *, UINT,
                           const TRITON_DXBC_SIGNATURE *, UINT, const void *, UINT, size_t, size_t *outSize)
{
    void *result = HeapAlloc(nullptr, 0, size);
    *outSize = result ? size : 0;
    if (result) std::memcpy(result, p, size);
    return result;
}
namespace ShaderConv {
ShaderConverterAPI::~ShaderConverterAPI() {}
HRESULT ShaderConverterAPI::ConvertShader(ConvertShaderArgs &args)
{
    ++conversionCalls;
    args.convertedByteCode.m_pByteCode = HeapAlloc(nullptr, 0, 4);
    args.convertedByteCode.m_byteCodeSize = args.convertedByteCode.m_pByteCode ? 4 : 0;
    if (!args.convertedByteCode.m_pByteCode) return E_OUTOFMEMORY;
    std::memset(args.convertedByteCode.m_pByteCode, 0x5a, 4);
    return S_OK;
}
HRESULT ShaderConverterAPI::ConvertTLShader(ConvertTLShaderArgs &args)
{
    ++conversionCalls;
    args.convertedByteCode.m_pByteCode = HeapAlloc(nullptr, 0, 4);
    args.convertedByteCode.m_byteCodeSize = args.convertedByteCode.m_pByteCode ? 4 : 0;
    if (!args.convertedByteCode.m_pByteCode) return E_OUTOFMEMORY;
    std::memset(args.convertedByteCode.m_pByteCode, 0xa5, 4);
    return S_OK;
}
void ShaderConverterAPI::CleanUpConvertedShader(ByteCode &code)
{ HeapFree(nullptr, 0, code.m_pByteCode); code = {}; }
}
/* CACHE */
/* CONVERT */
static HRESULT layoutStatus;
static unsigned layoutCalls;
static HRESULT triton9EnsureInputLayout(TRITON9_DEVICE *, Triton9Shader *, Triton9VertexDeclaration *)
{ ++layoutCalls; return layoutStatus; }
static HRESULT triton9EnsureTransformedVertexShader(TRITON9_DEVICE *, Triton9VertexDeclaration *, Triton9Shader **)
{ return E_NOTIMPL; }
static void triton9FixedSnapshot(const TRITON9_DEVICE *device, const Triton9VertexDeclaration *, const Triton9Shader *, Triton9Fixed::State &out)
{ out = device->testState; }
/* FIXED */

static void base(Triton9Fixed::State &state)
{
    state = {};
    for (auto &matrix : state.world) for (unsigned i = 0; i < 4; ++i) matrix.m[i][i] = 1;
    for (unsigned i = 0; i < 4; ++i) state.view.m[i][i] = state.projection.m[i][i] = 1;
    state.inputs = {{D3DDECLUSAGE_POSITION,0,0,4},{D3DDECLUSAGE_COLOR,0,1,4}};
    state.render[D3DRS_POINTSIZE] = state.render[D3DRS_POINTSIZE_MIN] = 0x3f800000;
    state.render[D3DRS_POINTSIZE_MAX] = 0x42800000;
}

int main()
{
    // A declaration vector copy is a real C++ allocation in conversion.
    // Every injected allocation failure must become an HRESULT, not escape
    // across the DDI while its caller still owns shaderLock.
    for (unsigned transformed = 0; transformed < 2; ++transformed) {
        for (long index = 0; index < 8; ++index) {
            Host host; TRITON9_DEVICE device{&host};
            Triton9VertexDeclaration declaration; declaration.serial = 1;
            declaration.hasTransformedPosition = transformed;
            declaration.inputDecls.AddDecl(D3DDECLUSAGE_POSITION,0,0);
            Triton9Shader shader(Triton9ShaderStage::Vertex); shader.legacyTokens = {1};
            failNew = index; injectedNew = false;
            HRESULT hr = S_OK; bool escaped = false;
            try { hr = transformed ? triton9ConvertTransformedVertexShader(&device,&shader,&declaration)
                                   : triton9ConvertShader(&device,&shader,&declaration,nullptr); }
            catch (const std::bad_alloc &) { escaped = true; }
            failNew = -1;
            CHECK(!escaped, "conversion allocation exception escaped");
            CHECK(!injectedNew || FAILED(hr) || shader.vertexShader,
                  "conversion fault must fail or retain a complete shader");
            hr = transformed ? triton9ConvertTransformedVertexShader(&device,&shader,&declaration)
                             : triton9ConvertShader(&device,&shader,&declaration,nullptr);
            CHECK(SUCCEEDED(hr) && shader.vertexShader && shader.dxbc, "conversion retry after OOM");
        }
    }
    CHECK(!heapLive && !comLive, "conversion OOM sweep ownership");
    {
        Host host; TRITON9_DEVICE device{&host};
        Triton9VertexDeclaration declaration; declaration.serial = 1;
        Triton9Shader vertex(Triton9ShaderStage::Vertex), pixel(Triton9ShaderStage::Pixel);
        vertex.legacyTokens = pixel.legacyTokens = {1};
        declaration.inputDecls.AddDecl(D3DDECLUSAGE_POSITION,0,0);
        CHECK(SUCCEEDED(triton9ConvertShader(&device,&vertex,&declaration,nullptr)), "initial cached VS");
        CHECK(vertex.variants.size() == 1 && vertex.vertexShader->references == 2, "cache owns host reference");
        void *oldBytes = vertex.dxbc; Com *oldShader = vertex.vertexShader;
        failHeap = true;
        CHECK(triton9RestoreShaderVariant(&vertex,&declaration,nullptr) == E_OUTOFMEMORY &&
              vertex.dxbc == oldBytes && vertex.vertexShader == oldShader, "restore allocation failure preserves active variant");
        failNew = 0; injectedNew = false;
        CHECK(triton9RestoreShaderVariant(&vertex,&declaration,nullptr) == E_OUTOFMEMORY &&
              injectedNew && vertex.dxbc == oldBytes && vertex.vertexShader == oldShader,
              "restore metadata OOM preserves active variant");
        failNew = -1;
        CHECK(SUCCEEDED(triton9RestoreShaderVariant(&vertex,&declaration,nullptr)), "restore retry");
        ++declaration.serial;
        CHECK(triton9RestoreShaderVariant(&vertex,&declaration,nullptr) == S_FALSE, "declaration address reuse serial");
        CHECK(SUCCEEDED(triton9ConvertShader(&device,&vertex,&declaration,nullptr)), "new declaration instance");
        CHECK(SUCCEEDED(triton9ConvertShader(&device,&pixel,nullptr,&vertex)), "initial linked PS");
        CHECK(SUCCEEDED(triton9RestoreShaderVariant(&pixel,nullptr,&vertex)), "PS linkage cache hit");
        ++vertex.instanceSerial;
        CHECK(triton9RestoreShaderVariant(&pixel,nullptr,&vertex) == S_FALSE, "VS address reuse serial");
        --vertex.instanceSerial; ++vertex.generation;
        CHECK(triton9RestoreShaderVariant(&pixel,nullptr,&vertex) == S_FALSE, "VS generation invalidation");
        --vertex.generation;
        for (unsigned i = 0; i < 12; ++i) {
            vertex.rasterStates.TCIMapping = i;
            CHECK(SUCCEEDED(triton9ConvertShader(&device,&vertex,&declaration,nullptr)), "cache eviction conversion");
        }
        CHECK(vertex.variants.size() == 8, "bounded variant eviction");
        vertex.rasterStates.TCIMapping = 0;
        CHECK(triton9RestoreShaderVariant(&vertex,&declaration,nullptr) == S_FALSE, "evicted variant misses");
        vertex.rasterStates.TCIMapping = 11;
        CHECK(SUCCEEDED(triton9RestoreShaderVariant(&vertex,&declaration,nullptr)), "last retained variant restores");
    }
    CHECK(!heapLive && !comLive, "variant ownership after teardown");
    // Cover fixed snapshots, generator vectors, shader allocation, cache
    // records, and snapshot commit with real C++ allocation failures.
    for (unsigned pixelStage = 0; pixelStage < 2; ++pixelStage) {
        unsigned faults = 0;
        for (long index = 0; index < 48; ++index) {
            Host host; TRITON9_DEVICE device{&host}; base(device.testState);
            Triton9VertexDeclaration declaration; declaration.serial = 1;
            declaration.inputDecls.AddDecl(D3DDECLUSAGE_POSITION,0,0);
            Triton9Shader linkage(Triton9ShaderStage::Vertex), *result = nullptr;
            failNew = index; injectedNew = false;
            HRESULT hr = S_OK; bool escaped = false;
            try { hr = pixelStage ? triton9EnsureFixedPixelShader(&device,&linkage,&result)
                                  : triton9EnsureFixedVertexShader(&device,&declaration,&result); }
            catch (const std::bad_alloc &) { escaped = true; }
            failNew = -1;
            faults += injectedNew;
            CHECK(!escaped, "fixed shader allocation exception escaped");
            CHECK(FAILED(hr) || (result && result->dxbc), "fixed success retains a complete shader");
            hr = pixelStage ? triton9EnsureFixedPixelShader(&device,&linkage,&result)
                            : triton9EnsureFixedVertexShader(&device,&declaration,&result);
            CHECK(SUCCEEDED(hr) && result && result->dxbc, "fixed shader retry after OOM");
            triton9ReleaseFixedFunctionShaders(&device); triton9ReleaseConstantBuffers(&device);
            CHECK(!heapLive && !comLive, "fixed OOM sweep ownership");
        }
        CHECK(faults >= 6, "fixed OOM sweep exercised multiple allocation sites");
    }
    {
        Host host; TRITON9_DEVICE device{&host}; base(device.testState);
        Triton9VertexDeclaration declaration; declaration.serial = 20;
        declaration.inputDecls.AddDecl(D3DDECLUSAGE_POSITION,0,0);
        Triton9Shader *vertex = nullptr, *pixel = nullptr;
        CHECK(SUCCEEDED(triton9EnsureFixedVertexShader(&device,&declaration,&vertex)), "fixed VS initial");
        CHECK(SUCCEEDED(triton9EnsureFixedPixelShader(&device,vertex,&pixel)), "fixed PS initial");
        unsigned before = conversionCalls;
        CHECK(SUCCEEDED(triton9EnsureFixedVertexShader(&device,&declaration,&vertex)) &&
              SUCCEEDED(triton9EnsureFixedPixelShader(&device,vertex,&pixel)) &&
              conversionCalls == before, "unchanged fixed snapshots reuse shaders");
        device.testState.world[0].m[3][0] = .25f;
        CHECK(SUCCEEDED(triton9EnsureFixedVertexShader(&device,&declaration,&vertex)) &&
              conversionCalls == before, "constant-only change reuses token variant");
        device.testState.stages[0].colorOp = D3DTOP_SELECTARG1;
        device.testState.stages[0].colorArg[1] = D3DTA_TFACTOR;
        host.status = E_OUTOFMEMORY;
        CHECK(FAILED(triton9EnsureFixedPixelShader(&device,vertex,&pixel)), "failed fixed replacement");
        auto *cache = static_cast<Triton9FixedFunctionShaders *>(device.fixedFunctionShaders);
        CHECK(!cache->pixelSnapshot, "failed rebuild invalidates prior snapshot");
        host.status = S_OK;
        CHECK(SUCCEEDED(triton9EnsureFixedPixelShader(&device,vertex,&pixel)), "failed fixed replacement retry");
        device.drawVertexShader = vertex; device.drawVertexDeclaration = &declaration;
        cache->geometryShader = new Com;
        cache->geometryLinkage = new Triton9Shader(Triton9ShaderStage::Vertex);
        triton9ReleaseFixedFunctionShaders(&device);
        CHECK(!device.fixedFunctionShaders && !device.drawVertexShader && !device.drawVertexDeclaration,
              "fixed reset clears draw linkage");
        triton9ReleaseFixedFunctionShaders(&device);
        triton9ReleaseConstantBuffers(&device);
        triton9ReleaseConstantBuffers(&device);
        CHECK(!heapLive && !comLive, "idempotent fixed/constant reset releases ownership");
        CHECK(SUCCEEDED(triton9EnsureFixedVertexShader(&device,&declaration,&vertex)), "fixed shader recreation after reset");
        triton9ReleaseFixedFunctionShaders(&device); triton9ReleaseConstantBuffers(&device);
    }
    CHECK(!heapLive && !comLive, "final ownership");
    puts("Production shader cache/reset/OOM checks passed (ASan/UBSan)");
}
