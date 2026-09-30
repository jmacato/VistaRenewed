#!/usr/bin/env python3
"""Transactional x86/x64 MSHTML shim deployment for the Vista VM."""
import argparse
import contextlib
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import time

import vista_control
import vista_transfer
from vista_supermium_policy import DURABLE, Policy, policy_lock

ROOT = Path(__file__).resolve().parents[1]
RECEIPT = ROOT / 'build' / 'vista-mshtml-install-receipt.json'
CLSID = '{25336920-03F9-11CF-8FD0-00AA00686F13}'
KEY = rf'HKCU\Software\Classes\CLSID\{CLSID}\InprocServer32'
OWNER = 'triton-mshtml-durable-v1'
REMOTE_ROOT = r'C:\TritonSupermiumBridge\versions'
ARTIFACTS = {
    '32': ROOT / 'build' / 'triton-mshtml-x86.dll',
    '64': ROOT / 'build' / 'triton-mshtml-x64.dll',
}
HASH_LOCAL = ROOT / 'build' / 'vista-mshtml-hash.exe'
HASH_REMOTE = r'C:\TritonSupermiumBridge\vista-mshtml-hash.exe'
HASH64_LOCAL = ROOT / 'build' / 'vista-mshtml-hash64.exe'
HASH64_REMOTE = r'C:\TritonSupermiumBridge\vista-mshtml-hash64.exe'


class InstallError(RuntimeError):
    pass


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def run_guest(command, *, user=True, timeout=120, quiet=False):
    data = io.BytesIO()
    capture = io.TextIOWrapper(data, encoding='utf-8')
    try:
        argv = ['run'] + (['--user'] if user else []) + ['--timeout', str(timeout), command]
        with contextlib.redirect_stdout(capture), contextlib.redirect_stderr(capture):
            code = vista_control.main(argv)
        capture.flush()
        output = data.getvalue().decode('utf-8', errors='replace')
    finally:
        capture.detach()
    if not quiet:
        print(output, end='')
    return code, output


def require_guest(command, **kwargs):
    code, output = run_guest(command, **kwargs)
    if code:
        raise InstallError(f'guest command failed ({code}): {command}')
    return output


def registration(view):
    code, output = run_guest(f'reg query "{KEY}" /reg:{view}', quiet=True)
    if code == 1 and 'unable to find the specified registry key or value' in output.lower():
        return None
    if code:
        raise InstallError(f'cannot establish {view}-bit registration state')
    values = {}
    for line in output.splitlines():
        match = re.fullmatch(r'\s+(\(Default\)|[A-Za-z0-9_.-]+)\s+(REG_\w+)\s+(.*)', line)
        if match:
            if match[1] in values:
                raise InstallError('duplicate registry value')
            values[match[1]] = [match[2], match[3].rstrip()]
    if not values:
        raise InstallError(f'unrecognized {view}-bit registration output')
    return values


def expected(view, item):
    return {
        '(Default)': ['REG_SZ', item['remote']],
        'ThreadingModel': ['REG_SZ', 'Apartment'],
        'TritonOwner': ['REG_SZ', OWNER],
        'TritonVersion': ['REG_SZ', item['sha256'][:16]],
        'TritonSHA256': ['REG_SZ', item['sha256']],
        'TritonArchitecture': ['REG_SZ', 'x86' if view == '32' else 'x64'],
    }


def live_hosts():
    output = require_guest(
        'tasklist /FI "IMAGENAME eq iexplore.exe" /FO CSV /NH & '
        'tasklist /FI "IMAGENAME eq mshta.exe" /FO CSV /NH & '
        'tasklist /FI "IMAGENAME eq chrome.exe" /FO CSV /NH', quiet=True)
    return [name.lower() for name in re.findall(r'"(iexplore|mshta|chrome)\.exe"', output, re.I)]


def loaded_payloads():
    receipt = load_receipt()
    if not receipt:
        return []
    loaded = []
    for view, item in receipt['items'].items():
        helper = HASH_REMOTE if view == '32' else HASH64_REMOTE
        code, output = run_guest(
            f'"{helper}" --module-in-use "{item["remote"]}"',
            user=False, quiet=True)
        if code == 0:
            match = re.search(r'pid=([0-9]+) image=([^\r\n]+)', output, re.I)
            detail = match.group(0) if match else 'mapped by an unidentified process'
            loaded.append((view, item['remote'], detail))
        elif code != 3:
            raise InstallError(f'cannot establish {view}-bit payload module usage')
    return loaded


