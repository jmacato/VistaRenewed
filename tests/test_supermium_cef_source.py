import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    'supermium_cef_source', Path(__file__).resolve().parents[1] /
    'scripts/supermium_cef_source.py')
SOURCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SOURCE)


class SourcePinTests(unittest.TestCase):
    def run_verify(self, revision=None, version=None, dirty=False):
        def fake_git(repo, *args, **kwargs):
            if args == ('rev-parse', 'HEAD'):
                output = revision or SOURCE.PINS[repo.name]
            elif args == ('show', 'HEAD:chrome/VERSION'):
                output = 'MAJOR=144\nMINOR=0\nBUILD=7559\nPATCH=256\n'
            elif args == ('show', 'HEAD:CHROMIUM_BUILD_COMPATIBILITY.txt'):
                output = repr({'chromium_checkout': 'refs/tags/' +
                               (version or SOURCE.VERSION)})
            elif args == ('status', '--porcelain'):
                output = ' M BUILD.gn' if dirty else ''
            else:
                self.fail(f'Unexpected command: {args}')
            return subprocess.CompletedProcess(args, 0, output, '')
        with patch.object(SOURCE, 'git', side_effect=fake_git), \
                contextlib.redirect_stdout(io.StringIO()):
            SOURCE.verify()

    def test_matching_pins(self):
        self.run_verify()

    def test_wrong_revision_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'wrong revision'):
            self.run_verify(revision='0' * 40)

    def test_same_branch_wrong_patch_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'version mismatch'):
            self.run_verify(version='144.0.7559.262')

    def test_dirty_cef_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'unreviewed modifications'):
            self.run_verify(dirty=True)


class PatchContextTests(unittest.TestCase):
    def merged(self, content):
        with patch.object(Path, 'read_text', side_effect=[
                content, '{"example": [["old context", "new context"]]}']):
            return SOURCE.patch_text('example', merged=True)

    def test_unique_context_replaced(self):
        self.assertEqual(self.merged('before old context after'),
                         'before new context after')

    def test_missing_context_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'expected one context match'):
            self.merged('unrelated context')

    def test_ambiguous_context_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'expected one context match'):
            self.merged('old context old context')


if __name__ == '__main__':
    unittest.main()
