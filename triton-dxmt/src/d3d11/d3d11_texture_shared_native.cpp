#ifndef _WIN32

#include "d3d11_texture_shared_native.hpp"
#include "d3d11_resource.hpp"
#include "dxmt_format.hpp"

#include <cstdint>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if __has_include(<sys/posix_shm.h>)
#include <sys/posix_shm.h>
#else
/* Only macOS exposes the header; the kernel limit is the same everywhere. */
#define PSHMNAMLEN 31
#endif
#include <unistd.h>

/* An app group prefix must leave room for "/" and one nonce digit within
 * PSHMNAMLEN. */
#define APP_SANDBOX_GROUP_ID_MAX (PSHMNAMLEN - 2)

namespace dxmt {

static uint64_t
page_align(uint64_t n) {
  const uint64_t page = (uint64_t)getpagesize();
  return (n + page - 1) & ~(page - 1);
}

/* Anonymous shm: exclusively-created then immediately unlinked, so the fd is
 * the only name. */
static int
anon_shm_file(uint64_t size) {
  /* Under App Sandbox a shm name must live in the app group container, i.e. be
   * prefixed with the group identifier instead of '/'; anything else is
   * denied.  The prefix leaves no room for the tag or the pid, so a sandboxed
   * name is just as much of the nonce as still fits -- eight hex digits while
   * the prefix is at most 22 chars, down to one at the limit. */
  const char *group = getenv("APP_SANDBOX_GROUP_ID");
  if (group && !group[0])
    group = nullptr;
  if (group && strlen(group) > APP_SANDBOX_GROUP_ID_MAX) {
    ERR("shm texture: APP_SANDBOX_GROUP_ID is ", strlen(group), " chars, at most ", APP_SANDBOX_GROUP_ID_MAX, " fit");
    errno = EINVAL;
    return -1;
  }
  const size_t nonce_room = group ? PSHMNAMLEN - strlen(group) - 1 : 8;
  const unsigned nonce_digits = nonce_room < 8 ? (unsigned)nonce_room : 8;

  for (int attempt = 0; attempt < 64; attempt++) {
    /* macOS PSHMNAMLEN caps shm names at 31 chars. */
    char name[PSHMNAMLEN + 1];
    if (group)
      snprintf(name, sizeof(name), "%s/%0*x", group, (int)nonce_digits, arc4random() >> (32 - 4 * nonce_digits));
    else
      snprintf(name, sizeof(name), "/dxmt-%x-%08x", getpid(), arc4random());
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
      if (errno == EEXIST)
        continue;
      return -1;
    }
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) != 0) {
      close(fd);
      return -1;
    }
    int fdflags = fcntl(fd, F_GETFD);
    if (fdflags >= 0)
      fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
    return fd;
  }
  return -1;
}

