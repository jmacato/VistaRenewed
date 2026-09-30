#!/usr/bin/env python3
"""Install/status/uninstall Supermium's low-integrity IE launch policy in the VM.

Run on the host. Uses the existing private SYSTEM control service; no guest
Python, zone changes, COM activation override, or medium-integrity launch.
"""
import argparse
import contextlib
import fcntl
import io
import os
import re
import signal

import vista_control

KEY = r'HKLM\Software\Microsoft\Internet Explorer\Low Rights\ElevationPolicy\{08A1D321-9C62-4FC8-84EF-7A5F8BF3C147}'
FULL_KEY = KEY.replace('HKLM', 'HKEY_LOCAL_MACHINE', 1)
OWNER = 'TritonPolicyOwner'
DURABLE = 'supermium-low-integrity-v1'
TEMPORARY = 'supermium-low-integrity-temporary-v1'
USER_ROOT = r'HKCU\Software\Microsoft\Internet Explorer\Low Rights\ElevationPolicy'
USER_FULL_ROOT = USER_ROOT.replace('HKCU', 'HKEY_CURRENT_USER', 1)
BASE = {'AppName': ('REG_SZ', 'chrome.exe'),
        'AppPath': ('REG_SZ', r'C:\TritonSupermium'),
        'Policy': ('REG_DWORD', '0x1')}


def run(command, user=False, timeout=120):
    data = io.BytesIO()
    capture = io.TextIOWrapper(data, encoding='utf-8')
    try:
        with contextlib.redirect_stdout(capture):
            code = vista_control.main(['run', *(['--user'] if user else []),
                                       '--timeout', str(timeout), command])
        capture.flush()
        output = data.getvalue().decode('utf-8', errors='replace')
    finally:
        capture.detach()
    print(output, end='', flush=True)
    return code, output


def require(command, **kwargs):
    code, output = run(command, **kwargs)
    if code:
        raise RuntimeError(f'guest command failed: exit={code}')
    return output


def interrupt(*_):
    raise KeyboardInterrupt


@contextlib.contextmanager
def policy_lock():
    """Serialize whole install/exercise workflows, not just serial requests."""
    path = str(vista_control.DEFAULT_SOCKET) + '.supermium-policy-lock'
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('another Supermium policy operation is active') from None
        yield
    finally:
        os.close(fd)


def expected(owner):
    return {**BASE, OWNER: ('REG_SZ', owner)}


def parse_query(code, output):
    if code:
        if code == 1 and 'unable to find the specified registry key or value' in output:
            return None
        raise RuntimeError('policy query failed; absence is not established')
    values = {}
    seen_key = False
    for line in output.splitlines():
        if line.startswith('HKEY_'):
            if seen_key or line.strip().casefold() != FULL_KEY.casefold():
                raise RuntimeError('unexpected policy subkey; refusing to modify it')
            seen_key = True
        elif seen_key and line.strip():
            match = re.fullmatch(r'\s+(\S+)\s+(REG_\w+)\s+(.*)', line)
            if not match or match[1] in values:
                raise RuntimeError('unrecognized registry values; refusing to modify them')
            values[match[1]] = (match[2], match[3].rstrip())
    if not seen_key:
        raise RuntimeError('policy query did not identify the requested key')
    return values


def user_conflicts(code, output):
    """A remembered per-user prompt choice can override machine policy."""
    if code:
        if code == 1 and 'unable to find the specified registry key or value' in output:
            return []
        raise RuntimeError('per-user policy query failed; effective policy is unknown')
    entries = {}
    current = None
    for line in output.splitlines():
        if line.startswith('HKEY_'):
            current = line.strip()
            if (current.casefold() != USER_FULL_ROOT.casefold() and
                    not current.casefold().startswith(USER_FULL_ROOT.casefold() + '\\')):
                raise RuntimeError('unexpected per-user registry key')
            if current in entries:
                raise RuntimeError('duplicate per-user registry key')
            entries[current] = {}
        elif current and line.strip():
            match = re.fullmatch(r'\s+(\S+)\s+(REG_\w+)\s+(.*)', line)
            if not match or match[1] in entries[current]:
                raise RuntimeError('unrecognized per-user registry values')
            entries[current][match[1]] = (match[2], match[3].rstrip())
    if not entries:
        raise RuntimeError('per-user policy query did not identify a key')
    conflicts = []
    for key, values in entries.items():
        name = values.get('AppName', ('', ''))
        path = values.get('AppPath', ('', ''))
        if (name[0] == path[0] == 'REG_SZ' and name[1].casefold() == 'chrome.exe' and
                path[1].rstrip('\\/').casefold() == r'C:\TritonSupermium'.casefold() and
                values.get('Policy') != ('REG_DWORD', '0x1')):
            conflicts.append(key)
    return conflicts


