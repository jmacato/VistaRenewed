/* GPU-filtered StretchRect, isolated from the caller's D3D9 pipeline. */
#include "triton9.h"
#include "../triton/tritonBlitShaders.h"

typedef struct TRITON9_STRETCH_STATE {
    ID3D11DeviceContext *context;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11PixelShader *formatPS[3];
    ID3D11SamplerState *samplers[2];
    ID3D11Buffer *constants;
} TRITON9_STRETCH_STATE;

void triton9ReleaseStretchBlit(TRITON9_DEVICE *device)
{
    TRITON9_STRETCH_STATE *state = device->stretchBlitState;
    if (!state) return;
    if (state->context) ID3D11DeviceContext_Release(state->context);
    if (state->vs) ID3D11VertexShader_Release(state->vs);
    if (state->ps) ID3D11PixelShader_Release(state->ps);
    for (UINT i = 0; i < 3; ++i)
        if (state->formatPS[i]) ID3D11PixelShader_Release(state->formatPS[i]);
    for (UINT i = 0; i < 2; ++i)
        if (state->samplers[i]) ID3D11SamplerState_Release(state->samplers[i]);
    if (state->constants) ID3D11Buffer_Release(state->constants);
    HeapFree(GetProcessHeap(), 0, state);
    device->stretchBlitState = NULL;
}

static HRESULT triton9EnsureStretchBlit(TRITON9_DEVICE *device)
{
    TRITON9_STRETCH_STATE *state;
    D3D11_BUFFER_DESC buffer = {0};
    D3D11_SUBRESOURCE_DATA data = {0};
    const FLOAT transform[4] = {1, 1, 0, 0};
    HRESULT hr;
    if (device->stretchBlitState) return S_OK;
    state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
    if (!state) return E_OUTOFMEMORY;
    device->stretchBlitState = state;
    hr = ID3D11Device1_CreateDeferredContext(device->hostDevice, 0, &state->context);
    if (FAILED(hr)) goto fail;
    hr = ID3D11Device1_CreateVertexShader(device->hostDevice, g_tritonBlitVS,
            sizeof(g_tritonBlitVS), NULL, &state->vs);
    if (FAILED(hr)) goto fail;
    hr = ID3D11Device1_CreatePixelShader(device->hostDevice, g_tritonBlitPS,
            sizeof(g_tritonBlitPS), NULL, &state->ps);
    if (FAILED(hr)) goto fail;
    {
        const void *code[3] = { g_tritonBlitRaaaPS, g_tritonBlitRgaaPS, g_tritonBlitRgbOnePS };
        const SIZE_T size[3] = { sizeof(g_tritonBlitRaaaPS), sizeof(g_tritonBlitRgaaPS),
            sizeof(g_tritonBlitRgbOnePS) };
        for (UINT i = 0; i < 3; ++i) {
            hr = ID3D11Device1_CreatePixelShader(device->hostDevice, code[i], size[i], NULL, &state->formatPS[i]);
            if (FAILED(hr)) goto fail;
        }
    }
    for (UINT i = 0; i < 2; ++i) {
        D3D11_SAMPLER_DESC sampler = {0};
        sampler.Filter = i ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        hr = ID3D11Device1_CreateSamplerState(device->hostDevice, &sampler, &state->samplers[i]);
        if (FAILED(hr)) goto fail;
    }
    buffer.ByteWidth = sizeof(transform);
    buffer.Usage = D3D11_USAGE_IMMUTABLE;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    data.pSysMem = transform;
    hr = ID3D11Device1_CreateBuffer(device->hostDevice, &buffer, &data, &state->constants);
    if (SUCCEEDED(hr)) return hr;
fail:
    triton9ReleaseStretchBlit(device);
    return hr;
}

