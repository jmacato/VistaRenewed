#!/usr/bin/env python3
"""Restore the pinned DXVK checkout, including our unpublished source commit."""
import argparse
from pathlib import Path
import subprocess

BASE = '404240fdacf47470b02c76d6e684639a95dc7387'
COMMIT = 'c6bb6d57fac2b6cae7f6adbbc521eb949849815e'
URL = 'https://github.com/osy/dxvk.git'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-submodules', action='store_true',
                        help='Restore DXVK only; leave its dependencies untouched')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = root / 'triton-dxvk'

    def git(*arguments, capture=False):
        return subprocess.run(['git', '-C', str(source), *arguments], check=True,
                              text=True, stdout=subprocess.PIPE if capture else None)

    if not (source / '.git').exists():
        if source.exists() and any(source.iterdir()):
            raise SystemExit(f'Refusing to replace nonempty source export: {source}')
        subprocess.run(['git', 'clone', '--no-checkout', URL, str(source)], check=True)
        git('checkout', '--detach', BASE)
    if git('status', '--porcelain', '--untracked-files=normal', capture=True).stdout:
        raise SystemExit('DXVK has local changes; commit or stash them before bootstrapping.')
    head = git('rev-parse', 'HEAD', capture=True).stdout.strip()
    if head not in (BASE, COMMIT):
        raise SystemExit(f'Refusing to switch an unexpected DXVK revision: {head}')
    if head == BASE:
        bundle = str(root / 'patches/dxvk-neptune.bundle')
        git('bundle', 'verify', bundle)
        git('fetch', bundle, 'HEAD')
        git('cat-file', '-e', COMMIT + '^{commit}')
        git('checkout', '--detach', COMMIT)
    if not args.skip_submodules:
        git('submodule', 'update', '--init', '--recursive')
    print(f'DXVK ready at {COMMIT}')


if __name__ == '__main__':
    main()
