#!/usr/bin/env python3
"""Exercise the installed shim through native IE input and native title/HWND observations."""
import re
import subprocess
import time
import uuid

from vista_mshtml_install import require_guest
from vista_mshtml_navigation_exercise import Qmp, type_text, capture

owned_pid = None


def health():
    output = require_guest(r'C:\TritonSupermiumBridge\ie-window-health.exe', quiet=True)
    frames = re.findall(r'IE_HEALTH pid=(\d+) hwnd=(\w+) responsive=(\d)', output)
    if len(frames) != 1 or frames[0][2] != '1':
        raise RuntimeError('expected exactly one responsive IE frame: ' + output)
    title = re.search(r'IE_TITLE=([^\r\n]*)', output)
    rect = re.search(r'IE_RECT=(-?\d+),(-?\d+),(-?\d+),(-?\d+)', output)
    viewport = re.search(r'IE_VIEWPORT_RECT=(-?\d+),(-?\d+),(-?\d+),(-?\d+)', output)
    apps = re.findall(r'EMBED_CHILD hwnd=(\w+) class=Chrome_WidgetWin_1 pid=(\d+)', output)
    return frames[0][:2], title.group(1) if title else '', tuple(map(int, rect.groups())), \
        tuple(map(int, viewport.groups())) if viewport else None, apps


def wait_title(wanted, identity=None):
    deadline = time.monotonic() + 30
    matched_since = None
    while time.monotonic() < deadline:
        state = health()
        if identity and state[0] != identity:
            raise RuntimeError('navigation changed native IE window identity')
        if state[1].startswith(wanted + ' - '):
            if matched_since is None:
                matched_since = time.monotonic()
            if time.monotonic() - matched_since >= 1:
                print('NATIVE IE TITLE VERIFIED ' + state[1], flush=True)
                return state
        else:
            matched_since = None
        time.sleep(.3)
    raise RuntimeError('IE did not settle at ' + wanted + ': ' + repr(state))


def main():
    global owned_pid
    subprocess.run(['podman', 'exec', 'triton-vista-x64-normal', 'python3', '-c',
                    "import urllib.request; "
                    "[urllib.request.urlopen('http://127.0.0.1:%d/a'%p,timeout=3).read() "
                    "for p in (18767,18768)]"], check=True)
    tasks = require_guest('tasklist /FI "IMAGENAME eq iexplore.exe" /FO CSV /NH', quiet=True)
    if not re.search(r'"iexplore\.exe"', tasks, re.I):
        identity_output = require_guest('whoami', quiet=True)
        identity = re.search(r'(?im)^([a-z0-9_.-]+\\[a-z0-9_.-]+)\r?$', identity_output)
        if not identity:
            raise RuntimeError('cannot identify interactive desktop user')
        task = 'TritonNativeNavigation' + uuid.uuid4().hex
        require_guest(f'schtasks /create /tn {task} /tr "C:\\Windows\\System32\\cmd.exe /c start iexplore about:blank" '
                      f'/sc ONCE /st 23:59 /ru "{identity.group(1)}" /it /f', user=False, quiet=True)
        try:
            require_guest(f'schtasks /run /tn {task}', user=False, quiet=True)
        finally:
            require_guest(f'schtasks /delete /tn {task} /f', user=False, quiet=True)
        deadline = time.monotonic() + 30
        while True:
            try:
                state = health()
                owned_pid = state[0][0]
                if state[3] and state[4]: break
            except RuntimeError:
                if time.monotonic() >= deadline: raise
            if time.monotonic() >= deadline: raise RuntimeError('IE did not embed its initial page')
            time.sleep(.3)
    else:
        state = health()
    identity, _, rect, viewport, apps = state
    if not viewport or len(apps) != 1:
        raise RuntimeError('expected an existing embedded IE page')
    time.sleep(1)  # Let initial view activation finish before testing input.
    with Qmp() as q:
        # Start with renderer focus to exercise the formerly swallowed shortcut.
        q.click(viewport[0] + 150, viewport[1] + 100)
        q.key('ctrl', 'l')
        time.sleep(.3)
        type_text(q, 'http://10.0.2.2:18767/a')
        q.key('ret')
    wait_title('Navigation A', identity)
    with Qmp() as q:
        q.click(rect[0] + 300, rect[1] + 44)
        q.key('ctrl', 'a'); type_text(q, 'http://10.0.2.2:18767/native-links'); q.key('ret')
    state = wait_title('Native Link Test', identity)
    viewport = state[3]
    with Qmp() as q:
        q.click(viewport[0] + 80, viewport[1] + 60)
    wait_title('Navigation B', identity)
    for url, title in [('http://10.0.2.2:18767/a', 'Navigation A'),
                       ('https://example.org/', 'Example Domain')]:
        with Qmp() as q:
            q.click(rect[0] + 300, rect[1] + 44)
            q.key('ctrl', 'a'); type_text(q, url); q.key('ret')
        wait_title(title, identity)
    with Qmp() as q:
        q.click(rect[0] + 300, rect[1] + 14)  # Activate IE if another desktop window took focus.
        q.click(rect[0] + 23, rect[1] + 44)
    wait_title('Navigation A', identity)
    with Qmp() as q:
        q.click(rect[0] + 300, rect[1] + 14)
        q.click(rect[0] + 50, rect[1] + 44)
    state = wait_title('Example Domain', identity)
    if state[4] != apps:
        raise RuntimeError('navigation replaced the Chromium window/context')
    print('MSHTML NATIVE IE ADDRESS AND HISTORY PASSED')


if __name__ == '__main__':
    try:
        main()
    except Exception:
        capture('check-failure')
        raise
    finally:
        if owned_pid:
            require_guest(f'taskkill /PID {owned_pid} /F', quiet=True)
