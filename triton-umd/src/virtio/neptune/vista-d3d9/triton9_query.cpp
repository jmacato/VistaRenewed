/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Vista D3D9 event and occlusion queries.  The internal D3D11 proxy reaches
 * Neptune's renderer, whose drain path appends the private render-event
 * command and waits for KMD retirement with a finite timeout.
 */

#include "triton9.h"

namespace {

struct Triton9Query {
    TRITON9_DEVICE *device;
    Triton9Query *next;
    ID3D11Query *hostQuery;
    D3DDDIQUERYTYPE type;
    BOOL active;
    BOOL ended;
};

static D3D11_QUERY
triton9QueryType(D3DDDIQUERYTYPE type)
{
    return type == D3DDDIQUERYTYPE_EVENT ? D3D11_QUERY_EVENT
                                         : D3D11_QUERY_OCCLUSION;
}

/* The caller owns shaderLock. */
static Triton9Query *
triton9FindQuery(TRITON9_DEVICE *device, HANDLE handle, Triton9Query **previous)
{
    Triton9Query *current;
    Triton9Query *prior = nullptr;

    if (previous)
        *previous = nullptr;
    if (!device || !handle)
        return nullptr;
    current = static_cast<Triton9Query *>(device->queries);
    while (current) {
        if (current == handle) {
            if (previous)
                *previous = prior;
            return current;
        }
        prior = current;
        current = current->next;
    }
    return nullptr;
}

static HRESULT
triton9MapQueryFailure(TRITON9_DEVICE *device, HRESULT hr)
{
    return triton9MapDeviceFailure(device, hr);
}

} /* namespace */

extern "C" HRESULT APIENTRY
triton9CreateQuery(HANDLE hDevice, D3DDDIARG_CREATEQUERY *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    Triton9Query *query;
    D3D11_QUERY_DESC desc;
    HRESULT hr;

    if (!device || !args || !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (args->QueryType != D3DDDIQUERYTYPE_EVENT &&
        args->QueryType != D3DDDIQUERYTYPE_OCCLUSION)
        return D3DDDIERR_NOTAVAILABLE;
    /* Reject an unsupported query before lazy host-device startup.  Invalid
     * input must not have a transport side effect. */
    hr = triton9EnsureHostDevice(device);
    if (FAILED(hr))
        return hr;
    query = static_cast<Triton9Query *>(HeapAlloc(GetProcessHeap(),
                                                   HEAP_ZERO_MEMORY,
                                                   sizeof(*query)));
    if (!query)
        return E_OUTOFMEMORY;
    ZeroMemory(&desc, sizeof(desc));
    desc.Query = triton9QueryType(args->QueryType);
    EnterCriticalSection(&device->shaderLock);
    hr = device->hostDevice->CreateQuery(&desc, &query->hostQuery);
    if (SUCCEEDED(hr) && query->hostQuery)
        hr = triton9CheckHostDevice(device);
    if (SUCCEEDED(hr) && query->hostQuery) {
        query->device = device;
        query->type = args->QueryType;
        query->next = static_cast<Triton9Query *>(device->queries);
        device->queries = query;
        args->hQuery = query;
    }
    LeaveCriticalSection(&device->shaderLock);
    if (FAILED(hr) || !query->hostQuery) {
        if (query->hostQuery) {
            query->hostQuery->Release();
            query->hostQuery = nullptr;
        }
        HeapFree(GetProcessHeap(), 0, query);
        return FAILED(hr) ? triton9MapQueryFailure(device, hr) : E_FAIL;
    }
    return S_OK;
}

extern "C" HRESULT APIENTRY
triton9DestroyQuery(HANDLE hDevice, const HANDLE handle)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    Triton9Query *query;
    Triton9Query *previous;

    if (!device || !handle || !device->shaderLockInitialized)
        return E_INVALIDARG;
    EnterCriticalSection(&device->shaderLock);
    query = triton9FindQuery(device, handle, &previous);
    if (!query || query->device != device) {
        LeaveCriticalSection(&device->shaderLock);
        return D3DDDIERR_INVALIDCALL;
    }
    if (previous)
        previous->next = query->next;
    else
        device->queries = query->next;
    if (query->hostQuery) {
        query->hostQuery->Release();
        query->hostQuery = nullptr;
    }
    HRESULT hr = triton9CheckHostDevice(device);
    LeaveCriticalSection(&device->shaderLock);
    HeapFree(GetProcessHeap(), 0, query);
    return hr;
}

