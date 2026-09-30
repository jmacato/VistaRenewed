#!/usr/bin/env python3
"""Compile the blob harness against the helper extracted from QEMU source."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = root / "triton-qemu/hw/display/virtio-gpu-virgl.c"
harness = Path(__file__).with_suffix(".c")
text = source.read_text()
start = text.index("static int virtio_gpu_neptune_readback_blob(")
end = text.index("static int virtio_gpu_neptune_copy_resource(", start)
helper = text[start:end].rstrip()

test = harness.read_text()
begin = "/* BEGIN_EXTRACTED_HELPER: rewritten by test-blob-readback.py before compile. */"
end_marker = "/* END_EXTRACTED_HELPER */"
head, remainder = test.split(begin, 1)
_, tail = remainder.split(end_marker, 1)
generated = head + begin + "\n" + helper + "\n" + end_marker + tail

with tempfile.TemporaryDirectory(prefix="triton-blob-readback-") as tmp:
    generated_path = Path(tmp) / "test-blob-readback.c"
    binary = Path(tmp) / "test-blob-readback"
    generated_path.write_text(generated)
    subprocess.run([
        "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
        str(generated_path), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
