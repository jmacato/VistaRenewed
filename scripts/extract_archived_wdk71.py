#!/usr/bin/env python3
"""Verify and extract archived WDK 7.1 build inputs without running installers.

Requires 7z and msiextract (p7zip-full and msitools on Ubuntu).
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--download', action='store_true', help='Download the pinned archive if the ISO is absent')
args = parser.parse_args()

root = Path(__file__).resolve().parent.parent / 'driver/toolchains/wdk71'
iso = root / 'GRMWDK_EN_7600_1.ISO'
expected = '5edc723b50ea28a070cad361dd0927df402b7a861a036bbcf11d27ebba77657d'
download_url = 'https://archive.org/download/en_windows_driver_kit_version_7.1.0_x86_x64_ia64_dvd_496758/en_windows_driver_kit_version_7.1.0_x86_x64_ia64_dvd_496758.iso'
if args.download and not iso.exists():
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=root, suffix='.download') as temporary:
        with urllib.request.urlopen(download_url, timeout=60) as response:
            while chunk := response.read(1024 * 1024):
                temporary.write(chunk)
        temporary.flush()
        temporary.seek(0)
        if hashlib.file_digest(temporary, 'sha256').hexdigest() != expected:
            raise SystemExit('Downloaded WDK ISO SHA-256 mismatch')
        # Copy only after verification; the temporary download is always removed.
        import shutil
        shutil.copyfile(temporary.name, iso)
with iso.open('rb') as stream:
    actual = hashlib.file_digest(stream, 'sha256').hexdigest()
if actual != expected:
    raise SystemExit(f'WDK ISO SHA-256 mismatch: {actual}')
with iso.open('rb') as stream:
    sha1 = hashlib.file_digest(stream, 'sha1').hexdigest()
assert sha1 == 'de6abdb8eb4e08942add4aa270c763ed4e3d8242'
media = root / 'media'
destination = root / 'extracted'
subprocess.run(['7z', 'x', '-y', f'-o{media}', str(iso), 'WDK/*'], check=True)
packages = ['headers', 'buildtools_x86fre', 'buildtools_x64fre',
            'vistalibs_x86fre', 'vistalibs_x64fre', 'libs_x86fre', 'libs_x64fre',
            'tools_x86fre', 'tools_x64fre',
            'drvtools_x86fre', 'drvtools_x64fre']
for package in packages:
    subprocess.run(['msiextract', '-C', str(destination),
                    str(media / 'WDK' / (package + '.msi'))], check=True)
files = {}
for path in sorted(destination.rglob('*')):
    if path.is_file():
        with path.open('rb') as stream:
            files[str(path.relative_to(destination))] = hashlib.file_digest(stream, 'sha256').hexdigest()
record = {
    'source': 'https://archive.org/details/en_windows_driver_kit_version_7.1.0_x86_x64_ia64_dvd_496758',
    'download_url': 'https://archive.org/download/en_windows_driver_kit_version_7.1.0_x86_x64_ia64_dvd_496758/en_windows_driver_kit_version_7.1.0_x86_x64_ia64_dvd_496758.iso',
    'iso_sha256': actual, 'iso_sha1': sha1, 'iso_size': iso.stat().st_size,
    'packages': packages, 'files': files,
}
(root / 'provenance.json').write_text(json.dumps(record, indent=2) + '\n')
print(f'Verified WDK 7.1 ISO; extracted and hashed {len(files)} files.')
