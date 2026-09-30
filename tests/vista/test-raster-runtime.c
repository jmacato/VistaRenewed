#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

/* Exercise the public runtime, including the Video content hint used by WMC.
 * Run each of native/WOW, D3D9/Ex, and ordinary/video swap chains separately. */
static LRESULT CALLBACK WindowProc(HWND w, UINT m, WPARAM a, LPARAM b)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT p;
        BeginPaint(w, &p); EndPaint(w, &p);
        return 0;
    }
    return DefWindowProc(w, m, a, b);
}

static int CheckPixels(IDirect3DDevice9 *device)
{
    IDirect3DSurface9 *target = NULL, *cpu = NULL;
    const DWORD colors[] = {0xffff0000, 0xff00ff00, 0xff0000ff};
    HRESULT hr;
    int failed = 0;
#define CHECK(call) do { hr = (call); if (FAILED(hr)) { \
    printf("PIXELS FAIL %s hr=%08lx\n", #call, (unsigned long)hr); \
    failed = 1; goto done; } } while (0)
    CHECK(IDirect3DDevice9_GetRenderTarget(device, 0, &target));
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device, 1280, 720,
        D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &cpu, NULL));
    for (unsigned c = 0; c < 3; c++) {
        D3DLOCKED_RECT lock;
        CHECK(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, colors[c], 1, 0));
        CHECK(IDirect3DDevice9_GetRenderTargetData(device, target, cpu));
        CHECK(IDirect3DSurface9_LockRect(cpu, &lock, NULL, D3DLOCK_READONLY));
        unsigned mismatches = 0;
        for (unsigned y = 0; y < 720; y++) {
            const DWORD *row = (const DWORD *)((const BYTE *)lock.pBits + y * lock.Pitch);
            for (unsigned x = 0; x < 1280; x++)
                mismatches += (row[x] & 0xffffff) != (colors[c] & 0xffffff);
        }
        CHECK(IDirect3DSurface9_UnlockRect(cpu));
        printf("PIXELS color=%08lx checked=921600 mismatches=%u\n",
            (unsigned long)colors[c], mismatches);
        if (mismatches) failed = 1;
    }
done:
    if (cpu) IDirect3DSurface9_Release(cpu);
    if (target) IDirect3DSurface9_Release(target);
    return failed;
#undef CHECK
}

