/* SPDX-License-Identifier: MIT
 * Native pixel, query and state oracle for Neptune's GPU color conversion.
 * CPU uploads/readbacks are only fixture setup and observation, never part
 * of the production conversion under test. */
#define TRITON_LINUX_D3D11_NO_MAIN
#include "../linux-d3d11-test.cpp"
#include <dxvk_neptune.h>
#include <array>
#include <algorithm>

static bool dropCopy = false;
static bool queryPollution = false;
static constexpr UINT Width = 17, Height = 19;

static HRESULT copyColor(Ctx& c, ID3D11RenderTargetView* dst,
                         ID3D11ShaderResourceView* src) {
  return dropCopy ? S_OK : dxvk_d3d11_copy_color(c.ctx, dst, src);
}

static bool isBgra(DXGI_FORMAT f) {
  return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM;
}

static std::array<uint8_t, 4> pattern(unsigned i) {
  return {uint8_t(i), uint8_t(i * 73 + 17), uint8_t(i * 139 + 91),
          uint8_t(i * 47 + 11)};
}

static std::vector<uint8_t> pixels(DXGI_FORMAT format) {
  std::vector<uint8_t> out(Width * Height * 4);
  for (unsigned i = 0; i < Width * Height; i++) {
    auto p = pattern(i);
    if (isBgra(format)) std::swap(p[0], p[2]);
    std::copy(p.begin(), p.end(), out.begin() + i * 4);
  }
  return out;
}

struct Scratch {
  Com<ID3D11Texture2D> texture;
  Com<ID3D11RenderTargetView> rtv;
  Com<ID3D11ShaderResourceView> srv;
  bool valid = false;

  Scratch(Ctx& c, DXGI_FORMAT format, const void* initial = nullptr,
          UINT width = Width, UINT height = Height, UINT mips = 1,
          UINT layers = 1, UINT samples = 1, UINT misc = 0,
          D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu = 0,
          UINT bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) {
    const char* T = "color_copy_setup";
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = width; d.Height = height; d.MipLevels = mips; d.ArraySize = layers;
    d.Format = format; d.SampleDesc.Count = samples; d.BindFlags = bind;
    d.MiscFlags = misc; d.Usage = usage; d.CPUAccessFlags = cpu;
    D3D11_SUBRESOURCE_DATA data = {initial, width * 4, 0};
    CHECK_HR(T, c.device->CreateTexture2D(&d, initial ? &data : nullptr, &texture));
    if (bind & D3D11_BIND_RENDER_TARGET)
      CHECK_HR(T, c.device->CreateRenderTargetView(texture, nullptr, &rtv));
    if (bind & D3D11_BIND_SHADER_RESOURCE)
      CHECK_HR(T, c.device->CreateShaderResourceView(texture, nullptr, &srv));
    valid = true;
  }
};

static bool exactPixels(Ctx& c, ID3D11Texture2D* texture,
                        DXGI_FORMAT source, DXGI_FORMAT destination) {
  auto actual = readback_tex2d(c, texture, 0, 4);
  if (actual.size() != Width * Height * 4) return false;
  for (unsigned i = 0; i < Width * Height; i++) {
    auto expected = pattern(i);
    if (source == DXGI_FORMAT_B8G8R8X8_UNORM) expected[3] = 255;
    if (isBgra(destination)) std::swap(expected[0], expected[2]);
    for (unsigned channel = 0; channel < 4; channel++) {
      // The X byte of an X8 destination is not an application color channel.
      // Callers that must preserve its physical byte use a BGRA scratch RTV.
      if (channel == 3 && destination == DXGI_FORMAT_B8G8R8X8_UNORM) continue;
      if (actual[i * 4 + channel] != expected[channel]) {
        emit("[PIXEL] src=%u dst=%u pixel=%u channel=%u got=%u expected=%u",
             unsigned(source), unsigned(destination), i, channel,
             unsigned(actual[i * 4 + channel]), unsigned(expected[channel]));
        return false;
      }
    }
  }
  return true;
}

