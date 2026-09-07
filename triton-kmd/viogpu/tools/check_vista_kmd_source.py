#!/usr/bin/env python3
"""Check the source-level invariants for the Vista WDDM 1.0 KMD.

This check is not a replacement for a Vista WDK build.  It catches edits that
would make the Vista configuration select WDDM 1.3 callbacks or unavailable
pool APIs before the Windows build host can report a clearer error.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


MODERN_CALLBACKS = (
    "DxgkDdiQueryVidPnHWCapability",
    "DxgkDdiSystemDisplayEnable",
    "DxgkDdiSystemDisplayWrite",
    "DxgkDdiStopDeviceAndReleasePostDisplayOwnership",
    "DxgkDdiCancelCommand",
    "DxgkDdiQueryEngineStatus",
    "DxgkDdiResetEngine",
    "DxgkDdiGetNodeMetadata",
)

BANNED_VISTA_POOL_APIS = re.compile(
    r"\b(?:NonPagedPoolN[xX]|POOL_NX_ALLOCATION|ExAllocatePoolUninitialized|"
    r"ExAllocatePool[23]|ExFreePool2)\b"
)


def read(path: Path, errors: list[str]) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        errors.append(f"{path}: {exc}")
        return ""


def non_vista_blocks(text: str) -> tuple[str, ...]:
    return tuple(
        match.group("body")
        for match in re.finditer(
            r"^\s*#if\s+!defined\(VIOGPU_TARGET_VISTA\)\s*$"
            r"(?P<body>.*?)^\s*#endif\b",
            text,
            flags=re.MULTILINE | re.DOTALL,
        )
    )


def function_body(text: str, signature: str) -> str:
    """Return one C/C++ function body, including its outer braces."""
    start = text.find(signature)
    if start < 0:
        return ""
    brace = text.find("{", start + len(signature))
    if brace < 0:
        return ""

    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : index + 1]
    return ""


def audit(root: Path) -> list[str]:
    errors: list[str] = []
    driver = read(root / "viogpu3d" / "driver.cpp", errors)
    driver_header = read(root / "viogpu3d" / "driver.h", errors)
    helper = read(root / "common" / "helper.h", errors)
    adapter_header = read(root / "viogpu3d" / "viogpu_adapter.h", errors)
    adapter = read(root / "viogpu3d" / "viogpu_adapter.cpp", errors)
    device = read(root / "viogpu3d" / "viogpu_device.cpp", errors)
    command = read(root / "viogpu3d" / "viogpu_command.cpp", errors)
    command_header = read(root / "viogpu3d" / "viogpu_command.h", errors)
    vidpn = read(root / "viogpu3d" / "viogpu_vidpn.cpp", errors)
    protocol = read(root / "shared" / "viogpum.h", errors)
    virtio_protocol = read(root / "common" / "viogpu.h", errors)
    queue = read(root / "common" / "viogpu_queue.cpp", errors)
    allocation = read(root / "viogpu3d" / "viogpu_allocation.cpp", errors)

    required_driver_text = (
        "#if defined(VIOGPU_TARGET_VISTA)",
        "InitialData.Version = DXGKDDI_INTERFACE_VERSION_VISTA_SP1;",
        "InitialData.DxgkDdiNotifyAcpiEvent = VioGpu3DNotifyAcpiEvent;",
        "InitialData.DxgkDdiQueryInterface = VioGpu3DQueryInterface;",
        "InitialData.DxgkDdiControlEtwLogging = VioGpu3DControlEtwLogging;",
        "InitialData.DxgkDdiSetDisplayPrivateDriverFormat = VioGpu3DSetDisplayPrivateDriverFormat;",
        "InitialData.DxgkDdiAcquireSwizzlingRange = VioGpu3DAcquireSwizzlingRange;",
        "InitialData.DxgkDdiReleaseSwizzlingRange = VioGpu3DReleaseSwizzlingRange;",
        "InitialData.DxgkDdiSetPalette = VioGpu3DSetPalette;",
        "InitialData.DxgkDdiStopCapture = VioGpu3DStopCapture;",
        "InitialData.DxgkDdiCreateOverlay = VioGpu3DCreateOverlay;",
        "InitialData.DxgkDdiUpdateOverlay = VioGpu3DUpdateOverlay;",
        "InitialData.DxgkDdiFlipOverlay = VioGpu3DFlipOverlay;",
        "InitialData.DxgkDdiDestroyOverlay = VioGpu3DDestroyOverlay;",
        "pCreateDevice->pInfo = NULL;",
        "NTSTATUS Status = DxgkInitialize(",
    )
    for required in required_driver_text:
        if required not in driver:
            errors.append(f"driver.cpp is missing: {required}")

    if "pDriverCaps->SchedulingCaps.MultiEngineAware = 1;" not in adapter:
        errors.append(
            "viogpu_adapter.cpp must enable Vista's context-aware scheduling path"
        )

    if "pSegmentDesc[0].Flags.CpuVisible = TRUE;" not in adapter:
        errors.append(
            "viogpu_adapter.cpp must mark Vista's allocation aperture CPU-visible"
        )

    callback_pattern = re.compile(
        r"NTSTATUS\s+APIENTRY\s+VioGpu3DSetVidPnSourceAddress\s*\(",
        flags=re.MULTILINE,
    )
    if not callback_pattern.search(driver):
        errors.append(
            "driver.cpp does not declare VioGpu3DSetVidPnSourceAddress with APIENTRY"
        )
    if not callback_pattern.search(driver_header):
        errors.append(
            "driver.h does not declare VioGpu3DSetVidPnSourceAddress with APIENTRY"
        )

    guarded = non_vista_blocks(driver)
    for callback in MODERN_CALLBACKS:
        if not any(callback in block for block in guarded):
            errors.append(f"{callback} is not guarded from the Vista table")

    required_helper_text = (
        "#define VIOGPU_NONPAGED_POOL              NonPagedPool",
        "#define VIOGPU_NPAGED_LOOKASIDE_POOL      NonPagedPool",
        "return ExAllocatePoolWithTag(poolType, size, tag);",
    )
    for required in required_helper_text:
        if required not in helper:
            errors.append(f"helper.h is missing Vista pool compatibility: {required}")

    required_segment_text = (
        "#define VIOGPU_QUERY_SEGMENT_TYPE DXGKQAITYPE_QUERYSEGMENT",
        "typedef DXGK_QUERYSEGMENTOUT VIOGPU_QUERYSEGMENTOUT;",
        "typedef DXGK_SEGMENTDESCRIPTOR VIOGPU_SEGMENTDESCRIPTOR;",
    )
    for required in required_segment_text:
        if required not in adapter_header:
            errors.append(f"viogpu_adapter.h is missing Vista segment compatibility: {required}")

    adapter = read(root / "viogpu3d" / "viogpu_adapter.cpp", errors)
    for required in (
        "m_LastCompletedFenceId = 0;",
        "m_LastSubmittedFenceId = 0;",
        "m_supportedCapsetIDs = 0;",
        "m_u64HostFeatures = 0;",
        "m_u64GuestFeatures = 0;",
    ):
        if required not in adapter:
            errors.append(f"viogpu_adapter.cpp leaves scheduler or capability state uninitialized: {required}")

    required_protocol_text = (
        "#define VIOGPU_FEATURE_RENDER_EVENT   (1ull << 0)",
        "#define VIOGPU_CMD_SIGNAL_EVENT        0x6",
        "typedef struct _VIOGPU_SIGNAL_EVENT_CMD",
        "static_assert(sizeof(VIOGPU_SIGNAL_EVENT_CMD) == 8",
        "static_assert(offsetof(VIOGPU_SIGNAL_EVENT_CMD, Event) == 0",
    )
    for required in required_protocol_text:
        if required not in protocol:
            errors.append(f"viogpum.h is missing render-event ABI: {required}")

    required_render_text = (
        "case VIOGPU_CMD_SIGNAL_EVENT:",
        "inputHeader.size != sizeof(VIOGPU_SIGNAL_EVENT_CMD)",
        "VioGpuUmHandleValue(signal.Event)",
        "SYNCHRONIZE | EVENT_MODIFY_STATE",
        "UserMode",
        "cmd->SetRenderEvent(renderEvent);",
        "renderStatus = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;",
        "return renderStatus;",
    )
    for required in required_render_text:
        if required not in device:
            errors.append(f"viogpu_device.cpp is missing render-event validation: {required}")

    if re.search(
        r"pRender->MultipassOffset\s*=\s*"
        r"\(UINT\)\(cmdBuf\s*-\s*commandBase\);\s*"
        r"return\s+STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;",
        device,
    ):
        errors.append(
            "viogpu_device.cpp returns before it creates a command for a copied multipass prefix"
        )

    required_retirement_text = (
        "case VIOGPU_CMD_SIGNAL_EVENT:",
        "SignalRenderEvent();",
        "ObDereferenceObject(event);",
        "void VioGpuCommander::CancelRenderEvents()",
    )
    for required in required_retirement_text:
        if required not in command:
            errors.append(f"viogpu_command.cpp is missing render-event retirement: {required}")

    required_submit_ownership_text = (
        "m_pExpectedDevice",
        "BOOLEAN BelongsToDevice(const VioGpuDevice *device) const",
        "BOOLEAN HasDmaBuffer() const",
        "VioGpuDevice *expectedDevice",
    )
    for required in required_submit_ownership_text:
        if required not in command_header:
            errors.append(f"viogpu_command.h is missing submit ownership: {required}")

    required_submit_validation_text = (
        "expectedDevice != m_pExpectedDevice",
        "!cmd->BelongsToDevice(submitDevice)",
        "!cmd->HasDmaBuffer()",
        "cmd->PrepareSubmit(pSubmitCommand, submitDevice);",
    )
    for required in required_submit_validation_text:
        if required not in command:
            errors.append(f"viogpu_command.cpp is missing submit validation: {required}")

    required_vidpn_text = (
        "pCommitVidPn->AffectedVidPnSourceId != D3DDDI_ID_ALL",
        "pCommitVidPn->AffectedVidPnSourceId == D3DDDI_ID_ALL",
        "VirtIO scanout has no",
        "physical video-output codec",
    )
    for required in required_vidpn_text:
        if required not in vidpn:
            errors.append(f"viogpu_vidpn.cpp is missing Vista VidPn handling: {required}")

    if "#define VIRTIO_GPU_FLAG_INFO_RING_IDX (1 << 1)" not in virtio_protocol:
        errors.append("viogpu.h is missing the standard context-ring fence flag")

    required_backing_text = (
        "UINT entryCount = 0;",
        "PFN_NUMBER *pfns = MmGetMdlPfnArray(pMDL);",
        "ents[entryCount - 1].length <= MAXULONG - PAGE_SIZE",
        "ents[entryCount - 1].length += PAGE_SIZE;",
        "m_adapter->ctrlQueue.AttachBacking(m_Id, ents, entryCount);",
    )
    for required in required_backing_text:
        if required not in allocation:
            errors.append(
                "viogpu_allocation.cpp is missing backing PFN coalescing: "
                f"{required}"
            )

    for function in (
        "CtrlQueue::CreateResourceBlob(",
        "CtrlQueue::CtxResource(",
        "CtrlQueue::SubmitCommand(",
        "CtrlQueue::TransferHostCmd(",
        "CtrlQueue::ResourceMapBlob(",
        "CtrlQueue::ResourceUnmapBlob(",
    ):
        body = function_body(queue, function)
        if not body:
            errors.append(f"viogpu_queue.cpp is missing function: {function}")
            continue
        if "VIRTIO_GPU_FLAG_FENCE" not in body or \
           "VIRTIO_GPU_FLAG_INFO_RING_IDX" not in body:
            errors.append(
                f"{function} must retire through its renderer-context timeline"
            )

    destroy_context = function_body(queue, "CtrlQueue::DestroyCtx(")
    if not destroy_context:
        errors.append("viogpu_queue.cpp is missing CtrlQueue::DestroyCtx")
    elif "VIRTIO_GPU_FLAG_FENCE" in destroy_context:
        errors.append(
            "CtrlQueue::DestroyCtx must use its synchronous control response, "
            "not create a fence on the destroyed context"
        )

    destroy_resource = function_body(queue, "CtrlQueue::DestroyResource(")
    if not destroy_resource:
        errors.append("viogpu_queue.cpp is missing CtrlQueue::DestroyResource")
    elif "VIRTIO_GPU_FLAG_FENCE" in destroy_resource:
        errors.append(
            "CtrlQueue::DestroyResource must use its synchronous control "
            "response, not the unrelated GL context-zero fence"
        )

    if "pfnAssignMultisamplingMethodSet(" in vidpn:
        errors.append(
            "viogpu_vidpn.cpp must not advertise output-codec multisampling for VirtIO scanout"
        )

    if re.search(
        r"pCommitVidPn\s*==\s*NULL\s*\)\s*\|\|\s*"
        r"\(pCommitVidPn->AffectedVidPnSourceId\s*>=\s*MAX_VIEWS",
        vidpn,
    ):
        errors.append("viogpu_vidpn.cpp rejects the valid D3DDDI_ID_ALL commit source")

    for directory in (root / "common", root / "viogpu3d"):
        for path in sorted(directory.rglob("*")):
            if path.suffix not in {".c", ".cc", ".cpp", ".h", ".hpp"}:
                continue
            if path == root / "common" / "helper.h":
                continue
            source = read(path, errors)
            match = BANNED_VISTA_POOL_APIS.search(source)
            if match:
                errors.append(f"{path}: unavailable Vista pool API {match.group(0)}")
            if re.search(r"\bnew\s*\(\s*NonPagedPool", source):
                errors.append(f"{path}: direct NonPagedPool placement new bypasses compatibility macro")
            if path.name != "trace.h" and re.search(r"\bDbgBreakPoint\s*\(", source):
                errors.append(
                    f"{path}: direct DbgBreakPoint bypasses the Vista diagnostic no-break policy"
                )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="path to the viogpu source root",
    )
    args = parser.parse_args()
    errors = audit(args.source_root)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(f"{args.source_root}: Vista KMD source audit passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
