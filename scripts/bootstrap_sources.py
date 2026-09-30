#!/usr/bin/env python3
"""Restore pinned public DXVK sources and verified downstream patches.

Repeated execution accepts the exact curated tree. Divergent tracked or nonignored
untracked files are rejected; no reset, clean, stash, or forced checkout is used.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def git(repo, *args, env=None):
    return subprocess.check_output(['git', '-C', str(repo), *args], env=env)


def verify(root, name, digest):
    path = root / name
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise RuntimeError(f'Source input checksum mismatch: {name}')
    return path


def plan_patch(repo, pin, patch):
    if git(repo, 'rev-parse', 'HEAD').decode().strip() != pin:
        raise RuntimeError(f'Unexpected source revision: {repo}')
    with tempfile.TemporaryDirectory(prefix='triton-source-index-') as directory:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(directory) / 'index'))
        git(repo, 'read-tree', pin, env=env)
        base = git(repo, 'write-tree', env=env)
        git(repo, 'apply', '--cached', str(patch), env=env)
        expected = git(repo, 'write-tree', env=env)
        git(repo, 'read-tree', pin, env=env)
        git(repo, 'add', '-A', '--', '.', env=env)
        actual = git(repo, 'write-tree', env=env)
    # Preserve staged changes too: bootstrap never silently consumes an index.
    if git(repo, 'diff', '--cached', '--name-only', pin).strip():
        raise RuntimeError(f'Staged changes in {repo}; refusing to bootstrap')
    if actual == expected:
        return False
    if actual != base:
        raise RuntimeError(f'Divergent source changes in {repo}; refusing to bootstrap')
    git(repo, 'apply', '--check', str(patch))
    return True


def bootstrap(root):
    manifest = json.loads((root / 'patches/sources.json').read_text())
    if manifest['version'] != 1:
        raise RuntimeError('Unsupported source manifest version')
    bundle = verify(root, manifest['bundle'], manifest['bundle_sha256'])
    patches = [verify(root, item['patch'], item['sha256'])
               for item in manifest['repositories']]
    source = root / 'triton-dxvk'
    pin = manifest['repositories'][0]['commit']
    if not (source / '.git').exists():
        if source.exists() and any(source.iterdir()):
            raise RuntimeError(f'Refusing nonempty source directory: {source}')
        subprocess.run(['git', 'clone', '--no-checkout', manifest['url'], str(source)], check=True)
        git(source, 'checkout', '--no-overwrite-ignore', '--detach', manifest['base'])
    head = git(source, 'rev-parse', 'HEAD').decode().strip()
    if head == manifest['base']:
        if git(source, 'status', '--porcelain').strip():
            raise RuntimeError('Base checkout has local changes')
        git(source, 'bundle', 'verify', str(bundle))
        git(source, 'fetch', str(bundle), 'HEAD')
        git(source, 'cat-file', '-e', pin + '^{commit}')
        git(source, 'checkout', '--no-overwrite-ignore', '--detach', pin)
    elif head != pin:
        raise RuntimeError(f'Unexpected DXVK revision: {head}')
    # Validate the parent before reading its .gitmodules for network operations.
    plan_patch(source, pin, patches[0])
    # A patched dependency may have nested modules of its own. Validate its
    # source, including .gitmodules, before recursive update follows any URLs.
    for entry, patch in zip(manifest['repositories'][1:], patches[1:]):
        dependency = root / entry['path']
        if (dependency / '.git').exists():
            plan_patch(dependency, entry['commit'], patch)
    # --init without --force preserves existing submodule working changes.
    # Reject unexpected initialized revisions before update can move them.
    for line in git(source, 'submodule', 'status', '--recursive').decode().splitlines():
        if line.startswith(('+', 'U')):
            raise RuntimeError(f'Unexpected dependency revision: {line}')
        if not line.startswith('-'):
            dependency = line[1:].split()[1]
            if dependency != 'subprojects/dxbc-spirv':
                if git(source / dependency, 'status', '--porcelain',
                       '--untracked-files=normal', '--ignore-submodules=all').strip():
                    raise RuntimeError(f'Divergent dependency changes: {dependency}')
    git(source, 'submodule', 'update', '--init', '--recursive')
    plans = [(root / entry['path'], patch,
              plan_patch(root / entry['path'], entry['commit'], patch))
             for entry, patch in zip(manifest['repositories'], patches)]
    for repo, patch, apply in plans:
        if apply:
            git(repo, 'apply', str(patch))
    print('Pinned DXVK and shader sources ready; patches verified.')


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    try:
        bootstrap(Path(__file__).resolve().parent.parent)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'bootstrap: {error}\n')


if __name__ == '__main__':
    main()
