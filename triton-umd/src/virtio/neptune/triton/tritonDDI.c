/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DDI thunk declarations + per-tier _DEVICEFUNCS tables, plus the
 * OpenAdapter10_2 export and adapter-level callbacks.
 *
 * OpenAdapter10_2 is the single entry point the WDDM runtime calls to
 * discover our adapter. It returns a D3D10_2DDI_ADAPTERFUNCS table whose
 * pfnCreateDevice fills in the device function table for the requested
 * DDI interface version.
 */

#include "triton.h"
#include "triton_log.h"
/* npt_log for the DestroyDevice drain timeout: unbuffered WriteFile, so the
 * report survives a run with no debugger attached and output redirected. */
#include "npt_common.h"
#include "tritonDxgi.h"
#include "tritonPresent.h"

/* Statically linked from src/virtio/neptune/npt_entry_d3d11.c.  The
 * function pulls in the protocol-generated COM machinery internally;
 * declaring the extern here lets tritonCreateDevice call it without
 * dragging the protocol headers into Triton's SDK-flavoured include
 * graph. */
extern HRESULT
npt_d3d11_create_device_internal(IDXGIAdapter *pAdapter,
                                 D3D_DRIVER_TYPE DriverType,
                                 HMODULE Software,
                                 UINT Flags,
                                 const D3D_FEATURE_LEVEL *pFeatureLevels,
                                 UINT FeatureLevels,
                                 UINT SDKVersion,
                                 ID3D11Device **ppDevice,
                                 D3D_FEATURE_LEVEL *pFeatureLevel,
                                 ID3D11DeviceContext **ppImmediateContext);

/* Forward declarations of handlers implemented in other translation
 * units. APIENTRY signatures keep the function-table installs free of
 * casts where possible. */

/* tritonResource.c */
SIZE_T APIENTRY tritonCalcPrivateResourceSize(D3D10DDI_HDEVICE,
                                              const D3D11DDIARG_CREATERESOURCE *);
SIZE_T APIENTRY tritonCalcPrivateOpenedResourceSize(D3D10DDI_HDEVICE,
                                                    const D3D10DDIARG_OPENRESOURCE *);
void   APIENTRY tritonCreateResource(D3D10DDI_HDEVICE,
                                     const D3D11DDIARG_CREATERESOURCE *, D3D10DDI_HRESOURCE, D3D10DDI_HRTRESOURCE);
void   APIENTRY tritonDestroyResource(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE);
void   APIENTRY tritonOpenResource(D3D10DDI_HDEVICE, const D3D10DDIARG_OPENRESOURCE *, D3D10DDI_HRESOURCE, D3D10DDI_HRTRESOURCE);

/* tritonView.c */
SIZE_T APIENTRY tritonCalcPrivateRenderTargetViewSize(D3D10DDI_HDEVICE,
                                                      const D3D10DDIARG_CREATERENDERTARGETVIEW *);
void   APIENTRY tritonCreateRenderTargetView(D3D10DDI_HDEVICE,
                                             const D3D10DDIARG_CREATERENDERTARGETVIEW *, D3D10DDI_HRENDERTARGETVIEW, D3D10DDI_HRTRENDERTARGETVIEW);
void   APIENTRY tritonDestroyRenderTargetView(D3D10DDI_HDEVICE, D3D10DDI_HRENDERTARGETVIEW);
void   APIENTRY tritonClearRenderTargetView(D3D10DDI_HDEVICE, D3D10DDI_HRENDERTARGETVIEW, FLOAT[4]);
void   APIENTRY tritonClearDepthStencilView(D3D10DDI_HDEVICE, D3D10DDI_HDEPTHSTENCILVIEW,
                                            UINT, FLOAT, UINT8);
void   APIENTRY tritonFlush(D3D10DDI_HDEVICE);
BOOL   APIENTRY tritonFlush11_1(D3D10DDI_HDEVICE, UINT);
void   APIENTRY tritonSetRenderTargets(D3D10DDI_HDEVICE,
                                       const D3D10DDI_HRENDERTARGETVIEW *,
                                       UINT, UINT, D3D10DDI_HDEPTHSTENCILVIEW,
                                       const D3D11DDI_HUNORDEREDACCESSVIEW *,
                                       const UINT *,
                                       UINT, UINT, UINT, UINT);

/* tritonState.c */
SIZE_T APIENTRY tritonCalcPrivateBlendStateSize(D3D10DDI_HDEVICE, const D3D11_1_DDI_BLEND_DESC *);
void   APIENTRY tritonCreateBlendState(D3D10DDI_HDEVICE, const D3D11_1_DDI_BLEND_DESC *, D3D10DDI_HBLENDSTATE, D3D10DDI_HRTBLENDSTATE);
void   APIENTRY tritonDestroyBlendState(D3D10DDI_HDEVICE, D3D10DDI_HBLENDSTATE);
void   APIENTRY tritonSetBlendState(D3D10DDI_HDEVICE, D3D10DDI_HBLENDSTATE, const FLOAT[4], UINT);
SIZE_T APIENTRY tritonCalcPrivateBlendStateSize_10(D3D10DDI_HDEVICE, const D3D10_1_DDI_BLEND_DESC *);
void   APIENTRY tritonCreateBlendState_10(D3D10DDI_HDEVICE, const D3D10_1_DDI_BLEND_DESC *, D3D10DDI_HBLENDSTATE, D3D10DDI_HRTBLENDSTATE);

SIZE_T APIENTRY tritonCalcPrivateDepthStencilStateSize(D3D10DDI_HDEVICE,
                                                       const D3D10_DDI_DEPTH_STENCIL_DESC *);
void   APIENTRY tritonCreateDepthStencilState(D3D10DDI_HDEVICE, const D3D10_DDI_DEPTH_STENCIL_DESC *, D3D10DDI_HDEPTHSTENCILSTATE, D3D10DDI_HRTDEPTHSTENCILSTATE);
void   APIENTRY tritonDestroyDepthStencilState(D3D10DDI_HDEVICE, D3D10DDI_HDEPTHSTENCILSTATE);
void   APIENTRY tritonSetDepthStencilState(D3D10DDI_HDEVICE, D3D10DDI_HDEPTHSTENCILSTATE, UINT);

SIZE_T APIENTRY tritonCalcPrivateRasterizerStateSize(D3D10DDI_HDEVICE,
                                                     const D3D11_1_DDI_RASTERIZER_DESC *);
void   APIENTRY tritonCreateRasterizerState(D3D10DDI_HDEVICE, const D3D11_1_DDI_RASTERIZER_DESC *, D3D10DDI_HRASTERIZERSTATE, D3D10DDI_HRTRASTERIZERSTATE);
void   APIENTRY tritonDestroyRasterizerState(D3D10DDI_HDEVICE, D3D10DDI_HRASTERIZERSTATE);
void   APIENTRY tritonSetRasterizerState(D3D10DDI_HDEVICE, D3D10DDI_HRASTERIZERSTATE);
SIZE_T APIENTRY tritonCalcPrivateRasterizerStateSize_10(D3D10DDI_HDEVICE,
                                                        const D3D10_DDI_RASTERIZER_DESC *);
void   APIENTRY tritonCreateRasterizerState_10(D3D10DDI_HDEVICE,
                                               const D3D10_DDI_RASTERIZER_DESC *, D3D10DDI_HRASTERIZERSTATE, D3D10DDI_HRTRASTERIZERSTATE);

SIZE_T APIENTRY tritonCalcPrivateSamplerSize(D3D10DDI_HDEVICE, const D3D10_DDI_SAMPLER_DESC *);
void   APIENTRY tritonCreateSampler(D3D10DDI_HDEVICE, const D3D10_DDI_SAMPLER_DESC *, D3D10DDI_HSAMPLER, D3D10DDI_HRTSAMPLER);
void   APIENTRY tritonDestroySampler(D3D10DDI_HDEVICE, D3D10DDI_HSAMPLER);
void   APIENTRY tritonPsSetSamplers(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSAMPLER *);
void   APIENTRY tritonVsSetSamplers(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSAMPLER *);

SIZE_T APIENTRY tritonCalcPrivateElementLayoutSize(D3D10DDI_HDEVICE,
                                                   const D3D10DDIARG_CREATEELEMENTLAYOUT *);
void   APIENTRY tritonCreateElementLayout(D3D10DDI_HDEVICE,
                                          const D3D10DDIARG_CREATEELEMENTLAYOUT *, D3D10DDI_HELEMENTLAYOUT, D3D10DDI_HRTELEMENTLAYOUT);
void   APIENTRY tritonDestroyElementLayout(D3D10DDI_HDEVICE, D3D10DDI_HELEMENTLAYOUT);
void   APIENTRY tritonIaSetInputLayout(D3D10DDI_HDEVICE, D3D10DDI_HELEMENTLAYOUT);

void   APIENTRY tritonSetViewports(D3D10DDI_HDEVICE, UINT, UINT, const D3D10_DDI_VIEWPORT *);
void   APIENTRY tritonSetScissorRects(D3D10DDI_HDEVICE, UINT, UINT, const D3D10_DDI_RECT *);
void   APIENTRY tritonIaSetTopology(D3D10DDI_HDEVICE, D3D10_DDI_PRIMITIVE_TOPOLOGY);

