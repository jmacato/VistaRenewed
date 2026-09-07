#!/usr/bin/env python3
"""Build a self-contained development ISO with a disposable signing identity.

Consumes freshly built driver binaries. Never uses the local VM's signing key
or changes its deploy-current pointer. Run inside the Linux builder image.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
out = root / 'dist'
out.mkdir(exist_ok=True)


def run(*args, **kwargs):
    return subprocess.run(list(map(str, args)), cwd=root, check=True, **kwargs)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


inputs = {
    'viogpu3d.sys': root / 'test-artifacts/linux-build/kmd-x64/viogpu3d.sys',
    'neptune_d3d9.dll': root / 'triton-umd/build-vista-linux-x64/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll',
    'neptune_d3d9_wow.dll': root / 'triton-umd/build-vista-linux-x86/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll',
    'triton9_runtime_probe_x64.exe': root / 'triton-umd/build-vista-linux-x64/src/virtio/neptune/vista-d3d9/triton9_runtime_probe.exe',
    'viogpu3d-diagnostic.inf': root / 'test-artifacts/vista-driver-x64-kd-serialtrace/viogpu3d-diagnostic.inf',
}
for path in inputs.values():
    if not path.is_file():
        raise SystemExit(f'Build input missing: {path}')

with tempfile.TemporaryDirectory(prefix='triton-ci-signing-') as temporary:
    work = Path(temporary)
    key, pem, cert = work/'private.key', work/'certificate.pem', work/'certificate.cer'
    # No timestamp service or release credential: this is a development identity.
    run('openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes', '-x509',
        '-sha1', '-days', '365', '-subj', '/CN=Triton Vista CI development/',
        '-addext', 'basicConstraints=critical,CA:TRUE',
        '-addext', 'keyUsage=critical,digitalSignature,keyCertSign',
        '-addext', 'extendedKeyUsage=codeSigning', '-keyout', key, '-out', pem,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    key.chmod(0o600)
    run('openssl', 'x509', '-in', pem, '-outform', 'DER', '-out', cert)
    run('bash', 'scripts/build_vista_service_linux.sh',
        env={**os.environ, 'VISTA_DEPLOY_CERT': str(cert)})
    inputs['triton-vista-deploy.exe'] = root/'test-artifacts/linux-build/triton-vista-deploy.exe'
    if sha(cert).encode('utf-16le') not in inputs['triton-vista-deploy.exe'].read_bytes():
        raise SystemExit('Deployment service does not contain this build certificate pin')
    package = work/'package'
    package.mkdir()
    for name, path in inputs.items():
        shutil.copy2(path, package/name)
    inf = package/'viogpu3d-diagnostic.inf'
    inf.write_bytes(inf.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))

    def sign(path):
        signed = path.with_suffix(path.suffix + '.signed')
        run('osslsigncode', 'sign', '-certs', pem, '-key', key, '-h', 'sha1',
            *(['-ph'] if path.suffix != '.cat' else []), '-in', path, '-out', signed)
        signed.replace(path)
        run('osslsigncode', 'verify', '-CAfile', pem, '-in', path)

    binaries = [package/name for name in inputs if Path(name).suffix in ('.sys', '.dll', '.exe')]
    for path in binaries:
        sign(path)
    run('python3', 'scripts/create_vista_catalog.py', package)
    catalog = package/'viogpu3d-vista-x64.cat'
    sign(catalog)
    for path in binaries:
        run('osslsigncode', 'verify', '-CAfile', pem, '-catalog', catalog, '-in', path)
    run('python3', 'triton-kmd/viogpu/tools/check_vista_inf.py', '--arch', 'x64',
        inf, '--package-dir', package)
    publications = work/'publications'
    notices = work/'notices'
    notices.mkdir()
    for name, source in {
        'virtio-gpu-LICENSE.txt': root/'triton-kmd/viogpu/LICENSE',
        'virtio-LICENSE.txt': root/'triton-kmd/VirtIO/LICENSE',
        'shader-converter-LICENSE.txt': root/'triton-umd/src/virtio/neptune/vista-d3d9/third_party/d3d9on12-shaderconverter/LICENSE',
        'upstream-sources.md': root/'docs/UPSTREAM.md',
    }.items():
        shutil.copy2(source, notices/name)
    shutil.copytree(root/'triton-umd/licenses', notices/'mesa-licenses')
    run('python3', 'scripts/stage_vista_linux_media.py', '--package', package,
        '--certificate', cert, '--publications', publications, '--no-activate',
        '--installer-readme', root/'docs/INSTALL-ISO.txt', '--notices-dir', notices)
    publication, = publications.iterdir()
    # Read the ISO back through an independent filesystem parser before export.
    extracted = work/'extracted'
    run('7z', 'x', '-y', f'-o{extracted}', publication/'media.iso', stdout=subprocess.DEVNULL)
    for name, digest in json.loads((publication/'tree-sha256.json').read_text()).items():
        if sha(extracted/name) != digest:
            raise SystemExit(f'ISO round-trip mismatch: {name}')
    forbidden = {'.key', '.pem', '.pfx', '.p12', '.pdb'}
    if any(p.suffix.lower() in forbidden for p in extracted.rglob('*') if p.is_file()):
        raise SystemExit('Unexpected signing or debug file in ISO')
    # Publish only explicit artifacts; the temporary signing directory is removed.
    iso = out/'triton-vista-x64.iso'
    shutil.copy2(publication/'media.iso', iso)
    (out/'SHA256SUMS').write_text(f'{sha(iso)}  {iso.name}\n')
    shutil.copy2(root/'docs/INSTALL-ISO.txt', out/'INSTALL.txt')
    (out/'build-info.json').write_text(json.dumps({
        'source_revision': os.environ.get('VISTA_SOURCE_REVISION', 'local-unrecorded'),
        'target': 'Windows Vista SP2 x64 (includes x86 WoW64 UMD)',
        'signing': 'ephemeral self-signed development certificate; not WHQL',
        'certificate_sha256': sha(cert),
        'deployment_id': publication.name,
        'iso_sha256': sha(iso),
        'files': json.loads((publication/'tree-sha256.json').read_text()),
        'guest_runtime_tested': False,
    }, indent=2) + '\n')
print(f'Development installer ISO: {out / "triton-vista-x64.iso"}')
