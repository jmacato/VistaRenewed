#!/usr/bin/env python3
"""Build optional IE/MSHTML experiments without installing or registering them."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    (ROOT / 'build').mkdir(exist_ok=True)
    for script, arguments in (
        ('build_ie7_cef_bridge.py', []),
        ('build_ie7_mshtml_probe.py', []),
        ('build_ie7_mshtml_activation_probe.py', ['--both']),
    ):
        subprocess.run([sys.executable, str(ROOT / 'scripts' / script), *arguments], cwd=ROOT, check=True)
    for kind in ('content', 'element', 'navigation', 'transport', 'view'):
        subprocess.run([str(ROOT / 'scripts/dev-container.sh'), 'run', 'bash',
                        f'scripts/build_ie7_mshtml_{kind}_test.sh'], cwd=ROOT, check=True)
    print('Optional IE/MSHTML payloads built; guest runtime validation is separate.')


if __name__ == '__main__':
    main()