/* tritonShader.c */
SIZE_T APIENTRY tritonCalcPrivateShaderSize(D3D10DDI_HDEVICE, const UINT *, const VOID *);
SIZE_T APIENTRY tritonCalcPrivateTessellationShaderSize(D3D10DDI_HDEVICE, const UINT *, const VOID *);
void   APIENTRY tritonCreateVertexShader(D3D10DDI_HDEVICE, const UINT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);
void   APIENTRY tritonCreatePixelShader(D3D10DDI_HDEVICE, const UINT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);
void   APIENTRY tritonDestroyShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void   APIENTRY tritonVsSetShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void   APIENTRY tritonPsSetShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void   APIENTRY tritonGsSetShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void   APIENTRY tritonHsSetShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void   APIENTRY tritonDsSetShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void   APIENTRY tritonCsSetShader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER);
void   APIENTRY tritonCreateGeometryShader(D3D10DDI_HDEVICE, const UINT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);
void   APIENTRY tritonCreateHullShader(D3D10DDI_HDEVICE, const UINT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);
void   APIENTRY tritonCreateDomainShader(D3D10DDI_HDEVICE, const UINT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);
void   APIENTRY tritonCreateComputeShader(D3D10DDI_HDEVICE, const UINT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER);
SIZE_T APIENTRY tritonCalcPrivateGSWithSOSize(D3D10DDI_HDEVICE, const VOID *);
void   APIENTRY tritonCreateGSWithSO_11(D3D10DDI_HDEVICE,
                                        const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *, D3D10DDI_HSHADER, D3D10DDI_HRTSHADER, const VOID *);

/* tritonState.c (draw / dispatch / IA buffers) */
void   APIENTRY tritonIaSetVertexBuffers(D3D10DDI_HDEVICE, UINT, UINT,
                                         const D3D10DDI_HRESOURCE *,
                                         const UINT *, const UINT *);
void   APIENTRY tritonIaSetIndexBuffer(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, DXGI_FORMAT, UINT);
void   APIENTRY tritonDraw(D3D10DDI_HDEVICE, UINT, UINT);
void   APIENTRY tritonDrawIndexed(D3D10DDI_HDEVICE, UINT, UINT, INT);
void   APIENTRY tritonDrawInstanced(D3D10DDI_HDEVICE, UINT, UINT, UINT, UINT);
void   APIENTRY tritonDrawIndexedInstanced(D3D10DDI_HDEVICE, UINT, UINT, UINT, INT, UINT);
void   APIENTRY tritonDrawAuto(D3D10DDI_HDEVICE);
void   APIENTRY tritonDrawInstancedIndirect(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT);
void   APIENTRY tritonDrawIndexedInstancedIndirect(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT);
void   APIENTRY tritonGsSetSamplers(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSAMPLER *);
void   APIENTRY tritonHsSetSamplers(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSAMPLER *);
void   APIENTRY tritonDsSetSamplers(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSAMPLER *);
void   APIENTRY tritonCsSetSamplers(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSAMPLER *);
void   APIENTRY tritonDispatch(D3D10DDI_HDEVICE, UINT, UINT, UINT);
void   APIENTRY tritonDispatchIndirect(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT);
void   APIENTRY tritonSoSetTargets(D3D10DDI_HDEVICE, UINT, UINT,
                                   const D3D10DDI_HRESOURCE *, const UINT *);
void   APIENTRY tritonSetResourceMinLOD(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, FLOAT);

/* tritonQuery.c */
SIZE_T APIENTRY tritonCalcPrivateQuerySize(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEQUERY *);
void   APIENTRY tritonCreateQuery(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEQUERY *, D3D10DDI_HQUERY, D3D10DDI_HRTQUERY);
void   APIENTRY tritonDestroyQuery(D3D10DDI_HDEVICE, D3D10DDI_HQUERY);
void   APIENTRY tritonQueryBegin(D3D10DDI_HDEVICE, D3D10DDI_HQUERY);
void   APIENTRY tritonQueryEnd(D3D10DDI_HDEVICE, D3D10DDI_HQUERY);
void   APIENTRY tritonQueryGetData(D3D10DDI_HDEVICE, D3D10DDI_HQUERY, VOID *, UINT, UINT);
void   APIENTRY tritonSetPredication(D3D10DDI_HDEVICE, D3D10DDI_HQUERY, BOOL);
void   APIENTRY tritonCheckFormatSupport(D3D10DDI_HDEVICE, DXGI_FORMAT, UINT *);
void   APIENTRY tritonCheckMultisampleQualityLevels(D3D10DDI_HDEVICE, DXGI_FORMAT, UINT, UINT *);
void   APIENTRY tritonCheckCounterInfo(D3D10DDI_HDEVICE, D3D10DDI_COUNTER_INFO *);
void   APIENTRY tritonCheckCounter(D3D10DDI_HDEVICE, D3D10DDI_QUERY, D3D10DDI_COUNTER_TYPE *,
                                   UINT *, LPSTR, UINT *, LPSTR, UINT *, LPSTR, UINT *);
void   APIENTRY tritonDiscard(D3D10DDI_HDEVICE, D3D11DDI_HANDLETYPE, VOID *,
                              const D3D10_DDI_RECT *, UINT);
void   APIENTRY tritonClearView(D3D10DDI_HDEVICE, D3D11DDI_HANDLETYPE, VOID *,
                                const FLOAT[4], const D3D10_DDI_RECT *, UINT);
void   APIENTRY tritonCheckDirectFlipSupport(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                                             D3D10DDI_HRESOURCE, UINT, BOOL *);
void   APIENTRY tritonAssignDebugBinary(D3D10DDI_HDEVICE, D3D10DDI_HSHADER, UINT, const VOID *);

/* tritonQuery.c (WDDM 1.3 / 2.0) */
void   APIENTRY tritonCheckMultisampleQualityLevels_1_3(D3D10DDI_HDEVICE, DXGI_FORMAT,
                                                        UINT, UINT, UINT *);
void   APIENTRY tritonSetMarker(D3D10DDI_HDEVICE);
void   APIENTRY tritonSetMarkerMode(D3D10DDI_HDEVICE, D3DWDDM1_3DDI_MARKER_TYPE, UINT);
SIZE_T APIENTRY tritonCalcPrivateQuerySize_WDDM2_0(D3D10DDI_HDEVICE,
                                                   const D3DWDDM2_0DDIARG_CREATEQUERY *);
void   APIENTRY tritonCreateQuery_WDDM2_0(D3D10DDI_HDEVICE,
                                          const D3DWDDM2_0DDIARG_CREATEQUERY *, D3D10DDI_HQUERY, D3D10DDI_HRTQUERY);

/* tritonResource.c (Map / Unmap / copies / hazards / tiled) */
void   APIENTRY tritonResourceMap(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                  D3D10_DDI_MAP, UINT, D3D10DDI_MAPPED_SUBRESOURCE *);
void   APIENTRY tritonResourceUnmap(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT);
BOOL   APIENTRY tritonResourceIsStagingBusy(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE);
void   APIENTRY tritonResourceUpdateSubresourceUP(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                                                  UINT, const D3D10_DDI_BOX *,
                                                  const VOID *, UINT, UINT);
void   APIENTRY tritonResourceUpdateSubresourceUP_11_1(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                                                       UINT, const D3D10_DDI_BOX *,
                                                       const VOID *, UINT, UINT, UINT);
void   APIENTRY tritonResourceCopy(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, D3D10DDI_HRESOURCE);
void   APIENTRY tritonResourceCopyRegion(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                         UINT, UINT, UINT, D3D10DDI_HRESOURCE, UINT,
                                         const D3D10_DDI_BOX *);
void   APIENTRY tritonResourceCopyRegion_11_1(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                              UINT, UINT, UINT, D3D10DDI_HRESOURCE, UINT,
                                              const D3D10_DDI_BOX *, UINT);
void   APIENTRY tritonResourceResolveSubresource(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT, D3D10DDI_HRESOURCE, UINT, DXGI_FORMAT);
void   APIENTRY tritonResourceReadAfterWriteHazard(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE);
void   APIENTRY tritonShaderResourceViewReadAfterWriteHazard(D3D10DDI_HDEVICE, D3D10DDI_HSHADERRESOURCEVIEW, D3D10DDI_HRESOURCE);
void   APIENTRY tritonDefaultCbUpdateSubresourceUP(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                                   const D3D10_DDI_BOX *, const VOID *, UINT, UINT);
void   APIENTRY tritonDefaultCbUpdateSubresourceUP_11_1(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                                                        UINT, const D3D10_DDI_BOX *,
                                                        const VOID *, UINT, UINT, UINT);

#define TR_DECL_CB_SETTER_10(stage) \
    void APIENTRY tritonCs_ ## stage ## _Set10(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HRESOURCE *)
#define TR_DECL_CB_SETTER_11_1(stage) \
    void APIENTRY tritonCs_ ## stage ## _Set11_1(D3D10DDI_HDEVICE, UINT, UINT, \
                                                 const D3D10DDI_HRESOURCE *, const UINT *, const UINT *)
