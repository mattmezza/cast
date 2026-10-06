#!/usr/bin/env python3
"""Speech candidates never become official by setting a manifest boolean."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import io
import tarfile
ROOT = Path(__file__).resolve().parents[1]
class ProfileTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.library = self.root / 'libwhisper.so'
        self.library.write_bytes(b'fixture library, not a production SDK')
        self.profile = self.root / 'profile.json'
        self.data = {'schema': 1, 'profile': 'cast-pro-speech-cpu', 'cpu_only': True,
                     'release_eligible': False, 'cflags': '-Ifixture', 'libs': '-lwhisper',
                     'cpu_backend': 'fixture-cpu', 'libraries': [{'path': str(self.library),
                     'sha256': hashlib.sha256(self.library.read_bytes()).hexdigest(),
                     'chosen_license': 'MIT'}]}
    def tearDown(self):
        self.temp.cleanup()
    def check(self, official=0, engineering=1, **changes):
        self.data.update(changes)
        self.profile.write_text(json.dumps(self.data))
        return subprocess.run(['python3', str(ROOT / 'packaging/speech-profile.py'), 'verify',
            '--profile', str(self.profile), '--official', str(official), '--engineering', str(engineering),
            '--cpu-backend', 'fixture-cpu', '--cflags=-Ifixture', '--libs=-lwhisper'], capture_output=True, text=True)
    def test_explicit_candidate_guard(self):
        self.assertEqual(self.check().returncode, 0)
        self.assertNotEqual(self.check(engineering=0).returncode, 0)
    def test_official_always_rejects_uncontrolled_sdk(self):
        self.assertNotEqual(self.check(official=1).returncode, 0)
        self.assertNotEqual(self.check(official=1, release_eligible=True).returncode, 0)
    def test_changed_sdk_rejected(self):
        self.library.write_bytes(b'changed library')
        self.assertNotEqual(self.check().returncode, 0)
    def test_unknown_or_gpl_license_rejected(self):
        self.data['libraries'][0]['chosen_license'] = 'GPL-3.0-only'
        self.assertNotEqual(self.check().returncode, 0)
        self.data['libraries'][0]['chosen_license'] = 'NOASSERTION'
        self.assertNotEqual(self.check().returncode, 0)
    def test_gpu_or_flags_rejected(self):
        self.assertNotEqual(self.check(cpu_only=False).returncode, 0)
        self.assertNotEqual(self.check(cpu_only=True, libs='-lother').returncode, 0)
    def test_offline_builder_plan_and_absent_cache(self):
        command = ['python3', str(ROOT / 'tools/deps-stt.py')]
        result = subprocess.run(command + ['--print-plan'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        plan = json.loads(result.stdout)
        self.assertTrue(plan['cache_only'])
        self.assertFalse(plan['release_eligible'])
        self.assertEqual(plan['options']['GGML_BACKEND_DL'], 'OFF')
        self.assertEqual(plan['options']['WHISPER_USE_SYSTEM_GGML'], 'OFF')
        output = self.root / 'unbuilt'
        result = subprocess.run(command + ['--source-git', str(self.root / 'absent'),
             '--output', str(output)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no network fetch', result.stderr)
        self.assertFalse(output.exists())

class ControlledTest(unittest.TestCase):
    """Structural verifier tests use explicit nonproduction mock archives/evidence.

    These do not claim the pinned SDK was built or inference acceptance passed.
    """
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.profile = self.root / 'profile.json'
        def digest(path):
            return hashlib.sha256(path.read_bytes()).hexdigest()
        self.digest = digest
        lock_path = ROOT / 'packaging/speech-cpu-lock.json'
        recipe = ROOT / 'tools/deps-stt.py'
        lock = json.loads(lock_path.read_text())
        entries = []
        for name in ['libwhisper.a', 'libggml.a', 'libggml-base.a', 'libggml-cpu.a']:
            library = self.root / name
            library.write_bytes(b'!<arch>\n')
            entries.append({'path': str(library), 'sha256': digest(library), 'chosen_license': 'MIT'})
        smoke = self.root / 'cpu-smoke-result.txt'
        smoke.write_text('CPU_ONLY devices=1\nTEST-ONLY structural evidence\n')
        source_bytes = b'TEST-ONLY source materialization, not an upstream SDK'
        kit = self.root / 'sources.tar.gz'
        with tarfile.open(kit, 'w:gz') as archive:
            for name, contents in [(f'whisper.cpp-{lock["commit"]}.tar', source_bytes),
                    ('build-materials/deps-stt.py', recipe.read_bytes()),
                    ('build-materials/speech-cpu-lock.json', lock_path.read_bytes())]:
                item = tarfile.TarInfo(name)
                item.size = len(contents)
                archive.addfile(item, io.BytesIO(contents))
        self.data = {'schema': 1, 'profile': 'cast-pro-speech-cpu', 'build_kind': 'controlled-source',
             'cpu_only': True, 'release_eligible': False, 'cpu_backend': '',
             'source_commit': lock['commit'], 'source_archive_sha256': hashlib.sha256(source_bytes).hexdigest(),
             'lock_sha256': digest(lock_path), 'recipe_sha256': digest(recipe),
             'whisper_version': lock['version'], 'ggml_version': lock['ggml']['version'],
             'libraries': entries, 'build_options': lock['build_options'], 'cflags': '', 'libs': '-lfixture',
             'corresponding_sources': str(kit), 'corresponding_sources_sha256': digest(kit),
             'cpu_smoke_result': str(smoke), 'cpu_smoke_sha256': digest(smoke)}
    def tearDown(self):
        self.temp.cleanup()
    def check(self, official=0):
        self.profile.write_text(json.dumps(self.data))
        return subprocess.run(['python3', str(ROOT / 'packaging/speech-profile.py'), 'verify',
           '--profile', str(self.profile), '--official', str(official), '--engineering=1',
           '--libs=-lfixture'], capture_output=True, text=True)
    def test_controlled_local_evidence_and_pending_review(self):
        self.assertEqual(self.check().returncode, 0)
        self.assertNotEqual(self.check(official=1).returncode, 0)
        self.data['release_eligible'] = True
        self.assertNotEqual(self.check(official=1).returncode, 0)
    def test_exact_pin_options_and_sourcekit(self):
        self.data['source_commit'] = '0' * 40
        self.assertNotEqual(self.check().returncode, 0)
        self.data['source_commit'] = json.loads((ROOT / 'packaging/speech-cpu-lock.json').read_text())['commit']
        self.data['build_options']['GGML_VULKAN'] = 'ON'
        self.assertNotEqual(self.check().returncode, 0)
    def test_artifact_changes_fail_closed(self):
        Path(self.data['corresponding_sources']).write_bytes(b'changed')
        self.assertNotEqual(self.check().returncode, 0)
    def test_qualify_requires_exact_review_artifacts(self):
        fields = ('source_commit', 'source_archive_sha256', 'lock_sha256', 'recipe_sha256',
             'whisper_version', 'ggml_version', 'cflags', 'libs', 'libraries', 'build_options',
             'cpu_smoke_sha256', 'corresponding_sources_sha256')
        identity = hashlib.sha256(json.dumps({key: self.data.get(key) for key in fields},
             sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        acceptance = {}
        for name in ['cpu_model_acceptance', 'signed_helper_boundary', 'full_application',
                     'source_license_inventory', 'runtime_closure']:
            proof = self.root / (name + '.txt')
            proof.write_text('TEST-ONLY structural acceptance record\n')
            acceptance[name] = {'path': str(proof), 'sha256': self.digest(proof), 'passed': True}
        record = self.root / 'review.json'
        record.write_text(json.dumps({'schema': 1, 'sdk_sha256': identity,
                 'reviewed_by': 'TEST-ONLY fixture owner', 'acceptance': acceptance}))
        self.profile.write_text(json.dumps(self.data))
        command = ['python3', str(ROOT / 'packaging/speech-profile.py'), 'qualify',
                   '--profile', str(self.profile), '--review-record', str(record)]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
        self.data = json.loads(self.profile.read_text())
        self.assertEqual(self.check(official=1).returncode, 0)
        Path(acceptance['full_application']['path']).write_text('changed')
        self.assertNotEqual(self.check(official=1).returncode, 0)
if __name__ == '__main__':
    unittest.main()
