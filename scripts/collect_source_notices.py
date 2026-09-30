#!/usr/bin/env python3
"""Collect existing guest-component notices without assigning licenses or owners."""
import argparse
import os
from collections import defaultdict
from pathlib import Path
import re

ROOTS = ('triton-kmd', 'triton-umd', 'packaging', 'tests/vista')
SUFFIXES = {'.c', '.h', '.cpp', '.hpp', '.cc', '.cxx', '.hxx', '.inl', '.inc',
            '.hlsl', '.rc', '.rs', '.py', '.sh', '.build', '.txt', '.md'}
# Preserve entire comment blocks, including continuation lines and license text.
COMMENTS = re.compile(r'/\*.*?\*/|(?:(?:^[ \t]*//[^\n]*(?:\n|$))+)|'
                      r'(?:(?:^[ \t]*\#[^\n]*(?:\n|$))+)', re.M | re.S)
MARKER = re.compile(r'copyright|SPDX-License-Identifier:', re.I)


def notices(data):
    if data.startswith((b'\xff\xfe', b'\xfe\xff')):
        text = data.decode('utf-16')
    else:
        text = data.decode('utf-8-sig', errors='replace')
    return tuple(dict.fromkeys(m.group().rstrip() for m in COMMENTS.finditer(text)
                               if MARKER.search(m.group())))


def source_files(root):
    for name in ROOTS:
        directory = root / name
        if not directory.is_dir():
            raise ValueError(f'Missing source notice root: {name}')
        for current, directories, filenames in os.walk(directory, followlinks=False):
            current = Path(current)
            kept = []
            for child in sorted(directories):
                candidate = current / child
                # Named output directories used by public build scripts, plus
                # any custom Meson output identified by its configuration marker.
                # build-support and triton-kmd/build are real source directories.
                generated = (child == 'build-linux' or
                             child.startswith('build-vista-linux-') or
                             (candidate / 'meson-private/coredata.dat').is_file())
                if (not child.startswith('.') and
                        child not in {'__pycache__', 'node_modules'} and
                        not generated and not candidate.is_symlink()):
                    kept.append(child)
            directories[:] = kept
            for filename in sorted(filenames):
                path = current / filename
                if (not filename.startswith('.') and not path.is_symlink() and
                        path.suffix.lower() in SUFFIXES):
                    yield path


def collect(root):
    groups = defaultdict(list)
    for path in source_files(root):
        for notice in notices(path.read_bytes()):
            groups[notice].append(path.relative_to(root).as_posix())
    if not groups:
        raise ValueError('No source notices found; refusing an empty attribution file')
    header = '''Supplementary guest source notices

These are existing copyright and SPDX comment blocks retained verbatim from
this source tree. The scope conservatively includes the guest KMD/UMD source
components, packaging and guest test sources, including code not compiled into
this particular package. This is not an exact binary dependency inventory.
Full component license texts accompany this file. A missing notice does not
supply a license grant, prove public-domain status or resolve authorship.
No new copyright ownership or licensing grant is asserted by this collection.
Known build output directories, configured Meson build directories and external
SDK/WDK inputs are excluded. Unmarked custom build output may require cleanup.

'''
    entries = []
    for notice, paths in sorted(groups.items(), key=lambda item: (item[1][0], item[0])):
        entries.append('Source paths:\n' + ''.join(f'  {p}\n' for p in sorted(paths)) +
                       '\n' + notice + '\n')
    return header + '\n' + ('\n' + '=' * 72 + '\n\n').join(entries)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.write_text(collect(args.root), encoding='utf-8')


if __name__ == '__main__':
    main()
