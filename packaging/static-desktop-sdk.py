#!/usr/bin/env python3
"""Build missing desktop archives from exact installed Ubuntu source packages."""
from pathlib import Path
import shutil
import subprocess


def build(base, prefix, jobs, environment):
    cache = base / 'upstream/ubuntu-desktop'
    cache.mkdir(parents=True, exist_ok=True)
    for package, name, options in [
        ('libwayland-client0', 'wayland', ['-Ddocumentation=false', '-Dtests=false', '-Dscanner=false']),
        ('libxkbcommon0', 'xkbcommon', ['-Denable-wayland=false', '-Denable-x11=false',
            '-Denable-xkbregistry=false', '-Denable-tools=false', '-Denable-docs=false'])]:
        source, version = subprocess.check_output(['dpkg-query', '-W',
            '-f=${source:Package}\t${source:Version}', package], text=True).split('\t')
        subprocess.run(['apt-get', 'source', '--download-only', source + '=' + version], cwd=cache, check=True)
        descriptors = list(cache.glob(source + '_*.dsc'))
        descriptor = next(path for path in descriptors if 'Version: ' + version + '\n' in path.read_text())
        destination = base / 'sources' / ('ubuntu-' + name)
        if destination.exists():
            shutil.rmtree(destination)
        subprocess.run(['dpkg-source', '-x', descriptor, destination], check=True)
        target = base / (name + '-static-build')
        if target.exists():
            shutil.rmtree(target)
        subprocess.run(['meson', 'setup', target, destination, '--prefix=' + str(prefix),
            '--libdir=lib', '--default-library=static', '--buildtype=release', '--wrap-mode=nodownload',
            *options], env=environment, check=True)
        subprocess.run(['ninja', '-C', target, '-j' + str(jobs)], env=environment, check=True)
        subprocess.run(['meson', 'install', '-C', target], env=environment, check=True)
