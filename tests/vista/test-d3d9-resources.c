/* Public-runtime pixel checks. Run both native and WOW builds in the guest. */
#define COBJMACROS
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
static int failures, checks;
#define HR(x)                                                                  \
  do {                                                                         \
    HRESULT result_ = (x);                                                     \
    ++checks;                                                                  \
    if (FAILED(result_)) {                                                     \
      printf("FAIL %s hr=%08lx line=%d\n", #x, (unsigned long)result_,         \
             __LINE__);                                                        \
      ++failures;                                                              \
      goto done;                                                               \
    }                                                                          \
  } while (0)
#define EXPECT(x)                                                              \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(x)) {                                                                \
      printf("FAIL %s line=%d\n", #x, __LINE__);                               \
      ++failures;                                                              \
    }                                                                          \
  } while (0)
static IDirect3DDevice9 *d;
static IDirect3DSurface9 *target, *cpu;
static DWORD ps2[] = {0xffff0200, 0x0200001f, 0x80000000, 0xb00f0000,
                      0x0200001f, 0x90000000, 0xa00f0800, 0x03000042,
                      0x800f0000, 0xb0e40000, 0xa0e40800, 0x02000001,
                      0x800f0800, 0x80e40000, 0x0000ffff};
static DWORD readpixel(const char *name, DWORD expected, int tolerance) {
  D3DLOCKED_RECT l;
  DWORD p = 0;
  int good = 1;
  HR(IDirect3DDevice9_GetRenderTargetData(d, target, cpu));
  HR(IDirect3DSurface9_LockRect(cpu, &l, NULL, D3DLOCK_READONLY));
  p = *(DWORD *)((BYTE *)l.pBits + 8 * l.Pitch + 8 * 4);
  HR(IDirect3DSurface9_UnlockRect(cpu));
  for (unsigned s = 0; s < 32; s += 8) {
    int delta = (int)((p >> s) & 255) - (int)((expected >> s) & 255);
    if (delta > tolerance || delta < -tolerance)
      good = 0;
  }
  printf("PIXEL %s got=%08lx expected=%08lx %s\n", name, (unsigned long)p,
         (unsigned long)expected, good ? "PASS" : "FAIL");
  EXPECT(good);
done:
  return p;
}
static void sample(IDirect3DBaseTexture9 *t, UINT kind, UINT mip, float u,
                   float v, float w, const char *name, DWORD expected,
                   int tolerance) {
  IDirect3DPixelShader9 *p = NULL;
  DWORD shader[sizeof(ps2) / sizeof(ps2[0])];
  struct {
    float x, y, z, rhw, u, v, w;
  } vertices[] = {{-.5f, -.5f, .5f, 1, u, v, w},
                  {15.5f, -.5f, .5f, 1, u, v, w},
                  {-.5f, 15.5f, .5f, 1, u, v, w},
                  {15.5f, 15.5f, .5f, 1, u, v, w}};
  memcpy(shader, ps2, sizeof(shader));
  shader[5] = kind == 3 ? 0x98000000 : kind == 4 ? 0xa0000000 : 0x90000000;
  HR(IDirect3DDevice9_CreatePixelShader(d, shader, &p));
  HR(IDirect3DDevice9_SetPixelShader(d, p));
  HR(IDirect3DDevice9_SetTexture(d, 0, t));
  HR(IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MAXMIPLEVEL, mip));
  HR(IDirect3DDevice9_BeginScene(d));
  HRESULT draw = IDirect3DDevice9_DrawPrimitiveUP(
      d, D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(vertices[0]));
  HRESULT end = IDirect3DDevice9_EndScene(d);
  HR(draw);
  HR(end);
  readpixel(name, expected, tolerance);
