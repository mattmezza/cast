#!/usr/bin/env python3
"""Offline edition/config migration and actual shell completion regressions.

Run with --binary ./cast or --binary ./cast-pro. No private fixtures, daemon,
capture devices or network are needed. Missing optional shells are reported skips.
"""
import argparse
import configparser
import ctypes
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / 'cast'


class SplitCliTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='cast-split-cli-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.env = os.environ.copy()
        self.env.update(HOME=str(self.root), XDG_DATA_HOME=str(self.root / 'data'),
                        XDG_CONFIG_HOME=str(self.root / 'config'),
                        XDG_RUNTIME_DIR=str(self.root),
                        PATH=str(BINARY.parent) + os.pathsep + self.env['PATH'])

    def cli(self, *args, expected=0):
        value = subprocess.run([str(BINARY), *map(str, args)], cwd=self.root, env=self.env,
                               text=True, capture_output=True, timeout=15)
        self.assertEqual(value.returncode, expected, (args, value.stdout, value.stderr))
        return value

    def config(self, text, name='input.conf'):
        path = self.root / name
        path.write_text(text)
        return path

    def test_defaults_round_trip_and_new_fields(self):
        text = self.cli('config', 'defaults').stdout
        # Generated defaults group related controls in repeated INI sections.
        # Production config check below still rejects repeated *keys* strictly.
        parser = configparser.ConfigParser(interpolation=None, strict=False)
        parser.read_string(text)
        for section, key, value in (('record', 'video_codec', 'auto'),
                                    ('record', 'bitrate_kbps', '6000'),
                                    ('record', 'rate_control', 'auto'),
                                    ('stream', 'video_encoder', 'auto'),
                                    ('licensing', 'file', ''),
                                    ('licensing', 'upgrade_url', '')):
            self.assertEqual(parser[section][key], value)
        self.cli('config', 'check', self.config(text))

    def test_check_separates_syntax_and_encoder_availability(self):
        path = self.config('[record]\nvideo_codec = nonexistent-cast-test-encoder\n'
                           '[stream]\nvideo_encoder = nonexistent-cast-test-encoder\n')
        self.cli('config', 'check', path)
        result = self.cli('config', 'check', path, '--availability', expected=5)
        self.assertIn('configuration valid', result.stdout)
        self.assertIn('recording unavailable:', result.stderr)
        self.assertIn('streaming encoder unavailable:', result.stderr)
        self.assertIn('nonexistent-cast-test-encoder', result.stderr)

    def test_record_bitrate_bounds(self):
        for value, code in ((100, 0), (100000, 0), (99, 2), (100001, 2), ('2x', 2)):
            with self.subTest(value=value):
                self.cli('config', 'check', self.config(f'[record]\nbitrate_kbps = {value}\n'),
                         expected=code)

    def test_record_rate_control_enum(self):
        for value, code in (('auto', 0), ('bitrate', 0), ('crf', 0), ('quality', 2), ('', 2)):
            with self.subTest(value=value):
                self.cli('config', 'check', self.config(f'[record]\nrate_control = {value}\n'),
                         expected=code)

    def test_literal_string_bounds(self):
        for section, key, limit in (('stream', 'video_encoder', 64),
                                    ('licensing', 'file', 4096)):
            for length, code in ((limit - 1, 0), (limit, 2)):
                with self.subTest(key=key, length=length):
                    self.cli('config', 'check',
                             self.config(f'[{section}]\n{key} = {"x" * length}\n'),
                             expected=code)
        self.cli('config', 'check', self.config('[licensing]\nfile = $HOME/literal-license.json\n'))

    def test_upgrade_url_bounds_and_https_requirement(self):
        for value, code in (('', 0), ('https://example.invalid/buy', 0),
                            ('https://[::1]:8443/info', 0),
                            ('http://example.invalid', 2), ('https://', 2),
                            ('https:///path', 2), ('https://user@example.invalid', 2),
                            ('https://?buy', 2), ('https://#buy', 2), ('https://:443', 2),
                            ('https://[]', 2), ('https://[::1', 2), ('https://[::1]junk', 2),
                            ('https://example.invalid/a b', 2),
                            ('https://' + 'a' * (1023 - 8), 0),
                            ('https://' + 'a' * (1024 - 8), 2)):
            with self.subTest(value=value[:50]):
                self.cli('config', 'check',
                         self.config(f'[licensing]\nupgrade_url = {value}\n'), expected=code)

    def test_unknown_keys_remain_strict(self):
        for text in ('[licensing]\npro = true\n', '[record]\nentitled = true\n',
                     '[stream]\nunknown = value\n', '[unknown]\nkey = value\n'):
            with self.subTest(text=text):
                self.cli('config', 'check', self.config(text), expected=2)

    def test_startup_encoder_flags_validate_without_daemon(self):
        self.cli('--record-bitrate', '100', '--record-rate-control', 'bitrate',
                 '--stream-video-encoder', 'auto', 'features', '--json')
        for args in (('--record-bitrate', '99'), ('--record-rate-control', 'quality'),
                     ('--stream-video-encoder', 'x' * 64)):
            with self.subTest(args=args):
                self.cli(*args, 'features', '--json', expected=1)

    def test_migration_dry_run_preserves_input_and_comments(self):
        original = ('# before\n[record]\nvideo_codec : libx264\ncrf : 19\n'
                    'preset = ultrafast\n# keep legacy values\n[stream]\nvideo_encoder : libx264\n'
                    '[licensing]\nfile = $HOME/literal.json\n')
        path = self.config(original)
        result = self.cli('config', 'migrate', path)
        self.assertEqual(path.read_text(), original)
        self.assertEqual(set(self.root.iterdir()), {path})
        self.assertIn('Dry-run migration', result.stdout)
        for line in ('# before', 'crf : 19', 'preset = ultrafast', '# keep legacy values',
                     'file = $HOME/literal.json', 'video_codec = auto',
                     'rate_control = bitrate', 'video_encoder = auto'):
            self.assertIn(line, result.stdout)

    def test_migration_repeated_sections_colon_assignments(self):
        original = ('# header\n[record]\ncontainer : mkv\n\n[record]\n'
                    'video_codec : libx264\n# keep me\n[stream]\n'
                    'video_encoder : libx264\n[stream]\nvideo_bitrate_kbps : 2500\n')
        path = self.config(original)
        self.cli('config', 'check', path)
        output = self.root / 'output.conf'
        self.cli('config', 'migrate', path, '--write', output)
        self.cli('config', 'check', output)
        self.assertEqual(path.read_text(), original)
        result = output.read_text()
        self.assertEqual(result.count('video_codec = auto'), 1)
        self.assertEqual(result.count('rate_control = bitrate'), 1)
        self.assertEqual(result.count('video_encoder = auto'), 1)
        for line in ('# header', '# keep me', 'container : mkv', 'video_bitrate_kbps : 2500'):
            self.assertIn(line, result)

    def test_migration_backup_and_community_target(self):
        original = '[record]\nvideo_codec = libx264\ncrf = 17\n'
        path = self.config(original)
        backup = self.root / 'backup.conf'
        self.cli('config', 'migrate', path, '--edition', 'community', '--write', path,
                 '--backup', backup)
        self.assertEqual(backup.read_text(), original)
        self.assertEqual(backup.stat().st_mode & 0o777, 0o600)
        self.assertIn('rate_control = auto', path.read_text())
        self.assertIn('crf = 17', path.read_text())
        self.cli('config', 'check', path)
        self.assertFalse(list(self.root.glob('*.migration.*')))

    def test_migration_retains_target_line_comments(self):
        path = self.config('[record]\nvideo_codec : libx264 ; legacy encoder\n'
                           '[stream]\nvideo_encoder = libx264 # stream encoder\n'
                           '[output]\nwidth = 320\nheight = 180\n')
        output = self.root / 'output.conf'
        self.cli('config', 'migrate', path, '--write', output)
        text = output.read_text()
        for comment in ('; legacy encoder', '# stream encoder'):
            self.assertIn(comment, text)
        self.cli('config', 'check', output, '--availability')

    def test_migration_refuses_overwrite_and_existing_backup(self):
        original = '[record]\nvideo_codec = libx264\n'
        path = self.config(original)
        self.cli('config', 'migrate', path, '--write', path, expected=2)
        self.assertEqual(path.read_text(), original)
        output = self.config('existing output\n', 'output.conf')
        self.cli('config', 'migrate', path, '--write', output, expected=5)
        self.assertEqual(output.read_text(), 'existing output\n')
        backup = self.config('existing backup\n', 'backup.conf')
        self.cli('config', 'migrate', path, '--write', path, '--backup', backup, expected=5)
        self.assertEqual(backup.read_text(), 'existing backup\n')
        self.assertEqual(path.read_text(), original)

    def test_migration_refuses_symlink_output(self):
        path = self.config('[record]\nvideo_codec = libx264\n')
        target = self.config('untouched\n', 'target.conf')
        link = self.root / 'link.conf'
        link.symlink_to(target)
        self.cli('config', 'migrate', path, '--write', link, expected=5)
        self.assertEqual(target.read_text(), 'untouched\n')

    def test_edition_features_and_missing_license_are_offline(self):
        before = set(self.root.iterdir())
        edition = json.loads(self.cli('edition', '--json').stdout)
        self.assertEqual(edition['schema'], 1)
        self.assertIn(edition['edition'], ('community', 'pro'))
        self.assertIn(edition['media_profile'], ('system', 'lgpl'))
        features = json.loads(self.cli('features', '--json').stdout)
        self.assertEqual(features['schema'], 1)
        self.assertEqual(len(features['features']), 4)
        for feature in features['features']:
            self.assertFalse(feature['active'])
            expected = ('community_build' if edition['edition'] == 'community' else
                        'not_implemented' if not feature['implemented'] else
                        'dependency_missing' if not feature['compiled'] or not feature['dependency_ready'] else
                        'unsupported_platform' if not feature['platform_supported'] else
                        'license_missing')
            self.assertEqual(feature['reason'], expected)
        status = json.loads(self.cli('license', 'status', '--json').stdout)
        self.assertEqual(status['schema'], 1)
        self.assertEqual(status['state'], 'license_missing')
        self.assertEqual(status['masked_id'], '')
        self.assertEqual(set(self.root.iterdir()), before, 'read-only commands wrote files')
        self.cli('--config', self.root / 'absent.conf', 'edition', '--json')

    def test_cli_argument_errors_are_distinct_from_readiness(self):
        for args in (('config', 'check', '--bogus'), ('config', 'check', '--availability',
                     '--availability'), ('config', 'migrate', '--edition', 'other'),
                     ('config', 'migrate', '--write')):
            with self.subTest(args=args):
                self.cli(*args, expected=2)

    def test_pro_rejects_injected_gpl_media_before_capture(self):
        edition = json.loads(self.cli('edition', '--json').stdout)
        if edition['edition'] != 'pro':
            self.skipTest('GPL loader rejection applies to Pro')
        if not shutil.which('readelf'):
            self.skipTest('readelf unavailable; required FFmpeg SONAME cannot be identified')
        elf = subprocess.run(['readelf', '-d', str(BINARY)], text=True,
                             capture_output=True, timeout=10)
        match = re.search(r'\(NEEDED\).*\[(libavcodec\.so\.\d+)\]', elf.stdout)
        if elf.returncode or match is None:
            self.skipTest('no direct shared libavcodec dependency to inject')
        system_root = Path('/usr/lib')
        gpl_library = None
        for path in (system_root / match.group(1),):
            try:
                library = ctypes.CDLL(str(path))
                library.avcodec_license.restype = ctypes.c_char_p
                license_name = library.avcodec_license().decode('utf-8')
            except (OSError, AttributeError, UnicodeError):
                continue
            if license_name.startswith('GPL '):
                gpl_library = path
                break
        if gpl_library is None:
            self.skipTest('no known GPL FFmpeg in /usr/lib; injection cannot be exercised')
        config = self.config('[output]\nbackend = synthetic\ndevice = none\n'
                             'enabled = false\nwidth = 320\nheight = 180\n'
                             '[camera]\nenabled = false\n')
        socket = self.root / 'never-created.sock'
        env = self.env.copy()
        env['LD_LIBRARY_PATH'] = str(system_root)
        value = subprocess.run([str(BINARY), '--headless', '--config', str(config),
                                '--socket', str(socket)], env=env, cwd=self.root,
                               text=True, capture_output=True, timeout=15)
        self.assertNotEqual(value.returncode, 0, value.stdout)
        self.assertIn('Pro requires LGPL shared media', value.stderr)
        self.assertIn('GPL', value.stderr)
        self.assertFalse(socket.exists(), 'startup reached daemon/capture before media rejection')


class CompletionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='cast-split-completion-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.env = os.environ.copy()
        self.env['PATH'] = str(BINARY.parent) + os.pathsep + self.env['PATH']

    def complete(self, shell, *words):
        if not shutil.which(shell):
            self.skipTest(f'{shell} unavailable; actual shell completion cannot run')
        filename = 'cast.bash' if shell == 'bash' else '_cast'
        script = ROOT / 'completions' / filename
        quoted = ' '.join(shlex.quote(word) for word in words)
        if shell == 'bash':
            program = (f'source {shlex.quote(str(script))}\nCOMP_WORDS=({quoted})\n'
                       f'COMP_CWORD={len(words) - 1}\n_cast_complete\n'
                       'printf "%s\\n" "${COMPREPLY[@]}"\n')
            args = ['bash', '--noprofile', '--norc', '-c', program]
        else:
            # Stub candidate sinks; execute the real parser and branching logic.
            program = ('compadd() { shift; print -l -- "$@"; }\n'
                       '_files() { :; }\n_directories() { :; }\n'
                       f'words=({quoted})\nCURRENT={len(words)}\n'
                       f'source {shlex.quote(str(script))}\n')
            args = ['zsh', '-f', '-c', program]
        value = subprocess.run(args, cwd=self.root, env=self.env, text=True,
                               capture_output=True, timeout=10)
        self.assertEqual(value.returncode, 0, value.stderr)
        return set(value.stdout.splitlines()) - {''}

    def check_shell(self, shell):
        for name in ('cast', 'cast-pro'):
            with self.subTest(shell=shell, binary=name):
                self.assertTrue({'edition', 'features', 'license', '--record-bitrate',
                                 '--record-rate-control', '--stream-video-encoder'} <=
                                self.complete(shell, name, ''))
                self.assertEqual(self.complete(shell, name, '--record-rate-control', ''),
                                 {'auto', 'bitrate', 'crf'})
                self.assertIn('auto', self.complete(shell, name, '--stream-video-encoder', ''))
                self.assertIn('--json', self.complete(shell, name, 'edition', ''))
                self.assertIn('--json', self.complete(shell, name, 'features', ''))
                self.assertEqual(self.complete(shell, name, 'license', ''),
                                 {'status', 'inspect', 'import', 'reload', 'remove'})
                self.assertIn('--json', self.complete(shell, name, 'license', 'status', ''))
                self.assertIn('--json', self.complete(shell, name, 'license', 'inspect', 'a.json', ''))
                self.assertIn('migrate', self.complete(shell, name, 'config', ''))
                self.assertIn('--availability', self.complete(shell, name, 'config', 'check', '--'))
                self.assertTrue({'--edition', '--write', '--backup'} <=
                                self.complete(shell, name, 'config', 'migrate', '--'))
                self.assertEqual(self.complete(shell, name, 'config', 'migrate', '--edition', ''),
                                 {'community', 'pro'})
                self.assertEqual(self.complete(shell, name, 'settings', 'record.rate_control', ''),
                                 {'auto', 'bitrate', 'crf'})
                self.assertIn('auto', self.complete(shell, name, 'settings', 'stream.video_encoder', ''))

    def test_bash(self):
        self.check_shell('bash')
        fixture = self.root / 'source with spaces.conf'
        fixture.write_text('# fixture\n')
        self.assertIn(str(fixture), self.complete('bash', 'cast', 'config', 'migrate',
                                               '--write', str(self.root / 'source')))

    def test_zsh(self):
        self.check_shell('zsh')

    def test_fish(self):
        if not shutil.which('fish'):
            self.skipTest('fish unavailable; native fish completion remains unexecuted')
        script = ROOT / 'completions/cast.fish'
        for command, expected in (('cast ', {'edition', 'features', 'license'}),
                                   ('cast-pro ', {'edition', 'features', 'license'}),
                                   ('cast config migrate --', {'--edition', '--write', '--backup'}),
                                   ('cast config check input.conf --', {'--availability'}),
                                   ('cast --record-rate-control ', {'auto', 'bitrate', 'crf'})):
            program = f'source {shlex.quote(str(script))}; complete -C {shlex.quote(command)}'
            value = subprocess.run(['fish', '--no-config', '-c', program], cwd=self.root,
                                   text=True, capture_output=True, timeout=10)
            self.assertEqual(value.returncode, 0, value.stderr)
            actual = {line.split('\t')[0] for line in value.stdout.splitlines()}
            self.assertTrue(expected <= actual, (command, actual))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, default=BINARY)
    args, rest = parser.parse_known_args()
    BINARY = args.binary.resolve()
    unittest.main(argv=[__file__, *rest], verbosity=2)
