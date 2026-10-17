#!/usr/bin/env python3
"""Build the pinned Linux model transport without optional curl dependencies."""
import argparse
import os
from pathlib import Path
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
             '--enable-static', '--disable-shared', '--with-openssl',
             '--without-libpsl', '--without-libidn2', '--without-libssh2', '--without-libssh',
             '--without-nghttp2', '--without-nghttp3', '--without-ngtcp2',
             '--without-brotli', '--without-zstd', '--without-zlib',
             '--without-gssapi', '--without-libgsasl', '--disable-ldap', '--disable-ldaps',
             '--disable-docs', '--disable-manual', '--disable-sspi',
             '--with-ca-bundle=/etc/ssl/certs/ca-certificates.crt',
             '--with-ca-path=/etc/ssl/certs']
    # OpenSSL's static archives already underpin the media TLS profile. Exact
    # distro source versions are retained by static-linux-sources.py's map audit.
    for command in (flags, ['make', '-j' + str(args.jobs)], ['make', 'install']):
        subprocess.run(command, cwd=args.source, env=os.environ, check=True)


if __name__ == '__main__':
    main()
