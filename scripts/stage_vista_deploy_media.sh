#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
workspace=${script_dir:h}
transfer_root="$workspace/aaaaa/vista-signing-transfer"
package_name=${1:-vista-driver-x64-pnp-current}
bundle_link=${2:-"$transfer_root/vista-deploy-current"}
publication_root="$transfer_root/vista-deploy-publications"
media_root="$bundle_link/tree"
media_iso="$bundle_link/media.iso"
package_dir="$transfer_root/$package_name"
service_exe="$package_dir/triton-vista-deploy.exe"
probe_exe="$package_dir/triton9_runtime_probe_x64.exe"
signing_cert="$transfer_root/triton-vista-unlazy-signing.cer"
maximum_media_bytes=$((512 * 1024 * 1024))

[[ -n "$package_name" && "$package_name" != *[^A-Za-z0-9._-]* &&
   "$package_name" != . && "$package_name" != .. ]] || {
    print -u2 -- "package name must be one safe leaf name"
    exit 1
}
transfer_canonical=$(realpath "$transfer_root")
bundle_parent_canonical=$(realpath "${bundle_link:h}")
[[ "$bundle_parent_canonical" == "$transfer_canonical" &&
   -n "${bundle_link:t}" &&
   "${bundle_link:t}" != *[^A-Za-z0-9._-]* &&
   "${bundle_link:t}" != . && "${bundle_link:t}" != .. ]] || {
    print -u2 -- "deployment bundle pointer must be a safe leaf in $transfer_root"
    exit 1
}
if [[ -e "$bundle_link" && ! -h "$bundle_link" ]]; then
    print -u2 -- "deployment bundle pointer is not a symbolic link: $bundle_link"
    exit 1
fi

available_kib=$(df -Pk "$transfer_root" | awk 'NR == 2 { print $4 }')
[[ "$available_kib" == <-> ]] || {
    print -u2 -- "cannot determine free space for deployment media"
    exit 1
}
(( available_kib >= 2 * 1024 * 1024 )) || {
    print -u2 -- "less than 2 GiB is free; refusing to stage deployment media"
    exit 1
}

for file in \
    package-manifest.sha256 \
    viogpu3d-diagnostic.inf \
    viogpu3d-vista-x64.cat \
    viogpu3d.sys \
    neptune_d3d9.dll \
    neptune_d3d9_wow.dll \
    triton-vista-deploy.exe \
    triton9_runtime_probe_x64.exe; do
    [[ -s "$package_dir/$file" ]] || {
        print -u2 -- "missing package file: $package_dir/$file"
        exit 1
    }
done

(cd "$package_dir" && shasum -a 256 -c package-manifest.sha256)

deploy_id=$(shasum -a 256 "$package_dir/package-manifest.sha256" | awk '{print $1}')
mkdir -p "$publication_root"
publication_canonical=$(realpath "$publication_root")
[[ "${publication_canonical:h}" == "$transfer_canonical" ]] || {
    print -u2 -- "publication directory escaped the transfer root"
    exit 1
}
stage=$(mktemp -d "$publication_root/.vista-deploy-bundle.XXXXXX")
tree="$stage/tree"
iso_stage="$stage/media.iso"
pointer_stage=""
cleanup_stage=1
cleanup() {
    if [[ -n "$pointer_stage" &&
          ( -e "$pointer_stage" || -h "$pointer_stage" ) ]]; then
        [[ "$pointer_stage" == "$transfer_root/.vista-deploy-current."* ]] || return
        rm -f -- "$pointer_stage"
    fi
    if (( cleanup_stage )) && [[ -d "$stage" ]]; then
        [[ "$stage" == "$publication_root/.vista-deploy-bundle."* ]] || return
        rm -rf -- "$stage"
    fi
}
trap cleanup EXIT INT TERM HUP

mkdir -p "$tree/$package_name"
cp -f "$package_dir"/* "$tree/$package_name/"
cp -f "$service_exe" "$tree/"
[[ -s "$probe_exe" ]] || {
    print -u2 -- "missing self-contained D3D9 probe: $probe_exe"
    exit 1
}
probe_sha256=$(shasum -a 256 "$probe_exe" | awk '{print $1}')
cp -f "$probe_exe" "$tree/"
[[ -s "$signing_cert" ]] || {
    print -u2 -- "missing public signing certificate: $signing_cert"
    exit 1
}
signing_cert_sha256=$(shasum -a 256 "$signing_cert" | awk '{print $1}')
cp -f "$signing_cert" "$tree/"

{
    print -r -- '[triton-deploy]'
    print -r -- 'version=1'
    print -r -- "id=$deploy_id"
    print -r -- "package=$package_name"
    print -r -- 'inf=viogpu3d-diagnostic.inf'
    print -r -- 'hardware_id=PCI\VEN_1AF4&DEV_1050'
    print -r -- 'probe=triton9_runtime_probe_x64.exe'
    print -r -- "probe_sha256=$probe_sha256"
    print -r -- 'signing_cert=triton-vista-unlazy-signing.cer'
    print -r -- "signing_cert_sha256=$signing_cert_sha256"
} > "$tree/triton-deploy.ini"

# Use ISO9660/Joliet rather than a raw FAT image.  The result is immutable
# optical media that Vista mounts through its inbox CD stack in Safe Mode.
hdiutil makehybrid -quiet -iso -joliet \
    -default-volume-name TritonVistaDeploy -o "$stage/media" "$tree"
[[ -s "$iso_stage" ]] || {
    print -u2 -- "deployment ISO was not created"
    exit 1
}
iso_bytes=$(stat -f %z "$iso_stage")
[[ "$iso_bytes" == <-> ]] || {
    print -u2 -- "cannot determine deployment ISO size"
    exit 1
}
(( iso_bytes <= maximum_media_bytes )) || {
    print -u2 -- "deployment ISO exceeds the 512 MiB bound"
    exit 1
}
iso_sha256=$(shasum -a 256 "$iso_stage" | awk '{print $1}')
print -r -- "$iso_sha256  media.iso" > "$stage/media.iso.sha256"
print -r -- "$deploy_id" > "$stage/deployment-id"

version_dir="$publication_root/$deploy_id"
if [[ -e "$version_dir" || -h "$version_dir" ]]; then
    [[ -d "$version_dir" && ! -h "$version_dir" &&
       -f "$version_dir/deployment-id" &&
       "$(<"$version_dir/deployment-id")" == "$deploy_id" ]] || {
        print -u2 -- "existing immutable deployment bundle is invalid: $version_dir"
        exit 1
    }
    (cd "$version_dir" && shasum -a 256 -c media.iso.sha256)
    (cd "$version_dir/tree/$package_name" &&
        shasum -a 256 -c package-manifest.sha256)
    rm -rf -- "$stage"
else
    mv -- "$stage" "$version_dir"
fi
cleanup_stage=0

pointer_stage="$transfer_root/.vista-deploy-current.$$"
[[ ! -e "$pointer_stage" && ! -h "$pointer_stage" ]] || {
    print -u2 -- "temporary bundle pointer already exists: $pointer_stage"
    exit 1
}
ln -s "vista-deploy-publications/$deploy_id" "$pointer_stage"
mv -fh -- "$pointer_stage" "$bundle_link"
trap - EXIT INT TERM HUP

print -r -- "deployment id: $deploy_id"
print -r -- "immutable bundle: $version_dir"
print -r -- "atomic bundle pointer: $bundle_link"
print -r -- "media directory: $media_root"
print -r -- "QEMU media: read-only ISO9660/Joliet optical image (not a raw disk): $media_iso"