static void conversions(Ctx& c) {
  const char* T = "color_copy_pixels";
  c.ctx->ClearState();
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM};
  for (auto source : formats) for (auto destination : formats) {
    auto data = pixels(source);
    Scratch src(c, source, data.data()), dst(c, destination);
    CHECK(T, src.valid && dst.valid, return);
    const float sentinel[] = {0.125f, 0.25f, 0.75f, 0.5f};
    c.ctx->ClearRenderTargetView(dst.rtv, sentinel);
    CHECK_HR(T, copyColor(c, dst.rtv, src.srv));
    // Every 8-bit code occurs in each source channel, including dark and
    // midrange SRGB encodings. UNORM views must preserve those exact bytes.
    CHECK(T, exactPixels(c, dst.texture, source, destination));
    CHECK(T, exactPixels(c, src.texture, source, source));
  }

  auto data = pixels(DXGI_FORMAT_B8G8R8X8_UNORM);
  Scratch dst(c, DXGI_FORMAT_R8G8B8A8_UNORM);
  CHECK(T, dst.valid, return);
  {
    Scratch src(c, DXGI_FORMAT_B8G8R8X8_UNORM, data.data());
    CHECK(T, src.valid, return);
    CHECK_HR(T, copyColor(c, dst.rtv, src.srv));
    // Drop all source COM objects before the CS worker/GPU need the data.
  }
  dst.rtv.p->Release(); dst.rtv.p = nullptr;
  CHECK(T, exactPixels(c, dst.texture, DXGI_FORMAT_B8G8R8X8_UNORM,
                      DXGI_FORMAT_R8G8B8A8_UNORM));
}

struct DrawState {
  Scratch target;
  Com<ID3D11Buffer> vertices;
  Com<ID3D11VertexShader> vs;
  Com<ID3D11PixelShader> ps;
  Com<ID3D11InputLayout> layout;
  Com<ID3D11RasterizerState> raster;
  Com<ID3D11SamplerState> sampler;
  bool valid = false;

  DrawState(Ctx& c) : target(c, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 16, 16) {
    const char* T = "color_copy_draw_setup";
    CHECK(T, target.valid, return);
    CHECK_HR(T, c.device->CreateVertexShader(dxbc_vs_pass, sizeof(dxbc_vs_pass), nullptr, &vs));
    CHECK_HR(T, c.device->CreatePixelShader(dxbc_ps_color, sizeof(dxbc_ps_color), nullptr, &ps));
    CHECK_HR(T, c.device->CreateInputLayout(kLayout, 3, dxbc_vs_pass, sizeof(dxbc_vs_pass), &layout));
    Vertex v[12];
    std::copy(std::begin(kQuadFull), std::end(kQuadFull), v);
    std::copy(std::begin(kQuadFull), std::end(kQuadFull), v + 6);
    // Distinct vertex records and outputs for the two draws avoid requesting
    // exactly the same post-transform results twice (blue, then red).
    for (unsigned i = 0; i < 12; i++) {
      v[i].color[0] = i >= 6; v[i].color[1] = 0;
      v[i].color[2] = i < 6; v[i].color[3] = 1;
    }
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(v); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data = {v, 0, 0};
    CHECK_HR(T, c.device->CreateBuffer(&bd, &data, &vertices));
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    CHECK_HR(T, c.device->CreateRasterizerState(&rd, &raster));
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK_HR(T, c.device->CreateSamplerState(&sd, &sampler));
    valid = true;
  }

  void bind(Ctx& c, ID3D11ShaderResourceView* source) {
    c.ctx->OMSetRenderTargets(1, &target.rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, 16, 16, 0, 1}; c.ctx->RSSetViewports(1, &vp);
    c.ctx->RSSetState(raster);
    UINT stride = sizeof(Vertex), offset = 0;
    c.ctx->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
    c.ctx->IASetInputLayout(layout);
    c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c.ctx->VSSetShader(vs, nullptr, 0); c.ctx->PSSetShader(ps, nullptr, 0);
    c.ctx->PSSetSamplers(0, 1, &sampler); c.ctx->PSSetShaderResources(0, 1, &source);
  }

