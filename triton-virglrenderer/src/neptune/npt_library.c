/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Host-side D3D backend library loader.  Any library may be absent;
 * the corresponding top-level overrides return E_FAIL in that case.
 */

#include "npt_library.h"

#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif
#include <stdio.h>

/* Defaults; override via NPT_*_LIBRARY_PATH env vars.
 *
 * On darwin the D3D entry points and the embedder event API both live in
 * one backend umbrella, dlopened (never linked) and dlsym'd at runtime:
 * libd3dmetal-native (Apple D3DMetal, x86_64/Rosetta) on the x86_64 slice,
 * libdxmt-native (native D3D11-on-Metal) on the arm64 slice.  All three
 * D3D slots resolve to that one image.  DXMT has no D3D12 entry point; that
 * slot's dlsym fails and the D3D12 overrides degrade to E_FAIL by design. */
#if defined(__APPLE__) && defined(__aarch64__)
#define NPT_D3D11_LIBRARY_DEFAULT "libdxmt-native.dylib"
#define NPT_DXGI_LIBRARY_DEFAULT  "libdxmt-native.dylib"
#define NPT_D3D12_LIBRARY_DEFAULT "libdxmt-native.dylib"
#elif defined(__APPLE__)
#define NPT_D3D11_LIBRARY_DEFAULT "libd3dmetal-native.dylib"
#define NPT_DXGI_LIBRARY_DEFAULT  "libd3dmetal-native.dylib"
#define NPT_D3D12_LIBRARY_DEFAULT "libd3dmetal-native.dylib"
#else
#define NPT_D3D11_LIBRARY_DEFAULT "libdxvk_d3d11.so"
#define NPT_DXGI_LIBRARY_DEFAULT  "libdxvk_dxgi.so"
#define NPT_D3D12_LIBRARY_DEFAULT "libvkd3d-proton-d3d12.so"
#endif

#ifdef HAVE_DLFCN_H

static void *
npt_library_open(const char *env_var, const char *default_name)
{
   const char *path = getenv(env_var);
   if (!path)
      path = default_name;

   dlerror(); /* clear */
   void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!handle) {
      npt_log("failed to open %s (%s): %s", env_var, path, dlerror());
   }
   return handle;
}

static void *
npt_library_sym(void *handle, const char *name)
{
   dlerror(); /* clear */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
   void *sym = dlsym(handle, name);
#pragma GCC diagnostic pop

   const char *error = dlerror();
   if (error) {
      npt_log("failed to load %s: %s", name, error);
      return NULL;
   }
   return sym;
}

#ifdef __APPLE__
/* Bind the backend's embedder event API from the loaded umbrella.
 * d3dmetal exports dmn_event_*, dxmt exports dxmt_event_*; probe create()
 * for each prefix to identify the backend, then bind the trio npt_event.c
 * uses.  Also records lib->backend for the workaround-flags gate below. */
static void
npt_library_load_event_api(struct npt_d3d_library *lib)
{
   void *mod = lib->d3d11_module ? lib->d3d11_module
             : lib->dxgi_module  ? lib->dxgi_module
             :                     lib->d3d12_module;
   if (!mod)
      return;

   static const struct {
      const char *prefix;
      enum npt_backend_kind kind;
   } candidates[] = {
      { "dmn_",  NPT_BACKEND_D3DMETAL },
      { "dxmt_", NPT_BACKEND_DXMT },
   };

   for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
      char name[32];
      snprintf(name, sizeof(name), "%sevent_create", candidates[i].prefix);
      dlerror();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
      void *create = dlsym(mod, name);
#pragma GCC diagnostic pop
      if (!create)
         continue;

      lib->backend = candidates[i].kind;
      lib->pfn_event_create =
         ((union { void *p; void *(*f)(int, int); }){ .p = create }).f;

      snprintf(name, sizeof(name), "%sevent_close", candidates[i].prefix);
      lib->pfn_event_close =
         ((union { void *p; void (*f)(void *); }){
            .p = npt_library_sym(mod, name) }).f;

      snprintf(name, sizeof(name), "%sevent_dup_fd", candidates[i].prefix);
      lib->pfn_event_dup_fd =
         ((union { void *p; int (*f)(void *); }){
            .p = npt_library_sym(mod, name) }).f;
      return;
   }
   npt_log("backend event API (dmn_/dxmt_event_*) not found");
}

static void
npt_library_load_dxmt_clear_api(struct npt_d3d_library *lib)
{
   if (lib->backend != NPT_BACKEND_DXMT || !lib->d3d11_module)
      return;

   lib->pfn_clear_depth_stencil_rects =
      ((union {
         void *p;
         HRESULT (*f)(void *, void *, uint32_t, float, uint8_t, uint32_t,
                      const int32_t *);
      }){
         .p = npt_library_sym(
            lib->d3d11_module,
            "dxmt_d3d11_clear_depth_stencil_rects")
      }).f;
}
#endif /* __APPLE__ */

#ifdef __linux__
static void
npt_library_load_dxvk_clear_api(struct npt_d3d_library *lib)
{
   if (!lib->d3d11_module)
      return;

   lib->pfn_clear_depth_stencil_rects =
      ((union {
         void *p;
         HRESULT (*f)(void *, void *, uint32_t, float, uint8_t, uint32_t,
                      const int32_t *);
      }){
         .p = npt_library_sym(lib->d3d11_module,
                              "dxvk_d3d11_clear_depth_stencil_rects")
      }).f;
   if (lib->pfn_clear_depth_stencil_rects)
      lib->backend = NPT_BACKEND_DXVK;
}
#endif

