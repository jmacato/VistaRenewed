#!/usr/bin/env python3
"""CPU regression tests for supplementary source-notice preservation."""
import importlib.util
from pathlib import Path
import tempfile
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
