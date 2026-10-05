#!/usr/bin/env python3
"""Stage the real install recipes and check app/headless launcher contracts."""
import configparser
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / 'cast'


def digest():
    return hashlib.sha256(BINARY.read_bytes()).hexdigest()


def make(*arguments):
    # The parent suite owns the built executable. Never relink it during installation checks.
    result = subprocess.run(['make', '-o', 'cast', 'X11=0', 'WAYLAND=0', *arguments],
                            cwd=ROOT, text=True, capture_output=True, timeout=20)
    assert result.returncode == 0, (result.stdout, result.stderr)


with tempfile.TemporaryDirectory(prefix='cast-app-install-') as directory:
    temporary = Path(directory)
    before = digest()
    app = temporary / 'app'
    make('PANEL=1', 'PREFIX=/usr', f'DESTDIR={app}', 'install')
    executable = app / 'usr/bin/cast'
    launcher = app / 'usr/bin/cast-app'
    assert executable.is_file() and os.access(executable, os.X_OK)
    assert launcher.is_symlink() and os.readlink(launcher) == 'cast'
    assert hashlib.sha256(executable.read_bytes()).hexdigest() == before
    entry = app / 'usr/share/applications/cast.desktop'
    desktop = configparser.ConfigParser(interpolation=None)
    desktop.read(entry)
    properties = desktop['Desktop Entry']
    assert properties['Type'] == 'Application' and properties['Name'] == 'Cast'
    assert properties['Exec'] == 'cast-app' and properties['Terminal'] == 'false'
    assert properties['Icon'] == 'cast' and properties['StartupWMClass'] == 'CastPanel'
    ET.parse(app / 'usr/share/icons/hicolor/scalable/apps/cast.svg')
    if shutil.which('desktop-file-validate'):
        subprocess.run(['desktop-file-validate', str(entry)], check=True, timeout=5)

    # The dmenu executable resolves its companion after relocation and preserves literal args.
    executable.write_text('#!/usr/bin/python3\nimport json,sys\nprint(json.dumps(sys.argv[1:]))\n')
    arguments = ['--config', 'configuration with spaces $(touch unwanted)', '--no-camera']
    result = subprocess.run([str(launcher), *arguments], capture_output=True, text=True,
                            cwd=temporary, timeout=5)
    assert result.returncode == 0 and json.loads(result.stdout) == arguments
    assert not (temporary / 'unwanted').exists()

    headless = temporary / 'headless'
    make('PANEL=0', 'PREFIX=/usr', f'DESTDIR={headless}', 'install')
    assert (headless / 'usr/bin/cast').is_file()
    assert not (headless / 'usr/bin/cast-app').exists()
    assert not (headless / 'usr/share/applications/cast.desktop').exists()
    assert not (headless / 'usr/share/icons/hicolor/scalable/apps/cast.svg').exists()

    make('PANEL=1', 'PREFIX=/usr', f'DESTDIR={app}', 'uninstall')
    assert not launcher.is_symlink() and not executable.exists() and not entry.exists()
    assert not (app / 'usr/share/icons/hicolor/scalable/apps/cast.svg').exists()
    assert digest() == before, 'install checks changed the shared executable'

print('application install: desktop entry, dmenu launcher, literal args, headless build and uninstall passed')