#endif /* HAVE_DLFCN_H */

bool
npt_library_init(struct npt_d3d_library *lib)
{
   memset(lib, 0, sizeof(*lib));

#ifdef HAVE_DLFCN_H
#ifndef __APPLE__
   /* Headless operation requires the host D3D11/DXGI library's
    * headless WSI backend. */
   setenv("DXVK_WSI_DRIVER", "Headless", 0);
#endif

   lib->d3d11_module = npt_library_open("NPT_D3D11_LIBRARY_PATH",
                                         NPT_D3D11_LIBRARY_DEFAULT);
   if (lib->d3d11_module) {
      lib->pfn_D3D11CreateDevice =
         ((union { void *p; PFN_D3D11CreateDevice f; }){
            .p = npt_library_sym(lib->d3d11_module, "D3D11CreateDevice")
         }).f;
      if (!lib->pfn_D3D11CreateDevice) {
         npt_log("D3D11 library loaded but D3D11CreateDevice not found");
         dlclose(lib->d3d11_module);
         lib->d3d11_module = NULL;
      } else {
         /* Optional. */
         lib->pfn_D3D11On12CreateDevice =
            ((union { void *p; PFN_D3D11On12CreateDevice f; }){
               .p = npt_library_sym(lib->d3d11_module,
                                    "D3D11On12CreateDevice")
            }).f;
      }
   }

   lib->dxgi_module = npt_library_open("NPT_DXGI_LIBRARY_PATH",
                                        NPT_DXGI_LIBRARY_DEFAULT);
   if (lib->dxgi_module) {
      lib->pfn_CreateDXGIFactory1 =
         ((union { void *p; PFN_CreateDXGIFactory1 f; }){
            .p = npt_library_sym(lib->dxgi_module, "CreateDXGIFactory1")
         }).f;
      if (!lib->pfn_CreateDXGIFactory1) {
         npt_log("DXGI library loaded but CreateDXGIFactory1 not found");
         dlclose(lib->dxgi_module);
         lib->dxgi_module = NULL;
      }
   }

   lib->d3d12_module = npt_library_open("NPT_D3D12_LIBRARY_PATH",
                                         NPT_D3D12_LIBRARY_DEFAULT);
   if (lib->d3d12_module) {
      lib->pfn_D3D12CreateDevice =
         ((union { void *p; PFN_D3D12CreateDevice f; }){
            .p = npt_library_sym(lib->d3d12_module, "D3D12CreateDevice")
         }).f;
      if (!lib->pfn_D3D12CreateDevice) {
         npt_log("D3D12 library loaded but D3D12CreateDevice not found");
         dlclose(lib->d3d12_module);
         lib->d3d12_module = NULL;
      }
   }

   if (lib->d3d11_module) {
      const char *p = getenv("NPT_D3D11_LIBRARY_PATH");
      npt_log("loaded D3D11 library: %s", p ? p : NPT_D3D11_LIBRARY_DEFAULT);
   }
   if (lib->dxgi_module) {
      const char *p = getenv("NPT_DXGI_LIBRARY_PATH");
      npt_log("loaded DXGI library: %s", p ? p : NPT_DXGI_LIBRARY_DEFAULT);
   }
   if (lib->d3d12_module) {
      const char *p = getenv("NPT_D3D12_LIBRARY_PATH");
      npt_log("loaded D3D12 library: %s", p ? p : NPT_D3D12_LIBRARY_DEFAULT);
   }

#ifdef __linux__
   npt_library_load_dxvk_clear_api(lib);
#endif

#ifdef __APPLE__
   npt_library_load_event_api(lib);
   npt_library_load_dxmt_clear_api(lib);

   /* D3DMetal's DXBC->AIR/DXIL shader converter has several defects the guest
    * driver (Triton) patches around. Advertise exactly those patches -- each a
    * specific ISGN/OSGN edit -- so the guest applies them only against
    * D3DMetal; DXMT reports none. */
   if (lib->backend == NPT_BACKEND_D3DMETAL && lib->d3d11_module)
      lib->workaround_flags =
         NPT_WA_WIDEN_SCALAR_VS_INPUT_MASK |
         NPT_WA_TYPE_VS_INPUT_FROM_VERTEX_FORMAT |
         NPT_WA_LINEARIZE_NOPERSPECTIVE_PS_INPUT |
         NPT_WA_SYNTHESIZE_IO_SIGNATURE_FROM_SHDR;
#endif

   /* Debug/test override: NPT_WA_FLAGS forces the workaround-flags set (hex or
    * decimal), e.g. NPT_WA_FLAGS=0 disables every host workaround so the raw
    * backend behavior (and the gates) can be exercised. */
   {
      const char *ov = getenv("NPT_WA_FLAGS");
      if (ov) {
         lib->workaround_flags = (uint32_t)strtoul(ov, NULL, 0);
         npt_log("NPT_WA_FLAGS override -> workaround_flags=0x%08x",
                 lib->workaround_flags);
      }
   }
#else
   npt_log("D3D library loading: dlopen not available");
#endif

   return true;
}

void
npt_library_fini(struct npt_d3d_library *lib)
{
#ifdef HAVE_DLFCN_H
   if (lib->d3d11_module)
      dlclose(lib->d3d11_module);
   if (lib->dxgi_module)
      dlclose(lib->dxgi_module);
   if (lib->d3d12_module)
      dlclose(lib->d3d12_module);
#endif

   memset(lib, 0, sizeof(*lib));
}