extern "C" HRESULT APIENTRY
triton9IssueQuery(HANDLE hDevice, const D3DDDIARG_ISSUEQUERY *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    Triton9Query *query;
    HRESULT hr;

    if (!device || !args || !args->hQuery || !device->hostContext ||
        !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if ((!args->Flags.Begin && !args->Flags.End) ||
        (args->Flags.Begin && args->Flags.End) ||
        (args->Flags.Value & ~3u))
        return D3DDDIERR_INVALIDCALL;
    EnterCriticalSection(&device->shaderLock);
    query = triton9FindQuery(device, args->hQuery, nullptr);
    if (!query || query->device != device || !query->hostQuery) {
        LeaveCriticalSection(&device->shaderLock);
        return D3DDDIERR_INVALIDCALL;
    }
    if (query->type == D3DDDIQUERYTYPE_EVENT) {
        if (!args->Flags.End) {
            LeaveCriticalSection(&device->shaderLock);
            return D3DDDIERR_INVALIDCALL;
        }
        device->hostContext->End(query->hostQuery);
        query->active = FALSE;
        query->ended = TRUE;
    } else {
        if (args->Flags.Begin) {
            if (query->active) {
                LeaveCriticalSection(&device->shaderLock);
                return D3DDDIERR_INVALIDCALL;
            }
            device->hostContext->Begin(query->hostQuery);
            query->active = TRUE;
            query->ended = FALSE;
        } else {
            if (!query->active) {
                LeaveCriticalSection(&device->shaderLock);
                return D3DDDIERR_INVALIDCALL;
            }
            device->hostContext->End(query->hostQuery);
            query->active = FALSE;
            query->ended = TRUE;
        }
    }
    hr = triton9CheckHostDevice(device);
    LeaveCriticalSection(&device->shaderLock);
    return hr;
}

extern "C" HRESULT APIENTRY
triton9GetQueryData(HANDLE hDevice, const D3DDDIARG_GETQUERYDATA *args)
{
    TRITON9_DEVICE *device = static_cast<TRITON9_DEVICE *>(hDevice);
    Triton9Query *query;
    BOOL eventResult = FALSE;
    DWORD occlusionResult = 0;
    BOOL publishResult = FALSE;
    HRESULT hr;

    if (!device || !args || !args->hQuery || !args->pData ||
        !device->hostContext || !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    EnterCriticalSection(&device->shaderLock);
    query = triton9FindQuery(device, args->hQuery, nullptr);
    if (!query || query->device != device || !query->hostQuery || !query->ended) {
        LeaveCriticalSection(&device->shaderLock);
        return D3DDDIERR_INVALIDCALL;
    }
    if (query->type == D3DDDIQUERYTYPE_EVENT) {
        BOOL complete = FALSE;
        hr = device->hostContext->GetData(query->hostQuery, &complete,
                                          sizeof(complete), 0);
        /* S_OK is a completion result at the Vista DDI boundary.  Do not turn
         * an inconsistent host S_OK/FALSE pair into a completed event query. */
        if (hr == S_OK && complete) {
            eventResult = TRUE;
            publishResult = TRUE;
        } else if (hr == S_OK) {
            hr = S_FALSE;
        }
    } else {
        UINT64 samples = 0;
        hr = device->hostContext->GetData(query->hostQuery, &samples,
                                          sizeof(samples), 0);
        if (hr == S_OK) {
            occlusionResult = samples > 0xffffffffu
                ? 0xffffffffu : static_cast<DWORD>(samples);
            publishResult = TRUE;
        }
    }
    /* A local feedback slot can report a completed query without another wire
     * call.  Do not publish that success after a different asynchronous submit
     * poisoned either the context ring or the primary ring. */
    if (hr == S_OK)
        hr = triton9CheckHostDevice(device);
    if (hr == S_OK && publishResult) {
        if (query->type == D3DDDIQUERYTYPE_EVENT)
            *static_cast<BOOL *>(args->pData) = eventResult;
        else
            *static_cast<DWORD *>(args->pData) = occlusionResult;
    }
    LeaveCriticalSection(&device->shaderLock);
    return triton9MapQueryFailure(device, hr);
}

extern "C" void
triton9DestroyAllQueries(TRITON9_DEVICE *device)
{
    Triton9Query *query;

    if (!device || !device->shaderLockInitialized)
        return;
    EnterCriticalSection(&device->shaderLock);
    query = static_cast<Triton9Query *>(device->queries);
    device->queries = nullptr;
    while (query) {
        Triton9Query *next = query->next;
        if (query->hostQuery)
            query->hostQuery->Release();
        HeapFree(GetProcessHeap(), 0, query);
        query = next;
    }
    LeaveCriticalSection(&device->shaderLock);
}
