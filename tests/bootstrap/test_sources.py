#!/usr/bin/env python3
"""Exercise patch setup against real disposable Git repositories (CPU only)."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch as mock_patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bootstrap', ROOT / 'scripts/bootstrap_sources.py')
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name) / 'repo'
        self.repo.mkdir()
        self.git('init', '-q')
        self.git('config', 'user.name', 'Fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        (self.repo / 'source').write_text('original\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'fixture')
        self.pin = self.git('rev-parse', 'HEAD').decode().strip()
        (self.repo / 'source').write_text('curated\n')
        (self.repo / 'new-source').write_text('new\n')
        self.git('add', '-N', 'new-source')
        self.patch = Path(self.temp.name) / 'source.patch'
        self.patch.write_bytes(self.git('diff', '--binary', 'HEAD'))
        self.git('reset', '--hard', '-q', 'HEAD')

    def git(self, *args):
        return bootstrap.git(self.repo, *args)

    def plan(self):
        return bootstrap.plan_patch(self.repo, self.pin, self.patch)

    def test_initial_repeat_and_index_preserved(self):
        before = (self.repo / '.git/index').read_bytes()
        self.assertTrue(self.plan())
        self.assertEqual(before, (self.repo / '.git/index').read_bytes())
        self.git('apply', str(self.patch))
        self.assertFalse(self.plan())
        self.assertEqual((self.repo / 'new-source').read_text(), 'new\n')

    def test_divergent_tracked_preserved(self):
        (self.repo / 'source').write_text('human edits\n')
        with self.assertRaisesRegex(RuntimeError, 'Divergent'):
            self.plan()
        self.assertEqual((self.repo / 'source').read_text(), 'human edits\n')

    def test_divergent_untracked_preserved(self):
        (self.repo / 'human').write_text('keep\n')
        with self.assertRaisesRegex(RuntimeError, 'Divergent'):
            self.plan()
        self.assertEqual((self.repo / 'human').read_text(), 'keep\n')

    def test_staged_preserved(self):
        self.git('apply', '--index', str(self.patch))
        before = self.git('diff', '--cached')
        with self.assertRaisesRegex(RuntimeError, 'Staged'):
            self.plan()
        self.assertEqual(before, self.git('diff', '--cached'))

    def test_wrong_pin(self):
        with self.assertRaisesRegex(RuntimeError, 'Unexpected source revision'):
            bootstrap.plan_patch(self.repo, '0' * 40, self.patch)

    def test_checksum(self):
        with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
            bootstrap.verify(Path(self.temp.name), 'source.patch', '0' * 64)

    def prepare_bundle(self):
        root = Path(self.temp.name)
        destination = root / 'triton-dxvk'
        self.repo.rename(destination)
        self.repo = destination
        base = self.pin
        (self.repo / 'bundle-source').write_text('bundled source\n')
        self.git('add', 'bundle-source')
        self.git('commit', '-qm', 'bundled source')
        pin = self.git('rev-parse', 'HEAD').decode().strip()
        bundle = root / 'source.bundle'
        self.git('bundle', 'create', str(bundle), 'HEAD')
        self.git('checkout', '--detach', base)
        (root / 'patches').mkdir()
        (root / 'patches/sources.json').write_text(json.dumps({
            'version': 1, 'url': str(self.repo), 'base': base,
            'bundle': bundle.name,
            'bundle_sha256': hashlib.sha256(bundle.read_bytes()).hexdigest(),
            'repositories': [{'path': 'triton-dxvk', 'commit': pin,
                              'patch': self.patch.name,
                              'sha256': hashlib.sha256(self.patch.read_bytes()).hexdigest()}],
        }))
        return root, base, pin

    def test_full_bundle_bootstrap_and_repeat(self):
        root, _, pin = self.prepare_bundle()
        bootstrap.bootstrap(root)
        bootstrap.bootstrap(root)
        self.assertEqual(self.git('rev-parse', 'HEAD').decode().strip(), pin)
        self.assertEqual((self.repo / 'source').read_text(), 'curated\n')
        self.assertEqual((self.repo / 'bundle-source').read_text(), 'bundled source\n')

    def test_bundle_checkout_preserves_ignored_user_file(self):
        root, base, _ = self.prepare_bundle()
        (self.repo / '.git/info/exclude').write_text('/bundle-source\n')
        valuable = self.repo / 'bundle-source'
        valuable.write_text('user data\n')
        self.assertEqual(self.git('status', '--porcelain').strip(), b'')
        with self.assertRaises(subprocess.CalledProcessError):
            bootstrap.bootstrap(root)
        self.assertEqual(valuable.read_text(), 'user data\n')
        self.assertEqual(self.git('rev-parse', 'HEAD').decode().strip(), base)

    def test_patch_preserves_ignored_user_file(self):
        (self.repo / '.git/info/exclude').write_text('/new-source\n')
        valuable = self.repo / 'new-source'
        valuable.write_text('user data\n')
        with self.assertRaises(subprocess.CalledProcessError):
            self.plan()
        self.assertEqual(valuable.read_text(), 'user data\n')

    def test_nested_manifest_is_checked_before_update(self):
        root = Path(self.temp.name) / 'nested-fixture'
        root.mkdir()

        def init(path):
            path.mkdir(parents=True)
            bootstrap.git(path, 'init', '-q')
            bootstrap.git(path, 'config', 'user.name', 'Fixture')
            bootstrap.git(path, 'config', 'user.email', 'fixture@example.invalid')
            (path / 'source').write_text('original\n')
            bootstrap.git(path, 'add', '.')
            bootstrap.git(path, 'commit', '-qm', 'source')

        def digest(path):
            return hashlib.sha256(path.read_bytes()).hexdigest()

        # File transport is enabled only for these disposable test repositories.
        settings = {'GIT_CONFIG_COUNT': '1', 'GIT_CONFIG_KEY_0': 'protocol.file.allow',
                    'GIT_CONFIG_VALUE_0': 'always'}
        with mock_patch.dict(os.environ, settings):
            leaf, shader = root / 'leaf', root / 'shader'
            init(leaf)
            init(shader)
            bootstrap.git(shader, 'submodule', 'add', str(leaf), 'nested')
            bootstrap.git(shader, 'commit', '-qam', 'nested dependency')
            shader_pin = bootstrap.git(shader, 'rev-parse', 'HEAD').decode().strip()
            project = root / 'project'
            source = project / 'triton-dxvk'
            init(source)
            bootstrap.git(source, 'submodule', 'add', str(shader), 'subprojects/dxbc-spirv')
            bootstrap.git(source, 'commit', '-qam', 'shader dependency')
            pin = bootstrap.git(source, 'rev-parse', 'HEAD').decode().strip()
            live_shader = source / 'subprojects/dxbc-spirv'
            modules = live_shader / '.gitmodules'
            original_modules = modules.read_bytes()
            modules.write_bytes(original_modules + b'\n# user edit\n')
            patches = project / 'patches'
            patches.mkdir()
            # This patch changes the same ordinary file in both repositories.
            for name in ('parent.patch', 'shader.patch'):
                (patches / name).write_bytes(self.patch.read_bytes())
            bundle = patches / 'bundle'
            bundle.write_bytes(b'not used: already at the pinned revision')
            (patches / 'sources.json').write_text(json.dumps({
                'version': 1, 'url': str(source), 'base': '0' * 40,
                'bundle': 'patches/bundle', 'bundle_sha256': digest(bundle),
                'repositories': [
                    {'path': 'triton-dxvk', 'commit': pin, 'patch': 'patches/parent.patch',
                     'sha256': digest(patches / 'parent.patch')},
                    {'path': 'triton-dxvk/subprojects/dxbc-spirv', 'commit': shader_pin,
                     'patch': 'patches/shader.patch', 'sha256': digest(patches / 'shader.patch')},
                ],
            }))
            self.assertFalse((live_shader / 'nested/.git').exists())
            with mock_patch.object(bootstrap, 'git', wraps=bootstrap.git) as calls:
                with self.assertRaisesRegex(RuntimeError, 'Divergent'):
                    bootstrap.bootstrap(project)
            updates = [call for call in calls.call_args_list
                       if call.args[1:3] == ('submodule', 'update')]
            self.assertEqual(updates, [])
            self.assertFalse((live_shader / 'nested/.git').exists())
            self.assertEqual(modules.read_bytes(), original_modules + b'\n# user edit\n')


if __name__ == '__main__':
    unittest.main()
