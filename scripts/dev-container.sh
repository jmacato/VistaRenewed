#!/usr/bin/env bash
# Ephemeral build commands; no named container or hardware access is required.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
usage() {
    echo "Usage: $0 build | run COMMAND [ARG ...]"
    echo "Environment: CONTAINER_ENGINE=podman|docker, VISTA_BUILD_IMAGE, JOBS"
}
case "${1:-}" in
    -h|--help) usage; exit 0 ;;
    build|run) action=$1; shift ;;
    *) usage >&2; exit 2 ;;
esac
engine=${CONTAINER_ENGINE:-}
if [[ -z "$engine" ]]; then
    if command -v podman >/dev/null; then engine=podman; else engine=docker; fi
fi
command -v "$engine" >/dev/null || { echo "Container engine not found: $engine" >&2; exit 1; }
image=${VISTA_BUILD_IMAGE:-localhost/triton-vista-builder:preview}
if [[ "$action" == build ]]; then
    [[ $# == 0 ]] || { usage >&2; exit 2; }
    exec "$engine" build -f "$root/scripts/linux-driver.Containerfile" -t "$image" "$root/scripts"
fi
[[ $# -gt 0 ]] || { usage >&2; exit 2; }
args=(run --rm -i --workdir /workspace --security-opt label=disable
      --volume "$root:/workspace" --env "JOBS=${JOBS:-4}")
if [[ $(basename -- "$engine") == podman ]]; then
    args+=(--userns=keep-id)
fi
# Capture on the host: a Git worktree's .git may refer outside its mounted tree.
revision=${VISTA_SOURCE_REVISION:-$(git -C "$root" rev-parse HEAD)}
args+=(--env "VISTA_SOURCE_REVISION=$revision")
exec "$engine" "${args[@]}" "$image" "$@"
