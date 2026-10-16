#!/usr/bin/env python3
"""Stage only hash-bound shared libraries from the verified speech profile."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--profile', type=Path, required=True)
parser.add_argument('--destination', type=Path, required=True)
args = parser.parse_args()
profile = json.loads(args.profile.read_text())
args.destination.mkdir(parents=True, exist_ok=True)
for item in profile['libraries']:
    source = Path(item['path'])
    if '.so' not in source.name and source.suffix != '.dylib':
        continue
    if hashlib.sha256(source.read_bytes()).hexdigest() != item['sha256']:
        raise ValueError('Speech library changed: ' + str(source))
    target = args.destination / source.name
    if target.is_symlink():
        raise ValueError('Speech destination must not be a symlink')
    # Dereference SDK SONAME links; each installed name has a bound content hash.
    shutil.copyfile(source, target)
    target.chmod(0o755)