def require_quiet_desktop():
    running = live_hosts()
    payloads = loaded_payloads()
    if running or payloads:
        names = sorted(set(running))
        names.extend(f'{view}-bit payload ({detail})' for view, _, detail in payloads)
        raise InstallError('refusing live MSHTML replacement while these hosts run: ' +
                           ', '.join(names))


def remote_hash(path):
    code, output = run_guest(f'"{HASH_REMOTE}" "{path}"', user=False, quiet=True)
    if code:
        return None
    values = [re.sub(r'\s', '', line).lower() for line in output.splitlines()]
    return next((value for value in values if re.fullmatch(r'[0-9a-f]{64}', value)), None)


def save_receipt(receipt):
    RECEIPT.parent.mkdir(exist_ok=True)
    temporary = RECEIPT.with_suffix('.json.new')
    temporary.write_text(json.dumps(receipt, indent=2) + '\n')
    temporary.replace(RECEIPT)


def load_receipt():
    if not RECEIPT.exists():
        return None
    data = json.loads(RECEIPT.read_text())
    if data.get('owner') != OWNER or data.get('version') != 1:
        raise InstallError('foreign or malformed installer receipt')
    return data


def build_payloads(container):
    subprocess.run([
        sys.executable, str(ROOT / 'scripts/build_mshtml.py'), '--both'
    ], cwd=ROOT, check=True)
    subprocess.run([
        str(ROOT / 'scripts/dev-container.sh'), 'run',
        'i686-w64-mingw32-gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-static', '-municode', '-Wl,--subsystem,console:6.0',
        'tools/vista_mshtml_hash.c', '-o', str(HASH_LOCAL.relative_to(ROOT)),
        '-ladvapi32', '-lole32'
    ], cwd=ROOT, check=True)
    subprocess.run([
        str(ROOT / 'scripts/dev-container.sh'), 'run',
        'x86_64-w64-mingw32-gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-static', '-municode', '-Wl,--subsystem,console:6.0',
        'tools/vista_mshtml_hash.c', '-o', str(HASH64_LOCAL.relative_to(ROOT)),
        '-ladvapi32', '-lole32'
    ], cwd=ROOT, check=True)
    vista_transfer.upload(HASH_LOCAL, HASH_REMOTE, container)
    vista_transfer.upload(HASH64_LOCAL, HASH64_REMOTE, container)


def launch_live_host(view, item):
    """Create HTMLDocument outside the serialized control service job."""
    helper = HASH_REMOTE if view == '32' else HASH64_REMOTE
    if any(found_view == view for found_view, _, _ in loaded_payloads()):
        return None
    _, identity_output = run_guest('whoami', quiet=True)
    match = re.search(r'(?im)^([a-z0-9_.-]+\\[a-z0-9_.-]+)\r?$', identity_output)
    if not match:
        raise InstallError('cannot identify the interactive user for live-host check')
    identity = match.group(1)
    task = f'TritonMshtmlLiveHost{view}'
    image = 'vista-mshtml-hash.exe' if view == '32' else 'vista-mshtml-hash64.exe'
    command = (f'schtasks /end /tn {task} >NUL 2>&1 & '
               f'schtasks /delete /tn {task} /f >NUL 2>&1 & '
               f'schtasks /create /tn {task} /tr "{helper} --hold-com 60000" '
               f'/sc ONCE /st 23:59 /ru "{identity}" /it /f & '
               f'schtasks /run /tn {task}')
    for _ in range(3):
        require_guest(command, user=False, quiet=True)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if any(found_view == view for found_view, _, _ in loaded_payloads()):
                return image, task
            time.sleep(.5)
    run_guest(f'schtasks /delete /tn {task} /f', user=False, quiet=True)
    raise InstallError(f'live-host positive control did not map {view}-bit payload')


def make_items():
    items = {}
    for view, local in ARTIFACTS.items():
        sha = digest(local)
        arch = 'x86' if view == '32' else 'x64'
        items[view] = {
            'local': str(local),
            'sha256': sha,
            'remote': rf'{REMOTE_ROOT}\triton-mshtml-{arch}-{sha[:16]}.dll',
            'preexisting': False,
        }
    return items


