#!/usr/bin/env python3
"""Exercise stream framing and failure handling without a Windows guest."""
import hashlib
import importlib.util
from pathlib import Path
import socket
import tempfile
import threading
import unittest

spec = importlib.util.spec_from_file_location('vista_control', Path(__file__).resolve().parents[1] / 'tools/vista_control.py')
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)
JOB = '0123456789abcdef' * 2


class ProtocolTest(unittest.TestCase):
    def exchange(self, responder, test):
        errors = []
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'control.sock'
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            server.bind(str(path))
            server.listen(1)
            def serve():
                try:
                    connection, _ = server.accept()
                    with connection, connection.makefile('rb') as stream:
                        parts = stream.readline().decode().split()
                        self.assertEqual(parts[:2], ['TC1', JOB])
                        payload = stream.read(int(parts[4]))
                        self.assertEqual(hashlib.sha256(payload).hexdigest(), parts[5])
                        responder(connection, parts, payload)
                except BaseException as error:
                    errors.append(error)
            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            try:
                with control.Client(path, timeout=1) as client:
                    test(client)
                thread.join(3)
                self.assertFalse(thread.is_alive(), 'server thread hung')
                if errors:
                    raise errors[0]
            finally:
                server.close()

    def test_concurrent_client_rejected_before_connect(self):
        def check(client):
            path = client.socket.getpeername()
            with self.assertRaisesRegex(control.ControlError, 'another guest-control client'):
                control.Client(path, timeout=1)
            client.checked('PING', job_id=JOB)
        self.exchange(lambda sock, *_: sock.sendall(f'TC1 {JOB} OK 0 0\n'.encode()), check)

    def test_fragmented_binary_and_stale_response(self):
        blob = bytes(range(256)) * 5
        def respond(sock, parts, payload):
            self.assertEqual(parts[2:4], ['READ', '17'])
            self.assertEqual(payload, b'C:\\binary.dat')
            wire = b'TC1 ' + b'f' * 32 + b' OK 0 3\nold'
            wire += f'TC1 {JOB} OK 0 {len(blob)}\n'.encode() + blob
            for offset in range(0, len(wire), 7):
                sock.sendall(wire[offset:offset+7])
        self.exchange(respond, lambda c: self.assertEqual(c.checked('READ', b'C:\\binary.dat', 17, JOB)['data'], blob))

    def test_truncated_payload(self):
        def respond(sock, *_):
            sock.sendall(f'TC1 {JOB} OK 0 10\nabc'.encode())
        def check(client):
            with self.assertRaisesRegex(control.ControlError, 'truncated'):
                client.checked('PING', job_id=JOB)
        self.exchange(respond, check)

    def test_oversized_payload(self):
        def respond(sock, *_):
            sock.sendall(f'TC1 {JOB} OK 0 {control.MAX_PAYLOAD + 1}\n'.encode())
        def check(client):
            with self.assertRaisesRegex(control.ControlError, 'oversized'):
                client.checked('PING', job_id=JOB)
        self.exchange(respond, check)

    def test_guest_error_and_exit_code(self):
        def respond(sock, *_):
            sock.sendall(f'TC1 {JOB} ERROR 5 6\ndenied'.encode())
        def check(client):
            with self.assertRaisesRegex(control.ControlError, 'guest error 5: denied'):
                client.checked('RUN', b'whoami', 1000, JOB)
        self.exchange(respond, check)

    def test_run_is_sent_once_on_disconnect(self):
        received = []
        def respond(sock, parts, payload):
            received.append((parts[1], parts[2], payload))
        def check(client):
            with self.assertRaisesRegex(control.ControlError, 'may still be running'):
                client.checked('RUN', b'install.cmd', 1000, JOB)
        self.exchange(respond, check)
        self.assertEqual(received, [(JOB, 'RUN', b'install.cmd')])


if __name__ == '__main__':
    unittest.main()
