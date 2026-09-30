# Presentation optimization research

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


This note records observed code paths and proposals only.  It does not claim
that any of the proposals below is implemented.

## Observed path

Neptune HOST3D blobs use a CPU presentation path in
`triton-qemu/hw/display/virtio-gpu-virgl.c`:

* `virtio_gpu_neptune_readback_blob()` exports a blob FD, maps it, surrounds
  CPU reads with `DMA_BUF_IOCTL_SYNC`, copies rows into a `DisplaySurface`,
  then unmaps and closes the FD.
* `virtio_gpu_neptune_present_blt()` first snapshots the blob through that
  path, then writes it into the ordinary primary with
  `virgl_renderer_transfer_write_iov()`.
* `virgl_cmd_resource_flush()` clips the flush rectangle to the scanout and
  publishes only that rectangle with `dpy_gfx_update()`.

The tree already has a separate QEMU DMA-BUF scanout implementation for
ordinary resources: `hw/display/virtio-gpu-udmabuf.c` creates/reuses
`QemuDmaBuf` objects and calls `dpy_gl_scanout_dmabuf()`, while
`ui/egl-helpers.c` imports them through `EGL_LINUX_DMA_BUF_EXT` and can create
native-fence FDs.  This is useful precedent, not evidence that Neptune blobs
currently use it.

`triton9_output.c` contains rectangle-aware `triton9Blt()` and
`triton9ColorFill()` operations, but no D3D9 Present callback.  The current
Present implementation is `triton9_resource.c::triton9Present()`: it waits
for GPU completion and drains the transport before its Present callback.

## 1. Direct DMA-BUF scanout for eligible HOST3D blobs

**Proposal.** Extend the existing `QemuDmaBuf` path to represent a
renderer-exported blob and select it from
`virgl_cmd_set_scanout_blob()` when Neptune is enabled and the active display
frontend supports DMA-BUF import.  Cache the export/EGL import by resource and
layout identity.  This removes the export/map/CPU-copy/unmap cycle in
`virtio_gpu_neptune_readback_blob()` and can remove the source-to-primary
copy, but the latter requires a KMD/presentation-contract redesign: the
current protocol deliberately copies the blob into an ordinary standard
primary before it is flushed.

**Prerequisites and correctness.**

* Preserve the standard-primary/DWM semantics before bypassing
  `virtio_gpu_neptune_present_blt()`; a direct scanout source cannot change
  what Present exposes to the display stack.
* Carry and validate all import metadata: plane count, DRM fourcc, modifier,
  stride, byte offset, dimensions, and Y orientation.  Use checked 64-bit
  arithmetic for `offset + (height - 1) * stride + row_bytes`.
* Add a plane-offset field to `QemuDmaBuf`/EGL import first: the current import
  passes `EGL_DMA_BUF_PLANE0_OFFSET_EXT = 0`, which is insufficient for a blob
  view with a nonzero offset.
* Keep the FD and imported EGL object alive until every display consumer has
  released it; release them on resource unref, scanout replacement, reset, and
  frontend teardown.  Fall back to the current CPU path for unsupported EGL,
  formats, modifiers, or frontend capabilities.

**Measurement.** Count CPU bytes copied, blob-export/map/unmap calls, EGL
imports, and frame time from the final guest submission through presentation.
For a stable same-layout flip chain, the target is zero CPU pixel-copy bytes
and no repeated EGL import for an unchanged buffer.  Validate pixels, crop,
stride, and nonzero-offset cases against the CPU fallback.

Primary references: [QEMU virtio-gpu documentation](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html),
[EGL_EXT_image_dma_buf_import](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt),
and [EGL_EXT_image_dma_buf_import_modifiers](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import_modifiers.txt).

## 2. Make direct scanout synchronization explicit

**Proposal.** Once direct blob scanout exists, propagate an acquire fence from
the producer to the display consumer and return/observe a release fence before
that flip-chain buffer is reused.  Reuse the QEMU EGL native-fence support in
`egl_dmabuf_create_sync()` and `egl_dmabuf_create_fence()` rather than using a
CPU wait as the steady-state display synchronization mechanism.

**Prerequisites and correctness.**

* Do not treat `DMA_BUF_IOCTL_SYNC` as GPU ordering.  It brackets CPU mapping
  for cache coherency; it does not exclude concurrent device access.  The
  current CPU path remains valid only because Present already waits for the
  producer before the CPU read.
* Define the protocol ownership and lifetime of each `sync_file`: display must
  wait for the acquire fence before sampling; the producer must wait for the
  release fence before rendering into that buffer again.  Close every FD on
  success, error, reset, and replacement.
* Do not wait while holding the UMD shader lock or KMD transport locks.  Keep
  the current Present completion path as the fallback until early reuse,
  device loss, and reset have been exercised.

**Measurement.** Record acquire-to-sample latency, release-to-buffer-reuse
latency, blocked UI time, and max frames in flight.  Stress a multi-buffer
flip chain, resize/reset, and rapid cursor motion; pass only with no stale or
torn frames and no leaked fence FDs.

Primary references: [Linux DMA-BUF synchronization](https://docs.kernel.org/driver-api/dma-buf.html),
[Linux sync-file API](https://docs.kernel.org/driver-api/sync_file.html), and
[EGL_ANDROID_native_fence_sync](https://registry.khronos.org/EGL/extensions/ANDROID/EGL_ANDROID_native_fence_sync.txt).

## 3. Preserve damage and use the independent cursor queue

**Proposal.** Keep exact destination damage through a DWM present group and
publish it in the final primary flush; avoid inflating it to a full frame.
Where the KMD/display path permits it, send pointer shape changes and movement
through virtio-gpu `UPDATE_CURSOR`/`MOVE_CURSOR` rather than repainting the
cursor into the primary.  QEMU already consumes these commands independently
in `hw/display/virtio-gpu.c`.

**Prerequisites and correctness.**

* The damage set must contain every changed pixel since the last published
  image.  Clip it to the scanout and use a full-frame fallback for unknown or
  unsupported writes; under-reporting damage corrupts output.
* Preserve the ordering of destination writes, final flush, and presentation
  fence.  Merge only rectangles that are complete before that publication.
* For cursors, preserve ARGB pixels, hotspot, visibility, scanout-relative
  coordinates, and multi-output routing.  Retain composited-primary fallback
  when the cursor queue or frontend cannot represent the required cursor.

**Measurement.** Track damage area/frame, number of flush rectangles, primary
bytes copied, and cursor commands versus full-primary updates.  Validate
disjoint and overlapping rectangles, clipping, cursor hotspot edges,
multi-output movement, and cursor changes during Present.

Primary references: [Linux DRM/KMS damage and plane documentation](https://docs.kernel.org/gpu/drm-kms.html)
and [virtio-gpu cursor commands in the Linux UAPI](https://github.com/torvalds/linux/blob/master/include/uapi/linux/virtio_gpu.h).
