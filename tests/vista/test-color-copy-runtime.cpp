/* SPDX-License-Identifier: MIT
 * Native GPU oracle for the actual extracted UMD color-copy helper.
 * CPU uploads/readbacks below are test input and independent pixel observation;
 * the production helper must use only DEFAULT GPU resources between them.
 * This fixture does not establish Vista application or window performance.
 */
#include <d3d11_1.h>
#include <dxgi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "triton-dxmt/tests/native/shaders_dxbc.h"

extern "C" HRESULT dxvk_d3d11_copy_color(void *, void *, void *);

template <typename T> struct Com {
    T *p = nullptr;
    Com() = default;
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    Com(Com &&other) noexcept : p(other.p) { other.p = nullptr; }
    ~Com() { if (p) p->Release(); }
    T *operator->() const { return p; }
    T **put() { if (p) throw std::runtime_error("COM output overwrite"); return &p; }
};

static unsigned checks, failures, cases, bridgeCalls;
static void require(HRESULT value, const char *expression) {
    if (FAILED(value)) {
        std::printf("[FAIL] %s: HRESULT=%08x\n", expression, unsigned(value));
        throw std::runtime_error(expression);
    }
}
#define HR(expression) require((expression), #expression)
static void expect(bool value, const std::string &label) {
    ++checks;
    if (!value) { ++failures; std::printf("[FAIL] %s\n", label.c_str()); }
}

struct TRITON_DEVICE {
    ID3D11Device1 *pDev1;
    ID3D11DeviceContext1 *pCtx1;
};
using PTRITON_DEVICE = TRITON_DEVICE *;
struct TRITON_RESOURCE {
    ID3D11Resource *pResource;
    DXGI_FORMAT HostFormat;
};
using PTRITON_RESOURCE = TRITON_RESOURCE *;

static HRESULT tritonSharedBridgeCopyColor(ID3D11DeviceContext1 *context,
                                           ID3D11RenderTargetView *destination,
                                           ID3D11ShaderResourceView *source) {
    Com<ID3D11Predicate> predicate;
    BOOL value = FALSE;
    context->GetPredication(predicate.put(), &value);
    expect(!predicate.p, "application predication disabled for conversion preparation");
    ++bridgeCalls;
    return dxvk_d3d11_copy_color(static_cast<ID3D11DeviceContext *>(context),
                               destination, source);
}

// GENERATED_PRODUCTION

using Pixel = std::array<unsigned char, 4>;
using Image = std::vector<Pixel>;

static bool rgba(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}
static bool xrgb(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B8G8R8X8_UNORM ||
           format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8X8_TYPELESS;
}
static bool srgb(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}
static float encodeSrgb(float value) {
    return value <= 0.0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}
static float decodeSrgb(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}
static DXGI_FORMAT storageFormat(DXGI_FORMAT format) {
    return rgba(format) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
}
static Pixel convert(Pixel pixel, DXGI_FORMAT source, DXGI_FORMAT destination) {
    if (rgba(source) != rgba(destination)) std::swap(pixel[0], pixel[2]);
    if (xrgb(source)) pixel[3] = 255;
    return pixel;
}
static UINT mipWidth(const D3D11_TEXTURE2D_DESC &desc, UINT subresource) {
    return std::max(1u, desc.Width >> (subresource % desc.MipLevels));
}
static UINT mipHeight(const D3D11_TEXTURE2D_DESC &desc, UINT subresource) {
    return std::max(1u, desc.Height >> (subresource % desc.MipLevels));
}
static D3D11_TEXTURE2D_DESC description(DXGI_FORMAT format, UINT width = 8,
                                        UINT height = 8, UINT mips = 1,
                                        UINT layers = 1, UINT samples = 1) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = mips; desc.ArraySize = layers;
    desc.Format = format; desc.SampleDesc.Count = samples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    return desc;
}
static std::vector<Image> patterns(const D3D11_TEXTURE2D_DESC &desc, unsigned seed) {
    std::vector<Image> result(desc.MipLevels * desc.ArraySize);
    for (UINT sub = 0; sub < result.size(); ++sub) {
        UINT width = mipWidth(desc, sub), height = mipHeight(desc, sub);
        auto &pixels = result[sub]; pixels.resize(width * height);
        for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
            // Asymmetric values include low/high SRGB bytes and non-opaque X8.
            pixels[y * width + x] = {static_cast<unsigned char>(seed + 19*x + 3*y + 7*sub),
                static_cast<unsigned char>(seed*3 + 5*x + 23*y + 11*sub),
                static_cast<unsigned char>(seed*7 + 29*x + 13*y + 17*sub),
                static_cast<unsigned char>(17 + 31*x + 7*y + 3*sub)};
        }
    }
    return result;
}