HRESULT triton9StretchBlt(TRITON9_DEVICE *device, TRITON9_RESOURCE *source,
                          TRITON9_RESOURCE *destination,
                          const RECT *sourceRect, const RECT *destinationRect,
                          BOOL linear)
{
    TRITON9_STRETCH_STATE *state;
    ID3D11Texture2D *input = NULL, *output = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11CommandList *commands = NULL;
    ID3D11PixelShader *pixelShader;
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_VIEWPORT viewport = {0};
    D3D11_BOX box;
    HRESULT hr;

    /* Called under shaderLock after ownership, geometry and format checks.
     * Snapshot the selected source rectangle so sampling clamps to its edges,
     * even for an interior crop or an overlapping self-blit. Temporary views
     * also avoid requiring bind flags that the public resource never had. */
    hr = triton9EnsureStretchBlit(device);
    if (FAILED(hr)) return hr;
    state = device->stretchBlitState;
    pixelShader = state->ps;
    switch (source->format) {
    case D3DDDIFMT_R16F: case D3DDDIFMT_R32F:
        pixelShader = state->formatPS[0]; break;
    case D3DDDIFMT_G16R16: case D3DDDIFMT_G16R16F: case D3DDDIFMT_G32R32F:
        pixelShader = state->formatPS[1]; break;
    case D3DDDIFMT_X8B8G8R8: case D3DDDIFMT_X1R5G5B5: case D3DDDIFMT_X4R4G4B4:
        pixelShader = state->formatPS[2]; break;
    default: break;
    }
    desc.Width = sourceRect->right - sourceRect->left;
    desc.Height = sourceRect->bottom - sourceRect->top;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = source->hostFormat;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device1_CreateTexture2D(device->hostDevice, &desc, NULL, &input);
    if (FAILED(hr)) goto done;
    desc.Width = destinationRect->right - destinationRect->left;
    desc.Height = destinationRect->bottom - destinationRect->top;
    desc.Format = destination->hostFormat;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = ID3D11Device1_CreateTexture2D(device->hostDevice, &desc, NULL, &output);
    if (FAILED(hr)) goto done;
    hr = ID3D11Device1_CreateShaderResourceView(device->hostDevice,
                                               (ID3D11Resource *)input, NULL, &srv);
    if (FAILED(hr)) goto done;
    hr = ID3D11Device1_CreateRenderTargetView(device->hostDevice,
                                             (ID3D11Resource *)output, NULL, &rtv);
    if (FAILED(hr)) goto done;
    box.left = sourceRect->left; box.top = sourceRect->top;
    box.right = sourceRect->right; box.bottom = sourceRect->bottom;
    box.front = 0; box.back = 1;
    if (triton9ResourceIsSystemMemory(source)) {
        D3D11_BOX uploadBox = { 0, 0, 0, (UINT)(sourceRect->right - sourceRect->left),
            (UINT)(sourceRect->bottom - sourceRect->top), 1 };
        hr = triton9UpdateHostTexture(device, source, state->context,
            (ID3D11Resource *)input, 0, &uploadBox,
            source->shadow + (SIZE_T)sourceRect->top * source->pitch +
                (SIZE_T)sourceRect->left * source->bytesPerPixel,
            source->pitch, source->slicePitch);
        if (FAILED(hr)) goto done;
    } else {
        ID3D11DeviceContext_CopySubresourceRegion(state->context, (ID3D11Resource *)input,
            0, 0, 0, 0, source->hostResource, source->subresourceIndex, &box);
    }
    viewport.Width = (FLOAT)desc.Width; viewport.Height = (FLOAT)desc.Height;
    viewport.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(state->context, 1, &viewport);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 1, &rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(state->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(state->context, state->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(state->context, pixelShader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(state->context, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(state->context, 0, 1, &state->samplers[linear ? 1 : 0]);
    ID3D11DeviceContext_PSSetConstantBuffers(state->context, 0, 1, &state->constants);
    ID3D11DeviceContext_Draw(state->context, 3, 0);
    ID3D11DeviceContext_OMSetRenderTargets(state->context, 0, NULL, NULL);
    ID3D11DeviceContext_CopySubresourceRegion(state->context,
        triton9ResourceIsSystemMemory(destination) ? destination->stagingResource
                                                  : destination->hostResource,
        triton9ResourceIsSystemMemory(destination) ? 0 : destination->subresourceIndex,
        destinationRect->left, destinationRect->top, 0, (ID3D11Resource *)output, 0, NULL);
    hr = ID3D11DeviceContext_FinishCommandList(state->context, FALSE, &commands);
    if (SUCCEEDED(hr)) {
        /* RestoreContextState preserves every app binding, including states
         * not shadowed by the Vista UMD. Finish(FALSE) resets our recorder. */
        ID3D11DeviceContext1_ExecuteCommandList(device->hostContext, commands, TRUE);
        triton9ResourceWritten(destination);
        hr = triton9CheckHostDevice(device);
    } else {
        /* Discard a failed recording before a later request can reuse it. */
        triton9ReleaseStretchBlit(device);
    }
done:
    if (commands) ID3D11CommandList_Release(commands);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (output) ID3D11Texture2D_Release(output);
    if (input) ID3D11Texture2D_Release(input);
    return hr;
}
