#!/usr/bin/env python3
"""Reject private paths and secrets before public archival; explicit public roots."""
import argparse
import gzip
import json
import pathlib
import re
import sys
import subprocess
import tarfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
PUBLIC = ('Makefile', '.clang-format', '.gitignore', 'README.md', 'PRODUCT.md',
          'DESIGN.md', 'LICENSE', 'CONTRIBUTING.md', 'BRANDING.md', 'licenses', 'src',
          'vendor', 'assets', 'completions', 'tests', 'docs', 'examples', 'packaging',
          'tools', '.github')
FORBIDDEN = re.compile(r'(^|/)(cast-pro|cast-pro-private|private|issuer|secrets?)(/|$)|\.(key|pem|p12|pfx|o|d|a)$|\.so(?:\.[^/]+)?$|(^|/)(trusted_keys\.h|pro_provider\.c|bounded_json\.c|issuer\.py)$', re.I)
SECRET = re.compile(rb'-----BEGIN (?:[A-Z ]*PRIVATE KEY)-----|"(?:secret_key|private_key)"\s*:')


def safe_name(name):
    return not FORBIDDEN.search(name) and '..' not in pathlib.PurePosixPath(name).parts


def public_files(root=ROOT):
    for name in PUBLIC:
        path = root / name
        if path.is_file():
            yield path
        elif path.is_dir():
            yield from sorted(p for p in path.rglob('*') if (p.is_file() or p.is_symlink()) and '__pycache__' not in p.parts and p.suffix not in ('.pyc', '.pyo') and not p.name.endswith('-prompt.md') and p.name != 'prompt.md')


def check(root=ROOT):
    # The whole public checkout is checked for in-tree private paths, including ignored paths.
    for p in root.rglob('*'):
        relative = p.relative_to(root)
        if relative.parts[0] in ('.git', 'build', 'dist', '.agents', '.codex') or '__pycache__' in relative.parts:
            continue
        if str(relative) in ('cast', 'cast-pro') and p.is_file() and not p.is_symlink():
            tracked = subprocess.run(['git', '-C', str(root), 'ls-files', '--error-unmatch', str(relative)],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if tracked.returncode == 0:
                raise ValueError(f'compiled application must not be tracked/publicly archived: {relative}')
            continue
        if not safe_name(str(relative)):
            raise ValueError(f'private/secret path in public worktree: {relative}')
        if p.is_file() and not p.is_symlink():
            data = p.read_bytes()
            if data.startswith(b'\x7fELF'):
                raise ValueError(f'compiled/debug artifact in public source roots: {relative}')
            if SECRET.search(data):
                raise ValueError(f'key material in public worktree: {relative}')
            if p.suffix == '.json':
                try:
                    value = json.loads(data)
                except (ValueError, UnicodeError):
                    value = None
                if isinstance(value, dict) and (('payload' in value and 'signature' in value) or ('license_id' in value and value.get('product') == 'cast-pro')):
                    raise ValueError(f'customer token/claims in public worktree: {relative}')
    for p in public_files(root):
        relative = p.relative_to(root)
        if p.is_symlink():
            raise ValueError(f'public source symlink refused: {relative}')
        if not safe_name(str(relative)) or SECRET.search(p.read_bytes()):
            raise ValueError(f'private artifact/key material refused: {relative}')


def archive(output, prefix):
    check()
    def stable_metadata(member):
        member.uid = member.gid = member.mtime = 0
        member.uname = member.gname = ''
        member.pax_headers = {}
        return member
    # Both edition packages reference one public archive. Repeated creation from
    # unchanged sources must preserve its checksum rather than encode wall time.
    with open(output, 'wb') as raw:
        with gzip.GzipFile(fileobj=raw, filename='', mode='wb', mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w') as tar:
                for p in public_files():
                    tar.add(p, arcname=f'{prefix}/{p.relative_to(ROOT)}', recursive=False,
                            filter=stable_metadata)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--archive')
    parser.add_argument('--prefix', default='cast-source')
    args = parser.parse_args()
    try:
        if args.archive:
            archive(args.archive, args.prefix)
        else:
            check()
            print('public source boundary: clean')
    except (ValueError, OSError) as e:
        print(f'public-boundary: {e}', file=sys.stderr)
        sys.exit(1)