struct Test {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11Device1> device1;
    Com<ID3D11DeviceContext1> context1;
    TRITON_DEVICE triton = {};
    Test() {
        D3D_FEATURE_LEVEL level;
        HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, device.put(), &level, context.put()));
        HR(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(device1.put())));
        HR(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(context1.put())));
        triton = {device1.p, context1.p};
        Com<IDXGIDevice> dxgi; Com<IDXGIAdapter> adapter;
        HR(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(dxgi.put())));
        HR(dxgi->GetAdapter(adapter.put()));
        DXGI_ADAPTER_DESC desc = {}; HR(adapter->GetDesc(&desc));
        std::printf("ADAPTER vendor=%04x device=%04x feature=%04x name=", desc.VendorId, desc.DeviceId, level);
        for (unsigned i = 0; i < 128 && desc.Description[i]; ++i)
            std::putchar(desc.Description[i] < 128 ? char(desc.Description[i]) : '?');
        std::putchar('\n');
    }
    ~Test() { context->ClearState(); context->Flush(); }
    Com<ID3D11Texture2D> texture(const D3D11_TEXTURE2D_DESC &desc,
                                const std::vector<Image> *input = nullptr) {
        std::vector<D3D11_SUBRESOURCE_DATA> data;
        if (input) {
            for (UINT sub = 0; sub < input->size(); ++sub)
                data.push_back({(*input)[sub].data(), mipWidth(desc, sub) * 4, 0});
        }
        Com<ID3D11Texture2D> result;
        HR(device->CreateTexture2D(&desc, data.empty() ? nullptr : data.data(), result.put()));
        return result;
    }
    Image read(ID3D11Texture2D *texture, UINT subresource) {
        Com<ID3D11Predicate> saved; BOOL value = FALSE;
        context->GetPredication(saved.put(), &value);
        context->SetPredication(nullptr, FALSE);
        D3D11_TEXTURE2D_DESC desc; texture->GetDesc(&desc);
        UINT width = mipWidth(desc, subresource), height = mipHeight(desc, subresource);
        desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = storageFormat(desc.Format); // X8 must expose its physical A byte.
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> staging = this->texture(desc);
        context->CopySubresourceRegion(staging.p, 0, 0, 0, 0, texture, subresource, nullptr);
        D3D11_MAPPED_SUBRESOURCE map = {};
        HR(context->Map(staging.p, 0, D3D11_MAP_READ, 0, &map));
        Image result(width * height);
        for (UINT row = 0; row < height; ++row)
            std::memcpy(result.data() + row * width,
                        static_cast<const char *>(map.pData) + row * map.RowPitch, width * 4);
        context->Unmap(staging.p, 0);
        context->SetPredication(saved.p, value);
        return result;
    }
    void wait(ID3D11Asynchronous *query, void *output, UINT bytes) {
        auto start = std::chrono::steady_clock::now();
        for (;;) {
            HRESULT status = context->GetData(query, output, bytes, 0);
            if (status != S_FALSE) { HR(status); return; }
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(15))
                throw std::runtime_error("query timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

static void compare(const Image &observed, const Image &expected, const std::string &name,
                    unsigned tolerance = 0) {
    expect(observed.size() == expected.size(), name + " size");
    unsigned mismatches = 0;
    for (size_t i = 0; i < std::min(observed.size(), expected.size()); ++i) {
        bool equal = true;
        for (unsigned c = 0; c < 4; ++c)
            equal &= unsigned(std::abs(int(observed[i][c]) - int(expected[i][c]))) <= tolerance;
        if (!equal && mismatches++ < 4)
            std::printf("PIXEL %s index=%zu actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                name.c_str(), i, observed[i][0], observed[i][1], observed[i][2], observed[i][3],
                expected[i][0], expected[i][1], expected[i][2], expected[i][3]);
    }
    expect(mismatches == 0, name + " pixel bytes");
}

static void copyCase(Test &test, DXGI_FORMAT sourceFormat, DXGI_FORMAT destinationFormat,
                     bool crop) {
    test.context->ClearState();
    auto sourceDesc = description(sourceFormat, crop ? 32 : 8, crop ? 16 : 8, crop ? 4 : 1, crop ? 3 : 1);
    auto destinationDesc = description(destinationFormat, crop ? 32 : 8, crop ? 16 : 8, crop ? 4 : 1, crop ? 3 : 1);
    auto sourceData = patterns(sourceDesc, 3), expected = patterns(destinationDesc, 71);
    auto source = test.texture(sourceDesc, &sourceData), destination = test.texture(destinationDesc, &expected);
    UINT sourceSub = crop ? 10 : 0, destinationSub = crop ? 5 : 0;
    D3D11_BOX box = crop ? D3D11_BOX{1, 1, 0, 7, 3, 1} : D3D11_BOX{0, 0, 0, 8, 8, 1};
    UINT x = crop ? 5 : 0, y = crop ? 3 : 0;
    TRITON_RESOURCE src{source.p, storageFormat(sourceFormat)}, dst{destination.p, storageFormat(destinationFormat)};
    unsigned before = bridgeCalls;
    HR(tritonResourceCopyConverted(&test.triton, &dst, destinationSub, x, y, 0,
                                    &src, sourceSub, crop ? &box : nullptr, DXGI_FORMAT_UNKNOWN));
    expect(bridgeCalls == before + 1, "one real host color conversion");
    UINT sw = mipWidth(sourceDesc, sourceSub), dw = mipWidth(destinationDesc, destinationSub);
    for (UINT row = box.top; row < box.bottom; ++row) for (UINT col = box.left; col < box.right; ++col)
        expected[destinationSub][(y + row - box.top) * dw + x + col - box.left] =
            convert(sourceData[sourceSub][row * sw + col], sourceFormat, destinationFormat);
    std::string name = "format " + std::to_string(sourceFormat) + "->" +
                       std::to_string(destinationFormat) + (crop ? " mip-array-crop" : " full");
    for (UINT sub = 0; sub < expected.size(); ++sub)
        compare(test.read(destination.p, sub), expected[sub], name + " dst-sub=" + std::to_string(sub));
    compare(test.read(source.p, sourceSub), sourceData[sourceSub], name + " unchanged source");
    ++cases;
    std::printf("CASE %s\n", name.c_str());
}

struct Vertex { float position[2], uv[2], color[4]; };
struct Draw {
    Test &test;
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11InputLayout> layout;
    Com<ID3D11Buffer> vertices;
    Com<ID3D11RasterizerState> raster;
    explicit Draw(Test &test) : test(test) {
        HR(test.device->CreateVertexShader(dxbc_vs_pass, sizeof(dxbc_vs_pass), nullptr, vs.put()));
        HR(test.device->CreatePixelShader(dxbc_ps_color, sizeof(dxbc_ps_color), nullptr, ps.put()));
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        HR(test.device->CreateInputLayout(elements, 3, dxbc_vs_pass, sizeof(dxbc_vs_pass), layout.put()));
        D3D11_BUFFER_DESC buffer = {}; buffer.ByteWidth = 6 * sizeof(Vertex); buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        HR(test.device->CreateBuffer(&buffer, nullptr, vertices.put()));
        D3D11_RASTERIZER_DESC rasterDesc = {}; rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE; rasterDesc.DepthClipEnable = TRUE;
        rasterDesc.MultisampleEnable = TRUE;
        HR(test.device->CreateRasterizerState(&rasterDesc, raster.put()));
    }
    void color(const std::array<float, 4> &color) {
        Vertex data[6] = {{{-1,-1},{0,1},{}}, {{-1,1},{0,0},{}}, {{1,1},{1,0},{}},
                          {{-1,-1},{0,1},{}}, {{1,1},{1,0},{}}, {{1,-1},{1,1},{}}};
        for (auto &vertex : data) std::copy(color.begin(), color.end(), vertex.color);
        test.context->UpdateSubresource(vertices.p, 0, nullptr, data, 0, 0);
    }
    void bind(ID3D11RenderTargetView *target, UINT width, UINT height, UINT sampleMask = ~0u) {
        test.context->OMSetRenderTargets(1, &target, nullptr);
        test.context->OMSetBlendState(nullptr, nullptr, sampleMask);
        test.context->OMSetDepthStencilState(nullptr, 0);
        D3D11_VIEWPORT viewport = {0, 0, float(width), float(height), 0, 1};
        test.context->RSSetViewports(1, &viewport); test.context->RSSetState(raster.p);
        UINT stride = sizeof(Vertex), offset = 0;
        test.context->IASetVertexBuffers(0, 1, &vertices.p, &stride, &offset);
        test.context->IASetInputLayout(layout.p);
        test.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        test.context->VSSetShader(vs.p, nullptr, 0); test.context->PSSetShader(ps.p, nullptr, 0);
    }
};

static Com<ID3D11RenderTargetView> targetView(Test &test, ID3D11Texture2D *texture,
                                           DXGI_FORMAT format, bool msaa = false, UINT layer = 0) {
    Com<ID3D11RenderTargetView> result;
    D3D11_TEXTURE2D_DESC td; texture->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC desc = {}; desc.Format = format;
    if (msaa && td.ArraySize > 1) {
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
        desc.Texture2DMSArray.FirstArraySlice = layer; desc.Texture2DMSArray.ArraySize = 1;
    } else desc.ViewDimension = msaa ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    HR(test.device->CreateRenderTargetView(texture, &desc, result.put()));
    return result;
}

static void msaaCase(Test &test, Draw &draw, DXGI_FORMAT sourceFormat,
                     DXGI_FORMAT logicalResolve, DXGI_FORMAT hostFormat,
                     DXGI_FORMAT destinationFormat) {
    test.context->ClearState();
    DXGI_FORMAT physicalResolve = logicalResolve;
    // This oracle's format selection is explicit and does not call production.
    if (hostFormat == DXGI_FORMAT_R8G8B8A8_UNORM)
        physicalResolve = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    UINT quality = 0;
    HR(test.device->CheckMultisampleQualityLevels(physicalResolve, 4, &quality));
    if (!quality) throw std::runtime_error("required 4x color MSAA unavailable");
    auto srcDesc = description(sourceFormat, 8, 8, 1, 2, 4);
    srcDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    auto source = test.texture(srcDesc);
    DXGI_FORMAT renderFormat = sourceFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS ?
                                physicalResolve : sourceFormat;
    auto sourceView = targetView(test, source.p, renderFormat, true, 1);
    const std::array<float,4> colors[] = {{0.02f,0.18f,0.91f,0.10f}, {0.71f,0.03f,0.33f,0.30f},
                                        {0.35f,0.83f,0.07f,0.60f}, {0.96f,0.41f,0.54f,0.90f}};
    for (UINT sample = 0; sample < 4; ++sample) {
        draw.color(colors[sample]); draw.bind(sourceView.p, 8, 8, 1u << sample); test.context->Draw(6, 0);
    }
    test.context->OMSetRenderTargets(0, nullptr, nullptr);

    // Independent legal resolve reference: normalize the complete typed source
    // before resolving, then read encoded bytes. Distinct sample values catch
    // resolving SRGB storage as UNORM (or converting bytes through an SRGB view).
    auto typedDesc = srcDesc; typedDesc.Format = physicalResolve; typedDesc.BindFlags = 0;
    auto typed = test.texture(typedDesc);
    test.context->CopyResource(typed.p, source.p);
    auto referenceDesc = description(physicalResolve);
    auto reference = test.texture(referenceDesc);
    test.context->ResolveSubresource(reference.p, 0, typed.p, 1, physicalResolve);
    Image resolved = test.read(reference.p, 0);
    Pixel analytic = {};
    for (unsigned component = 0; component < 4; ++component) {
        float average = 0;
        for (const auto &color : colors) {
            float stored = srgb(renderFormat) && component < 3 ? encodeSrgb(color[component]) : color[component];
            stored = std::round(stored * 255.0f) / 255.0f;
            average += srgb(physicalResolve) && component < 3 ? decodeSrgb(stored) : stored;
        }
        average *= 0.25f;
        if (srgb(physicalResolve) && component < 3) average = encodeSrgb(average);
        analytic[component] = static_cast<unsigned char>(std::round(average * 255.0f));
    }
    if (!rgba(physicalResolve)) std::swap(analytic[0], analytic[2]);
    // The analytic result is independent of both helper and reference commands.
    // A two-byte tolerance accommodates implementation-dependent SRGB rounding;
    // helper-vs-reference comparison below remains byte-exact.
    compare(resolved, Image(64, analytic), "typed MSAA resolve analytic sample average", 2);
    auto destinationDesc = description(destinationFormat, 16, 16, 2, 2);
    auto expected = patterns(destinationDesc, 63);
    auto destination = test.texture(destinationDesc, &expected);
    TRITON_RESOURCE src{source.p, hostFormat}, dst{destination.p, storageFormat(destinationFormat)};
    D3D11_BOX box = {1, 2, 0, 7, 6, 1};
    HR(tritonResourceCopyConverted(&test.triton, &dst, 3, 1, 3, 0, &src, 1, &box, logicalResolve));
    for (UINT y = box.top; y < box.bottom; ++y) for (UINT x = box.left; x < box.right; ++x)
        expected[3][(3 + y - box.top) * 8 + 1 + x - box.left] =
            convert(resolved[y*8+x], physicalResolve, destinationFormat);
    std::string name = "MSAA source=" + std::to_string(sourceFormat) + " resolve=" +
                       std::to_string(logicalResolve) + " host=" + std::to_string(hostFormat);
    for (UINT sub = 0; sub < expected.size(); ++sub)
        compare(test.read(destination.p, sub), expected[sub], name + " sub=" + std::to_string(sub));
    ++cases; std::printf("CASE %s\n", name.c_str());
}

struct QueryValues {
    UINT64 samples = 0;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS pipeline = {};
};
static std::array<UINT64, 11> fields(const QueryValues &values) {
    const auto &p = values.pipeline;
    return {p.IAVertices,p.IAPrimitives,p.VSInvocations,p.GSInvocations,p.GSPrimitives,
            p.CInvocations,p.CPrimitives,p.PSInvocations,p.HSInvocations,p.DSInvocations,p.CSInvocations};
}
static void stateCases(Test &test, Draw &draw) {
    test.context->ClearState();
    auto appDesc = description(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16);
    appDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    auto appTarget = test.texture(appDesc);
    auto appView = targetView(test, appTarget.p, appDesc.Format);
    auto sourceDesc = description(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    auto destinationDesc = description(DXGI_FORMAT_B8G8R8A8_TYPELESS);
    auto sourceData = patterns(sourceDesc, 27), initial = patterns(destinationDesc, 101);
    auto source = test.texture(sourceDesc, &sourceData), destination = test.texture(destinationDesc, &initial);
    TRITON_RESOURCE src{source.p, DXGI_FORMAT_R8G8B8A8_UNORM}, dst{destination.p, DXGI_FORMAT_B8G8R8A8_UNORM};
    Image converted = sourceData[0];
    for (auto &p : converted) p = convert(p, sourceDesc.Format, destinationDesc.Format);
    auto copy = [&]() { HR(tritonResourceCopyConverted(&test.triton, &dst, 0, 0, 0, 0, &src, 0, nullptr, DXGI_FORMAT_UNKNOWN)); };

    auto query = [&](bool includeCopy, bool includeDraws) {
        test.context->ClearState(); draw.color({1, 0, 0, 1}); draw.bind(appView.p, 16, 16);
        const float black[] = {0, 0, 0, 0}; test.context->ClearRenderTargetView(appView.p, black);
        Com<ID3D11Query> occlusion, pipeline;
        D3D11_QUERY_DESC desc = {D3D11_QUERY_OCCLUSION, 0};
        HR(test.device->CreateQuery(&desc, occlusion.put())); desc.Query = D3D11_QUERY_PIPELINE_STATISTICS;
        HR(test.device->CreateQuery(&desc, pipeline.put()));
        test.context->Begin(occlusion.p); test.context->Begin(pipeline.p);
        if (includeDraws) test.context->Draw(6, 0);
        if (includeCopy) copy();
        if (includeDraws) test.context->Draw(6, 0); // Deliberately no rebind after private operation.
        test.context->End(pipeline.p); test.context->End(occlusion.p);
        QueryValues result;
        test.wait(occlusion.p, &result.samples, sizeof(result.samples));
        test.wait(pipeline.p, &result.pipeline, sizeof(result.pipeline));
        if (includeCopy) compare(test.read(destination.p, 0), converted, "copy inside active application queries");
        if (includeDraws) compare(test.read(appTarget.p, 0), Image(16*16, Pixel{255,0,0,255}), "application draw state after conversion");
        return result;
    };
    QueryValues baseline = query(false, true), withCopy = query(true, true), copyOnly = query(true, false);
    expect(baseline.samples == 512, "baseline occlusion observes two complete 16x16 draws");
    expect(baseline.pipeline.IAVertices == 12 && baseline.pipeline.IAPrimitives == 4,
           "baseline pipeline query observes real application primitives");
    expect(withCopy.samples == baseline.samples, "private copy adds no occlusion samples");
    expect(withCopy.pipeline.IAVertices == baseline.pipeline.IAVertices &&
           withCopy.pipeline.IAPrimitives == baseline.pipeline.IAPrimitives,
           "private copy adds no input-assembler vertices or primitives");
    // Splitting a render pass may discard the post-transform cache. Vulkan
    // permits different VS invocation counts for repeated identical vertices;
    // exact comparison here would mistake legitimate cache behavior for query
    // pollution. The copy-only case below still requires EVERY counter zero.
    expect(withCopy.pipeline.GSInvocations == 0 && withCopy.pipeline.GSPrimitives == 0 &&
           withCopy.pipeline.HSInvocations == 0 && withCopy.pipeline.DSInvocations == 0 &&
           withCopy.pipeline.CSInvocations == 0,
           "private copy adds no inactive shader-stage work");
    expect(copyOnly.samples == 0 && fields(copyOnly) == std::array<UINT64,11>{},
           "copy-only active queries remain zero");
    std::printf("QUERY baseline-samples=%llu copy-samples=%llu empty-samples=%llu ia=%llu/%llu\n",
        static_cast<unsigned long long>(baseline.samples), static_cast<unsigned long long>(withCopy.samples),
        static_cast<unsigned long long>(copyOnly.samples),
        static_cast<unsigned long long>(withCopy.pipeline.IAVertices),
        static_cast<unsigned long long>(withCopy.pipeline.IAPrimitives));
    cases += 3;

    for (bool truth : {false, true}) {
        test.context->ClearState(); draw.color({0, 1, 0, 1}); draw.bind(appView.p, 16, 16);
        Com<ID3D11Predicate> predicate;
        D3D11_QUERY_DESC desc = {D3D11_QUERY_OCCLUSION_PREDICATE, 0};
        HR(test.device->CreatePredicate(&desc, predicate.put()));
        test.context->Begin(predicate.p); if (truth) test.context->Draw(6, 0); test.context->End(predicate.p);
        BOOL observed = FALSE; test.wait(predicate.p, &observed, sizeof(observed));
        expect(bool(observed) == truth, "predicate has known GPU result");
        for (BOOL value : {FALSE, TRUE}) {
            test.context->SetPredication(nullptr, FALSE);
            test.context->UpdateSubresource(destination.p, 0, nullptr, initial[0].data(), 8*4, 0);
            test.context->SetPredication(predicate.p, value);
            unsigned before = bridgeCalls; copy();
            expect(bridgeCalls == before + 1, "preparation executes under either predicate outcome");
            Com<ID3D11Predicate> retained; BOOL retainedValue = FALSE;
            test.context->GetPredication(retained.put(), &retainedValue);
            expect(retained.p == predicate.p && retainedValue == value, "application predicate restored exactly");
            // D3D suppresses commands when predicate result equals the comparison value.
            bool execute = truth != bool(value);
            compare(test.read(destination.p, 0), execute ? converted : initial[0],
                    "predicated final copy result=" + std::to_string(truth) + " value=" + std::to_string(value));
            test.context->SetPredication(nullptr, FALSE);
            ++cases;
        }
    }
    test.context->ClearState();
}

int main() {
    std::printf("HELPER-SHA256 GENERATED_HELPER_SHA256\n");
    try {
        Test test;
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_TYPELESS,
            DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8X8_TYPELESS};
        for (auto source : formats) for (auto destination : formats) copyCase(test, source, destination, false);
        copyCase(test, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_TYPELESS, true);
        copyCase(test, DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, true);
        copyCase(test, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_TYPELESS, true);
        copyCase(test, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, true);
        Draw draw(test);
        msaaCase(test, draw, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                 DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_B8G8R8A8_UNORM);
        msaaCase(test, draw, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
                 DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM);
        msaaCase(test, draw, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                 DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        msaaCase(test, draw, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                 DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_B8G8R8A8_UNORM);
        msaaCase(test, draw, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        stateCases(test, draw);
        HR(test.device->GetDeviceRemovedReason());
    } catch (const std::exception &error) {
        ++failures; std::printf("[FAIL] exception: %s\n", error.what());
    }
    std::printf("COLOR-COPY-SUMMARY checks=%u failures=%u cases=%u bridge-calls=%u\n",
                checks, failures, cases, bridgeCalls);
    return failures ? 1 : 0;
}
