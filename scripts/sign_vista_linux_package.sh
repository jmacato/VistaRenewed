#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
builder=${VISTA_BUILDER:-vista-driver-builder}
python=${VISTA_PYTHON:-/tmp/vista-inspect-venv/bin/python}
identity=${VISTA_SIGNING_DIRECTORY:-$HOME/.local/share/triton-vista-signing}
package=test-artifacts/linux-build/package-x64
mkdir -p "$package"
podman exec "$builder" bash scripts/build_vista_service_linux.sh
cp test-artifacts/linux-build/kmd-x64/viogpu3d.sys "$package/"
cp triton-umd/build-vista-linux-x64/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll "$package/"
cp triton-umd/build-vista-linux-x86/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll "$package/neptune_d3d9_wow.dll"
cp triton-umd/build-vista-linux-x64/src/virtio/neptune/vista-d3d9/triton9_runtime_probe.exe "$package/triton9_runtime_probe_x64.exe"
cp test-artifacts/linux-build/triton-vista-deploy.exe "$package/"
cp test-artifacts/vista-driver-x64-kd-serialtrace/viogpu3d-diagnostic.inf "$package/"
# Ship canonical Windows INF text before calculating the catalog hash.
python3 - <<'PYINF'
from pathlib import Path
p = Path('test-artifacts/linux-build/package-x64/viogpu3d-diagnostic.inf')
p.write_bytes(p.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
PYINF
podman exec "$builder" mkdir -m 700 -p /opt/vista-signing
trap 'podman exec "$builder" rm -f /opt/vista-signing/private.key' EXIT
podman cp "$identity/linux-vista-test.key" "$builder:/opt/vista-signing/private.key"
podman cp "$identity/linux-vista-test.pem" "$builder:/opt/vista-signing/cert.pem"
podman exec "$builder" bash -c '
set -e
for file in /workspace/test-artifacts/linux-build/package-x64/*.sys /workspace/test-artifacts/linux-build/package-x64/*.dll /workspace/test-artifacts/linux-build/package-x64/*.exe; do
    osslsigncode sign -certs /opt/vista-signing/cert.pem -key /opt/vista-signing/private.key -h sha1 -ph -in "$file" -out "$file.signed"
    mv "$file.signed" "$file"
    osslsigncode verify -CAfile /opt/vista-signing/cert.pem -in "$file"
done' > test-artifacts/linux-build/package-sign.log 2>&1
"$python" scripts/create_vista_catalog.py "$package"
podman exec "$builder" bash -c '
set -e
dir=/workspace/test-artifacts/linux-build/package-x64
osslsigncode sign -certs /opt/vista-signing/cert.pem -key /opt/vista-signing/private.key -h sha1 -in "$dir/viogpu3d-vista-x64.cat" -out "$dir/catalog.signed"
mv "$dir/catalog.signed" "$dir/viogpu3d-vista-x64.cat"
osslsigncode verify -CAfile /opt/vista-signing/cert.pem -in "$dir/viogpu3d-vista-x64.cat"
for file in "$dir"/*.sys "$dir"/*.dll "$dir"/*.exe; do
    osslsigncode verify -CAfile /opt/vista-signing/cert.pem -catalog "$dir/viogpu3d-vista-x64.cat" -in "$file"
done' > test-artifacts/linux-build/catalog-pe-verify.log 2>&1
python3 triton-kmd/viogpu/tools/check_vista_inf.py --arch x64 "$package/viogpu3d-diagnostic.inf" --package-dir "$package"
python3 scripts/stage_vista_linux_media.py
