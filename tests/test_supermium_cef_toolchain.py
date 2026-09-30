import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'toolchain', Path(__file__).resolve().parents[1] /
    'scripts/supermium_cef_toolchain.py')
TOOLCHAIN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOLCHAIN)


class ToolchainTests(unittest.TestCase):
    def test_install_requires_license_consent(self):
        with self.assertRaisesRegex(RuntimeError, 'explicit --accept-license'):
            TOOLCHAIN.install(False)

    def test_missing_version_fails(self):
        with self.assertRaises(RuntimeError):
            TOOLCHAIN.only([])

    def test_ambiguous_version_fails(self):
        with self.assertRaises(RuntimeError):
            TOOLCHAIN.only([Path('a'), Path('b')])

    def test_overlay_preserves_file_contents_and_casing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header = root / 'Windows.h'
            header.write_text('example header\n')
            overlay = TOOLCHAIN.overlay_directory(root)
            self.assertEqual(overlay['contents'], [
                {'type': 'file', 'name': 'Windows.h',
                 'external-contents': str(header)}])
            self.assertEqual(header.read_text(), 'example header\n')


if __name__ == '__main__':
    unittest.main()
