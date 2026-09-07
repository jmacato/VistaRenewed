/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * GetDesc returns the cached descriptor; falls back to the
 * generated sync thunk when the wrapper came in via swapchain
 * GetBuffer / OpenSharedResource.  All dimensions share
 * npt_d3d11_texture but each family has its own vtbl storage.
 */

#include "npt_com.h"
#include "npt_overrides.h"
#include "npt_resource.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_client_id3d11texture1d.h"
#include "neptune-protocol/npt_protocol_client_id3d11texture2d.h"
#include "neptune-protocol/npt_protocol_client_id3d11texture3d.h"

#define NPT_REGISTER_OVERRIDE_D3D11_TEXTURE1D(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11texture1d, m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_TEXTURE2D1(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11texture2d1, m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_TEXTURE2D(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11texture2d, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_TEXTURE2D1(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_TEXTURE3D1(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11texture3d1, m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_TEXTURE3D(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11texture3d, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_TEXTURE3D1(m, f)

static const GUID *const texture1d_tiers[] = { &NPT_IID_ID3D11Texture1D, NULL };
static const GUID *const texture2d_tiers[] = {
   &NPT_IID_ID3D11Texture2D, &NPT_IID_ID3D11Texture2D1, NULL };
static const GUID *const texture3d_tiers[] = {
   &NPT_IID_ID3D11Texture3D, &NPT_IID_ID3D11Texture3D1, NULL };

static void NPT_STDMETHODCALLTYPE
tex1d_GetDesc_override(void *self, D3D11_TEXTURE1D_DESC *pDesc)
{
   if (!pDesc) return;
   struct npt_d3d11_texture *t = (struct npt_d3d11_texture *)self;
   if (npt_d3d11_texture_has_desc(t)) {
      npt_d3d11_texture_fill_desc1d(t, pDesc);
      return;
   }
   npt_id3d11texture1d_default_GetDesc(self, pDesc);
}

static void NPT_STDMETHODCALLTYPE
tex2d_GetDesc_override(void *self, D3D11_TEXTURE2D_DESC *pDesc)
{
   if (!pDesc) return;
   struct npt_d3d11_texture *t = (struct npt_d3d11_texture *)self;
   if (npt_d3d11_texture_has_desc(t)) {
      npt_d3d11_texture_fill_desc2d(t, pDesc);
      return;
   }
   npt_id3d11texture2d_default_GetDesc(self, pDesc);
}

static void NPT_STDMETHODCALLTYPE
tex2d1_GetDesc1_override(void *self, D3D11_TEXTURE2D_DESC1 *pDesc)
{
   if (!pDesc) return;
   struct npt_d3d11_texture *t = (struct npt_d3d11_texture *)self;
   if (npt_d3d11_texture_has_desc(t)) {
      npt_d3d11_texture_fill_desc2d1(t, pDesc);
      return;
   }
   npt_id3d11texture2d1_default_GetDesc1(self, pDesc);
}

static void NPT_STDMETHODCALLTYPE
tex3d_GetDesc_override(void *self, D3D11_TEXTURE3D_DESC *pDesc)
{
   if (!pDesc) return;
   struct npt_d3d11_texture *t = (struct npt_d3d11_texture *)self;
   if (npt_d3d11_texture_has_desc(t)) {
      npt_d3d11_texture_fill_desc3d(t, pDesc);
      return;
   }
   npt_id3d11texture3d_default_GetDesc(self, pDesc);
}

static void NPT_STDMETHODCALLTYPE
tex3d1_GetDesc1_override(void *self, D3D11_TEXTURE3D_DESC1 *pDesc)
{
   if (!pDesc) return;
   struct npt_d3d11_texture *t = (struct npt_d3d11_texture *)self;
   if (npt_d3d11_texture_has_desc(t)) {
      npt_d3d11_texture_fill_desc3d1(t, pDesc);
      return;
   }
   npt_id3d11texture3d1_default_GetDesc1(self, pDesc);
}

void
npt_overrides_d3d11_texture_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D11_TEXTURE1D(GetDesc, tex1d_GetDesc_override);
   npt_com_register_family(texture1d_tiers,
                           sizeof(struct npt_d3d11_texture_aux),
                           npt_d3d11_texture_aux_init);

   NPT_REGISTER_OVERRIDE_D3D11_TEXTURE2D (GetDesc,  tex2d_GetDesc_override);
   NPT_REGISTER_OVERRIDE_D3D11_TEXTURE2D1(GetDesc1, tex2d1_GetDesc1_override);
   npt_com_register_family(texture2d_tiers,
                           sizeof(struct npt_d3d11_texture_aux),
                           npt_d3d11_texture_aux_init);

   NPT_REGISTER_OVERRIDE_D3D11_TEXTURE3D (GetDesc,  tex3d_GetDesc_override);
   NPT_REGISTER_OVERRIDE_D3D11_TEXTURE3D1(GetDesc1, tex3d1_GetDesc1_override);
   npt_com_register_family(texture3d_tiers,
                           sizeof(struct npt_d3d11_texture_aux),
                           npt_d3d11_texture_aux_init);
}
