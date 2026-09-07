/* SPDX-License-Identifier: MIT
 * Run the existing offscreen acceptance suite against DXVK's native ABI.
 * Linux DXVK signals eventfd handles; these helpers adapt only that OS API.
 */
#define dxmt_d3d11_clear_depth_stencil_rects dxvk_d3d11_clear_depth_stencil_rects
#define dxmt_event_create triton_test_event_create
#define dxmt_event_close triton_test_event_close
#define dxmt_event_wait triton_test_event_wait
#define main triton_original_main
#include "../triton-dxmt/tests/native/d3d11_test.cpp"
#undef main

#include <cerrno>
#include <climits>
#include <poll.h>
#include <sys/eventfd.h>
#include <dxvk_shared_resource.h>

extern "C" void *triton_test_event_create(int manual_reset, int initial_state) {
  if (manual_reset)
    return nullptr;
  int fd = eventfd(initial_state ? 1 : 0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd < 0)
    return nullptr;
  if (fd == 0) {
    int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    close(fd);
    fd = replacement;
  }
  return fd < 0 ? nullptr : reinterpret_cast<void *>(intptr_t(fd));
}

extern "C" void triton_test_event_close(void *event) {
  if (event)
    close(int(intptr_t(event)));
}

extern "C" int triton_test_event_wait(void *event, uint64_t timeout_ns) {
  if (!event)
    return -1;
  pollfd pfd = { int(intptr_t(event)), POLLIN, 0 };
  uint64_t ms = timeout_ns / 1000000 + (timeout_ns % 1000000 != 0);
  int rc = poll(&pfd, 1, int(std::min(ms, uint64_t(INT_MAX))));
  if (rc <= 0 || !(pfd.revents & POLLIN))
    return rc == 0 ? 1 : -1;
  uint64_t value;
  return read(pfd.fd, &value, sizeof(value)) == sizeof(value) ? 0 : -1;
}