TR_DECL_CB_SETTER_10(VS); TR_DECL_CB_SETTER_10(PS); TR_DECL_CB_SETTER_10(GS);
TR_DECL_CB_SETTER_10(HS); TR_DECL_CB_SETTER_10(DS); TR_DECL_CB_SETTER_10(CS);
TR_DECL_CB_SETTER_11_1(VS); TR_DECL_CB_SETTER_11_1(PS); TR_DECL_CB_SETTER_11_1(GS);
TR_DECL_CB_SETTER_11_1(HS); TR_DECL_CB_SETTER_11_1(DS); TR_DECL_CB_SETTER_11_1(CS);
#undef TR_DECL_CB_SETTER_10
#undef TR_DECL_CB_SETTER_11_1

/* tritonView.c (SRV / DSV / UAV) */
SIZE_T APIENTRY tritonCalcPrivateSRVSize(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *);
void   APIENTRY tritonCreateSRV(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *, D3D10DDI_HSHADERRESOURCEVIEW, D3D10DDI_HRTSHADERRESOURCEVIEW);
void   APIENTRY tritonDestroySRV(D3D10DDI_HDEVICE, D3D10DDI_HSHADERRESOURCEVIEW);
SIZE_T APIENTRY tritonCalcPrivateDSVSize(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *);
void   APIENTRY tritonCreateDSV(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *, D3D10DDI_HDEPTHSTENCILVIEW, D3D10DDI_HRTDEPTHSTENCILVIEW);
void   APIENTRY tritonDestroyDSV(D3D10DDI_HDEVICE, D3D10DDI_HDEPTHSTENCILVIEW);
SIZE_T APIENTRY tritonCalcPrivateUAVSize(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *);
void   APIENTRY tritonCreateUAV(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *, D3D11DDI_HUNORDEREDACCESSVIEW, D3D11DDI_HRTUNORDEREDACCESSVIEW);
void   APIENTRY tritonDestroyUAV(D3D10DDI_HDEVICE, D3D11DDI_HUNORDEREDACCESSVIEW);
void   APIENTRY tritonClearUAVUint(D3D10DDI_HDEVICE, D3D11DDI_HUNORDEREDACCESSVIEW, const UINT[4]);
void   APIENTRY tritonClearUAVFloat(D3D10DDI_HDEVICE, D3D11DDI_HUNORDEREDACCESSVIEW, const FLOAT[4]);
void   APIENTRY tritonCopyStructureCount(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT, D3D11DDI_HUNORDEREDACCESSVIEW);
void   APIENTRY tritonCsSetUAVs(D3D10DDI_HDEVICE, UINT, UINT,
                                const D3D11DDI_HUNORDEREDACCESSVIEW *, const UINT *);
void   APIENTRY tritonGenerateMips(D3D10DDI_HDEVICE, D3D10DDI_HSHADERRESOURCEVIEW);
void   APIENTRY tritonVsSetShaderResources(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSHADERRESOURCEVIEW *);
void   APIENTRY tritonPsSetShaderResources(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSHADERRESOURCEVIEW *);
void   APIENTRY tritonGsSetShaderResources(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSHADERRESOURCEVIEW *);
void   APIENTRY tritonHsSetShaderResources(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSHADERRESOURCEVIEW *);
void   APIENTRY tritonDsSetShaderResources(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSHADERRESOURCEVIEW *);
void   APIENTRY tritonCsSetShaderResources(D3D10DDI_HDEVICE, UINT, UINT, const D3D10DDI_HSHADERRESOURCEVIEW *);

/* tritonResource.c (WDDM 1.3 / 2.0 / 2.1) */
void   APIENTRY tritonUpdateTileMappings(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                         const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *,
                                         const D3DWDDM1_3DDI_TILE_REGION_SIZE *, D3D10DDI_HRESOURCE, UINT,
                                         const UINT *, const UINT *, const UINT *, UINT);
void   APIENTRY tritonCopyTileMappings(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                                       const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *, D3D10DDI_HRESOURCE,
                                       const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *,
                                       const D3DWDDM1_3DDI_TILE_REGION_SIZE *, UINT);
void   APIENTRY tritonCopyTiles(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                                const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *,
                                const D3DWDDM1_3DDI_TILE_REGION_SIZE *, D3D10DDI_HRESOURCE, UINT64, UINT);
void   APIENTRY tritonUpdateTiles(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE,
                                  const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *,
                                  const D3DWDDM1_3DDI_TILE_REGION_SIZE *,
                                  const VOID *, UINT);
void   APIENTRY tritonTiledResourceBarrier(D3D10DDI_HDEVICE, D3D11DDI_HANDLETYPE, VOID *, D3D11DDI_HANDLETYPE, VOID *);
void   APIENTRY tritonGetMipPacking(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT *, UINT *);
void   APIENTRY tritonResizeTilePool(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT64);
void   APIENTRY tritonGetResourceLayout(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                        D3DKMT_HANDLE *, D3DWDDM2_0DDI_TEXTURE_LAYOUT *,
                                        UINT *, D3DWDDM2_0DDI_SUBRESOURCE_LAYOUT *);
void   APIENTRY tritonSetHardwareProtection(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, BOOL);
void   APIENTRY tritonSetHardwareProtectionState(D3D10DDI_HDEVICE, BOOL);
void   APIENTRY tritonAcquireResource(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, HANDLE);
void   APIENTRY tritonReleaseResource(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, HANDLE);

/* tritonView.c (WDDM 2.0) */
SIZE_T APIENTRY tritonCalcPrivateSRVSize_WDDM2_0(D3D10DDI_HDEVICE,
                                                 const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *);
void   APIENTRY tritonCreateSRV_WDDM2_0(D3D10DDI_HDEVICE,
                                        const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *, D3D10DDI_HSHADERRESOURCEVIEW, D3D10DDI_HRTSHADERRESOURCEVIEW);
SIZE_T APIENTRY tritonCalcPrivateRenderTargetViewSize_WDDM2_0(D3D10DDI_HDEVICE,
                                                              const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *);
void   APIENTRY tritonCreateRenderTargetView_WDDM2_0(D3D10DDI_HDEVICE,
                                                     const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *, D3D10DDI_HRENDERTARGETVIEW, D3D10DDI_HRTRENDERTARGETVIEW);
SIZE_T APIENTRY tritonCalcPrivateUAVSize_WDDM2_0(D3D10DDI_HDEVICE,
                                                 const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *);
void   APIENTRY tritonCreateUAV_WDDM2_0(D3D10DDI_HDEVICE,
                                        const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *, D3D11DDI_HUNORDEREDACCESSVIEW, D3D11DDI_HRTUNORDEREDACCESSVIEW);
BOOL   APIENTRY tritonFlush_WDDM2_0(D3D10DDI_HDEVICE, UINT, UINT);

/* tritonState.c (WDDM 2.0) */
SIZE_T APIENTRY tritonCalcPrivateRasterizerStateSize_WDDM2_0(D3D10DDI_HDEVICE,
                                                             const D3DWDDM2_0DDI_RASTERIZER_DESC *);
void   APIENTRY tritonCreateRasterizerState_WDDM2_0(D3D10DDI_HDEVICE,
                                                    const D3DWDDM2_0DDI_RASTERIZER_DESC *, D3D10DDI_HRASTERIZERSTATE, D3D10DDI_HRTRASTERIZERSTATE);

/* tritonShader.c (WDDM 2.0) */
HRESULT APIENTRY tritonRetrieveShaderComment(D3D10DDI_HDEVICE, D3D10DDI_HSHADER,
                                             WCHAR *, SIZE_T *);


/* ---------- Per-return-type stub helpers ----------
 *
 * x64 Windows uses a single calling convention and unused parameters
 * cost nothing, so a single stub per return type is installed via cast
 * into every PFND3D... slot of the device function table. TR_STUB logs
 * the slot once per process. */

static void APIENTRY tritonStubVoid(D3D10DDI_HDEVICE hDevice)
{
    (void)hDevice;
    TR_STUB("Triton<void> stub");
}

/* GCC's -Wcast-function-type is unhelpful for stub-table population. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"

#define TR_STUB_V    ((void *)tritonStubVoid)

/* ---------- D3D11_0 device-funcs table ---------- */

