/* Buffered, opt-in Vista tracing. The collector owns the shared section;
 * ordinary driver operation never creates it or writes diagnostic files. */
#include "triton9.h"
#include "triton_trace_wire.h"
#include "tritonSharedBridge.h"

static TRITON_TRACE_HEADER *triton9TraceHeader(TRITON9_DEVICE *device)
{
    if (!device->traceMap) {
        DWORD now = GetTickCount();
        if (device->traceTriedOpen && now - device->traceLastOpen < 1000) return NULL;
        device->traceTriedOpen = TRUE;
        device->traceLastOpen = now;
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                          TRITON_TRACE_MAPPING);
        if (!mapping) return NULL;
        void *view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
            sizeof(TRITON_TRACE_HEADER) + TRITON_TRACE_CAPACITY * sizeof(TRITON_TRACE_RECORD));
        if (!view) { CloseHandle(mapping); return NULL; }
        device->traceMap = view;
        device->traceMapHandle = mapping;
    }
    TRITON_TRACE_HEADER *header = device->traceMap;
    if (header->magic != TRITON_TRACE_MAGIC || header->version != TRITON_TRACE_VERSION ||
        header->record_size != sizeof(TRITON_TRACE_RECORD) ||
        header->capacity != TRITON_TRACE_CAPACITY) return NULL;
    return header;
}

BOOL triton9TraceEnabled(TRITON9_DEVICE *device)
{
    TRITON_TRACE_HEADER *header = triton9TraceHeader(device);
    if (header && header->reserved[2] &&
        GetTickCount() - (DWORD)header->reserved[1] >= header->reserved[2]) {
        InterlockedAnd(&header->enabled, ~1L);
        return FALSE;
    }
    return header && (InterlockedCompareExchange(&header->enabled, 0, 0) & 1);
}

/* Bit zero permits acquisition; upper bits count writers. STOP clears only
 * bit zero, then drains references. A paused producer cannot increment a
 * separate counter while the collector is resetting it for another run. */
static BOOL triton9TraceAcquire(TRITON_TRACE_HEADER *header)
{
    LONG state = InterlockedCompareExchange(&header->enabled, 0, 0);
    while (state & 1) {
        LONG observed = InterlockedCompareExchange(&header->enabled, state + 2, state);
        if (observed == state) return TRUE;
        state = observed;
    }
    return FALSE;
}

void triton9TraceEventAt(TRITON9_DEVICE *device, UINT kind, UINT64 ticks,
                        UINT64 a, UINT64 b, UINT64 c)
{
    TRITON_TRACE_HEADER *header = device->traceMap;
    if (!device->traceFrame || !header) return;
    if (!triton9TraceAcquire(header)) return;
    if (device->traceRun != header->run) {
        InterlockedExchangeAdd(&header->enabled, -2);
        return;
    }
    LONG index = InterlockedIncrement(&header->count) - 1;
    if ((ULONG)index < header->capacity) {
        TRITON_TRACE_RECORD *r = (TRITON_TRACE_RECORD *)(header + 1) + index;
        r->ticks = ticks; r->run = device->traceRun;
        r->frame = device->traceFrame; r->command = 0;
        r->arg0 = a; r->arg1 = b; r->arg2 = c;
        r->kind = kind; r->pid = GetCurrentProcessId();
        r->tid = GetCurrentThreadId(); r->context = device->traceContext;
        r->seq = index;
    } else InterlockedIncrement(&header->dropped);
    InterlockedExchangeAdd(&header->enabled, -2);
}

void triton9TraceEvent(TRITON9_DEVICE *device, UINT kind, UINT64 a, UINT64 b, UINT64 c)
{
    if (!device->traceFrame) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    triton9TraceEventAt(device, kind, now.QuadPart, a, b, c);
}

void triton9TraceBegin(TRITON9_DEVICE *device, UINT64 entryTicks, UINT flags, UINT kind)
{
    device->traceFrame = 0;
    if (!triton9TraceEnabled(device)) return;
    TRITON_TRACE_HEADER *header = device->traceMap;
    if (!triton9TraceAcquire(header)) return;
    device->traceRun = header->run;
    device->traceFrame = InterlockedIncrement64(&header->next_frame);
    device->traceGpuEvery = header->reserved[4] >= 1 && header->reserved[4] <= 256
        ? (UINT)header->reserved[4] : 1;
    InterlockedExchangeAdd(&header->enabled, -2);
    TRITON_TRACE_REQUEST request;
    D3DDDICB_ESCAPE callback;
    ZeroMemory(&request, sizeof(request)); ZeroMemory(&callback, sizeof(callback));
    request.type = TRITON_TRACE_ESCAPE_TYPE;
    request.length = sizeof(request) - 4;
    request.operation = TT_MARK;
    request.run = device->traceRun; request.frame = device->traceFrame;
    request.arg0 = flags; request.arg1 = entryTicks;
    callback.hDevice = device->hRTDevice;
    callback.pPrivateDriverData = &request;
    callback.PrivateDriverDataSize = sizeof(request);
    HRESULT hr = device->callbacks.pfnEscapeCb
        ? device->callbacks.pfnEscapeCb(device->adapter->hRTAdapter, &callback) : E_NOTIMPL;
    device->traceContext = SUCCEEDED(hr) ? (UINT)request.arg0 : 0;
    /* A rejected trace marker makes the capture incomplete, not the device
     * lost. Instrumentation must not change an application's result. */
    triton9TraceEventAt(device, kind, entryTicks, flags, (UINT)hr, sizeof(void *) * 8);
}

