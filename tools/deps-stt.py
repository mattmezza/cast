#!/usr/bin/env python3
"""Import a hash-pinned OFFLINE Needle/Whistle binary engineering SDK.

Never downloads, source-builds, or qualifies a release. The historical Whisper
source recipe remains tools/deps-stt-whisper.py for existing evidence only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shlex
import shutil
import sys
import zipfile
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'packaging/speech-needle-lock.json'


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def checked(path, expected):
    if not path.is_file() or path.is_symlink() or sha(path) != expected:
        raise ValueError(f'missing, symlinked or changed pinned artifact: {path}')


def build(args):
    lock = json.loads(LOCK.read_text())
    target_platform = args.platform or {'Linux': 'linux', 'Darwin': 'macos'}.get(platform.system(), '') + '-' + {'aarch64': 'arm64'}.get(platform.machine(), platform.machine())
    if target_platform not in lock['platforms']:
        raise ValueError('unsupported pinned SDK platform: ' + target_platform)
    if args.print_plan:
        print(json.dumps({**lock, 'platform': target_platform, 'offline_only': True,
                          'required_local_files': ['libneedle.a', 'needle.h', 'LICENSE'],
                          'optional_model': 'whistle.cact'}, indent=2))
        return
    if not args.sdk_dir:
        raise ValueError('--sdk-dir must contain pinned libneedle.a, needle.h and LICENSE; no network fetch is attempted')
    shared = args.wheel is not None
    if shared and target_platform != 'linux-x86_64':
        raise ValueError('shared wheel recipe is pinned only for Linux x86_64')
    if shared:
        checked(args.wheel, lock['linux_shared']['wheel_sha256'])
    sources = {'needle.h': lock['header_sha256'], 'LICENSE': lock['license_sha256']}
    if not shared:
        sources['libneedle.a'] = lock['platforms'][target_platform]['sha256']
    # Verify every supplied upstream artifact before creating output.
    for name, digest in sources.items():
        checked(args.sdk_dir / name, digest)
    if args.model:
        checked(args.model, lock['model_sha256'])
    output = args.output.resolve()
    if output.exists():
        raise ValueError('output must be new; refusing to mix SDK evidence')
    output.mkdir(parents=True)
    include, lib, notices = output / 'prefix/include', output / 'prefix/lib', output / 'license-texts'
    for directory in (include, lib, notices):
        directory.mkdir(parents=True)
    for name, destination in [('needle.h', include / 'needle.h'), ('LICENSE', notices / 'Needle-Apache-2.0.txt')]:
        shutil.copyfile(args.sdk_dir / name, destination)
    if shared:
        with zipfile.ZipFile(args.wheel) as wheel:
            payload = wheel.read(lock['linux_shared']['member'])
        if hashlib.sha256(payload).hexdigest() != lock['linux_shared']['sha256']:
            raise ValueError('wheel member differs from pinned Needle shared library')
        archive = lib / 'libneedle.so'
        archive.write_bytes(payload)
        shutil.copyfile(args.wheel, output / 'upstream-wheel.whl')
        dynamic = subprocess.run(['readelf', '-d', str(archive)], capture_output=True, text=True, check=True).stdout
        import re
        needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
        if set(needed) != set(lock['linux_shared']['needed']):
            raise ValueError('unreviewed Needle shared runtime dependency closure')
        if 'not found' in subprocess.run(['ldd', str(archive)], capture_output=True, text=True, check=True).stdout:
            raise ValueError('unresolved Needle shared runtime dependency')
    else:
        archive = lib / 'libneedle.a'
        shutil.copyfile(args.sdk_dir / 'libneedle.a', archive)
    cflags = '-I' + shlex.quote(str(include))
    libs = shlex.quote(str(archive)) + (' -lc++ -lm -lpthread' if target_platform.startswith('macos') else ' -lc++ -lc++abi -lm -lpthread')
    if shared:
        libs = shlex.quote(str(archive)) + ' -Wl,-rpath,' + shlex.quote(str(lib)) + ' -lm -lpthread -ldl'
    pkg = lib / 'pkgconfig'
    pkg.mkdir()
    (pkg / 'needle.pc').write_text('Name: needle\nDescription: Pinned binary engineering Needle runtime\nVersion: 3\nCflags: ' + cflags + '\nLibs: ' + libs + '\n')
    profile = {'schema': 1, 'profile': 'cast-pro-speech-needle-cpu',
        'engine': 'needle-whistle', 'build_kind': 'pinned-binary-engineering',
        'release_eligible': False, 'cpu_only': True, 'source_available': False,
        'platform': target_platform, 'sdk_revision': lock['sdk_revision'],
        'lock_sha256': sha(LOCK), 'recipe_sha256': sha(__file__),
        'cpu_backend': '', 'cflags': cflags, 'libs': libs,
        'header': {'path': str(include / 'needle.h'), 'sha256': lock['header_sha256']},
        'license': {'path': str(notices / 'Needle-Apache-2.0.txt'), 'sha256': lock['license_sha256']},
        'libraries': [{'path': str(archive), 'sha256': sha(archive), 'chosen_license': 'Apache-2.0', 'distributed': False}],
        'sources': [], 'corresponding_sources': None,
        'production_blockers': ['native runtime source/build provenance unavailable', 'binary SDK is engineering-only and cannot be qualified', 'runtime/helper/model acceptance and platform redistribution review pending']}
    if shared:
        profile['runtime_variant'] = 'linux-shared-wheel'
        profile['wheel'] = {'path': str(output / 'upstream-wheel.whl'), 'sha256': lock['linux_shared']['wheel_sha256'], 'repository': lock['sdk_repository'], 'revision': lock['sdk_revision'], 'upstream_file': lock['linux_shared']['wheel']}
        profile['needed'] = needed
    if args.model:
        profile['model'] = {'path': str(args.model.resolve()), 'sha256': lock['model_sha256'],
                            'revision': lock['model_revision'], 'repository': lock['model_repository']}
    (output / 'profile.json').write_text(json.dumps(profile, indent=2) + '\n')
    (output / 'THIRD-PARTY-NOTICES.txt').write_text('Needle native static SDK: Apache-2.0. Full text: license-texts/Needle-Apache-2.0.txt.\nPinned upstream binary; native runtime source unavailable. ENGINEERING ONLY, NOT RELEASE QUALIFIED.\n')
    print(output / 'profile.json')


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk-dir', type=Path)
parser.add_argument('--model', type=Path)
parser.add_argument('--wheel', type=Path, help='Offline pinned Linux wheel; import its shared runtime instead of static archive')
parser.add_argument('--platform', choices=('linux-x86_64', 'macos-arm64'))
parser.add_argument('--output', type=Path, default=ROOT / 'build/deps-stt-needle')
parser.add_argument('--print-plan', action='store_true')
args = parser.parse_args()
try:
    build(args)
except (ValueError, OSError, KeyError, TypeError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
    print(f'deps-stt: {error}', file=sys.stderr)
    sys.exit(1)
