#!/usr/bin/env python3
"""Run the fixed public CPU regression suite. No GPU, container or VM is started."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CPU_FIXTURES = (
    'blob-readback', 'color-copy-transport', 'context-fence-notify',
    'cursor-queue', 'cursor-rgba', 'ddi-trace-budget', 'device-health',
    'draw-auto', 'dwm-blt-flags', 'egl-export-query',
    'egl-external-copy-lifecycle', 'egl-upload-context', 'external-copy-integration',
    'fence-health', 'flip-copy-completion', 'flip-flags', 'flip-wait-status',
    'gl-reset-thread', 'gtk-egl-viewport', 'host-present-blt', 'host-trace',
    'present-consumption', 'present-timeline', 'primary-allocation', 'primary-layout',
    'query-transport', 'ring-failure', 'ring-publication', 'scanout-address',
    'scanout-refresh', 'scanout-reuse', 'scanout-surface', 'shared-pending',
    'synchronized-flip', 'texture-transform', 'trace-buffer', 'vsync-timing', 'vsync-worker',
    'x86-normal-signing',
)


def cases(output):
    result = [(n, ['tests/vista/test-' + n + '.py']) for n in CPU_FIXTURES]
    result += [('kart-probe', ['tests/vista/run-kart-game.py', 'self-test']),
               ('d3d9-query', ['tests/vista/test-d3d9-pipeline.py', '--cpu']),
               ('d3d10-cpu', ['tests/vista/run-d3d10-compat.py', '--cpu']),
               ('shared-map-initialization', ['tests/vista/test-shared-map-initialization.py',
                                              '--evidence', str(output / 'shared-map')]),
               ('shader-cache', ['triton-umd/src/virtio/neptune/vista-d3d9/tests/native_compat/shader_cache_test.py'])]
    result += [(n, ['-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_' + n + '.py'])
               for n in ('run_vm', 'triton_trace', 'vista_control', 'vista_transfer', 'vista_uia_client', 'source_notices', 'sdk_headers', 'supermium_cef_source', 'supermium_cef_toolchain', 'vista_supermium_policy', 'sidebar')]
    result += [('sidebar-source', ['tools/verify_vista_sidebar_gadgets.py', '--source']),
               ('bootstrap-sources', ['tests/bootstrap/test_sources.py']),
               ('bootstrap-qemu-sources', ['tests/bootstrap/test_qemu_sources.py'])]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--list', action='store_true', help='Print the CPU manifest without running tests')
    parser.add_argument('--output', type=Path, help='Fresh evidence directory (default test-artifacts/cpu-TIMESTAMP-PID)')
    parser.add_argument('--only', action='append', metavar='NAME', help='Run named CPU cases; report remains a subset')
    parser.add_argument('--timeout', type=float, default=180, help='Timeout per case in seconds')
    args = parser.parse_args()
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    output = (args.output or ROOT / 'test-artifacts' / f'cpu-{stamp}-{os.getpid()}').resolve()
    selected = cases(output)
    known = {name for name, _ in selected}
    if args.only:
        unknown = set(args.only) - known
        if unknown:
            parser.error('unknown CPU case(s): ' + ', '.join(sorted(unknown)))
        selected = [(n, c) for n, c in selected if n in args.only]
    if args.list:
        for name, command in selected:
            print(name + ': ' + ' '.join(command))
        return 0
    if not __debug__ or os.environ.get('PYTHONOPTIMIZE'):
        parser.error('Python assertions are required; unset PYTHONOPTIMIZE and omit -O')
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    missing = [name for name in ('clang', 'clang++', 'cc', 'c++', 'g++', 'pkg-config', 'qemu-img') if not shutil.which(name)]
    if missing:
        parser.error('missing CPU test tools: ' + ', '.join(missing) + '. See tests/vista/README.md.')
    check = subprocess.run(['pkg-config', '--exists', 'glib-2.0', 'epoxy', 'zlib'])
    if check.returncode:
        parser.error('missing GLib, libepoxy or zlib development metadata; install the development packages (no GPU is used)')
    if not (ROOT / 'triton-dxvk/include/native/directx/d3d11.h').is_file():
        parser.error('DXVK sources are missing; bootstrap the public sources first')
    for name, command in selected:
        source = ROOT / (('tests/test_' + name + '.py') if command[0] == '-m' else command[0])
        if not source.is_file():
            parser.error(f'missing declared CPU case: {source}')
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, PYTHONUNBUFFERED='1', LC_ALL='C',
               TRITON_TEST_ARTIFACTS=str(output / 'fixtures'))
    report = {'suite': 'public-cpu', 'subset': bool(args.only), 'gpu_executed': False,
              'vm_started': False, 'performance_proven': False, 'cases': []}
    for name, command in selected:
        cmd = [sys.executable, *command]
        started = time.monotonic()
        record = {'name': name, 'command': cmd, 'status': 'RUNNING'}
        report['cases'].append(record)
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print('RUN ' + name, flush=True)
        with (output / (name + '.log')).open('w') as log:
            process = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = process.wait(timeout=args.timeout)
                record.update(status='PASS' if code == 0 else 'FAIL', exit_code=code)
            except KeyboardInterrupt:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                record.update(status='INTERRUPTED', exit_code=process.returncode)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                record.update(status='TIMEOUT', exit_code=process.returncode)
        record['elapsed_seconds'] = round(time.monotonic() - started, 3)
        record['log_sha256'] = hashlib.sha256((output / (name + '.log')).read_bytes()).hexdigest()
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print(record['status'] + ' ' + name, flush=True)
        if record['status'] == 'INTERRUPTED':
            print('CPU suite interrupted; child process group stopped. Evidence: ' + str(output))
            return 130
    report['passed'] = all(r['status'] == 'PASS' for r in report['cases'])
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    failed = [r['name'] for r in report['cases'] if r['status'] != 'PASS']
    print(f"{len(selected) - len(failed)}/{len(selected)} CPU cases passed. Evidence: {output}")
    if failed:
        print('Failed: ' + ', '.join(failed))
    print('Native GPU, Windows ABI/build, installed Vista applications and actual QEMU-window acceptance remain separate.')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
