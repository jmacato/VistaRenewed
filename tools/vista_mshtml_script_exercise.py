#!/usr/bin/env python3
"""Run a real WSH htmlfile consumer with a reversible 32-bit per-user override."""
import signal
from vista_low_launch_exercise import interrupt, require, run

KEY = r'HKCU\Software\Classes\CLSID\{25336920-03F9-11CF-8FD0-00AA00686F13}\InprocServer32'
STOCK = r'reg query "HKCR\CLSID\{25336920-03F9-11CF-8FD0-00AA00686F13}\InprocServer32" /ve /reg:32'


def absent():
    code, output = run(f'reg query "{KEY}" /reg:32', user=True)
    return code == 1 and 'unable to find the specified registry key or value' in output


def main():
    signal.signal(signal.SIGTERM, interrupt)
    if not absent():
        raise RuntimeError('existing override not confirmed absent; refusing to overwrite')
    before = require(STOCK, user=True)
    owned = False
    try:
        require(f'reg add "{KEY}" /ve /t REG_SZ /d '
                r'C:\TritonSupermiumBridge\triton-ie7-mshtml-activation-probe.dll /f /reg:32', user=True)
        owned = True
        output = require(r'C:\Windows\SysWOW64\cscript.exe //nologo C:\TritonSupermiumBridge\mshtml-content.js',
                         user=True, timeout=90)
        if 'WINDOWS SCRIPT HOST MSHTML CONTENT PASSED' not in output:
            raise RuntimeError('native script host success marker missing')
    finally:
        if owned:
            require(f'reg delete "{KEY}" /f /reg:32', user=True)
        if not absent() or require(STOCK, user=True) != before:
            raise RuntimeError('activation restoration could not be verified')
    print('WINDOWS SCRIPT HOST MSHTML RESTORED AND PASSED')


if __name__ == '__main__':
    main()
