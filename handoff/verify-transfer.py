#!/usr/bin/env python3
"""Check the source export against its compressed transfer manifest."""
import gzip
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent.parent
manifest = root / 'handoff' / 'transfer-manifest.jsonl.gz'
failures = []
count = 0
with gzip.open(manifest, 'rt', encoding='utf-8') as stream:
    for line in stream:
        item = json.loads(line)
        relative = Path(item['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise SystemExit('Unsafe manifest path')
        path = root / relative
        count += 1
        if item['kind'] == 'symlink':
            if not path.is_symlink() or str(path.readlink()) != item['target']:
                failures.append(str(relative))
            continue
        if path.is_symlink() or not path.is_file():
            failures.append(str(relative))
            continue
        digest = hashlib.sha256()
        with path.open('rb') as content:
            for chunk in iter(lambda: content.read(1024 * 1024), b''):
                digest.update(chunk)
        if digest.hexdigest() != item['sha256']:
            failures.append(str(relative))
if failures:
    print('\n'.join(failures[:30]))
    raise SystemExit(f'FAIL: {len(failures)} missing or changed paths')
print(f'PASS: {count} file contents and symlink targets match')
