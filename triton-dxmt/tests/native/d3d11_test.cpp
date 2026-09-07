/*
 * Offscreen D3D11 smoke suite for libdxmt-native.
 *
 * Runs the same binary on macOS and on the headless Apple platforms (iOS,
 * tvOS, ...): no swapchain, no window -- every test renders or computes into
 * ordinary resources and reads the result back through a STAGING copy.
 *
 * Output is line-oriented so runs are diffable across platforms:
 *   [PASS]/[FAIL] <test>: <detail>
 *   [CRC] <test>.<image> = <crc32>      -- payload for cross-platform compare
 *   [INFO] ...                          -- environment, never compared
 * A summary line ends the run.  Exit code is the number of failures.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>

#include <cinttypes>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "shaders_dxbc.h"

extern "C" {
void *dxmt_event_create(int manual_reset, int initial_state);
void dxmt_event_close(void *);
int dxmt_event_wait(void *, uint64_t timeout_ns);
int32_t dxmt_d3d11_clear_depth_stencil_rects(
    void *context, void *depth_stencil_view, uint32_t clear_flags,
    float depth, uint8_t stencil, uint32_t rect_count,
    const int32_t *rect_ltrb);
}

static FILE *g_out = nullptr;
static int g_fails = 0;
static int g_passes = 0;

static void
emit(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char line[1024];
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  fprintf(g_out, "%s\n", line);
  fflush(g_out);
  if (g_out != stdout) {
    fprintf(stdout, "%s\n", line);
    fflush(stdout);
  }
}

#define CHECK(test, cond, ...)                                                                                         \
  do {                                                                                                                 \
    if (cond) {                                                                                                        \
      g_passes++;                                                                                                      \
      emit("[PASS] %s: %s", test, #cond);                                                                              \
    } else {                                                                                                           \
      g_fails++;                                                                                                       \
      emit("[FAIL] %s: %s", test, #cond);                                                                              \
      __VA_ARGS__;                                                                                                     \
    }                                                                                                                  \
  } while (0)

#define CHECK_HR(test, expr)                                                                                           \
  do {                                                                                                                 \
    HRESULT hr_ = (expr);                                                                                              \
    if (SUCCEEDED(hr_)) {                                                                                              \
      g_passes++;                                                                                                      \
      emit("[PASS] %s: %s", test, #expr);                                                                              \
    } else {                                                                                                           \
      g_fails++;                                                                                                       \
      emit("[FAIL] %s: %s -> hr=0x%08x", test, #expr, (unsigned)hr_);                                                  \
      return;                                                                                                          \
    }                                                                                                                  \
  } while (0)

static uint32_t
crc32_of(const void *data, size_t len) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++)
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  const uint8_t *p = (const uint8_t *)data;
  while (len--)
    crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

template <typename T> struct Com {
  T *p = nullptr;
  ~Com() {
    if (p)
      p->Release();
  }
  T *operator->() const { return p; }
  T **operator&() { return &p; }
  operator T *() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

struct Vertex {
  float pos[2];
  float uv[2];
  float color[4];
};

static const D3D11_INPUT_ELEMENT_DESC kLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

/* Shared state for the render tests */
struct Ctx {
  Com<ID3D11Device> device;
  Com<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
};

static bool
create_device(Ctx &c) {
  const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
  };
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 4, D3D11_SDK_VERSION, &c.device, &c.fl, &c.ctx
  );
  if (FAILED(hr)) {
    emit("[FAIL] device_create: D3D11CreateDevice -> hr=0x%08x", (unsigned)hr);
    g_fails++;
    return false;
  }
  return true;
}

/* Copy any Texture2D to a fresh STAGING twin and map it. Returns tightly
 * packed bytes (pitch removed). */
static std::vector<uint8_t>
readback_tex2d(Ctx &c, ID3D11Texture2D *tex, UINT subresource, UINT bytes_per_pixel) {
  D3D11_TEXTURE2D_DESC desc;
  tex->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  Com<ID3D11Texture2D> staging;
  if (FAILED(c.device->CreateTexture2D(&desc, nullptr, &staging)))
    return {};
  c.ctx->CopyResource(staging, tex);
  D3D11_MAPPED_SUBRESOURCE map = {};
  if (FAILED(c.ctx->Map(staging, subresource, D3D11_MAP_READ, 0, &map)))
    return {};
  UINT mip = subresource % desc.MipLevels;
  UINT w = desc.Width >> mip ? desc.Width >> mip : 1;
  UINT h = desc.Height >> mip ? desc.Height >> mip : 1;
  std::vector<uint8_t> out(w * h * bytes_per_pixel);
  for (UINT y = 0; y < h; y++)
    memcpy(&out[y * w * bytes_per_pixel], (uint8_t *)map.pData + y * map.RowPitch, w * bytes_per_pixel);
  c.ctx->Unmap(staging, subresource);
  return out;
}

static std::vector<uint8_t>
readback_buffer(Ctx &c, ID3D11Buffer *buf) {
  D3D11_BUFFER_DESC desc;
  buf->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  desc.StructureByteStride = 0;
  Com<ID3D11Buffer> staging;
  if (FAILED(c.device->CreateBuffer(&desc, nullptr, &staging)))
    return {};
  c.ctx->CopyResource(staging, buf);
  D3D11_MAPPED_SUBRESOURCE map = {};
  if (FAILED(c.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
    return {};
  std::vector<uint8_t> out((uint8_t *)map.pData, (uint8_t *)map.pData + desc.ByteWidth);
  c.ctx->Unmap(staging, 0);
  return out;
}

static bool
px_eq(const std::vector<uint8_t> &img, UINT w, UINT x, UINT y, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int tol) {
  const uint8_t *p = &img[(y * w + x) * 4];
  return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol && abs(p[3] - a) <= tol;
}

static void
log_px(const std::vector<uint8_t> &img, UINT w, UINT x, UINT y, const char *tag) {
  const uint8_t *p = &img[(y * w + x) * 4];
  emit("[INFO]   %s px(%u,%u) = %u,%u,%u,%u", tag, x, y, p[0], p[1], p[2], p[3]);
}

/* ============================ tests ============================ */

static void
test_device_info(Ctx &c) {
  const char *T = "device_info";
  emit("[INFO] feature_level = 0x%x", c.fl);
  CHECK(T, c.fl >= D3D_FEATURE_LEVEL_11_0);

  Com<IDXGIDevice> dxgi_dev;
  CHECK_HR(T, c.device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_dev));
  Com<IDXGIAdapter> adapter;
  CHECK_HR(T, dxgi_dev->GetAdapter(&adapter));
  DXGI_ADAPTER_DESC ad = {};
  CHECK_HR(T, adapter->GetDesc(&ad));
  char name[129] = {};
  for (int i = 0; i < 128 && ad.Description[i]; i++)
    name[i] = (char)(ad.Description[i] < 128 ? ad.Description[i] : '?');
  emit("[INFO] adapter = '%s' vid=0x%04x vram=%zu MB", name, ad.VendorId, (size_t)(ad.DedicatedVideoMemory >> 20));

  D3D11_FEATURE_DATA_THREADING th = {};
  CHECK_HR(T, c.device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof(th)));
  emit("[INFO] threading: concurrent_creates=%d command_lists=%d", th.DriverConcurrentCreates, th.DriverCommandLists);

  Com<ID3D11Device5> dev5;
  CHECK(T, SUCCEEDED(c.device->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev5)));
}

static void
test_format_support(Ctx &c) {
  const char *T = "format_support";
  struct {
    DXGI_FORMAT fmt;
    const char *name;
    UINT required; /* 0 = informational */
  } formats[] = {
      {DXGI_FORMAT_R8G8B8A8_UNORM, "RGBA8", D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET},
      {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "RGBA8_SRGB", D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET},
      {DXGI_FORMAT_B8G8R8A8_UNORM, "BGRA8", D3D11_FORMAT_SUPPORT_TEXTURE2D},
      {DXGI_FORMAT_R32G32B32A32_FLOAT, "RGBA32F", D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, "RGBA16F", D3D11_FORMAT_SUPPORT_TEXTURE2D},
      {DXGI_FORMAT_D32_FLOAT, "D32F", D3D11_FORMAT_SUPPORT_DEPTH_STENCIL},
      {DXGI_FORMAT_D24_UNORM_S8_UINT, "D24S8", D3D11_FORMAT_SUPPORT_DEPTH_STENCIL},
      {DXGI_FORMAT_BC1_UNORM, "BC1", 0},
      {DXGI_FORMAT_BC3_UNORM, "BC3", 0},
      {DXGI_FORMAT_BC7_UNORM, "BC7", 0},
  };
  for (auto &f : formats) {
    UINT support = 0;
    HRESULT hr = c.device->CheckFormatSupport(f.fmt, &support);
    emit("[INFO] format %s: hr=0x%08x support=0x%08x", f.name, (unsigned)hr, support);
    if (f.required) {
      char tn[64];
      snprintf(tn, sizeof(tn), "%s.%s", T, f.name);
      CHECK(tn, SUCCEEDED(hr) && (support & f.required) == f.required);
    }
  }
}

