#!/usr/bin/env python3
"""Structural safety tests; fake local bytes never claim upstream inference proof."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NeedleProfileTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        for folder in ('tools', 'packaging', 'sdk'):
            (self.root / folder).mkdir()
        for name in ('tools/deps-stt.py', 'packaging/speech-profile.py'):
            shutil.copyfile(ROOT / name, self.root / name)
        lock = json.loads((ROOT / 'packaging/speech-needle-lock.json').read_text())
        for name, key, content in [('needle.h', 'header_sha256', b'TEST ONLY HEADER'),
                                    ('LICENSE', 'license_sha256', b'TEST ONLY LICENSE')]:
            (self.root / 'sdk' / name).write_bytes(content)
            lock[key] = hashlib.sha256(content).hexdigest()
        archive = b'TEST ONLY STATIC ARCHIVE; NOT AN UPSTREAM SDK'
        (self.root / 'sdk/libneedle.a').write_bytes(archive)
        lock['platforms']['linux-x86_64']['sha256'] = hashlib.sha256(archive).hexdigest()
        lock['platforms']['linux-x86_64']['size'] = len(archive)
        (self.root / 'packaging/speech-needle-lock.json').write_text(json.dumps(lock))
        self.command = ['python3', str(self.root / 'tools/deps-stt.py'), '--platform=linux-x86_64',
                        '--sdk-dir', str(self.root / 'sdk'), '--output', str(self.root / 'out')]
        self.assertEqual(subprocess.run(self.command, capture_output=True).returncode, 0)
        self.path = self.root / 'out/profile.json'
        self.profile = json.loads(self.path.read_text())

    def tearDown(self):
        self.tmp.cleanup()

    def check(self, *extra):
        self.path.write_text(json.dumps(self.profile))
        return subprocess.run(['python3', str(self.root / 'packaging/speech-profile.py'), 'verify',
            '--profile', str(self.path), '--engineering=1', '--cflags=' + self.profile['cflags'],
            '--libs=' + self.profile['libs'], *extra], capture_output=True)

    def test_engineering_only_and_official_rejected(self):
        self.assertEqual(self.check().returncode, 0)
        self.assertNotEqual(self.check('--official=1').returncode, 0)
        self.assertNotEqual(self.check('--engineering=0').returncode, 0)
        self.profile['release_eligible'] = True
        self.assertNotEqual(self.check().returncode, 0)

    def test_header_and_archive_tampering(self):
        for field in ('header', 'license'):
            path = Path(self.profile[field]['path']); original = path.read_bytes()
            path.write_bytes(b'changed')
            self.assertNotEqual(self.check().returncode, 0)
            path.write_bytes(original)
        Path(self.profile['libraries'][0]['path']).write_bytes(b'changed')
        self.assertNotEqual(self.check().returncode, 0)

    def test_cannot_claim_source_provenance_or_extra_libraries(self):
        self.profile['source_available'] = True
        self.assertNotEqual(self.check().returncode, 0)
        self.profile['source_available'] = False
        self.profile['libraries'].append(self.profile['libraries'][0].copy())
        self.assertNotEqual(self.check().returncode, 0)

    def test_cannot_qualify_or_reuse_output(self):
        result = subprocess.run(['python3', str(self.root / 'packaging/speech-profile.py'), 'qualify',
            '--profile', str(self.path), '--review-record', str(self.root / 'nonexistent')], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotEqual(subprocess.run(self.command, capture_output=True).returncode, 0)

    def test_offline_import_rejects_changed_inputs(self):
        (self.root / 'sdk/needle.h').write_bytes(b'changed')
        self.command[-1] = str(self.root / 'other-output')
        self.assertNotEqual(subprocess.run(self.command, capture_output=True).returncode, 0)
        self.assertFalse((self.root / 'other-output').exists())


if __name__ == '__main__':
    unittest.main()
