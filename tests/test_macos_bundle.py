#!/usr/bin/env python3
"""Verify an actual standalone bundle's identity, signature, and dependency closure."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--app', type=Path, required=True)
args = parser.parse_args()
app = args.app.resolve(strict=True)
contents = app / 'Contents'
plist = plistlib.loads((contents / 'Info.plist').read_bytes())
binary = contents / 'MacOS' / plist['CFBundleExecutable']
identity = json.loads(subprocess.check_output([str(binary), 'edition', '--json'], text=True))
assert plist['CFBundleIdentifier'] == 'com.superscreencast.' + identity['edition']
assert plist['CFBundleShortVersionString'] == identity['version']
assert (contents / 'Resources' / plist['CFBundleIconFile']).stat().st_size > 0
subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
manifest = json.loads((contents / 'Resources/runtime-libraries.json').read_text())
for name, metadata in manifest.items():
    library = contents / 'Frameworks' / name
    assert hashlib.sha256(library.read_bytes()).hexdigest() == metadata['sha256'], name
for path in [*list((contents / 'MacOS').iterdir()), *list((contents / 'Frameworks').iterdir())]:
    install_ids = subprocess.check_output(['otool', '-D', str(path)], text=True).splitlines()[1:]
    for line in subprocess.check_output(['otool', '-L', str(path)], text=True).splitlines()[1:]:
        dependency = line.strip().split(' (compatibility')[0]
        if dependency in install_ids or dependency.startswith(('/System/', '/usr/lib/')):
            continue
        assert dependency.startswith('@loader_path/'), (path, dependency)
        resolved = (path.parent / dependency[len('@loader_path/'):]).resolve(strict=True)
        assert resolved.is_relative_to(contents), (path, resolved)
print(f"Standalone {identity['edition']} {identity['version']}: identity, icon, signatures, hashes and library closure passed")
