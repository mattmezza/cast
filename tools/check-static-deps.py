#!/usr/bin/env python3
"""Report missing static-link inputs without changing the build or downloading SDKs."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--edition', choices=('community', 'pro'), default='community')
    parser.add_argument('--lgpl-root', type=Path)
    parser.add_argument('--x11', choices=('0', '1'), default='1')
    parser.add_argument('--wayland', choices=('0', '1'), default='1')
    parser.add_argument('--panel', choices=('0', '1'), default='1')
    parser.add_argument('--stt', choices=('0', '1'), default='0')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    environment = os.environ.copy()
    if args.edition == 'pro':
        if args.lgpl_root is None:
            parser.error('Pro requires --lgpl-root pointing to its controlled media SDK')
        metadata = args.lgpl_root.resolve() / 'prefix/lib/pkgconfig'
        environment['PKG_CONFIG_PATH'] = str(metadata)
    packages = ['fontconfig', 'freetype2', 'libavcodec', 'libavformat', 'libavutil',
                'libswscale', 'libswresample', 'libpipewire-0.3']
    if args.x11 == '1':
        packages += ['x11', 'xext', 'xrandr', 'xi', 'xfixes', 'xcomposite']
    if args.wayland == '1':
        packages += ['gio-2.0', 'gio-unix-2.0']
    if args.panel == '1':
        packages += ['sdl3', 'sdl3-ttf']
    if args.edition == 'pro':
        packages += ['libsodium']
        if args.stt == '1':
            packages += ['whisper']
    compiler = shlex.split(environment.get('CC', 'cc'))
    rows = []
    for package in packages:
        flags = subprocess.run(['pkg-config', '--static', '--libs', package], env=environment,
                               capture_output=True, text=True, check=False)
        if flags.returncode:
            rows.append({'package': package, 'error': flags.stderr.strip(), 'missing': []})
            continue
        tokens = shlex.split(flags.stdout)
        directories = [Path(token[2:]) for token in tokens if token.startswith('-L')]
        libraries = []
        for token in tokens:
            if not token.startswith('-l'):
                continue
            name = token[2:]
            filename = name[1:] if name.startswith(':') else 'lib' + name + '.a'
            # An exact shared-object linker input is not a static archive.
            archive = None
            if filename.endswith('.a'):
                archive = next((str(directory / filename) for directory in directories
                                if (directory / filename).is_file()), None)
                if archive is None:
                    lookup = subprocess.run(compiler + ['-print-file-name=' + filename],
                                            capture_output=True, text=True, check=False)
                    candidate = Path(lookup.stdout.strip())
                    if lookup.returncode == 0 and candidate.is_file():
                        archive = str(candidate.resolve())
            libraries.append({'name': name, 'archive': archive})
        rows.append({'package': package, 'libraries': libraries,
                     'missing': sorted({library['name'] for library in libraries
                                        if library['archive'] is None})})
    missing = sorted({name for row in rows for name in row['missing']})
    report = {'edition': args.edition, 'audit_only': True,
              'static_link_inputs_present': not missing and not any('error' in row for row in rows),
              'fully_static_binary_verified': False, 'inference_helper': args.edition == 'pro' and args.stt == '1',
              'missing_libraries': missing,
              'packages': rows,
              'runtime_requirements': ['desktop display/compositor', 'PipeWire server/modules',
                                       'portal service for Wayland capture',
                                       'v4l2loopback module/device for virtual camera',
                                       'fonts for composition text', 'TLS trust store']}
    if args.edition == 'pro' and args.stt == '1':
        report['runtime_requirements'] += ['separate structured STT helper', 'explicit local STT/VAD model assets',
                                           'reviewed CPU/GPU inference backend modules']
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print('Static dependency input audit (' + args.edition + '); no static binary produced.')
        for row in rows:
            detail = row.get('error') or ('missing: ' + ', '.join(row['missing'])
                                         if row['missing'] else 'reported archives present')
            print(row['package'] + ': ' + detail)
        print('Archive presence alone does not verify static linking or runtime completeness.')
    return 0 if report['static_link_inputs_present'] else 2


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print('check-static-deps: ' + str(error), file=sys.stderr)
        sys.exit(2)
