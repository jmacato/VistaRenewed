"""Check SDK cache integrity and failed-download preservation without network."""
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch
import zipfile


ROOT = Path(__file__).resolve().parents[1]
PINS = {
    'microsoft.windows.sdk.cpp': '0e287e9382fe92736111840b186b7690bcce623c3ce0e2a8a7d2d0d04199482c',
    'microsoft.windows.wdk.x64': '506103e3da1cacad2b98193faf9ee873f3300560959caba85349d9e04afd4803',
}


def archive(entries):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w') as output:
        for name, data in entries.items():
            output.writestr(name, data)
    return stream.getvalue()


class HeaderCacheTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.script = self.root / 'scripts/fetch_vista_sdk_headers.py'
        self.script.parent.mkdir()
        self.payloads = {name: archive({'c/Include/test.h': name.encode()})
                         for name in PINS}
        self.write_script()

    def write_script(self):
        source = (ROOT / 'scripts/fetch_vista_sdk_headers.py').read_text()
        for name, pin in PINS.items():
            source = source.replace(pin, hashlib.sha256(self.payloads[name]).hexdigest())
        self.script.write_text(source)

    def run_fetch(self, force=False):
        def download(url, **kwargs):
            name = url.split('/v3-flatcontainer/')[1].split('/')[0]
            return io.BytesIO(self.payloads[name])
        with patch('urllib.request.urlopen', side_effect=download) as request:
            with patch('sys.argv', [str(self.script)] + (['--force'] if force else [])):
                with redirect_stdout(io.StringIO()):
                    runpy.run_path(str(self.script), run_name='__main__')
            return request.call_count

    def header(self):
        return self.root / 'driver/sdk/microsoft.windows.sdk.cpp/c/Include/test.h'

    def test_initial_and_repeat(self):
        self.assertEqual(self.run_fetch(), 2)
        self.assertEqual(self.run_fetch(), 0)
        self.assertEqual(self.header().read_bytes(), b'microsoft.windows.sdk.cpp')

    def test_same_count_corruption_is_repaired(self):
        self.run_fetch()
        self.header().write_bytes(b'changed contents, same file count')
        self.assertEqual(self.run_fetch(), 1)
        self.assertEqual(self.header().read_bytes(), b'microsoft.windows.sdk.cpp')

    def test_extra_header_is_removed(self):
        self.run_fetch()
        extra = self.header().with_name('stale.h')
        extra.write_text('stale')
        self.assertEqual(self.run_fetch(), 1)
        self.assertFalse(extra.exists())
        self.assertEqual(self.run_fetch(), 0)

    def test_legacy_count_only_record_is_not_trusted(self):
        self.run_fetch()
        record = self.root / 'driver/sdk/microsoft.windows.sdk.cpp.json'
        data = json.loads(record.read_text())
        del data['files']
        record.write_text(json.dumps(data))
        self.assertEqual(self.run_fetch(), 1)

    def test_bad_download_preserves_previous_headers(self):
        self.run_fetch()
        self.payloads['microsoft.windows.sdk.cpp'] = b'bad download'
        with self.assertRaisesRegex(SystemExit, 'SHA-256 mismatch'):
            self.run_fetch(force=True)
        self.assertEqual(self.header().read_bytes(), b'microsoft.windows.sdk.cpp')

    def test_unsafe_archive_preserves_previous_headers(self):
        self.run_fetch()
        self.payloads['microsoft.windows.sdk.cpp'] = archive({
            'c/Include/test.h': b'new header', 'c/Include/../../../escape': b'bad'})
        self.write_script()
        with self.assertRaisesRegex(SystemExit, 'Unsafe archive path'):
            self.run_fetch(force=True)
        self.assertEqual(self.header().read_bytes(), b'microsoft.windows.sdk.cpp')
        self.assertFalse((self.root / 'escape').exists())

    def test_empty_archive_preserves_previous_headers(self):
        self.run_fetch()
        self.payloads['microsoft.windows.sdk.cpp'] = archive({'README': b'no headers'})
        self.write_script()
        with self.assertRaisesRegex(SystemExit, 'no SDK headers'):
            self.run_fetch(force=True)
        self.assertEqual(self.header().read_bytes(), b'microsoft.windows.sdk.cpp')


if __name__ == '__main__':
    unittest.main()
