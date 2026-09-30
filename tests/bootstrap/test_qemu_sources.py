#!/usr/bin/env python3
"""CPU-only dependency safety regressions using disposable Git repositories."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('qemu_bootstrap', ROOT / 'scripts/bootstrap_qemu_sources.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class QemuSources(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / 'source'
        self.repo.mkdir()
        self.git('init', '-q')
        self.git('config', 'user.name', 'Fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        (self.repo / '.gitignore').write_text('ignored\n')
        (self.repo / 'code').write_text('original\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'fixture')
        self.pin = self.git('rev-parse', 'HEAD').decode().strip()
        self.overlay = self.root / 'meson.build'
        self.overlay.write_text('project(\'fixture\')\n')
        self.files = {'meson.build': self.overlay}

    def git(self, *args):
        return module.git(self.repo, *args)

    def plan(self):
        return module.plan(self.repo, self.pin, self.files)

    def test_initial_overlay_and_repeat(self):
        self.assertTrue(self.plan())
        module.apply_overlay(self.repo, self.files)
        self.assertFalse(self.plan())
        self.assertEqual((self.repo / 'meson.build').read_bytes(), self.overlay.read_bytes())

    def test_ignored_user_file_preserved(self):
        p = self.repo / 'ignored'
        p.write_text('user data\n')
        with self.assertRaisesRegex(RuntimeError, 'Divergent'):
            self.plan()
        self.assertEqual(p.read_text(), 'user data\n')

    def test_overlay_collision_preserved(self):
        p = self.repo / 'meson.build'
        p.write_text('user overlay\n')
        with self.assertRaisesRegex(RuntimeError, 'Divergent'):
            self.plan()
        self.assertEqual(p.read_text(), 'user overlay\n')

    def test_tracked_change_and_mode_preserved(self):
        p = self.repo / 'code'
        p.chmod(0o755)
        with self.assertRaisesRegex(RuntimeError, 'Divergent'):
            self.plan()
        self.assertTrue(p.stat().st_mode & 0o111)

    def test_staged_change_preserved(self):
        (self.repo / 'code').write_text('staged\n')
        self.git('add', 'code')
        before = self.git('diff', '--cached')
        with self.assertRaisesRegex(RuntimeError, 'Staged'):
            self.plan()
        self.assertEqual(before, self.git('diff', '--cached'))

    def test_unexpected_revision(self):
        with self.assertRaisesRegex(RuntimeError, 'Unexpected'):
            module.plan(self.repo, '0' * 40, self.files)

    def test_symlink_repository_rejected(self):
        link = self.root / 'alias'
        link.symlink_to(self.repo)
        with self.assertRaisesRegex(RuntimeError, 'linked'):
            module.plan(link, self.pin, self.files)


if __name__ == '__main__':
    unittest.main()