static void tritonFillD3D11DeviceFuncs(D3D11DDI_DEVICEFUNCS *p)
{
    /* High-frequency */
    p->pfnDefaultConstantBufferUpdateSubresourceUP  = tritonDefaultCbUpdateSubresourceUP;
    p->pfnVsSetConstantBuffers                      = tritonCs_VS_Set10;
    p->pfnPsSetShaderResources                      = tritonPsSetShaderResources;
    p->pfnPsSetShader                               = tritonPsSetShader;
    p->pfnPsSetSamplers                             = tritonPsSetSamplers;
    p->pfnVsSetShader                               = tritonVsSetShader;
    p->pfnDrawIndexed                               = tritonDrawIndexed;
    p->pfnDraw                                      = tritonDraw;
    p->pfnDynamicIABufferMapNoOverwrite             = tritonResourceMap;
    p->pfnDynamicIABufferUnmap                      = tritonResourceUnmap;
    p->pfnDynamicConstantBufferMapDiscard           = tritonResourceMap;
    p->pfnDynamicIABufferMapDiscard                 = tritonResourceMap;
    p->pfnDynamicConstantBufferUnmap                = tritonResourceUnmap;
    p->pfnPsSetConstantBuffers                      = tritonCs_PS_Set10;
    p->pfnIaSetInputLayout                          = tritonIaSetInputLayout;
    p->pfnIaSetVertexBuffers                        = tritonIaSetVertexBuffers;
    p->pfnIaSetIndexBuffer                          = tritonIaSetIndexBuffer;

    /* Middle-frequency */
    p->pfnDrawIndexedInstanced                      = tritonDrawIndexedInstanced;
    p->pfnDrawInstanced                             = tritonDrawInstanced;
    p->pfnDynamicResourceMapDiscard                 = tritonResourceMap;
    p->pfnDynamicResourceUnmap                      = tritonResourceUnmap;
    p->pfnGsSetConstantBuffers                      = tritonCs_GS_Set10;
    p->pfnGsSetShader                               = tritonGsSetShader;
    p->pfnIaSetTopology                             = tritonIaSetTopology;
    p->pfnStagingResourceMap                        = tritonResourceMap;
    p->pfnStagingResourceUnmap                      = tritonResourceUnmap;
    p->pfnVsSetShaderResources                      = tritonVsSetShaderResources;
    p->pfnVsSetSamplers                             = tritonVsSetSamplers;
    p->pfnGsSetShaderResources                      = tritonGsSetShaderResources;
    p->pfnGsSetSamplers                             = tritonGsSetSamplers;
    p->pfnSetRenderTargets                          = tritonSetRenderTargets;
    p->pfnShaderResourceViewReadAfterWriteHazard    = tritonShaderResourceViewReadAfterWriteHazard;
    p->pfnResourceReadAfterWriteHazard              = tritonResourceReadAfterWriteHazard;
    p->pfnSetBlendState                             = tritonSetBlendState;
    p->pfnSetDepthStencilState                      = tritonSetDepthStencilState;
    p->pfnSetRasterizerState                        = tritonSetRasterizerState;
    p->pfnQueryEnd                                  = tritonQueryEnd;
    p->pfnQueryBegin                                = tritonQueryBegin;
    p->pfnResourceCopyRegion                        = tritonResourceCopyRegion;
    p->pfnResourceUpdateSubresourceUP               = tritonResourceUpdateSubresourceUP;
    p->pfnSoSetTargets                              = tritonSoSetTargets;
    p->pfnDrawAuto                                  = tritonDrawAuto;
    p->pfnSetViewports                              = tritonSetViewports;
    p->pfnSetScissorRects                           = tritonSetScissorRects;
    p->pfnClearRenderTargetView                     = tritonClearRenderTargetView;
    p->pfnClearDepthStencilView                     = tritonClearDepthStencilView;
    p->pfnSetPredication                            = tritonSetPredication;
    p->pfnQueryGetData                              = tritonQueryGetData;
    p->pfnFlush                                     = tritonFlush;
    p->pfnGenMips                                   = tritonGenerateMips;
    p->pfnResourceCopy                              = tritonResourceCopy;
    p->pfnResourceResolveSubresource                = tritonResourceResolveSubresource;

    /* Infrequent */
    p->pfnResourceMap                               = tritonResourceMap;
    p->pfnResourceUnmap                             = tritonResourceUnmap;
    p->pfnResourceIsStagingBusy                     = tritonResourceIsStagingBusy;
    p->pfnRelocateDeviceFuncs                       = (PFND3D11DDI_RELOCATEDEVICEFUNCS)TR_STUB_V;
    p->pfnCalcPrivateResourceSize                   = tritonCalcPrivateResourceSize;
    p->pfnCalcPrivateOpenedResourceSize             = tritonCalcPrivateOpenedResourceSize;
    p->pfnCreateResource                            = tritonCreateResource;
    p->pfnOpenResource                              = tritonOpenResource;
    p->pfnDestroyResource                           = tritonDestroyResource;
    p->pfnCalcPrivateShaderResourceViewSize         = tritonCalcPrivateSRVSize;
    p->pfnCreateShaderResourceView                  = tritonCreateSRV;
    p->pfnDestroyShaderResourceView                 = tritonDestroySRV;
    p->pfnCalcPrivateRenderTargetViewSize           = tritonCalcPrivateRenderTargetViewSize;
    p->pfnCreateRenderTargetView                    = tritonCreateRenderTargetView;
    p->pfnDestroyRenderTargetView                   = tritonDestroyRenderTargetView;
    p->pfnCalcPrivateDepthStencilViewSize           = tritonCalcPrivateDSVSize;
    p->pfnCreateDepthStencilView                    = tritonCreateDSV;
    p->pfnDestroyDepthStencilView                   = tritonDestroyDSV;
    p->pfnCalcPrivateElementLayoutSize              = tritonCalcPrivateElementLayoutSize;
    p->pfnCreateElementLayout                       = tritonCreateElementLayout;
    p->pfnDestroyElementLayout                      = tritonDestroyElementLayout;
    /* The 11.0 table uses the D3D10.1 blend desc (no LogicOp).
     * tritonCreateBlendState_10 upgrades to the 11.1 form. */
    p->pfnCalcPrivateBlendStateSize                 = tritonCalcPrivateBlendStateSize_10;
    p->pfnCreateBlendState                          = tritonCreateBlendState_10;
    p->pfnDestroyBlendState                         = tritonDestroyBlendState;
    p->pfnCalcPrivateDepthStencilStateSize          = tritonCalcPrivateDepthStencilStateSize;
    p->pfnCreateDepthStencilState                   = tritonCreateDepthStencilState;
    p->pfnDestroyDepthStencilState                  = tritonDestroyDepthStencilState;
    /* The 11.0 table uses the D3D10.0 rasterizer desc (no
     * ForcedSampleCount). _10 upgrades and delegates to the 11.1 path. */
    p->pfnCalcPrivateRasterizerStateSize            = tritonCalcPrivateRasterizerStateSize_10;
    p->pfnCreateRasterizerState                     = tritonCreateRasterizerState_10;
    p->pfnDestroyRasterizerState                    = tritonDestroyRasterizerState;
    p->pfnCalcPrivateShaderSize                     = (PFND3D10DDI_CALCPRIVATESHADERSIZE)tritonCalcPrivateShaderSize;
    p->pfnCreateVertexShader                        = (PFND3D10DDI_CREATEVERTEXSHADER)tritonCreateVertexShader;
    p->pfnCreateGeometryShader                      = (PFND3D10DDI_CREATEGEOMETRYSHADER)tritonCreateGeometryShader;
    p->pfnCreatePixelShader                         = (PFND3D10DDI_CREATEPIXELSHADER)tritonCreatePixelShader;
    p->pfnCalcPrivateGeometryShaderWithStreamOutput = (PFND3D11DDI_CALCPRIVATEGEOMETRYSHADERWITHSTREAMOUTPUT)tritonCalcPrivateGSWithSOSize;
    p->pfnCreateGeometryShaderWithStreamOutput      = (PFND3D11DDI_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT)tritonCreateGSWithSO_11;
    p->pfnDestroyShader                             = tritonDestroyShader;
    p->pfnCalcPrivateSamplerSize                    = tritonCalcPrivateSamplerSize;
    p->pfnCreateSampler                             = tritonCreateSampler;
    p->pfnDestroySampler                            = tritonDestroySampler;
    p->pfnCalcPrivateQuerySize                      = tritonCalcPrivateQuerySize;
    p->pfnCreateQuery                               = tritonCreateQuery;
    p->pfnDestroyQuery                              = tritonDestroyQuery;

    p->pfnCheckFormatSupport                        = tritonCheckFormatSupport;
    p->pfnCheckMultisampleQualityLevels             = tritonCheckMultisampleQualityLevels;
    p->pfnCheckCounterInfo                          = tritonCheckCounterInfo;
    p->pfnCheckCounter                              = tritonCheckCounter;

    p->pfnDestroyDevice                             = (PFND3D10DDI_DESTROYDEVICE)TR_STUB_V;
    p->pfnSetTextFilterSize                         = (PFND3D10DDI_SETTEXTFILTERSIZE)TR_STUB_V;

    /* 10.1 entries */
    p->pfnResourceConvert                           = (PFND3D10DDI_RESOURCECOPY)TR_STUB_V;
    p->pfnResourceConvertRegion                     = (PFND3D10DDI_RESOURCECOPYREGION)TR_STUB_V;

    /* 11.0 entries */
    p->pfnDrawIndexedInstancedIndirect              = tritonDrawIndexedInstancedIndirect;
    p->pfnDrawInstancedIndirect                     = tritonDrawInstancedIndirect;
    p->pfnCommandListExecute                        = (PFND3D11DDI_COMMANDLISTEXECUTE)0;
    p->pfnHsSetShaderResources                      = tritonHsSetShaderResources;
    p->pfnHsSetShader                               = tritonHsSetShader;
    p->pfnHsSetSamplers                             = tritonHsSetSamplers;
    p->pfnHsSetConstantBuffers                      = tritonCs_HS_Set10;
    p->pfnDsSetShaderResources                      = tritonDsSetShaderResources;
    p->pfnDsSetShader                               = tritonDsSetShader;
    p->pfnDsSetSamplers                             = tritonDsSetSamplers;
    p->pfnDsSetConstantBuffers                      = tritonCs_DS_Set10;
    p->pfnCreateHullShader                          = (PFND3D11DDI_CREATEHULLSHADER)tritonCreateHullShader;
    p->pfnCreateDomainShader                        = (PFND3D11DDI_CREATEDOMAINSHADER)tritonCreateDomainShader;
    p->pfnCheckDeferredContextHandleSizes           = (PFND3D11DDI_CHECKDEFERREDCONTEXTHANDLESIZES)0;
    p->pfnCalcDeferredContextHandleSize             = (PFND3D11DDI_CALCDEFERREDCONTEXTHANDLESIZE)0;
    p->pfnCalcPrivateDeferredContextSize            = (PFND3D11DDI_CALCPRIVATEDEFERREDCONTEXTSIZE)0;
    p->pfnCreateDeferredContext                     = (PFND3D11DDI_CREATEDEFERREDCONTEXT)0;
    p->pfnAbandonCommandList                        = (PFND3D11DDI_ABANDONCOMMANDLIST)0;
    p->pfnCalcPrivateCommandListSize                = (PFND3D11DDI_CALCPRIVATECOMMANDLISTSIZE)0;
    p->pfnCreateCommandList                         = (PFND3D11DDI_CREATECOMMANDLIST)0;
    p->pfnDestroyCommandList                        = (PFND3D11DDI_DESTROYCOMMANDLIST)0;
    p->pfnCalcPrivateTessellationShaderSize         = (PFND3D11DDI_CALCPRIVATETESSELLATIONSHADERSIZE)tritonCalcPrivateTessellationShaderSize;
    p->pfnPsSetShaderWithIfaces                     = (PFND3D11DDI_SETSHADER_WITH_IFACES)TR_STUB_V;
    p->pfnVsSetShaderWithIfaces                     = (PFND3D11DDI_SETSHADER_WITH_IFACES)TR_STUB_V;
    p->pfnGsSetShaderWithIfaces                     = (PFND3D11DDI_SETSHADER_WITH_IFACES)TR_STUB_V;
    p->pfnHsSetShaderWithIfaces                     = (PFND3D11DDI_SETSHADER_WITH_IFACES)TR_STUB_V;
    p->pfnDsSetShaderWithIfaces                     = (PFND3D11DDI_SETSHADER_WITH_IFACES)TR_STUB_V;
    p->pfnCsSetShaderWithIfaces                     = (PFND3D11DDI_SETSHADER_WITH_IFACES)TR_STUB_V;
    p->pfnCreateComputeShader                       = tritonCreateComputeShader;
    p->pfnCsSetShader                               = tritonCsSetShader;
    p->pfnCsSetShaderResources                      = tritonCsSetShaderResources;
    p->pfnCsSetSamplers                             = tritonCsSetSamplers;
    p->pfnCsSetConstantBuffers                      = tritonCs_CS_Set10;
    p->pfnCalcPrivateUnorderedAccessViewSize        = tritonCalcPrivateUAVSize;
    p->pfnCreateUnorderedAccessView                 = tritonCreateUAV;
    p->pfnDestroyUnorderedAccessView                = tritonDestroyUAV;
    p->pfnClearUnorderedAccessViewUint              = tritonClearUAVUint;
    p->pfnClearUnorderedAccessViewFloat             = tritonClearUAVFloat;
    p->pfnCsSetUnorderedAccessViews                 = tritonCsSetUAVs;
    p->pfnDispatch                                  = tritonDispatch;
    p->pfnDispatchIndirect                          = tritonDispatchIndirect;
    p->pfnSetResourceMinLOD                         = tritonSetResourceMinLOD;
    p->pfnCopyStructureCount                        = tritonCopyStructureCount;
    p->pfnRecycleCommandList                        = (PFND3D11DDI_RECYCLECOMMANDLIST)0;
    p->pfnRecycleCreateCommandList                  = (PFND3D11DDI_RECYCLECREATECOMMANDLIST)0;
    p->pfnRecycleCreateDeferredContext              = (PFND3D11DDI_RECYCLECREATEDEFERREDCONTEXT)0;
    p->pfnRecycleDestroyCommandList                 = (PFND3D11DDI_DESTROYCOMMANDLIST)0;
}