int main(int argc, char **argv)
{
    int ex = 0, video = 0, immediate = 0, failed = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ex")) ex = 1;
        else if (!strcmp(argv[i], "--video")) video = 1;
        else if (!strcmp(argv[i], "--immediate")) immediate = 1;
        else return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    IDirect3D9 *api = NULL;
    IDirect3D9Ex *apiEx = NULL;
    HRESULT hr;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "TritonRasterTest";
    if (!RegisterClassA(&wc)) return 1;
    HWND window = CreateWindowA(wc.lpszClassName, "Triton raster and synchronized presentation",
        WS_POPUP | WS_VISIBLE, 0, 0, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    if (!window) return 1;
    SetForegroundWindow(window);
    if (ex) {
        hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &apiEx);
        api = (IDirect3D9 *)apiEx;
    } else {
        api = Direct3DCreate9(D3D_SDK_VERSION);
        hr = api ? S_OK : E_FAIL;
    }
    if (FAILED(hr)) return 1;
    D3DCAPS9 caps = {0};
    hr = IDirect3D9_GetDeviceCaps(api, 0, D3DDEVTYPE_HAL, &caps);
    if (FAILED(hr) || !(caps.Caps & D3DCAPS_READ_SCANLINE) ||
        !(caps.PresentationIntervals & D3DPRESENT_INTERVAL_ONE)) return 2;
    for (unsigned rate = 60; rate <= 300; rate += 240) {
        IDirect3DDevice9 *device = NULL;
        IDirect3DDevice9Ex *deviceEx = NULL;
        D3DPRESENT_PARAMETERS p = {0};
        p.BackBufferWidth = 1280; p.BackBufferHeight = 720;
        p.BackBufferFormat = D3DFMT_X8R8G8B8; p.BackBufferCount = 1;
        p.Flags = video ? D3DPRESENTFLAG_VIDEO : 0;
        p.SwapEffect = D3DSWAPEFFECT_DISCARD; p.hDeviceWindow = window;
        p.PresentationInterval = immediate ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
        p.FullScreen_RefreshRateInHz = rate;
        D3DDISPLAYMODEEX mode = {sizeof(mode), 1280, 720, rate,
            D3DFMT_X8R8G8B8, D3DSCANLINEORDERING_PROGRESSIVE};
        if (ex) {
            hr = IDirect3D9Ex_CreateDeviceEx(apiEx, 0, D3DDEVTYPE_HAL, window,
                D3DCREATE_HARDWARE_VERTEXPROCESSING, &p, &mode, &deviceEx);
            device = (IDirect3DDevice9 *)deviceEx;
        } else hr = IDirect3D9_CreateDevice(api, 0, D3DDEVTYPE_HAL, window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &p, &device);
        if (FAILED(hr)) {
            printf("CREATE ex=%d rate=%u hr=%08lx\n", ex, rate, (unsigned long)hr);
            failed = 1; continue;
        }
        failed |= CheckPixels(device);
        unsigned blank = 0, active = 0, changes = 0, last = ~0u, invalid = 0;
        LARGE_INTEGER frequency, start, now;
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
        do {
            D3DRASTER_STATUS r;
            hr = IDirect3DDevice9_GetRasterStatus(device, 0, &r);
            if (FAILED(hr)) { failed = 1; break; }
            if (r.InVBlank) { blank++; if (r.ScanLine) invalid++; }
            else {
                active++;
                if (r.ScanLine >= 720) invalid++;
                if (r.ScanLine != last) changes++;
                last = r.ScanLine;
            }
            QueryPerformanceCounter(&now);
        } while (now.QuadPart - start.QuadPart < frequency.QuadPart / 2);
        printf("RASTER ex=%d rate=%u blank=%u active=%u changes=%u invalid=%u\n",
            ex, rate, blank, active, changes, invalid);
        if (!blank || !active || changes < 100 || invalid) failed = 1;
        QueryPerformanceCounter(&start);
        unsigned frames = 0;
        for (; frames < 120; frames++) {
            MSG m;
            while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m); DispatchMessage(&m);
            }
            hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                0xff000000 | ((frames * 3571) & 0xffffff), 1, 0);
            if (FAILED(hr)) break;
            hr = ex ? IDirect3DDevice9Ex_PresentEx(deviceEx, NULL, NULL, NULL, NULL, 0) :
                IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
            if (hr != S_OK) break;
        }
        QueryPerformanceCounter(&now);
        double seconds = (double)(now.QuadPart - start.QuadPart) / frequency.QuadPart;
        int loaded = GetModuleHandleA(sizeof(void *) == 8 ?
            "neptune_d3d9.dll" : "neptune_d3d9_wow.dll") != NULL;
        printf("INTERVAL_%s ex=%d rate=%u frames=%u seconds=%.6f fps=%.2f hr=%08lx loaded=%d begin=%llu end=%llu frequency=%llu\n",
            immediate ? "IMMEDIATE" : "ONE", ex, rate, frames, seconds, frames / seconds, (unsigned long)hr, loaded, (unsigned long long)start.QuadPart,
            (unsigned long long)now.QuadPart, (unsigned long long)frequency.QuadPart);
        if (frames != 120 || hr != S_OK || !loaded ||
            (!immediate && (frames / seconds > rate * 1.2 ||
                            (rate == 60 && frames / seconds < 50.0))))
            failed = 1;
        IDirect3DDevice9_Release(device);
    }
    IDirect3D9_Release(api); DestroyWindow(window);
    printf("RASTER-INTERVAL %s ex=%d bits=%u video=%d immediate=%d\n",
        failed ? "FAIL" : "PASS", ex, (unsigned)sizeof(void *) * 8, video, immediate);
    return failed;
}