  bool unchanged(Ctx& c, ID3D11ShaderResourceView* source) {
    Com<ID3D11RenderTargetView> r; Com<ID3D11DepthStencilView> d;
    Com<ID3D11VertexShader> v; Com<ID3D11PixelShader> p;
    Com<ID3D11InputLayout> l; Com<ID3D11Buffer> b;
    Com<ID3D11RasterizerState> rs; Com<ID3D11SamplerState> s;
    Com<ID3D11ShaderResourceView> t;
    UINT stride = 0, offset = 1, count = 1; D3D11_VIEWPORT vp = {};
    D3D11_PRIMITIVE_TOPOLOGY topology;
    c.ctx->OMGetRenderTargets(1, &r, &d);
    c.ctx->VSGetShader(&v, nullptr, nullptr); c.ctx->PSGetShader(&p, nullptr, nullptr);
    c.ctx->IAGetInputLayout(&l); c.ctx->IAGetVertexBuffers(0, 1, &b, &stride, &offset);
    c.ctx->IAGetPrimitiveTopology(&topology); c.ctx->RSGetState(&rs);
    c.ctx->RSGetViewports(&count, &vp); c.ctx->PSGetSamplers(0, 1, &s);
    c.ctx->PSGetShaderResources(0, 1, &t);
    return r.p == target.rtv.p && !d.p && v.p == vs.p && p.p == ps.p
        && l.p == layout.p && b.p == vertices.p && stride == sizeof(Vertex) && !offset
        && topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && rs.p == raster.p
        && count == 1 && !vp.TopLeftX && !vp.TopLeftY && vp.Width == 16
        && vp.Height == 16 && !vp.MinDepth && vp.MaxDepth == 1
        && s.p == sampler.p && t.p == source;
  }
};

template<typename Data>
static bool queryData(Ctx& c, ID3D11Query* query, Data& result) {
  HRESULT hr = S_FALSE;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (hr == S_FALSE && std::chrono::steady_clock::now() < deadline) {
    hr = c.ctx->GetData(query, &result, sizeof(result), 0);
    if (hr == S_FALSE) usleep(100);
  }
  return hr == S_OK;
}

