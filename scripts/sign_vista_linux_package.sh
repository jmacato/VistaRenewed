#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
# Run through scripts/dev-container.sh; keep the identity in an ignored input
# directory mounted into that container. The key is read in place.
python=${VISTA_PYTHON:-python3}
identity=${VISTA_SIGNING_DIRECTORY:-$root/driver/signing}
key="$identity/linux-vista-test.key"
cert="$identity/linux-vista-test.pem"
[[ -r "$key" && -r "$cert" ]] || {
    echo "Set VISTA_SIGNING_DIRECTORY to a directory containing linux-vista-test.key and linux-vista-test.pem." >&2
    exit 1
}
certificate="$root/build/persistent-signing-certificate.cer"
package=build/package-x64
mkdir -p "$package"
openssl x509 -in "$cert" -outform DER -out "$certificate"
VISTA_DEPLOY_CERT="$certificate" bash scripts/build_vista_service_linux.sh --arch x64
cp build/kmd-x64/viogpu3d.sys "$package/"
cp triton-umd/build-vista-linux-x64/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll "$package/"
cp triton-umd/build-vista-linux-x64/src/virtio/neptune/vista-d3d10/neptune_d3d10.dll "$package/"
cp triton-umd/build-vista-linux-x86/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll "$package/neptune_d3d9_wow.dll"
cp triton-umd/build-vista-linux-x86/src/virtio/neptune/vista-d3d10/neptune_d3d10.dll "$package/neptune_d3d10_wow.dll"
cp triton-umd/build-vista-linux-x64/src/virtio/neptune/vista-d3d9/triton9_runtime_probe.exe "$package/triton9_runtime_probe_x64.exe"
for probe_arch in x64 x86; do
    for probe in runtime present; do
        cp "triton-umd/build-vista-linux-$probe_arch/src/virtio/neptune/vista-d3d10/triton10_${probe}_probe.exe" "$package/triton10_${probe}_probe_${probe_arch}.exe"
    done
done
cp build/triton-vista-deploy.exe "$package/"
cp packaging/viogpu3d-diagnostic.inf "$package/"
# Ship canonical Windows INF text before calculating the catalog hash.
python3 - <<'PYINF'
from pathlib import Path
p = Path('build/package-x64/viogpu3d-diagnostic.inf')
p.write_bytes(p.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
PYINF
sign() {
    local file=$1
    local flags=()
    [[ $file == *.cat ]] || flags+=(-ph)
    osslsigncode sign -certs "$cert" -key "$key" -h sha1 "${flags[@]}" -in "$file" -out "$file.signed"
    mv "$file.signed" "$file"
    osslsigncode verify -CAfile "$cert" -in "$file"
}
for file in "$package"/*.sys "$package"/*.dll "$package"/*.exe; do
    sign "$file"
done > build/package-sign.log 2>&1
"$python" scripts/create_vista_catalog.py "$package"
sign "$package/viogpu3d-vista-x64.cat" > build/catalog-sign.log 2>&1
for file in "$package"/*.sys "$package"/*.dll "$package"/*.exe; do
    osslsigncode verify -CAfile "$cert" -catalog "$package/viogpu3d-vista-x64.cat" -in "$file"
done > build/catalog-pe-verify.log 2>&1
python3 triton-kmd/viogpu/tools/check_vista_inf.py --arch x64 "$package/viogpu3d-diagnostic.inf" --package-dir "$package"
python3 scripts/stage_vista_linux_media.py --certificate "$certificate" \
    --publications "$root/dist/persistent" --no-activate
