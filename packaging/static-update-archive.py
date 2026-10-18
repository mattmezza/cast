#!/usr/bin/env python3
"""Build the pinned updater archive library with zlib as its only external codec."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', required=True, type=Path)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error('jobs must be 1..32')
    prefix = args.prefix.resolve()
    flags = ['./configure', '--prefix=' + str(prefix), '--libdir=' + str(prefix / 'lib'),
             '--enable-static', '--disable-shared', '--with-zlib',
             '--without-bz2lib', '--without-libb2', '--without-iconv', '--without-lz4',
             '--without-zstd', '--without-lzma', '--without-lzo2', '--without-cng',
             '--without-mbedtls', '--without-nettle', '--without-openssl', '--without-md',
             '--without-xml2', '--without-expat', '--disable-acl', '--disable-xattr',
             '--disable-posix-regex-lib', '--disable-bsdtar', '--disable-bsdcpio',
             '--disable-bsdcat', '--disable-bsdunzip']
    for command in (flags, ['make', '-j' + str(args.jobs)], ['make', 'install']):
        subprocess.run(command, cwd=args.source, env=os.environ, check=True)
    environment = dict(os.environ, PKG_CONFIG_PATH=str(prefix / 'lib/pkgconfig') + ':' +
                       os.environ.get('PKG_CONFIG_PATH', ''))
    flags = subprocess.check_output(['pkg-config', '--libs', '--static', 'libarchive'],
                                    env=environment, text=True)
    libraries = {flag[2:] for flag in shlex.split(flags) if flag.startswith('-l')}
    if libraries != {'archive', 'z'}:
        raise ValueError('unexpected updater archive dependency closure: ' + flags)


if __name__ == '__main__':
    main()
