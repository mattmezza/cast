#!/usr/bin/env python3
"""Stage edition binaries, dependency evidence and public source; never publish."""
import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


def run(*args):
    subprocess.run(args, cwd=ROOT, check=True)


def main():
    p = argparse.ArgumentParser()
    for name in ('edition', 'binary', 'version', 'media-profile', 'pro-root', 'lgpl-root'):
        p.add_argument('--' + name, required=True)
    for name in ('x11', 'wayland', 'panel', 'official'):
        p.add_argument('--' + name, type=int, choices=(0, 1), required=True)
    p.add_argument('--stt', type=int, choices=(0, 1), default=0)
    p.add_argument('--trust-header', default='')
    p.add_argument('--stt-helper-path', default='/usr/libexec/cast-pro-stt-helper')
    p.add_argument('--stt-cpu-backend', default='')
    p.add_argument('--stt-profile', default=str(ROOT / 'build/deps-stt/profile.json'))
    p.add_argument('--stt-engineering', type=int, choices=(0, 1), default=0)
    a = p.parse_args()
    if a.edition == 'pro' and a.stt and a.stt_helper_path != '/usr/libexec/cast-pro-stt-helper':
        raise ValueError('STT packages require PREFIX=/usr and STT_HELPER_PATH=/usr/libexec/cast-pro-stt-helper; rebuild with those values')
    private_module = pathlib.Path(a.pro_root)
    if (private_module / 'pro/src/pro_provider.c').is_file():
        private_module /= 'pro'
    arch = subprocess.check_output(['uname', '-m'], text=True).strip()
    runtime_env = os.environ.copy()
    if a.edition == 'pro':
        runtime_env['LD_LIBRARY_PATH'] = str(pathlib.Path(a.lgpl_root).resolve() / 'prefix/lib')
    identity = json.loads(subprocess.check_output([str((ROOT / a.binary).resolve()), 'edition', '--json'], text=True, env=runtime_env))
    # Read edition nested schema as returned by current public API.
    core_identity = identity.get('identity', identity)
    if a.edition == 'pro' and a.official:
        if not core_identity.get('official', False) or not core_identity.get('release_timestamp', 0):
            raise ValueError('official Pro package requires production identity and authenticated release inputs')
        metadata = private_module / 'release/production.json'
        if not metadata.is_file():
            raise ValueError('official Pro needs reviewed private release/production.json and production trusted keys')
        proof = json.loads(metadata.read_text())
        if proof.get('schema') != 1 or proof.get('reviewed') is not True:
            raise ValueError('production release metadata missing owner review')
        for field in ('version', 'core_revision', 'private_revision', 'release_timestamp', 'platform', 'media_profile'):
            if proof.get(field) != core_identity.get(field):
                raise ValueError(f'production release metadata differs from compiled identity: {field}')
        for field in ('trusted_key_set_sha256', 'legal_review_reference', 'patent_sdk_review_reference', 'clean_runtime_acceptance_reference'):
            if not proof.get(field):
                raise ValueError(f'production input missing: {field}')
        actual = hashlib.sha256((private_module / 'src/trusted_keys.h').read_bytes()).hexdigest()
        if proof['trusted_key_set_sha256'] != actual:
            raise ValueError('production key-set provenance mismatch')
    dist = ROOT / 'dist'
    dist.mkdir(exist_ok=True)
    dependency_source_asset = None
    speech_source_asset = None
    stage = dist / f'stage-{a.edition}-{a.media_profile}'
    if stage.exists():
        shutil.rmtree(stage)
    run('make', f'EDITION={a.edition}', f'MEDIA_PROFILE={a.media_profile}', f'PRO_ROOT={a.pro_root}',
        f'LGPL_ROOT={a.lgpl_root}', f'X11={a.x11}', f'WAYLAND={a.wayland}', f'PANEL={a.panel}',
        f'STT={a.stt}', f'PRO_TRUST_HEADER={a.trust_header}',
        f'STT_HELPER_PATH={a.stt_helper_path}', f'STT_CPU_BACKEND={a.stt_cpu_backend}',
        f'STT_PROFILE={a.stt_profile}', f'STT_ENGINEERING={a.stt_engineering}',
        f'OFFICIAL_RELEASE={a.official}', f'DESTDIR={stage}', 'PREFIX=/usr', 'install')
    doc = stage / f'usr/share/doc/{a.binary}'
    if a.edition == 'pro':
        # Validate and complete the staged closure before ldd or execution can
        # follow a loader path. Audit the actual $ORIGIN media, not an SDK override.
        lib = stage / 'usr/lib/cast-pro/media'
        lib.mkdir(parents=True, exist_ok=True)
        for source in (pathlib.Path(a.lgpl_root) / 'prefix/lib').glob('*.so*'):
            if source.is_symlink():
                target = source.readlink()
                if target.is_absolute() or '..' in target.parts:
                    raise ValueError('controlled library symlink escapes package directory')
                if not (lib / source.name).is_symlink():
                    (lib / source.name).symlink_to(target)
            elif source.is_file():
                shutil.copy2(source, lib / source.name)
    audit_args = ['python3', 'packaging/audit.py', 'inventory', '--edition', a.edition, '--media-profile', a.media_profile,
                  '--pro-root', a.pro_root,
                  '--binary', str(stage / f'usr/bin/{a.binary}'), '--official', str(a.official), '--lgpl-root', a.lgpl_root, '--output', str(doc / 'dependencies')]
    if a.official:
        audit_args.append('--strict')
    run(*audit_args)
    if a.edition == 'pro' and a.stt:
        helper = stage / 'usr/libexec/cast-pro-stt-helper'
        if not helper.is_file():
            raise ValueError('inference-enabled package omitted its structured helper')
        helper_audit = audit_args.copy()
        helper_audit[helper_audit.index('--binary') + 1] = str(helper)
        helper_audit[helper_audit.index('--output') + 1] = str(doc / 'speech-dependencies')
        run(*helper_audit)
        speech_profile = pathlib.Path(a.stt_profile).resolve()
        speech_evidence = json.loads(speech_profile.read_text())
        if speech_evidence.get('schema') != 1:
            raise ValueError('inference profile evidence has an unsupported schema')
        shutil.copy2(speech_profile, doc / 'speech-profile.json')
        notices = speech_profile.parent / 'THIRD-PARTY-NOTICES.txt'
        texts = speech_profile.parent / 'license-texts'
        if not notices.is_file() or not texts.is_dir():
            raise ValueError('inference profile omitted preserved notices/license texts')
        shutil.copy2(notices, doc / 'speech-dependencies/SDK-NOTICES.txt')
        shutil.copytree(texts, doc / 'speech-dependencies/SDK-license-texts', dirs_exist_ok=True)
        speech_sources = speech_evidence.get('corresponding_sources')
        if speech_sources:
            source = pathlib.Path(speech_sources)
            if (not source.is_file() or hashlib.sha256(source.read_bytes()).hexdigest() !=
                    speech_evidence.get('corresponding_sources_sha256')):
                raise ValueError('controlled speech source kit is missing or changed')
            speech_source_asset = dist / f'cast-pro-{a.version}-linux-{arch}-speech-dependency-sources.tar.gz'
            shutil.copy2(source, speech_source_asset)
        elif a.official:
            raise ValueError('official speech package requires matching controlled speech sources')

    if a.edition == 'pro':
        private = private_module
        for filename in ('LICENSE', 'EULA-DRAFT.md'):
            if not (private / filename).is_file():
                raise ValueError(f'private legal notice absent: {filename}')
            destination_name = 'PRO-LICENSE' if filename == 'LICENSE' else filename
            shutil.copy2(private / filename, stage / f'usr/share/licenses/cast-pro/{destination_name}')
        profile = pathlib.Path(a.lgpl_root) / 'profile.json'
        shutil.copy2(profile, doc / 'media-profile.json')
        # Corresponding source includes all controlled shared dependency builds; excludes private app source.
        source_bundle = pathlib.Path(a.lgpl_root) / 'corresponding-sources.tar.gz'
        if source_bundle.exists():
            source_evidence = json.loads(profile.read_text())
            if source_evidence.get('corresponding_sources') != source_bundle.name or hashlib.sha256(source_bundle.read_bytes()).hexdigest() != source_evidence.get('corresponding_sources_sha256'):
                raise ValueError('corresponding dependency-source bundle differs from media profile')
            dependency_source_asset = dist / f'cast-pro-{a.version}-linux-{arch}-lgpl-dependency-sources.tar.gz'
            shutil.copy2(source_bundle, dependency_source_asset)
        elif a.official:
            raise ValueError('official Pro requires matching controlled dependency sources/build material')
    (doc / 'integration-manifest.json').write_text(json.dumps(identity, indent=2) + '\n')
    (doc / 'build-info.txt').write_text(f'{a.binary} {a.version}\nEdition: {a.edition}\nMedia: {a.media_profile}\nX11={a.x11} WAYLAND={a.wayland} PANEL={a.panel}\nProduction={a.official}\n' + subprocess.check_output(['ldd', a.binary], text=True))
    name = f'{a.binary}-{a.version}-linux-{arch}-{a.media_profile}'
    artifact = dist / f'{name}.tar.gz'
    with tarfile.open(artifact, 'w:gz') as tar:
        tar.add(stage / 'usr', arcname='usr')
    # Always explicit public source manifest, never include the private checkout/build objects.
    public_archive = dist / f'cast-{a.version}-source.tar.gz'
    run('python3', 'packaging/public-boundary.py', '--archive', str(public_archive), '--prefix', f'cast-{a.version}')
    sums = dist / f'{name}-SHA256SUMS'
    assets = [artifact, public_archive]
    if dependency_source_asset:
        assets.append(dependency_source_asset)
    if speech_source_asset:
        assets.append(speech_source_asset)
    if a.edition == 'community':
        # Preserve longstanding Community filenames used by local release tooling.
        compatibility = dist / f'cast-{a.version}-linux-{arch}.tar.gz'
        shutil.copy2(artifact, compatibility)
        assets.append(compatibility)
    sums.write_text(''.join(f'{hashlib.sha256(x.read_bytes()).hexdigest()}  {x.name}\n' for x in assets))
    print(f'local {a.edition} package staged: {artifact}; production={a.official}')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as e:
        print(f'package-edition: {e}', file=sys.stderr)
        sys.exit(1)
