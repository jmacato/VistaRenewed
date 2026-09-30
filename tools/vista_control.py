#!/usr/bin/env python3
"""Host-only client for the Triton Vista LocalSystem control service."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import re
import socket
import sys
import time
import uuid

DEFAULT_SOCKET = Path(__file__).resolve().parents[1] / 'vista-kvm/x64-base/control/control.sock'
MAX_PAYLOAD = 1024 * 1024
JOB_ROOT = r'C:\ProgramData\TritonControl\jobs'


class ControlError(RuntimeError):
    pass


class Client:
    def __init__(self, path=DEFAULT_SOCKET, timeout=15):
        # A QEMU serial chardev accepts one host stream at a time. A second
        # connection can otherwise queue silently and strand a partial request.
        self.lock = open(str(path) + '.client-lock', 'a+b')
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            self.lock.close()
            raise ControlError('another guest-control client is connected; wait for it to finish')
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(timeout)
        try:
            self.socket.connect(str(path))
        except BaseException:
            self.socket.close()
            self.lock.close()
            raise
        self.stream = self.socket.makefile('rb')

    def close(self):
        self.stream.close()
        self.socket.close()
        self.lock.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def request(self, operation, payload=b'', arg=0, job_id=None):
        previous_timeout = self.socket.gettimeout()
        # A 1 MiB upload takes about 91 seconds at COM2's 115200 baud.
        # Timing out halfway through leaves the guest waiting for the body.
        if operation == 'WRITE' and previous_timeout is not None:
            self.socket.settimeout(max(previous_timeout, 120))
        try:
            return self._request(operation, payload, arg, job_id)
        finally:
            self.socket.settimeout(previous_timeout)

    def _request(self, operation, payload=b'', arg=0, job_id=None):
        job_id = job_id or uuid.uuid4().hex
        if not re.fullmatch('[0-9a-f]{32}', job_id):
            raise ValueError('job ID must contain 32 lowercase hexadecimal digits')
        if operation not in ('PING', 'RUN', 'USER', 'STATUS', 'READ', 'WRITE'):
            raise ValueError('unknown operation')
        if not isinstance(payload, bytes) or len(payload) > MAX_PAYLOAD:
            raise ValueError('payload exceeds 1 MiB or is not bytes')
        if not 0 <= arg <= 0xffffffff:
            raise ValueError('argument exceeds unsigned 32-bit range')
        header = f'TC1 {job_id} {operation} {arg} {len(payload)} {hashlib.sha256(payload).hexdigest()}\n'.encode('ascii')
        self.socket.sendall(header + payload)
        while True:
            line = self.stream.readline(257)
            if not line:
                raise ControlError(f'connection closed; job {job_id} may still be running; use status')
            if len(line) > 256 or not line.endswith(b'\n'):
                raise ControlError('invalid response header')
            match = re.fullmatch(rb'TC1 ([0-9a-f]{32}) (OK|RUNNING|DONE|ERROR) ([0-9]+) ([0-9]+)\r?\n', line)
            if not match:
                # Serial startup diagnostics are not protocol responses.
                continue
            response_id, status, code, length = match.groups()
            length = int(length)
            if length > MAX_PAYLOAD:
                raise ControlError('oversized response')
            chunks = bytearray()
            while len(chunks) < length:
                part = self.stream.read(length - len(chunks))
                if not part:
                    raise ControlError('truncated response payload')
                chunks.extend(part)
            if response_id.decode() != job_id:
                continue
            return {'id': job_id, 'status': status.decode(), 'code': int(code), 'data': bytes(chunks)}

    def checked(self, operation, payload=b'', arg=0, job_id=None):
        reply = self.request(operation, payload, arg, job_id)
        if reply['status'] == 'ERROR':
            raise ControlError(f"guest error {reply['code']}: {reply['data'].decode('utf-8', errors='replace')}")
        return reply

    def read_file(self, remote, output):
        offset = 0
        while True:
            reply = self.checked('READ', remote.encode('utf-8'), offset)
            data = reply['data']
            if not data:
                return offset
            output.write(data)
            offset += len(data)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', type=Path, default=DEFAULT_SOCKET)
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('ping')
    run = sub.add_parser('run')
    run.add_argument('--user', action='store_true', help='Run on the active user desktop; use linked admin token when available')
    run.add_argument('--timeout', type=int, default=120, help='Guest execution limit in seconds (1..3600)')
    run.add_argument('--id', help='Stable job ID; reuse queries the existing job, never launches twice')
    run.add_argument('--detach', action='store_true')
    run.add_argument('command', help='One quoted Windows cmd.exe command')
    status = sub.add_parser('status')
    status.add_argument('id')
    get = sub.add_parser('get')
    get.add_argument('remote')
    get.add_argument('local', type=Path)
    put = sub.add_parser('put')
    put.add_argument('local', type=Path)
    put.add_argument('remote')
    uia = sub.add_parser('uia', help='Run bounded UI Automation on the active desktop')
    uia.add_argument('operation', choices=['probe', 'tree', 'focus', 'invoke', 'set-value'])
    uia.add_argument('--hwnd', default='0', help='Root HWND in hexadecimal; 0 means desktop for tree')
    uia.add_argument('--path', default='0', help='Control-view child path from tree output, e.g. 0.2.1')
    uia.add_argument('--expect-name', help='Required exact current name before a mutating operation')
    uia.add_argument('--value', help='Text for set-value; UTF-8 hex encoded, never interpreted by cmd.exe')
    args = parser.parse_args(argv)
    if args.action == 'uia':
        if not re.fullmatch(r'(?:0x)?[0-9a-fA-F]{1,16}', args.hwnd):
            parser.error('invalid hexadecimal HWND')
        if not re.fullmatch(r'0(?:\.[0-9]{1,3}){0,8}', args.path):
            parser.error('invalid UIA path')
        mutating = args.operation not in ('probe', 'tree')
        if mutating and (not int(args.hwnd, 16) or args.expect_name is None):
            parser.error('UIA actions require a nonzero --hwnd and --expect-name')
        if args.operation == 'set-value' and args.value is None:
            parser.error('set-value requires --value')
        def encoded(text):
            data = text.encode('utf-8')
            if b'\0' in data or len(data) > 8000:
                parser.error('UIA text must not contain NUL and must be at most 8000 UTF-8 bytes')
            return '"' + data.hex() + '"'
        command = r'C:\ProgramData\TritonControl\vista-uia.exe ' + args.operation
        if args.operation != 'probe':
            command += f' {args.hwnd} {args.path}'
        if mutating:
            command += ' ' + encoded(args.expect_name)
        if args.operation == 'set-value':
            command += ' ' + encoded(args.value)
        if len(command) > 7000:
            parser.error('encoded UIA command exceeds safe cmd.exe command-line length')
        return main(['--socket', str(args.socket), 'run', '--user', '--timeout', '20', command])
    with Client(args.socket) as client:
        if args.action == 'ping':
            reply = client.checked('PING')
            print(reply['data'].decode('utf-8', errors='replace'))
        elif args.action == 'status':
            reply = client.request('STATUS', job_id=args.id)
            print(json.dumps({**reply, 'data': reply['data'].decode('utf-8', errors='replace')}))
            return 1 if reply['status'] == 'ERROR' else 0
        elif args.action == 'get':
            with args.local.open('wb') as output:
                client.read_file(args.remote, output)
        elif args.action == 'put':
            payload = args.remote.encode('utf-8') + b'\0' + args.local.read_bytes()
            if len(payload) > MAX_PAYLOAD:
                raise ControlError('upload exceeds 1 MiB; mount larger packages on the guest CD')
            client.checked('WRITE', payload)
        elif args.action == 'run':
            if not 1 <= args.timeout <= 3600:
                parser.error('--timeout must be 1..3600 seconds')
            job_id = args.id or uuid.uuid4().hex
            print(f'job={job_id}', file=sys.stderr, flush=True)
            reply = client.checked('USER' if args.user else 'RUN', args.command.encode('utf-8'), args.timeout * 1000, job_id)
            if args.detach:
                print(json.dumps({'id': job_id, 'status': reply['status'], 'code': reply['code']}))
                return 0
            deadline = time.monotonic() + args.timeout + 30
            while reply['status'] == 'RUNNING':
                if time.monotonic() > deadline:
                    raise ControlError(f'host wait expired; query status {job_id}; command was not resubmitted')
                time.sleep(.25)
                reply = client.checked('STATUS', job_id=job_id)
            if reply['status'] != 'DONE':
                raise ControlError(f'unexpected job status: {reply["status"]}')
            client.read_file(JOB_ROOT + '\\' + job_id + '.out', sys.stdout.buffer)
            return reply['code'] if 0 <= reply['code'] <= 255 else 1
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ControlError, OSError, ValueError) as exc:
        print(f'vista-control: {exc}', file=sys.stderr)
        raise SystemExit(1)
