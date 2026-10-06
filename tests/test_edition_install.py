#!/usr/bin/env python3
"""Stage side-by-side edition installs without relinking shared executables."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile
import tarfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--pro-root', type=pathlib.Path, required=True)
    p.add_argument('--lgpl-root', type=pathlib.Path, required=True)
    a = p.parse_args()
    hashes = {name: digest(ROOT / name) for name in ('cast', 'cast-pro')}
    with tempfile.TemporaryDirectory(prefix='cast-edition-install-') as d:
        stage = pathlib.Path(d)
        def make(edition, action):
            binary = 'cast-pro' if edition == 'pro' else 'cast'
            argv = ['make', '-o', binary, f'EDITION={edition}', 'X11=1', 'WAYLAND=1', 'PANEL=1',
                    f'DESTDIR={stage}', 'PREFIX=/usr', action]
            if edition == 'pro':
                argv += ['MEDIA_PROFILE=lgpl', f'PRO_ROOT={a.pro_root.resolve()}', f'LGPL_ROOT={a.lgpl_root.resolve()}']
            subprocess.run(argv, cwd=ROOT, check=True, stdout=subprocess.DEVNULL, timeout=30)
        make('community', 'install')
        make('pro', 'install')
        for binary in ('cast', 'cast-pro'):
            assert digest(stage / f'usr/bin/{binary}') == hashes[binary]
            assert os.readlink(stage / f'usr/bin/{binary}-app') == binary
            desktop = (stage / f'usr/share/applications/{binary}.desktop').read_text()
            assert f'Exec={binary}-app' in desktop and f'Icon={binary}' in desktop
            assert (stage / f'usr/share/icons/hicolor/scalable/apps/{binary}.svg').is_file()
            assert (stage / f'usr/share/licenses/{binary}/LICENSE').read_bytes() == (ROOT / 'LICENSE').read_bytes()
            assert (stage / f'usr/share/doc/{binary}/editions.md').is_file()
            assert (stage / f'usr/share/man/man1/{binary}.1').is_file()
        assert (stage / 'usr/share/licenses/cast-pro/PRO-LICENSE').read_bytes() == (a.pro_root / 'LICENSE').read_bytes()
        assert (stage / 'usr/lib/cast-pro/media/libavcodec.so').is_symlink()
        ldd = subprocess.check_output(['ldd',str(stage / 'usr/bin/cast-pro')],text=True)
        for line in ldd.splitlines():
            if any(f in line for f in ('libavcodec.so','libavformat.so','libavutil.so','libswscale.so','libswresample.so')):
                assert '/lib/cast-pro/media/' in line, line
        subprocess.run(['bash','-n',str(stage / 'usr/share/bash-completion/completions/cast-pro')],check=True)
        make('community','uninstall')
        assert not (stage / 'usr/bin/cast').exists()
        assert (stage / 'usr/bin/cast-pro').exists()
        assert (stage / 'usr/lib/cast-pro/media/libavcodec.so').exists()
        make('pro','uninstall')
        assert not (stage / 'usr/bin/cast-pro').exists()
        assert not (stage / 'usr/lib/cast-pro').exists()
    assert hashes == {name:digest(ROOT / name) for name in ('cast','cast-pro')}
    archive = ROOT / 'dist/cast-0.9.0-source.tar.gz'
    if archive.exists():
        with tarfile.open(archive) as tar:
            for name in tar.getnames():
                assert not any(x in name for x in ('cast-pro-private','/pro_provider.c','/trusted_keys.h','/issuer.py','.secret.json','__pycache__'))
    print('edition install: side-by-side identities, MIT/private notices, origin media, independent uninstall and source archive boundary passed')


if __name__ == '__main__': main()