static void
test_buffer_roundtrip(Ctx &c) {
  const char *T = "buffer_roundtrip";
  const UINT N = 65536;
  std::vector<uint8_t> pattern(N);
  for (UINT i = 0; i < N; i++)
    pattern[i] = (uint8_t)(i * 2654435761u >> 13);

  /* DEFAULT + UpdateSubresource */
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = N;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  Com<ID3D11Buffer> buf;
  CHECK_HR(T, c.device->CreateBuffer(&bd, nullptr, &buf));
  c.ctx->UpdateSubresource(buf, 0, nullptr, pattern.data(), 0, 0);
  auto rb = readback_buffer(c, buf);
  CHECK(T, rb == pattern);

  /* IMMUTABLE with init data */
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  D3D11_SUBRESOURCE_DATA init = {pattern.data(), 0, 0};
  Com<ID3D11Buffer> ibuf;
  CHECK_HR(T, c.device->CreateBuffer(&bd, &init, &ibuf));
  rb = readback_buffer(c, ibuf);
  CHECK(T, rb == pattern);

  /* DYNAMIC + Map(WRITE_DISCARD) */
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  Com<ID3D11Buffer> dbuf;
  CHECK_HR(T, c.device->CreateBuffer(&bd, nullptr, &dbuf));
  D3D11_MAPPED_SUBRESOURCE map = {};
  CHECK_HR(T, c.ctx->Map(dbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &map));
  memcpy(map.pData, pattern.data(), N);
  c.ctx->Unmap(dbuf, 0);
  rb = readback_buffer(c, dbuf);
  CHECK(T, rb == pattern);

  /* Partial CopySubresourceRegion buffer->buffer */
  Com<ID3D11Buffer> half;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.CPUAccessFlags = 0;
  bd.ByteWidth = N / 2;
  CHECK_HR(T, c.device->CreateBuffer(&bd, nullptr, &half));
  D3D11_BOX box = {N / 4, 0, 0, N / 4 + N / 2, 1, 1};
  c.ctx->CopySubresourceRegion(half, 0, 0, 0, 0, buf, 0, &box);
  rb = readback_buffer(c, half);
  CHECK(T, rb.size() == N / 2 && memcmp(rb.data(), pattern.data() + N / 4, N / 2) == 0);
}

static void
test_texture_roundtrip(Ctx &c) {
  const char *T = "texture_roundtrip";
  const UINT W = 64, H = 64;
  std::vector<uint8_t> mip0(W * H * 4), mip1(W / 2 * H / 2 * 4);
  for (size_t i = 0; i < mip0.size(); i++)
    mip0[i] = (uint8_t)(i * 40503u >> 9);
  for (size_t i = 0; i < mip1.size(); i++)
    mip1[i] = (uint8_t)(i * 2246822519u >> 17);

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = W;
  td.Height = H;
  td.MipLevels = 2;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc = {1, 0};
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init[2] = {
      {mip0.data(), W * 4, 0},
      {mip1.data(), W / 2 * 4, 0},
  };
  Com<ID3D11Texture2D> tex;
  CHECK_HR(T, c.device->CreateTexture2D(&td, init, &tex));

  auto rb0 = readback_tex2d(c, tex, 0, 4);
  auto rb1 = readback_tex2d(c, tex, 1, 4);
  CHECK(T, rb0 == mip0);
  CHECK(T, rb1 == mip1);

  /* Partial CopySubresourceRegion into a second texture */
  td.MipLevels = 1;
  Com<ID3D11Texture2D> tex2;
  CHECK_HR(T, c.device->CreateTexture2D(&td, nullptr, &tex2));
  D3D11_BOX box = {8, 8, 0, 40, 40, 1};
  c.ctx->CopySubresourceRegion(tex2, 0, 4, 4, 0, tex, 0, &box);
  auto rb2 = readback_tex2d(c, tex2, 0, 4);
  bool region_ok = true;
  for (UINT y = 0; y < 32 && region_ok; y++)
    region_ok = memcmp(&rb2[((y + 4) * W + 4) * 4], &mip0[((y + 8) * W + 8) * 4], 32 * 4) == 0;
  CHECK(T, region_ok);
}

/* Renders `verts` (triangle list) into a fresh RGBA8 RT of size WxH with the
 * given shaders bound; returns the readback. */
static std::vector<uint8_t>
draw_and_read(
    Ctx &c, UINT W, UINT H, const Vertex *verts, UINT nverts, const void *vs, size_t vs_len, const void *ps,
    size_t ps_len, DXGI_FORMAT rt_format = DXGI_FORMAT_R8G8B8A8_UNORM,
    void (*extra)(Ctx &, ID3D11DeviceContext *) = nullptr, ID3D11DeviceContext *record = nullptr, UINT instances = 0
) {
  ID3D11DeviceContext *ctx = record ? record : c.ctx.p;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = W;
  td.Height = H;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = rt_format;
  td.SampleDesc = {1, 0};
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  Com<ID3D11Texture2D> rt;
  if (FAILED(c.device->CreateTexture2D(&td, nullptr, &rt)))
    return {};
  Com<ID3D11RenderTargetView> rtv;
  if (FAILED(c.device->CreateRenderTargetView(rt, nullptr, &rtv)))
    return {};

  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(Vertex) * nverts;
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA init = {verts, 0, 0};
  Com<ID3D11Buffer> vb;
  if (FAILED(c.device->CreateBuffer(&bd, &init, &vb)))
    return {};

  Com<ID3D11VertexShader> vshader;
  Com<ID3D11PixelShader> pshader;
  Com<ID3D11InputLayout> layout;
  if (FAILED(c.device->CreateVertexShader(vs, vs_len, nullptr, &vshader)))
    return {};
  if (FAILED(c.device->CreatePixelShader(ps, ps_len, nullptr, &pshader)))
    return {};
  if (FAILED(c.device->CreateInputLayout(kLayout, 3, vs, vs_len, &layout)))
    return {};

  float clear[4] = {0, 0, 0, 1};
  ctx->ClearRenderTargetView(rtv, clear);
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
  ctx->RSSetViewports(1, &vp);
  UINT stride = sizeof(Vertex), offset = 0;
  ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  ctx->IASetInputLayout(layout);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vshader, nullptr, 0);
  ctx->PSSetShader(pshader, nullptr, 0);
  if (extra)
    extra(c, ctx);
  if (instances)
    ctx->DrawInstanced(nverts, instances, 0, 0);
  else
    ctx->Draw(nverts, 0);

  if (record) {
    Com<ID3D11CommandList> cl;
    if (FAILED(((ID3D11DeviceContext *)record)->FinishCommandList(FALSE, &cl)))
      return {};
    c.ctx->ExecuteCommandList(cl, FALSE);
  }
  auto out = readback_tex2d(c, rt, 0, 4);
  c.ctx->ClearState();
  return out;
}

/* Left half of the RT covered by a rectangle (two triangles), flat color.
 * Colors chosen exactly representable in UNORM8. */
static const Vertex kQuadLeft[6] = {
    {{-1, -1}, {0, 1}, {1, 0.5f, 0.25f, 1}}, {{-1, 1}, {0, 0}, {1, 0.5f, 0.25f, 1}},
    {{0, 1}, {0.5f, 0}, {1, 0.5f, 0.25f, 1}}, {{-1, -1}, {0, 1}, {1, 0.5f, 0.25f, 1}},
    {{0, 1}, {0.5f, 0}, {1, 0.5f, 0.25f, 1}}, {{0, -1}, {0.5f, 1}, {1, 0.5f, 0.25f, 1}},
};

