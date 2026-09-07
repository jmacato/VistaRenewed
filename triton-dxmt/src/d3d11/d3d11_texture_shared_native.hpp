/*
 * Native (non-Wine) cross-process shared textures over anonymous POSIX shm.
 *
 * On Windows/Wine the MISC_SHARED path rides D3DKMT + mach-port shared
 * IOSurfaces; neither exists in a native macOS build.  Here a shared
 * texture is instead one linear plane of anonymous shared memory:
 * IDXGIResource::GetSharedHandle returns a pointer to the POD below, whose
 * fd travels to the peer process via SCM_RIGHTS.  A peer may mmap the fd
 * directly, so the texture's pixels MUST live in the shm at the recorded
 * stride — achieved with a linear MTLTexture over a
 * newBufferWithBytesNoCopy buffer wrapping the mapping.
 */

#pragma once

#ifndef _WIN32

#include "../dxmt-native/dxmt_native.h"
#include "d3d11_device.hpp"
#include "dxmt_texture.hpp"

namespace dxmt {

/* The GetSharedHandle/OpenSharedResource wire format is the embedder
 * handle type; alias it so the internal code has one source of truth. */
using shm_texture_pod = dxmt_shared_texture_handle;

static_assert(sizeof(shm_texture_pod) == 64, "wire format");

/*
 * Build a linear shm-backed texture for `finalDesc` (already normalized by
 * CreateMTLTextureDescriptor, `info` from the same call).
 *
 * import_fd < 0: create a fresh anonymous shm region (producer);
 * import_fd >= 0: map the given fd (consumer; the fd is NOT consumed) and
 * use import_stride/import_size verbatim — the byte-exact contract with the
 * producer.
 *
 * On success out_pod is fully filled and owns a NEW fd (close it when the
 * owning resource dies), and out_texture/out_allocation hold the linear
 * texture (not yet renamed in).
 */
HRESULT AllocateShmLinearTexture2D(
    MTLD3D11Device *pDevice, const D3D11_TEXTURE2D_DESC1 &finalDesc, const WMTTextureInfo &info, int import_fd,
    uint64_t import_stride, uint64_t import_size, Rc<Texture> &out_texture, Rc<TextureAllocation> &out_allocation,
    shm_texture_pod &out_pod
);

} // namespace dxmt

#endif /* !_WIN32 */
