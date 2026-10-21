#!/usr/bin/env python3
"""Dependency cache failures must preserve existing trees and never use the network."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
lock = json.loads((ROOT / 'packaging/media-lgpl-lock.json').read_text())
with tempfile.TemporaryDirectory(prefix='cast-deps-fail-') as directory:
    base = Path(directory)
    tree, cache = base / 'media', base / 'cache'
    tree.mkdir()
    cache.mkdir()
    marker = tree / 'profile.json'
    marker.write_text('existing-good-tree\n')
    args = [str(ROOT / 'tools/deps-lgpl.sh'), '--offline', '--root', str(tree), '--cache', str(cache)]
    missing = subprocess.run(args, text=True, capture_output=True)
    assert missing.returncode != 0 and 'offline cache missing' in missing.stderr
    assert lock['sources'][0]['sha256'] in missing.stderr
    assert marker.read_text() == 'existing-good-tree\n'
    archive = cache / lock['sources'][0]['archive']
    archive.write_bytes(b'corrupt source fixture')
    invalid = subprocess.run(args, text=True, capture_output=True)
    assert invalid.returncode != 0 and 'cache SHA256 mismatch' in invalid.stderr
    assert archive.read_bytes() == b'corrupt source fixture'
    assert marker.read_text() == 'existing-good-tree\n'
    assert not (tree / 'sources').exists()
print('deps-lgpl missing/corrupt offline cache fails before mutation and preserves existing media')

# Exercise native-platform policy directly: never build, download or open devices.
import importlib.util
spec = importlib.util.spec_from_file_location('cast_deps_lgpl', ROOT / 'tools/deps-lgpl.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
with tempfile.TemporaryDirectory(prefix='cast-vt-policy-') as directory:
    existing = Path(directory)
    fresh = existing / 'candidate'
    assert builder.hardware_policy('none', False, existing, False) == ([], [])
    for darwin, destination, explicit in [(False, fresh, True), (True, existing, True), (True, fresh, False)]:
        try:
            builder.hardware_policy('videotoolbox', darwin, destination, explicit)
        except RuntimeError:
            pass
        else:
            raise AssertionError('unsafe candidate destination/platform accepted')
    flags, integrations = builder.hardware_policy('videotoolbox', True, fresh, True)
    assert flags == ['--enable-videotoolbox', '--enable-encoder=h264_videotoolbox']
    assert integrations[0]['qualification'] == 'unqualified-candidate'
    assert not fresh.exists()
assert not lock['hardware_integrations']
assert not any('videotoolbox' in flag for flag in lock['ffmpeg_flags'])
print('VideoToolbox candidate policy: native macOS, explicit fresh root, unchanged defaults and unqualified provenance passed')
spec = importlib.util.spec_from_file_location('cast_media_audit', ROOT / 'packaging/audit.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
candidate = {'platform': 'darwin', 'release_eligible': False,
             'hardware_integrations': integrations, 'configure_flags': flags}
audit.validate_hardware_profile(candidate, False)
for changed, official in [(candidate, True),
                          ({**candidate, 'release_eligible': True}, False),
                          ({**candidate, 'platform': 'linux'}, False),
                          ({**candidate, 'hardware_integrations': []}, False),
                          ({**candidate, 'configure_flags': []}, False)]:
    try:
        audit.validate_hardware_profile(changed, official)
    except ValueError:
        pass
    else:
        raise AssertionError('unqualified/inconsistent hardware provenance accepted')
audit.validate_hardware_profile({'configure_flags': lock['ffmpeg_flags']}, False)
print('VideoToolbox provenance rejects official release and inconsistent platform/flags/eligibility metadata')
