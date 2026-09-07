#!/usr/bin/env python3
"""Generate deployment-service pins from an explicitly supplied DER certificate."""
import argparse
import hashlib
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('certificate', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
data = args.certificate.read_bytes()
# Reject PEM, malformed inputs, and DER with trailing bytes before pinning it.
canonical = subprocess.check_output([
    'openssl', 'x509', '-inform', 'DER', '-in', str(args.certificate), '-outform', 'DER'])
if canonical != data:
    raise SystemExit('Expected one canonical DER X.509 certificate')
sha256 = hashlib.sha256(data).hexdigest()
thumbprint = ', '.join(f'0x{byte:02x}' for byte in hashlib.sha1(data).digest())
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(
    '/* Generated from the public build certificate; contains no private key. */\n'
    f'#define SIGNING_CERT_SHA256 L"{sha256}"\n'
    f'#define SIGNING_CERT_THUMBPRINT_BYTES {thumbprint}\n')
