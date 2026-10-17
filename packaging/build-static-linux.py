#!/usr/bin/env python3
"""Explicit glibc static Linux build; requires the workflow's Ubuntu SDK inputs."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent


def run(*args, cwd=ROOT, env=None):
    subprocess.run([str(x) for x in args], cwd=cwd, env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT / 'build/static-linux')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--reuse-sdk', action='store_true', help='reuse only a matching locked SDK/toolchain')
    parser.add_argument('--sdk-only', action='store_true', help='prepare dependencies without application/tests')
    parser.add_argument('--edition', choices=('community', 'pro'), default='community')
    parser.add_argument('--pro-root', type=Path)
    parser.add_argument('--stt-profile', type=Path)
    parser.add_argument('--pro-trust-header', type=Path)
    parser.add_argument('--release-timestamp', type=int, help='positive exact public core commit UTC timestamp')
    args = parser.parse_args()
    if args.edition == 'pro' and (not args.pro_root or not args.stt_profile):
        parser.error('Pro requires --pro-root and --stt-profile')
    if os.uname().sysname != 'Linux' or os.uname().machine != 'x86_64':
        parser.error('requires native Linux x86_64 with glibc')
    if not 1 <= args.jobs <= 32:
        parser.error('jobs must be 1..32')
    base = args.root.resolve()
    prefix, cache = base / 'prefix', base / 'upstream'
    cache.mkdir(parents=True, exist_ok=True)
    prefix.mkdir(exist_ok=True)
    spec = importlib.util.spec_from_file_location('static_pipewire', ROOT / 'packaging/static-pipewire.py')
    static_pipewire = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(static_pipewire)
    desktop_spec = importlib.util.spec_from_file_location('static_desktop', ROOT / 'packaging/static-desktop-sdk.py')
    static_desktop = importlib.util.module_from_spec(desktop_spec)
    desktop_spec.loader.exec_module(static_desktop)
    sdk_spec = importlib.util.spec_from_file_location('static_linux_sdk', ROOT / 'packaging/static-linux-sdk.py')
    sdk_recipe = importlib.util.module_from_spec(sdk_spec)
    sdk_spec.loader.exec_module(sdk_recipe)
    lock_path = ROOT / 'packaging/static-linux-lock.json'
    lock = json.loads(lock_path.read_text())
    toolchain = subprocess.check_output(['dpkg-query', '-W', '-f=${binary:Package}=${Version}\n'])
    identity = hashlib.sha256(lock_path.read_bytes() +
                              (ROOT / 'packaging/media-lgpl-lock.json').read_bytes() +
                              (ROOT / 'packaging/static-linux-sdk.py').read_bytes() + (ROOT / 'packaging/static-pipewire.py').read_bytes() +
                              (ROOT / 'packaging/static-desktop-sdk.py').read_bytes() + toolchain).hexdigest()
    marker = base / 'sdk.identity'
    reuse = args.reuse_sdk and marker.is_file() and marker.read_text().strip() == identity

    sources = {}
    for source in lock['sources']:
        archive = cache / source['archive']
        if not archive.exists():
            partial = archive.with_suffix('.partial')
            with urllib.request.urlopen(source['url'], timeout=120) as response, partial.open('wb') as out:
                shutil.copyfileobj(response, out)
            if hashlib.sha256(partial.read_bytes()).hexdigest() != source['sha256']:
                raise ValueError('source checksum mismatch: ' + source['name'])
            partial.replace(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != source['sha256']:
            raise ValueError('cached source checksum mismatch: ' + source['name'])
        destination = base / 'sources' / source['name']
        if destination.exists():
            shutil.rmtree(destination)
        destination.mkdir(parents=True)
        with tarfile.open(archive) as tar:
            tar.extractall(destination, filter='data')
        directories = list(destination.iterdir())
        if len(directories) != 1 or not directories[0].is_dir():
            raise ValueError('unexpected archive root: ' + source['name'])
        sources[source['name']] = directories[0]
    environment = dict(os.environ, PKG_CONFIG_PATH=os.pathsep.join([str(prefix / 'lib/pkgconfig'),
                       str(prefix / 'lib/x86_64-linux-gnu/pkgconfig')]),
                       CMAKE_PREFIX_PATH=str(prefix))
    jobs = '-j' + str(args.jobs)
    if not reuse:
        sdk_recipe.build(args, base, prefix, sources, environment, jobs, ROOT, run,
                         static_desktop, static_pipewire)
        marker.write_text(identity + '\n')
    else:
        print('Reusing verified-source SDK with matching installed toolchain', flush=True)
    run('python3', ROOT / 'tools/check-static-deps.py', '--edition', 'community', env=environment)
    if args.sdk_only:
        return
    if args.edition == 'pro':
        media = json.loads((ROOT / 'packaging/media-lgpl-lock.json').read_text())
        libraries = {str(p.relative_to(base)): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in (prefix / 'lib').glob('*.a')}
        (base / 'profile.json').write_text(json.dumps({'schema': 1, 'profile': 'lgpl',
            'linkage': 'static', 'release_eligible': False, 'libraries': libraries,
            'configure_flags': [x for x in media['ffmpeg_flags'] if x not in ('--enable-shared', '--disable-static')] + ['--enable-static', '--disable-shared']}))
    packages = ['fontconfig', 'freetype2', 'libavcodec', 'libavformat', 'libavutil',
                'libswscale', 'libswresample', 'libpipewire-0.3', 'x11', 'xext', 'xrandr',
                'xi', 'xfixes', 'xcomposite', 'gio-2.0', 'gio-unix-2.0', 'sdl3', 'sdl3-ttf', 'xcursor']
    if args.edition == 'pro':
        packages.append('libsodium')
    libs = subprocess.check_output(['pkg-config', '--libs', '--static', *packages],
                                   env=environment, text=True).strip()
    # A linker group resolves cycles between all static media/UI dependencies.
    libs = '-Wl,--start-group -L' + str(prefix / 'lib') + ' ' + libs + ' -lstdc++ -Wl,--end-group'
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    timestamp = int(subprocess.check_output(['git', 'show', '-s', '--format=%ct', commit], cwd=ROOT, text=True))
    if args.release_timestamp is not None and (args.release_timestamp <= 0 or args.release_timestamp != timestamp):
        parser.error('release timestamp must equal the positive exact public core commit UTC timestamp')
    loader = base / 'static-loader.o'
    run('cc', '-O2', '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra',
        '-Werror', '-c', ROOT / 'packaging/static-loader.c', '-o', loader)
    loader_flags = str(loader) + ' -Wl,--wrap=dlopen,--wrap=dlsym,--wrap=dlclose,--wrap=dlerror ' + libs
    variables = ['EDITION=' + args.edition, 'X11=1', 'WAYLAND=1', 'PANEL=1', 'STT=0',
                 'STATIC_PANEL_X11=1', 'SOURCE_COMMIT=' + commit, 'OFFICIAL_RELEASE=1', 'TEST_LINK_DNS=1',
                 'PKG_LIBS=' + libs, 'PANEL_LIBS=', 'LDFLAGS=-static -no-pie ' + loader_flags + ' -Wl,-Map,' + str(base / 'cast.map')]
    if args.edition == 'pro':
        profile = json.loads(args.stt_profile.read_text())
        if args.pro_trust_header:
            variables.append('PRO_TRUST_HEADER=' + str(args.pro_trust_header.resolve()))
        variables += ['MEDIA_PROFILE=lgpl', 'PRO_ROOT=' + str(args.pro_root.resolve()),
            'LGPL_ROOT=' + str(base), 'OFFICIAL_RELEASE=0', 'STT=1', 'STT_ENGINEERING=1',
            'RELEASE_TIMESTAMP=' + str(timestamp), 'PRODUCTION_LICENSE_ACCEPTANCE=1', 'STT_HELPER_SIBLING=1', 'STT_PROFILE=' + str(args.stt_profile.resolve()), 'STT_CFLAGS=' + profile['cflags'],
            'STT_LIBS=' + profile['libs'], 'STT_CPU_BACKEND=',
            'STT_LDFLAGS=-Wl,-Map,' + str(base / 'helper.map')]
    run('make', jobs, *variables, 'all', env=environment)
    helper = None
    test_variables = variables.copy()
    if args.edition == 'pro':
        helper = max((ROOT / 'build').glob('pro-*/cast-pro-stt-helper'), key=lambda p: p.stat().st_mtime)
        private = args.pro_root.resolve()
        if (private / 'pro/src/trusted_keys.h').is_file():
            private /= 'pro'
        test_variables.append('PRODUCTION_LICENSE_ACCEPTANCE=0')
        test_variables.append('RELEASE_TIMESTAMP=0')
        test_header = private / 'tests/generated/trusted_keys.h'
        if not test_header.is_file():
            raise ValueError('Pro static tests require explicit private verifier fixtures')
        test_variables.append('PRO_TRUST_HEADER=' + str(test_header))
    environment['CAST_TEST_LINK_DNS'] = '1'
    for suite in ('check', 'check-xorg', 'check-panel'):
        try:
            run('make', jobs, *test_variables, suite, env=environment)
        except subprocess.CalledProcessError:
            if suite == 'check-xorg' and os.environ.get('GITHUB_ACTIONS') == 'true':
                # Preserve a real remote crash stack without adding debugger
                # packages to the dependency SDK/toolchain cache identity.
                run('sudo', 'apt-get', 'install', '-y', 'gdb')
                smoke = max((ROOT / 'build').glob('*/test_xorg'), key=lambda p: p.stat().st_mtime)
                run('xvfb-run', '-a', '-s', '-screen 0 800x600x24',
                    'gdb', '--batch', '-ex', 'run', '-ex', 'thread apply all bt',
                    '-ex', 'info sharedlibrary', '-ex', 'frame 8', '-ex', 'x/20i $pc-48',
                    '--args', smoke, '--exercise', env=environment)
            raise
    # Each test executable also consumes LDFLAGS. Relink the release executable
    # last, with its own map, so source collection audits the actual payload.
    program = ROOT / ('cast-pro' if args.edition == 'pro' else 'cast')
    program.unlink()
    run('make', jobs, *variables, 'all', env=environment)
    headers = subprocess.check_output(['readelf', '-l', program], text=True)
    dynamic = subprocess.check_output(['readelf', '-d', program], text=True)
    if 'INTERP' in headers or '(NEEDED)' in dynamic:
        raise ValueError('result is not fully statically linked')
    run(program, '--version')
    run(program, '--help')
    shutil.copy2(program, base / program.name)
    if args.edition == 'pro':
        for option in ('-l', '-d'):
            result = subprocess.check_output(['readelf', option, helper], text=True)
            if 'INTERP' in result or '(NEEDED)' in result:
                raise ValueError('speech helper is not static')
        shutil.copy2(helper, base / helper.name)

    (base / 'build-info.json').write_text(json.dumps({
        'helper_path': str(helper.relative_to(ROOT)) if helper else None,
        'source_commit': commit, 'make_variables': variables, 'sources': lock['sources'],
        'linkage': 'static glibc; no PT_INTERP or DT_NEEDED',
        'runtime_requirements': ['PipeWire configuration and server (client modules and SPA factories are built in)',
                                 'X11 or XWayland for the software panel',
                                 'Wayland display and portal for Wayland capture',
                                 'fonts', 'TLS trust store'],
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
