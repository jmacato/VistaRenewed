#!/usr/bin/env python3
"""Exercise the production GTK cursor callback and the old channel-swap bug."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from harness import extract_section, run_harness

root = Path(__file__).resolve().parents[2]
fixture = Path(__file__).with_suffix('.c')
parser = argparse.ArgumentParser()
parser.add_argument('--negative-control', action='store_true')
args = parser.parse_args()
start = 'static void gd_cursor_define(DisplayChangeListener *dcl,'
end = '\nstatic void gd_switch('
code = extract_section(root / 'triton-qemu/ui/gtk.c', start, end)
run_harness(fixture, code)
if args.negative_control:
    # Reintroduce the old little-endian BGRA-as-RGBA interpretation. Keep
    # this independent of Git HEAD so the control survives committing the fix.
    red = 'row[x * 4 + 0] = (pixel >> 16) & 0xff;'
    blue = 'row[x * 4 + 2] = pixel & 0xff;'
    assert code.count(red) == 1 and code.count(blue) == 1
    old = code.replace(red, 'row[x * 4 + 0] = pixel & 0xff;')
    old = old.replace(blue, 'row[x * 4 + 2] = (pixel >> 16) & 0xff;')
    with tempfile.TemporaryDirectory(prefix='cursor-rgba-control-') as tmp:
        source, binary = Path(tmp) / 'control.c', Path(tmp) / 'control'
        source.write_text(fixture.read_text().replace('/* SOURCE_UNDER_TEST */', old))
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], cwd=tmp, capture_output=True, text=True)
        assert result.returncode != 0 and 'memcmp(p->pixels' in result.stderr, result
    print('BGRA-as-RGBA negative control rejected for incorrect color bytes')
print('Cursor RGBA regression verified')
