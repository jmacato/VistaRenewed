/* Native regression for query availability during concurrent submission. */
#include <d3d11.h>
#include "tritonBlitShaders.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

template<typename T> struct Com {
  T* p = nullptr;
  ~Com() { if (p) p->Release(); }
  T* operator->() { return p; }
  T** put() { return &p; }
};

static void require_hr(HRESULT result) {
  if (FAILED(result)) {
    std::fprintf(stderr, "HRESULT=%08x\n", unsigned(result));
    throw std::runtime_error("Direct3D call failed");
  }
}

int main() {
  try {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context, deferred;
    require_hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
      nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
    require_hr(device->CreateDeferredContext(0, deferred.put()));
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    require_hr(device->CreateVertexShader(g_tritonBlitVS, sizeof(g_tritonBlitVS), nullptr, vs.put()));
    require_hr(device->CreatePixelShader(g_tritonBlitPS, sizeof(g_tritonBlitPS), nullptr, ps.put()));

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 32;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    Com<ID3D11Texture2D> texture, staging;
    Com<ID3D11RenderTargetView> rtv;
    require_hr(device->CreateTexture2D(&td, nullptr, texture.put()));
    require_hr(device->CreateRenderTargetView(texture.p, nullptr, rtv.put()));
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require_hr(device->CreateTexture2D(&td, nullptr, staging.put()));
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    rd.DepthClipEnable = TRUE;
    Com<ID3D11RasterizerState> raster;
    require_hr(device->CreateRasterizerState(&rd, raster.put()));
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    Com<ID3D11DepthStencilState> depth;
    require_hr(device->CreateDepthStencilState(&dd, depth.put()));

    D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION, 0 };
    Com<ID3D11Query> outer, inner, empty;
    require_hr(device->CreateQuery(&qd, outer.put()));
    require_hr(device->CreateQuery(&qd, inner.put()));
    require_hr(device->CreateQuery(&qd, empty.put()));
    unsigned passed = 0, failed = 0;
    auto check = [&](bool condition, const char* label, unsigned iteration) {
      if (condition) ++passed;
      else { ++failed; std::printf("FAIL iteration=%u %s\n", iteration, label); }
    };
    auto get = [&](ID3D11Query* query) {
      UINT64 value = ~UINT64(0);
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      for (;;) {
        HRESULT hr = context->GetData(query, &value, sizeof(value), 0);
        if (hr != S_FALSE) { require_hr(hr); return value; }
        if (std::chrono::steady_clock::now() > deadline)
          throw std::runtime_error("query timeout");
        std::this_thread::yield();
      }
    };
    for (unsigned iteration = 0; iteration < 400; ++iteration) {
      ID3D11DeviceContext* draw = iteration & 1 ? deferred.p : context.p;
      draw->ClearState();
      draw->VSSetShader(vs.p, nullptr, 0);
      draw->PSSetShader(ps.p, nullptr, 0);
      draw->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      draw->RSSetState(raster.p);
      draw->OMSetDepthStencilState(depth.p, 0);
      draw->OMSetRenderTargets(1, &rtv.p, nullptr);
      D3D11_VIEWPORT vp = { 0, 0, 32, 32, 0, 1 };
      D3D11_RECT rect = { 4, 4, 5 + LONG(iteration % 7), 5 + LONG(iteration % 11) };
      const UINT64 area = (rect.right - rect.left) * (rect.bottom - rect.top);
      draw->RSSetViewports(1, &vp);
      draw->RSSetScissorRects(1, &rect);
      const float white[4] = { 1, 1, 1, 1 };
      draw->ClearRenderTargetView(rtv.p, white);
      draw->Begin(outer.p);
      draw->Begin(inner.p);
      draw->Draw(3, 0);
      draw->End(inner.p);
      draw->Draw(3, 0);
      draw->End(outer.p);
      draw->Begin(empty.p);
      draw->End(empty.p);
      if (draw == deferred.p) {
        Com<ID3D11CommandList> commands;
        require_hr(draw->FinishCommandList(FALSE, commands.put()));
        context->ExecuteCommandList(commands.p, FALSE);
      }

      // Read the numeric results before any event, mapping or explicit flush.
      const UINT64 first = get(inner.p);
      const UINT64 second = get(outer.p);
      check(first == area, "inner exact count", iteration);
      check(second == 2 * area, "overlapping outer exact count", iteration);
      check(get(empty.p) == 0, "empty scope count", iteration);
      check(get(inner.p) == first, "stable repeated result", iteration);

      context->CopyResource(staging.p, texture.p);
      D3D11_MAPPED_SUBRESOURCE map = {};
      require_hr(context->Map(staging.p, 0, D3D11_MAP_READ, 0, &map));
      UINT64 changed = 0;
      for (UINT y = 0; y < 32; ++y) {
        const UINT* row = reinterpret_cast<const UINT*>(
          static_cast<const char*>(map.pData) + y * map.RowPitch);
        for (UINT x = 0; x < 32; ++x) changed += row[x] != 0xffffffffu;
      }
      context->Unmap(staging.p, 0);
      check(changed == area, "independent changed-pixel count", iteration);
    }

    // Force many physical fragments while one virtual query remains active.
    // This also exercises accumulation from the command-recording thread.
    context->ClearState();
    context->VSSetShader(vs.p, nullptr, 0);
    context->PSSetShader(ps.p, nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->RSSetState(raster.p);
    context->OMSetDepthStencilState(depth.p, 0);
    context->OMSetRenderTargets(1, &rtv.p, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, 32, 32, 0, 1 };
    D3D11_RECT rect = { 4, 4, 12, 12 };
    context->RSSetViewports(1, &vp);
    context->RSSetScissorRects(1, &rect);
    context->Begin(outer.p);
    for (unsigned segment = 0; segment < 64; ++segment) {
      context->Draw(3, 0);
      context->CopyResource(staging.p, texture.p);
      context->Flush();
    }
    context->End(outer.p);
    check(get(outer.p) == 64 * 64, "fragmented scope exact count", 400);
    check(device->GetDeviceRemovedReason() == S_OK, "device health", 400);
    std::printf("OCCLUSION-READINESS passed=%u failed=%u\n", passed, failed);
    return failed ? 1 : 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 2;
  }
}
