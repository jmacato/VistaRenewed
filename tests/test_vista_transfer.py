#!/usr/bin/env python3
"""Test the restricted bulk-data endpoint without a VM."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import urllib.error
import urllib.request


class TransferTest(unittest.TestCase):
    def test_exact_file_only_and_shutdown(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'payload'
            payload = bytes(range(256)) * 8193  # Exceeds serial's 1 MiB limit.
            path.write_bytes(payload)
            process = subprocess.Popen(
                [sys.executable, 'tools/vista_transfer.py', '_serve', str(path), 'test-token'],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            try:
                port = json.loads(process.stdout.readline())['port']
                base = f'http://127.0.0.1:{port}'
                for suffix in ['/', '/payload', '/test-token/../payload']:
                    with self.assertRaises(urllib.error.HTTPError) as error:
                        urllib.request.urlopen(base + suffix)
                    self.assertEqual(error.exception.code, 404)
                with urllib.request.urlopen(base + '/test-token') as response:
                    self.assertEqual(response.read(), payload)
            finally:
                process.stdin.close()
                self.assertEqual(process.wait(timeout=5), 0)
                process.stdout.close()


if __name__ == '__main__':
    unittest.main()