def install(container, desired_items=None):
    receipt = load_receipt()
    if receipt:
        for view, item in receipt['items'].items():
            if registration(view) != expected(view, item):
                raise InstallError('owned registration drifted; refusing repair')
            if remote_hash(item['remote']) != item['sha256']:
                raise InstallError('owned payload drifted; refusing repair')
        desired_items = desired_items or make_items()
        if all(receipt['items'][view]['sha256'] == desired_items[view]['sha256']
               for view in ('32', '64')):
            if Policy().status():
                raise InstallError('durable Supermium launch policy is absent')
            print('MSHTML DURABLE INSTALL ALREADY CURRENT')
            return receipt
        require_quiet_desktop()
        rollback(container)

    require_quiet_desktop()

    prior = {view: registration(view) for view in ('32', '64')}
    if any(value is not None for value in prior.values()):
        raise InstallError('foreign or unreceipted HTMLDocument registration exists; preserved unchanged')
    policy = Policy()
    policy_preexisting = policy.status() == 0
    policy.install()
    items = desired_items or make_items()
    receipt = {
        'version': 1,
        'owner': OWNER,
        'state': 'installing',
        'prior': prior,
        'policy_preexisting': policy_preexisting,
        'items': items,
    }
    save_receipt(receipt)
    try:
        require_guest(f'mkdir "{REMOTE_ROOT}" 2>NUL & exit /b 0', user=False, quiet=True)
        for item in items.values():
            found = remote_hash(item['remote'])
            if found and found != item['sha256']:
                raise InstallError('versioned destination contains foreign bytes')
            item['preexisting'] = found == item['sha256']
            save_receipt(receipt)
            if not found:
                vista_transfer.upload(Path(item['local']), item['remote'], container)
            if remote_hash(item['remote']) != item['sha256']:
                raise InstallError('guest artifact hash mismatch after transfer')
        for view, item in items.items():
            values = expected(view, item)
            order = ('TritonOwner', 'TritonVersion', 'TritonSHA256',
                     'TritonArchitecture', '(Default)', 'ThreadingModel')
            for name in order:
                kind, value = values[name]
                selector = '/ve' if name == '(Default)' else f'/v {name}'
                command = f'reg add "{KEY}" {selector} /t {kind} /d "{value}" /f /reg:{view}'
                require_guest(command, quiet=True)
            if registration(view) != values:
                raise InstallError(f'{view}-bit registration read-back mismatch')
        receipt['state'] = 'installed'
        save_receipt(receipt)
        print('MSHTML DURABLE X86 X64 INSTALL VERIFIED')
        return receipt
    except BaseException:
        rollback(container, allow_installing=True)
        raise


def rollback(container, allow_installing=False):
    running = live_hosts()
    if running:
        names = ', '.join(sorted(set(running)))
        raise InstallError(f'refusing rollback while interactive browser hosts run: {names}')
    receipt = load_receipt()
    if not receipt:
        print('MSHTML DURABLE INSTALL ABSENT')
        return
    if receipt.get('state') not in ('installed', 'rollback-pending-reboot') and not allow_installing:
        raise InstallError('partial install requires verify/recovery')
    pending_reboot = False
    for view, item in receipt['items'].items():
        current = registration(view)
        if current is not None and current != expected(view, item):
            raise InstallError(f'{view}-bit registration changed; preserving all state')
    for view, item in receipt['items'].items():
        if registration(view) is not None:
            require_guest(f'reg delete "{KEY}" /f /reg:{view}', quiet=True)
        if receipt['prior'][view] is not None:
            raise InstallError('unexpected nonempty prior registration in owned receipt')
        if not item.get('preexisting') and remote_hash(item['remote']) == item['sha256']:
            require_guest(f'del /f /q "{item["remote"]}"', user=False, quiet=True)
        if registration(view) is not None:
            raise InstallError(f'{view}-bit registration rollback failed')
        if not item.get('preexisting') and remote_hash(item['remote']) is not None:
            command = f'"{HASH64_REMOTE}" --delete-reboot "{item["remote"]}"'
            require_guest(command, user=False, quiet=True)
            pending_reboot = True
    if pending_reboot:
        receipt['state'] = 'rollback-pending-reboot'
        save_receipt(receipt)
        reboot_and_wait()
        for item in receipt['items'].values():
            if not item.get('preexisting') and remote_hash(item['remote']) is not None:
                raise InstallError('scheduled payload deletion did not complete after reboot')
    archive = RECEIPT.with_name('vista-mshtml-install-last-rollback.json')
    if archive.exists():
        archive.unlink()
    RECEIPT.replace(archive)
    if not receipt.get('policy_preexisting', True):
        Policy().remove(DURABLE)
    print('MSHTML DURABLE INSTALL ROLLED BACK EXACTLY')