void triton9TraceClose(TRITON9_DEVICE *device)
{
    if (device->traceMap) UnmapViewOfFile(device->traceMap);
    if (device->traceMapHandle) CloseHandle(device->traceMapHandle);
    device->traceMap = NULL; device->traceMapHandle = NULL;
}

/* Timestamp queries cover the GPU command interval queued between consecutive
 * Presents. They are hardware-clock spans, not CPU waits or GPU busy time.
 * The first frame of a capture is warm-up and has no earlier begin timestamp. */
void triton9TraceGpuFinish(TRITON9_DEVICE *device)
{
    if (!device->traceFrame || !device->traceGpuArmed ||
        device->traceGpuRun != device->traceRun) return;
    ID3D11DeviceContext1_End(device->hostContext, (ID3D11Asynchronous *)device->traceGpuEnd);
    ID3D11DeviceContext1_End(device->hostContext, (ID3D11Asynchronous *)device->traceGpuDisjoint);
}

void triton9TraceGpuResolve(TRITON9_DEVICE *device)
{
    if (!device->traceFrame) return;
    UINT64 begin = 0, end = 0;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT info;
    ZeroMemory(&info, sizeof(info));
    HRESULT hr = S_FALSE;
    if (device->traceGpuArmed && device->traceGpuRun == device->traceRun) {
        hr = tritonSharedBridgeTraceQueryRead(device->hostContext,
            device->traceGpuDisjoint, &info, sizeof(info));
        if (hr == S_OK) hr = tritonSharedBridgeTraceQueryRead(device->hostContext,
            device->traceGpuBegin, &begin, sizeof(begin));
        if (hr == S_OK) hr = tritonSharedBridgeTraceQueryRead(device->hostContext,
            device->traceGpuEnd, &end, sizeof(end));
    }
    if (hr == S_OK && !info.Disjoint && info.Frequency && end >= begin)
        triton9TraceEvent(device, TT_GPU_SPAN, begin, end, info.Frequency);
    else
        triton9TraceEvent(device, TT_GPU_UNAVAILABLE, (UINT)hr, info.Disjoint,
                           device->traceGpuArmed ? 1 : 2);
    if (device->traceGpuArmed && device->traceGpuRun != device->traceRun) {
        ID3D11DeviceContext1_End(device->hostContext, (ID3D11Asynchronous *)device->traceGpuDisjoint);
        triton9TraceGpuRelease(device);
    }
    device->traceGpuArmed = FALSE;
}

void triton9TraceGpuRelease(TRITON9_DEVICE *device)
{
    if (device->traceGpuBegin) ID3D11Query_Release(device->traceGpuBegin);
    if (device->traceGpuEnd) ID3D11Query_Release(device->traceGpuEnd);
    if (device->traceGpuDisjoint) ID3D11Query_Release(device->traceGpuDisjoint);
    device->traceGpuBegin = device->traceGpuEnd = device->traceGpuDisjoint = NULL;
    device->traceGpuArmed = FALSE;
}

void triton9TraceGpuStart(TRITON9_DEVICE *device)
{
    if (!device->traceFrame || !triton9TraceEnabled(device) || device->deviceLost) return;
    if (device->traceGpuSampleRun != device->traceRun) {
        device->traceGpuSampleRun = device->traceRun;
        device->traceGpuSampleCount = 0;
    }
    /* Sample one frame interval, not the whole gap between samples. Finish
     * and resolve run at the immediately following Present in either mode. */
    if (device->traceGpuSampleCount++ % device->traceGpuEvery) return;
    if (!device->traceGpuBegin) {
        D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP, 0};
        HRESULT hr = ID3D11Device1_CreateQuery(device->hostDevice, &desc, &device->traceGpuBegin);
        if (SUCCEEDED(hr)) hr = ID3D11Device1_CreateQuery(device->hostDevice, &desc, &device->traceGpuEnd);
        desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (SUCCEEDED(hr)) hr = ID3D11Device1_CreateQuery(device->hostDevice, &desc, &device->traceGpuDisjoint);
        if (FAILED(hr)) { triton9TraceGpuRelease(device); return; }
    }
    device->traceGpuRun = device->traceRun;
    ID3D11DeviceContext1_Begin(device->hostContext, (ID3D11Asynchronous *)device->traceGpuDisjoint);
    ID3D11DeviceContext1_End(device->hostContext, (ID3D11Asynchronous *)device->traceGpuBegin);
    device->traceGpuArmed = TRUE;
}