class Policy:
    """Caller holds policy_lock while making any changes."""
    def __init__(self, runner=run):
        self.run = runner

    def require(self, command):
        code, output = self.run(command)
        if code:
            raise RuntimeError(f'policy command failed: exit={code}')
        return output

    def read(self):
        return parse_query(*self.run(f'reg query "{KEY}" /s /reg:32'))

    def validate_user_policy(self):
        conflicts = user_conflicts(*self.run(f'reg query "{USER_ROOT}" /s /reg:32', user=True))
        if conflicts:
            raise RuntimeError('conflicting per-user Supermium launch policy; left unchanged: ' +
                               ', '.join(conflicts))

    def create(self, owner):
        if self.read() is not None:
            raise RuntimeError('policy already exists; refusing to overwrite it')
        self.require(r'if exist C:\TritonSupermium\chrome.exe (echo SUPERMIUM_EXECUTABLE_PRESENT) else (exit /b 1)')
        wanted = expected(owner)
        try:
            # Write the ownership marker first and the effective policy last.
            for name in (OWNER, 'AppName', 'AppPath', 'Policy'):
                kind, value = wanted[name]
                self.require(f'reg add "{KEY}" /v {name} /t {kind} /d "{value}" /f /reg:32')
            if self.read() != wanted:
                raise RuntimeError('installed policy read-back does not match')
        except BaseException:
            current = self.read()
            # Roll back only our verified partial write. Preserve any foreign
            # additions or concurrent edits instead of deleting them.
            if current is not None:
                if (current.get(OWNER) != wanted[OWNER] or
                        any(wanted.get(name) != value for name, value in current.items())):
                    raise RuntimeError('policy changed during install; preserved for inspection')
                self.require(f'reg delete "{KEY}" /f /reg:32')
                if self.read() is not None:
                    raise RuntimeError('partial policy rollback could not be verified')
            raise

    def install(self):
        self.validate_user_policy()
        current = self.read()
        if current == expected(DURABLE):
            print('SUPERMIUM LOW-INTEGRITY POLICY ALREADY INSTALLED')
            return
        if current is not None:
            raise RuntimeError('foreign, temporary, or changed policy exists; refusing to overwrite it')
        self.create(DURABLE)
        print('SUPERMIUM LOW-INTEGRITY POLICY INSTALLED')

    def remove(self, owner):
        current = self.read()
        if current is None:
            return
        if current != expected(owner):
            raise RuntimeError('policy is not an exact owned entry; refusing to delete it')
        self.require(f'reg delete "{KEY}" /f /reg:32')
        if self.read() is not None:
            raise RuntimeError('policy removal could not be verified')

    def status(self):
        self.validate_user_policy()
        current = self.read()
        if current is None:
            print('SUPERMIUM LOW-INTEGRITY POLICY ABSENT')
            return 1
        if current != expected(DURABLE):
            raise RuntimeError('policy is foreign, temporary, or changed')
        print('SUPERMIUM LOW-INTEGRITY POLICY INSTALLED')
        return 0


@contextlib.contextmanager
def exercise_policy(policy=None):
    """Borrow a durable entry, or own and clean up a temporary one."""
    policy = policy or Policy()
    policy.validate_user_policy()
    current = policy.read()
    durable = current == expected(DURABLE)
    if current is not None and not durable:
        raise RuntimeError('unexpected policy exists; refusing the exercise')
    if not durable:
        policy.create(TEMPORARY)
    try:
        yield durable
    finally:
        if durable:
            if policy.read() != current:
                raise RuntimeError('durable policy changed during exercise; not overwritten')
            print('DURABLE_LOW_POLICY_PRESERVED')
        else:
            policy.remove(TEMPORARY)
            print('TEMPORARY_LOW_POLICY_REMOVED')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['install', 'status', 'uninstall'])
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, interrupt)
    with policy_lock():
        policy = Policy()
        if args.action == 'status':
            return policy.status()
        if args.action == 'install':
            policy.install()
        else:
            policy.remove(DURABLE)
            print('SUPERMIUM LOW-INTEGRITY POLICY UNINSTALLED')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
