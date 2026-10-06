#!/usr/bin/env python3
"""Build contract, archive security and license-policy failures without private inputs."""
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import tempfile
import types
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'packaging/{name}.py')
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


boundary = module('public-boundary')
audit = module('audit')


class EditionBuildTests(unittest.TestCase):
    def make(self, *args):
        return subprocess.run(['make', '-n', 'X11=0', 'WAYLAND=0', 'PANEL=0', *args], cwd=ROOT,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=20)

    def test_community_requires_no_private_source_or_crypto(self):
        value = self.make('EDITION=community', 'PRO_ROOT=/not/a/private/checkout', 'all')
        self.assertEqual(value.returncode, 0, value.stderr)
        self.assertNotIn('-DWITH_PRO', value.stdout)
        self.assertNotIn('-lsodium', value.stdout)
        self.assertIn('community-x0-w0-p0-system-', value.stdout)

    def test_invalid_variables_fail_explicitly(self):
        for arg in ('EDITION=other', 'MEDIA_PROFILE=other', 'X11=2', 'WAYLAND=yes', 'PANEL=3'):
            with self.subTest(arg=arg):
                self.assertNotEqual(self.make(arg).returncode, 0)

    def test_pro_never_substitutes_community(self):
        value = self.make('EDITION=pro', 'MEDIA_PROFILE=system')
        self.assertNotEqual(value.returncode, 0)
        self.assertIn('Pro requires MEDIA_PROFILE=lgpl', value.stderr)
        value = self.make('EDITION=pro', 'MEDIA_PROFILE=lgpl', 'PRO_ROOT=/absent/cast-pro')
        self.assertNotEqual(value.returncode, 0)
        self.assertIn('private provider', value.stderr)
        value = self.make('EDITION=pro', 'MEDIA_PROFILE=lgpl', 'PRO_ROOT=relative')
        self.assertNotEqual(value.returncode, 0)
        self.assertIn('absolute', value.stderr)

    def test_public_archive_rejects_ignored_private_and_secret_files(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            (root / 'src').mkdir()
            (root / 'src/public.c').write_text('int public;')
            boundary.check(root)
            (root / 'cast-pro').mkdir()
            (root / 'cast-pro/private.c').write_text('private source')
            with self.assertRaises(ValueError): boundary.check(root)
            (root / 'cast-pro/private.c').unlink()
            (root / 'cast-pro').rmdir()
            (root / 'src/accidental.c').write_text('-----BEGIN ' + 'PRIVATE KEY-----')
            with self.assertRaises(ValueError): boundary.check(root)
            (root / 'src/accidental.c').unlink()
            (root / 'src/leak').symlink_to('/tmp')
            with self.assertRaises(ValueError): boundary.check(root)

    def test_local_prompts_are_not_public_source_files(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            (root / 'docs').mkdir()
            (root / 'local-prompt.md').write_text('Preserved local brief')
            (root / 'docs/nested-prompt.md').write_text('Preserved nested brief')
            (root / 'docs/prompt.md').write_text('Preserved generic brief')
            (root / 'README.md').write_text('Public project description')
            self.assertEqual([p.relative_to(root).as_posix() for p in boundary.public_files(root)], ['README.md'])
            self.assertTrue((root / 'local-prompt.md').exists())

    def test_profile_absence_fails_before_compilation(self):
        with tempfile.TemporaryDirectory() as d:
            args = types.SimpleNamespace(edition='community', media_profile='lgpl', lgpl_root=d,
                                         pro_root='/absent', official=0, release_timestamp=0)
            with self.assertRaisesRegex(ValueError, 'absent'):
                audit.profile(args)
            p = pathlib.Path(d) / 'profile.json'
            p.write_text(json.dumps({'schema': 1, 'profile': 'lgpl', 'configure_flags': ['--enable-gpl']}))
            with self.assertRaisesRegex(ValueError, 'exclude GPL'):
                audit.profile(args)

    def test_transitive_unknown_library_fails_pro_closed(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            binary = root / 'cast-pro'; binary.write_bytes(b'local executable')
            library = root / 'libunknown.so.1'; library.write_bytes(b'unknown runtime')
            original_run, original_package = audit.run, audit.package_metadata
            audit.run = lambda *a, **kw: f'libunknown.so.1 => {library} (0x1234)'
            audit.package_metadata = lambda p: ('unreviewed-sdk', {'Version': '1.0', 'Licenses': 'MIT OR Proprietary'})
            try:
                args = types.SimpleNamespace(binary=str(binary), edition='pro', media_profile='lgpl',
                                             lgpl_root=str(root / 'controlled'), output=str(root / 'audit'), strict=True, official=0)
                with self.assertRaisesRegex(ValueError, 'Pro dependency review failed'):
                    audit.inventory(args)
                inventory = json.loads((root / 'audit/inventory.json').read_text())
                self.assertEqual(inventory['runtime_libraries'][0]['chosen_license'], 'NOASSERTION')
                self.assertTrue(inventory['review_failures'])
            finally:
                audit.run, audit.package_metadata = original_run, original_package

    def test_permissive_alternatives_and_runtime_exceptions(self):
        self.assertEqual(audit.CHOICES['freetype2'], 'FTL')
        self.assertEqual(audit.CHOICES['glibc'], 'LGPL-2.1-or-later')
        self.assertIn('WITH GCC-exception-3.1', audit.CHOICES['libgcc'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
