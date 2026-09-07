#!/usr/bin/env python3
"""Check the Triton host fence and blob-map source contract."""

from __future__ import annotations

import sys
from pathlib import Path


WORKSPACE = Path(__file__).resolve().parents[1]


def read(relative: str, errors: list[str]) -> str:
    file = WORKSPACE / relative
    try:
        return file.read_text(encoding="utf-8")
    except OSError as error:
        errors.append(f"{relative}: {error}")
        return ""


def require(text: str, file: str, values: tuple[str, ...], errors: list[str]) -> None:
    for value in values:
        if value not in text:
            errors.append(f"{file} is missing: {value}")


def main() -> int:
    errors: list[str] = []
    qemu_file = "triton-qemu/hw/display/virtio-gpu-virgl.c"
    qemu_header_file = "triton-qemu/include/hw/virtio/virtio-gpu.h"
    qemu_queue_file = "triton-qemu/hw/display/virtio-gpu.c"
    renderer_file = "triton-virglrenderer/src/virglrenderer.c"
    renderer_header_file = "triton-virglrenderer/src/virglrenderer.h"
    neptune_file = "triton-virglrenderer/src/neptune/npt_context.c"
    proxy_file = "triton-virglrenderer/src/proxy/proxy_context.c"

    qemu = read(qemu_file, errors)
    qemu_header = read(qemu_header_file, errors)
    qemu_queue = read(qemu_queue_file, errors)
    renderer = read(renderer_file, errors)
    renderer_header = read(renderer_header_file, errors)
    neptune = read(neptune_file, errors)
    proxy = read(proxy_file, errors)

    require(
        renderer_header,
        renderer_header_file,
        ("virgl_renderer_context_get_capset_id",),
        errors,
    )
    require(
        renderer,
        renderer_file,
        (
            "int virgl_renderer_context_get_capset_id(",
            "struct virgl_context *ctx = virgl_context_lookup(ctx_id);",
            "*capset_id = ctx->capset_id;",
        ),
        errors,
    )
    require(
        qemu_header,
        qemu_header_file,
        (
            "bool context_fence;",
            "bool reset_pending;",
        ),
        errors,
    )
    require(
        qemu_queue,
        qemu_queue_file,
        (
            "cmd->context_fence = false;",
            "cmd->cmd_hdr.type == VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING",
            "max_entries = 32768;",
            "triton-backing-compat legacy-page-list",
            "static void virtio_gpu_reset_bh(VirtIOGPU *g);",
            "if (!g->reset_pending)",
            "g->reset_pending = true;",
            "g->reset_pending = false;",
            "qemu_bh_schedule(g->ctrl_bh);",
        ),
        errors,
    )
    for obsolete_reset_wait in (
        "reset_cond",
        "reset_finished",
        "qemu_cond_wait_bql(&g->reset_cond)",
    ):
        if obsolete_reset_wait in qemu_header or obsolete_reset_wait in qemu_queue:
            errors.append(
                f"virtio-gpu reset still contains the deadlocking wait: "
                f"{obsolete_reset_wait}"
            )
    for bh_name, handler in (
        ("virtio_gpu_ctrl_bh", "vgc->handle_ctrl"),
        ("virtio_gpu_cursor_bh", "virtio_gpu_handle_cursor"),
    ):
        start = qemu_queue.find(f"static void {bh_name}")
        reset = qemu_queue.find("virtio_gpu_reset_bh(g);", start)
        handle = qemu_queue.find(handler, start)
        if start < 0 or reset < start or handle < reset:
            errors.append(
                f"{qemu_queue_file}: {bh_name} does not serialize reset "
                f"before queue handling"
            )
    require(
        qemu,
        qemu_file,
        (
            "virtio_gpu_virgl_legacy_neptune_context_fence(",
            "virgl_renderer_context_get_capset_id(cmd->cmd_hdr.ctx_id,",
            "capset_id == VIRTIO_GPU_CAPSET_NEPTUNE",
            "cmd->context_fence =",
            "cmd->cmd_hdr.type == VIRTIO_GPU_CMD_CTX_DESTROY",
            "virgl_renderer_context_create_fence(cmd->cmd_hdr.ctx_id,",
            "cmd->context_fence) {\n            continue;",
            "cmd->context_fence) &&",
            "triton-attach-backing failed",
            "triton-attach-backing map failed",
            "triton-map-blob map-info failed",
            "triton-map-blob map failed",
            "triton-unmap-blob already-unmapped",
            '"blob=%u mem=%u flags=0x%x ctx=%u "',
            "if (virtio_gpu_neptune_enabled(g->parent_obj.conf))",
            "cmd->cmd_hdr.type == VIRTIO_GPU_CMD_RESOURCE_UNREF",
            "triton-fence-route generic",
        ),
        errors,
    )
    require(
        neptune,
        neptune_file,
        (
            "blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE",
            "VIRGL_RENDERER_MAP_CACHE_WC",
            "VIRGL_RENDERER_MAP_CACHE_NONE",
        ),
        errors,
    )
    require(
        proxy,
        proxy_file,
        (
            "failed to receive submit_fence reply",
            "return -EIO;",
            "if (!reply.ok)",
            "render server rejected submit_fence",
        ),
        errors,
    )

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("Triton host fence, blob-map, and deferred-reset source audit passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
