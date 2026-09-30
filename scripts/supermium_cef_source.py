#!/usr/bin/env python3
"""Verify the experimental CEF source pins; audit patches without editing source.

The audit uses a disposable Git index. Failed patches remain failures, and
dependency repositories are reported separately, never silently certified.
"""
import argparse
import ast
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'third_party/supermium-cef'
PINS = {
    'src': '82756ad44eee3166e8ac4fa7605632690ac70bc9',
    'cef': '0b1a01255cca9c6a000582ed81afcb3b67b2043d',
}
VERSION = '144.0.7559.256'


def git(repo, *args, **kwargs):
    return subprocess.run(['git', '-C', str(repo), *args],
                          text=True, capture_output=True, **kwargs)


def verify():
    for name, pin in PINS.items():
        result = git(BASE / name, 'rev-parse', 'HEAD', check=True)
        if result.stdout.strip() != pin:
            raise RuntimeError(f'{name}: wrong revision: {result.stdout.strip()}')
        print(f'{name}: {pin}', flush=True)
    raw = git(BASE / 'src', 'show', 'HEAD:chrome/VERSION', check=True).stdout
    fields = dict(line.split('=', 1) for line in raw.splitlines() if '=' in line)
    version = '.'.join(fields[k] for k in ('MAJOR', 'MINOR', 'BUILD', 'PATCH'))
    compatibility = ast.literal_eval(git(
        BASE / 'cef', 'show', 'HEAD:CHROMIUM_BUILD_COMPATIBILITY.txt',
        check=True).stdout)
    if version != VERSION or compatibility['chromium_checkout'] != 'refs/tags/' + version:
        raise RuntimeError('Chromium version mismatch')
    if git(BASE / 'cef', 'status', '--porcelain', check=True).stdout.strip():
        raise RuntimeError('CEF checkout has unreviewed modifications')
    print(f'Chromium version: {version}', flush=True)
    print('SUPERMIUM CEF SOURCE PINS VERIFIED', flush=True)


def patch_text(name, merged=False):
    content = (BASE / 'cef/patch/patches' / (name + '.patch')).read_text()
    if merged:
        contexts = json.loads((ROOT / 'packaging/supermium-cef/supermium_cef_patch_context.json').read_text())
        for before, after in contexts.get(name, []):
            if content.count(before) != 1:
                raise RuntimeError(f'{name}: expected one context match')
            content = content.replace(before, after)
    return content


def audit(merged=False):
    verify()
    config = ast.parse((BASE / 'cef/patch/patch.cfg').read_text())
    assignments = [node for node in config.body if isinstance(node, ast.Assign)
                   and any(isinstance(t, ast.Name) and t.id == 'patches'
                           for t in node.targets)]
    if len(assignments) != 1:
        raise RuntimeError('Unrecognized patch configuration')
    patches = ast.literal_eval(assignments[0].value)
    passed = failed = dependencies = 0
    results = []
    with tempfile.TemporaryDirectory(prefix='supermium-cef-index-') as temporary:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(temporary) / 'index'))
        git(BASE / 'src', 'read-tree', 'HEAD', env=env, check=True)
        for entry in patches:
            name = entry['name']
            if entry.get('path') or entry.get('condition'):
                dependencies += 1
                results.append({'name': name, 'status': 'untested', 'config': entry})
                print(f'UNTESTED {name}: {entry}', flush=True)
                continue
            result = git(BASE / 'src', 'apply', '-p0', '--recount', '--cached',
                         '--whitespace=nowarn', env=env, input=patch_text(name, merged))
            if result.returncode:
                failed += 1
                results.append({'name': name, 'status': 'conflict', 'stderr': result.stderr})
                print(f'CONFLICT {name}\n{result.stderr}', flush=True)
            else:
                passed += 1
                results.append({'name': name, 'status': 'applies'})
                print(f'APPLIES {name}', flush=True)
        if merged and not failed:
            combined = git(BASE / 'src', 'diff', '--cached', '--binary',
                           'HEAD', env=env, check=True).stdout
            (ROOT / 'build/supermium-cef-root.patch').write_text(combined)
    print(f'Patch audit: applies={passed}, conflicts={failed}, '
          f'untested_dependency_or_conditional={dependencies}', flush=True)
    print('Audit is sequential; conflicts can cause downstream failures. '
          'No working-tree patches were applied.', flush=True)
    report = ROOT / ('build/supermium-cef-merged-audit.json' if merged else
                     'build/supermium-cef-patch-audit.json')
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps({'pins': PINS, 'version': VERSION,
                                 'applies': passed, 'conflicts': failed,
                                 'untested': dependencies, 'patches': results}, indent=2) + '\n')
    return 1 if failed or dependencies else 0


def prefetch():
    """Fetch only blobs touched by CEF patches, in one network transaction."""
    verify()
    paths = set()
    for patch in (BASE / 'cef/patch/patches').glob('*.patch'):
        for line in patch.read_text().splitlines():
            if line.startswith('--- '):
                paths.add(line[4:].split('\t')[0])
    tree = git(BASE / 'src', 'ls-tree', '-rz', 'HEAD', check=True).stdout
    blobs = set()
    for record in tree.split('\0'):
        if not record:
            continue
        metadata, path = record.split('\t', 1)
        mode, kind, oid = metadata.split()
        if kind == 'blob' and path in paths:
            blobs.add(oid)
    if not blobs:
        raise RuntimeError('No patch input blobs found')
    print(f'Prefetching {len(blobs)} patch input blobs', flush=True)
    result = git(BASE / 'src', '-c', 'fetch.negotiationAlgorithm=noop',
                 'fetch', '--no-tags', '--no-write-fetch-head',
                 '--recurse-submodules=no', '--filter=blob:none',
                 'origin', '--stdin', input='\n'.join(sorted(blobs)) + '\n')
    if result.returncode:
        raise RuntimeError(result.stderr)
    print('Patch input blobs fetched', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('verify', 'audit', 'audit-merged', 'prefetch'))
    args = parser.parse_args()
    if args.command == 'audit':
        return audit()
    if args.command == 'audit-merged':
        return audit(merged=True)
    if args.command == 'prefetch':
        prefetch()
        return 0
    verify()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
