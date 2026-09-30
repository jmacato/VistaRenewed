#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

/* A presentation diagnostic, not a throughput benchmark. Compare the host
 * screenshot during each hold with both the requested colour and GPU readback. */
static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        BeginPaint(window, &paint); EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProc(window, message, w, l);
}

int main(int argc, char **argv)
{
    const int doReadback = argc == 1;
    const int hideCursor = argc == 2 && !strcmp(argv[1], "--hide-cursor");
    if (argc > 1 && (argc != 2 || strcmp(argv[1], "--present-only") && !hideCursor)) return 2;
    WNDCLASSA windowClass = {0};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandle(NULL);
    windowClass.lpszClassName = "TritonFullscreenDiagnostic";
    if (!RegisterClassA(&windowClass)) return 1;
    HWND window = CreateWindowA(windowClass.lpszClassName, "Triton fullscreen presentation",
        WS_POPUP | WS_VISIBLE, 0, 0, 1280, 720, NULL, NULL, GetModuleHandle(NULL), NULL);
    IDirect3D9 *api = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9 *device = NULL;
    IDirect3DSurface9 *target = NULL, *readback = NULL;
    D3DPRESENT_PARAMETERS pp = {0};
    const DWORD colours[] = {0xffff0000, 0xff00ff00, 0xff0000ff};
    HRESULT hr = E_FAIL;
    int failed = 0;
#define CHECK(call) do { hr = (call); if (FAILED(hr)) { printf("FAIL %s hr=%08lx\n", #call, (unsigned long)hr); failed=1; goto done; } } while (0)
    if (!window || !api) { failed=1; goto done; }
    if (hideCursor) while (ShowCursor(FALSE) >= 0) {}
    SetForegroundWindow(window);
    UpdateWindow(window);
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 720;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    CHECK(IDirect3D9_CreateDevice(api, 0, D3DDEVTYPE_HAL, window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
    if (doReadback)
        CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device, 1280, 720,
            D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, NULL));
    for (unsigned i=0; i<3; ++i) {
        D3DLOCKED_RECT mapped;
        DWORD pixel = 0;
        CHECK(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, colours[i], 1, 0));
        if (doReadback) {
        CHECK(IDirect3DDevice9_GetRenderTarget(device, 0, &target));
        printf("FULLSCREEN phase=%u before-getdata tick=%lu\n", i, (unsigned long)GetTickCount()); fflush(stdout);
        CHECK(IDirect3DDevice9_GetRenderTargetData(device, target, readback));
        printf("FULLSCREEN phase=%u after-getdata tick=%lu\n", i, (unsigned long)GetTickCount()); fflush(stdout);
        IDirect3DSurface9_Release(target); target=NULL;
        CHECK(IDirect3DSurface9_LockRect(readback, &mapped, NULL, D3DLOCK_READONLY));
        printf("FULLSCREEN phase=%u after-lock tick=%lu\n", i, (unsigned long)GetTickCount()); fflush(stdout);
        pixel = *(DWORD *)((BYTE *)mapped.pBits + 360*mapped.Pitch + 640*4);
        CHECK(IDirect3DSurface9_UnlockRect(readback));
        if ((pixel & 0xffffff) != (colours[i] & 0xffffff)) failed=1;
        }
        CHECK(IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL));
        printf("FULLSCREEN hold=%u expected=%08lx readback=%08lx enabled=%d; inspect scanout for 8 seconds\n",
            i, (unsigned long)colours[i], (unsigned long)pixel, doReadback);
        fflush(stdout);
        for (unsigned tick=0; tick<80; ++tick) {
            MSG message;
            while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessage(&message);
            }
            Sleep(100);
        }
    }
done:
    if (target) IDirect3DSurface9_Release(target);
    if (readback) IDirect3DSurface9_Release(readback);
    if (device) IDirect3DDevice9_Release(device);
    if (api) IDirect3D9_Release(api);
    if (hideCursor) while (ShowCursor(TRUE) < 0) {}
    if (window) DestroyWindow(window);
    printf("FULLSCREEN calls %s readback_enabled=%d; scanout requires independent validation\n", failed ? "FAIL" : "PASS", doReadback);
    return failed;
}
