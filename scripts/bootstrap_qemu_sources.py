#!/usr/bin/env python3
"""Restore QEMU's four pinned C build dependencies and Meson overlays.

Uses the checked-in .wrap revisions. Existing sources must match either the
pristine pin or that pin with its exact packagefiles overlay. No user files,
including ignored files, are discarded. No reset, clean, or force is used.
"""
import argparse
import configparser
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

NAMES = ('keycodemapdb', 'dtc', 'berkeley-softfloat-3', 'berkeley-testfloat-3')


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args])


def file_entry(repo, path):
    if path.is_symlink():
        data, mode = os.fsencode(os.readlink(path)), '120000'
    elif path.is_file():
        data = path.read_bytes()
        mode = '100755' if path.stat().st_mode & 0o111 else '100644'
    else:
        raise RuntimeError(f'Unsupported source file: {path}')
    digest = subprocess.check_output(['git', '-C', str(repo), 'hash-object', '--stdin'], input=data).decode().strip()
    return mode, digest


def actual_tree(repo):
    result = {}
    for directory, dirs, files in os.walk(repo, followlinks=False):
        relative = Path(directory).relative_to(repo)
        if relative == Path('.'):
            dirs[:] = [name for name in dirs if name != '.git']
            files = [name for name in files if name != '.git']
        for name in list(dirs):
            path = Path(directory) / name
            if path.is_symlink():
                files.append(name)
                dirs.remove(name)
        for name in files:
            path = Path(directory) / name
            result[path.relative_to(repo).as_posix()] = file_entry(repo, path)
    return result


def pin_tree(repo, pin):
    result = {}
    for entry in git(repo, 'ls-tree', '-rz', pin).split(b'\0'):
        if not entry:
            continue
        metadata, name = entry.split(b'\t', 1)
        mode, kind, digest = metadata.decode().split()
        if kind != 'blob':
            raise RuntimeError(f'Unsupported nested Git dependency in {repo}: {name!r}')
        result[os.fsdecode(name)] = mode, digest
    return result


def overlay_files(subprojects, config):
    name = config.get('patch_directory')
    if not name:
        return {}
    if Path(name).name != name:
        raise RuntimeError(f'Unsafe patch_directory: {name}')
    directory = subprojects / 'packagefiles' / name
    if not directory.is_dir():
        raise RuntimeError(f'Missing Meson overlay: {directory}')
    result = {}
    for path in directory.rglob('*'):
        if path.is_symlink():
            raise RuntimeError(f'Unsupported overlay symlink: {path}')
        if path.is_file():
            result[path.relative_to(directory).as_posix()] = path
    return result


def plan(repo, pin, overlay):
    if repo.is_symlink() or not (repo / '.git').is_dir():
        raise RuntimeError(f'Refusing non-Git or linked source directory: {repo}')
    if git(repo, 'rev-parse', 'HEAD').decode().strip() != pin:
        raise RuntimeError(f'Unexpected dependency revision: {repo}')
    if git(repo, 'diff', '--cached', '--name-only', pin).strip():
        raise RuntimeError(f'Staged changes in {repo}')
    base = pin_tree(repo, pin)
    expected = dict(base)
    expected.update({name: file_entry(repo, path) for name, path in overlay.items()})
    actual = actual_tree(repo)
    if actual == expected:
        return False
    if actual != base:
        raise RuntimeError(f'Divergent dependency files in {repo}; preserving them')
    return True


def apply_overlay(repo, overlay):
    for name, path in overlay.items():
        target = repo / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)


def bootstrap(root):
    subprojects = root / 'triton-qemu/subprojects'
    entries = []
    for name in NAMES:
        parser = configparser.ConfigParser()
        with (subprojects / (name + '.wrap')).open() as stream:
            parser.read_file(stream)
        config = parser['wrap-git']
        pin, url = config['revision'], config['url']
        if not re.fullmatch('[0-9a-f]{40}', pin):
            raise RuntimeError(f'Unpinned revision for {name}')
        if url != f'https://gitlab.com/qemu-project/{name}.git':
            raise RuntimeError(f'Unexpected public dependency URL for {name}: {url}')
        overlay = overlay_files(subprojects, config)
        repo = subprojects / name
        if repo.exists() or repo.is_symlink():
            needed = plan(repo, pin, overlay)
        else:
            needed = True
        entries.append((repo, pin, url, overlay, needed))
    # All existing trees are checked before any download or overlay writes.
    for repo, pin, url, overlay, needed in entries:
        if not repo.exists():
            with tempfile.TemporaryDirectory(prefix='.bootstrap-', dir=subprojects) as staging:
                source = Path(staging) / 'source'
                source.mkdir()
                git(source, 'init', '-q')
                git(source, 'remote', 'add', 'origin', url)
                git(source, 'fetch', '--depth=1', 'origin', pin)
                git(source, 'checkout', '--no-overwrite-ignore', '--detach', pin)
                plan(source, pin, overlay)
                apply_overlay(source, overlay)
                if plan(source, pin, overlay):
                    raise RuntimeError(f'Overlay verification failed: {repo}')
                if repo.exists() or repo.is_symlink():
                    raise RuntimeError(f'Destination appeared during download: {repo}')
                source.rename(repo)
        elif needed:
            apply_overlay(repo, overlay)
        print(f'{repo.name}: {pin} (source and overlay verified)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    try:
        bootstrap(Path(__file__).resolve().parent.parent)
    except (OSError, RuntimeError, KeyError, configparser.Error, subprocess.CalledProcessError) as error:
        parser.exit(1, f'bootstrap-qemu: {error}\n')


if __name__ == '__main__':
    main()