/* ---------- D3D11_1 device-funcs table ---------- */

static void tritonFillD3D11_1DeviceFuncs(D3D11_1DDI_DEVICEFUNCS *p)
{
    /* D3D11_1DDI_DEVICEFUNCS starts with the same layout as
     * D3D11DDI_DEVICEFUNCS for the shared prefix; install the 11.0
     * fillers via cast, then override the 11.1-specific slots. */
    tritonFillD3D11DeviceFuncs((D3D11DDI_DEVICEFUNCS *)(p));

    p->pfnDefaultConstantBufferUpdateSubresourceUP  = tritonDefaultCbUpdateSubresourceUP_11_1;
    p->pfnVsSetConstantBuffers                      = tritonCs_VS_Set11_1;
    p->pfnPsSetConstantBuffers                      = tritonCs_PS_Set11_1;
    p->pfnGsSetConstantBuffers                      = tritonCs_GS_Set11_1;
    p->pfnHsSetConstantBuffers                      = tritonCs_HS_Set11_1;
    p->pfnDsSetConstantBuffers                      = tritonCs_DS_Set11_1;
    p->pfnCsSetConstantBuffers                      = tritonCs_CS_Set11_1;
    p->pfnResourceCopyRegion                        = tritonResourceCopyRegion_11_1;
    p->pfnResourceUpdateSubresourceUP               = tritonResourceUpdateSubresourceUP_11_1;
    p->pfnFlush                                     = tritonFlush11_1;
    p->pfnCalcPrivateBlendStateSize                 = tritonCalcPrivateBlendStateSize;
    p->pfnCreateBlendState                          = tritonCreateBlendState;
    p->pfnCalcPrivateRasterizerStateSize            = tritonCalcPrivateRasterizerStateSize;
    p->pfnCreateRasterizerState                     = tritonCreateRasterizerState;
    p->pfnCalcPrivateShaderSize                     = (PFND3D11_1DDI_CALCPRIVATESHADERSIZE)tritonCalcPrivateShaderSize;
    p->pfnCreateVertexShader                        = (PFND3D11_1DDI_CREATEVERTEXSHADER)tritonCreateVertexShader;
    p->pfnCreateGeometryShader                      = (PFND3D11_1DDI_CREATEGEOMETRYSHADER)tritonCreateGeometryShader;
    p->pfnCreatePixelShader                         = (PFND3D11_1DDI_CREATEPIXELSHADER)tritonCreatePixelShader;
    p->pfnCalcPrivateGeometryShaderWithStreamOutput = (PFND3D11_1DDI_CALCPRIVATEGEOMETRYSHADERWITHSTREAMOUTPUT)tritonCalcPrivateGSWithSOSize;
    p->pfnCreateGeometryShaderWithStreamOutput      = (PFND3D11_1DDI_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT)tritonCreateGSWithSO_11;
    p->pfnCreateHullShader                          = (PFND3D11_1DDI_CREATEHULLSHADER)tritonCreateHullShader;
    p->pfnCreateDomainShader                        = (PFND3D11_1DDI_CREATEDOMAINSHADER)tritonCreateDomainShader;
    p->pfnCalcPrivateTessellationShaderSize         = (PFND3D11_1DDI_CALCPRIVATETESSELLATIONSHADERSIZE)tritonCalcPrivateTessellationShaderSize;

    /* 11.1-only slots. */
    p->pfnDiscard                                   = tritonDiscard;
    p->pfnAssignDebugBinary                         = tritonAssignDebugBinary;
    p->pfnDynamicConstantBufferMapNoOverwrite       = tritonResourceMap;
    p->pfnCheckDirectFlipSupport                    = tritonCheckDirectFlipSupport;
    p->pfnClearView                                 = tritonClearView;
}

/* ---------- WDDM 1.3 device-funcs table ----------
 *
 * Same layout as 11.1 for every shared field. Differences:
 *   - pfnRelocateDeviceFuncs has a tier-specific PFN type.
 *   - pfnCheckMultisampleQualityLevels gains a Flags parameter.
 *   - Nine new tiled-resource + marker entrypoints appended. */

static void tritonFillWDDM1_3DeviceFuncs(D3DWDDM1_3DDI_DEVICEFUNCS *p)
{
    tritonFillD3D11_1DeviceFuncs((D3D11_1DDI_DEVICEFUNCS *)(p));

    p->pfnRelocateDeviceFuncs           = (PFND3DWDDM1_3DDI_RELOCATEDEVICEFUNCS)TR_STUB_V;
    p->pfnCheckMultisampleQualityLevels = tritonCheckMultisampleQualityLevels_1_3;

    p->pfnUpdateTileMappings   = tritonUpdateTileMappings;
    p->pfnCopyTileMappings     = tritonCopyTileMappings;
    p->pfnCopyTiles            = tritonCopyTiles;
    p->pfnUpdateTiles          = tritonUpdateTiles;
    p->pfnTiledResourceBarrier = tritonTiledResourceBarrier;
    p->pfnGetMipPacking        = tritonGetMipPacking;
    p->pfnResizeTilePool       = tritonResizeTilePool;
    p->pfnSetMarker            = tritonSetMarker;
    p->pfnSetMarkerMode        = tritonSetMarkerMode;
}