static const Vertex kQuadFull[6] = {
    {{-1, -1}, {0, 1}, {1, 1, 1, 1}}, {{-1, 1}, {0, 0}, {1, 1, 1, 1}}, {{1, 1}, {1, 0}, {1, 1, 1, 1}},
    {{-1, -1}, {0, 1}, {1, 1, 1, 1}}, {{1, 1}, {1, 0}, {1, 1, 1, 1}},  {{1, -1}, {1, 1}, {1, 1, 1, 1}},
};

static void
test_draw_basic(Ctx &c) {
  const char *T = "draw_basic";
  const UINT W = 128, H = 128;
  auto img =
      draw_and_read(c, W, H, kQuadLeft, 6, dxbc_vs_pass, sizeof(dxbc_vs_pass), dxbc_ps_color, sizeof(dxbc_ps_color));
  CHECK(T, img.size() == W * H * 4, return);
  /* 0.5 -> 127.5: allow the half-ULP rounding either way; 0.25 -> 63.75 */
  CHECK(T, px_eq(img, W, 16, 64, 255, 128, 64, 255, 1), log_px(img, W, 16, 64, "left"));
  CHECK(T, px_eq(img, W, 100, 64, 0, 0, 0, 255, 0), log_px(img, W, 100, 64, "right"));
  emit("[CRC] %s.rt = 0x%08x", T, crc32_of(img.data(), img.size()));
}

/* ps_compat carries the DXBC patterns whose AIR translation used to emit
 * constructs pre-LLVM-8-era bitcode readers reject (fneg/UNOP, funnel-shift
 * intrinsics): on an affected OS (e.g. iOS 16.3) the un-lowered forms fail
 * pipeline creation with AGX "Failed to materializeAll", and the lowered
 * replacements must still compute the same pixels. */
static void
test_compat_shapes(Ctx &c) {
  const char *T = "compat_shapes";
  const UINT W = 64, H = 64;
  auto img = draw_and_read(
      c, W, H, kQuadFull, 6, dxbc_vs_pass, sizeof(dxbc_vs_pass), dxbc_ps_compat, sizeof(dxbc_ps_compat));
  CHECK(T, img.size() == W * H * 4, return);
  /* color=(1,1,1,1): r = saturate(-(1*1)) + 0.5 = 0.5 (half-ULP window),
   * g = ((200<<3)|(200>>29)) & 255 = 64, b = max(100, 55) = 100 */
  CHECK(T, px_eq(img, W, 32, 32, 128, 64, 100, 255, 1), log_px(img, W, 32, 32, "center"));
  emit("[CRC] %s.rt = 0x%08x", T, crc32_of(img.data(), img.size()));
}

static void
test_draw_deferred(Ctx &c) {
  const char *T = "draw_deferred";
  const UINT W = 128, H = 128;
  auto ref =
      draw_and_read(c, W, H, kQuadLeft, 6, dxbc_vs_pass, sizeof(dxbc_vs_pass), dxbc_ps_color, sizeof(dxbc_ps_color));
  Com<ID3D11DeviceContext> def;
  CHECK_HR(T, c.device->CreateDeferredContext(0, &def));
  auto img = draw_and_read(
      c, W, H, kQuadLeft, 6, dxbc_vs_pass, sizeof(dxbc_vs_pass), dxbc_ps_color, sizeof(dxbc_ps_color),
      DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, def.p
  );
  CHECK(T, !img.empty() && img == ref);
}

static Com<ID3D11Buffer> g_cbuf; /* bound by the cbuf extra */

static void
extra_bind_cbuf(Ctx &c, ID3D11DeviceContext *ctx) {
  ctx->PSSetConstantBuffers(0, 1, &g_cbuf);
}

static void
test_draw_cbuffer(Ctx &c) {
  const char *T = "draw_cbuffer";
  const UINT W = 64, H = 64;
  float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(color);
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA init = {color, 0, 0};
  CHECK_HR(T, c.device->CreateBuffer(&bd, &init, &g_cbuf));
  auto img = draw_and_read(
      c, W, H, kQuadFull, 6, dxbc_vs_pass, sizeof(dxbc_vs_pass), dxbc_ps_cbuf, sizeof(dxbc_ps_cbuf),
      DXGI_FORMAT_R8G8B8A8_UNORM, extra_bind_cbuf
  );
  CHECK(T, img.size() == W * H * 4, return);
  CHECK(T, px_eq(img, W, 32, 32, 64, 128, 191, 255, 1), log_px(img, W, 32, 32, "center"));
  emit("[CRC] %s.rt = 0x%08x", T, crc32_of(img.data(), img.size()));
  g_cbuf.p->Release();
  g_cbuf.p = nullptr;
}

static Com<ID3D11ShaderResourceView> g_srv;
static Com<ID3D11SamplerState> g_sampler;

static void
extra_bind_tex(Ctx &c, ID3D11DeviceContext *ctx) {
  ctx->PSSetShaderResources(0, 1, &g_srv);
  ctx->PSSetSamplers(0, 1, &g_sampler);
}

static void
test_draw_textured(Ctx &c) {
  const char *T = "draw_textured";
  const UINT W = 64, H = 64;
  /* 2x2 texture, one solid color per quadrant */
  const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = 2;
  td.Height = 2;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc = {1, 0};
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init = {texels, 8, 0};
  Com<ID3D11Texture2D> tex;
  CHECK_HR(T, c.device->CreateTexture2D(&td, &init, &tex));
  CHECK_HR(T, c.device->CreateShaderResourceView(tex, nullptr, &g_srv));
  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  CHECK_HR(T, c.device->CreateSamplerState(&sd, &g_sampler));

  auto img = draw_and_read(
      c, W, H, kQuadFull, 6, dxbc_vs_pass, sizeof(dxbc_vs_pass), dxbc_ps_tex, sizeof(dxbc_ps_tex),
      DXGI_FORMAT_R8G8B8A8_UNORM, extra_bind_tex
  );
  CHECK(T, img.size() == W * H * 4, return);
  CHECK(T, px_eq(img, W, 16, 16, 255, 0, 0, 255, 0), log_px(img, W, 16, 16, "tl"));
  CHECK(T, px_eq(img, W, 48, 16, 0, 255, 0, 255, 0), log_px(img, W, 48, 16, "tr"));
  CHECK(T, px_eq(img, W, 16, 48, 0, 0, 255, 255, 0), log_px(img, W, 16, 48, "bl"));
  CHECK(T, px_eq(img, W, 48, 48, 255, 255, 0, 255, 0), log_px(img, W, 48, 48, "br"));
  emit("[CRC] %s.rt = 0x%08x", T, crc32_of(img.data(), img.size()));
  g_srv.p->Release();
  g_srv.p = nullptr;
  g_sampler.p->Release();
  g_sampler.p = nullptr;
}

static void
test_draw_instanced(Ctx &c) {
  const char *T = "draw_instanced";
  const UINT W = 128, H = 64;
  /* A narrow quad on the left; instance 1 lands 0.5 NDC (32px) to the right
   * at half intensity (color * 1/2). */
  Vertex verts[6];
  memcpy(verts, kQuadLeft, sizeof(verts));
  for (auto &v : verts) {
    v.pos[0] = v.pos[0] < 0 ? -1.f : -0.5f;
    v.color[0] = 1;
    v.color[1] = 1;
    v.color[2] = 1;
  }
  auto img = draw_and_read(
      c, W, H, verts, 6, dxbc_vs_inst, sizeof(dxbc_vs_inst), dxbc_ps_color, sizeof(dxbc_ps_color),
      DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, nullptr, 2
  );
  CHECK(T, img.size() == W * H * 4, return);
  CHECK(T, px_eq(img, W, 8, 32, 255, 255, 255, 255, 0), log_px(img, W, 8, 32, "inst0"));
  CHECK(T, px_eq(img, W, 40, 32, 128, 128, 128, 128, 1), log_px(img, W, 40, 32, "inst1"));
  CHECK(T, px_eq(img, W, 80, 32, 0, 0, 0, 255, 0), log_px(img, W, 80, 32, "bg"));
  emit("[CRC] %s.rt = 0x%08x", T, crc32_of(img.data(), img.size()));
}

