#!/usr/bin/env python3
"""Create an ad-hoc signed local app; Homebrew runtime libraries stay external."""
import argparse
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('app packaging and signing must run on macOS')
    binary = args.binary.resolve(strict=True)
    output = args.output.absolute()
    if output.suffix != '.app' or output.is_symlink():
        parser.error('output must be a nonsymlink .app path')
    identifier = 'com.superscreencast.community'
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
    import re
    match = re.search(r'\b(\d+\.\d+\.\d+)\b', version)
    if not match:
        parser.error('could not determine binary version')
    with tempfile.TemporaryDirectory(prefix='.cast-app-', dir=output.parent) as temporary:
        bundle = Path(temporary) / output.name
        contents = bundle / 'Contents'
        executable = contents / 'MacOS/cast'
        executable.parent.mkdir(parents=True)
        resources = contents / 'Resources'
        resources.mkdir()
        shutil.copy2(binary, executable)
        executable.chmod(0o755)
        plist = {
            'CFBundleIdentifier': identifier,
            'CFBundleName': 'SuperScreencast',
            'CFBundleDisplayName': 'SuperScreencast Community',
            'CFBundleExecutable': 'cast',
            'CFBundlePackageType': 'APPL',
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
        subprocess.run(['codesign', '--force', '--sign', '-', str(bundle)], check=True)
        subprocess.run(['codesign', '--verify', '--strict', str(bundle)], check=True)
        if output.exists():
            shutil.rmtree(output)
        bundle.rename(output)
    print(f'Created {output}')
    print('Local development bundle; requires installed Homebrew libraries on this Mac.')


if __name__ == '__main__':
    main()