HRESULT
AllocateShmLinearTexture2D(
    MTLD3D11Device *pDevice, const D3D11_TEXTURE2D_DESC1 &finalDesc, const WMTTextureInfo &info, int import_fd,
    uint64_t import_stride, uint64_t import_size, Rc<Texture> &out_texture, Rc<TextureAllocation> &out_allocation,
    shm_texture_pod &out_pod
) {
  /* Metal linear (buffer-backed) textures: 2D, single subresource, no
   * compressed/depth formats, no MSAA.  Real shared-texture traffic is
   * BGRA8-family render targets, which all fit. */
  if (finalDesc.MipLevels != 1 || finalDesc.ArraySize != 1 || finalDesc.SampleDesc.Count > 1 ||
      (finalDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)) {
    ERR("shm texture: unsupported layout (mips=", finalDesc.MipLevels, " array=", finalDesc.ArraySize,
        " samples=", finalDesc.SampleDesc.Count, ")");
    return E_FAIL;
  }

  MTL_DXGI_FORMAT_DESC format_desc;
  if (FAILED(MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, format_desc)) ||
      format_desc.PixelFormat == WMTPixelFormatInvalid || format_desc.BytesPerTexel == 0 ||
      (format_desc.Flag & (MTL_DXGI_FORMAT_BC | MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER))) {
    ERR("shm texture: format ", finalDesc.Format, " cannot back a linear texture");
    return E_FAIL;
  }

  uint64_t stride, logical_size;
  if (import_fd >= 0) {
    /* Consumer: the producer's layout is the contract; never recompute.
     * Every descriptor field is untrusted: validate without overflowing
     * (stride * Height can wrap in 64 bits) and within the unsigned range
     * the Texture constructor truncates to. */
    stride = import_stride;
    logical_size = import_size;
    if (!stride || !finalDesc.Height || stride > UINT32_MAX || logical_size > UINT32_MAX ||
        stride < (uint64_t)finalDesc.Width * format_desc.BytesPerTexel ||
        logical_size / stride < finalDesc.Height) {
      ERR("shm texture: import descriptor stride/size inconsistent");
      return E_INVALIDARG;
    }
  } else {
    uint32_t bytes_per_row, bytes_per_image, bytes_per_slice;
    if (FAILED(GetLinearTextureLayout(pDevice, finalDesc, 0, bytes_per_row, bytes_per_image, bytes_per_slice, true))) {
      ERR("shm texture: no linear layout for format ", finalDesc.Format);
      return E_FAIL;
    }
    stride = bytes_per_row;
    logical_size = bytes_per_image;
  }

  const uint64_t mapped_size = page_align(logical_size);

  int fd = import_fd >= 0 ? import_fd : anon_shm_file(mapped_size);
  if (fd < 0) {
    ERR("shm texture: shm_open failed: ", errno);
    return E_FAIL;
  }

  if (import_fd >= 0) {
    /* Accessing pages past the shm object's actual size raises SIGBUS, so
     * the mapping must be backed in full. */
    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < mapped_size) {
      ERR("shm texture: shm object smaller than descriptor size");
      return E_INVALIDARG;
    }
  }

  void *mapped = mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    ERR("shm texture: mmap failed: ", errno);
    if (import_fd < 0)
      close(fd);
    return E_FAIL;
  }

  /* Buffer over the mapping; its deallocator munmaps, so the memory lives
   * exactly as long as the last Metal reference (in-flight command buffers
   * included). */
  WMTBufferInfo buffer_info;
  buffer_info.length = mapped_size;
  buffer_info.options = WMTResourceHazardTrackingModeUntracked; /* StorageModeShared */
  buffer_info.memory.set(mapped);
  buffer_info.gpu_address = 0;
  auto buffer = pDevice->GetMTLDevice().newBufferMapped(buffer_info);
  if (!buffer) {
    ERR("shm texture: newBufferWithBytesNoCopy failed");
    munmap(mapped, mapped_size);
    if (import_fd < 0)
      close(fd);
    return E_FAIL;
  }

  WMTTextureInfo linear_info = info;
  linear_info.mach_port = 0;
  linear_info.options = WMTResourceHazardTrackingModeUntracked;

  Flags<TextureAllocationFlag> flags;
  flags.set(TextureAllocationFlag::Shared);
  if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
    flags.set(TextureAllocationFlag::GpuReadonly);
  if (!(finalDesc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)))
    flags.set(TextureAllocationFlag::ShaderReadonly);

  Rc<Texture> texture = new Texture((unsigned)logical_size, (unsigned)stride, linear_info, pDevice->GetMTLDevice());
  Rc<TextureAllocation> allocation = texture->allocateFromBuffer(std::move(buffer), mapped, flags);
  if (!allocation->texture()) {
    ERR("shm texture: linear texture creation failed (stride=", stride, ")");
    /* buffer reference died with `allocation`; its deallocator munmaps. */
    if (import_fd < 0)
      close(fd);
    return E_FAIL;
  }

  /* The POD owns a fresh fd in both directions so an opened texture can be
   * re-exported and outlive the descriptor it was opened from. */
  int pod_fd = import_fd >= 0 ? dup(fd) : fd;
  if (pod_fd < 0) {
    ERR("shm texture: dup failed: ", errno);
    return E_FAIL;
  }

  out_pod.magic = DXMT_SHARED_TEXTURE_MAGIC;
  out_pod.version = DXMT_SHARED_HANDLE_VERSION;
  out_pod.fd = pod_fd;
  out_pod.width = finalDesc.Width;
  out_pod.height = finalDesc.Height;
  out_pod.dxgi_format = finalDesc.Format;
  out_pod.mip_levels = finalDesc.MipLevels;
  out_pod.array_size = finalDesc.ArraySize;
  out_pod.sample_count = finalDesc.SampleDesc.Count;
  out_pod.bind_flags = finalDesc.BindFlags;
  out_pod.misc_flags = finalDesc.MiscFlags;
  out_pod.cpu_access = finalDesc.CPUAccessFlags;
  out_pod.stride = stride;
  out_pod.size = logical_size;

  out_texture = std::move(texture);
  out_allocation = std::move(allocation);
  return S_OK;
}

} // namespace dxmt

#endif /* !_WIN32 */