static void queriesAndState(Ctx& c) {
  const char* T = "color_copy_queries";
  c.ctx->ClearState(); DrawState draw(c);
  auto data = pixels(DXGI_FORMAT_B8G8R8X8_UNORM);
  Scratch src(c, DXGI_FORMAT_B8G8R8X8_UNORM, data.data());
  Scratch dst(c, DXGI_FORMAT_R8G8B8A8_UNORM);
  Scratch spill(c, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 16, 16);
  CHECK(T, draw.valid && src.valid && dst.valid && spill.valid, return);
  std::array<D3D11_QUERY_DATA_PIPELINE_STATISTICS, 4> stats = {};
  std::array<UINT64, 4> samples = {};
  // Baseline with two app draws, the same draws split by the meta operation,
  // the operation alone, and an ordinary GPU copy that splits rendering.
  // X8 source alpha forces the meta draw path. The ordinary copy preserves
  // the first draw's blue output before the second draw overwrites it red.
  for (unsigned mode = 0; mode < 4; mode++) {
    Com<ID3D11Query> occlusion, pipeline;
    D3D11_QUERY_DESC q = {D3D11_QUERY_OCCLUSION, 0};
    CHECK_HR(T, c.device->CreateQuery(&q, &occlusion));
    q.Query = D3D11_QUERY_PIPELINE_STATISTICS;
    CHECK_HR(T, c.device->CreateQuery(&q, &pipeline));
    const float green[] = {0, 1, 0, 1}; c.ctx->ClearRenderTargetView(draw.target.rtv, green);
    draw.bind(c, src.srv);
    c.ctx->Begin(occlusion); c.ctx->Begin(pipeline);
    if (mode != 2) c.ctx->Draw(6, 0);
    if (mode == 1 || mode == 2) {
      CHECK_HR(T, copyColor(c, dst.rtv, src.srv));
      if (queryPollution) c.ctx->Draw(3, 0);
      CHECK(T, draw.unchanged(c, src.srv));
    } else if (mode == 3) {
      c.ctx->CopyResource(spill.texture, draw.target.texture);
      CHECK(T, draw.unchanged(c, src.srv));
    }
    if (mode != 2) c.ctx->Draw(6, 6);
    c.ctx->End(pipeline); c.ctx->End(occlusion);
    CHECK(T, queryData(c, occlusion, samples[mode]), return);
    CHECK(T, queryData(c, pipeline, stats[mode]), return);
    auto image = readback_tex2d(c, draw.target.texture, 0, 4);
    CHECK(T, image.size() == 16 * 16 * 4, return);
    CHECK(T, px_eq(image, 16, 8, 8, mode == 2 ? 0 : 255,
                   mode == 2 ? 255 : 0, 0, 255, 0));
    if (mode == 3) {
      auto first = readback_tex2d(c, spill.texture, 0, 4);
      CHECK(T, first.size() == 16 * 16 * 4, return);
      CHECK(T, px_eq(first, 16, 8, 8, 0, 0, 255, 255, 0));
    }
    emit("[QUERY] mode=%u samples=%" PRIu64 " vertices=%" PRIu64
         " primitives=%" PRIu64 " VS=%" PRIu64 " PS=%" PRIu64
         " GS=%" PRIu64 " GSPrimitives=%" PRIu64 " C=%" PRIu64
         " CPrimitives=%" PRIu64 " HS=%" PRIu64 " DS=%" PRIu64 " CS=%" PRIu64,
         mode, uint64_t(samples[mode]), uint64_t(stats[mode].IAVertices),
         uint64_t(stats[mode].IAPrimitives), uint64_t(stats[mode].VSInvocations),
         uint64_t(stats[mode].PSInvocations), uint64_t(stats[mode].GSInvocations),
         uint64_t(stats[mode].GSPrimitives), uint64_t(stats[mode].CInvocations),
         uint64_t(stats[mode].CPrimitives), uint64_t(stats[mode].HSInvocations),
         uint64_t(stats[mode].DSInvocations), uint64_t(stats[mode].CSInvocations));
    if (mode != 2) {
      CHECK(T, samples[mode] == 512);
      CHECK(T, stats[mode].IAVertices == 12 && stats[mode].IAPrimitives == 4);
      CHECK(T, stats[mode].VSInvocations > 0 && stats[mode].PSInvocations >= samples[mode]);
      CHECK(T, !stats[mode].HSInvocations && !stats[mode].DSInvocations && !stats[mode].CSInvocations);
    }
  }
  // Do not compare whole app-draw statistics structs: shader-result reuse,
  // clipping implementation and PS helper invocation counting may differ
  // across a render-pass split. The D3D11.3 functional spec, sections 9.2 and
  // 20.4.7, explicitly distinguishes execution counts from IA work counts:
  // https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
  // Exact app IA/occlusion work above catches an extra internal triangle even
  // while app queries are active; all-zero meta-only counters below also cover
  // every pipeline stage. Keep both checks and the query-pollution control.
  CHECK(T, samples[2] == 0);
  const D3D11_QUERY_DATA_PIPELINE_STATISTICS empty = {};
  CHECK(T, std::memcmp(&stats[2], &empty, sizeof(empty)) == 0);
  CHECK(T, exactPixels(c, dst.texture, DXGI_FORMAT_B8G8R8X8_UNORM,
                      DXGI_FORMAT_R8G8B8A8_UNORM));

  Com<ID3D11Predicate> predicate, saved;
  D3D11_QUERY_DESC pd = {D3D11_QUERY_OCCLUSION_PREDICATE, 0};
  CHECK_HR(T, c.device->CreatePredicate(&pd, &predicate));
  c.ctx->Begin(predicate); c.ctx->End(predicate);
  const float green[] = {0, 1, 0, 1}; c.ctx->ClearRenderTargetView(dst.rtv, green);
  c.ctx->SetPredication(predicate, FALSE);
  CHECK_HR(T, copyColor(c, dst.rtv, src.srv));
  BOOL value = TRUE; c.ctx->GetPredication(&saved, &value);
  CHECK(T, saved.p == predicate.p && value == FALSE);
  CHECK(T, draw.unchanged(c, src.srv));
  c.ctx->SetPredication(nullptr, FALSE);
  CHECK(T, exactPixels(c, dst.texture, DXGI_FORMAT_B8G8R8X8_UNORM,
                      DXGI_FORMAT_R8G8B8A8_UNORM));
  c.ctx->ClearState();
}

