/* Failure-only DDI boundary tracing. Preserve callback results and arguments;
 * the checked DWM reports a generic render-thread failure otherwise. */

static HRESULT APIENTRY
triton9TraceCreateResource(HANDLE hDevice, D3DDDIARG_CREATERESOURCE *args)
{
    HRESULT hr = triton9CreateResource(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-CreateResource", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceDestroyResource(HANDLE hDevice, HANDLE hResource)
{
    HRESULT hr = triton9DestroyResource(hDevice, hResource);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-DestroyResource", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceLock(HANDLE hDevice, D3DDDIARG_LOCK *args)
{
    HRESULT hr = triton9Lock(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-Lock", (DWORD)hr);
        if (args) {
            TRITON9_RESOURCE *resource = (TRITON9_RESOURCE *)args->hResource;
            triton9DiagU32("TRITON9-LOCK-FAIL-FLAGS", args->Flags.Value);
            triton9DiagU32("TRITON9-LOCK-FAIL-INDEX", args->SubResourceIndex);
            if (triton9ResourceBelongsToDevice((TRITON9_DEVICE *)hDevice, resource)) {
                resource = triton9ResourceSurface(resource, args->SubResourceIndex);
                if (resource) {
                    DWORD state = (resource->locked ? 1u : 0u) |
                        (resource->pendingRename ? 2u : 0u) |
                        (resource->shadow ? 4u : 0u) |
                        (resource->notLockable ? 8u : 0u) |
                        (resource->writeOnly ? 16u : 0u) |
                        (resource->canDrawWhileLocked ? 32u : 0u) |
                        (resource->isBuffer ? 64u : 0u);
                    triton9DiagU32("TRITON9-LOCK-FAIL-STATE", state);
                    triton9DiagU32("TRITON9-LOCK-FAIL-POOL", resource->pool);
                    triton9DiagU32("TRITON9-LOCK-FAIL-FORMAT", resource->format);
                    triton9DiagU32("TRITON9-LOCK-FAIL-WIDTH", resource->width);
                    triton9DiagU32("TRITON9-LOCK-FAIL-HEIGHT", resource->height);
                    triton9DiagU32("TRITON9-LOCK-FAIL-LEFT", args->Area.left);
                    triton9DiagU32("TRITON9-LOCK-FAIL-TOP", args->Area.top);
                    triton9DiagU32("TRITON9-LOCK-FAIL-RIGHT", args->Area.right);
                    triton9DiagU32("TRITON9-LOCK-FAIL-BOTTOM", args->Area.bottom);
                }
            }
        }
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceUnlock(HANDLE hDevice, const D3DDDIARG_UNLOCK *args)
{
    HRESULT hr = triton9Unlock(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-Unlock", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceLockAsync(HANDLE hDevice, D3DDDIARG_LOCKASYNC *args)
{
    static LONG failures;
    HRESULT hr = triton9LockAsync(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-LockAsync", (DWORD)hr);
        if (args && InterlockedIncrement(&failures) <= 32) {
            TRITON9_RESOURCE *resource = (TRITON9_RESOURCE *)args->hResource;
            triton9DiagU32("TRITON9-ASYNC-FAIL-FLAGS", args->Flags.Value);
            triton9DiagU32("TRITON9-ASYNC-FAIL-INDEX", args->SubResourceIndex);
            if (triton9ResourceBelongsToDevice((TRITON9_DEVICE *)hDevice, resource)) {
                resource = triton9ResourceSurface(resource, args->SubResourceIndex);
                if (resource) {
                    DWORD state = (resource->locked ? 1u : 0u) |
                        (resource->pendingRename ? 2u : 0u) |
                        (resource->shadow ? 4u : 0u) |
                        (resource->notLockable ? 8u : 0u) |
                        (resource->writeOnly ? 16u : 0u) |
                        (resource->canDrawWhileLocked ? 32u : 0u) |
                        (resource->isBuffer ? 64u : 0u);
                    triton9DiagU32("TRITON9-ASYNC-FAIL-STATE", state);
                    triton9DiagU32("TRITON9-ASYNC-FAIL-POOL", resource->pool);
                    triton9DiagU32("TRITON9-ASYNC-FAIL-FORMAT", resource->format);
                    triton9DiagU32("TRITON9-ASYNC-FAIL-WIDTH", resource->width);
                }
            }
        }
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceUnlockAsync(HANDLE hDevice, const D3DDDIARG_UNLOCKASYNC *args)
{
    HRESULT hr = triton9UnlockAsync(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-UnlockAsync", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceRename(HANDLE hDevice, const D3DDDIARG_RENAME *args)
{
    HRESULT hr = triton9Rename(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-Rename", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceSetDisplayMode(HANDLE hDevice, const D3DDDIARG_SETDISPLAYMODE *args)
{
    HRESULT hr = triton9SetDisplayMode(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-SetDisplayMode", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TracePresent(HANDLE hDevice, const D3DDDIARG_PRESENT *args)
{
    HRESULT hr = triton9Present(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-Present", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceOpenResource(HANDLE hDevice, D3DDDIARG_OPENRESOURCE *args)
{
    HRESULT hr = triton9OpenResource(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-OpenResource", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceSetRenderTarget(HANDLE hDevice, const D3DDDIARG_SETRENDERTARGET *args)
{
    HRESULT hr = triton9SetRenderTarget(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-SetRenderTarget", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceSetDepthStencil(HANDLE hDevice, const D3DDDIARG_SETDEPTHSTENCIL *args)
{
    HRESULT hr = triton9SetDepthStencil(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-SetDepthStencil", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceClear(HANDLE hDevice, const D3DDDIARG_CLEAR *args, UINT rectCount, const RECT *rects)
{
    HRESULT hr = triton9Clear(hDevice, args, rectCount, rects);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-Clear", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceBlt(HANDLE hDevice, const D3DDDIARG_BLT *args)
{
    HRESULT hr = triton9Blt(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-Blt", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceBufBlt(HANDLE hDevice, const D3DDDIARG_BUFFERBLT *args)
{
    HRESULT hr = triton9BufBlt(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-BufBlt", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceTexBlt(HANDLE hDevice, const D3DDDIARG_TEXBLT *args)
{
    HRESULT hr = triton9TexBlt(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-TexBlt", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceColorFill(HANDLE hDevice, const D3DDDIARG_COLORFILL *args)
{
    HRESULT hr = triton9ColorFill(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-ColorFill", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceGenerateMipSubLevels(HANDLE hDevice, const D3DDDIARG_GENERATEMIPSUBLEVELS *args)
{
    HRESULT hr = triton9GenerateMipSubLevels(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-GenerateMipSubLevels", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceSetTexture(HANDLE hDevice, UINT stage, HANDLE resource)
{
    HRESULT hr = triton9SetTexture(hDevice, stage, resource);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-SetTexture", (DWORD)hr);
        triton9DiagU32("TRITON9-SETTEXTURE-FAIL-STAGE", stage);
        if (hDevice) {
            TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
            TRITON9_RESOURCE *texture = (TRITON9_RESOURCE *)resource;
            triton9DiagU32("TRITON9-SETTEXTURE-FAIL-IS-RT",
                           texture && texture == device->renderTarget);
            if (texture && triton9ResourceBelongsToDevice(device, texture)) {
                triton9DiagU32("TRITON9-SETTEXTURE-FAIL-FORMAT", texture->format);
                triton9DiagU32("TRITON9-SETTEXTURE-FAIL-POOL", texture->pool);
                triton9DiagU32("TRITON9-SETTEXTURE-FAIL-BIND", texture->hostBindFlags);
            }
        }
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceCreateQuery(HANDLE hDevice, D3DDDIARG_CREATEQUERY *args)
{
    HRESULT hr = triton9CreateQuery(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-CreateQuery", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceIssueQuery(HANDLE hDevice, const D3DDDIARG_ISSUEQUERY *args)
{
    HRESULT hr = triton9IssueQuery(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-IssueQuery", (DWORD)hr);
    }
    return hr;
}

static HRESULT APIENTRY
triton9TraceGetQueryData(HANDLE hDevice, const D3DDDIARG_GETQUERYDATA *args)
{
    HRESULT hr = triton9GetQueryData(hDevice, args);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-DDI-FAIL-PID", GetCurrentProcessId());
        triton9DiagU32("TRITON9-DDI-FAIL-GetQueryData", (DWORD)hr);
    }
    return hr;
}
