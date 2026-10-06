#!/usr/bin/env python3
"""Official dynamic closure reviews must bind policy, release and exact artifacts."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('audit', ROOT / 'packaging/audit.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class RuntimeAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.release = self.root / 'pro/release'
        self.release.mkdir(parents=True)
        (self.root / 'pro/src').mkdir()
        (self.root / 'pro/src/pro_provider.c').write_text('fixture')
        self.binary = self.root / 'cast-pro'
        self.binary.write_bytes(b'exact reviewed executable')
        self.dynamic = ROOT / 'packaging/runtime-dynamic.json'
        self.requirements = json.loads(self.dynamic.read_text())['required_review']
        self.identity = {'version': '0.10.1', 'core_revision': 'a' * 40,
                         'private_revision': 'b' * 40, 'release_timestamp': 1800000000,
                         'platform': 'linux-x86_64', 'media_profile': 'lgpl'}
        self.evidence = {'schema': 1, 'reviewed': True, 'product': 'cast-pro',
                         'identity': self.identity, 'required_reviews': self.requirements,
                         'runtime_dynamic_sha256': audit.digest(self.dynamic),
                         'artifacts': {'cast-pro': audit.digest(self.binary)}}
        self.proof = {'schema': 1, 'reviewed': True, **self.identity,
                      'clean_runtime_acceptance_file': 'runtime-acceptance.json'}
        self.save()

    def save(self):
        evidence_file = self.release / 'runtime-acceptance.json'
        evidence_file.write_text(json.dumps(self.evidence))
        self.proof['clean_runtime_acceptance_reference'] = audit.digest(evidence_file)
        (self.release / 'production.json').write_text(json.dumps(self.proof))

    def accept(self):
        return audit.dynamic_acceptance(self.root, self.binary, self.dynamic, self.requirements)

    def test_exact_owner_acceptance_succeeds(self):
        with patch.object(audit, 'run', return_value=json.dumps(
                {'edition': 'pro', 'official': True, **self.identity})):
            result = self.accept()
        self.assertEqual(result['artifact_sha256'], audit.digest(self.binary))
        self.assertEqual(result['sha256'], self.proof['clean_runtime_acceptance_reference'])

    def test_missing_review_stays_closed(self):
        (self.release / 'runtime-acceptance.json').unlink()
        with self.assertRaises(OSError):
            self.accept()

    def test_changed_review_bytes_cannot_reuse_approval(self):
        (self.release / 'runtime-acceptance.json').write_text('{}')
        with self.assertRaisesRegex(ValueError, 'hash differs'):
            self.accept()

    def test_wrong_policy_release_and_artifact_fail_even_with_new_review_hash(self):
        for field in ('runtime_dynamic_sha256', 'required_reviews', 'identity', 'artifacts', 'reviewed'):
            original = self.evidence[field]
            self.evidence[field] = False if field == 'reviewed' else {} if field in ('identity', 'artifacts') else 'wrong'
            self.save()
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.accept()
            self.evidence[field] = original

    def test_compiled_release_must_match_review(self):
        with patch.object(audit, 'run', return_value=json.dumps(
                {'edition': 'pro', 'official': True, **self.identity, 'version': '9.99.0'})):
            with self.assertRaisesRegex(ValueError, 'compiled identity'):
                self.accept()

    def test_helper_covered_independently(self):
        self.binary = self.root / 'cast-pro-stt-helper'
        self.binary.write_bytes(b'exact reviewed helper')
        with self.assertRaisesRegex(ValueError, 'exact binary'):
            self.accept()
        self.evidence['artifacts'][self.binary.name] = audit.digest(self.binary)
        self.save()
        self.accept()
        self.binary.write_bytes(b'a different helper')
        with self.assertRaisesRegex(ValueError, 'exact binary'):
            self.accept()


if __name__ == '__main__':
    unittest.main()
