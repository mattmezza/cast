#!/usr/bin/env python3
"""Evidence-based runtime inventory. Unknown library choices fail official Pro closed."""
import argparse
import ctypes
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MEDIA = ('libavcodec', 'libavformat', 'libavutil', 'libswscale', 'libswresample')
# Library-level alternatives; package-wide lists must not assign GPL tools to libc.
CHOICES = {
 'freetype2': 'FTL', 'fontconfig': 'HPND AND Unicode-DFS-2016', 'glibc': 'LGPL-2.1-or-later',
 'libgcc': 'GPL-3.0-or-later WITH GCC-exception-3.1',
 'libstdc++': 'GPL-3.0-or-later WITH GCC-exception-3.1',
 'gcc-libs': 'GPL-3.0-or-later WITH GCC-exception-3.1',
 'libgomp': 'GPL-3.0-or-later WITH GCC-exception-3.1',
 'whisper-cpp': 'MIT', 'ggml': 'MIT',
 'glib2': 'LGPL-2.1-or-later', 'glib2-devel': 'LGPL-2.1-or-later',
 'libpipewire': 'MIT', 'pipewire': 'MIT', 'graphite': 'LGPL-2.1-or-later', 'harfbuzz': 'MIT', 'libsodium': 'ISC',
 'openh264': 'BSD-2-Clause', 'openssl': 'Apache-2.0', 'sdl3': 'Zlib', 'sdl3_ttf': 'Zlib',
 'expat': 'MIT', 'zlib': 'Zlib', 'libpng': 'Libpng-2.0', 'bzip2': 'bzip2-1.0.6',
 'brotli': 'MIT', 'libffi': 'MIT', 'pcre2': 'BSD-3-Clause', 'libx11': 'MIT',
 'libxext': 'MIT', 'libxrandr': 'MIT', 'libxi': 'MIT', 'libxfixes': 'MIT',
 'libxcomposite': 'MIT', 'libxrender': 'MIT', 'libxau': 'MIT', 'libxdmcp': 'MIT',
 'libxcb': 'MIT', 'util-linux-libs': 'LGPL-2.1-or-later', 'libcap': 'BSD-3-Clause',
 'libgcrypt': 'LGPL-2.1-or-later', 'libgpg-error': 'LGPL-2.1-or-later',
 'xz': '0BSD', 'lz4': 'BSD-2-Clause', 'zstd': 'BSD-3-Clause',
 'dbus': 'AFL-2.1', 'systemd-libs': 'LGPL-2.1-or-later',
 'ffmpeg': 'GPL-3.0-only', 'x264': 'GPL-2.0-only', 'x265': 'GPL-2.0-or-later',
 'xvidcore': 'GPL-2.0-only',
}
# Choices are conservative library family evidence, reviewed scope kept explicit.
ALLOWED = ('MIT', 'ISC', 'BSD-', 'Zlib', 'FTL', 'HPND', 'Unicode-', 'Apache-', 'Libpng-',
           'bzip2-', '0BSD', 'LGPL-', 'MPL-2.0', 'AFL-2.1')


def run(*args, required=False):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if required and result.returncode:
        raise ValueError(f'{args[0]} failed: {result.stderr.strip()}')
    return result.stdout if not result.returncode else ''