static void
test_depth(Ctx &c) {
  const char *T = "depth";
  const UINT W = 64, H = 64;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = W;
  td.Height = H;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc = {1, 0};
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  Com<ID3D11Texture2D> rt;
  CHECK_HR(T, c.device->CreateTexture2D(&td, nullptr, &rt));
  Com<ID3D11RenderTargetView> rtv;
  CHECK_HR(T, c.device->CreateRenderTargetView(rt, nullptr, &rtv));

  td.Format = DXGI_FORMAT_D32_FLOAT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  Com<ID3D11Texture2D> ds;
  CHECK_HR(T, c.device->CreateTexture2D(&td, nullptr, &ds));
  Com<ID3D11DepthStencilView> dsv;
  CHECK_HR(T, c.device->CreateDepthStencilView(ds, nullptr, &dsv));

  /* Full-screen green at z=0.25 (uv.x carries z for vs_depth), then
   * full-screen red at z=0.75: depth LESS keeps green. */
  Vertex green[6], red[6];
  memcpy(green, kQuadFull, sizeof(green));
  memcpy(red, kQuadFull, sizeof(red));
  for (auto &v : green) {
    v.uv[0] = 0.25f;
    v.color[0] = 0;
    v.color[1] = 1;
    v.color[2] = 0;
  }
  for (auto &v : red) {
    v.uv[0] = 0.75f;
    v.color[0] = 1;
    v.color[1] = 0;
    v.color[2] = 0;
  }
  Vertex all[12];
  memcpy(all, green, sizeof(green));
  memcpy(all + 6, red, sizeof(red));

  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(all);
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA init = {all, 0, 0};
  Com<ID3D11Buffer> vb;
  CHECK_HR(T, c.device->CreateBuffer(&bd, &init, &vb));

  Com<ID3D11VertexShader> vshader;
  Com<ID3D11PixelShader> pshader;
  Com<ID3D11InputLayout> layout;
  CHECK_HR(T, c.device->CreateVertexShader(dxbc_vs_depth, sizeof(dxbc_vs_depth), nullptr, &vshader));
  CHECK_HR(T, c.device->CreatePixelShader(dxbc_ps_color, sizeof(dxbc_ps_color), nullptr, &pshader));
  CHECK_HR(T, c.device->CreateInputLayout(kLayout, 3, dxbc_vs_depth, sizeof(dxbc_vs_depth), &layout));

  D3D11_DEPTH_STENCIL_DESC dsd = {};
  dsd.DepthEnable = TRUE;
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dsd.DepthFunc = D3D11_COMPARISON_LESS;
  Com<ID3D11DepthStencilState> dss;
  CHECK_HR(T, c.device->CreateDepthStencilState(&dsd, &dss));

  float clear[4] = {0, 0, 0, 1};
  c.ctx->ClearRenderTargetView(rtv, clear);
  c.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
  c.ctx->OMSetRenderTargets(1, &rtv, dsv);
  c.ctx->OMSetDepthStencilState(dss, 0);
  D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
  c.ctx->RSSetViewports(1, &vp);
  UINT stride = sizeof(Vertex), offset = 0;
  c.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  c.ctx->IASetInputLayout(layout);
  c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c.ctx->VSSetShader(vshader, nullptr, 0);
  c.ctx->PSSetShader(pshader, nullptr, 0);
  c.ctx->Draw(12, 0);
  auto img = readback_tex2d(c, rt, 0, 4);
  c.ctx->ClearState();
  CHECK(T, img.size() == W * H * 4, return);
  CHECK(T, px_eq(img, W, 32, 32, 0, 255, 0, 255, 0), log_px(img, W, 32, 32, "center"));
  emit("[CRC] %s.rt = 0x%08x", T, crc32_of(img.data(), img.size()));
}

/* D24S8 is not an ordinary CPU-mappable DXMT format.  Read it through the
 * depth and stencil tests, and return their result in an ordinary RGBA8
 * target.  A white pixel passed the requested tests; a black pixel failed. */
static std::vector<uint8_t>
probe_depth_stencil(Ctx &c, ID3D11DepthStencilView *dsv, UINT width,
                    UINT height, float depth, bool test_depth,
                    bool test_stencil, uint8_t stencil_ref) {
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = width;
  td.Height = height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc = {1, 0};
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  Com<ID3D11Texture2D> target;
  if (FAILED(c.device->CreateTexture2D(&td, nullptr, &target)))
    return {};
  Com<ID3D11RenderTargetView> rtv;
  if (FAILED(c.device->CreateRenderTargetView(target, nullptr, &rtv)))
    return {};

  Vertex quad[6];
  memcpy(quad, kQuadFull, sizeof(quad));
  for (auto &vertex : quad)
    vertex.uv[0] = depth;
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(quad);
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA init = {quad, 0, 0};
  Com<ID3D11Buffer> vb;
  if (FAILED(c.device->CreateBuffer(&bd, &init, &vb)))
    return {};

  Com<ID3D11VertexShader> vertex_shader;
  Com<ID3D11PixelShader> pixel_shader;
  Com<ID3D11InputLayout> layout;
  if (FAILED(c.device->CreateVertexShader(
          dxbc_vs_depth, sizeof(dxbc_vs_depth), nullptr, &vertex_shader)) ||
      FAILED(c.device->CreatePixelShader(
          dxbc_ps_color, sizeof(dxbc_ps_color), nullptr, &pixel_shader)) ||
      FAILED(c.device->CreateInputLayout(
          kLayout, 3, dxbc_vs_depth, sizeof(dxbc_vs_depth), &layout)))
    return {};

  D3D11_DEPTH_STENCIL_DESC state_desc = {};
  state_desc.DepthEnable = test_depth;
  state_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  state_desc.DepthFunc = D3D11_COMPARISON_LESS;
  state_desc.StencilEnable = test_stencil;
  state_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
  state_desc.StencilWriteMask = 0;
  state_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
  state_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
  state_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
  state_desc.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
  state_desc.BackFace = state_desc.FrontFace;
  Com<ID3D11DepthStencilState> state;
  if (FAILED(c.device->CreateDepthStencilState(&state_desc, &state)))
    return {};

  const float black[4] = {0, 0, 0, 1};
  c.ctx->ClearRenderTargetView(rtv, black);
  c.ctx->OMSetRenderTargets(1, &rtv, dsv);
  c.ctx->OMSetDepthStencilState(state, stencil_ref);
  D3D11_VIEWPORT viewport = {0, 0, (float)width, (float)height, 0, 1};
  c.ctx->RSSetViewports(1, &viewport);
  UINT stride = sizeof(Vertex);
  UINT offset = 0;
  c.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  c.ctx->IASetInputLayout(layout);
  c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c.ctx->VSSetShader(vertex_shader, nullptr, 0);
  c.ctx->PSSetShader(pixel_shader, nullptr, 0);
  c.ctx->Draw(6, 0);

  auto image = readback_tex2d(c, target, 0, 4);
  c.ctx->ClearState();
  return image;
}

static void
test_partial_d16_clear(Ctx &c) {
  const char *T = "partial_d16_clear";
  const UINT W = 8, H = 8;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = W;
  td.Height = H;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_D16_UNORM;
  td.SampleDesc = {1, 0};
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;

  Com<ID3D11Texture2D> texture;
  CHECK_HR(T, c.device->CreateTexture2D(&td, nullptr, &texture));
  Com<ID3D11DepthStencilView> dsv;
  CHECK_HR(T, c.device->CreateDepthStencilView(texture, nullptr, &dsv));
  c.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.25f, 0);

  Com<ID3D11DeviceContext1> context1;
  CHECK_HR(T, c.ctx->QueryInterface(
                  __uuidof(ID3D11DeviceContext1), (void **)&context1));
  const float clear_value[4] = {0.75f, 0.0f, 0.0f, 0.0f};
  const D3D11_RECT clear_rect = {2, 2, 6, 6};
  context1->ClearView(dsv, clear_value, &clear_rect, 1);

  auto image =
      probe_depth_stencil(c, dsv, W, H, 0.5f, true, false, 0);
  CHECK(T, image.size() == W * H * 4, return);
  CHECK(T, px_eq(image, W, 1, 1, 0, 0, 0, 255, 0),
        log_px(image, W, 1, 1, "D16 outside"));
  CHECK(T, px_eq(image, W, 2, 2, 255, 255, 255, 255, 0),
        log_px(image, W, 2, 2, "D16 inside"));
}