done:
  IDirect3DDevice9_SetTexture(d, 0, NULL);
  IDirect3DDevice9_SetPixelShader(d, NULL);
  if (p)
    IDirect3DPixelShader9_Release(p);
}
static void mipmaps(void) {
  IDirect3DTexture9 *t = NULL;
  D3DLOCKED_RECT l;
  DWORD colors[] = {0xff123456, 0xffa12345, 0xff12ab45, 0xff1256cd};
  HR(IDirect3DDevice9_CreateTexture(d, 8, 8, 4, 0, D3DFMT_A8R8G8B8,
                                    D3DPOOL_MANAGED, &t, NULL));
  EXPECT(IDirect3DTexture9_GetLevelCount(t) == 4);
  for (UINT m = 0; m < 4; m++) {
    HR(IDirect3DTexture9_LockRect(t, m, &l, NULL, 0));
    for (UINT y = 0; y < (8u >> m); y++)
      for (UINT x = 0; x < (8u >> m); x++)
        *(DWORD *)((BYTE *)l.pBits + y * l.Pitch + x * 4) = colors[m];
    HR(IDirect3DTexture9_UnlockRect(t, m));
  }
  for (UINT m = 0; m < 4; m++)
    sample((IDirect3DBaseTexture9 *)t, 2, m, .5f, .5f, 0, "2D mip", colors[m],
           0);
done:
  if (t)
    IDirect3DTexture9_Release(t);
}
static void cubes(void) {
  IDirect3DCubeTexture9 *t = NULL;
  D3DLOCKED_RECT l;
  const float xyz[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                           {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  HR(IDirect3DDevice9_CreateCubeTexture(d, 8, 4, 0, D3DFMT_A8R8G8B8,
                                        D3DPOOL_MANAGED, &t, NULL));
  for (UINT f = 0; f < 6; f++)
    for (UINT m = 0; m < 4; m++) {
      DWORD color = 0xff000000u | ((1 + f * 4 + m) * 0x030507u);
      HR(IDirect3DCubeTexture9_LockRect(t, (D3DCUBEMAP_FACES)f, m, &l, NULL,
                                        0));
      for (UINT y = 0; y < (8u >> m); y++)
        for (UINT x = 0; x < (8u >> m); x++)
          *(DWORD *)((BYTE *)l.pBits + y * l.Pitch + x * 4) = color;
      HR(IDirect3DCubeTexture9_UnlockRect(t, (D3DCUBEMAP_FACES)f, m));
    }
  for (UINT f = 0; f < 6; f++)
    for (UINT m = 0; m < 4; m++)
      sample((IDirect3DBaseTexture9 *)t, 3, m, xyz[f][0], xyz[f][1], xyz[f][2],
             "cube face/mip", 0xff000000u | ((1 + f * 4 + m) * 0x030507u), 0);
done:
  if (t)
    IDirect3DCubeTexture9_Release(t);
}
static void volumes(void) {
  IDirect3DVolumeTexture9 *t = NULL;
  D3DLOCKED_BOX l;
  HR(IDirect3DDevice9_CreateVolumeTexture(d, 4, 4, 4, 3, 0, D3DFMT_A8R8G8B8,
                                          D3DPOOL_MANAGED, &t, NULL));
  for (UINT m = 0; m < 3; m++) {
    UINT n = 4 >> m;
    HR(IDirect3DVolumeTexture9_LockBox(t, m, &l, NULL, 0));
    for (UINT z = 0; z < n; z++)
      for (UINT y = 0; y < n; y++)
        for (UINT x = 0; x < n; x++)
          *(DWORD *)((BYTE *)l.pBits + z * l.SlicePitch + y * l.RowPitch +
                     x * 4) = 0xff000000 | ((1 + m * 4 + z) * 0x070503);
    HR(IDirect3DVolumeTexture9_UnlockBox(t, m));
  }
  for (UINT m = 0; m < 3; m++)
    for (UINT z = 0; z < (4u >> m); z++)
      sample((IDirect3DBaseTexture9 *)t, 4, m, .5f, .5f, (z + .5f) / (4u >> m),
             "volume slice/mip", 0xff000000 | ((1 + m * 4 + z) * 0x070503), 0);
done:
  if (t)
    IDirect3DVolumeTexture9_Release(t);
}
static void compressed(void) {
  D3DFORMAT formats[] = {D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5};
  for (UINT i = 0; i < 3; i++) {
    IDirect3DTexture9 *t = NULL;
    D3DLOCKED_RECT l;
    BYTE block[16] = {0};
    UINT bytes = i ? 16 : 8;
    BYTE *rgb = block + (i ? 8 : 0);
    rgb[0] = 0;
    rgb[1] = 0xf8;
    rgb[2] = 0xe0;
    rgb[3] = 7;
    if (i == 1)
      memset(block, 255, 8);
    if (i == 2)
      block[0] = 255;
    HR(IDirect3DDevice9_CreateTexture(d, 4, 4, 1, 0, formats[i],
                                      D3DPOOL_MANAGED, &t, NULL));
    HR(IDirect3DTexture9_LockRect(t, 0, &l, NULL, 0));
    memcpy(l.pBits, block, bytes);
    HR(IDirect3DTexture9_UnlockRect(t, 0));
    sample((IDirect3DBaseTexture9 *)t, 2, 0, .5f, .5f, 0, "BC decoded red",
           0xffff0000, 0);
  done:
    if (t)
      IDirect3DTexture9_Release(t);
  }
}
static void formats(void) {
  struct {
    D3DFORMAT f;
    UINT size;
    BYTE data[16];
    DWORD expected;
  } cases[] = {{D3DFMT_R5G6B5, 2, {8, 0xfc}, 0xffff8242},
               {D3DFMT_A1R5G5B5, 2, {8, 0xfe}, 0xffff8442},
               {D3DFMT_X1R5G5B5, 2, {8, 0x7e}, 0xffff8442},
               {D3DFMT_A4R4G4B4, 2, {0x84, 0xff}, 0xffff8844},
               {D3DFMT_X4R4G4B4, 2, {0x84, 0x0f}, 0xffff8844},
               {D3DFMT_G16R16, 4, {255, 255, 128, 128}, 0xffff80ff},
               {D3DFMT_A8, 1, {0x80}, 0x80000000},
               {D3DFMT_L8, 1, {0x80}, 0xff808080},
               {D3DFMT_L16, 2, {0x80, 0x80}, 0xff808080},
               {D3DFMT_A8L8, 2, {0x80, 0x40}, 0x40808080},
               {D3DFMT_X8B8G8R8, 4, {0x12, 0x34, 0x56, 0}, 0xff123456},
               {D3DFMT_A2R10G10B10, 4, {0, 1, 0xf8, 0xff}, 0xffff8040},
               {D3DFMT_R16F, 2, {0, 0x38}, 0xff80ffff},
               {D3DFMT_G16R16F, 4, {0, 0x3c, 0, 0x38}, 0xffff80ff},
               {D3DFMT_A16B16G16R16F,
                8,
                {0, 0x3c, 0, 0x38, 0, 0x34, 0, 0x3c},
                0xffff8040},
               {D3DFMT_R32F, 4, {0, 0, 0, 0x3f}, 0xff80ffff}};
  for (UINT i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    IDirect3DTexture9 *t = NULL;
    D3DLOCKED_RECT l;
    HR(IDirect3DDevice9_CreateTexture(d, 1, 1, 1, 0, cases[i].f,
                                      D3DPOOL_MANAGED, &t, NULL));
    HR(IDirect3DTexture9_LockRect(t, 0, &l, NULL, 0));
    memcpy(l.pBits, cases[i].data, cases[i].size);
    HR(IDirect3DTexture9_UnlockRect(t, 0));
    sample((IDirect3DBaseTexture9 *)t, 2, 0, .5f, .5f, 0, "format sampling",
           cases[i].expected, 1);
  done:
    if (t)
      IDirect3DTexture9_Release(t);
  }
}
static void srgb(void) {
  IDirect3DTexture9 *t = NULL;
  D3DLOCKED_RECT l;
  HR(IDirect3DDevice9_CreateTexture(d, 1, 1, 1, 0, D3DFMT_A8R8G8B8,
                                    D3DPOOL_MANAGED, &t, NULL));
  HR(IDirect3DTexture9_LockRect(t, 0, &l, NULL, 0));
  *(DWORD *)l.pBits = 0xff808080;
  HR(IDirect3DTexture9_UnlockRect(t, 0));
  HR(IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_SRGBTEXTURE, TRUE));
  sample((IDirect3DBaseTexture9 *)t, 2, 0, .5f, .5f, 0, "sRGB read", 0xff373737,
         1);
  HR(IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_SRGBTEXTURE, FALSE));
  HR(IDirect3DDevice9_SetRenderState(d, D3DRS_SRGBWRITEENABLE, TRUE));
  sample((IDirect3DBaseTexture9 *)t, 2, 0, .5f, .5f, 0, "sRGB write",
         0xffbcbcbc, 1);
  HR(IDirect3DDevice9_Clear(d, 0, NULL, D3DCLEAR_TARGET, 0xff808080, 1, 0));
  readpixel("sRGB clear raw color", 0xff808080, 0);
done:
  IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_SRGBTEXTURE, FALSE);
  IDirect3DDevice9_SetRenderState(d, D3DRS_SRGBWRITEENABLE, FALSE);
  if (t)
    IDirect3DTexture9_Release(t);
}
static void msaa(void) {
  for (UINT n = 2; n <= 4; n += 2) {
    IDirect3DSurface9 *s = NULL;
    HR(IDirect3DDevice9_CreateRenderTarget(d, 16, 16, D3DFMT_A8R8G8B8,
                                           (D3DMULTISAMPLE_TYPE)n, 0, FALSE, &s,
                                           NULL));
    HR(IDirect3DDevice9_SetRenderTarget(d, 0, s));
    HR(IDirect3DDevice9_Clear(d, 0, NULL, D3DCLEAR_TARGET, 0xffe17329, 1, 0));
    HR(IDirect3DDevice9_SetRenderTarget(d, 0, target));
    HR(IDirect3DDevice9_StretchRect(d, s, NULL, target, NULL, D3DTEXF_NONE));
    readpixel("MSAA clear/resolve", 0xffe17329, 0);
  done:
    IDirect3DDevice9_SetRenderTarget(d, 0, target);
    if (s)
      IDirect3DSurface9_Release(s);
  }
}
static void mrt(void) {
  IDirect3DSurface9 *s[3] = {0};
  for (UINT i = 0; i < 3; i++) {
    HR(IDirect3DDevice9_CreateRenderTarget(d, 16, 16, D3DFMT_A8R8G8B8,
                                           D3DMULTISAMPLE_NONE, 0, FALSE, &s[i],
                                           NULL));
    HR(IDirect3DDevice9_SetRenderTarget(d, i + 1, s[i]));
  }
  HR(IDirect3DDevice9_Clear(d, 0, NULL, D3DCLEAR_TARGET, 0xff57ac31, 1, 0));
  readpixel("MRT0 clear", 0xff57ac31, 0);
  for (UINT i = 0; i < 3; i++) {
    HR(IDirect3DDevice9_SetRenderTarget(d, i + 1, NULL));
    HR(IDirect3DDevice9_StretchRect(d, s[i], NULL, target, NULL, D3DTEXF_NONE));
    readpixel("MRT1-3 clear", 0xff57ac31, 0);
  }
done:
  for (UINT i = 0; i < 3; i++) {
    IDirect3DDevice9_SetRenderTarget(d, i + 1, NULL);
    if (s[i])
      IDirect3DSurface9_Release(s[i]);
  }
}
static void uploads(void) {
  IDirect3DTexture9 *source = NULL, *dest = NULL;
  D3DLOCKED_RECT l;
  DWORD colors[] = {0xff235789, 0xff831975, 0xff759123, 0xff328a71};
  HR(IDirect3DDevice9_CreateTexture(d, 8, 8, 4, 0, D3DFMT_A8R8G8B8,
                                    D3DPOOL_SYSTEMMEM, &source, NULL));
  HR(IDirect3DDevice9_CreateTexture(d, 8, 8, 4, 0, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &dest, NULL));
  for (UINT m = 0; m < 4; m++) {
    HR(IDirect3DTexture9_LockRect(source, m, &l, NULL, 0));
    for (UINT y = 0; y < (8u >> m); y++)
      for (UINT x = 0; x < (8u >> m); x++)
        *(DWORD *)((BYTE *)l.pBits + y * l.Pitch + x * 4) = colors[m];
    HR(IDirect3DTexture9_UnlockRect(source, m));
  }
  HR(IDirect3DDevice9_UpdateTexture(d, (IDirect3DBaseTexture9 *)source,
                                    (IDirect3DBaseTexture9 *)dest));
  for (UINT m = 0; m < 4; m++)
    sample((IDirect3DBaseTexture9 *)dest, 2, m, .5f, .5f, 0,
           "UpdateTexture mip", colors[m], 0);
  RECT dirty = {4, 0, 8, 8};
  HR(IDirect3DTexture9_LockRect(source, 0, &l, &dirty, 0));
  for (UINT y = 0; y < 8; y++)
    for (UINT x = 0; x < 4; x++)
      *(DWORD *)((BYTE *)l.pBits + y * l.Pitch + x * 4) = 0xffd04172;
  HR(IDirect3DTexture9_UnlockRect(source, 0));
  HR(IDirect3DDevice9_UpdateTexture(d, (IDirect3DBaseTexture9 *)source,
                                    (IDirect3DBaseTexture9 *)dest));
  sample((IDirect3DBaseTexture9 *)dest, 2, 0, .25f, .5f, 0,
         "dirty upload preserves outside", colors[0], 0);
  sample((IDirect3DBaseTexture9 *)dest, 2, 0, .75f, .5f, 0,
         "dirty upload writes inside", 0xffd04172, 0);
done:
  if (dest)
    IDirect3DTexture9_Release(dest);
  if (source)
    IDirect3DTexture9_Release(source);
}
static void volumeUpload(void) {
  IDirect3DVolumeTexture9 *source = NULL, *dest = NULL;
  D3DLOCKED_BOX l;
  HR(IDirect3DDevice9_CreateVolumeTexture(d, 4, 4, 4, 3, 0, D3DFMT_A8R8G8B8,
                                          D3DPOOL_SYSTEMMEM, &source, NULL));
  HR(IDirect3DDevice9_CreateVolumeTexture(d, 4, 4, 4, 3, 0, D3DFMT_A8R8G8B8,
                                          D3DPOOL_DEFAULT, &dest, NULL));
  for (UINT m = 0; m < 3; m++) {
    UINT n = 4u >> m;
    HR(IDirect3DVolumeTexture9_LockBox(source, m, &l, NULL, 0));
    for (UINT z = 0; z < n; z++)
      for (UINT y = 0; y < n; y++)
        for (UINT x = 0; x < n; x++)
          *(DWORD *)((BYTE *)l.pBits + z * l.SlicePitch + y * l.RowPitch +
                     x * 4) = 0xff783421 + m;
    HR(IDirect3DVolumeTexture9_UnlockBox(source, m));
  }
  HR(IDirect3DDevice9_UpdateTexture(d, (IDirect3DBaseTexture9 *)source,
                                    (IDirect3DBaseTexture9 *)dest));
  for (UINT m = 0; m < 3; m++)
    sample((IDirect3DBaseTexture9 *)dest, 4, m, .5f, .5f, .5f,
           "volume UpdateTexture mip", 0xff783421 + m, 0);
done:
  if (dest)
    IDirect3DVolumeTexture9_Release(dest);
  if (source)
    IDirect3DVolumeTexture9_Release(source);
}
static void autogen(void) {
  IDirect3DTexture9 *t = NULL;
  D3DLOCKED_RECT l;
  HR(IDirect3DDevice9_CreateTexture(d, 8, 8, 0, D3DUSAGE_AUTOGENMIPMAP,
                                    D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t,
                                    NULL));
  EXPECT(IDirect3DTexture9_GetLevelCount(t) == 1);
  HR(IDirect3DTexture9_LockRect(t, 0, &l, NULL, 0));
  for (UINT y = 0; y < 8; y++)
    for (UINT x = 0; x < 8; x++)
      *(DWORD *)((BYTE *)l.pBits + y * l.Pitch + x * 4) = 0xff319a72;
  HR(IDirect3DTexture9_UnlockRect(t, 0));
  for (UINT f = D3DTEXF_POINT; f <= D3DTEXF_LINEAR; f++) {
    HR(IDirect3DTexture9_SetAutoGenFilterType(t, (D3DTEXTUREFILTERTYPE)f));
    IDirect3DTexture9_GenerateMipSubLevels(t);
    sample((IDirect3DBaseTexture9 *)t, 2, 3, .5f, .5f, 0, "autogen last mip",
           0xff319a72, 0);
  }
done:
  if (t)
    IDirect3DTexture9_Release(t);
}
static void renderMip(void) {
  IDirect3DTexture9 *t = NULL;
  IDirect3DSurface9 *s = NULL;
  HR(IDirect3DDevice9_CreateTexture(d, 8, 8, 4, D3DUSAGE_RENDERTARGET,
                                    D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t,
                                    NULL));
  HR(IDirect3DTexture9_GetSurfaceLevel(t, 2, &s));
  HR(IDirect3DDevice9_ColorFill(d, s, NULL, 0xff27a94b));
  sample((IDirect3DBaseTexture9 *)t, 2, 2, .5f, .5f, 0,
         "render-target mip ColorFill", 0xff27a94b, 0);
done:
  if (s)
    IDirect3DSurface9_Release(s);
  if (t)
    IDirect3DTexture9_Release(t);
}
static void dynamic(void) {
  IDirect3DTexture9 *t = NULL;
  D3DLOCKED_RECT l;
  HR(IDirect3DDevice9_CreateTexture(d, 8, 8, 4, D3DUSAGE_DYNAMIC,
                                    D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t,
                                    NULL));
  for (UINT n = 0; n < 3; n++) {
    HR(IDirect3DTexture9_LockRect(t, 0, &l, NULL, D3DLOCK_DISCARD));
    for (UINT y = 0; y < 8; y++)
      for (UINT x = 0; x < 8; x++)
        *(DWORD *)((BYTE *)l.pBits + y * l.Pitch + x * 4) = 0xff125673 + n;
    HR(IDirect3DTexture9_UnlockRect(t, 0));
    sample((IDirect3DBaseTexture9 *)t, 2, 0, .5f, .5f, 0,
           "dynamic discard rename", 0xff125673 + n, 0);
  }
done:
  if (t)
    IDirect3DTexture9_Release(t);
}
static void nonblocking(void) {
  IDirect3DSurface9 *surface = NULL;
  D3DLOCKED_RECT lock;
  HR(IDirect3DDevice9_CreateRenderTarget(d, 16, 16, D3DFMT_A8R8G8B8,
                                         D3DMULTISAMPLE_NONE, 0, TRUE, &surface,
                                         NULL));
  HR(IDirect3DDevice9_ColorFill(d, surface, NULL, 0xff29a573));
  DWORD started = GetTickCount();
  HRESULT result = D3DERR_WASSTILLDRAWING;
  while (result == D3DERR_WASSTILLDRAWING && GetTickCount() - started < 5000) {
    result = IDirect3DSurface9_LockRect(surface, &lock, NULL,
                                        D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
    if (result == D3DERR_WASSTILLDRAWING)
      Sleep(1);
  }
  HR(result);
  EXPECT(*(DWORD *)((BYTE *)lock.pBits + lock.Pitch * 8 + 8 * 4) == 0xff29a573);
  HR(IDirect3DSurface9_UnlockRect(surface));
  printf("PIXEL DONOTWAIT eventually returns current data PASS\n");
done:
  if (surface)
    IDirect3DSurface9_Release(surface);
}

int main(void) {
  HWND w = CreateWindowA("STATIC", "D3D9 resources", WS_OVERLAPPEDWINDOW, 0, 0,
                         64, 64, NULL, NULL, GetModuleHandle(NULL), NULL);
  IDirect3D9 *api = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {0};
  if (!api || !w) {
    ++failures;
    goto done;
  }
  pp.Windowed = TRUE;
  pp.hDeviceWindow = w;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = pp.BackBufferHeight = 16;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  HR(IDirect3D9_CreateDevice(api, 0, D3DDEVTYPE_HAL, w,
                             D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d));
  EXPECT(GetModuleHandleA("neptune_d3d9.dll") != NULL);
  HR(IDirect3DDevice9_CreateRenderTarget(d, 16, 16, D3DFMT_A8R8G8B8,
                                         D3DMULTISAMPLE_NONE, 0, FALSE, &target,
                                         NULL));
  HR(IDirect3DDevice9_CreateOffscreenPlainSurface(
      d, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &cpu, NULL));
  HR(IDirect3DDevice9_SetRenderTarget(d, 0, target));
  HR(IDirect3DDevice9_SetDepthStencilSurface(d, NULL));
  HR(IDirect3DDevice9_SetFVF(d, D3DFVF_XYZRHW | D3DFVF_TEX1 |
                                    D3DFVF_TEXCOORDSIZE3(0)));
  HR(IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, FALSE));
  HR(IDirect3DDevice9_SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE));
  HR(IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
  HR(IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
  HR(IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_POINT));
  mipmaps();
  cubes();
  volumes();
  compressed();
  formats();
  srgb();
  msaa();
  mrt();
  uploads();
  volumeUpload();
  autogen();
  renderMip();
  dynamic();
  nonblocking();
done:
  if (d) {
    IDirect3DDevice9_SetTexture(d, 0, NULL);
    IDirect3DDevice9_SetRenderTarget(d, 0, NULL);
  }
  if (cpu)
    IDirect3DSurface9_Release(cpu);
  if (target)
    IDirect3DSurface9_Release(target);
  if (d)
    IDirect3DDevice9_Release(d);
  if (api)
    IDirect3D9_Release(api);
  if (w)
    DestroyWindow(w);
  printf("D3D9-RESOURCES checks=%d failures=%d\n", checks, failures);
  return failures ? 1 : 0;
}
