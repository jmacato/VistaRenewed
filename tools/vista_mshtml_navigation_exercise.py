#!/usr/bin/env python3
"""Visible IE7 address-bar exercise; keep the owned override while IE is alive.

start/stop are explicit reversible per-user registration operations. No zone or
launch-policy settings are modified. fixture binds only the QEMU loopback.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import threading
import time

from vista_supermium_policy import require, run, policy_lock

ROOT = Path(__file__).resolve().parents[1]
KEY = r'HKCU\Software\Classes\CLSID\{25336920-03F9-11CF-8FD0-00AA00686F13}\InprocServer32'
DLL = r'C:\TritonSupermiumBridge\triton-ie7-mshtml-activation-probe.dll'
OWNER = 'native-navigation-exercise-v1'
RECEIPT = ROOT / 'build/mshtml-navigation-exercise.json'


def ie_pids():
    output = require('tasklist /FI "IMAGENAME eq iexplore.exe" /FO CSV /NH', user=True)
    return [int(pid) for pid in re.findall(r'"iexplore.exe","(\d+)"', output, re.I)]


def registration():
    code, output = run(f'reg query "{KEY}" /reg:32', user=True)
    if code == 1 and 'unable to find the specified registry key or value' in output:
        return None
    if code:
        raise RuntimeError('registration state is unknown')
    values = {}
    for line in output.splitlines():
        match = re.fullmatch(r'\s+(\S+)\s+(REG_\w+)\s+(.*)', line)
        if match:
            values[match[1]] = (match[2], match[3])
    return values


def expected():
    return {'(Default)': ('REG_SZ', DLL), 'ThreadingModel': ('REG_SZ', 'Apartment'),
            'TritonOwner': ('REG_SZ', OWNER)}


def start():
    with policy_lock():
        if ie_pids():
            raise RuntimeError('close existing IE windows before starting the controlled exercise')
        current = registration()
        if current is not None and (current != expected() or not RECEIPT.exists()):
            raise RuntimeError('foreign or unreceipted override; refusing to overwrite')
        if current is None:
            # Receipt first, so interrupted installation is visible/recoverable.
            RECEIPT.write_text(json.dumps({'owner': OWNER, 'previous': None, 'dll': DLL}, indent=2) + '\n')
            require(f'reg add "{KEY}" /ve /t REG_SZ /d "{DLL}" /f /reg:32', user=True)
            require(f'reg add "{KEY}" /v ThreadingModel /t REG_SZ /d Apartment /f /reg:32', user=True)
            require(f'reg add "{KEY}" /v TritonOwner /t REG_SZ /d {OWNER} /f /reg:32', user=True)
        if registration() != expected():
            raise RuntimeError('installed override did not verify')
        # A child of the control service is killed when its job closes.
        # Launch through Explorer's native Run box, like the interactive user.
        with Qmp() as qmp:
            qmp.key('meta_l', 'r')
            time.sleep(.5)
            type_text(qmp, r'"C:\Program Files (x86)\Internet Explorer\iexplore.exe" about:blank')
            qmp.key('ret')
        for _ in range(20):
            if ie_pids():
                break
            time.sleep(.2)
        else:
            raise RuntimeError('interactive IE launch was not observed')
        print('IE NAVIGATION EXERCISE STARTED; OVERRIDE REMAINS UNTIL EXPLICIT STOP')


def stop():
    with policy_lock():
        if ie_pids():
            raise RuntimeError('close IE before restoring registration; never mix engines in a live session')
        if not RECEIPT.exists() or json.loads(RECEIPT.read_text()).get('owner') != OWNER:
            raise RuntimeError('owned installation receipt missing')
        if registration() != expected():
            raise RuntimeError('registration drift; refusing to remove')
        require(f'reg delete "{KEY}" /f /reg:32', user=True)
        if registration() is not None:
            raise RuntimeError('registration restoration failed')
        RECEIPT.rename(RECEIPT.with_suffix('.restored.json'))
        print('IE NAVIGATION EXERCISE REGISTRATION RESTORED')


class Qmp:
    def __enter__(self):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(15)
        self.socket.connect(str(ROOT / 'vista-kvm/x64-base/qmp.sock'))
        self.stream = self.socket.makefile('rb')
        if 'QMP' not in json.loads(self.stream.readline()):
            raise RuntimeError('invalid QMP greeting')
        self.call('qmp_capabilities')
        return self

    def call(self, command, arguments=None):
        self.socket.sendall((json.dumps({'execute': command, 'arguments': arguments or {}}) + '\n').encode())
        while True:
            line = self.stream.readline()
            if not line:
                raise RuntimeError('QMP disconnected')
            reply = json.loads(line)
            if 'error' in reply:
                raise RuntimeError(reply['error'])
            if 'return' in reply:
                return reply['return']

    def key(self, *keys):
        hold = 150 if len(keys) > 1 else 30
        self.call('send-key', {'keys': [{'type': 'qcode', 'data': key} for key in keys], 'hold-time': hold})
        time.sleep(hold / 1000 + .08)

    def address_click(self):
        # Controlled IE window has its native address control at this point.
        self.click(240, 44)

    def click(self, x, y):
        # The VM viewer can resize the desktop independently of the IE window.
        # QMP absolute coordinates use the whole display, not the window size.
        with tempfile.TemporaryDirectory(prefix='qmp-input-', dir=ROOT / 'build') as folder:
            path = Path(folder) / 'geometry.ppm'
            self.call('screendump', {'filename': str(path)})
            with path.open('rb') as source:
                header = source.read(64)
            match = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', header)
            if not match:
                raise RuntimeError('cannot determine current QMP display geometry')
            width, height = map(int, match.groups())
        if width < 2 or height < 2 or not (0 <= x < width and 0 <= y < height):
            raise ValueError('click lies outside the controlled display')
        self.call('input-send-event', {'events': [
            {'type': 'abs', 'data': {'axis': 'x', 'value': round(x * 32767 / (width - 1))}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': round(y * 32767 / (height - 1))}},
            {'type': 'btn', 'data': {'button': 'left', 'down': True}}]})
        time.sleep(.15)
        self.call('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
        time.sleep(.2)

    def __exit__(self, *_):
        self.stream.close()
        self.socket.close()


def type_text(qmp, text):
    mapping = {':': ('shift', 'semicolon'), '/': ('slash',), '.': ('dot',),
               '-': ('minus',), '_': ('shift', 'minus'), '?': ('shift', 'slash'),
               '=': ('equal',), '#': ('shift', '3'), '%': ('shift', '5'),
               '\\': ('backslash',), ' ': ('spc',), '(': ('shift', '9'),
               ')': ('shift', '0'), '"': ('shift', 'apostrophe')}
    sequence = []
    for char in text:
        if char in mapping:
            sequence.append(mapping[char])
        elif char.isascii() and char.isalnum():
            sequence.append(('shift', char.lower()) if char.isupper() else (char,))
        else:
            raise ValueError('unsupported keyboard character: ' + repr(char))
    for keys in sequence:
        qmp.key(*keys)


def navigate(url):
    # No COM put_URL: this intentionally types into the native address bar.
    with Qmp() as qmp:
        qmp.address_click()
        qmp.key('ctrl', 'a')
        type_text(qmp, url)
        qmp.key('ret')
    print('NATIVE ADDRESS BAR ENTERED ' + url)


def capture(label):
    if not re.fullmatch('[a-z0-9-]+', label):
        raise ValueError('invalid capture label')
    base = ROOT / ('build/mshtml-native-navigation-' + label)
    with Qmp() as qmp:
        qmp.call('screendump', {'filename': str(base.with_suffix('.ppm'))})
    subprocess.run(['magick', str(base.with_suffix('.ppm')), str(base.with_suffix('.png'))], check=True)
    output = require(r'C:\TritonSupermiumBridge\ie-window-health.exe', user=True)
    base.with_suffix('.health.txt').write_text(output)
    print('CAPTURED ' + str(base.with_suffix('.png')))


def fixture():
    class Handler(BaseHTTPRequestHandler):
        counter = 0
        counter_lock = threading.Lock()

        def do_POST(self):
            length = int(self.headers.get('Content-Length', '0'))
            body = self.rfile.read(length)
            print(json.dumps({'method': 'POST', 'path': self.path,
                              'body': body.decode('utf-8', 'replace'),
                              'x_triton': self.headers.get('X-Triton-Request'),
                              'content_type': self.headers.get('Content-Type')}), flush=True)
            if self.path == '/post-redirect-same':
                self.send_response(307)
                self.send_header('Location', '/post-echo')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            if self.path == '/post-redirect-cross':
                self.send_response(302)
                self.send_header('Location', 'http://10.0.2.2:18768/cross-target')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            if self.path != '/post-echo':
                self.send_error(404)
                return
            text = body.decode('utf-8', 'replace')
            marker = self.headers.get('X-Triton-Request', 'MISSING')
            kind = self.headers.get('Content-Type', 'MISSING')
            page = f'<!doctype html><title>POST {text} {marker} {kind}</title><p>POST echo</p>'.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(page)))
            self.end_headers()
            self.wfile.write(page)

        def do_GET(self):
            print(json.dumps({'path': self.path, 'user_agent': self.headers.get('User-Agent')}), flush=True)
            if self.path in ('/popup-proxy', '/native-links'):
                body = ((ROOT / 'tests/fixtures/mshtml-popup-proxy.html').read_bytes()
                        if self.path == '/popup-proxy' else
                        b'<!doctype html><title>Native Link Test</title><a href="/b" style="display:block;padding:40px;font:24px sans-serif">Navigate to B</a>')
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if self.path == '/download':
                body = b'Triton download contract\n'
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Disposition', 'attachment; filename="triton.txt"')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if self.path == '/slow-headers':
                time.sleep(12)
            if self.path == '/slow-script':
                time.sleep(12)
                body = b'document.title = "Slow resource finished";'
                self.send_response(200)
                self.send_header('Content-Type', 'application/javascript')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
            if self.path == '/redirect':
                self.send_response(302)
                self.send_header('Location', '/b')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            if self.path == '/cross-target':
                marker = self.headers.get('X-Triton-Request', 'MISSING')
                body = f'<!doctype html><title>CROSS GET {marker}</title><p>cross target</p>'.encode()
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if self.path not in ('/a', '/b', '/links', '/slow-headers', '/slow-page', '/counter',
                                  '/history-a', '/new-window', '/frames', '/frame-a', '/frame-b',
                                  '/frame-x', '/frame-y'):
                self.send_error(404)
                return
            name = self.path[1:].upper()
            if self.path == '/counter':
                with self.counter_lock:
                    Handler.counter += 1
                    name = 'REFRESH COUNT ' + str(Handler.counter)
            color = '#163e2e' if name == 'A' else '#183e69'
            if self.path == '/frames':
                body = '''<!doctype html><title>Navigation FRAMES</title>
<body style="background:#32324f;color:white;font:22px Arial;padding:24px">
<h1>Top-level frame isolation</h1>
<script>
const counts={same:0,cross:0};
function loaded(which){counts[which]++;document.title='Frames '+counts.same+' '+counts.cross}
</script>
<iframe id="same" title="same-origin" src="/frame-a" onload="loaded('same')"></iframe>
<iframe id="cross" title="cross-origin" src="http://10.0.2.2:18768/frame-x" onload="loaded('cross')"></iframe>'''
            elif self.path.startswith('/frame-'):
                body = f'''<!doctype html><title>{name}</title><body style="background:#526;color:white">
<p>{name}</p>'''
            else:
                body = f'''<!doctype html><title>Navigation {name}</title>
<body style="background:{color};color:white;font:26px Arial;padding:32px">
<h1>Page {name}</h1><p id="engine">Engine script pending</p><input placeholder="Type here">
<p><a id="next" style="color:yellow" href="/b">Go to B</a>
<a id="fragment" style="color:yellow" href="#section">Same-page fragment</a>
<a id="redirect" style="color:yellow" href="/redirect">Redirect to B</a></p>
<p><button id="script-back" onclick="history.back()">Script history.back()</button>
<button id="script-forward" onclick="history.forward()">Script history.forward()</button></p>
<script>const identify = () => {{ document.getElementById('engine').textContent = 'Chromium: ' + navigator.userAgent; }}; identify();</script>
'''
            if self.path == '/history-a':
                body += '<style>body{min-height:3000px}</style>'
            if self.path == '/new-window':
                body += '<p><a id="new-link" target="link-name" href="/a">New host window</a></p>'
            if self.path == '/slow-page':
                body += '<script src="/slow-script"></script>'
            body = body.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *_):
            pass
    servers = [ThreadingHTTPServer(('127.0.0.1', port), Handler) for port in (18767, 18768)]
    thread = threading.Thread(target=servers[1].serve_forever, daemon=True)
    thread.start()
    timer = threading.Timer(900, lambda: [server.shutdown() for server in servers])
    timer.start()
    print('NAVIGATION FIXTURE READY ports=18767,18768', flush=True)
    try:
        servers[0].serve_forever()
    finally:
        timer.cancel()
        for server in servers:
            server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['start', 'stop', 'navigate', 'capture', 'fixture'])
    parser.add_argument('value', nargs='?')
    args = parser.parse_args()
    if args.action in ('navigate', 'capture'):
        if not args.value:
            parser.error('this action requires a value')
        (navigate if args.action == 'navigate' else capture)(args.value)
    else:
        {'start': start, 'stop': stop, 'fixture': fixture}[args.action]()


if __name__ == '__main__':
    main()