static void
test_partial_d24s8_clear(Ctx &c) {
  const char *T = "partial_d24s8_clear";
  const UINT W = 8, H = 8;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = W;
  td.Height = H;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  td.SampleDesc = {1, 0};
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;

  Com<ID3D11Texture2D> texture;
  CHECK_HR(T, c.device->CreateTexture2D(&td, nullptr, &texture));
  Com<ID3D11DepthStencilView> dsv;
  CHECK_HR(T, c.device->CreateDepthStencilView(texture, nullptr, &dsv));

  c.ctx->ClearDepthStencilView(
      dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.25f, 0x55);

  const int32_t depth_rect[4] = {2, 2, 6, 6};
  CHECK_HR(T, (HRESULT)dxmt_d3d11_clear_depth_stencil_rects(
                  c.ctx, dsv, D3D11_CLEAR_DEPTH, 0.75f, 0,
                  1, depth_rect));
  auto image = probe_depth_stencil(c, dsv, W, H, 0.5f, true, false, 0);
  CHECK(T, image.size() == W * H * 4, return);
  CHECK(T, px_eq(image, W, 1, 1, 0, 0, 0, 255, 0),
        log_px(image, W, 1, 1, "depth outside"));
  CHECK(T, px_eq(image, W, 2, 2, 255, 255, 255, 255, 0),
        log_px(image, W, 2, 2, "depth inside"));
  image = probe_depth_stencil(c, dsv, W, H, 0.0f, false, true, 0x55);
  CHECK(T, image.size() == W * H * 4, return);
  CHECK(T, px_eq(image, W, 1, 1, 255, 255, 255, 255, 0),
        log_px(image, W, 1, 1, "stencil outside after depth"));
  CHECK(T, px_eq(image, W, 3, 3, 255, 255, 255, 255, 0),
        log_px(image, W, 3, 3, "stencil inside after depth"));

  const int32_t stencil_rect[4] = {3, 3, 5, 5};
  CHECK_HR(T, (HRESULT)dxmt_d3d11_clear_depth_stencil_rects(
                  c.ctx, dsv, D3D11_CLEAR_STENCIL, 0.0f, 0xaa,
                  1, stencil_rect));
  image = probe_depth_stencil(c, dsv, W, H, 0.5f, true, false, 0);
  CHECK(T, image.size() == W * H * 4, return);
  CHECK(T, px_eq(image, W, 1, 1, 0, 0, 0, 255, 0),
        log_px(image, W, 1, 1, "depth outside after stencil"));
  CHECK(T, px_eq(image, W, 3, 3, 255, 255, 255, 255, 0),
        log_px(image, W, 3, 3, "depth inside after stencil"));
  image = probe_depth_stencil(c, dsv, W, H, 0.0f, false, true, 0xaa);
  CHECK(T, image.size() == W * H * 4, return);
  CHECK(T, px_eq(image, W, 2, 2, 0, 0, 0, 255, 0),
        log_px(image, W, 2, 2, "stencil outside"));
  CHECK(T, px_eq(image, W, 3, 3, 255, 255, 255, 255, 0),
        log_px(image, W, 3, 3, "stencil inside"));

  Com<ID3D11DeviceContext1> context1;
  CHECK_HR(T, c.ctx->QueryInterface(
                  __uuidof(ID3D11DeviceContext1), (void **)&context1));
  const float clear_value[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  const D3D11_RECT clear_rect = {0, 0, (LONG)W, (LONG)H};
  context1->ClearView(dsv, clear_value, &clear_rect, 1);
  image = probe_depth_stencil(c, dsv, W, H, 0.5f, true, false, 0);
  CHECK(T, image.size() == W * H * 4, return);
  CHECK(T, px_eq(image, W, 1, 1, 0, 0, 0, 255, 0));
  CHECK(T, px_eq(image, W, 3, 3, 255, 255, 255, 255, 0));
  image = probe_depth_stencil(c, dsv, W, H, 0.0f, false, true, 0xaa);
  CHECK(T, image.size() == W * H * 4, return);
  CHECK(T, px_eq(image, W, 2, 2, 0, 0, 0, 255, 0));
  CHECK(T, px_eq(image, W, 3, 3, 255, 255, 255, 255, 0));
}

static Com<ID3D11BlendState> g_blend;

static void
extra_bind_blend(Ctx &c, ID3D11DeviceContext *ctx) {
  float bf[4] = {0, 0, 0, 0};
  ctx->OMSetBlendState(g_blend, bf, 0xFFFFFFFF);
}

static void
test_blend_additive(Ctx &c) {
  const char *T = "blend_additive";
  const UINT W = 64, H = 64;
  D3D11_BLEND_DESC bd = {};
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  CHECK_HR(T, c.device->CreateBlendState(&bd, &g_blend));

  /* Draw the quarter-intensity full quad twice in one Draw call (12 verts):
   * 0.25 + 0.25 = 0.5 over black clear + 1 alpha saturates alpha. */
  Vertex quad[12];
  memcpy(quad, kQuadFull, sizeof(kQuadFull));
  memcpy(quad + 6, kQuadFull, sizeof(kQuadFull));
  for (auto &v : quad) {
    v.color[0] = 0.25f;
    v.color[1] = 0.25f;
    v.color[2] = 0.25f;
    v.color[3] = 0.25f;
  }
  auto img = draw_and_read(
      c, W, H, quad, 12, dxbc_vs_pass, sizeof(dxbc_vs_pass), dxbc_ps_color, sizeof(dxbc_ps_color),
      DXGI_FORMAT_R8G8B8A8_UNORM, extra_bind_blend
  );
  CHECK(T, img.size() == W * H * 4, return);
  CHECK(T, px_eq(img, W, 32, 32, 128, 128, 128, 255, 1), log_px(img, W, 32, 32, "center"));
  emit("[CRC] %s.rt = 0x%08x", T, crc32_of(img.data(), img.size()));
  g_blend.p->Release();
  g_blend.p = nullptr;
}

static void
test_msaa_resolve(Ctx &c) {
  const char *T = "msaa_resolve";
  const UINT W = 64, H = 64;
  UINT quality = 0;
  HRESULT hr = c.device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &quality);
  emit("[INFO] msaa4 quality levels = %u (hr=0x%08x)", quality, (unsigned)hr);
  if (FAILED(hr) || quality == 0) {
    emit("[INFO] %s: 4x MSAA unsupported, skipping", T);
    return;
  }
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = W;
  td.Height = H;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc = {4, 0};
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  Com<ID3D11Texture2D> ms;
  CHECK_HR(T, c.device->CreateTexture2D(&td, nullptr, &ms));
  Com<ID3D11RenderTargetView> rtv;
  CHECK_HR(T, c.device->CreateRenderTargetView(ms, nullptr, &rtv));
  td.SampleDesc = {1, 0};
  td.BindFlags = 0;
  Com<ID3D11Texture2D> resolved;
  CHECK_HR(T, c.device->CreateTexture2D(&td, nullptr, &resolved));

  D3D11_BUFFER_DESC bdv = {};
  bdv.ByteWidth = sizeof(kQuadLeft);
  bdv.Usage = D3D11_USAGE_IMMUTABLE;
  bdv.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA init = {kQuadLeft, 0, 0};
  Com<ID3D11Buffer> vb;
  CHECK_HR(T, c.device->CreateBuffer(&bdv, &init, &vb));
  Com<ID3D11VertexShader> vshader;
  Com<ID3D11PixelShader> pshader;
  Com<ID3D11InputLayout> layout;
  CHECK_HR(T, c.device->CreateVertexShader(dxbc_vs_pass, sizeof(dxbc_vs_pass), nullptr, &vshader));
  CHECK_HR(T, c.device->CreatePixelShader(dxbc_ps_color, sizeof(dxbc_ps_color), nullptr, &pshader));
  CHECK_HR(T, c.device->CreateInputLayout(kLayout, 3, dxbc_vs_pass, sizeof(dxbc_vs_pass), &layout));

  float clear[4] = {0, 0, 0, 1};
  c.ctx->ClearRenderTargetView(rtv, clear);
  c.ctx->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
  c.ctx->RSSetViewports(1, &vp);
  UINT stride = sizeof(Vertex), offset = 0;
  c.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  c.ctx->IASetInputLayout(layout);
  c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c.ctx->VSSetShader(vshader, nullptr, 0);
  c.ctx->PSSetShader(pshader, nullptr, 0);
  c.ctx->Draw(6, 0);
  c.ctx->ResolveSubresource(resolved, 0, ms, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
  auto img = readback_tex2d(c, resolved, 0, 4);
  c.ctx->ClearState();
  CHECK(T, img.size() == W * H * 4, return);
  /* interior pixels are fully covered -> exact */
  CHECK(T, px_eq(img, W, 8, 32, 255, 128, 64, 255, 1), log_px(img, W, 8, 32, "interior"));
  CHECK(T, px_eq(img, W, 56, 32, 0, 0, 0, 255, 0), log_px(img, W, 56, 32, "outside"));
  emit("[CRC] %s.rt = 0x%08x", T, crc32_of(img.data(), img.size()));
}

static void
test_compute(Ctx &c) {
  const char *T = "compute";
  const UINT N = 256;
  std::vector<uint32_t> input(N);
  for (UINT i = 0; i < N; i++)
    input[i] = i * 7 + 3;

  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = N * 4;
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = 4;
  D3D11_SUBRESOURCE_DATA init = {input.data(), 0, 0};
  Com<ID3D11Buffer> inbuf;
  CHECK_HR(T, c.device->CreateBuffer(&bd, &init, &inbuf));
  Com<ID3D11ShaderResourceView> srv;
  CHECK_HR(T, c.device->CreateShaderResourceView(inbuf, nullptr, &srv));

  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  Com<ID3D11Buffer> outbuf;
  CHECK_HR(T, c.device->CreateBuffer(&bd, nullptr, &outbuf));
  Com<ID3D11UnorderedAccessView> uav;
  CHECK_HR(T, c.device->CreateUnorderedAccessView(outbuf, nullptr, &uav));

  bd.ByteWidth = 4;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  bd.StructureByteStride = 0;
  uint32_t zero = 0;
  D3D11_SUBRESOURCE_DATA zinit = {&zero, 0, 0};
  Com<ID3D11Buffer> counter;
  CHECK_HR(T, c.device->CreateBuffer(&bd, &zinit, &counter));
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = DXGI_FORMAT_R32_TYPELESS;
  ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = 1;
  ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  Com<ID3D11UnorderedAccessView> counter_uav;
  CHECK_HR(T, c.device->CreateUnorderedAccessView(counter, &ud, &counter_uav));

  Com<ID3D11ComputeShader> cs;
  CHECK_HR(T, c.device->CreateComputeShader(dxbc_cs_main, sizeof(dxbc_cs_main), nullptr, &cs));

  c.ctx->CSSetShader(cs, nullptr, 0);
  c.ctx->CSSetShaderResources(0, 1, &srv);
  ID3D11UnorderedAccessView *uavs[2] = {uav, counter_uav};
  c.ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
  c.ctx->Dispatch(N / 64, 1, 1);
  c.ctx->ClearState();

  auto rb = readback_buffer(c, outbuf);
  CHECK(T, rb.size() == N * 4, return);
  bool values_ok = true;
  const uint32_t *words = (const uint32_t *)rb.data();
  for (UINT i = 0; i < N && values_ok; i++)
    values_ok = words[i] == input[i] * 3 + i;
  CHECK(T, values_ok);

  auto rc = readback_buffer(c, counter);
  CHECK(T, rc.size() == 4, return);
  uint32_t sum = *(const uint32_t *)rc.data();
  CHECK(T, sum == N * (N - 1) / 2, emit("[INFO]   counter = %u expected %u", sum, N * (N - 1) / 2));
}

static void
test_query_event(Ctx &c) {
  const char *T = "query_event";
  D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
  Com<ID3D11Query> q;
  CHECK_HR(T, c.device->CreateQuery(&qd, &q));
  c.ctx->End(q);
  c.ctx->Flush();
  BOOL done = FALSE;
  HRESULT hr = S_FALSE;
  // An iteration budget can expire before an asynchronous Vulkan/Metal
  // submission gets scheduled. Use elapsed time, retaining the real result.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (hr == S_FALSE && std::chrono::steady_clock::now() < deadline) {
    hr = c.ctx->GetData(q, &done, sizeof(done), 0);
    if (hr == S_FALSE)
      usleep(1000);
  }
  CHECK(T, hr == S_OK && done == TRUE);

  /* Occlusion query around a covered draw.  The draw is inlined: a
   * synchronizing Map between Begin and End would split the command buffer
   * while the visibility counter is live. */
  qd.Query = D3D11_QUERY_OCCLUSION;
  Com<ID3D11Query> oq;
  CHECK_HR(T, c.device->CreateQuery(&qd, &oq));

  const UINT W = 32, H = 32;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = W;
  td.Height = H;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc = {1, 0};
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  Com<ID3D11Texture2D> rt;
  CHECK_HR(T, c.device->CreateTexture2D(&td, nullptr, &rt));
  Com<ID3D11RenderTargetView> rtv;
  CHECK_HR(T, c.device->CreateRenderTargetView(rt, nullptr, &rtv));
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(kQuadLeft);
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA init = {kQuadLeft, 0, 0};
  Com<ID3D11Buffer> vb;
  CHECK_HR(T, c.device->CreateBuffer(&bd, &init, &vb));
  Com<ID3D11VertexShader> vshader;
  Com<ID3D11PixelShader> pshader;
  Com<ID3D11InputLayout> layout;
  CHECK_HR(T, c.device->CreateVertexShader(dxbc_vs_pass, sizeof(dxbc_vs_pass), nullptr, &vshader));
  CHECK_HR(T, c.device->CreatePixelShader(dxbc_ps_color, sizeof(dxbc_ps_color), nullptr, &pshader));
  CHECK_HR(T, c.device->CreateInputLayout(kLayout, 3, dxbc_vs_pass, sizeof(dxbc_vs_pass), &layout));

  float clear[4] = {0, 0, 0, 1};
  c.ctx->ClearRenderTargetView(rtv, clear);
  c.ctx->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
  c.ctx->RSSetViewports(1, &vp);
  UINT stride = sizeof(Vertex), offset = 0;
  c.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  c.ctx->IASetInputLayout(layout);
  c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c.ctx->VSSetShader(vshader, nullptr, 0);
  c.ctx->PSSetShader(pshader, nullptr, 0);
  c.ctx->Begin(oq);
  c.ctx->Draw(6, 0);
  c.ctx->End(oq);
  c.ctx->Flush();
  UINT64 samples = 0;
  hr = S_FALSE;
  for (int i = 0; i < 5000 && hr == S_FALSE; i++) {
    hr = c.ctx->GetData(oq, &samples, sizeof(samples), 0);
    if (hr == S_FALSE)
      usleep(1000);
  }
  c.ctx->ClearState();
  emit("[INFO] occlusion samples = %llu (hr=0x%08x)", (unsigned long long)samples, (unsigned)hr);
  CHECK(T, hr == S_OK && samples == (W / 2) * H); /* left half covered */
}

