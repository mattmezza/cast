#!/usr/bin/env python3
"""Exercise actual private signatures and atomic public updater without media/capture.
Explicit opt-in private checkout and nonproduction keys; nothing copied into public source.
"""
import argparse
import fcntl
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
PRIVATE = None


class ProUpdateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='cast-pro-update-tests-')
        cls.root = pathlib.Path(cls.temp.name)
        generated = PRIVATE / 'tests/generated'
        if not (generated / 'trusted_keys.h').is_file():
            raise RuntimeError('run the private nonproduction verifier tests to provision explicit test keys first')
        cls.generated = generated
        harness = cls.root / 'main.c'
        harness.write_text('#include "update.h"\n#include <stdio.h>\n'
                           'int main(int n,char **v){char e[1024];if(n<4)return 2;'
                           'int r=cast_update_with_context(n-3,v+3,v[1],v[2],e,sizeof e);'
                           'if(r)fprintf(stderr,"%s\\n",e);return r<0?5:r;}\n')
        assembly = cls.root / 'assets.S'
        assembly.write_text('.section .rodata\n.global cast_install_script_start\n.global cast_install_script_end\n'
                            'cast_install_script_start:\n.byte 0\ncast_install_script_end:\n'
                            '.section .note.GNU-stack,""\n')
        cls.binary = cls.root / 'updater'
        subprocess.run(['cc', '-D_GNU_SOURCE', '-DWITH_PRO', '-DCAST_PLATFORM="linux"',
                        '-DCAST_RELEASE_TIMESTAMP=100', '-DCAST_MEDIA_PROFILE="lgpl"',
                        f'-DCAST_TRUST_HEADER="{generated / "trusted_keys.h"}"',
                        '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'src'),
                        '-I', str(PRIVATE / 'src'), str(harness), str(assembly),
                        str(ROOT / 'src/update.c'), str(ROOT / 'src/edition.c'),
                        str(ROOT / 'src/license_store.c'), str(ROOT / 'src/edition_extensions.c'),
                        str(ROOT / 'src/workflow_schema.c'),
                        str(PRIVATE / 'tests/verifier_provider.c'),
                        str(PRIVATE / 'src/license_verifier.c'),
                        str(PRIVATE / 'src/bounded_json.c'), '-lsodium', '-lm', '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=self.root)
        self.addCleanup(self.tmp.cleanup)
        self.path = pathlib.Path(self.tmp.name)
        self.license = self.path / 'license.json'
        shutil.copyfile(self.generated / 'perpetual.json', self.license)
        self.license.chmod(0o600)
        self.socket = self.path / 'cast-pro.sock'
        self.bundle = self.path / 'bundle'; self.bundle.mkdir()
        self.payload = b'nonproduction signed executable fixture\n'
        self.target = self.path / 'cast-pro'
        self.target.write_bytes(b'old working executable\n'); self.target.chmod(0o755)
        self.make_bundle()

    def make_bundle(self, **changes):
        (self.bundle / 'cast-pro').write_bytes(self.payload)
        claims = {'schema': 1, 'product': 'cast-pro', 'key_id': 'test-update-1',
                  'version': '0.9.0-test', 'platform': 'linux', 'media_profile': 'lgpl',
                  'extension_api': 2, 'release_timestamp': 250, 'core_revision': 'working-tree',
                  'private_revision': 'test-scaffold', 'sha256': hashlib.sha256(self.payload).hexdigest()}
        claims.update(changes)
        cp = self.path / 'claims.json'; cp.write_text(json.dumps(claims))
        mf = self.bundle / 'manifest.json'; mf.unlink(missing_ok=True)
        subprocess.run(['python3', str(PRIVATE / 'tools/issuer.py'), 'issue', '--key',
                        str(self.generated / 'test-update-1.secret.json'), '--claims', str(cp),
                        '--out', str(mf)], check=True, stdout=subprocess.DEVNULL)

    def update(self, *args):
        return subprocess.run([str(self.binary), str(self.license), str(self.socket), 'update',
                               *map(str, args)], text=True, capture_output=True, timeout=10)

    def test_verify_does_not_replace_or_start_daemon(self):
        value = self.update('--bundle', self.bundle)
        self.assertEqual(value.returncode, 0, value.stderr)
        self.assertEqual(self.target.read_bytes(), b'old working executable\n')
        self.assertFalse(self.socket.exists())

    def test_install_is_atomic_with_rollback_backup(self):
        value = self.update('--bundle', self.bundle, '--install', self.target)
        self.assertEqual(value.returncode, 0, value.stderr)
        self.assertEqual(self.target.read_bytes(), self.payload)
        self.assertEqual((self.path / 'cast-pro.rollback').read_bytes(), b'old working executable\n')
        self.assertEqual(os.stat(self.target).st_mode & 0o777, 0o755)
        self.assertFalse(list(self.path.glob('.cast-pro-update-*')))

    def test_running_daemon_session_prevents_replace(self):
        with open(str(self.socket) + '.lock', 'w') as f:
            os.chmod(f.name, 0o600); fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            value = self.update('--bundle', self.bundle, '--install', self.target)
        self.assertNotEqual(value.returncode, 0)
        self.assertIn('stop the Cast Pro daemon', value.stderr)
        self.assertEqual(self.target.read_bytes(), b'old working executable\n')

    def test_ineligible_release_refused_install_download_warns(self):
        self.make_bundle(release_timestamp=350)
        value = self.update('--bundle', self.bundle, '--install', self.target)
        self.assertEqual(value.returncode, 4, value.stderr)
        self.assertEqual(self.target.read_bytes(), b'old working executable\n')
        destination = self.path / 'download'; destination.mkdir()
        value = self.update('--bundle', self.bundle, '--download-only', destination)
        self.assertEqual(value.returncode, 0, value.stderr)
        self.assertIn('download only', value.stderr)
        self.assertEqual((destination / 'cast-pro').read_bytes(), self.payload)
        self.assertTrue((destination / 'manifest.json').is_file())

    def test_tampered_hash_and_wrong_platform_fail_without_replace(self):
        (self.bundle / 'cast-pro').write_bytes(b'tampered')
        value = self.update('--bundle', self.bundle, '--install', self.target)
        self.assertNotEqual(value.returncode, 0)
        self.assertEqual(self.target.read_bytes(), b'old working executable\n')
        self.make_bundle(platform='windows')
        self.assertNotEqual(self.update('--bundle', self.bundle, '--install', self.target).returncode, 0)
        self.assertEqual(self.target.read_bytes(), b'old working executable\n')

    def test_special_bundle_files_fail_without_blocking(self):
        artifact = self.bundle / 'cast-pro'
        artifact.unlink(); os.mkfifo(artifact)
        self.assertNotEqual(self.update('--bundle', self.bundle, '--install', self.target).returncode, 0)
        artifact.unlink(); artifact.write_bytes(self.payload)
        manifest = self.bundle / 'manifest.json'
        manifest.unlink(); os.mkfifo(manifest)
        self.assertNotEqual(self.update('--bundle', self.bundle, '--install', self.target).returncode, 0)
        self.assertEqual(self.target.read_bytes(), b'old working executable\n')

    def test_update_and_license_domains_are_distinct(self):
        shutil.copyfile(self.generated / 'perpetual.json', self.bundle / 'manifest.json')
        self.assertNotEqual(self.update('--bundle', self.bundle).returncode, 0)

    def test_rollback_must_be_explicit_and_still_authenticated(self):
        self.make_bundle(release_timestamp=50)
        value = self.update('--bundle', self.bundle, '--install', self.target)
        self.assertEqual(value.returncode, 4)
        self.assertEqual(self.target.read_bytes(), b'old working executable\n')
        value = self.update('--bundle', self.bundle, '--install', self.target, '--rollback')
        self.assertEqual(value.returncode, 0, value.stderr)

    def test_symlink_and_backup_conflicts_preserve_working_files(self):
        protected = self.path / 'protected'; protected.write_bytes(b'private data')
        self.target.unlink(); self.target.symlink_to(protected)
        self.assertNotEqual(self.update('--bundle', self.bundle, '--install', self.target).returncode, 0)
        self.assertEqual(protected.read_bytes(), b'private data')
        self.target.unlink(); self.target.write_bytes(b'old'); self.target.chmod(0o755)
        (self.path / 'cast-pro.rollback').write_bytes(b'existing backup')
        self.assertNotEqual(self.update('--bundle', self.bundle, '--install', self.target).returncode, 0)
        self.assertEqual(self.target.read_bytes(), b'old')

    def test_absent_channel_never_runs_community_installer(self):
        value = self.update()
        self.assertEqual(value.returncode, 5)
        self.assertIn('hosted Pro updates are unavailable', value.stderr)
        self.assertNotEqual(self.update('v0.9').returncode, 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pro-root', type=pathlib.Path, required=True)
    args, rest = parser.parse_known_args()
    PRIVATE = args.pro_root.resolve()
    unittest.main(argv=[__file__, *rest], verbosity=2)
