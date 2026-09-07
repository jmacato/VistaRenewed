/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * IDXGIOutput{,1..6}: every wrapper is guest-fabricated and tied to a
 * specific DRM connector (or to a synthetic 1920x1080 fallback when no
 * DRM is available, matching DXVK's dmabuf-backend behavior).
 */

#include "npt_com.h"
#include "npt_display.h"
#include "npt_overrides.h"
#include "npt_device.h"

#include "neptune-protocol/npt_protocol_client_idxgioutput.h"
#include "neptune-protocol/npt_protocol_directx_types.h"
#include "neptune-protocol/npt_protocol_defs.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

/* The protocol header typedefs BOOL but doesn't define TRUE/FALSE. */
#ifndef TRUE
#define TRUE  ((BOOL)1)
#endif
#ifndef FALSE
#define FALSE ((BOOL)0)
#endif

#define NPT_GAMMA_CP_COUNT 1024u

#define NPT_REGISTER_OVERRIDE_DXGI_OUTPUT6(m, f) \
   NPT_REGISTER_OVERRIDE(idxgioutput6, m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_OUTPUT5(m, f) \
   NPT_REGISTER_OVERRIDE(idxgioutput5, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT6(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_OUTPUT4(m, f) \
   NPT_REGISTER_OVERRIDE(idxgioutput4, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT5(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_OUTPUT3(m, f) \
   NPT_REGISTER_OVERRIDE(idxgioutput3, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT4(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_OUTPUT2(m, f) \
   NPT_REGISTER_OVERRIDE(idxgioutput2, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT3(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_OUTPUT1(m, f) \
   NPT_REGISTER_OVERRIDE(idxgioutput1, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT2(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_OUTPUT(m, f) \
   NPT_REGISTER_OVERRIDE(idxgioutput, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT1(m, f)

static const GUID *const output_tiers[] = {
   &NPT_IID_IDXGIOutput,  &NPT_IID_IDXGIOutput1, &NPT_IID_IDXGIOutput2,
   &NPT_IID_IDXGIOutput3, &NPT_IID_IDXGIOutput4, &NPT_IID_IDXGIOutput5,
   &NPT_IID_IDXGIOutput6, NULL,
};

static const GUID *const output_qi_chain[] = {
   &NPT_IID_IUnknown,
   &NPT_IID_IDXGIObject,
   &NPT_IID_IDXGIOutput,
   &NPT_IID_IDXGIOutput1,
   &NPT_IID_IDXGIOutput2,
   &NPT_IID_IDXGIOutput3,
   &NPT_IID_IDXGIOutput4,
   &NPT_IID_IDXGIOutput5,
   &NPT_IID_IDXGIOutput6,
   NULL,
};

struct npt_output_aux {
   _Atomic uint32_t populated;
   uint64_t parent_adapter_host_id;
   uint32_t connector_index;
   /* "\\.\\DISPLAY{n}" in UTF-16LE, NUL-terminated. */
   WCHAR    device_name[32];
   int32_t  desktop_x, desktop_y;
   uint32_t current_width, current_height;
   uint32_t num_modes;
   struct npt_display_mode modes[NPT_DISPLAY_MAX_MODES];
   DXGI_GAMMA_CONTROL gamma;
   /* Per-output WaitForVBlank pacing anchor. */
   _Atomic uint64_t wfvb_anchor_ns;
};

static struct npt_output_aux *
out_aux(void *self)
{
   return ((struct npt_com_base *)self)->aux;
}

static void
fill_device_name(WCHAR out[32], uint32_t connector_index)
{
   /* "\\\\.\\DISPLAYn"; built as UTF-16LE explicitly so we don't rely
    * on compiler L"" encoding. */
   const char prefix[] = { '\\','\\','.','\\','D','I','S','P','L','A','Y' };
   uint32_t pos = 0;
   for (uint32_t i = 0; i < sizeof(prefix); i++)
      out[pos++] = (WCHAR)prefix[i];
   uint32_t n = connector_index + 1;
   char buf[12];
   int len = 0;
   do { buf[len++] = '0' + (n % 10); n /= 10; } while (n && len < (int)sizeof(buf));
   while (len > 0)
      out[pos++] = (WCHAR)buf[--len];
   out[pos] = 0;
   while (pos < 31) out[++pos] = 0;
}

void
npt_output_aux_populate(struct npt_com_base *out_com,
                        struct npt_com_base *parent_adapter,
                        uint32_t connector_index,
                        const struct npt_output_info *info)
{
   if (!out_com || !info)
      return;
   struct npt_output_aux *aux = out_com->aux;
   if (!aux)
      return;
   uint32_t expected = 0;
   if (!atomic_compare_exchange_strong_explicit(
          &aux->populated, &expected, 1,
          memory_order_acq_rel, memory_order_acquire))
      return;

   aux->parent_adapter_host_id = parent_adapter ? parent_adapter->base.id : 0;
   aux->connector_index = connector_index;
   fill_device_name(aux->device_name, connector_index);
   aux->desktop_x = info->desktop_x;
   aux->desktop_y = info->desktop_y;
   aux->current_width = info->current_width;
   aux->current_height = info->current_height;
   aux->num_modes = info->num_modes <= NPT_DISPLAY_MAX_MODES
                        ? info->num_modes : NPT_DISPLAY_MAX_MODES;
   memcpy(aux->modes, info->modes,
          aux->num_modes * sizeof(aux->modes[0]));

   /* Initial gamma = identity ramp; ScaleAndOffsetSupported=FALSE means
    * Scale/Offset are ignored, but zero them to be tidy. */
   memset(&aux->gamma, 0, sizeof(aux->gamma));
   for (uint32_t i = 0; i < NPT_GAMMA_CP_COUNT; i++) {
      float f = (float)i / (float)(NPT_GAMMA_CP_COUNT - 1);
      aux->gamma.GammaCurve[i].Red   = f;
      aux->gamma.GammaCurve[i].Green = f;
      aux->gamma.GammaCurve[i].Blue  = f;
   }
}

static void
out_aux_init(struct npt_com_base *com, struct npt_device *dev, uint64_t host_id)
{
   (void)dev; (void)host_id;
   com->aux_destroy = free;
}

/* Returns 0 for non-scanout-capable formats. */
static uint32_t
dxgi_format_bpp(DXGI_FORMAT f)
{
   switch (f) {
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
   case DXGI_FORMAT_R10G10B10A2_UNORM:
   case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return 32;
   default:
      return 0;
   }
}

static void
fill_output_desc(const struct npt_output_aux *aux, DXGI_OUTPUT_DESC *desc)
{
   memcpy(desc->DeviceName, aux->device_name, sizeof(desc->DeviceName));
   desc->DesktopCoordinates.left   = aux->desktop_x;
   desc->DesktopCoordinates.top    = aux->desktop_y;
   desc->DesktopCoordinates.right  = aux->desktop_x +
                                     (LONG)aux->current_width;
   desc->DesktopCoordinates.bottom = aux->desktop_y +
                                     (LONG)aux->current_height;
   desc->AttachedToDesktop = TRUE;
   desc->Rotation          = DXGI_MODE_ROTATION_IDENTITY;
   /* Opaque HMONITOR cookie tied to this output's ordinal. */
   desc->Monitor = (HMONITOR)(uintptr_t)(aux->connector_index + 1);
}

static void
mode_to_dxgi(const struct npt_display_mode *src, DXGI_FORMAT format,
             DXGI_MODE_DESC *dst)
{
   memset(dst, 0, sizeof(*dst));
   dst->Width                    = src->width;
   dst->Height                   = src->height;
   dst->RefreshRate.Numerator    = src->refresh_num;
   dst->RefreshRate.Denominator  = src->refresh_den;
   dst->Format                   = format;
   dst->ScanlineOrdering         = src->interlaced
      ? DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST
      : DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
   dst->Scaling                  = DXGI_MODE_SCALING_UNSPECIFIED;
}

static void
mode_to_dxgi1(const struct npt_display_mode *src, DXGI_FORMAT format,
              DXGI_MODE_DESC1 *dst)
{
   memset(dst, 0, sizeof(*dst));
   dst->Width                    = src->width;
   dst->Height                   = src->height;
   dst->RefreshRate.Numerator    = src->refresh_num;
   dst->RefreshRate.Denominator  = src->refresh_den;
   dst->Format                   = format;
   dst->ScanlineOrdering         = src->interlaced
      ? DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST
      : DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
   dst->Scaling                  = DXGI_MODE_SCALING_UNSPECIFIED;
   dst->Stereo                   = FALSE;
}

static UINT
count_filtered_modes(const struct npt_output_aux *aux,
                     UINT flags, uint32_t target_bpp)
{
   UINT n = 0;
   for (uint32_t i = 0; i < aux->num_modes; i++) {
      const struct npt_display_mode *m = &aux->modes[i];
      if (m->interlaced && !(flags & DXGI_ENUM_MODES_INTERLACED))
         continue;
      if (m->bpp != target_bpp)
         continue;
      n++;
   }
   return n;
}

/* ============================================================
 * Method overrides (shared across all IDXGIOutputN tiers)
 * ============================================================ */

static HRESULT NPT_STDMETHODCALLTYPE
out_QueryInterface_override(void *self, REFIID riid, void **ppvObject)
{
   HRESULT hr = npt_com_default_query_interface_chain(
      self, riid, ppvObject, output_qi_chain);
   return hr == NPT_S_OK ? hr : NPT_E_NOINTERFACE;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_GetParent_override(void *self, REFIID riid, void **ppParent)
{
   if (!ppParent)
      return NPT_E_POINTER;
   *ppParent = NULL;
   struct npt_output_aux *aux = out_aux(self);
   if (!aux || !aux->parent_adapter_host_id)
      return NPT_E_FAIL;
   struct npt_device *dev = npt_com_self_device(self);
   void *parent = npt_com_get_or_wrap(dev, &NPT_IID_IDXGIAdapter4,
                                      aux->parent_adapter_host_id, NULL);
   if (!parent)
      return NPT_E_FAIL;
   /* npt_com_get_or_wrap returns with an extra public ref; let QI
    * either match the requested IID (no extra ref) or fall through to
    * release. */
   struct npt_com_base *parent_com = parent;
   const struct npt_idxgioutput_client_vtbl *vtbl =
      (const struct npt_idxgioutput_client_vtbl *)parent_com->lpVtbl;
   HRESULT hr = vtbl->QueryInterface(parent, riid, ppParent);
   /* Drop the AddRef from get_or_wrap; QI added its own if it matched. */
   vtbl->Release(parent);
   return hr;
}

/* The generated defaults for these encode on the wire, but every output
 * wrapper carries a guest-fabricated id the host never registered --
 * any wire method is a fatal host decode error.  Stub locally, matching
 * the guest swapchain. */
static HRESULT NPT_STDMETHODCALLTYPE
out_SetPrivateData_override(void *self, const GUID *Name, UINT DataSize,
                            const void *pData)
{
   (void)self; (void)Name; (void)DataSize; (void)pData;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_SetPrivateDataInterface_override(void *self, const GUID *Name,
                                     const IUnknown *pUnknown)
{
   (void)self; (void)Name; (void)pUnknown;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_GetPrivateData_override(void *self, const GUID *Name, UINT *pDataSize,
                            void *pData)
{
   (void)self; (void)Name; (void)pData;
   if (pDataSize)
      *pDataSize = 0;
   return NPT_DXGI_ERROR_NOT_FOUND;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_GetDesc_override(void *self, DXGI_OUTPUT_DESC *pDesc)
{
   if (!pDesc)
      return NPT_DXGI_ERROR_INVALID_CALL;
   struct npt_output_aux *aux = out_aux(self);
   if (!aux)
      return NPT_E_FAIL;
   memset(pDesc, 0, sizeof(*pDesc));
   fill_output_desc(aux, pDesc);
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_GetDisplayModeList_override(void *self, DXGI_FORMAT EnumFormat,
                                UINT Flags, UINT *pNumModes,
                                DXGI_MODE_DESC *pDesc)
{
   if (!pNumModes)
      return NPT_DXGI_ERROR_INVALID_CALL;
   if (EnumFormat == DXGI_FORMAT_UNKNOWN) {
      *pNumModes = 0;
      return NPT_S_OK;
   }
   uint32_t target_bpp = dxgi_format_bpp(EnumFormat);
   if (target_bpp == 0) {
      *pNumModes = 0;
      return NPT_S_OK;
   }
   struct npt_output_aux *aux = out_aux(self);
   if (!aux) { *pNumModes = 0; return NPT_E_FAIL; }

   UINT total = count_filtered_modes(aux, Flags, target_bpp);
   if (!pDesc) { *pNumModes = total; return NPT_S_OK; }

   UINT cap = *pNumModes;
   UINT written = 0;
   for (uint32_t i = 0; i < aux->num_modes && written < cap; i++) {
      const struct npt_display_mode *m = &aux->modes[i];
      if (m->interlaced && !(Flags & DXGI_ENUM_MODES_INTERLACED))
         continue;
      if (m->bpp != target_bpp)
         continue;
      mode_to_dxgi(m, EnumFormat, &pDesc[written]);
      written++;
   }
   *pNumModes = written;
   if (total > cap)
      return NPT_DXGI_ERROR_MORE_DATA;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
out1_GetDisplayModeList1_override(void *self, DXGI_FORMAT EnumFormat,
                                  UINT Flags, UINT *pNumModes,
                                  DXGI_MODE_DESC1 *pDesc)
{
   if (!pNumModes)
      return NPT_DXGI_ERROR_INVALID_CALL;
   if (EnumFormat == DXGI_FORMAT_UNKNOWN) {
      *pNumModes = 0;
      return NPT_S_OK;
   }
   uint32_t target_bpp = dxgi_format_bpp(EnumFormat);
   if (target_bpp == 0) {
      *pNumModes = 0;
      return NPT_S_OK;
   }
   struct npt_output_aux *aux = out_aux(self);
   if (!aux) { *pNumModes = 0; return NPT_E_FAIL; }

   UINT total = count_filtered_modes(aux, Flags, target_bpp);
   if (!pDesc) { *pNumModes = total; return NPT_S_OK; }

   UINT cap = *pNumModes;
   UINT written = 0;
   for (uint32_t i = 0; i < aux->num_modes && written < cap; i++) {
      const struct npt_display_mode *m = &aux->modes[i];
      if (m->interlaced && !(Flags & DXGI_ENUM_MODES_INTERLACED))
         continue;
      if (m->bpp != target_bpp)
         continue;
      mode_to_dxgi1(m, EnumFormat, &pDesc[written]);
      written++;
   }
   *pNumModes = written;
   if (total > cap)
      return NPT_DXGI_ERROR_MORE_DATA;
   return NPT_S_OK;
}

struct fcm_list {
   DXGI_MODE_DESC1 modes[NPT_DISPLAY_MAX_MODES];
   UINT count;
};

static void
fcm_filter_in_place(struct fcm_list *list, const DXGI_MODE_DESC1 *target)
{
   bool test_scanline = false;
   bool test_scaling  = false;
   bool test_format   = false;
   for (UINT i = 0; i < list->count; i++) {
      const DXGI_MODE_DESC1 *m = &list->modes[i];
      test_scanline |= target->ScanlineOrdering != DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED &&
                       target->ScanlineOrdering == m->ScanlineOrdering;
      test_scaling  |= target->Scaling != DXGI_MODE_SCALING_UNSPECIFIED &&
                       target->Scaling == m->Scaling;
      test_format   |= target->Format != DXGI_FORMAT_UNKNOWN &&
                       target->Format == m->Format;
   }

   UINT w = 0;
   for (UINT i = 0; i < list->count; i++) {
      const DXGI_MODE_DESC1 *m = &list->modes[i];
      bool skip = m->Stereo != target->Stereo;
      if (test_scanline)
         skip |= m->ScanlineOrdering != target->ScanlineOrdering;
      if (test_scaling)
         skip |= m->Scaling != target->Scaling;
      if (test_format)
         skip |= m->Format != target->Format;
      if (!skip) {
         if (w != i)
            list->modes[w] = *m;
         w++;
      }
   }
   list->count = w;

   if (target->Width && list->count) {
      uint32_t min_diff = UINT32_MAX;
      for (UINT i = 0; i < list->count; i++) {
         uint32_t dw = list->modes[i].Width > target->Width
                          ? list->modes[i].Width - target->Width
                          : target->Width - list->modes[i].Width;
         uint32_t dh = list->modes[i].Height > target->Height
                          ? list->modes[i].Height - target->Height
                          : target->Height - list->modes[i].Height;
         uint32_t d = dw + dh;
         if (d < min_diff) min_diff = d;
      }
      w = 0;
      for (UINT i = 0; i < list->count; i++) {
         uint32_t dw = list->modes[i].Width > target->Width
                          ? list->modes[i].Width - target->Width
                          : target->Width - list->modes[i].Width;
         uint32_t dh = list->modes[i].Height > target->Height
                          ? list->modes[i].Height - target->Height
                          : target->Height - list->modes[i].Height;
         if (dw + dh == min_diff) {
            if (w != i)
               list->modes[w] = list->modes[i];
            w++;
         }
      }
      list->count = w;
   }

   if (target->RefreshRate.Numerator && target->RefreshRate.Denominator &&
       list->count) {
      uint64_t target_rate = (uint64_t)target->RefreshRate.Numerator;
      uint64_t target_den  = (uint64_t)target->RefreshRate.Denominator;
      uint64_t min_diff = UINT64_MAX;
      for (UINT i = 0; i < list->count; i++) {
         uint64_t num = list->modes[i].RefreshRate.Numerator;
         uint64_t den = list->modes[i].RefreshRate.Denominator;
         if (!den) continue;
         uint64_t rate = num * target_den / den;
         uint64_t diff = rate > target_rate ? rate - target_rate
                                            : target_rate - rate;
         if (diff < min_diff) min_diff = diff;
      }
      w = 0;
      for (UINT i = 0; i < list->count; i++) {
         uint64_t num = list->modes[i].RefreshRate.Numerator;
         uint64_t den = list->modes[i].RefreshRate.Denominator;
         if (!den) continue;
         uint64_t rate = num * target_den / den;
         uint64_t diff = rate > target_rate ? rate - target_rate
                                            : target_rate - rate;
         if (diff == min_diff) {
            if (w != i)
               list->modes[w] = list->modes[i];
            w++;
         }
      }
      list->count = w;
   }
}

static HRESULT NPT_STDMETHODCALLTYPE
out1_FindClosestMatchingMode1_override(void *self,
                                       const DXGI_MODE_DESC1 *pModeToMatch,
                                       DXGI_MODE_DESC1 *pClosestMatch,
                                       IUnknown *pConcernedDevice)
{
   if (!pModeToMatch || !pClosestMatch)
      return NPT_DXGI_ERROR_INVALID_CALL;
   if (pModeToMatch->Format == DXGI_FORMAT_UNKNOWN && !pConcernedDevice)
      return NPT_DXGI_ERROR_INVALID_CALL;
   if ((pModeToMatch->Width == 0) != (pModeToMatch->Height == 0))
      return NPT_DXGI_ERROR_INVALID_CALL;

   struct npt_output_aux *aux = out_aux(self);
   if (!aux || !aux->num_modes)
      return NPT_DXGI_ERROR_NOT_FOUND;

   const struct npt_display_mode *cur = &aux->modes[0];
   for (uint32_t i = 0; i < aux->num_modes; i++) {
      if (aux->modes[i].preferred) { cur = &aux->modes[i]; break; }
   }
   DXGI_MODE_DESC1 active;
   mode_to_dxgi1(cur, DXGI_FORMAT_R8G8B8A8_UNORM, &active);

   DXGI_FORMAT target_format = pModeToMatch->Format;
   if (target_format == DXGI_FORMAT_UNKNOWN)
      target_format = active.Format;

   DXGI_MODE_DESC1 default_mode = { 0 };
   default_mode.Stereo = pModeToMatch->Stereo;
   if (pModeToMatch->ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED)
      default_mode.ScanlineOrdering = active.ScanlineOrdering;
   if (pModeToMatch->Scaling == DXGI_MODE_SCALING_UNSPECIFIED)
      default_mode.Scaling = active.Scaling;
   if (pModeToMatch->Format == DXGI_FORMAT_UNKNOWN)
      default_mode.Format = active.Format;
   if (!pModeToMatch->Width) {
      default_mode.Width  = active.Width;
      default_mode.Height = active.Height;
   }
   if (!pModeToMatch->RefreshRate.Numerator ||
       !pModeToMatch->RefreshRate.Denominator) {
      default_mode.RefreshRate.Numerator   = active.RefreshRate.Numerator;
      default_mode.RefreshRate.Denominator = active.RefreshRate.Denominator;
   }

   struct fcm_list list = { 0 };
   uint32_t target_bpp = dxgi_format_bpp(target_format);
   if (target_bpp == 0)
      return NPT_DXGI_ERROR_NOT_FOUND;
   for (uint32_t i = 0; i < aux->num_modes &&
                        list.count < NPT_DISPLAY_MAX_MODES; i++) {
      const struct npt_display_mode *m = &aux->modes[i];
      if (m->bpp != target_bpp)
         continue;
      mode_to_dxgi1(m, target_format, &list.modes[list.count++]);
   }
   if (!list.count)
      return NPT_DXGI_ERROR_NOT_FOUND;

   DXGI_MODE_DESC1 target_mode = *pModeToMatch;
   fcm_filter_in_place(&list, &target_mode);
   if (!list.count)
      return NPT_DXGI_ERROR_NOT_FOUND;
   fcm_filter_in_place(&list, &default_mode);
   if (!list.count)
      return NPT_DXGI_ERROR_NOT_FOUND;

   *pClosestMatch = list.modes[0];
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_FindClosestMatchingMode_override(void *self,
                                     const DXGI_MODE_DESC *pModeToMatch,
                                     DXGI_MODE_DESC *pClosestMatch,
                                     IUnknown *pConcernedDevice)
{
   if (!pModeToMatch || !pClosestMatch)
      return NPT_DXGI_ERROR_INVALID_CALL;
   DXGI_MODE_DESC1 in = { 0 };
   in.Width            = pModeToMatch->Width;
   in.Height           = pModeToMatch->Height;
   in.RefreshRate      = pModeToMatch->RefreshRate;
   in.Format           = pModeToMatch->Format;
   in.ScanlineOrdering = pModeToMatch->ScanlineOrdering;
   in.Scaling          = pModeToMatch->Scaling;
   in.Stereo           = FALSE;

   DXGI_MODE_DESC1 out = { 0 };
   HRESULT hr = out1_FindClosestMatchingMode1_override(self, &in, &out,
                                                       pConcernedDevice);
   if (hr != NPT_S_OK)
      return hr;
   pClosestMatch->Width            = out.Width;
   pClosestMatch->Height           = out.Height;
   pClosestMatch->RefreshRate      = out.RefreshRate;
   pClosestMatch->Format           = out.Format;
   pClosestMatch->ScanlineOrdering = out.ScanlineOrdering;
   pClosestMatch->Scaling          = out.Scaling;
   return NPT_S_OK;
}

/* IDXGIOutput::WaitForVBlank pacing.  Strict FIFO per output: sleep
 * until the next n-th anchored vblank from "now".  Per-output anchor
 * lives in aux so multiple outputs with different refresh periods stay
 * independent. */
static uint64_t
now_monotonic_ns(void)
{
#if defined(_WIN32)
   LARGE_INTEGER freq, ctr;
   QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&ctr);
   return (uint64_t)ctr.QuadPart * 1000000000ull / (uint64_t)freq.QuadPart;
#else
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void
sleep_until_ns(uint64_t target_ns, uint64_t now_ns)
{
   if (target_ns <= now_ns)
      return;
   uint64_t delta_ns = target_ns - now_ns;
#if defined(_WIN32)
   Sleep((DWORD)((delta_ns + 999999) / 1000000));
#else
   struct timespec ts = {
      .tv_sec  = (time_t)(delta_ns / 1000000000ull),
      .tv_nsec = (long)(delta_ns % 1000000000ull),
   };
   nanosleep(&ts, NULL);
#endif
}

static uint64_t
output_period_ns(const struct npt_output_aux *aux)
{
   uint64_t period_ns = 16666666;  /* 1/60s default */
   if (aux && aux->num_modes) {
      const struct npt_display_mode *m = &aux->modes[0];
      for (uint32_t i = 0; i < aux->num_modes; i++) {
         if (aux->modes[i].preferred) { m = &aux->modes[i]; break; }
      }
      if (m->refresh_num && m->refresh_den)
         period_ns = (uint64_t)1000000000ull * m->refresh_den / m->refresh_num;
   }
   return period_ns;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_WaitForVBlank_override(void *self)
{
   struct npt_output_aux *aux = out_aux(self);
   if (!aux)
      return NPT_S_OK;
   uint64_t period_ns = output_period_ns(aux);
   if (!period_ns)
      return NPT_S_OK;
   uint64_t now = now_monotonic_ns();
   uint64_t anchor = atomic_load_explicit(&aux->wfvb_anchor_ns,
                                          memory_order_acquire);
   if (anchor == 0) {
      uint64_t expected = 0;
      if (atomic_compare_exchange_strong(&aux->wfvb_anchor_ns,
                                         &expected, now ? now : 1))
         anchor = now ? now : 1;
      else
         anchor = expected;
   }
   uint64_t elapsed = now > anchor ? now - anchor : 0;
   uint64_t past = elapsed / period_ns;
   uint64_t target = anchor + (past + 1) * period_ns;
   sleep_until_ns(target, now);
   atomic_store_explicit(&aux->wfvb_anchor_ns, target,
                         memory_order_release);
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_TakeOwnership_override(void *self, IUnknown *pDevice, BOOL Exclusive)
{
   (void)self; (void)pDevice; (void)Exclusive;
   return NPT_S_OK;
}

static void NPT_STDMETHODCALLTYPE
out_ReleaseOwnership_override(void *self)
{
   (void)self;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_GetGammaControlCapabilities_override(
   void *self, DXGI_GAMMA_CONTROL_CAPABILITIES *pGammaCaps)
{
   (void)self;
   if (!pGammaCaps)
      return NPT_E_POINTER;
   pGammaCaps->ScaleAndOffsetSupported = FALSE;
   pGammaCaps->MinConvertedValue       = 0.0f;
   pGammaCaps->MaxConvertedValue       = 1.0f;
   pGammaCaps->NumGammaControlPoints   = NPT_GAMMA_CP_COUNT;
   for (uint32_t i = 0; i < NPT_GAMMA_CP_COUNT; i++)
      pGammaCaps->ControlPointPositions[i] =
         (float)i / (float)(NPT_GAMMA_CP_COUNT - 1);
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_SetGammaControl_override(void *self, const DXGI_GAMMA_CONTROL *pArray)
{
   if (!pArray)
      return NPT_E_POINTER;
   struct npt_output_aux *aux = out_aux(self);
   if (!aux)
      return NPT_E_FAIL;
   aux->gamma = *pArray;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_GetGammaControl_override(void *self, DXGI_GAMMA_CONTROL *pArray)
{
   if (!pArray)
      return NPT_E_POINTER;
   struct npt_output_aux *aux = out_aux(self);
   if (!aux)
      return NPT_E_FAIL;
   *pArray = aux->gamma;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_SetDisplaySurface_override(void *self, IDXGISurface *pScanoutSurface)
{
   (void)self; (void)pScanoutSurface;
   return NPT_E_NOTIMPL;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_GetDisplaySurfaceData_override(void *self, IDXGISurface *pDestination)
{
   (void)self; (void)pDestination;
   return NPT_E_NOTIMPL;
}

static HRESULT NPT_STDMETHODCALLTYPE
out1_GetDisplaySurfaceData1_override(void *self, IDXGIResource *pDestination)
{
   (void)self; (void)pDestination;
   return NPT_E_NOTIMPL;
}

static HRESULT NPT_STDMETHODCALLTYPE
out_GetFrameStatistics_override(void *self, DXGI_FRAME_STATISTICS *pStats)
{
   (void)self;
   if (!pStats)
      return NPT_E_POINTER;
   memset(pStats, 0, sizeof(*pStats));
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
out1_DuplicateOutput_override(void *self, IUnknown *pDevice,
                              IDXGIOutputDuplication **ppOutputDuplication)
{
   (void)self; (void)pDevice;
   if (ppOutputDuplication)
      *ppOutputDuplication = NULL;
   return NPT_DXGI_ERROR_UNSUPPORTED;
}

static BOOL NPT_STDMETHODCALLTYPE
out2_SupportsOverlays_override(void *self)
{
   (void)self;
   return FALSE;
}

static HRESULT NPT_STDMETHODCALLTYPE
out3_CheckOverlaySupport_override(void *self, DXGI_FORMAT EnumFormat,
                                  IUnknown *pConcernedDevice, UINT *pFlags)
{
   (void)self; (void)EnumFormat; (void)pConcernedDevice;
   if (pFlags)
      *pFlags = 0;
   return NPT_DXGI_ERROR_UNSUPPORTED;
}

static HRESULT NPT_STDMETHODCALLTYPE
out4_CheckOverlayColorSpaceSupport_override(void *self, DXGI_FORMAT Format,
                                            DXGI_COLOR_SPACE_TYPE ColorSpace,
                                            IUnknown *pConcernedDevice,
                                            UINT *pFlags)
{
   (void)self; (void)Format; (void)ColorSpace; (void)pConcernedDevice;
   if (pFlags)
      *pFlags = 0;
   return NPT_DXGI_ERROR_UNSUPPORTED;
}

static HRESULT NPT_STDMETHODCALLTYPE
out5_DuplicateOutput1_override(void *self, IUnknown *pDevice, UINT Flags,
                               UINT SupportedFormatsCount,
                               const DXGI_FORMAT *pSupportedFormats,
                               IDXGIOutputDuplication **ppOutputDuplication)
{
   (void)self; (void)pDevice; (void)Flags;
   (void)SupportedFormatsCount; (void)pSupportedFormats;
   if (ppOutputDuplication)
      *ppOutputDuplication = NULL;
   return NPT_DXGI_ERROR_UNSUPPORTED;
}

static HRESULT NPT_STDMETHODCALLTYPE
out6_GetDesc1_override(void *self, DXGI_OUTPUT_DESC1 *pDesc)
{
   if (!pDesc)
      return NPT_DXGI_ERROR_INVALID_CALL;
   struct npt_output_aux *aux = out_aux(self);
   if (!aux)
      return NPT_E_FAIL;
   memset(pDesc, 0, sizeof(*pDesc));
   /* DESC1 = DESC + HDR metadata; layouts share the prefix. */
   fill_output_desc(aux, (DXGI_OUTPUT_DESC *)pDesc);
   pDesc->BitsPerColor = 10;
   pDesc->ColorSpace   = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
out6_CheckHardwareCompositionSupport_override(void *self, UINT *pFlags)
{
   (void)self;
   if (pFlags)
      *pFlags = 0;
   return NPT_S_OK;
}

void
npt_overrides_dxgi_output_init(void)
{
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (QueryInterface,
                                      out_QueryInterface_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (GetParent,
                                      out_GetParent_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (SetPrivateData,
                                      out_SetPrivateData_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (SetPrivateDataInterface,
                                      out_SetPrivateDataInterface_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (GetPrivateData,
                                      out_GetPrivateData_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (GetDesc,
                                      out_GetDesc_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (GetDisplayModeList,
                                      out_GetDisplayModeList_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (FindClosestMatchingMode,
                                      out_FindClosestMatchingMode_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (WaitForVBlank,
                                      out_WaitForVBlank_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (TakeOwnership,
                                      out_TakeOwnership_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (ReleaseOwnership,
                                      out_ReleaseOwnership_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (GetGammaControlCapabilities,
                                      out_GetGammaControlCapabilities_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (SetGammaControl,
                                      out_SetGammaControl_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (GetGammaControl,
                                      out_GetGammaControl_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (SetDisplaySurface,
                                      out_SetDisplaySurface_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (GetDisplaySurfaceData,
                                      out_GetDisplaySurfaceData_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT (GetFrameStatistics,
                                      out_GetFrameStatistics_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT1(GetDisplayModeList1,
                                      out1_GetDisplayModeList1_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT1(FindClosestMatchingMode1,
                                      out1_FindClosestMatchingMode1_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT1(GetDisplaySurfaceData1,
                                      out1_GetDisplaySurfaceData1_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT1(DuplicateOutput,
                                      out1_DuplicateOutput_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT2(SupportsOverlays,
                                      out2_SupportsOverlays_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT3(CheckOverlaySupport,
                                      out3_CheckOverlaySupport_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT4(CheckOverlayColorSpaceSupport,
                                      out4_CheckOverlayColorSpaceSupport_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT5(DuplicateOutput1,
                                      out5_DuplicateOutput1_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT6(GetDesc1,
                                      out6_GetDesc1_override);
   NPT_REGISTER_OVERRIDE_DXGI_OUTPUT6(CheckHardwareCompositionSupport,
                                      out6_CheckHardwareCompositionSupport_override);

   npt_com_register_family(output_tiers,
                           sizeof(struct npt_output_aux),
                           out_aux_init);
}
