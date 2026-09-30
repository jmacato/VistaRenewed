#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Describe local image files needed by a containerized Vista qcow2 overlay."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys


def image_mounts(disk):
    tool = shutil.which('qemu-img')
    if not tool:
        raise ValueError('qemu-img is required to inspect the VM backing chain')
    current = os.path.realpath(disk)
    mounts = []
    seen = set()
    image_format = 'qcow2'
    # Inspect one layer at a time: validate a local backing path before opening
    # it, and retain the path used by QEMU for relative backing-name resolution.
    for depth in range(256):
        canonical = os.path.realpath(current)
        validate_mount_path(current)
        validate_mount_path(canonical)
        if canonical in seen:
            raise ValueError('VM backing chain contains a cycle')
        seen.add(canonical)
        if not Path(current).is_file() or not os.access(current, os.R_OK):
            raise ValueError('VM backing image is missing or unreadable: ' + current)
        command = [tool, 'info', '--output=json']
        if image_format:
            command += ['-f', image_format]
        result = subprocess.run(command + [current], check=True,
                                capture_output=True, text=True, timeout=30)
        info = json.loads(result.stdout)
        mounts.append(canonical + ':' + current + (':ro' if depth else ''))
        # A qcow2 external data file is part of this layer, with matching
        # mutability. QEMU resolves its relative name beside the qcow2 file.
        data = info.get('format-specific', {}).get('data', {})
        external = data.get('data-file')
        if external:
            data_path = local_relative_path(current, external)
            if not Path(data_path).is_file() or not os.access(data_path, os.R_OK):
                raise ValueError('VM external data file is missing: ' + data_path)
            data_source = os.path.realpath(data_path)
            validate_mount_path(data_source)
            mounts.append(data_source + ':' + data_path +
                          (':ro' if depth else ''))
        backing = info.get('backing-filename')
        if not backing:
            return mounts
        current = local_relative_path(current, backing)
        image_format = info.get('backing-filename-format')
    raise ValueError('VM backing chain exceeds 256 layers')


def local_relative_path(image, name):
    if not isinstance(name, str) or not name or any(c in name for c in ':\n\r'):
        raise ValueError('Only local VM backing/data file paths are supported')
    return os.path.normpath(name if os.path.isabs(name) else
                            os.path.join(os.path.dirname(image), name))


def validate_mount_path(path):
    if any(c in path for c in ':\n\r'):
        raise ValueError('VM image paths cannot contain colons or newlines')


if __name__ == '__main__':
    try:
        if len(sys.argv) != 2:
            raise ValueError('Usage: vista_disk_mounts.py OVERLAY.qcow2')
        print(json.dumps(image_mounts(sys.argv[1])))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        detail = getattr(error, 'stderr', None) or str(error)
        print('Cannot prepare VM image mounts: ' + detail.strip(), file=sys.stderr)
        sys.exit(1)