def status():
    receipt = load_receipt()
    if not receipt or receipt.get('state') != 'installed':
        raise InstallError('durable installation receipt is absent')
    for view, item in receipt['items'].items():
        if registration(view) != expected(view, item):
            raise InstallError(f'{view}-bit registration does not match receipt')
        if remote_hash(item['remote']) != item['sha256']:
            raise InstallError(f'{view}-bit payload hash does not match receipt')
    if Policy().status():
        raise InstallError('durable Supermium launch policy is absent')
    print('MSHTML DURABLE X86 X64 STATUS VERIFIED')


def reboot_and_wait():
    # A QMP system_reset loses recently modified user-hive and Session Manager
    # state on Vista. Ask Windows to reboot so registry deletion and pending
    # file renames are durably flushed before the kernel goes down.
    code = vista_control.main([
        'run', '--timeout', '30', '--detach', r'shutdown.exe /r /t 0 /f'
    ])
    if code:
        raise InstallError('could not request a graceful Vista reboot')
    deadline = time.monotonic() + 120
    went_down = False
    while time.monotonic() < deadline:
        time.sleep(2)
        try:
            with vista_control.Client(timeout=5) as client:
                reply = client.checked('PING')
            if went_down and reply['status'] == 'OK':
                print('VISTA CONTROL RESTORED AFTER REBOOT')
                return
        except (OSError, vista_control.ControlError):
            went_down = True
    raise InstallError('Vista control service did not return after reboot')


def verify(container):
    build_payloads(container)
    if load_receipt():
        rollback(container)
    if registration('32') is not None or registration('64') is not None:
        raise InstallError('verification found a foreign registration; preserved unchanged')

    foreign = r'C:\Windows\System32\foreign-mshtml-test.dll'
    command = f'reg add "{KEY}" /ve /t REG_SZ /d "{foreign}" /f /reg:32'
    require_guest(command, quiet=True)
    before = registration('32')
    try:
        try:
            install(container)
            raise InstallError('installer accepted a foreign registration')
        except InstallError as error:
            if 'foreign or unreceipted' not in str(error):
                raise
        if registration('32') != before:
            raise InstallError('foreign registration changed during refusal')
    finally:
        require_guest(f'reg delete "{KEY}" /f /reg:32', quiet=True)

    install(container)
    install(container)

    receipt = load_receipt()
    for view, item in receipt['items'].items():
        launched = launch_live_host(view, item)
        desired = make_items()
        desired[view]['sha256'] = '0' * 64
        desired[view]['remote'] = rf'{REMOTE_ROOT}\replacement-refusal-{view}.dll'
        try:
            try:
                install(container, desired)
                raise InstallError('installer did not refuse a live host')
            except InstallError as error:
                if 'refusing live MSHTML replacement' not in str(error):
                    raise
        finally:
            if launched:
                image, task = launched
                require_guest(f'taskkill /IM {image} /F', quiet=True)
                require_guest(f'schtasks /delete /tn {task} /f', user=False, quiet=True)

    reboot_and_wait()
    status()
    rollback(container)
    if registration('32') is not None or registration('64') is not None:
        raise InstallError('rollback did not restore absent prior registrations')
    install(container)
    reboot_and_wait()
    status()
    print('MSHTML DURABLE INSTALL ROLLBACK AND REBOOT PASSED')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['install', 'status', 'uninstall', 'verify'])
    parser.add_argument('--vm', default='triton-vista-x64-normal')
    args = parser.parse_args()
    subprocess.run(['podman', 'inspect', args.vm], stdout=subprocess.DEVNULL, check=True)
    with policy_lock():
        if args.action == 'install':
            build_payloads(args.vm)
            install(args.vm)
        elif args.action == 'status':
            status()
        elif args.action == 'uninstall':
            rollback(args.vm)
        else:
            verify(args.vm)


if __name__ == '__main__':
    try:
        main()
    except (InstallError, RuntimeError, vista_control.ControlError, OSError, ValueError,
            subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print(f'vista-mshtml-install: {error}', file=sys.stderr)
        raise SystemExit(1)
