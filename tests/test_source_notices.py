#!/usr/bin/env python3
"""CPU regression tests for supplementary source-notice preservation."""
import importlib.util
from pathlib import Path
import tempfile
import re
import unittest

spec = importlib.util.spec_from_file_location('notices', Path(__file__).resolve().parents[1] /
                                            'scripts/collect_source_notices.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SourceNotices(unittest.TestCase):
    def test_preserves_complete_multiline_notice(self):
        notice = '/* Copyright 2020 Alice\n * and Bob\n * Permission is hereby granted.\n */'
        self.assertEqual(module.notices((notice + '\nint x;').encode()), (notice,))

    def test_line_comments_and_spdx(self):
        text = '// Copyright Alice\n// continuation\nint x;\n# SPDX-License-Identifier: MIT\n'
        self.assertEqual(module.notices(text.encode()),
                         ('// Copyright Alice\n// continuation', '# SPDX-License-Identifier: MIT'))

    def test_unicode_and_bom(self):
        text = '/* Copyright © 2026 Example */'
        for encoding in ('utf-8-sig', 'utf-16'):
            self.assertEqual(module.notices(text.encode(encoding)), (text,))

    def test_no_license_invention(self):
        self.assertEqual(module.notices(b'/* A local helper. */\nint x;'), ())

    def test_deterministic_scope_and_deduplication(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for directory in module.ROOTS:
                (root / directory).mkdir(parents=True)
            for name in ('triton-kmd/z.c', 'triton-umd/a.h'):
                (root / name).write_text('/* Copyright Alice */\n')
            generated = root / 'triton-umd/build-vista-linux-x64'
            generated.mkdir()
            (generated / 'private.h').write_text('/* Copyright GENERATED */')
            source_build = root / 'triton-kmd/build'
            source_build.mkdir()
            (source_build / 'vendor.h').write_text('/* Copyright Vendor */')
            (root / 'packaging/link.h').symlink_to(generated / 'private.h')
            first = module.collect(root)
            self.assertEqual(first, module.collect(root))
            self.assertEqual(first.count('/* Copyright Alice */'), 1)
            self.assertIn('triton-kmd/z.c', first)
            self.assertIn('triton-umd/a.h', first)
            self.assertIn('/* Copyright Vendor */', first)
            self.assertNotIn('GENERATED', first)
            self.assertNotIn('link.h', first)

    def test_source_build_names_and_custom_meson_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for directory in module.ROOTS:
                (root / directory).mkdir(parents=True)
            support = root / 'triton-umd/build-support'
            support.mkdir()
            (support / 'build-helper.c').write_text('/* Copyright SourceSupport */')
            for name in ('build-linux', 'build-vista-linux-x86', 'custom-output'):
                directory = root / 'triton-umd' / name
                directory.mkdir()
                (directory / 'generated.c').write_text('/* Copyright GeneratedOutput */')
                if name == 'custom-output':
                    (directory / 'meson-private').mkdir()
                    (directory / 'meson-private/coredata.dat').touch()
            result = module.collect(root)
            self.assertIn('triton-umd/build-support/build-helper.c', result)
            self.assertIn('SourceSupport', result)
            self.assertNotIn('GeneratedOutput', result)

    def notice_tree(self, root):
        for directory in module.ROOTS:
            (root / directory).mkdir(parents=True)
        (root / 'packaging/service.c').write_text('/* Copyright Existing Author */')
        for relative in module.LICENSE_FILES.values():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('Existing notice: ' + relative)
        (root / 'triton-umd/licenses').mkdir()
        (root / 'triton-umd/licenses/MIT').write_text('Existing Mesa terms')

    def test_distribution_preserves_all_licenses_and_original_grant(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.notice_tree(root)
            output = root / 'distribution'
            module.assemble(root, output)
            for name, source in module.LICENSE_FILES.items():
                self.assertEqual((output / name).read_bytes(), (root / source).read_bytes())
            self.assertEqual((output / 'mesa-licenses/MIT').read_text(), 'Existing Mesa terms')
            self.assertIn('Existing Author', (output / 'SOURCE-NOTICES.txt').read_text())
            self.assertTrue((output / 'MIT-original.txt').is_file())
            self.assertTrue((output / 'LICENSE-scope.md').is_file())

    def test_packaged_document_links_resolve_and_sources_stay_unchanged(self):
        source_root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.notice_tree(root)
            originals = {}
            for name in ('LICENSE-scope.md', 'upstream-sources.md'):
                source = module.LICENSE_FILES[name]
                originals[source] = (source_root / source).read_bytes()
                (root / source).write_bytes(originals[source])
            output = root / 'distribution'
            module.assemble(root, output)
            local_links = []
            for name in ('LICENSE-scope.md', 'upstream-sources.md'):
                data = (output / name).read_text()
                for target in re.findall(r'\[[^\]]+\]\(([^)]+)\)', data):
                    if '://' not in target and not target.startswith('#'):
                        local_links.append(target)
                        self.assertTrue((output / target).is_file(), (name, target))
                source = module.LICENSE_FILES[name]
                self.assertEqual((root / source).read_bytes(), originals[source])
            self.assertEqual(sorted(local_links),
                             ['LICENSE-scope.md', 'MIT-original.txt',
                              'MIT-original.txt', 'upstream-sources.md'])
            # The complete MIT grant and third-party notices remain byte exact.
            for name, source in module.LICENSE_FILES.items():
                if name not in module.DISTRIBUTION_LINKS:
                    self.assertEqual((output / name).read_bytes(), (root / source).read_bytes())

    def test_distribution_requires_original_grant_before_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.notice_tree(root)
            (root / 'LICENSES/MIT-original.txt').unlink()
            output = root / 'distribution'
            with self.assertRaises(FileNotFoundError):
                module.assemble(root, output)
            self.assertFalse(output.exists())

    def test_distribution_refuses_existing_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.notice_tree(root)
            output = root / 'distribution'
            output.mkdir()
            (output / 'keep.txt').write_text('preserved')
            with self.assertRaises(FileExistsError):
                module.assemble(root, output)
            self.assertEqual((output / 'keep.txt').read_text(), 'preserved')

    def test_empty_or_missing_component_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaises(ValueError):
                module.collect(root)
            for directory in module.ROOTS:
                (root / directory).mkdir(parents=True)
            with self.assertRaises(ValueError):
                module.collect(root)


if __name__ == '__main__':
    unittest.main()
