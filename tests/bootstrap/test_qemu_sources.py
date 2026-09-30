#!/usr/bin/env python3
"""CPU-only dependency safety regressions using disposable Git repositories."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

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

    def test_overlay_directory_collision_preserved(self):
        destination = self.repo / 'meson.build'
        destination.mkdir()
        with self.assertRaisesRegex(RuntimeError, 'destination collision'):
            self.plan()
        with self.assertRaisesRegex(RuntimeError, 'destination collision'):
            module.apply_overlay(self.repo, self.files)
        self.assertEqual(list(destination.iterdir()), [])

    def test_overlay_symlink_destination_preserved(self):
        outside = self.root / 'valuable'
        outside.write_text('user data')
        destination = self.repo / 'meson.build'
        destination.symlink_to(outside)
        with self.assertRaisesRegex(RuntimeError, 'destination collision'):
            self.plan()
        self.assertEqual(outside.read_text(), 'user data')
        self.assertTrue(destination.is_symlink())

    def test_overlay_parent_file_and_symlink_preserved(self):
        parent = self.repo / 'nested'
        for is_link in (False, True):
            if is_link:
                parent.symlink_to(self.root, target_is_directory=True)
            else:
                parent.write_text('user data')
            with self.assertRaisesRegex(RuntimeError, 'parent collision'):
                module.plan(self.repo, self.pin, {'nested/meson.build': self.overlay})
            parent.unlink()

    def test_overlay_root_and_ancestor_symlink_rejected(self):
        projects = self.root / 'subprojects'
        projects.mkdir()
        packagefiles = projects / 'packagefiles'
        packagefiles.mkdir()
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'meson.build').write_text('external overlay')
        (packagefiles / 'fixture').symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, 'overlay symlink'):
            module.overlay_files(projects, {'patch_directory': 'fixture'})
        (packagefiles / 'fixture').unlink()
        packagefiles.rmdir()
        packagefiles.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, 'overlay symlink'):
            module.overlay_files(projects, {'patch_directory': 'fixture'})

    def prepare_bootstrap(self):
        projects = self.root / 'project/triton-qemu/subprojects'
        projects.mkdir(parents=True)
        destination = projects / 'keycodemapdb'
        self.repo.rename(destination)
        self.repo = destination
        overlay = projects / 'packagefiles/fixture'
        overlay.mkdir(parents=True)
        (overlay / 'meson.build').write_text(self.overlay.read_text())
        (projects / 'keycodemapdb.wrap').write_text(
            '[wrap-git]\nurl = https://gitlab.com/qemu-project/keycodemapdb.git\n'
            f'revision = {self.pin}\npatch_directory = fixture\n')
        return self.root / 'project'

    def test_existing_bootstrap_checks_overlay_result(self):
        root = self.prepare_bootstrap()
        with patch.object(module, 'NAMES', ('keycodemapdb',)):
            with patch.object(module, 'apply_overlay'):
                with self.assertRaisesRegex(RuntimeError, 'verification failed'):
                    module.bootstrap(root)
            module.bootstrap(root)
            module.bootstrap(root)
        self.assertEqual((self.repo / 'meson.build').read_bytes(), self.overlay.read_bytes())

    def test_existing_bootstrap_preserves_empty_directory(self):
        root = self.prepare_bootstrap()
        destination = self.repo / 'meson.build'
        destination.mkdir()
        with patch.object(module, 'NAMES', ('keycodemapdb',)):
            with self.assertRaisesRegex(RuntimeError, 'destination collision'):
                module.bootstrap(root)
        self.assertEqual(list(destination.iterdir()), [])

    def test_linked_dependency_ancestors_rejected_before_git(self):
        for name in ('triton-qemu', 'triton-qemu/subprojects'):
            root = self.root / ('linked-' + name.replace('/', '-'))
            root.mkdir()
            link = root / name
            link.parent.mkdir(parents=True, exist_ok=True)
            link.symlink_to(self.repo, target_is_directory=True)
            with patch.object(module, 'git') as calls:
                with self.assertRaisesRegex(RuntimeError, 'linked dependency ancestor'):
                    module.bootstrap(root)
            calls.assert_not_called()


if __name__ == '__main__':
    unittest.main()
