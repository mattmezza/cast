#!/usr/bin/env python3
"""Package an existing signed app without modifying its contents (macOS only)."""
import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile


def run(*args):
    return subprocess.check_output(args)


def contents(root):
    result = {}
    for path in sorted(root.rglob('*')):
        name = str(path.relative_to(root))
        if path.is_symlink():
            result[name] = ('link', os.readlink(path))
        elif path.is_file():
            result[name] = ('file', path.stat().st_mode & 0o777, hashlib.sha256(path.read_bytes()).hexdigest())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    app = args.app.resolve()
    output = args.output.resolve()
    if not app.is_dir() or app.suffix != '.app':
        parser.error('--app must be an existing .app directory')
    if output.exists():
        parser.error('refusing to overwrite an existing disk image')
    run('codesign', '--verify', '--deep', '--strict', str(app))
    expected = contents(app)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cast-dmg-') as temporary:
        temporary = Path(temporary)
        stage = temporary / 'stage'
        stage.mkdir()
        run('ditto', str(app), str(stage / app.name))
        (stage / 'Applications').symlink_to('/Applications')
        info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
        icon_name = info.get('CFBundleIconFile', '')
        icon = app / 'Contents/Resources' / icon_name
        if not icon.suffix:
            icon = icon.with_suffix('.icns')
        if not icon.is_file():
            raise RuntimeError('App icon missing')
        shutil.copy2(icon, stage / '.VolumeIcon.icns')
        # kHasCustomIcon on the volume root, inherited by the disk image.
        os.setxattr(stage, 'com.apple.FinderInfo', bytes(8) + b'\x04\x00' + bytes(22))
        writable = temporary / 'layout.dmg'
        run('hdiutil', 'create', '-quiet', '-fs', 'HFS+', '-volname', app.stem,
            '-srcfolder', str(stage), '-format', 'UDRW', str(writable))
        mount = temporary / 'mounted'
        mount.mkdir()
        run('hdiutil', 'attach', '-quiet', '-nobrowse', '-mountpoint', str(mount), str(writable))
        try:
            # Finder runs only on the packaging machine (hosted release runner).
            script = '''on run argv
set diskPath to POSIX file (item 1 of argv)
set appName to item 2 of argv
tell application "Finder"
set w to open folder diskPath
set current view of w to icon view
set toolbar visible of w to false
set statusbar visible of w to false
set bounds of w to {100, 100, 700, 470}
set arrangement of icon view options of w to not arranged
set icon size of icon view options of w to 96
set position of item appName of w to {150, 150}
set position of item "Applications" of w to {450, 150}
close w
end tell
end run'''
            subprocess.run(['osascript', '-', str(mount), app.name], input=script.encode(), check=True)
            run('sync')
        finally:
            run('hdiutil', 'detach', '-quiet', str(mount))
        run('hdiutil', 'convert', '-quiet', str(writable), '-format', 'UDZO', '-o', str(output))
        run('hdiutil', 'verify', str(output))
        run('hdiutil', 'attach', '-quiet', '-readonly', '-nobrowse', '-mountpoint', str(mount), str(output))
        try:
            assert (mount / 'Applications').is_symlink()
            assert os.readlink(mount / 'Applications') == '/Applications'
            assert contents(mount / app.name) == expected, 'Packaged app differs from original'
            run('codesign', '--verify', '--deep', '--strict', str(mount / app.name))
            assert (mount / '.VolumeIcon.icns').is_file()
        finally:
            run('hdiutil', 'detach', '-quiet', str(mount))
    print(f'Verified DMG, unchanged app signature and bytes: {output}')


if __name__ == '__main__':
    main()
