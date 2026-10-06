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
