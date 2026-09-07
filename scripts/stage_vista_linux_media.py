#!/usr/bin/env python3
"""Publish a hashed, read-only Vista driver test ISO from the signed package."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--package', type=Path, default=root/'build/package-x64')
parser.add_argument('--certificate', type=Path, default=root/'driver/signing/triton-vista-linux-signing.cer')
parser.add_argument('--publications', type=Path, default=root/'vista-kvm/publications')
parser.add_argument('--no-activate', action='store_true', help='Do not change the local VM deployment pointer')
parser.add_argument('--installer-readme', type=Path)
parser.add_argument('--notices-dir', type=Path)
args = parser.parse_args()
package = args.package.resolve()
files = ['viogpu3d-diagnostic.inf', 'viogpu3d-vista-x64.cat', 'viogpu3d.sys',
         'neptune_d3d9.dll', 'neptune_d3d9_wow.dll', 'triton-vista-deploy.exe',
         'triton9_runtime_probe_x64.exe']
def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()
for name in files:
    if not (package/name).is_file() or (package/name).stat().st_size == 0:
        raise SystemExit(f'Missing package file: {name}')
manifest = ''.join(f'{sha(package/name)}  {name}\n' for name in files)
(package/'package-manifest.sha256').write_text(manifest)
deployment = hashlib.sha256(manifest.encode()).hexdigest()
publications = args.publications.resolve()
publications.mkdir(parents=True, exist_ok=True)
destination = publications/deployment
if destination.exists():
    raise SystemExit(f'Publication already exists: {destination}')
with tempfile.TemporaryDirectory(prefix='.stage-', dir=publications) as temp:
    stage = Path(temp)
    tree = stage/'tree'
    tree.mkdir()
    payload = tree/'driver-x64'
    payload.mkdir()
    for name in files+['package-manifest.sha256']:
        shutil.copy2(package/name, payload/name)
    for name in ['triton-vista-deploy.exe', 'triton9_runtime_probe_x64.exe']:
        shutil.copy2(package/name, tree/name)
    cert = args.certificate.resolve()
    cert_name = 'triton-vista-linux-signing.cer'
    shutil.copy2(cert, tree/cert_name)
    ini = f'''[triton-deploy]
version=1
id={deployment}
package=driver-x64
inf=viogpu3d-diagnostic.inf
hardware_id=PCI\\VEN_1AF4&DEV_1050
probe=triton9_runtime_probe_x64.exe
probe_sha256={sha(tree/'triton9_runtime_probe_x64.exe')}
signing_cert={cert_name}
signing_cert_sha256={sha(cert)}
'''
    (tree/'triton-deploy.ini').write_text(ini)
    bootstrap = r'''@echo off
setlocal
cd /d "%~dp0"
set "media_drive=%~d0"
certutil -addstore -f Root triton-vista-linux-signing.cer
if errorlevel 1 goto failed
certutil -addstore -f TrustedPublisher triton-vista-linux-signing.cer
if errorlevel 1 goto failed
triton-vista-deploy.exe --verify-media %media_drive:~0,1%
if errorlevel 1 goto failed
triton-vista-deploy.exe --install
if errorlevel 1 goto failed
echo The guest deployment service now owns driver installation and restarts.
pause
exit /b 0
:failed
echo Bootstrap failed. Run this file as administrator inside Vista.
pause
exit /b 1
'''
    (tree/'bootstrap.cmd').write_bytes(bootstrap.replace('\n', '\r\n').encode('ascii'))
    (tree/'install.cmd').write_bytes((tree/'bootstrap.cmd').read_bytes())
    if args.installer_readme:
        (tree/'README.txt').write_bytes(args.installer_readme.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
    if args.notices_dir:
        shutil.copytree(args.notices_dir, tree/'NOTICES')
    subprocess.run(['xorriso', '-as', 'mkisofs', '-quiet', '-J', '-r',
                    '-V', 'TritonVistaDeploy', '-o', str(stage/'media.iso'), str(tree)], check=True)
    if (stage/'media.iso').stat().st_size > 512*1024*1024:
        raise SystemExit('Media exceeds 512 MiB')
    (stage/'deployment-id').write_text(deployment+'\n')
    (stage/'media.iso.sha256').write_text(sha(stage/'media.iso')+'  media.iso\n')
    (stage/'tree-sha256.json').write_text(json.dumps({str(p.relative_to(tree)):sha(p) for p in tree.rglob('*') if p.is_file()},indent=2)+'\n')
    shutil.move(str(stage), destination)
if not args.no_activate:
    pointer = root/'vista-kvm/deploy-current'
    temporary = pointer.with_name('.deploy-current.tmp')
    temporary.symlink_to(destination.relative_to(pointer.parent))
    temporary.replace(pointer)
print(f'Published {deployment}\n{destination}/media.iso')
