#!/usr/bin/env python3
"""Upload over QEMU's private host gateway; serial carries only the command."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import select
import shutil
import subprocess
import sys
import threading
import time

import vista_control


def serve(path, token):
    # Runs inside the QEMU container network namespace, not on a LAN address.
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != '/' + token:
                self.send_error(404)
                return
            with path.open('rb') as source:
                self.send_response(200)
                self.send_header('Content-Length', str(path.stat().st_size))
                self.end_headers()
                shutil.copyfileobj(source, self.wfile)

        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    print(json.dumps({'port': server.server_port}), flush=True)
    # Parent closes stdin on failure; hard deadline also limits orphan lifetime.
    try:
        readable, _, _ = select.select([sys.stdin], [], [], 900)
        if readable:
            sys.stdin.readline()
    finally:
        server.shutdown()
        server.server_close()


def upload(local, remote, container):
    if any(char in remote for char in '"%\r\n&|<>^!'):
        raise ValueError('destination contains cmd.exe metacharacters')
    if len(remote) < 3 or remote[1:3] != ':\\':
        raise ValueError('destination must be an absolute Windows drive path')
    local = local.resolve(strict=True)
    with local.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    token = secrets.token_hex(32)
    script = str(Path(__file__).resolve())
    # The VM container already bind-mounts this workspace at its host path.
    process = subprocess.Popen(['podman', 'exec', '-i', container, 'python3',
                                script, '_serve', str(local), token],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               text=True)
    start = time.monotonic()
    try:
        ready = json.loads(process.stdout.readline())
        url = f'http://10.0.2.2:{ready["port"]}/{token}'
        command = f'C:\\ProgramData\\TritonControl\\vista-fetch.exe "{url}" "{remote}" {digest}'
        result = vista_control.main(['run', '--timeout', '600', command])
        if result:
            raise vista_control.ControlError(f'TCP download failed, exit={result}')
        print(f'Transferred {local.stat().st_size} bytes in {time.monotonic() - start:.2f}s over TCP')
    finally:
        process.stdin.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--container', default='triton-vista-x64-normal')
    commands = parser.add_subparsers(dest='action', required=True)
    put = commands.add_parser('put')
    put.add_argument('local', type=Path)
    put.add_argument('remote')
    server = commands.add_parser('_serve')
    server.add_argument('local', type=Path)
    server.add_argument('token')
    args = parser.parse_args()
    if args.action == '_serve':
        serve(args.local, args.token)
    else:
        upload(args.local, args.remote, args.container)


if __name__ == '__main__':
    main()
