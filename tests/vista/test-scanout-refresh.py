"""Exercise production binding/flush commands with pixel-preserving readback."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
text = (root / 'triton-qemu/hw/display/virtio-gpu-virgl.c').read_text()
start = text.index('static void virgl_cmd_resource_flush(')
end = text.index('static void virgl_cmd_set_scanout(', start)
code = text[start:end]
fixture = Path(__file__).with_suffix('.c').read_text()
with tempfile.TemporaryDirectory(prefix='scanout-refresh-') as tmp:
    source = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    source.write_text(fixture.replace('/* SOURCE_UNDER_TEST */', code))
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Both ordinary and blob binding paths must use the refresh-aware helper.
assert text.count('virtio_gpu_neptune_set_scanout(g, ss.scanout_id, &fb, &ss.r)') == 2
print('SCANOUT REBIND AND PARTIAL FLUSH PASS')
