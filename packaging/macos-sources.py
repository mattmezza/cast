#!/usr/bin/env python3
"""Collect verified upstream sources for the exact Homebrew libraries in an app.

Explicit network operation. Never substitutes current sources for older bottles.
Controlled media/speech source kits are collected separately by their builders.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
from urllib.parse import urlsplit


def run(*command):
    return subprocess.check_output(command, text=True)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def collect(args):
    manifest = args.app / 'Contents/Resources/runtime-libraries.json'
    rows = json.loads(manifest.read_text())
    formulas = {}
    for basename, row in rows.items():
        library = args.app / 'Contents/Frameworks' / basename
        if Path(basename).name != basename or not library.is_file() or sha(library) != row.get('sha256'):
            raise ValueError('bundled library differs from runtime manifest: ' + basename)
        source = Path(row['source'])
        if 'Cellar' not in source.parts:
            continue
        i = source.parts.index('Cellar')
        formula, version = source.parts[i + 1:i + 3]
        if row.get('formula', formula) != formula or row.get('version', version) != version:
            raise ValueError('runtime manifest formula identity disagrees with source path')
        if formula in formulas and formulas[formula] != version:
            raise ValueError('multiple versions of one bundled formula')
        formulas[formula] = version
    if not formulas:
        raise ValueError('standalone app has no Homebrew source inventory')
    if args.output.exists():
        raise ValueError('source archive already exists; refusing overwrite')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='macos-sources-', dir=args.output.parent) as temp:
        root = Path(temp) / 'homebrew-sources'
        root.mkdir()
        shutil.copy2(manifest, root / manifest.name)
        shutil.copy2(__file__, root / 'macos-sources.py')
        inventory = []
        for name, version in sorted(formulas.items()):
            if not re.fullmatch(r'[A-Za-z0-9@+_.-]+', name):
                raise ValueError('invalid bundled formula name')
            data = json.loads(run('brew', 'info', '--json=v2', name))['formulae'][0]
            stable = data['versions']['stable']
            expected = stable + (f'_{data["revision"]}' if data['revision'] else '')
            if version != expected or version not in {x['version'] for x in data['installed']}:
                raise ValueError(f'{name}: bundled {version} differs from installed current stable {expected}; exact historic recipe required')
            directory = root / name
            directory.mkdir()
            keg = Path(run('brew', '--cellar', name).strip()) / version
            receipt = keg / 'INSTALL_RECEIPT.json'
            if not receipt.is_file():
                raise ValueError(f'{name}: exact installed bottle receipt missing')
            shutil.copy2(receipt, directory / receipt.name)
            installed_recipe = keg / '.brew' / (name + '.rb')
            if installed_recipe.is_file():
                shutil.copy2(installed_recipe, directory / 'installed-formula.rb')
            (directory / 'brew-info.json').write_text(json.dumps(data, indent=2) + '\n')
            recipe_path = directory / (name.replace('@', '-') + '.rb')
            recipe_url = f'https://raw.githubusercontent.com/Homebrew/homebrew-core/{data["tap_git_head"]}/{data["ruby_source_path"]}'
            subprocess.run(['curl', '--fail', '--location', '--retry', '3', '--proto', '=https', '--output', str(recipe_path), recipe_url], check=True)
            if sha(recipe_path) != data['ruby_source_checksum']['sha256']:
                raise ValueError(f'{name}: formula source differs from brew metadata')
            # The Formula API includes platform-specific resources and external
            # patches; embedded patches are already preserved in formula.rb.
            ruby = '''path=Pathname(ARGV.fetch(0)); mod=Module.new; Object.const_set(:CastSourceRecipe, mod); mod.const_set(:BUILD_FLAGS, []); mod.module_eval(path.read, path.to_s); klass=mod.constants.map { |k| mod.const_get(k) }.find { |k| k.is_a?(Class) && k < Formula }; f=klass.new(path.stem, path, :stable); puts({resources: (f.stable.resources.respond_to?(:values) ? f.stable.resources.values : f.stable.resources).map { |r| {name: r.name, url: r.url, checksum: r.checksum.to_s} }, patches: f.stable.patches.select(&:external?).map { |p| {name: "patch", url: p.resource.url, checksum: p.resource.checksum.to_s} }}.to_json)'''
            extras = json.loads(run('brew', 'ruby', '-e', ruby, str(recipe_path.resolve())))
            downloads = [{'name': 'upstream', **data['urls']['stable']}, *extras['resources'], *extras['patches']]
            for index, download in enumerate(downloads):
                checksum = download.get('checksum', '')
                url = download['url']
                if not re.fullmatch(r'[a-f0-9]{64}', checksum) or urlsplit(url).scheme != 'https':
                    raise ValueError(f'{name}: source requires a SHA256-pinned HTTPS archive: {url}')
                basename = Path(urlsplit(url).path).name or 'source'
                target = directory / f'{index:02d}-{basename}'
                subprocess.run(['curl', '--fail', '--location', '--retry', '3', '--proto', '=https', '--proto-redir', '=https', '--output', str(target), url], check=True)
                if sha(target) != checksum:
                    raise ValueError(f'{name}: source SHA256 mismatch: {url}')
                download['file'] = str(target.relative_to(root))
            inventory.append({'formula': name, 'version': version, 'license': data.get('license'),
                              'formula_sha256': sha(recipe_path), 'downloads': downloads})
        (root / 'inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
        (root / 'README.txt').write_text('Sources for the exact Homebrew formula versions bundled in runtime-libraries.json.\nOriginal upstream archives, formula recipes (including embedded patches), external patches and resources are SHA256 verified against brew metadata.\nBuild using the preserved formula recipes and Homebrew build dependencies recorded in brew-info.json. Bottle build toolchains are not reproduced by this collector.\nControlled media and speech SDK sources are separate release archives.\n')
        (root / 'SHA256SUMS').write_text(''.join(f'{sha(p)}  {p.relative_to(root)}\n' for p in sorted(root.rglob('*')) if p.is_file()))
        temporary = args.output.with_suffix(args.output.suffix + '.partial')
        try:
            with tarfile.open(temporary, 'w:gz') as archive:
                archive.add(root, arcname=root.name)
            temporary.replace(args.output)
        finally:
            temporary.unlink(missing_ok=True)
    print(args.output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        collect(args)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'macos-sources: {error}')