/* ---------- WDDM 2.0 device-funcs table ----------
 *
 * Shares field NAMES with 11.0/11.1 for most create-* entrypoints, but
 * the PFN types are wider (PlaneSlice in view ARGs; ContextType in
 * pfnFlush; new ARG types for query / UAV / rasterizer). The 11.1
 * chain-call writes 11.x-typed pointers; we overwrite them here. */

static void tritonFillWDDM2_0DeviceFuncs(D3DWDDM2_0DDI_DEVICEFUNCS *p)
{
    tritonFillWDDM1_3DeviceFuncs((D3DWDDM1_3DDI_DEVICEFUNCS *)(p));

    p->pfnRelocateDeviceFuncs                   = (PFND3DWDDM2_0DDI_RELOCATEDEVICEFUNCS)TR_STUB_V;

    p->pfnCalcPrivateShaderResourceViewSize     = tritonCalcPrivateSRVSize_WDDM2_0;
    p->pfnCreateShaderResourceView              = tritonCreateSRV_WDDM2_0;
    p->pfnCalcPrivateRenderTargetViewSize       = tritonCalcPrivateRenderTargetViewSize_WDDM2_0;
    p->pfnCreateRenderTargetView                = tritonCreateRenderTargetView_WDDM2_0;
    p->pfnCalcPrivateRasterizerStateSize        = tritonCalcPrivateRasterizerStateSize_WDDM2_0;
    p->pfnCreateRasterizerState                 = tritonCreateRasterizerState_WDDM2_0;
    p->pfnCalcPrivateQuerySize                  = tritonCalcPrivateQuerySize_WDDM2_0;
    p->pfnCreateQuery                           = tritonCreateQuery_WDDM2_0;
    p->pfnCalcPrivateUnorderedAccessViewSize    = tritonCalcPrivateUAVSize_WDDM2_0;
    p->pfnCreateUnorderedAccessView             = tritonCreateUAV_WDDM2_0;
    p->pfnFlush                                 = tritonFlush_WDDM2_0;

    /* RetrieveShaderComment + SetHardwareProtectionState were retro-added
     * to the WDDM 2.0 struct at MINOR_HEADER_VERSION >= 9. */
    p->pfnSetHardwareProtection      = tritonSetHardwareProtection;
    p->pfnGetResourceLayout          = tritonGetResourceLayout;
    p->pfnRetrieveShaderComment      = tritonRetrieveShaderComment;
    p->pfnSetHardwareProtectionState = tritonSetHardwareProtectionState;
}

/* ---------- WDDM 2.1 device-funcs table ----------
 *
 * WDDM 2.0 + Acquire/ReleaseResource sync-token entrypoints. */

static void tritonFillWDDM2_1DeviceFuncs(D3DWDDM2_1DDI_DEVICEFUNCS *p)
{
    tritonFillWDDM2_0DeviceFuncs((D3DWDDM2_0DDI_DEVICEFUNCS *)(p));

    p->pfnRelocateDeviceFuncs = (PFND3DWDDM2_1DDI_RELOCATEDEVICEFUNCS)TR_STUB_V;
    p->pfnAcquireResource     = (PFND3DWDDM2_1DDI_SYNC_TOKEN)tritonAcquireResource;
    p->pfnReleaseResource     = (PFND3DWDDM2_1DDI_SYNC_TOKEN)tritonReleaseResource;
}

/* ---------- DestroyDevice ---------- */

static void APIENTRY tritonDestroyDevice(D3D10DDI_HDEVICE hDevice)
{
    PTRITON_DEVICE p = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!p) {
        TR_LOG("DestroyDevice: hDevice has no driver private");
        return;
    }
    /* Wait out any present still in flight (see presentInFlight in triton.h)
     * BEFORE touching anything it uses.  The DDI gives no guarantee that a
     * present has retired by DestroyDevice, and everything below -- the
     * context flush, the kernel-context destroy, the object releases and the
     * critical-section deletes -- tears out state a running present holds.
     *
     * The present gate's own wait is bounded, so this normally terminates
     * quickly.  The deadline exceeds that bound and is a canary for what the
     * bound does not cover -- a stuck pfnPresentCb or an unbalanced counter --
     * which would otherwise spin here forever with no diagnostic.
     *
     * On expiry there is no safe action left.  The runtime frees this private
     * device block as soon as DestroyDevice returns, and presentLock,
     * kmCtxLock and presentInFlight all live inside it, so skipping the
     * teardown does not keep a stuck present's state valid -- it only avoids
     * compounding the fault with a release the present is actively using.
     * Return without tearing down and report it. */
    {
        const ULONGLONG drain_deadline =
            GetTickCount64() + TRITON_PRESENT_DRAIN_MS;
        BOOL drained = TRUE;
        while (InterlockedCompareExchange(&p->presentInFlight, 0, 0) != 0) {
            if (GetTickCount64() >= drain_deadline) { drained = FALSE; break; }
            Sleep(1);
        }
        if (!drained) {
            npt_log("triton: DestroyDevice: presentInFlight still %ld after "
                    "%u ms -- skipping teardown. A present is genuinely stuck, "
                    "or the counter is unbalanced; the runtime frees this "
                    "device block on return either way.",
                    (long)InterlockedCompareExchange(&p->presentInFlight, 0, 0),
                    (unsigned)TRITON_PRESENT_DRAIN_MS);
            p->pAdapter = NULL;
            return;
        }
    }

    if (p->pCtx1)
        ID3D11DeviceContext1_Flush(p->pCtx1);
    /* Tear down the present kernel context before the device goes away, under
     * the HCONTEXT-domain lock (see kmCtxLock in triton.h) so it cannot run
     * against a pfnPresentCb on the same handle. */
    if (p->hKMContext && p->KTCallbacks.pfnDestroyContextCb) {
        D3DDDICB_DESTROYCONTEXT dc;
        memset(&dc, 0, sizeof(dc));
        dc.hContext = p->hKMContext;
        if (p->presentLockInit)
            EnterCriticalSection(&p->kmCtxLock);
        p->KTCallbacks.pfnDestroyContextCb(p->hRTDevice.handle, &dc);
        p->hKMContext = NULL;
        if (p->presentLockInit)
            LeaveCriticalSection(&p->kmCtxLock);
    }

    /* Release the lazily-created present-fence objects under presentLock,
     * marking the fence not-ready+disabled first so a present still inside
     * the fence protocol falls back to unfenced instead of reviving a
     * torn-down object. */
    if (p->presentLockInit) {
        EnterCriticalSection(&p->presentLock);
        p->presentFenceReady    = FALSE;
        p->presentFenceDisabled = TRUE;
        if (p->pPresentFence) { ID3D11Fence_Release(p->pPresentFence);  p->pPresentFence = NULL; }
        if (p->pCtx4)         { ID3D11DeviceContext4_Release(p->pCtx4); p->pCtx4 = NULL; }
        if (p->pBlitVS)      { ID3D11VertexShader_Release(p->pBlitVS);      p->pBlitVS = NULL; }
        if (p->pBlitPS)      { ID3D11PixelShader_Release(p->pBlitPS);       p->pBlitPS = NULL; }
        if (p->pBlitSampler) { ID3D11SamplerState_Release(p->pBlitSampler); p->pBlitSampler = NULL; }
        if (p->pBlitCB)      { ID3D11Buffer_Release(p->pBlitCB);            p->pBlitCB = NULL; }
        /* Drop the pooled events without closing them.  A pooled event can
         * still carry an outstanding arm: SetEventOnCompletion is serviced by
         * npt_event's waiter thread, which belongs to the process-wide device
         * singleton and outlives this Triton device, and releasing the fence
         * does not reach it.  Closing the handle would let that waiter signal
         * whatever object later inherits the handle value.  The leak is
         * bounded to TRITON_PRESENT_EVENT_POOL handles per device teardown. */
        p->presentEventPoolCount = 0;
        LeaveCriticalSection(&p->presentLock);
        DeleteCriticalSection(&p->presentLock);
        DeleteCriticalSection(&p->kmCtxLock);
        p->presentLockInit = FALSE;
    }
    if (p->pCtx3) { ID3D11DeviceContext3_Release(p->pCtx3); p->pCtx3 = NULL; }
    if (p->pCtx2) { ID3D11DeviceContext2_Release(p->pCtx2); p->pCtx2 = NULL; }
    if (p->pCtx1) { ID3D11DeviceContext1_Release(p->pCtx1); p->pCtx1 = NULL; }
    if (p->pDev3) { ID3D11Device3_Release(p->pDev3);        p->pDev3 = NULL; }
    if (p->pDev2) { ID3D11Device2_Release(p->pDev2);        p->pDev2 = NULL; }
    if (p->pDev1) { ID3D11Device1_Release(p->pDev1);        p->pDev1 = NULL; }
    /* The runtime owns the TRITON_DEVICE memory (allocated via
     * pfnCalcPrivateDeviceSize); just clear the back-pointer. */
    p->pAdapter = NULL;
    TR_LOG("DestroyDevice");
}

