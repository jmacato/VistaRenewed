"""Explicit paths for public test runners; importing this module executes no tests."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
ARTIFACTS = Path(os.environ.get('TRITON_TEST_ARTIFACTS', ROOT / 'test-artifacts')).resolve()
HOST_PREFIX = Path(os.environ.get('VISTA_HOST_PREFIX', ROOT / 'host-linux')).resolve()
QEMU_BUILD = Path(os.environ.get('VISTA_QEMU_BUILD_DIR', ROOT / 'triton-qemu/build-linux')).resolve()
SDK_ROOT = Path(os.environ.get('VISTA_SDK_ROOT', ROOT / 'driver/sdk')).resolve()


def required_file(path, remedy):
    path = Path(path)
    if not path.is_file():
        raise SystemExit(f'Missing test dependency: {path}\n{remedy}')
    return path


def sdk_header(name, package='*'):
    matches = sorted(SDK_ROOT.glob(f'{package}/c/Include/*/*/{name}'))
    if not matches:
        raise SystemExit(f'Missing SDK header {name} under {SDK_ROOT}. Run the documented SDK bootstrap or set VISTA_SDK_ROOT.')
    if len(matches) > 1:
        version = os.environ.get('VISTA_SDK_VERSION')
        if version:
            matches = [p for p in matches if version in p.parts]
        if len(matches) != 1:
            raise SystemExit(f'Ambiguous SDK header {name}; set VISTA_SDK_VERSION. Candidates: {matches}')
    return matches[0]


def native_headers():
    """Use repository headers; no installed native DLLs are needed for CPU tests."""
    base = ROOT / 'triton-dxvk/include/native'
    required_file(base / 'directx/d3d11.h', 'Run scripts/bootstrap_sources.py first.')
    return [flag for p in ('directx', 'windows', '') for flag in ('-isystem', str(base / p))]


def native_libraries():
    override = os.environ.get('TRITON_TEST_DXVK_LIBDIR')
    directories = ([Path(p).resolve() for p in override.split(os.pathsep)] if override
                   else [HOST_PREFIX / 'lib/x86_64-linux-gnu'])
    for library in ('libdxvk_d3d11.so', 'libdxvk_dxgi.so'):
        if not any((d / library).is_file() for d in directories):
            raise SystemExit(f'Missing {library} in {directories}. Build the public native backend, or set TRITON_TEST_DXVK_LIBDIR to its d3d11 and dxgi directories.')
    return directories


def container_command(*args, workdir='/workspace'):
    """Use the public ephemeral build container; it exposes no GPU or VM device."""
    wrapper = required_file(ROOT / 'scripts/dev-container.sh', 'Use the public checkout with scripts/dev-container.sh.')
    engine = os.environ.get('CONTAINER_ENGINE') or ('podman' if shutil.which('podman') else 'docker')
    if not shutil.which(engine):
        raise SystemExit(f'Missing {engine}; install it or set CONTAINER_ENGINE.')
    return [str(wrapper), 'run', 'sh', '-c', 'cd "$1" && shift && exec "$@"',
            'test-build', workdir, *map(str, args)]


def container_path(path):
    try:
        return '/workspace/' + str(Path(path).resolve().relative_to(ROOT))
    except ValueError:
        raise SystemExit(f'Container build path must be inside this checkout: {path}')


def vista_compile_database(arch):
    path = Path(os.environ.get('VISTA_UMD_BUILD_' + arch.upper(),
                              ROOT / f'triton-umd/build-vista-linux-{arch}')) / 'compile_commands.json'
    return required_file(path, f'Build the {arch} Vista UMD first, or set VISTA_UMD_BUILD_{arch.upper()}.')
