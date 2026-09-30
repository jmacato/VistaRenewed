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
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

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

// Match QEMU's CPU scanout path, rather than importing the texture into a GPU.
// Export before rendering so this also detects detached or stale backing data.
static void test_export_cpu_visibility(Ctx &c) {
  const char *T = "export_cpu_visibility";
  struct Target {
    Com<ID3D11Texture2D> tex;
    Com<ID3D11RenderTargetView> rtv;
    Com<IDXGIResource> resource;
    DxvkSharedTextureDescriptor *desc = nullptr;
    void *map = MAP_FAILED;
    uint64_t offset = 0;
    uint32_t cpuPixel = 0;
    ~Target() {
      if (map != MAP_FAILED) munmap(map, desc->allocationSize);
    }
  } targets[3];
  constexpr unsigned width = 1280, height = 720;
  // Interleave a private image between two shared images to exercise selective
  // clear batching and retain the private clear for subsequent GPU use.
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM};
  for (unsigned i = 0; i < 3; ++i) {
    auto &t = targets[i];
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width; td.Height = height;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = formats[i];
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (i != 1)
      td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | DXVK_D3D11_RESOURCE_MISC_LINEAR_EXPORT;
    CHECK_HR(T, c.device->CreateTexture2D(&td, nullptr, &t.tex));
    CHECK(T, bool(t.tex), return);
    CHECK_HR(T, c.device->CreateRenderTargetView(t.tex, nullptr, &t.rtv));
    CHECK(T, bool(t.rtv), return);
    if (i == 1) continue;
    CHECK_HR(T, t.tex->QueryInterface(__uuidof(IDXGIResource), (void **)&t.resource));
    CHECK(T, bool(t.resource), return);
    HANDLE handle = nullptr;
    CHECK_HR(T, t.resource->GetSharedHandle(&handle));
    CHECK(T, handle != nullptr, return);
    t.desc = static_cast<DxvkSharedTextureDescriptor *>(handle);
    CHECK(T, t.desc->fd >= 0 && t.desc->planeCount == 1 && t.desc->drmFormatModifier == 0, return);
    t.offset = t.desc->planes[0].offset + (height/2) * t.desc->planes[0].pitch + (width/2) * 4;
    CHECK(T, t.offset + 4 <= t.desc->allocationSize, return);
    t.map = mmap(nullptr, t.desc->allocationSize, PROT_READ, MAP_SHARED, t.desc->fd, 0);
    CHECK(T, t.map != MAP_FAILED, return);
  }
  const FLOAT colours[][4] = {{1,0,0,1}, {0,1,0,1}, {0,0,1,1}};
  const uint32_t expected[] = {0x00ff0000, 0x0000ff00, 0x000000ff};
  for (unsigned phase = 0; phase < 3; ++phase) {
    for (unsigned i = 0; i < 3; ++i)
      c.ctx->ClearRenderTargetView(targets[i].rtv, colours[(phase+i)%3]);
    // Completion alone must publish the shared pixels. A staging copy before
    // CPU access would incidentally materialize the clear and hide the defect.
    Com<ID3D11Query> completion;
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    CHECK_HR(T, c.device->CreateQuery(&qd, &completion));
    CHECK(T, bool(completion), return);
    c.ctx->End(completion);
    c.ctx->Flush();
    BOOL done = FALSE;
    HRESULT ready = S_FALSE;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ready == S_FALSE && std::chrono::steady_clock::now() < deadline) {
      ready = c.ctx->GetData(completion, &done, sizeof(done), 0);
      if (ready == S_FALSE) usleep(1000);
    }
    CHECK(T, ready == S_OK && done, return);
    for (unsigned i = 0; i < 3; ++i) {
      auto &t = targets[i];
      if (!t.desc) continue;
      dma_buf_sync sync = {};
      sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
      int rc;
      do { rc = ioctl(t.desc->fd, DMA_BUF_IOCTL_SYNC, &sync); }
      while (rc < 0 && (errno == EINTR || errno == EAGAIN));
      CHECK(T, rc == 0, return);
      memcpy(&t.cpuPixel, static_cast<const char *>(t.map) + t.offset, 4);
      sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
      do { rc = ioctl(t.desc->fd, DMA_BUF_IOCTL_SYNC, &sync); }
      while (rc < 0 && (errno == EINTR || errno == EAGAIN));
      CHECK(T, rc == 0, return);
      emit("[EXPORT] format=%u phase=%u CPU=%08x expected=%08x",
           unsigned(formats[i]), phase, t.cpuPixel, expected[(phase+i)%3]);
      CHECK(T, (t.cpuPixel & 0xffffff) == expected[(phase+i)%3]);
    }
    // Only after sampling every external buffer, validate GPU contents,
    // including the private clear that must survive the selective flush.
    for (unsigned i = 0; i < 3; ++i) {
      auto gpu = readback_tex2d(c, targets[i].tex, 0, 4);
      CHECK(T, gpu.size() == width * height * 4, return);
      uint32_t pixel;
      memcpy(&pixel, gpu.data() + ((height/2)*width + width/2)*4, 4);
      CHECK(T, (pixel & 0xffffff) == expected[(phase+i)%3]);
    }
  }
}

#ifndef TRITON_LINUX_D3D11_NO_MAIN
int main(int argc, char **argv) {
  triton_original_main(argc, argv);
  g_out = stdout;
  Ctx c;
  if (create_device(c)) {
    test_clear_validation(c);
    if (getenv("TRITON_TEST_SHARED")) {
      test_linear_shared_texture(c);
      test_export_cpu_visibility(c);
    } else {
      emit("[SKIP] shared texture tests: set TRITON_TEST_SHARED=1 on a dma-buf GPU");
    }
  }
  emit("[LINUX-SUMMARY] passed=%d failed=%d", g_passes, g_fails);
  return g_fails ? 1 : 0;
}
#endif
