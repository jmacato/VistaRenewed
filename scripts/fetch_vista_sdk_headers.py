#!/usr/bin/env python3
"""Restore pinned Microsoft SDK/WDK headers for the Vista UMD cross-build."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import urllib.request
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--force', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent / 'driver/sdk'
version = '10.0.28000.1-rtm'
packages = {
    'microsoft.windows.sdk.cpp': '0e287e9382fe92736111840b186b7690bcce623c3ce0e2a8a7d2d0d04199482c',
    'microsoft.windows.wdk.x64': '506103e3da1cacad2b98193faf9ee873f3300560959caba85349d9e04afd4803',
}
root.mkdir(parents=True, exist_ok=True)
for package, expected in packages.items():
    record = root / f'{package}.json'
    if not args.force and record.exists():
        previous = json.loads(record.read_text())
        files = list((root / package / 'c/Include').rglob('*'))
        if (previous.get('package_sha256') == expected and
                sum(p.is_file() for p in files) == previous.get('extracted_headers')):
            print(f'{package}: pinned headers already present')
            continue
    url = f'https://api.nuget.org/v3-flatcontainer/{package}/{version}/{package}.{version}.nupkg'
    with tempfile.TemporaryFile() as archive:
        with urllib.request.urlopen(url, timeout=60) as response:
            while data := response.read(1024 * 1024):
                archive.write(data)
        archive.seek(0)
        actual = hashlib.file_digest(archive, 'sha256').hexdigest()
        if actual != expected:
            raise SystemExit(f'{package}: package SHA-256 mismatch')
        archive.seek(0)
        count = 0
        with zipfile.ZipFile(archive) as entries:
            for name in entries.namelist():
                relative = Path(name)
                if not name.startswith('c/Include/') or name.endswith('/'):
                    continue
                if relative.is_absolute() or '..' in relative.parts:
                    raise SystemExit(f'Unsafe archive path: {name}')
                destination = root / package / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(entries.read(name))
                count += 1
    record.write_text(json.dumps({'url': url, 'package_sha256': actual,
                                 'extracted_headers': count}, indent=2) + '\n')
    print(f'{package}: verified package, extracted {count} headers')
