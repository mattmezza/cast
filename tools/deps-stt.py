#!/usr/bin/env python3
"""Build pinned, bundled-ggml generic CPU speech archives from an OFFLINE Git cache.

This command never clones, fetches, downloads models, or blesses a release.
Successful builds emit source/build evidence and a nonrelease controlled profile.
An independently checked owner review record can subsequently qualify that exact
profile through packaging/speech-profile.py verify --official=1.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'packaging/speech-cpu-lock.json'
ARCHIVES = ('libwhisper.a', 'libggml.a', 'libggml-base.a', 'libggml-cpu.a')


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def execute(command, *, env=None, output=None):
    # A partial-clone cache must fail offline rather than fetch promised objects.
    env = {**(env if env is not None else os.environ), 'GIT_NO_LAZY_FETCH': '1',
           'GIT_OPTIONAL_LOCKS': '0'}
    if output:
        with output.open('wb') as file:
            subprocess.run(command, check=True, stdout=file, env=env)
        return ''
    return subprocess.run(command, check=True, capture_output=True, text=True,
                          env=env).stdout.strip()


def build(args):
    lock = json.loads(LOCK.read_text())
    if args.print_plan:
        print(json.dumps({'commit': lock['commit'], 'cache_only': True,
                          'options': lock['build_options'], 'archives': ARCHIVES,
                          'source_cache_required': str(args.source_git),
                          'release_eligible': False}, indent=2))
        return
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        raise ValueError('initial controlled SDK recipe supports Linux x86_64; other platforms require a reviewed recipe')
    if not args.source_git or not args.source_git.is_dir():
        raise ValueError('missing offline whisper.cpp Git source cache; no network fetch is attempted')
    cache = args.source_git.resolve()
    git = ['git', '-C', str(cache)]
    commit = execute(git + ['rev-parse', '--verify', lock['commit'] + '^{commit}'])
    if commit != lock['commit']:
        raise ValueError('source cache does not contain the exact pinned commit')
    execute(git + ['fsck', '--full', '--no-reflogs'])
    tree = execute(git + ['ls-tree', '-r', commit])
    if any(line.startswith('160000 ') for line in tree.splitlines()):
        raise ValueError('pinned tree unexpectedly uses unmaterialized submodules')
    for tool in ('cmake', 'cc', 'c++', 'ar', 'pkg-config', 'ldd'):
        if not shutil.which(tool):
            raise ValueError(f'missing build-only tool: {tool}')
    target = args.output.resolve()
    if target.exists():
        raise ValueError('output must be new; refusing to mix controlled sources with an existing SDK')
    target.mkdir(parents=True)
    materials = target / 'build-materials'
    materials.mkdir()
    source_archive = materials / f'whisper.cpp-{commit}.tar'
    execute(git + ['archive', '--format=tar', commit], output=source_archive)
    source = target / 'source'
    source.mkdir()
    with tarfile.open(source_archive) as archive:
        archive.extractall(source, filter='data')
    if not (source / 'ggml/src/ggml-cpu').is_dir():
        raise ValueError('pinned source lacks its bundled CPU backend')
    prefix = target / 'prefix'
    build_dir = target / 'compile'
    epoch = execute(git + ['show', '-s', '--format=%ct', commit])
    env = {**os.environ, 'SOURCE_DATE_EPOCH': epoch,
           'GIT_NO_LAZY_FETCH': '1', 'GIT_CEILING_DIRECTORIES': str(target),
           'PKG_CONFIG_PATH': '', 'PKG_CONFIG_LIBDIR': '',
           'CFLAGS': '', 'CXXFLAGS': '', 'LDFLAGS': '', 'CPPFLAGS': ''}
    options = lock['build_options']
    commands = [['cmake', '-S', str(source), '-B', str(build_dir),
                 '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
                 '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib',
                 '-DFETCHCONTENT_FULLY_DISCONNECTED=ON', '-DFETCHCONTENT_UPDATES_DISCONNECTED=ON',
                 '-DWHISPER_BUILD_COMMIT=' + commit,
                 '-DGGML_BUILD_COMMIT=' + commit]
                + [f'-D{name}={value}' for name, value in options.items()],
                ['cmake', '--build', str(build_dir), '--parallel', str(args.jobs)],
                ['cmake', '--install', str(build_dir)]]
    for index, command in enumerate(commands):
        result = subprocess.run(command, env=env, text=True, capture_output=True)
        (materials / f'command-{index}.log').write_text(result.stdout + result.stderr)
        if result.returncode:
            raise ValueError(f'controlled build failed; evidence preserved: {materials}/command-{index}.log')
    cache_text = (build_dir / 'CMakeCache.txt').read_text()
    for name, value in options.items():
        if f'{name}:BOOL={value}' not in cache_text:
            raise ValueError(f'required CPU-only build option was not honored: {name}')
    libdir = prefix / 'lib'
    if list(libdir.rglob('*.so*')):
        raise ValueError('controlled speech SDK unexpectedly produced dynamic backend libraries')
    inventory = []
    for name in ARCHIVES:
        library = libdir / name
        if not library.is_file() or library.read_bytes()[:8] != b'!<arch>\n':
            raise ValueError(f'missing actual static archive: {library}')
        inventory.append({'path': str(library), 'sha256': sha(library),
                          'package': 'whisper.cpp' if name == 'libwhisper.a' else 'ggml',
                          'version': lock['version'] if name == 'libwhisper.a' else lock['ggml']['version'],
                          'chosen_license': 'MIT', 'distributed': True})
    # Upstream static pkg-config metadata may omit ggml CPU dependency ordering.
    # Pin the complete archive group rather than permitting ambient backend libs.
    pc = libdir / 'pkgconfig/whisper.pc'
    pc.parent.mkdir(parents=True, exist_ok=True)
    pc.write_text(f'prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n'
                  f'Name: whisper\nDescription: Cast pinned bundled-ggml CPU SDK\nVersion: {lock["version"]}\n'
                  'Cflags: -I${includedir}\nLibs: -L${libdir} -Wl,--start-group -lwhisper -lggml '
                  '-lggml-cpu -lggml-base -Wl,--end-group -pthread -lm\n')
    pc_env = {**env, 'PKG_CONFIG_LIBDIR': str(pc.parent)}
    cflags = execute(['pkg-config', '--cflags', 'whisper'], env=pc_env)
    libs = execute(['pkg-config', '--libs', '--static', 'whisper'], env=pc_env)
    smoke = materials / 'cpu-smoke.cpp'
    smoke.write_text('#include "whisper.h"\n#include "ggml-backend.h"\n#include <cstdio>\n'
                     'int main(){ auto p=whisper_context_default_params(); p.use_gpu=false;\n'
                     'size_t n=ggml_backend_dev_count(); if(!n)return 1;\n'
                     'for(size_t i=0;i<n;++i)if(ggml_backend_dev_type(ggml_backend_dev_get(i))'
                     '!=GGML_BACKEND_DEVICE_TYPE_CPU)return 2;\n'
                     'std::printf("CPU_ONLY devices=%zu\\n",n); return 0;}\n')
    smoke_bin = materials / 'cpu-smoke'
    compile_smoke = ['c++', '-std=c++17', str(smoke)] + shlex.split(cflags + ' ' + libs) + ['-o', str(smoke_bin)]
    execute(compile_smoke, env=pc_env)
    smoke_result = execute([str(smoke_bin)], env=env)
    closure = execute(['ldd', str(smoke_bin)], env=env)
    if 'not found' in closure or any(x in closure.lower() for x in ('whisper', 'ggml', 'gomp', 'cuda', 'vulkan', 'opencl')):
        raise ValueError('CPU smoke linked an ambient speech/accelerator runtime')
    (materials / 'cpu-smoke-result.txt').write_text(smoke_result + '\n' + closure + '\n')
    commands.append(compile_smoke)
    (materials / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    (materials / 'toolchain.json').write_text(json.dumps({
        'cc': execute(['cc', '--version']).splitlines()[0],
        'cxx': execute(['c++', '--version']).splitlines()[0],
        'cmake': execute(['cmake', '--version']).splitlines()[0],
        'platform': platform.platform(), 'source_date_epoch': epoch}, indent=2) + '\n')
    shutil.copy2(build_dir / 'CMakeCache.txt', materials / 'CMakeCache.txt')
    shutil.copy2(build_dir / 'compile_commands.json', materials / 'compile_commands.json')
    shutil.copy2(LOCK, materials / LOCK.name)
    shutil.copy2(__file__, materials / 'deps-stt.py')
    notices = target / 'license-texts'
    notices.mkdir()
    # This pinned project root LICENSE credits the ggml authors and applies to
    # its bundled tree. Preserve a separate ggml LICENSE if the pin has one.
    for origin, name in ((source / 'LICENSE', 'whisper-MIT.txt'),
                         ((source / 'ggml/LICENSE') if (source / 'ggml/LICENSE').is_file()
                          else source / 'LICENSE', 'ggml-MIT.txt')):
        if not origin.is_file():
            raise ValueError(f'missing upstream license text: {origin}')
        shutil.copy2(origin, notices / name)
    for item in source.rglob('*'):
        if item.is_file() and item.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE')):
            relative = item.relative_to(source)
            destination = notices / 'upstream' / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, destination)
    (target / 'THIRD-PARTY-NOTICES.txt').write_text(
        f'whisper.cpp {lock["version"]}, commit {commit}: MIT.\n'
        f'Bundled ggml {lock["ggml"]["version"]} at the same commit: MIT.\n'
        'Exact source, unmodified upstream license texts, recipe and build commands are provided.\n'
        'Model weights are separate inputs and are not downloaded or licensed by this SDK build.\n')
    kit = target / 'corresponding-sources.tar.gz'
    with tarfile.open(kit, 'w:gz') as archive:
        archive.add(source_archive, arcname=source_archive.name)
        for item in materials.iterdir():
            if item not in (source_archive, smoke_bin):
                archive.add(item, arcname='build-materials/' + item.name)
        archive.add(notices, arcname='license-texts')
        archive.add(target / 'THIRD-PARTY-NOTICES.txt', arcname='THIRD-PARTY-NOTICES.txt')
    profile = {'schema': 1, 'profile': 'cast-pro-speech-cpu', 'cpu_only': True,
               'build_kind': 'controlled-source', 'release_eligible': False,
               'cpu_backend': '', 'backend_loading': 'statically registered generic CPU only; no runtime backend loading',
               'source_commit': commit, 'source_archive_sha256': sha(source_archive),
               'lock_sha256': sha(LOCK), 'recipe_sha256': sha(__file__),
               'whisper_version': lock['version'], 'ggml_version': lock['ggml']['version'],
               'cflags': cflags, 'libs': libs, 'libraries': inventory,
               'build_options': options, 'build_materials': str(materials),
               'cpu_smoke_result': str(materials / 'cpu-smoke-result.txt'),
               'cpu_smoke_sha256': sha(materials / 'cpu-smoke-result.txt'),
               'corresponding_sources': str(kit), 'corresponding_sources_sha256': sha(kit),
               'sources': [{'name': 'whisper.cpp', 'version': lock['version'], 'commit': commit,
                            'license': 'MIT', 'sha256': sha(source_archive)},
                           {'name': 'ggml', 'version': lock['ggml']['version'], 'commit': commit,
                            'license': 'MIT', 'source': 'bundled ggml subtree in the same exact archive',
                            'sha256': sha(source_archive)}],
               'production_blockers': ['exact resulting SDK/helper/model acceptance and source/license inventory owner review pending']}
    (target / 'profile.json').write_text(json.dumps(profile, indent=2) + '\n')
    print(target / 'profile.json')


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-git', type=Path)
parser.add_argument('--output', type=Path, default=ROOT / 'build/deps-stt')
parser.add_argument('--jobs', type=int, default=2)
parser.add_argument('--print-plan', action='store_true')
args = parser.parse_args()
try:
    if not 1 <= args.jobs <= 64:
        raise ValueError('jobs must be 1..64')
    build(args)
except (ValueError, OSError, KeyError, subprocess.CalledProcessError, tarfile.TarError) as error:
    print(f'deps-stt: {error}', file=sys.stderr)
    sys.exit(1)
