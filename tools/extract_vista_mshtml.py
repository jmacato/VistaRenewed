#!/usr/bin/env python3
"""Copy fixed guest DLLs over the private QEMU gateway into the RE workspace."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import select
import subprocess
import sys
import threading

import vista_control

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'build/mshtml-native-re'
FILES = {'mshtml-x86.dll': r'C:\Windows\SysWOW64\mshtml.dll',
         'ieframe-x86.dll': r'C:\Windows\SysWOW64\ieframe.dll',
         'urlmon-x86.dll': r'C:\Windows\SysWOW64\urlmon.dll',
         'mshtml-x64.dll': r'C:\Windows\System32\mshtml.dll'}


def serve(token):
    OUTPUT.mkdir(parents=True, exist_ok=True)
    if any((OUTPUT / name).exists() for name in (*FILES, 'provenance.json')):
        raise RuntimeError('extraction artifacts already exist; refusing overwrite')
    records = {}
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            parts = self.path.split('/')
            if len(parts) != 3 or parts[1] != token or parts[2] not in FILES:
                self.send_error(404)
                return
            name = parts[2]
            try:
                size = int(self.headers.get('Content-Length', '-1'))
            except ValueError:
                size = -1
            if not 512 <= size <= 64 * 1024 * 1024 or self.headers.get('X-Source-Path') != FILES[name]:
                self.send_error(400)
                return
            self.connection.settimeout(60)
            with lock:
                if name in records or (OUTPUT / name).exists():
                    self.send_error(409)
                    return
                data = self.rfile.read(size)
                if len(data) != size or data[:2] != b'MZ':
                    self.send_error(400)
                    return
                with (OUTPUT / name).open('xb') as target:
                    target.write(data)
                records[name] = {'source': FILES[name], 'version': self.headers.get('X-Source-Version'),
                                 'bytes': size, 'sha256': hashlib.sha256(data).hexdigest()}
                self.send_response(201)
                self.send_header('Content-Length', '0')
                self.end_headers()

        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    print(json.dumps({'port': server.server_port}), flush=True)
    try:
        readable, _, _ = select.select([sys.stdin], [], [], 600)
        if readable:
            sys.stdin.readline()
    finally:
        server.shutdown()
        server.server_close()
    if set(records) != set(FILES):
        raise RuntimeError(f'incomplete extraction: {list(records)}')
    with (OUTPUT / 'provenance.json').open('x') as target:
        json.dump(records, target, indent=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serve')
    args = parser.parse_args()
    if args.serve:
        serve(args.serve)
        return
    guest_script = r'C:\TritonSupermiumBridge\vista-mshtml-export.vbs'
    if vista_control.main(['put', str(ROOT / 'tools/vista_mshtml_export.vbs'), guest_script]):
        raise RuntimeError('could not stage the fixed export script')
    token = secrets.token_hex(32)
    process = subprocess.Popen(['podman', 'exec', '-i', 'triton-vista-x64-normal',
                                'python3', str(Path(__file__).resolve()), '--serve', token],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        ready = json.loads(process.stdout.readline())
        command = (r'C:\Windows\System32\cscript.exe //nologo ' + guest_script +
                   f' http://10.0.2.2:{ready["port"]}/{token}')
        if vista_control.main(['run', '--timeout', '180', command]):
            raise RuntimeError('guest extraction failed')
    finally:
        process.stdin.close()
        try:
            result = process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)
            raise RuntimeError('extraction server did not shut down') from None
    if result:
        raise RuntimeError('extraction server did not finish successfully')
    print('PRIVATE TCP NATIVE BINARY EXTRACTION COMPLETE')


if __name__ == '__main__':
    main()
