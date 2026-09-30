#!/usr/bin/env python3
"""Compile the production recorder and independently verify its exported bytes."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zlib

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools'))
import triton_trace

code = (root / 'triton-qemu/ui/triton-trace.c').read_text()
code = re.sub(r'^#include .*\n', '', code, flags=re.M)
fixture = Path(__file__).with_suffix('.c').read_text()
flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0', 'zlib'], text=True).split()
with tempfile.TemporaryDirectory(prefix='host-trace-') as tmp:
    directory = Path(tmp)
    source, binary = directory / 'trace.c', directory / 'trace'
    source.write_text(fixture.replace('/* SOURCE_UNDER_TEST */', code))
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter',
                    '-I' + str(root / 'triton-qemu/include'), str(source),
                    '-o', str(binary), *flags], check=True)
    subprocess.run([str(binary), str(directory)], check=True)
    data = (directory / 'capture.bin').read_bytes()
    trace = triton_trace.decode(data, 'host')
    assert trace.run == 0x0102030405060a0d and len(trace.events) == 7
    assert data[16:24] == bytes.fromhex('0d0a060504030201')
    assert data[128 + 40:128 + 48] == bytes.fromhex('0807060504030201')
    assert trace.events[0]['command'] == 0x0d0a
    assert trace.events[0]['context'] == 0x10203040
    for event, cookie, context, resource, console in zip(
        trace.events[3:], [11, 11, 12, 12], [101, 101, 102, 102],
        [201, 201, 202, 202], [1, 1, 2, 2],
    ):
        assert (event['command'], event['context'], event['arg0'], event['arg1']) == (cookie, context, resource, console)
    assert (directory / 'retry.bin').read_bytes() == data
    assert (directory / 'after-errors.bin').read_bytes() == data
    assert (directory / 'existing.bin').read_bytes() == b'preserve'
    assert (directory / 'capture.bin').stat().st_mode & 0o777 == 0o600
    full = bytearray((directory / 'full.bin').read_bytes())
    assert len(full) == 128 + 131072 * 80
    assert struct.unpack_from('<ii', full, 36) == (131072, 2147483647)
    checksum = struct.unpack_from('<Q', full, 80)[0]
    full[80:88] = bytes(8)
    assert zlib.crc32(full) == checksum

# Lifecycle hooks must invalidate before frontend callbacks/freeing the console.
console = (root / 'triton-qemu/ui/console.c').read_text()
for begin, end in [
    ('qemu_console_finalize(Object *obj)', 'qemu_console_class_init('),
    ('void dpy_gfx_replace_surface(', 'bool dpy_gfx_check_format('),
    ('void dpy_gl_scanout_disable(', 'void dpy_gl_scanout_texture('),
]:
    section = console[console.index(begin):console.index(end, console.index(begin))]
    assert 'triton_trace_invalidate(' in section
print('HOST TRACE WIRE, ERRORS, CAPACITY AND DISPLAY IDENTITY PASS')