static void validation(Ctx& c) {
  const char* T = "color_copy_validation";
  c.ctx->ClearState();
  auto data = pixels(DXGI_FORMAT_R8G8B8A8_UNORM);
  Scratch src(c, DXGI_FORMAT_R8G8B8A8_UNORM), dst(c, DXGI_FORMAT_R8G8B8A8_UNORM, data.data());
  CHECK(T, src.valid && dst.valid, return);
  CHECK(T, dxvk_d3d11_copy_color(nullptr, dst.rtv, src.srv) == E_INVALIDARG);
  CHECK(T, dxvk_d3d11_copy_color(c.ctx, nullptr, src.srv) == E_INVALIDARG);
  CHECK(T, dxvk_d3d11_copy_color(c.ctx, dst.rtv, nullptr) == E_INVALIDARG);
  CHECK(T, dxvk_d3d11_copy_color(c.ctx, dst.rtv, dst.srv) == E_INVALIDARG);
  Com<ID3D11DeviceContext> deferred;
  CHECK_HR(T, c.device->CreateDeferredContext(0, &deferred));
  CHECK(T, dxvk_d3d11_copy_color(deferred, dst.rtv, src.srv) == E_INVALIDARG);
  Ctx other; CHECK(T, create_device(other), return);
  Scratch foreign(other, DXGI_FORMAT_R8G8B8A8_UNORM);
  CHECK(T, foreign.valid, return);
  CHECK(T, dxvk_d3d11_copy_color(c.ctx, foreign.rtv, src.srv) == E_INVALIDARG);
  CHECK(T, dxvk_d3d11_copy_color(c.ctx, dst.rtv, foreign.srv) == E_INVALIDARG);
  CHECK(T, dxvk_d3d11_copy_color(other.ctx, dst.rtv, src.srv) == E_INVALIDARG);
  for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                      DXGI_FORMAT_R16G16B16A16_FLOAT}) {
    Scratch unsupported(c, format); CHECK(T, unsupported.valid, return);
    CHECK(T, dxvk_d3d11_copy_color(c.ctx, dst.rtv, unsupported.srv) == DXGI_ERROR_UNSUPPORTED);
    CHECK(T, dxvk_d3d11_copy_color(c.ctx, unsupported.rtv, src.srv) == DXGI_ERROR_UNSUPPORTED);
  }
  Scratch small(c, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, Width - 1);
  Scratch mips(c, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, Width, Height, 2);
  Scratch array(c, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, Width, Height, 1, 2);
  Scratch msaa(c, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, Width, Height, 1, 1, 4);
  Scratch dynamic(c, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, Width, Height, 1, 1, 1,
                  0, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, D3D11_BIND_SHADER_RESOURCE);
  CHECK(T, small.valid && mips.valid && array.valid && msaa.valid && dynamic.valid, return);
  for (auto* bad : {&small, &mips, &array, &msaa}) {
    CHECK(T, dxvk_d3d11_copy_color(c.ctx, dst.rtv, bad->srv) == E_INVALIDARG);
    CHECK(T, dxvk_d3d11_copy_color(c.ctx, bad->rtv, src.srv) == E_INVALIDARG);
  }
  CHECK(T, dxvk_d3d11_copy_color(c.ctx, dst.rtv, dynamic.srv) == E_INVALIDARG);
  D3D11_SHADER_RESOURCE_VIEW_DESC one = {};
  one.Format = DXGI_FORMAT_R8G8B8A8_UNORM; one.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  one.Texture2D.MipLevels = 1;
  Com<ID3D11ShaderResourceView> singleMip;
  CHECK_HR(T, c.device->CreateShaderResourceView(mips.texture, &one, &singleMip));
  CHECK(T, dxvk_d3d11_copy_color(c.ctx, dst.rtv, singleMip) == E_INVALIDARG);
  // No invalid request may touch the destination, including late size checks.
  CHECK(T, exactPixels(c, dst.texture, DXGI_FORMAT_R8G8B8A8_UNORM,
                      DXGI_FORMAT_R8G8B8A8_UNORM));
}

int main(int argc, char** argv) {
  g_out = stdout;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--drop-copy")) dropCopy = true;
    else if (!std::strcmp(argv[i], "--query-pollution")) queryPollution = true;
    else { emit("[FAIL] unknown option: %s", argv[i]); return 2; }
  }
  Ctx c;
  if (create_device(c)) {
    conversions(c);
    queriesAndState(c);
    validation(c);
    CHECK("color_copy_device", c.device->GetDeviceRemovedReason() == S_OK);
  }
  emit("[COLOR-COPY-SUMMARY] passed=%d failed=%d", g_passes, g_fails);
  return g_fails ? 1 : 0;
}
