#!/usr/bin/env python3
"""Explicit glibc static Community build; requires the workflow's Ubuntu SDK inputs."""
import argparse
import hashlib
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
    args = parser.parse_args()
    if os.uname().sysname != 'Linux' or os.uname().machine != 'x86_64':
        parser.error('requires native Linux x86_64 with glibc')
    if not 1 <= args.jobs <= 32:
        parser.error('jobs must be 1..32')
    base = args.root.resolve()
    prefix, cache = base / 'prefix', base / 'upstream'
    cache.mkdir(parents=True, exist_ok=True)
    prefix.mkdir(exist_ok=True)
    lock = json.loads((ROOT / 'packaging/static-linux-lock.json').read_text())
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
    # PipeWire upstream explicitly uses shared_library(), ignoring default_library.
    # Build a static counterpart from exactly the same compiled client objects.
    pipewire = sources['pipewire']
    pwbuild = base / 'pipewire-build'
    if pwbuild.exists():
        shutil.rmtree(pwbuild)
    run('meson', 'setup', pwbuild, pipewire, '--prefix=/usr', '--libdir=lib/x86_64-linux-gnu',
        '--auto-features=disabled', '-Dexamples=disabled', '-Dtests=disabled',
        '-Dspa-plugins=enabled', '-Dsupport=enabled', '-Daudioconvert=enabled',
        '-Daudiomixer=enabled', '-Dpipewire-jack=disabled', '-Dpipewire-v4l2=disabled', env=environment)
    run('ninja', '-C', pwbuild, jobs, env=environment)
    objects = sorted(pwbuild.glob('src/pipewire/libpipewire-0.3.so*.p/*.o'))
    if not objects:
        raise ValueError('PipeWire client objects missing')
    (prefix / 'lib').mkdir(exist_ok=True)
    run('ar', 'rcs', prefix / 'lib/libpipewire-0.3.a', *objects)
    # Install the same build's public/generated headers and pkg-config files.
    # Distro headers can describe an older ABI and must never drive this build.
    pwstage = base / 'pipewire-install'
    run('meson', 'install', '-C', pwbuild, '--destdir', pwstage, env=environment)
    shutil.copytree(pwstage / 'usr', prefix, dirs_exist_ok=True)
    for metadata in prefix.rglob('*.pc'):
        text = metadata.read_text().replace('prefix=/usr', 'prefix=' + str(prefix))
        metadata.write_text(text)
    run('make', jobs, 'BUILDTYPE=Release', 'libraries', cwd=sources['openh264'], env=environment)
    run('make', 'PREFIX=' + str(prefix), 'install-static', cwd=sources['openh264'], env=environment)
    media = json.loads((ROOT / 'packaging/media-lgpl-lock.json').read_text())
    flags = [x for x in media['ffmpeg_flags'] if x not in ('--enable-shared', '--disable-static')]
    run('./configure', '--prefix=' + str(prefix), '--enable-static', '--disable-shared',
        '--pkg-config-flags=--static', *flags, cwd=sources['ffmpeg'], env=environment)
    run('make', jobs, cwd=sources['ffmpeg'], env=environment)
    run('make', 'install', cwd=sources['ffmpeg'], env=environment)
    for name, options in [
        ('SDL', ['-DSDL_SHARED=OFF', '-DSDL_STATIC=ON', '-DSDL_TESTS=OFF',
                 '-DSDL_TEST_LIBRARY=OFF', '-DSDL_X11=ON', '-DSDL_WAYLAND=ON']),
        ('SDL_ttf', ['-DBUILD_SHARED_LIBS=OFF', '-DSDLTTF_SAMPLES=OFF', '-DSDLTTF_HARFBUZZ=OFF',
                     '-DSDLTTF_PLUTOSVG=OFF'])]:
        build = base / (name + '-build')
        run('cmake', '-S', sources[name], '-B', build, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(prefix),
            '-DCMAKE_INSTALL_LIBDIR=lib', *options, env=environment)
        run('cmake', '--build', build, '--parallel', args.jobs, env=environment)
        run('cmake', '--install', build, env=environment)
    packages = ['fontconfig', 'freetype2', 'libavcodec', 'libavformat', 'libavutil',
                'libswscale', 'libswresample', 'libpipewire-0.3', 'x11', 'xext', 'xrandr',
                'xi', 'xfixes', 'xcomposite', 'gio-2.0', 'gio-unix-2.0', 'sdl3', 'sdl3-ttf']
    libs = subprocess.check_output(['pkg-config', '--libs', '--static', *packages],
                                   env=environment, text=True).strip()
    # A linker group resolves cycles between all static media/UI dependencies.
    libs = '-Wl,--start-group -L' + str(prefix / 'lib') + ' ' + libs + ' -lstdc++ -Wl,--end-group'
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    variables = ['EDITION=community', 'X11=1', 'WAYLAND=1', 'PANEL=1', 'STT=0',
                 'SOURCE_COMMIT=' + commit, 'OFFICIAL_RELEASE=1',
                 'PKG_LIBS=' + libs, 'PANEL_LIBS=', 'LDFLAGS=-static -no-pie -Wl,-Map,' + str(base / 'cast.map')]
    run('make', jobs, *variables, 'all', 'check', 'check-xorg', 'check-panel', env=environment)
    # Each test executable also consumes LDFLAGS. Relink the release executable
    # last, with its own map, so source collection audits the actual payload.
    program = ROOT / 'cast'
    program.unlink()
    run('make', jobs, *variables, 'all', env=environment)
    headers = subprocess.check_output(['readelf', '-l', program], text=True)
    dynamic = subprocess.check_output(['readelf', '-d', program], text=True)
    if 'INTERP' in headers or '(NEEDED)' in dynamic:
        raise ValueError('result is not fully statically linked')
    run(program, '--version')
    run(program, '--help')
    shutil.copy2(program, base / 'cast')
    (base / 'build-info.json').write_text(json.dumps({
        'source_commit': commit, 'make_variables': variables, 'sources': lock['sources'],
        'linkage': 'static glibc; no PT_INTERP or DT_NEEDED',
        'runtime_requirements': ['compatible glibc for dynamically loaded desktop modules',
                                 'PipeWire client SPA/modules/configuration and server',
                                 'X11/Wayland display and portal', 'fonts', 'TLS trust store'],
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