static void
test_fence_event(Ctx &c) {
  const char *T = "fence_event";
  Com<ID3D11Device5> dev5;
  if (FAILED(c.device->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev5))) {
    emit("[INFO] %s: ID3D11Device5 unsupported, skipping", T);
    return;
  }
  Com<ID3D11DeviceContext4> ctx4;
  CHECK_HR(T, c.ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4));
  Com<ID3D11Fence> fence;
  CHECK_HR(T, dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), (void **)&fence));

  void *event = dxmt_event_create(0, 0);
  CHECK(T, event != nullptr, return);
  CHECK_HR(T, fence->SetEventOnCompletion(1, (HANDLE)event));
  CHECK_HR(T, ctx4->Signal(fence, 1));
  c.ctx->Flush();
  int wait = dxmt_event_wait(event, 5ull * 1000 * 1000 * 1000);
  CHECK(T, wait == 0 /* DXMT_WAIT_SIGNALED */, emit("[INFO]   wait = %d", wait));
  CHECK(T, fence->GetCompletedValue() >= 1);
  dxmt_event_close(event);
}

static void
test_srgb(Ctx &c) {
  const char *T = "srgb_write";
  const UINT W = 32, H = 32;
  Vertex quad[6];
  memcpy(quad, kQuadFull, sizeof(quad));
  for (auto &v : quad) {
    v.color[0] = 0.5f;
    v.color[1] = 0.5f;
    v.color[2] = 0.5f;
  }
  auto img = draw_and_read(
      c, W, H, quad, 6, dxbc_vs_pass, sizeof(dxbc_vs_pass), dxbc_ps_color, sizeof(dxbc_ps_color),
      DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
  );
  CHECK(T, img.size() == W * H * 4, return);
  /* sRGB(0.5) = 0.73536 -> 187.5, so 187 or 188 */
  CHECK(T, px_eq(img, W, 16, 16, 188, 188, 188, 255, 1), log_px(img, W, 16, 16, "center"));
  emit("[CRC] %s.rt = 0x%08x", T, crc32_of(img.data(), img.size()));
}

