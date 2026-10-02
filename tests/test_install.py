#!/usr/bin/env python3
"""Exercise the release installer and embedded updater without network or privileges."""
import hashlib
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import termios
import unittest

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = 'cast-0.2.0-archlinux-x86_64.pkg.tar.zst'
MANIFEST = 'cast-0.2.0-archlinux-x86_64-SHA256SUMS'
PAYLOAD = b'mock Arch Linux release package\n'
HASH = hashlib.sha256(PAYLOAD).hexdigest()

STUB = r'''#!/usr/bin/python3
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
with open(os.environ['CAST_TEST_LOG'], 'a') as stream:
    stream.write(json.dumps([name, *sys.argv[1:]]) + '\n')
if name == 'uname':
    print(os.environ.get('CAST_TEST_ARCH', 'x86_64'))
elif name == 'id':
    print(os.environ.get('CAST_TEST_UID', '1000'))
elif name == 'curl':
    args = sys.argv[1:]
    for flag, value in [('--proto', '=https'), ('--proto-redir', '=https')]:
        assert flag in args and args[args.index(flag) + 1] == value
    assert '--fail' in args and '--location' in args
    url = args[-1]
    assert url.startswith('https://github.com/mattmezza/cast/releases/')
    output = args[args.index('--output') + 1]
    if url.endswith('/latest'):
        if os.environ.get('CAST_TEST_LATEST_FAIL'):
            sys.exit(22)
        print(os.environ.get('CAST_TEST_LATEST_URL',
              'https://github.com/mattmezza/cast/releases/tag/v0.2'), end='')
    else:
        asset = url.rsplit('/', 1)[-1]
        if asset == os.environ.get('CAST_TEST_FAIL_ASSET'):
            sys.exit(22)
        pathlib.Path(output).write_bytes((pathlib.Path(os.environ['CAST_TEST_FIXTURE']) / asset).read_bytes())
elif name == 'sudo':
    assert sys.argv[1:3] == ['pacman', '-U']
    assert os.isatty(0), 'pacman confirmation must have terminal stdin'
    sys.exit(int(os.environ.get('CAST_TEST_PACMAN_STATUS', '0')))
elif name == 'pacman':
    assert sys.argv[1:3] == ['-U', '--']
    assert os.isatty(0), 'pacman confirmation must have terminal stdin'
    sys.exit(int(os.environ.get('CAST_TEST_PACMAN_STATUS', '0')))
else:
    raise AssertionError(name)
'''


class InstallerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='cast-updater-build-')
        directory = Path(cls.build.name)
        harness = directory / 'update_main.c'
        harness.write_text('#include "update.h"\n#include <stdio.h>\n'
                           'int main(int argc, char **argv) { char error[1024];\n'
                           'int result = cast_update(argc - 1, argv + 1, error, sizeof error);\n'
                           'if (result) fprintf(stderr, "%s\\n", error);\n'
                           'return result < 0 ? 1 : result; }\n')
        assembly = directory / 'install.S'
        assembly.write_text('.section .rodata\n'
                            '.global cast_install_script_start\n'
                            '.global cast_install_script_end\n'
                            'cast_install_script_start:\n'
                            f'.incbin "{ROOT / "packaging/install.sh"}"\n'
                            'cast_install_script_end:\n'
                            '.section .note.GNU-stack,""\n')
        cls.updater = directory / 'cast-updater'
        subprocess.run(['cc', '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'src'), str(ROOT / 'src/update.c'), str(harness),
                        str(assembly), '-o', str(cls.updater)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='cast-install-test-')
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.bin = self.directory / 'bin'
        self.bin.mkdir()
        self.fixture = self.directory / 'fixture'
        self.fixture.mkdir()
        (self.fixture / PACKAGE).write_bytes(PAYLOAD)
        (self.fixture / MANIFEST).write_text(f'{HASH}  {PACKAGE}\n')
        self.log = self.directory / 'calls.jsonl'
        self.env = os.environ.copy()
        self.env.update(PATH=str(self.bin), TMPDIR=str(self.directory),
                        CAST_TEST_LOG=str(self.log), CAST_TEST_FIXTURE=str(self.fixture))
        for name in ('curl', 'uname', 'id', 'pacman', 'sudo'):
            path = self.bin / name
            path.write_text(STUB)
            path.chmod(0o755)
        for name in ('awk', 'mktemp', 'sha256sum', 'rm', 'mkdir', 'cp'):
            (self.bin / name).symlink_to(shutil.which(name))
        self.output = self.directory / 'download with spaces $(touch injection)'

    def run_install(self, *arguments, embedded=False, terminal=False, piped=False):
        command = [str(self.updater), 'update'] if embedded else ['/bin/sh', str(ROOT / 'packaging/install.sh')]
        if piped:
            command = ['/bin/sh', '-s', '--']
        options = dict(env=self.env, cwd=self.directory, capture_output=True, text=True, timeout=10)
        if terminal:
            master, slave = os.openpty()
            def attach_terminal():
                os.setsid()
                fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            try:
                if piped:
                    options['input'] = (ROOT / 'packaging/install.sh').read_text()
                else:
                    options['stdin'] = slave
                return subprocess.run([*command, *map(str, arguments)], pass_fds=(slave,),
                                      preexec_fn=attach_terminal, **options)
            finally:
                os.close(slave)
                os.close(master)
        return subprocess.run([*command, *map(str, arguments)], stdin=subprocess.DEVNULL,
                              start_new_session=True, **options)

    def calls(self, name=None):
        calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return [call for call in calls if name is None or call[0] == name]

    def assert_failed_cleanly(self, result, text):
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(text, result.stderr)
        self.assertFalse(self.calls('sudo'))
        self.assertFalse(self.calls('pacman'))
        self.assertFalse(list(self.directory.glob('cast-install.*')))

    def test_tag_alias_and_verified_download(self):
        result = self.run_install('v0.2', '--download-only', self.output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / PACKAGE).read_bytes(), PAYLOAD)
        self.assertEqual((self.output / MANIFEST).read_text(), f'{HASH}  {PACKAGE}\n')
        self.assertTrue(all('/v0.2/' in call[-1] for call in self.calls('curl')))
        self.assertFalse(self.calls('sudo'))
        self.assertFalse(list(self.directory.glob('cast-install.*')))
        self.assertFalse((self.directory / 'injection').exists())

    def test_patch_version_and_relocated_embedded_updater(self):
        moved = self.directory / 'cast'
        shutil.copy2(self.updater, moved)
        result = subprocess.run([str(moved), 'update', 'v0.2.0', '--download-only', str(self.output)],
                                env=self.env, cwd='/', capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / PACKAGE).read_bytes(), PAYLOAD)
        self.assertTrue(all('/v0.2.0/' in call[-1] for call in self.calls('curl')))

    def test_latest_resolves_only_official_valid_tag(self):
        result = self.run_install('--download-only', self.output, embedded=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.calls('curl')[0][-1].endswith('/latest'))
        self.env['CAST_TEST_LATEST_URL'] = 'https://example.com/releases/tag/v0.2'
        result = self.run_install('--download-only', self.output)
        self.assert_failed_cleanly(result, 'official release tag')
        self.env['CAST_TEST_LATEST_URL'] = 'https://github.com/mattmezza/cast/releases/tag/v0.2/../../evil'
        result = self.run_install('--download-only', self.output)
        self.assert_failed_cleanly(result, 'unsupported version tag')

    def test_invalid_tags_and_arguments_never_download(self):
        for arguments in [('v0.2;touch bad',), ('../v0.2',), ('v0',), ('v0..2',),
                          ('v0.2.0.1',), ('--download-only',), ('v0.2', 'v0.3')]:
            for embedded in (False, True):
                with self.subTest(arguments=arguments, embedded=embedded):
                    result = self.run_install(*arguments, embedded=embedded)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(self.calls('curl'))

    def test_tampered_package_and_invalid_manifests(self):
        (self.fixture / PACKAGE).write_bytes(b'tampered')
        self.assert_failed_cleanly(self.run_install('v0.2', '--download-only', self.output), 'checksum mismatch')
        (self.fixture / PACKAGE).write_bytes(PAYLOAD)
        for manifest in (f'bad  {PACKAGE}\n', f'{HASH}  ../{PACKAGE}\n',
                         f'{HASH}  {PACKAGE}\n{HASH}  {PACKAGE}\n',
                         f'{HASH}  {PACKAGE} extra\n'):
            with self.subTest(manifest=manifest):
                (self.fixture / MANIFEST).write_text(manifest)
                self.assert_failed_cleanly(self.run_install('v0.2', '--download-only', self.output), 'exactly one valid checksum')

    def test_manifest_does_not_check_unrelated_paths(self):
        (self.fixture / MANIFEST).write_text(f'{HASH}  ../../untrusted-file\n{HASH.upper()}  {PACKAGE}\n')
        result = self.run_install('v0.2', '--download-only', self.output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / MANIFEST).read_text(), f'{HASH}  {PACKAGE}\n')

    def test_download_failures_cleanup(self):
        for asset, message in ((MANIFEST, 'checksum download failed'), (PACKAGE, 'package download failed')):
            self.env['CAST_TEST_FAIL_ASSET'] = asset
            self.assert_failed_cleanly(self.run_install('v0.2', '--download-only', self.output, embedded=True), message)

    def test_architecture_and_dependency_preflight(self):
        self.env['CAST_TEST_ARCH'] = 'aarch64'
        self.assert_failed_cleanly(self.run_install('v0.2', '--download-only', self.output), 'x86_64 only')
        self.env['CAST_TEST_ARCH'] = 'x86_64'
        (self.bin / 'curl').unlink()
        self.assert_failed_cleanly(self.run_install('v0.2', '--download-only', self.output), 'required command missing: curl')

    @unittest.skipUnless(Path('/etc/arch-release').is_file(), 'installation preflight requires an Arch test host')
    def test_privilege_routing_and_pacman_failure(self):
        result = self.run_install('v0.2', terminal=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls('sudo')[0][1:4], ['pacman', '-U', '--'])
        self.assertIn('Stop the cast daemon', result.stdout)
        self.env['CAST_TEST_UID'] = '0'
        result = self.run_install('v0.2', terminal=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.calls('sudo')), 1)
        self.assertEqual(self.calls('pacman')[0][1:3], ['-U', '--'])
        self.env['CAST_TEST_PACMAN_STATUS'] = '7'
        result = self.run_install('v0.2', embedded=True, terminal=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('pacman installation failed', result.stderr)
        self.assertIn('installer exited with status 1', result.stderr)
        self.assertFalse(list(self.directory.glob('cast-install.*')))

    @unittest.skipUnless(Path('/etc/arch-release').is_file(), 'installation preflight requires an Arch test host')
    def test_missing_sudo_fails_before_download(self):
        (self.bin / 'sudo').unlink()
        self.assert_failed_cleanly(self.run_install('v0.2'), 'requires sudo')
        self.assertFalse(self.calls('curl'))

    @unittest.skipUnless(Path('/etc/arch-release').is_file(), 'installation preflight requires an Arch test host')
    def test_missing_terminal_fails_before_download(self):
        self.assert_failed_cleanly(self.run_install('v0.2', embedded=True), 'needs an interactive terminal')
        self.assertFalse(self.calls('curl'))

    @unittest.skipUnless(Path('/etc/arch-release').is_file(), 'installation preflight requires an Arch test host')
    def test_piped_script_keeps_pacman_confirmation_on_terminal(self):
        result = self.run_install('v0.2', terminal=True, piped=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.calls('sudo')), 1)

    def test_help_is_offline(self):
        result = self.run_install('--help', embedded=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Usage:', result.stdout)
        self.assertFalse(self.calls())


if __name__ == '__main__':
    unittest.main(verbosity=2)
