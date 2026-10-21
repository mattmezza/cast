#!/usr/bin/env python3
"""Explicit CPU-only speech SDK evidence. No ordinary build downloads anything."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parent.parent
LICENSES = {'whisper-cpp': 'MIT', 'ggml': 'MIT', 'glibc': 'LGPL-2.1-or-later',
            'libgcc': 'GPL-3.0-or-later WITH GCC-exception-3.1',
            'libstdc++': 'GPL-3.0-or-later WITH GCC-exception-3.1',
            'libgomp': 'GPL-3.0-or-later WITH GCC-exception-3.1',
            'gcc-libs': 'GPL-3.0-or-later WITH GCC-exception-3.1'}

def run(*args):
    result = subprocess.run(args, text=True, capture_output=True)
    if result.returncode:
        raise ValueError(f'{args[0]} failed: {result.stderr.strip()}')
    return result.stdout.strip()

def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()

def closure(roots):
    pending = list(roots)
    paths = set()
    while pending:
        path = Path(pending.pop()).resolve()
        if path in paths:
            continue
        paths.add(path)
        output = run('ldd', str(path))
        if 'not found' in output:
            raise ValueError(f'unresolved speech dependency: {path}')
        for name in re.findall(r'(?:=>\s*)?(/\S+)\s+\(', output):
            resolved = Path(name).resolve()
            if resolved not in paths:
                pending.append(resolved)
    return sorted(paths)

def package(path):
    name = run('pacman', '-Qqo', str(path))
    version = run('pacman', '-Q', name).split(' ', 1)[1]
    return name, version

def candidate(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    backend = args.cpu_backend.resolve()
    if not backend.name.startswith('libggml-cpu-') or backend.suffix != '.so':
        raise ValueError('engineering speech requires one explicit ggml CPU backend')
    libdir = Path(run('pkg-config', '--variable=libdir', 'whisper'))
    roots = [libdir / name for name in ['libwhisper.so', 'libggml.so', 'libggml-base.so']]
    paths = closure(roots + [backend])
    entries = []
    notices = output / 'license-texts'
    notices.mkdir(exist_ok=True)
    for path in paths:
        owner, version = package(path)
        chosen = LICENSES.get(owner)
        if not chosen:
            raise ValueError(f'unreviewed speech runtime dependency: {owner}: {path}')
        entry = {'path': str(path), 'sha256': sha(path), 'package': owner,
                 'version': version, 'chosen_license': chosen, 'distributed': False}
        entries.append(entry)
        texts = Path('/usr/share/licenses') / owner
        if texts.is_dir():
            shutil.copytree(texts, notices / owner, dirs_exist_ok=True)
        if 'GCC-exception' in chosen:
            shutil.copy2(ROOT / 'licenses/GPL-3.0.txt', notices / 'GPL-3.0.txt')
        if owner == 'glibc':
            shutil.copy2(ROOT / 'licenses/LGPL-2.1.txt', notices / 'LGPL-2.1.txt')
    data = {'schema': 1, 'profile': 'cast-pro-speech-cpu', 'release_eligible': False,
            'candidate_identity': 'explicit installed engineering SDK; no controlled upstream source archives',
            'whisper_version': run('pkg-config', '--modversion', 'whisper'),
            'ggml_version': run('pkg-config', '--modversion', 'ggml'),
            'cpu_only': True, 'backend_loading': 'one explicit CPU backend; no load-all or GPU fallback',
            'cpu_backend': str(backend), 'cpu_backend_sha256': sha(backend),
            'cflags': run('pkg-config', '--cflags', 'whisper'),
            'libs': run('pkg-config', '--libs', '--static', 'whisper'),
            'libraries': entries,
            'sources': [{'name': 'whisper.cpp', 'version': run('pkg-config', '--modversion', 'whisper'),
                         'license': 'MIT', 'upstream': 'https://github.com/ggerganov/whisper.cpp', 'sha256': None},
                        {'name': 'ggml', 'version': run('pkg-config', '--modversion', 'ggml'),
                         'license': 'MIT', 'upstream': 'https://github.com/ggerganov/ggml', 'sha256': None}],
            'corresponding_sources': None,
            'production_blockers': ['verified exact upstream source archives/build recipe are absent',
                                    'engineering CPU backend is host-specific and unshipped',
                                    'official profile and model licensing/source provenance require owner review']}
    (output / 'profile.json').write_text(json.dumps(data, indent=2) + '\n')
    (output / 'THIRD-PARTY-NOTICES.txt').write_text('\n'.join(
        f'{x["path"]}: {x["chosen_license"]}; {x["package"]} {x["version"]}; unshipped system dependency' for x in entries)
        + '\nFull preserved texts: license-texts/. This evidence is not a source-built release profile.\n')
    print(output / 'profile.json')

def verify(args):
    path = args.profile.resolve()
    if not path.is_file():
        raise ValueError('STT=1 requires an explicit reviewed speech profile; generate engineering evidence separately')
    data = json.loads(path.read_text())
    if data.get('engine') == 'needle-whistle' or data.get('profile') == 'cast-pro-speech-needle-cpu':
        verify_needle(data, args)
        return
    if data.get('schema') != 1 or type(data.get('cpu_only')) is not bool or (data.get('profile'), data.get('cpu_only')) not in (('cast-pro-speech-cpu', True), ('cast-pro-speech-vulkan', False)):
        raise ValueError('unsupported speech backend profile')
    if data.get('cflags') != args.cflags.strip() or data.get('libs') != args.libs.strip():
        raise ValueError('speech SDK flags differ from audited profile')
    if data.get('cpu_backend', '') != args.cpu_backend:
        raise ValueError('speech CPU backend differs from audited profile')
    entries = data.get('libraries')
    if not isinstance(entries, list) or not entries or len(entries) > 128:
        raise ValueError('invalid speech dependency inventory')
    for item in entries:
        library = Path(item['path'])
        chosen = item.get('chosen_license', '')
        if chosen not in set(LICENSES.values()) or not library.is_file() or sha(library) != item.get('sha256'):
            raise ValueError(f'unreviewed or changed speech dependency: {library}')
        if any(name in library.name for name in ('cuda', 'opencl', 'hip', 'metal', 'blas')) or ('vulkan' in library.name and data.get('cpu_only')):
            raise ValueError('CPU-only speech profile contains an unapproved accelerator')
    if data.get('build_kind') == 'controlled-source':
        verify_controlled(data, official=bool(args.official))
        if not args.official and data.get('release_eligible') is False and not args.engineering:
            raise ValueError('unreviewed controlled SDK requires STT_ENGINEERING=1')
        print('speech profile validated: pinned bundled-ggml CPU source build' +
              ('; reviewed release evidence' if args.official else '; local evidence'))
        return
    if args.official:
        raise ValueError('official STT delivery requires a controlled-source CPU SDK with exact source/build/review evidence')
    if not args.engineering or data.get('release_eligible') is not False:
        raise ValueError('candidate speech SDK requires STT_ENGINEERING=1 and release_eligible=false')
    print('speech profile validated: CPU-only, explicit engineering candidate, nonrelease')


def verify_needle(data, args):
    """A binary hash proves identity, not source provenance or release eligibility."""
    lock_path = ROOT / 'packaging/speech-needle-lock.json'
    lock = json.loads(lock_path.read_text())
    if args.official:
        raise ValueError('binary-only Needle SDK cannot be used for official release delivery')
    if not args.engineering or data.get('release_eligible') is not False:
        raise ValueError('Needle binary SDK requires STT_ENGINEERING=1 and release_eligible=false')
    if (data.get('schema') != 1 or data.get('profile') != 'cast-pro-speech-needle-cpu' or
            data.get('engine') != 'needle-whistle' or data.get('cpu_only') is not True or
            data.get('build_kind') != 'pinned-binary-engineering' or
            data.get('source_available') is not False or data.get('sources') != [] or
            data.get('corresponding_sources') is not None or
            data.get('sdk_revision') != lock['sdk_revision'] or
            data.get('lock_sha256') != sha(lock_path) or
            data.get('recipe_sha256') != sha(ROOT / 'tools/deps-stt.py')):
        raise ValueError('Needle engineering provenance differs from the pinned binary recipe')
    target = lock['platforms'].get(data.get('platform'))
    if not target:
        raise ValueError('unsupported Needle SDK platform')
    if (data.get('cflags') != args.cflags.strip() or data.get('libs') != args.libs.strip() or
            data.get('cpu_backend') != '' or args.cpu_backend != ''):
        raise ValueError('Needle SDK flags/backend differ from audited profile')
    entries = data.get('libraries')
    if not isinstance(entries, list) or len(entries) != 1:
        raise ValueError('Needle SDK requires exactly one pinned static archive')
    item = entries[0]
    library = Path(item['path'])
    shared = data.get('runtime_variant') == 'linux-shared-wheel'
    expected_digest = lock['linux_shared']['sha256'] if shared else target['sha256']
    if (library.name != ('libneedle.so' if shared else 'libneedle.a') or item.get('sha256') != expected_digest or
            item.get('chosen_license') != 'Apache-2.0' or library.is_symlink() or
            not library.is_file() or sha(library) != expected_digest):
        raise ValueError('missing or changed pinned Needle archive')
    expected_cflags = '-I' + shlex.quote(str(Path(data['header']['path']).parent))
    expected_libs = shlex.quote(str(library)) + (' -lc++ -lm -lpthread' if data['platform'].startswith('macos') else ' -lc++ -lc++abi -lm -lpthread')
    if shared:
        wheel = data.get('wheel', {}); wheel_path = Path(wheel.get('path', ''))
        pin = lock['linux_shared']
        if (data['platform'] != 'linux-x86_64' or wheel.get('sha256') != pin['wheel_sha256'] or
                wheel.get('revision') != lock['sdk_revision'] or wheel.get('repository') != lock['sdk_repository'] or
                wheel.get('upstream_file') != pin['wheel'] or wheel_path.is_symlink() or
                not wheel_path.is_file() or sha(wheel_path) != pin['wheel_sha256']):
            raise ValueError('missing or changed upstream Needle wheel provenance')
        needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', run('readelf', '-d', str(library)))
        if set(needed) != set(pin['needed']) or set(data.get('needed', [])) != set(needed):
            raise ValueError('Needle shared dependency closure differs from pinned inventory')
        if 'not found' in run('ldd', str(library)):
            raise ValueError('Needle shared runtime closure unresolved')
        expected_libs = shlex.quote(str(library)) + ' -Wl,-rpath,' + shlex.quote(str(library.parent)) + ' -lm -lpthread -ldl'
    elif data.get('runtime_variant') not in (None, 'static'):
        raise ValueError('unrecognized Needle runtime variant')
    if data['cflags'] != expected_cflags or data['libs'] != expected_libs:
        raise ValueError('Needle SDK flags do not match its exact pinned artifact paths')
    for field, digest in [('header', lock['header_sha256']), ('license', lock['license_sha256'])]:
        artifact = data.get(field, {})
        file = Path(artifact.get('path', ''))
        if (artifact.get('sha256') != digest or file.is_symlink() or
                not file.is_file() or sha(file) != digest):
            raise ValueError('missing or changed Needle ' + field)
    if 'model' in data:
        model = data['model']; file = Path(model['path'])
        if (model.get('sha256') != lock['model_sha256'] or
                model.get('revision') != lock['model_revision'] or
                model.get('repository') != lock['model_repository'] or
                file.is_symlink() or not file.is_file() or sha(file) != lock['model_sha256']):
            raise ValueError('missing or changed pinned Whistle model')
    print('speech profile validated: pinned Needle/Whistle binary engineering SDK; not release qualified')


def sdk_identity(data):
    fields = ('source_commit', 'source_archive_sha256', 'lock_sha256', 'recipe_sha256',
              'whisper_version', 'ggml_version', 'cflags', 'libs', 'libraries',
              'build_options', 'cpu_variant_options', 'cpu_smoke_sha256', 'corresponding_sources_sha256')
    return hashlib.sha256(json.dumps({key: data.get(key) for key in fields},
                                    sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def verify_controlled(data, *, official=False):
    lock_path = ROOT / 'packaging/speech-cpu-lock.json'
    recipe_path = ROOT / 'tools/deps-stt-whisper.py'
    lock = json.loads(lock_path.read_text())
    options = dict(lock['build_options'])
    vulkan = data.get('profile') == 'cast-pro-speech-vulkan'
    if vulkan:
        options['GGML_VULKAN'] = 'ON'
    # Preserve the previously accepted CPU engineering SDK for local dev only.
    # Official profiles always require the current recipe, and the source kit
    # below must still contain the exact recipe whose hash is recorded.
    recipe_matches = data.get('recipe_sha256') == sha(recipe_path) or (
        not official and not vulkan and data.get('recipe_sha256') in {
        '33cd1f1d9acae7a18c3c4850c0e91acb2313a212cbd3c947103a78d6a807962e',
        '4de1237e77301f0fc25915d09ae434747c4747b71973f83235bb72985483e853'})
    if (data.get('source_commit') != lock['commit'] or
            data.get('whisper_version') != lock['version'] or
            data.get('ggml_version') != lock['ggml']['version'] or
            data.get('lock_sha256') != sha(lock_path) or
            not recipe_matches or
            data.get('build_options') != options or
            data.get('cpu_backend') != ''):
        raise ValueError('controlled speech profile differs from its pinned portable backend recipe')
    darwin = data.get('platform') == 'darwin'
    if darwin and official:
        raise ValueError('macOS controlled speech SDK is currently development evidence only')
    entries = data['libraries']
    import re
    expected = {'libwhisper.so', 'libggml.so', 'libggml-base.so',
                'libggml-cpu-generic.so', 'libggml-cpu-avx2.so'}
    if darwin:
        expected = {'libwhisper.dylib', 'libggml.dylib', 'libggml-base.dylib', 'libggml-cpu-generic.dylib'}
    if vulkan:
        expected.add('libggml-vulkan.so')
    names = {(re.sub(r'\.[0-9.]+(?=\.dylib$)', '', Path(item['path']).name) if darwin else re.sub(r'(?<=\.so)\.[0-9.]+$', '', Path(item['path']).name)) for item in entries}
    if (names != expected or data.get('cpu_variant_options') != ({} if darwin else lock['cpu_variant_options']) or
            any(item.get('chosen_license') != 'MIT' or
                sha(Path(item['path'])) != item.get('sha256') or
                (Path(item['path']).read_bytes()[:4] not in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe') if darwin else Path(item['path']).read_bytes()[:4] != b'\x7fELF') for item in entries)):
        raise ValueError('controlled speech library inventory is incomplete or has ambient libraries')
    kit = Path(data.get('corresponding_sources', ''))
    smoke = Path(data.get('cpu_smoke_result', ''))
    if not kit.is_file() or sha(kit) != data.get('corresponding_sources_sha256'):
        raise ValueError('controlled speech corresponding source kit is missing or changed')
    if (not smoke.is_file() or sha(smoke) != data.get('cpu_smoke_sha256') or
            not smoke.read_text().startswith('CPU_ONLY devices=')):
        raise ValueError('controlled speech CPU-only link smoke evidence is missing or changed')
    with tarfile.open(kit) as archive:
        member = archive.getmember(f'whisper.cpp-{lock["commit"]}.tar')
        file = archive.extractfile(member)
        if not file or hashlib.sha256(file.read()).hexdigest() != data.get('source_archive_sha256'):
            raise ValueError('source kit does not preserve the exact materialized pinned tree')
        for name, expected_hash in [('deps-stt.py', data['recipe_sha256']),
                                    ('speech-cpu-lock.json', data['lock_sha256'])]:
            file = archive.extractfile('build-materials/' + name)
            if not file or hashlib.sha256(file.read()).hexdigest() != expected_hash:
                raise ValueError('source kit does not preserve the matching build recipe/lock')
    if official:
        if data.get('release_eligible') is not True:
            raise ValueError('controlled SDK source build still needs exact owner acceptance review')
        record = Path(data.get('review_record', ''))
        if not record.is_file() or sha(record) != data.get('review_record_sha256'):
            raise ValueError('controlled SDK review record is missing or changed')
        review = json.loads(record.read_text())
        if (review.get('schema') != 1 or review.get('sdk_sha256') != sdk_identity(data) or
                not isinstance(review.get('reviewed_by'), str) or not review['reviewed_by'].strip()):
            raise ValueError('review record is not bound to this exact SDK')
        required = {'cpu_model_acceptance', 'signed_helper_boundary', 'full_application',
                    'source_license_inventory', 'runtime_closure'}
        if vulkan:
            required.add('gpu_model_acceptance')
        tests = review.get('acceptance', {})
        if not isinstance(tests, dict) or set(tests) != required:
            raise ValueError('review record lacks required inference/helper/application/source/runtime acceptance')
        for name, item in tests.items():
            artifact = Path(item.get('path', ''))
            if (item.get('passed') is not True or not artifact.is_file() or
                    sha(artifact) != item.get('sha256')):
                raise ValueError(f'missing, failed or changed owner review acceptance artifact: {name}')


def qualify(args):
    data = json.loads(args.profile.read_text())
    if data.get('build_kind') != 'controlled-source':
        raise ValueError('an installed engineering SDK cannot be qualified')
    data['release_eligible'] = True
    data['review_record'] = str(args.review_record.resolve())
    data['review_record_sha256'] = sha(args.review_record)
    verify_controlled(data, official=True)
    data['production_blockers'] = []
    args.profile.write_text(json.dumps(data, indent=2) + '\n')
    print('Exact controlled SDK qualified by supplied owner acceptance record')

parser = argparse.ArgumentParser()
sub = parser.add_subparsers(dest='command', required=True)
create = sub.add_parser('candidate')
create.add_argument('--cpu-backend', type=Path, required=True)
create.add_argument('--output', type=Path, required=True)
check = sub.add_parser('verify')
check.add_argument('--profile', type=Path, required=True)
check.add_argument('--official', type=int, choices=(0, 1), default=0)
check.add_argument('--engineering', type=int, choices=(0, 1), default=0)
check.add_argument('--cpu-backend', default='')
check.add_argument('--cflags', default='')
check.add_argument('--libs', required=True)
review = sub.add_parser('qualify')
review.add_argument('--profile', type=Path, required=True)
review.add_argument('--review-record', type=Path, required=True)
args = parser.parse_args()
try:
    {'candidate': candidate, 'verify': verify, 'qualify': qualify}[args.command](args)
except (ValueError, OSError, KeyError, TypeError, json.JSONDecodeError, tarfile.TarError) as error:
    print(f'speech-profile: {error}', file=sys.stderr)
    sys.exit(1)