/* BC1 sampling.  8x8 IMMUTABLE BC1 texture = 2x2 blocks, each a solid color;
 * point-sample it over a fullscreen quad and expect one color per quadrant.
 *
 * On devices where Metal has no BC support (iOS < 16.4, A-series GPUs) the
 * formats must not reach Metal at all: CheckFormatSupport reports 0 and
 * creation fails cleanly.  Handing MTLPixelFormatBC1_RGBA (130) to such a
 * device is the `pixelFormat (130) is not a valid MTLPixelFormat' validation
 * abort seen in the field. */
static void
test_bc1_texture(Ctx &c) {
  const char *T = "bc1_texture";
  UINT support = 0;
  HRESULT hr = c.device->CheckFormatSupport(DXGI_FORMAT_BC1_UNORM, &support);
  bool supported = SUCCEEDED(hr) && (support & D3D11_FORMAT_SUPPORT_TEXTURE2D);
  emit("[INFO] bc1: support=0x%08x -> %s", support, supported ? "supported" : "unsupported");

  /* A solid BC1 block: color0 == color1 (RGB565), all indices 0. */
  auto solid_block = [](uint8_t *out, uint16_t c565) {
    out[0] = (uint8_t)(c565 & 0xff);
    out[1] = (uint8_t)(c565 >> 8);
    out[2] = out[0];
    out[3] = out[1];
    out[4] = out[5] = out[6] = out[7] = 0;
  };
  /* Block row 0: red, green; block row 1: blue, yellow. */
  uint8_t blocks[4][8];
  solid_block(blocks[0], 0xF800);
  solid_block(blocks[1], 0x07E0);
  solid_block(blocks[2], 0x001F);
  solid_block(blocks[3], 0xFFE0);

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = 8;
  td.Height = 8;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_BC1_UNORM;
  td.SampleDesc = {1, 0};
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init = {blocks, 16 /* 2 blocks per row * 8 bytes */, 0};
  Com<ID3D11Texture2D> tex;
  hr = c.device->CreateTexture2D(&td, &init, &tex);
  if (!supported) {
    /* Must fail cleanly -- reaching Metal with a BC pixel format aborts. */
    CHECK(T, FAILED(hr) && !tex);
    return;
  }
  CHECK(T, SUCCEEDED(hr), return);
  CHECK_HR(T, c.device->CreateShaderResourceView(tex, nullptr, &g_srv));
  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  CHECK_HR(T, c.device->CreateSamplerState(&sd, &g_sampler));

  const UINT W = 64, H = 64;
  auto img = draw_and_read(
      c, W, H, kQuadFull, 6, dxbc_vs_pass, sizeof(dxbc_vs_pass), dxbc_ps_tex, sizeof(dxbc_ps_tex),
      DXGI_FORMAT_R8G8B8A8_UNORM, extra_bind_tex
  );
  CHECK(T, img.size() == W * H * 4, return);
  CHECK(T, px_eq(img, W, 16, 16, 255, 0, 0, 255, 1), log_px(img, W, 16, 16, "tl"));
  CHECK(T, px_eq(img, W, 48, 16, 0, 255, 0, 255, 1), log_px(img, W, 48, 16, "tr"));
  CHECK(T, px_eq(img, W, 16, 48, 0, 0, 255, 255, 1), log_px(img, W, 16, 48, "bl"));
  CHECK(T, px_eq(img, W, 48, 48, 255, 255, 0, 255, 1), log_px(img, W, 48, 48, "br"));
  emit("[CRC] %s.rt = 0x%08x", T, crc32_of(img.data(), img.size()));

  /* Same content through the other upload paths; each draw must match the
   * IMMUTABLE image bit-for-bit. */
  auto ref = img;

  /* UpdateSubresource on a DEFAULT texture */
  {
    const char *T2 = "bc1_update";
    td.Usage = D3D11_USAGE_DEFAULT;
    Com<ID3D11Texture2D> tex2;
    CHECK_HR(T2, c.device->CreateTexture2D(&td, nullptr, &tex2));
    c.ctx->UpdateSubresource(tex2, 0, nullptr, blocks, 16, 0);
    g_srv.p->Release();
    g_srv.p = nullptr;
    CHECK_HR(T2, c.device->CreateShaderResourceView(tex2, nullptr, &g_srv));
    auto img2 = draw_and_read(
        c, W, H, kQuadFull, 6, dxbc_vs_pass, sizeof(dxbc_vs_pass), dxbc_ps_tex, sizeof(dxbc_ps_tex),
        DXGI_FORMAT_R8G8B8A8_UNORM, extra_bind_tex
    );
    CHECK(T2, img2 == ref);
  }

  /* Rebind a texture as PS resource 0 and draw the reference quad. */
  auto draw_tex = [&](ID3D11Texture2D *t) -> std::vector<uint8_t> {
    if (g_srv.p) {
      g_srv.p->Release();
      g_srv.p = nullptr;
    }
    if (FAILED(c.device->CreateShaderResourceView(t, nullptr, &g_srv)))
      return {};
    return draw_and_read(
        c, W, H, kQuadFull, 6, dxbc_vs_pass, sizeof(dxbc_vs_pass), dxbc_ps_tex, sizeof(dxbc_ps_tex),
        DXGI_FORMAT_R8G8B8A8_UNORM, extra_bind_tex
    );
  };

  /* STAGING texture written through Map, copied into a DEFAULT texture */
  {
    const char *T2 = "bc1_staging_copy";
    D3D11_TEXTURE2D_DESC sd2 = td;
    sd2.Usage = D3D11_USAGE_STAGING;
    sd2.BindFlags = 0;
    sd2.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Texture2D> stage;
    CHECK_HR(T2, c.device->CreateTexture2D(&sd2, nullptr, &stage));
    D3D11_MAPPED_SUBRESOURCE map = {};
    CHECK_HR(T2, c.ctx->Map(stage, 0, D3D11_MAP_WRITE, 0, &map));
    /* two block rows of 2 blocks (8 bytes each) */
    for (unsigned row = 0; row < 2; row++)
      memcpy((char *)map.pData + row * map.RowPitch, blocks[row * 2], 16);
    c.ctx->Unmap(stage, 0);
    td.Usage = D3D11_USAGE_DEFAULT;
    Com<ID3D11Texture2D> tex3;
    CHECK_HR(T2, c.device->CreateTexture2D(&td, nullptr, &tex3));
    c.ctx->CopyResource(tex3, stage);
    CHECK(T2, draw_tex(tex3) == ref);
  }

  /* DYNAMIC texture created with initial data: the creation path uploads
   * through replaceRegion, which must decompress under BC emulation. */
  {
    const char *T2 = "bc1_dynamic_init";
    D3D11_TEXTURE2D_DESC dd = td;
    dd.Usage = D3D11_USAGE_DYNAMIC;
    dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Texture2D> dtex;
    CHECK_HR(T2, c.device->CreateTexture2D(&dd, &init, &dtex));
    CHECK(T2, draw_tex(dtex) == ref);
  }

  /* DYNAMIC texture filled through Map(WRITE_DISCARD)/Unmap. */
  {
    const char *T2 = "bc1_dynamic_map";
    D3D11_TEXTURE2D_DESC dd = td;
    dd.Usage = D3D11_USAGE_DYNAMIC;
    dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Texture2D> dtex;
    CHECK_HR(T2, c.device->CreateTexture2D(&dd, nullptr, &dtex));
    D3D11_MAPPED_SUBRESOURCE map = {};
    CHECK_HR(T2, c.ctx->Map(dtex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map));
    for (unsigned row = 0; row < 2; row++)
      memcpy((char *)map.pData + row * map.RowPitch, blocks[row * 2], 16);
    c.ctx->Unmap(dtex, 0);
    CHECK(T2, draw_tex(dtex) == ref);
  }

  /* DYNAMIC texture written on a DEFERRED context: the Unmap must read the
   * allocation the deferred Map handed out, not the immediate name. */
  {
    const char *T2 = "bc1_dynamic_deferred";
    D3D11_TEXTURE2D_DESC dd = td;
    dd.Usage = D3D11_USAGE_DYNAMIC;
    dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Texture2D> dtex;
    CHECK_HR(T2, c.device->CreateTexture2D(&dd, nullptr, &dtex));
    Com<ID3D11DeviceContext> def;
    CHECK_HR(T2, c.device->CreateDeferredContext(0, &def));
    D3D11_MAPPED_SUBRESOURCE map = {};
    CHECK_HR(T2, def->Map(dtex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map));
    for (unsigned row = 0; row < 2; row++)
      memcpy((char *)map.pData + row * map.RowPitch, blocks[row * 2], 16);
    def->Unmap(dtex, 0);
    Com<ID3D11CommandList> cl;
    CHECK_HR(T2, def->FinishCommandList(FALSE, &cl));
    c.ctx->ExecuteCommandList(cl, FALSE);
    CHECK(T2, draw_tex(dtex) == ref);
  }

  /* A staging->default copy recorded on a DEFERRED context samples the
   * staging contents at ExecuteCommandList time, not at record time. */
  {
    const char *T2 = "bc1_staging_late_write";
    D3D11_TEXTURE2D_DESC sd2 = td;
    sd2.Usage = D3D11_USAGE_STAGING;
    sd2.BindFlags = 0;
    sd2.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Texture2D> stage;
    CHECK_HR(T2, c.device->CreateTexture2D(&sd2, nullptr, &stage));
    td.Usage = D3D11_USAGE_DEFAULT;
    Com<ID3D11Texture2D> dst;
    CHECK_HR(T2, c.device->CreateTexture2D(&td, nullptr, &dst));
    Com<ID3D11DeviceContext> def;
    CHECK_HR(T2, c.device->CreateDeferredContext(0, &def));
    def->CopyResource(dst, stage); /* recorded while `stage` is still empty */
    D3D11_MAPPED_SUBRESOURCE map = {};
    CHECK_HR(T2, c.ctx->Map(stage, 0, D3D11_MAP_WRITE, 0, &map));
    for (unsigned row = 0; row < 2; row++)
      memcpy((char *)map.pData + row * map.RowPitch, blocks[row * 2], 16);
    c.ctx->Unmap(stage, 0);
    Com<ID3D11CommandList> cl;
    CHECK_HR(T2, def->FinishCommandList(FALSE, &cl));
    c.ctx->ExecuteCommandList(cl, FALSE);
    CHECK(T2, draw_tex(dst) == ref);
  }

  /* Reinterpreting upload: BC1 blocks staged as R32G32_UINT texels (one
   * texel per block) copied into a BC1 texture -- the texture-streaming
   * alias trick MSDN permits for CopySubresourceRegion. */
  {
    const char *T2 = "bc1_uint_alias_upload";
    D3D11_TEXTURE2D_DESC ud = {};
    ud.Width = 2;
    ud.Height = 2;
    ud.MipLevels = 1;
    ud.ArraySize = 1;
    ud.Format = DXGI_FORMAT_R32G32_UINT;
    ud.SampleDesc = {1, 0};
    ud.Usage = D3D11_USAGE_STAGING;
    ud.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Texture2D> alias;
    CHECK_HR(T2, c.device->CreateTexture2D(&ud, nullptr, &alias));
    D3D11_MAPPED_SUBRESOURCE map = {};
    CHECK_HR(T2, c.ctx->Map(alias, 0, D3D11_MAP_WRITE, 0, &map));
    for (unsigned row = 0; row < 2; row++)
      memcpy((char *)map.pData + row * map.RowPitch, blocks[row * 2], 16);
    c.ctx->Unmap(alias, 0);
    td.Usage = D3D11_USAGE_DEFAULT;
    Com<ID3D11Texture2D> dst;
    CHECK_HR(T2, c.device->CreateTexture2D(&td, nullptr, &dst));
    c.ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, alias, 0, nullptr);
    CHECK(T2, draw_tex(dst) == ref);
  }

  /* STAGING -> STAGING copy keeps the block layout on both sides. */
  {
    const char *T2 = "bc1_staging_to_staging";
    D3D11_TEXTURE2D_DESC sd2 = td;
    sd2.Usage = D3D11_USAGE_STAGING;
    sd2.BindFlags = 0;
    sd2.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Texture2D> src;
    CHECK_HR(T2, c.device->CreateTexture2D(&sd2, nullptr, &src));
    D3D11_MAPPED_SUBRESOURCE map = {};
    CHECK_HR(T2, c.ctx->Map(src, 0, D3D11_MAP_WRITE, 0, &map));
    for (unsigned row = 0; row < 2; row++)
      memcpy((char *)map.pData + row * map.RowPitch, blocks[row * 2], 16);
    c.ctx->Unmap(src, 0);
    sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Texture2D> dst;
    CHECK_HR(T2, c.device->CreateTexture2D(&sd2, nullptr, &dst));
    c.ctx->CopyResource(dst, src);
    CHECK_HR(T2, c.ctx->Map(dst, 0, D3D11_MAP_READ, 0, &map));
    bool same = true;
    for (unsigned row = 0; row < 2 && same; row++)
      same = memcmp((char *)map.pData + row * map.RowPitch, blocks[row * 2], 16) == 0;
    c.ctx->Unmap(dst, 0);
    CHECK(T2, same);
  }

  /* FORMAT_SUPPORT2 must not advertise typed-UAV/atomic ops for BC formats
   * (under emulation the stand-in's capabilities must not leak through). */
  {
    const char *T2 = "bc_format_support2";
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs2 = {DXGI_FORMAT_BC7_UNORM, 0};
    HRESULT hr2 = c.device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs2, sizeof(fs2));
    emit("[INFO] bc7 format_support2 = 0x%08x (hr=0x%08x)", fs2.OutFormatSupport2, (unsigned)hr2);
    const UINT uav_bits = D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE |
                          D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD | D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
                          D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE;
    CHECK(T2, SUCCEEDED(hr2) && (fs2.OutFormatSupport2 & uav_bits) == 0);
  }

  g_srv.p->Release();
  g_srv.p = nullptr;
  g_sampler.p->Release();
  g_sampler.p = nullptr;
}