static void test_clear_validation(Ctx &c) {
  const char *T = "private_clear_validation";
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = 8;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  Com<ID3D11Texture2D> tex;
  Com<ID3D11DepthStencilView> dsv;
  CHECK_HR(T, c.device->CreateTexture2D(&td, nullptr, &tex));
  CHECK_HR(T, c.device->CreateDepthStencilView(tex, nullptr, &dsv));
  c.ctx->ClearDepthStencilView(dsv, 3, 0.25f, 0x55);
  const int32_t rect[] = {0, 0, 2, 2};
  auto clear = [&](UINT flags, float depth, UINT count, const int32_t *rects) {
    return dxvk_d3d11_clear_depth_stencil_rects(c.ctx, dsv, flags, depth, 0xaa, count, rects);
  };
  CHECK(T, clear(0, 0.75f, 1, rect) == E_INVALIDARG);
  CHECK(T, clear(4, 0.75f, 1, rect) == E_INVALIDARG);
  CHECK(T, clear(1, NAN, 1, rect) == E_INVALIDARG);
  CHECK(T, clear(1, 1.1f, 1, rect) == E_INVALIDARG);
  CHECK(T, clear(1, -0.1f, 1, rect) == E_INVALIDARG);
  CHECK(T, clear(1, 0.75f, 0, rect) == E_INVALIDARG);
  CHECK(T, clear(1, 0.75f, 4097, rect) == E_INVALIDARG);
  CHECK(T, clear(1, 0.75f, 1, nullptr) == E_INVALIDARG);
  CHECK(T, dxvk_d3d11_clear_depth_stencil_rects(nullptr, dsv, 1, 0.75f, 0, 1, rect) == E_INVALIDARG);
  CHECK(T, dxvk_d3d11_clear_depth_stencil_rects(c.ctx, nullptr, 1, 0.75f, 0, 1, rect) == E_INVALIDARG);
  const int32_t negative[] = {-1, 0, 2, 2};
  const int32_t empty[] = {2, 0, 2, 2};
  const int32_t batch[] = {0, 0, 2, 2, 6, 6, 9, 9};
  CHECK(T, clear(1, 0.75f, 1, negative) == E_INVALIDARG);
  CHECK(T, clear(1, 0.75f, 1, empty) == E_INVALIDARG);
  CHECK(T, clear(3, 0.75f, 2, batch) == E_INVALIDARG);
  // A rejected batch must leave even its valid first rectangle unchanged.
  auto image = probe_depth_stencil(c, dsv, 8, 8, 0.5f, true, false, 0);
  CHECK(T, image.size() == 8 * 8 * 4, return);
  CHECK(T, px_eq(image, 8, 1, 1, 0, 0, 0, 255, 0));
  image = probe_depth_stencil(c, dsv, 8, 8, 0, false, true, 0x55);
  CHECK(T, image.size() == 8 * 8 * 4, return);
  CHECK(T, px_eq(image, 8, 1, 1, 255, 255, 255, 255, 0));
  Com<ID3D11DeviceContext> deferred;
  CHECK_HR(T, c.device->CreateDeferredContext(0, &deferred));
  CHECK(T, dxvk_d3d11_clear_depth_stencil_rects(deferred, dsv, 1, 0.75f, 0, 1, rect) == E_INVALIDARG);
  Ctx other;
  CHECK(T, create_device(other), return);
  CHECK(T, dxvk_d3d11_clear_depth_stencil_rects(other.ctx, dsv, 1, 0.75f, 0, 1, rect) == E_INVALIDARG);
  const int32_t corners[] = {0, 0, 2, 2, 6, 6, 8, 8};
  Com<ID3D11Query> completion;
  D3D11_QUERY_DESC queryDesc = {D3D11_QUERY_EVENT, 0};
  CHECK_HR(T, c.device->CreateQuery(&queryDesc, &completion));
  c.ctx->End(completion);
  CHECK_HR(T, clear(3, 0.75f, 2, corners));
  BOOL done = FALSE;
  CHECK(T, c.ctx->GetData(completion, &done, sizeof(done),
                         D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && done);
  image = probe_depth_stencil(c, dsv, 8, 8, 0.5f, true, false, 0);
  CHECK(T, image.size() == 8 * 8 * 4, return);
  CHECK(T, px_eq(image, 8, 1, 1, 255, 255, 255, 255, 0));
  CHECK(T, px_eq(image, 8, 7, 7, 255, 255, 255, 255, 0));
  CHECK(T, px_eq(image, 8, 4, 4, 0, 0, 0, 255, 0));
  // Maximum wire batch exceeds DXVK's inline command-chunk capacity.
  std::vector<int32_t> maximumBatch;
  for (UINT i = 0; i < 4096; ++i)
    maximumBatch.insert(maximumBatch.end(), {0, 0, 2, 2});
  CHECK_HR(T, clear(3, 0.25f, 4096, maximumBatch.data()));
  image = probe_depth_stencil(c, dsv, 8, 8, 0.5f, true, false, 0);
  CHECK(T, image.size() == 8 * 8 * 4, return);
  CHECK(T, px_eq(image, 8, 1, 1, 0, 0, 0, 255, 0));
  CHECK(T, px_eq(image, 8, 7, 7, 255, 255, 255, 255, 0));
  image = probe_depth_stencil(c, dsv, 8, 8, 0, false, true, 0xaa);
  CHECK(T, image.size() == 8 * 8 * 4, return);
  CHECK(T, px_eq(image, 8, 1, 1, 255, 255, 255, 255, 0));
  CHECK(T, px_eq(image, 8, 7, 7, 255, 255, 255, 255, 0));
  CHECK(T, px_eq(image, 8, 4, 4, 0, 0, 0, 255, 0));
}

static void test_linear_shared_texture(Ctx &c) {
  const char *T = "linear_shared_texture";
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = 8;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | DXVK_D3D11_RESOURCE_MISC_LINEAR_EXPORT;
  Com<ID3D11Texture2D> tex;
  Com<ID3D11RenderTargetView> rtv;
  CHECK_HR(T, c.device->CreateTexture2D(&td, nullptr, &tex));
  CHECK_HR(T, c.device->CreateRenderTargetView(tex, nullptr, &rtv));
  const FLOAT red[] = {1, 0, 0, 1};
  c.ctx->ClearRenderTargetView(rtv, red);
  auto image = readback_tex2d(c, tex, 0, 4);
  CHECK(T, image.size() == 8 * 8 * 4, return);
  CHECK(T, px_eq(image, 8, 4, 4, 0, 0, 255, 255, 0)); // BGRA bytes
  Com<IDXGIResource> resource;
  CHECK_HR(T, tex->QueryInterface(__uuidof(IDXGIResource), (void **)&resource));
  HANDLE handle = nullptr;
  CHECK_HR(T, resource->GetSharedHandle(&handle));
  CHECK(T, handle != nullptr, return);
  auto *descriptor = static_cast<DxvkSharedTextureDescriptor *>(handle);
  CHECK(T, descriptor->magic == DXVK_SHARED_DESCRIPTOR_TEXTURE);
  CHECK(T, descriptor->version == DXVK_SHARED_DESCRIPTOR_VERSION);
  CHECK(T, descriptor->structSize == sizeof(*descriptor));
  CHECK(T, descriptor->drmFormatModifier == 0 && descriptor->planeCount == 1);
  CHECK(T, descriptor->planes[0].pitch >= 32 && descriptor->fd >= 0);
  Ctx consumer;
  CHECK(T, create_device(consumer), return);
  auto transferred = *descriptor;
  transferred.fd = dup(descriptor->fd);
  CHECK(T, transferred.fd >= 0, return);
  Com<ID3D11Texture2D> imported;
  HRESULT hr = consumer.device->OpenSharedResource(
      &transferred, __uuidof(ID3D11Texture2D), (void **)&imported);
  close(transferred.fd); // Import must retain its own fd, not this copy.
  CHECK(T, hr == S_OK, return);
  image = readback_tex2d(consumer, imported, 0, 4);
  CHECK(T, image.size() == 8 * 8 * 4, return);
  CHECK(T, px_eq(image, 8, 4, 4, 0, 0, 255, 255, 0));
  Com<ID3D11RenderTargetView> importedRtv;
  CHECK_HR(T, consumer.device->CreateRenderTargetView(imported, nullptr, &importedRtv));
  const FLOAT green[] = {0, 1, 0, 1};
  consumer.ctx->ClearRenderTargetView(importedRtv, green);
  image = readback_tex2d(consumer, imported, 0, 4); // Complete consumer work.
  CHECK(T, image.size() == 8 * 8 * 4, return);
  CHECK(T, px_eq(image, 8, 4, 4, 0, 255, 0, 255, 0));
  image = readback_tex2d(c, tex, 0, 4);
  CHECK(T, image.size() == 8 * 8 * 4, return);
  CHECK(T, px_eq(image, 8, 4, 4, 0, 255, 0, 255, 0));
}

int main(int argc, char **argv) {
  triton_original_main(argc, argv);
  g_out = stdout;
  Ctx c;
  if (create_device(c)) {
    test_clear_validation(c);
    if (getenv("TRITON_TEST_SHARED"))
      test_linear_shared_texture(c);
    else
      emit("[SKIP] linear_shared_texture: set TRITON_TEST_SHARED=1 on a dma-buf GPU");
  }
  emit("[LINUX-SUMMARY] passed=%d failed=%d", g_passes, g_fails);
  return g_fails ? 1 : 0;
}
