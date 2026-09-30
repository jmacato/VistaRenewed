#!/usr/bin/env python3
"""Exercise actual default startup, without substituting about:blank."""
import re
import time
import uuid
from vista_mshtml_install import require_guest
from vista_mshtml_native_navigation_check import health, wait_title
from vista_mshtml_navigation_exercise import Qmp, type_text, capture


def main():
    tasks = require_guest('tasklist /FI "IMAGENAME eq iexplore.exe" /FO CSV /NH', quiet=True)
    if re.search(r'"iexplore\.exe"', tasks, re.I):
        raise RuntimeError('close existing IE before a fresh-start check')
    identity = re.search(r'(?im)^([a-z0-9_.-]+\\[a-z0-9_.-]+)\r?$',
                         require_guest('whoami', quiet=True)).group(1)
    for attempt in range(2):
        task = 'TritonFreshIe' + uuid.uuid4().hex
        pid = None
        rect = None
        require_guest(f'schtasks /create /tn {task} /tr "C:\\Windows\\System32\\cmd.exe /c start iexplore" '
                      f'/sc ONCE /st 23:59 /ru "{identity}" /it /f', user=False, quiet=True)
        try:
            require_guest(f'schtasks /run /tn {task}', user=False, quiet=True)
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                try:
                    state = health()
                    pid = state[0][0]
                    rect = state[2]
                    if state[3] and len(state[4]) == 1:
                        break
                except RuntimeError:
                    pass
                time.sleep(.3)
            else:
                raise RuntimeError('default startup never activated an embedded page')
            identity_hwnd, _, rect, _, apps = state
            with Qmp() as q:
                q.click(max(0, rect[0]) + 300, rect[1] + 44)
                q.key('ctrl', 'a'); type_text(q, 'https://example.org/'); q.key('ret')
            state = wait_title('Example Domain', identity_hwnd)
            if state[4] != apps:
                raise RuntimeError('startup recovery replaced the engine window')
            print(f'FRESH DEFAULT LAUNCH {attempt + 1} AND ADDRESS ENTRY VERIFIED', flush=True)
        except Exception:
            capture('fresh-start-failure')
            raise
        finally:
            require_guest(f'schtasks /delete /tn {task} /f', user=False, quiet=True)
            if pid and rect:
                with Qmp() as q:
                    q.click(rect[0] + 300, rect[1] + 44)
                    q.key('alt', 'f4')
                time.sleep(2)
                tasks = require_guest(f'tasklist /FI "PID eq {pid}" /FO CSV /NH', quiet=True)
                if re.search(r'"iexplore\.exe"', tasks, re.I):
                    require_guest(f'taskkill /PID {pid} /F', quiet=True)
                    raise RuntimeError('fresh IE did not close normally')
    print('MSHTML FRESH DEFAULT START PASSED')


if __name__ == '__main__':
    main()