def digest(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def profile(args):
    if args.edition == 'pro':
        if args.media_profile != 'lgpl':
            raise ValueError('Pro requires controlled LGPL media; no system/GPL bypass')
        pro = pathlib.Path(args.pro_root).resolve()
        if (pro / 'pro/src/pro_provider.c').is_file():
            pro = pro / 'pro'
        if pro.is_relative_to(ROOT):
            raise ValueError('private checkout must be outside the public worktree')
        for name in ('src/pro_provider.c', 'src/bounded_json.c', 'src/trusted_keys.h'):
            if not (pro / name).is_file():
                raise ValueError(f'private provider incomplete: {name}')
        header_path = pathlib.Path(args.trust_header) if args.trust_header else pro / 'src/trusted_keys.h'
        header = header_path.read_text()
        if args.official and (not re.search(r'#define\s+CAST_TRUST_PRODUCTION\s+1\b', header) or not re.search(r'#define\s+CAST_LICENSE_TRUST_COUNT\s+[1-9][0-9]*\b', header) or not re.search(r'#define\s+CAST_UPDATE_TRUST_COUNT\s+[1-9][0-9]*\b', header)):
            raise ValueError('official Pro cannot trust nonproduction test keys')
        if args.official and args.release_timestamp <= 0:
            raise ValueError('official release timestamp is required')
    if args.media_profile == 'lgpl':
        base = pathlib.Path(args.lgpl_root).resolve()
        manifest = base / 'profile.json'
        if not manifest.is_file():
            raise ValueError('controlled LGPL profile absent; run make deps-lgpl explicitly')
        data = json.loads(manifest.read_text())
        if args.official and not data.get('release_eligible', False):
            raise ValueError('candidate/unreviewed media provenance cannot produce an official release')
        if args.official:
            lock = json.loads((ROOT / 'packaging/media-lgpl-lock.json').read_text())
            if data.get('sources') != lock['sources'] or data.get('patches') != lock['patches']:
                raise ValueError('official dependency sources/patches differ from reviewed lock')
            source_bundle = base / data.get('corresponding_sources', 'absent')
            if not source_bundle.is_file() or digest(source_bundle) != data.get('corresponding_sources_sha256'):
                raise ValueError('official corresponding-source bundle missing or hash mismatch')
        if data.get('profile') != 'lgpl' or data.get('schema') != 1:
            raise ValueError('invalid controlled LGPL profile manifest')
        flags = ' '.join(data.get('configure_flags', []))
        if '--disable-gpl' not in flags or '--disable-nonfree' not in flags or '--disable-autodetect' not in flags:
            raise ValueError('media profile must explicitly exclude GPL/nonfree/autodetected libraries')
        if '--enable-gpl' in flags or '--enable-nonfree' in flags:
            raise ValueError('disallowed FFmpeg configure flags')
        libraries = data.get('libraries', {})
        if not libraries:
            raise ValueError('media profile has no verified library inventory')
        actual_files = {str(p.relative_to(base)) for p in (base / 'prefix/lib').glob('*.so*')
                        if p.is_file() and not p.is_symlink()}
        if actual_files != set(libraries):
            raise ValueError('controlled tree contains uncatalogued or missing shared libraries')
        for link in (base / 'prefix/lib').glob('*.so*'):
            if link.is_symlink() and (link.readlink().is_absolute() or '..' in link.readlink().parts or not link.resolve().is_relative_to(base / 'prefix/lib')):
                raise ValueError('controlled library symlink escapes relocatable media directory')
        for name, sha in libraries.items():
            p = base / name
            if not p.resolve().is_relative_to(base) or not p.is_file() or digest(p) != sha:
                raise ValueError(f'media profile provenance mismatch: {name}')
        codec = next((base / 'prefix/lib').glob('libavcodec.so.*'), None)
        if not codec:
            raise ValueError('controlled libavcodec missing')
        av = ctypes.CDLL(str(codec))
        av.avcodec_configuration.restype = ctypes.c_char_p
        av.avcodec_license.restype = ctypes.c_char_p
        actual_configuration = av.avcodec_configuration().decode('utf-8')
        actual_license = av.avcodec_license().decode('utf-8')
        if '--enable-gpl' in actual_configuration or '--enable-nonfree' in actual_configuration or 'LGPL' not in actual_license:
            raise ValueError('controlled profile actually loaded GPL/nonfree FFmpeg')
        for family in MEDIA:
            path = run('env', f'PKG_CONFIG_PATH={base}/prefix/lib/pkgconfig', 'pkg-config', '--variable=libdir', family).strip()
            if pathlib.Path(path).resolve() != base / 'prefix/lib':
                raise ValueError(f'{family} pkg-config resolved outside controlled profile: {path}')
    print(f'edition profile validated: {args.edition}/{args.media_profile}')


def package_metadata(path):
    pkg = run('pacman', '-Qqo', str(path)).strip() if shutil.which('pacman') else ''
    raw = run('pacman', '-Qi', pkg) if pkg else ''
    fields = {}
    for line in raw.splitlines():
        if ' : ' in line:
            k, v = line.split(' : ', 1)
            fields[k.strip()] = v.strip()
    return pkg, fields


def inventory(args):
    output = pathlib.Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    notices = output / 'license-texts'
    notices.mkdir(exist_ok=True)
    controlled = pathlib.Path(args.lgpl_root).resolve()
    profile_path = controlled / 'profile.json'
    media_evidence = json.loads(profile_path.read_text()) if profile_path.is_file() else None
    controlled_sources = {x['name']: x for x in media_evidence.get('sources', [])} if media_evidence else {}
    locked_sources = json.loads((ROOT / 'packaging/media-lgpl-lock.json').read_text())['sources']
    for locked in locked_sources:
        recorded = controlled_sources.get(locked['name'])
        if recorded and recorded.get('version') == locked['version'] and recorded.get('sha256') == locked['sha256']:
            controlled_sources[locked['name']] = {**locked, **recorded}
    binary = pathlib.Path(args.binary).resolve()
    result = run('ldd', str(binary), required=True)
    if 'not found' in result:
        raise ValueError('unresolved runtime dependency')
    paths = {pathlib.Path(p).resolve() for p in re.findall(r'(?:=>\s*)?(/\S+)\s+\(', result)}
    # Known dynamic loaders are accounted separately from ELF NEEDED closure.
    dynamic = ROOT / 'packaging/runtime-dynamic.json'
    dyn = json.loads(dynamic.read_text()) if dynamic.exists() else {'libraries': []}
    for entry in dyn['libraries']:
        p = pathlib.Path(entry['path'])
        if entry.get('required') and not p.is_file():
            raise ValueError(f'dynamic runtime absent: {p}')
        if p.is_file():
            paths.add(p.resolve())
    entries, failures = [], []
    for p in sorted(paths):
        package, metadata = package_metadata(p)
        chosen = CHOICES.get(package, 'NOASSERTION')
        source = None
        if p.is_relative_to(controlled / 'prefix/lib') or '/lib/cast-pro/media/' in str(p):
            if p.name.startswith('libopenh264'):
                package, chosen = 'openh264', 'BSD-2-Clause'
                source = controlled_sources.get('openh264')
            elif p.name.startswith(('libssl', 'libcrypto')):
                package, chosen = 'openssl-controlled', 'Apache-2.0'
                source = controlled_sources.get('openssl')
            elif p.name.startswith('libz.so'):
                package, chosen = 'zlib-controlled', 'Zlib'
                source = controlled_sources.get('zlib')
            elif p.name.startswith(MEDIA):
                package, chosen = 'ffmpeg-controlled', 'LGPL-3.0-or-later'
                source = controlled_sources.get('ffmpeg')
            else:
                package, chosen = 'controlled-unreviewed', 'NOASSERTION'
        reviewed = chosen != 'NOASSERTION'
        disallowed = ('GPL-' in chosen and 'WITH GCC-exception-3.1' not in chosen and 'LGPL-' not in chosen) or any(x in chosen for x in ('AGPL-', 'nonfree'))
        if args.edition == 'pro' and (not reviewed or disallowed):
            failures.append(f'{p.name}: {chosen} ({package or "unowned"})')
        copied = []
        if p.is_relative_to(controlled / 'prefix/lib') or '/lib/cast-pro/media/' in str(p):
            materials = controlled / 'build-materials/licenses'
            if materials.is_dir():
                dest = notices / 'controlled'
                shutil.copytree(materials, dest, dirs_exist_ok=True)
                copied.extend(str(x.relative_to(output)) for x in dest.rglob('*') if x.is_file())
        if package and package != 'ffmpeg-controlled':
            directory = pathlib.Path('/usr/share/licenses') / package
            if directory.is_dir():
                dest = notices / package
                shutil.copytree(directory, dest, dirs_exist_ok=True)
                copied.extend(str(x.relative_to(output)) for x in dest.rglob('*') if x.is_file())
        # Explicit alternative texts are preserved, including notices not supplied by Arch.
        for filename in {'libgcc': 'GPL-3.0.txt', 'libstdc++': 'GPL-3.0.txt', 'libgomp': 'GPL-3.0.txt', 'gcc-libs': 'GPL-3.0.txt', 'freetype2': 'FreeType-FTL.txt', 'fontconfig': 'Fontconfig-COPYING.txt', 'glibc': 'LGPL-2.1.txt', 'glib2': 'LGPL-2.1.txt', 'graphite': 'LGPL-2.1.txt', 'systemd-libs': 'LGPL-2.1.txt', 'ffmpeg-controlled': 'LGPL-3.0.txt GPL-3.0.txt'}.get(package, '').split():
            dest = notices / filename
            shutil.copy2(ROOT / 'licenses' / filename, dest)
            copied.append(str(dest.relative_to(output)))
        entries.append({'resolved_file': str(p), 'sha256': digest(p), 'package': package or None,
                        'version': source.get('version') if source else metadata.get('Version'),
                        'package_license_evidence': metadata.get('Licenses'), 'chosen_license': chosen,
                        'reviewed': reviewed, 'linkage': 'shared', 'distributed': args.edition == 'pro' and (p.is_relative_to(controlled / 'prefix/lib') or '/lib/cast-pro/media/' in str(p)),
                        'upstream': source.get('url') if source else metadata.get('URL'), 'source_build_reference': 'profile.json and matching corresponding source bundle; candidate status follows profile release_eligible' if p.is_relative_to(controlled) or '/lib/cast-pro/media/' in str(p) else f'installed distribution source package {package} {metadata.get("Version", "")}',
                        'license_texts': copied})
    components = [{'name': 'Cast public core', 'license': 'MIT', 'linkage': 'compiled', 'distributed': True},
                  {'name': 'inih r60', 'license': 'BSD-3-Clause', 'linkage': 'compiled', 'distributed': True}]
    if args.edition == 'pro':
        components.append({'name': 'Cast Pro private provider', 'license': 'LicenseRef-Cast-Pro-Commercial', 'linkage': 'compiled', 'distributed': True})
    # Font inclusion follows actual executable embedding, no copying installed fonts.
    symbols = run('nm', str(binary))
    if 'cast_panel_font_data' in symbols:
        components.append({'name': 'Inter bundled font', 'license': 'OFL-1.1', 'linkage': 'asset', 'distributed': True})
    if 'Clay_Initialize' in symbols:
        components.append({'name': 'Clay v0.14', 'license': 'MIT', 'linkage': 'compiled', 'distributed': True})
    if args.official and args.edition == 'pro' and dyn.get('required_review'):
        failures.append('dynamic runtime closure lacks recorded clean-image acceptance')
    sbom = {'schema': 1, 'format': 'cast-dependency-inventory', 'edition': args.edition,
            'media_profile': args.media_profile, 'binary': binary.name, 'binary_sha256': digest(binary),
            'runtime_libraries': entries, 'components': components,
            'controlled_media_evidence': media_evidence,
            'dynamic_modules': dyn, 'build_only': ['compiler', 'GNU make', 'pkg-config', 'Python tests', 'curl dependency fetch', 'NASM'],
            'external': ['v4l2loopback kernel module', 'system Noto fonts', 'portal service', 'PipeWire service'],
            'review_failures': failures}
    (output / 'inventory.json').write_text(json.dumps(sbom, indent=2) + '\n')
    (output / 'THIRD-PARTY-NOTICES.txt').write_text('Portions of this software are copyright © 1996–2026 The FreeType Project (www.freetype.org). All rights reserved.\nThis software uses shared FFmpeg libraries under the license recorded below.\nLibraries remain replaceable with ABI-compatible LGPL builds.\n\n' + '\n'.join(f'{x["resolved_file"]}: {x["chosen_license"]}; package {x["package"]}, version {x["version"]}; bundled={x["distributed"]}' for x in entries) + '\n\nFull preserved texts: license-texts/. Unknown choices require review before release. Corresponding sources/build material: exact distribution package sources for system libraries; controlled profile source bundle for shipped media.\n')
    print(f'dependency inventory: {len(entries)} shared libraries, {len(failures)} unresolved Pro reviews -> {output}')
    if args.strict and args.edition == 'pro' and failures:
        raise ValueError('Pro dependency review failed: ' + '; '.join(failures))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=('profile', 'inventory'))
    parser.add_argument('--edition', choices=('community', 'pro'), required=True)
    parser.add_argument('--media-profile', choices=('system', 'lgpl'), required=True)
    parser.add_argument('--lgpl-root', default=str(ROOT / 'build/deps-lgpl'))
    parser.add_argument('--pro-root', default=str(ROOT.parent / 'cast-pro'))
    parser.add_argument('--trust-header', default='')
    parser.add_argument('--official', type=int, choices=(0, 1), default=0)
    parser.add_argument('--release-timestamp', type=int, default=0)
    parser.add_argument('--binary')
    parser.add_argument('--output', default=str(ROOT / 'build/audit'))
    parser.add_argument('--strict', action='store_true')
    args = parser.parse_args()
    try:
        profile(args) if args.action == 'profile' else inventory(args)
    except (OSError, ValueError, KeyError) as e:
        print(f'license-audit: {e}', file=sys.stderr)
        sys.exit(1)
