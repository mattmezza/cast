#!/usr/bin/env python3
"""Create an ad-hoc signed macOS app, optionally bundling its runtime libraries."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile


def create_icon(source, destination):
    # sips uses the native SVG renderer; no external image tool is needed.
    with tempfile.TemporaryDirectory(prefix='cast-icon-') as temporary:
        iconset = Path(temporary) / 'Cast.iconset'
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                name = f'icon_{size}x{size}' + ('@2x' if scale == 2 else '') + '.png'
                subprocess.run(['sips', '-s', 'format', 'png', '-z', str(size * scale),
                                str(size * scale), str(source), '--out', str(iconset / name)],
                               check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['iconutil', '-c', 'icns', str(iconset), '-o', str(destination)], check=True)


def bundle_libraries(contents, extra_libraries=()):
    """Copy the Mach-O dependency closure and rewrite every non-system load path."""
    frameworks = contents / 'Frameworks'
    frameworks.mkdir()
    pending = [(path, path) for path in (contents / 'MacOS').iterdir() if path.is_file()]
    copied = {}
    names = {}
    signed = []
    minimum = (13, 0)
    for source in extra_libraries:
        source = Path(source).resolve(strict=True)
        if source in copied:
            continue
        destination = frameworks / source.name
        if source.name in names and names[source.name] != source:
            raise ValueError(f'Conflicting runtime dylib basename: {source.name}')
        names[source.name] = source
        shutil.copy2(source, destination)
        destination.chmod(0o755)
        copied[source] = destination
        pending.append((destination, source))
    while pending:
        target, original = pending.pop(0)
        loads = subprocess.check_output(['otool', '-L', str(original)], text=True).splitlines()[1:]
        rpaths = []
        commands = subprocess.check_output(['otool', '-l', str(original)], text=True)
        build = subprocess.check_output(['vtool', '-show-build', str(original)], text=True)
        for value in re.findall(r'\bminos (\d+(?:\.\d+)+)', build):
            minimum = max(minimum, tuple(map(int, value.split('.'))))
        for block in commands.split('cmd LC_RPATH')[1:]:
            match = re.search(r'\n\s*path (.+?) \(offset', block)
            if match:
                rpaths.append(match.group(1).replace('@loader_path', str(original.parent)))
        for line in loads:
            dependency = line.strip().split(' (compatibility')[0]
            if dependency.startswith(('/System/', '/usr/lib/')):
                continue
            if dependency.startswith('@loader_path/'):
                source = original.parent / dependency[len('@loader_path/'):]
            elif dependency.startswith('@rpath/'):
                relative = dependency[len('@rpath/'):]
                candidates = [Path(root) / relative for root in rpaths]
                candidates += [original.parent / relative]
                source = next((path for path in candidates if path.exists()), None)
                if source is None:
                    raise ValueError(f'Cannot resolve {dependency} from {original}')
            else:
                source = Path(dependency)
            source = source.resolve(strict=True)
            # The first dylib load entry is its own install name.
            if source == original.resolve() and target.parent == frameworks:
                continue
            if source not in copied:
                if source.name in names and names[source.name] != source:
                    raise ValueError(f'Conflicting runtime dylib basename: {source.name}')
                names[source.name] = source
                destination = frameworks / source.name
                shutil.copy2(source, destination)
                destination.chmod(0o755)
                copied[source] = destination
                subprocess.run(['install_name_tool', '-id', '@rpath/' + source.name,
                                str(destination)], check=True)
                pending.append((destination, source))
            relative = '@loader_path/' + ('../Frameworks/' if target.parent.name == 'MacOS' else '') + copied[source].name
            subprocess.run(['install_name_tool', '-change', dependency, relative, str(target)], check=True)
        signed.append(target)
    notices = contents / 'Resources/dependency-licenses'
    notices.mkdir()
    for source, destination in copied.items():
        if 'Cellar' in source.parts:
            index = source.parts.index('Cellar')
            formula = Path(*source.parts[:index + 3])
            target = notices / formula.parent.name
            target.mkdir(exist_ok=True)
            for candidate in formula.iterdir():
                if candidate.is_file() and candidate.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE', 'AUTHORS')):
                    shutil.copy2(candidate, target / candidate.name)
    for target in signed:
        subprocess.run(['codesign', '--force', '--sign', '-', str(target)], check=True)
    manifest = {}
    for source, path in copied.items():
        row = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'source': str(source)}
        if 'Cellar' in source.parts:
            index = source.parts.index('Cellar')
            row.update(formula=source.parts[index + 1], version=source.parts[index + 2])
        manifest[path.name] = row
    (contents / 'Resources/runtime-libraries.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return '.'.join(map(str, minimum))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--stt-helper', type=Path)
    parser.add_argument('--media-root', type=Path)
    parser.add_argument('--speech-profile', type=Path)
    parser.add_argument('--pro-root', type=Path)
    parser.add_argument('--standalone', action='store_true', help='bundle non-system runtime dylibs')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('app packaging and signing must run on macOS')
    binary = args.binary.resolve(strict=True)
    output = args.output.absolute()
    if output.suffix != '.app' or output.is_symlink():
        parser.error('output must be a nonsymlink .app path')
    identity = json.loads(subprocess.check_output([str(binary), 'edition', '--json'], text=True))
    edition = identity['edition']
    if edition not in ('community', 'pro'):
        parser.error('unrecognized binary edition')
    name = 'cast-pro' if edition == 'pro' else 'cast'
    identifier = 'com.superscreencast.' + edition
    if output.exists():
        try:
            with (output / 'Contents/Info.plist').open('rb') as source:
                old = plistlib.load(source)
            if old.get('CFBundleIdentifier') != identifier:
                parser.error('existing output is not a SuperScreencast development bundle')
        except (OSError, plistlib.InvalidFileException):
            parser.error('existing output is not a valid development bundle')
    output.parent.mkdir(parents=True, exist_ok=True)
    version = subprocess.check_output([str(binary), '--version'], text=True).splitlines()[0]
    # The source version is numeric; --version also includes branding/build details.
    match = re.search(r'\b(\d+\.\d+\.\d+)\b', version)
    if not match:
        parser.error('could not determine binary version')
    with tempfile.TemporaryDirectory(prefix='.cast-app-', dir=output.parent) as temporary:
        bundle = Path(temporary) / output.name
        contents = bundle / 'Contents'
        executable = contents / 'MacOS' / name
        executable.parent.mkdir(parents=True)
        resources = contents / 'Resources'
        resources.mkdir()
        shutil.copy2(binary, executable)
        executable.chmod(0o755)
        if args.stt_helper:
            helper = contents / 'MacOS/cast-pro-stt-helper'
            shutil.copy2(args.stt_helper.resolve(strict=True), helper)
            helper.chmod(0o755)
            subprocess.run(['codesign', '--force', '--sign', '-', str(helper)], check=True)
        plist = {
            'CFBundleIdentifier': identifier,
            'CFBundleName': 'Super Screencast',
            'CFBundleDisplayName': 'Super Screencast ' + edition.title(),
            'CFBundleExecutable': name,
            'CFBundlePackageType': 'APPL',
            'CFBundleIconFile': 'Cast.icns',
            'CFBundleShortVersionString': match.group(1),
            'CFBundleVersion': match.group(1),
            'LSMinimumSystemVersion': '13.0',
            'NSHighResolutionCapable': True,
            'NSScreenCaptureUsageDescription': 'Capture your selected display for recordings.',
            'NSAudioCaptureUsageDescription': 'Include system audio in recordings when enabled.',
            'NSCameraUsageDescription': 'Include your camera in the recording when enabled.',
            'NSMicrophoneUsageDescription': 'Include your microphone in recordings when enabled.',
        }
        with (contents / 'Info.plist').open('wb') as destination:
            plistlib.dump(plist, destination)
        repository = Path(__file__).resolve().parent.parent
        for source in (repository / 'LICENSE', repository / 'docs/macos.md',
                       repository / 'assets/fonts/OFL.txt'):
            shutil.copy2(source, resources / source.name)
        if edition == 'pro':
            if args.pro_root is None:
                parser.error('Pro packaging requires --pro-root for its license notices')
            private = args.pro_root / 'pro' if (args.pro_root / 'pro/src').is_dir() else args.pro_root
            shutil.copy2(private / 'LICENSE', resources / 'PRO-LICENSE')
            shutil.copy2(private / 'EULA-DRAFT.md', resources / 'PRO-EULA-DRAFT.md')
        shutil.copytree(repository / 'licenses', resources / 'license-texts')
        if args.media_root:
            shutil.copytree(args.media_root / 'build-materials/licenses', resources / 'media-licenses')
            shutil.copy2(args.media_root / 'profile.json', resources / 'media-profile.json')
        create_icon(repository / 'assets/macos-icon.svg', resources / 'Cast.icns')
        if args.standalone:
            extra_libraries = []
            if args.speech_profile:
                speech = json.loads(args.speech_profile.read_text())
                extra_libraries = [item['path'] for item in speech['libraries'] if item.get('distributed')]
                shutil.copy2(args.speech_profile, resources / 'speech-profile.json')
                shutil.copytree(args.speech_profile.parent / 'license-texts', resources / 'speech-licenses')
            plist['LSMinimumSystemVersion'] = bundle_libraries(contents, extra_libraries)
            with (contents / 'Info.plist').open('wb') as destination:
                plistlib.dump(plist, destination)
        subprocess.run(['codesign', '--force', '--sign', '-', str(bundle)], check=True)
        subprocess.run(['codesign', '--verify', '--strict', str(bundle)], check=True)
        if output.exists():
            shutil.rmtree(output)
        bundle.rename(output)
    print(f'Created {output}')
    print('Ad-hoc signed app; ' + ('runtime libraries bundled.' if args.standalone else 'requires installed Homebrew runtime libraries.'))


if __name__ == '__main__':
    main()