int
main(int argc, char **argv) {
  g_out = stdout;
  std::string out_path;
  if (argc > 1) {
    out_path = argv[1];
  } else if (const char *home = getenv("HOME")) {
    /* On the sandboxed platforms results also go to Documents so they can be
     * pulled off the device; harmless on a Mac when no path is given. */
    out_path = std::string(home) + "/Documents/dxmt_results.txt";
    /* Mirror stderr there too: Metal validation aborts print the failure
     * reason to stderr, and syslog redacts it as <private>. */
    std::string err_path = std::string(home) + "/Documents/dxmt_stderr.txt";
    FILE *ef = fopen(err_path.c_str(), "w");
    if (ef) {
      setvbuf(ef, nullptr, _IONBF, 0);
      dup2(fileno(ef), 2);
    }
  }
  if (!out_path.empty()) {
    FILE *f = fopen(out_path.c_str(), "w");
    if (f)
      g_out = f;
    else
      fprintf(stderr, "cannot open %s, using stdout\n", out_path.c_str());
  }
#if defined(__APPLE__)
  emit("[INFO] dxmt-native d3d11 offscreen suite");
#endif

  Ctx c;
  if (!create_device(c)) {
    emit("[SUMMARY] passed=0 failed=1 (no device)");
    return 1;
  }
  g_passes++;
  emit("[PASS] device_create");

  test_device_info(c);
  test_format_support(c);
  test_buffer_roundtrip(c);
  test_texture_roundtrip(c);
  test_draw_basic(c);
  test_compat_shapes(c);
  test_draw_deferred(c);
  test_draw_cbuffer(c);
  test_draw_textured(c);
  test_draw_instanced(c);
  test_depth(c);
  test_partial_d16_clear(c);
  test_partial_d24s8_clear(c);
  test_blend_additive(c);
  test_msaa_resolve(c);
  test_compute(c);
  test_query_event(c);
  test_fence_event(c);
  test_srgb(c);
  test_bc1_texture(c);

  emit("[SUMMARY] passed=%d failed=%d", g_passes, g_fails);
  if (g_out != stdout)
    fclose(g_out);
  return g_fails;
}
