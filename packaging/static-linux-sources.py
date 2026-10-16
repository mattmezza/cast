#!/usr/bin/env python3
"""Collect exact Ubuntu source packages owning archives consumed by the static link."""
import json
from pathlib import Path
import re
import shutil
import subprocess

root = Path(__file__).resolve().parent.parent
build = root / 'build/static-linux'
output = build / 'corresponding-sources'
output.mkdir(exist_ok=True)
shutil.copytree(build / 'upstream', output / 'upstream', dirs_exist_ok=True)
shutil.copytree(root / 'packaging', output / 'build-recipes', dirs_exist_ok=True)
shutil.copy2(build / 'build-info.json', output)
shutil.copy2(build / 'cast.map', output)
ubuntu = output / 'ubuntu'
ubuntu.mkdir(exist_ok=True)
packages = {}
archives = sorted(set(re.findall(r'(/[^\s()]+\.a)(?:\(|\s)', (build / 'cast.map').read_text())))
for archive in archives:
    path = Path(archive).resolve()
    if path.is_relative_to(build):
        continue
    # Linker maps retain GCC search paths containing ../..; dpkg stores
    # canonical install paths. Try the original path too for usrmerge aliases.
    candidates = [str(path), archive]
    if str(path).startswith('/usr/lib/'):
        candidates.append(str(path)[4:])
    for candidate in dict.fromkeys(candidates):
        ownership = subprocess.run(['dpkg-query', '-S', candidate], text=True,
                                   capture_output=True)
        if ownership.returncode == 0:
            break
    else:
        raise RuntimeError('No source-package owner for linked archive: ' + archive)
    owner = ownership.stdout.split(': /')[0].splitlines()[0]
    source, version = subprocess.check_output(
        ['dpkg-query', '-W', '-f=${source:Package}\t${source:Version}', owner], text=True).split('\t')
    packages[source] = version
for source, version in sorted(packages.items()):
    subprocess.run(['apt-get', 'source', '--download-only', source + '=' + version], cwd=ubuntu, check=True)
(output / 'ubuntu-source-versions.json').write_text(json.dumps(packages, indent=2) + '\n')
# Include full application source so the LGPL combined work can be rebuilt/relinked.
with (output / 'cast-source.tar.gz').open('wb') as target:
    subprocess.run(['git', 'archive', '--format=tar.gz', '--prefix=cast/', 'HEAD'], cwd=root, stdout=target, check=True)