/* ---------- pfnCreateDevice ---------- */

static HRESULT APIENTRY tritonCreateDevice(D3D10DDI_HADAPTER hAdapter,
                                           D3D10DDIARG_CREATEDEVICE *pArgs)
{
    PTRITON_ADAPTER pAdapter = (PTRITON_ADAPTER)(hAdapter.pDrvPrivate);
    PTRITON_DEVICE  p        = (PTRITON_DEVICE)(pArgs->hDrvDevice.pDrvPrivate);
    if (!pAdapter || !p) {
        TR_LOG("CreateDevice: missing pDrvPrivate (adapter=%p, device=%p)",
               (void *)pAdapter, (void *)p);
        return E_INVALIDARG;
    }

    /* Zero the private block before touching anything.  The runtime allocates
     * it from the size returned by pfnCalcPrivateDeviceSize, guarantees
     * nothing about its contents, and recycles it across Create/Destroy within
     * a process, so any field the assignments below miss would arrive holding
     * the previous device's value.  Zeroing makes "every field is initialised"
     * true by construction instead of by keeping that list in sync with the
     * struct, and is safe because this is opaque driver-owned storage that
     * nothing reads a pre-existing value out of. */
    memset(p, 0, sizeof(*p));

    TR_LOG("CreateDevice: Interface=0x%08x Version=0x%08x Flags=0x%08x",
           pArgs->Interface, pArgs->Version, pArgs->Flags);

    /* Install the device-funcs table for the requested interface.
     * Higher-tier fillers chain-call the lower-tier ones. */
    switch (pArgs->Interface) {
    case D3D11_0_DDI_INTERFACE_VERSION:
        tritonFillD3D11DeviceFuncs(pArgs->p11DeviceFuncs);
        pArgs->p11DeviceFuncs->pfnDestroyDevice = tritonDestroyDevice;
        break;
    case D3D11_1_DDI_INTERFACE_VERSION:
        tritonFillD3D11_1DeviceFuncs(pArgs->p11_1DeviceFuncs);
        pArgs->p11_1DeviceFuncs->pfnDestroyDevice = tritonDestroyDevice;
        break;
    case D3DWDDM1_3_DDI_INTERFACE_VERSION:
        tritonFillWDDM1_3DeviceFuncs(pArgs->pWDDM1_3DeviceFuncs);
        pArgs->pWDDM1_3DeviceFuncs->pfnDestroyDevice = tritonDestroyDevice;
        break;
    case D3DWDDM2_0_DDI_INTERFACE_VERSION:
        tritonFillWDDM2_0DeviceFuncs(pArgs->pWDDM2_0DeviceFuncs);
        pArgs->pWDDM2_0DeviceFuncs->pfnDestroyDevice = tritonDestroyDevice;
        break;
    case D3DWDDM2_1_DDI_INTERFACE_VERSION:
        tritonFillWDDM2_1DeviceFuncs(pArgs->pWDDM2_1DeviceFuncs);
        pArgs->pWDDM2_1DeviceFuncs->pfnDestroyDevice = tritonDestroyDevice;
        break;
    default:
        TR_LOG("CreateDevice: unsupported interface 0x%08x", pArgs->Interface);
        return E_FAIL;
    }

    p->hRTDevice    = pArgs->hRTDevice;
    p->uIfVersion   = pArgs->Interface;
    p->pAdapter     = pAdapter;
    p->KTCallbacks  = *pArgs->pKTCallbacks;
    p->pDev1          = NULL;
    p->pDev2          = NULL;
    p->pDev3          = NULL;
    p->pCtx1          = NULL;
    p->pCtx2          = NULL;
    p->pCtx3          = NULL;
    p->FeatureLevel   = D3D_FEATURE_LEVEL_11_0;
    p->pCurrentVS     = NULL;
    p->pCurrentLayout = NULL;
    p->nextShaderCookie = 0;
    p->hRTCoreLayer   = pArgs->hRTCoreLayer;
    /* The CREATEDEVICE callbacks are a union: every member aliases the
     * same pointer. Every D3D*DDI_CORELAYER_DEVICECALLBACKS version shares
     * pfnSetErrorCb as its first field, and the pfnState*Cb slots follow at
     * the same offsets in each, so reading through the 11.0-typed member is
     * correct for every tier tritonCreateDevice accepts. */
    p->pUMCallbacks   = pArgs->p11UMCallbacks;
    p->hKMContext     = NULL;
    p->pfnPresentCb   = NULL;
    p->RuntimeCtxInited = FALSE;
    /* Present-fence state (see triton.h): spelled out as the documented
     * starting state, though the memset above already covers it. */
    p->pCtx4                = NULL;
    p->pPresentFence        = NULL;
    p->presentFenceValue    = 0;
    p->presentFenceReady    = FALSE;
    p->presentFenceDisabled = FALSE;
    memset(p->presentEventPool, 0, sizeof(p->presentEventPool));
    p->presentEventPoolCount = 0;
    p->presentLockInit      = FALSE;

    /* The runtime encodes the requested pipeline level in pArgs->Flags
     * (per D3D11DDICAPS_3DPIPELINESUPPORT). Accept the full 10.0..11.1
     * range advertised by GetCaps; the dxvk/Vulkan backend creates a
     * device clamped to the requested feature level. */
    const D3D11DDI_3DPIPELINELEVEL level =
        D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(pArgs->Flags);
    D3D_FEATURE_LEVEL requested;
    switch (level) {
    case D3D11DDI_3DPIPELINELEVEL_10_0:    requested = D3D_FEATURE_LEVEL_10_0; break;
    case D3D11DDI_3DPIPELINELEVEL_10_1:    requested = D3D_FEATURE_LEVEL_10_1; break;
    case D3D11DDI_3DPIPELINELEVEL_11_0:    requested = D3D_FEATURE_LEVEL_11_0; break;
    case D3D11_1DDI_3DPIPELINELEVEL_11_1:  requested = D3D_FEATURE_LEVEL_11_1; break;
    default:
        TR_LOG("CreateDevice: pipeline level %d unsupported (Triton requires >= 10.0; Flags=0x%08x)",
               level, pArgs->Flags);
        return E_FAIL;
    }

    /* Statically-linked internal factory.  Returns the base
     * ID3D11Device + ID3D11DeviceContext; QI down to the Tier 1 forms
     * Triton works with. */
    ID3D11Device        *raw_dev = NULL;
    ID3D11DeviceContext *raw_ctx = NULL;
    HRESULT hr = npt_d3d11_create_device_internal(
        NULL /* pAdapter */, D3D_DRIVER_TYPE_HARDWARE, NULL,
        0 /* Flags */, &requested, 1, D3D11_SDK_VERSION,
        &raw_dev, &p->FeatureLevel, &raw_ctx);
    if (FAILED(hr) || !raw_dev || !raw_ctx) {
        TR_LOG("CreateDevice: npt_d3d11_create_device_internal failed 0x%08lx", hr);
        if (raw_dev) ID3D11Device_Release(raw_dev);
        if (raw_ctx) ID3D11DeviceContext_Release(raw_ctx);
        return FAILED(hr) ? hr : E_FAIL;
    }
    hr = ID3D11Device_QueryInterface(raw_dev, &IID_ID3D11Device1,
                                     (void **)&p->pDev1);
    ID3D11Device_Release(raw_dev);
    if (FAILED(hr) || !p->pDev1) {
        TR_LOG("CreateDevice: QI ID3D11Device1 failed 0x%08lx", hr);
        ID3D11DeviceContext_Release(raw_ctx);
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = ID3D11DeviceContext_QueryInterface(raw_ctx, &IID_ID3D11DeviceContext1,
                                            (void **)&p->pCtx1);
    ID3D11DeviceContext_Release(raw_ctx);
    if (FAILED(hr) || !p->pCtx1) {
        TR_LOG("CreateDevice: QI ID3D11DeviceContext1 failed 0x%08lx", hr);
        ID3D11Device1_Release(p->pDev1); p->pDev1 = NULL;
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    TR_LOG("CreateDevice: got Neptune device, feature level 0x%x", p->FeatureLevel);

    /* QI for higher-tier interfaces so WDDM 1.3 / 2.0 handlers can
     * forward instead of stub. NULL is non-fatal — handlers fall back
     * to TR_STUB when the pointer is missing. */
    if (FAILED(ID3D11Device1_QueryInterface(p->pDev1, &IID_ID3D11Device2,
                                            (void **)&p->pDev2)))
        p->pDev2 = NULL;
    if (FAILED(ID3D11Device1_QueryInterface(p->pDev1, &IID_ID3D11Device3,
                                            (void **)&p->pDev3)))
        p->pDev3 = NULL;
    if (FAILED(ID3D11DeviceContext1_QueryInterface(p->pCtx1, &IID_ID3D11DeviceContext2,
                                                   (void **)&p->pCtx2)))
        p->pCtx2 = NULL;
    if (FAILED(ID3D11DeviceContext1_QueryInterface(p->pCtx1, &IID_ID3D11DeviceContext3,
                                                   (void **)&p->pCtx3)))
        p->pCtx3 = NULL;
    TR_LOG("CreateDevice: Neptune interface tier: Device2=%s Device3=%s Context2=%s Context3=%s",
           p->pDev2 ? "yes" : "no", p->pDev3 ? "yes" : "no",
           p->pCtx2 ? "yes" : "no", p->pCtx3 ? "yes" : "no");

    /* The present kernel context (pfnCreateContextCb) is created lazily on
     * the first present (tritonPresentEnsureKernelContext), not here: some
     * runtimes probe-create the device and creating a kernel context during
     * that probe makes them reject/retry the adapter.  Wire DXGI runtime
     * callbacks once the D3D11 device is up; this also records pfnPresentCb
     * from the DXGI base callbacks. */
    tritonInstallDXGIFuncs(p, pArgs);

    /* Present-path locks; what each covers is documented on presentLock /
     * kmCtxLock in triton.h.  Created only on the success path: a failed
     * CreateDevice gets no matching DestroyDevice, so anything initialised
     * before this point would never be deleted. */
    InitializeCriticalSection(&p->presentLock);
    InitializeCriticalSection(&p->kmCtxLock);
    p->presentLockInit = TRUE;
    return S_OK;
}

static SIZE_T APIENTRY tritonCalcPrivateDeviceSize(D3D10DDI_HADAPTER hAdapter,
                                                   const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pArgs)
{
    (void)hAdapter;
    (void)pArgs;
    return sizeof(TRITON_DEVICE);
}

static HRESULT APIENTRY tritonCloseAdapter(D3D10DDI_HADAPTER hAdapter)
{
    PTRITON_ADAPTER pAdapter = (PTRITON_ADAPTER)(hAdapter.pDrvPrivate);
    TR_LOG("CloseAdapter");
    if (pAdapter)
        HeapFree(GetProcessHeap(), 0, pAdapter);
    return S_OK;
}

static HRESULT APIENTRY tritonGetSupportedVersions(D3D10DDI_HADAPTER hAdapter,
                                                   UINT32 *puEntries,
                                                   UINT64 *pSupportedDDIInterfaceVersions)
{
    (void)hAdapter;
    static const UINT64 kSupportedVersions[] = {
        D3D11_0_DDI_SUPPORTED,
        D3D11_1_DDI_SUPPORTED,
        /* Top out at the D3D11.1 device DDI.  At the WDDM2.x device interface
         * the D3D11 runtime enforces, during device finalization, a depth-
         * stencil-family MSAA consistency rule that this COM-forwarding driver
         * cannot satisfy: it validates the MULTISAMPLE_RENDERTARGET format-
         * support bit on the X-padded depth-read/stencil-read VIEW formats
         * (e.g. R32_FLOAT_X8X24_TYPELESS) by format support alone, with no
         * CheckMultisampleQualityLevels call -- and rejects the device whether
         * the bit is set (a non-target claiming an MSAA target) or clear
         * (mismatch with the typeless parent).  The D3D11.1 device DDI instead
         * drives MSAA via CheckMultisampleQualityLevels, which we report
         * consistently, so device creation finalizes.  D3D11.1 still provides
         * the flip-model present DWM needs; advertising WDDM2.x would also
         * require GPU virtual addressing this virtual GPU does not
         * implement. */
    };
    const UINT32 kCount = sizeof(kSupportedVersions) / sizeof(kSupportedVersions[0]);

    if (!puEntries)
        return E_INVALIDARG;

    if (!pSupportedDDIInterfaceVersions) {
        *puEntries = kCount;
        return S_OK;
    }

    const UINT32 toCopy = (*puEntries < kCount) ? *puEntries : kCount;
    for (UINT32 i = 0; i < toCopy; ++i)
        pSupportedDDIInterfaceVersions[i] = kSupportedVersions[i];
    *puEntries = toCopy;
    return S_OK;
}

static HRESULT APIENTRY tritonGetCaps(D3D10DDI_HADAPTER hAdapter,
                                      const D3D10_2DDIARG_GETCAPS *pArgs)
{
    (void)hAdapter;
    TR_LOG("GetCaps: Type=%d", pArgs->Type);
    if (!pArgs->pData || !pArgs->DataSize) return S_OK;
    ZeroMemory(pArgs->pData, pArgs->DataSize);

    switch (pArgs->Type) {
    case D3D11DDICAPS_THREADING: {
        D3D11DDI_THREADING_CAPS *pCaps = (D3D11DDI_THREADING_CAPS *)pArgs->pData;
        pCaps->Caps = 0; /* no driver-concurrent creates, no commandlists */
        break;
    }
    case D3D11DDICAPS_SHADER: {
        D3D11DDI_SHADER_CAPS *pCaps = (D3D11DDI_SHADER_CAPS *)pArgs->pData;
        pCaps->Caps = D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X;
        break;
    }
    case D3D11_1DDICAPS_D3D11_OPTIONS: {
        D3D11_1DDI_D3D11_OPTIONS_DATA *pCaps = (D3D11_1DDI_D3D11_OPTIONS_DATA *)pArgs->pData;
        pCaps->OutputMergerLogicOp      = TRUE;   /* required for 11.1 */
        pCaps->AssignDebugBinarySupport = FALSE;
        break;
    }
    case D3D11_1DDICAPS_ARCHITECTURE_INFO: {
        /* DDI struct (d3d10umddi.h), not the layout-identical KMD
         * D3DDDICAPS_ARCHITECTURE_INFO. */
        D3D11_1DDI_ARCHITECTURE_INFO_DATA *pCaps =
            (D3D11_1DDI_ARCHITECTURE_INFO_DATA *)pArgs->pData;
        pCaps->TileBasedDeferredRenderer = FALSE; /* dxvk over Vulkan, not TBDR */
        break;
    }
    case D3D11_1DDICAPS_SHADER_MIN_PRECISION_SUPPORT: {
        /* DDI struct (d3d10umddi.h), not the KMD
         * D3DDDICAPS_SHADER_MIN_PRECISION_SUPPORT — same 8 bytes but
         * different fields. 0 = no reduced-precision support (dxvk over
         * Vulkan does not advertise 10/16-bit min precision). */
        D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA *pCaps =
            (D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA *)pArgs->pData;
        pCaps->PixelShaderMinPrecision    = 0;
        pCaps->AllOtherStagesMinPrecision = 0;
        break;
    }
    case D3D11DDICAPS_3DPIPELINESUPPORT: {
        D3D11DDI_3DPIPELINESUPPORT_CAPS *pCaps = (D3D11DDI_3DPIPELINESUPPORT_CAPS *)pArgs->pData;
        /* D3D feature levels are strict supersets: an 11.0-capable device
         * must also report 10.1/10.0. Advertising 11.x alone is an invalid
         * mask and the runtime rejects the adapter (falling back to WARP).
         * The host backend satisfies 10.x as a subset of its 11.x support,
         * so report the contiguous range. */
        pCaps->Caps =
            D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0) |
            D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_1) |
            D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_11_0) |
            D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11_1DDI_3DPIPELINELEVEL_11_1);
        break;
    }
    default:
        /* Unhandled cap types keep the zero-filled pData set above, i.e.
         * "feature off". That covers the WDDM-tier caps an app may query on
         * the 2.0/2.1 interfaces — D3DWDDM1_3DDICAPS_D3D11_OPTIONS1 (136) /
         * _MARKER (137), D3DWDDM2_0DDICAPS_D3D11_OPTIONS2 (143) /
         * _MEMORY_ARCHITECTURE (145) / _D3D11_OPTIONS3 (152) / _GPUVA_CAPS
         * (153). The host backend's support for these optional features
         * cannot be probed at adapter scope (no device exists yet), so zero
         * is the only answer we can back. */
        break;
    }
    return S_OK;
}

/* ---------- OpenAdapter10_2 ---------- */

__declspec(dllexport)
HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *pOpenData)
{
    TR_LOG("OpenAdapter10_2: Interface=0x%08x Version=0x%08x",
           pOpenData->Interface, pOpenData->Version);

    PTRITON_ADAPTER pAdapter = (PTRITON_ADAPTER)(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TRITON_ADAPTER)));
    if (!pAdapter) return E_OUTOFMEMORY;

    pAdapter->hRTAdapter  = pOpenData->hRTAdapter;

    pOpenData->hAdapter.pDrvPrivate = pAdapter;

    D3D10_2DDI_ADAPTERFUNCS *p = pOpenData->pAdapterFuncs_2;
    p->pfnCalcPrivateDeviceSize  = tritonCalcPrivateDeviceSize;
    p->pfnCreateDevice           = tritonCreateDevice;
    p->pfnCloseAdapter           = tritonCloseAdapter;
    p->pfnGetSupportedVersions   = tritonGetSupportedVersions;
    p->pfnGetCaps                = tritonGetCaps;
    return S_OK;
}

#pragma GCC diagnostic pop
