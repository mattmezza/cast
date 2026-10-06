#!/usr/bin/env python3
"""Real Pro executable offline lifecycle/IPC and signed bundle integration.
Requires explicit nonproduction binary/private key fixture paths; no real devices.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import shlex
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
BINARY = PRIVATE = FIXTURES = None


class ProCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='cast-pro-cli-build-')
        cls.binary = pathlib.Path(cls.build.name) / 'cast-pro'
        shutil.copy2(BINARY, cls.binary)
        identity = json.loads(subprocess.check_output([str(cls.binary), 'edition', '--json'], text=True))
        if identity['edition'] != 'pro' or identity['official']:
            raise ValueError('explicit nonproduction Pro binary required')
        cls.identity = identity

    @classmethod
    def tearDownClass(cls): cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='cast-pro-cli-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.socket = self.root / 'cast-pro.sock'
        self.env = os.environ.copy()
        self.env.update(XDG_RUNTIME_DIR=str(self.root), XDG_DATA_HOME=str(self.root / 'data'),
                        XDG_CONFIG_HOME=str(self.root / 'config'))
        self.license = FIXTURES / 'perpetual.json'
        self.daemon = None

    def cli(self, *args, check=False):
        value = subprocess.run([str(self.binary), '--socket', str(self.socket), *map(str,args)],
                               env=self.env, text=True, capture_output=True, timeout=10)
        if check: self.assertEqual(value.returncode, 0, value.stderr)
        return value

    def test_local_import_invalid_replacement_reload_remove(self):
        self.assertIn('license_missing', self.cli('license', 'status', '--json', check=True).stdout)
        self.cli('license', 'import', self.license, check=True)
        store = self.root / 'data/cast/license.json'
        accepted = store.read_bytes()
        self.assertEqual(store.stat().st_mode & 0o777, 0o600)
        status = json.loads(self.cli('license','status','--json',check=True).stdout)
        self.assertEqual(status['state'], 'valid')
        self.assertNotIn('test-license', json.dumps(status))
        value = self.cli('license','import', FIXTURES / 'wrong-signature.json')
        self.assertNotEqual(value.returncode, 0)
        self.assertEqual(store.read_bytes(), accepted)
        self.cli('license','reload',check=True)
        features = json.loads(self.cli('features','--json',check=True).stdout)
        rows = {feature['id']: feature for feature in features['features']}
        self.assertEqual(set(rows), {'editable_projects', 'cinematic_zoom',
                                   'transcription_subtitles', 'speech_teleprompter'})
        self.assertFalse(rows['editable_projects']['implemented'])
        self.assertEqual(rows['editable_projects']['reason'], 'not_implemented')
        for name in ('cinematic_zoom', 'transcription_subtitles', 'speech_teleprompter'):
            self.assertTrue(rows[name]['implemented'])
            self.assertIsInstance(rows[name]['compiled'], bool)
            self.assertTrue(rows[name]['entitled'])
        self.cli('license','remove',check=True)
        self.assertFalse(store.exists())
        self.assertIn('license_missing',self.cli('license','status','--json',check=True).stdout)

    def start_daemon(self):
        self.log = open(self.root / 'daemon.log', 'w')
        self.daemon = subprocess.Popen([str(self.binary), '--headless', '--socket', str(self.socket),
                                       '--backend', 'synthetic', '--output-device', 'none', '--no-camera',
                                       '--no-virtual', '--width', '320', '--height', '180', '--fps', '10'],
                                      env=self.env, stdout=self.log, stderr=subprocess.STDOUT)
        def cleanup():
            if self.daemon.poll() is None:
                self.cli('quit')
                try: self.daemon.wait(timeout=5)
                except subprocess.TimeoutExpired: self.daemon.terminate(); self.daemon.wait(timeout=5)
            self.log.close()
        self.addCleanup(cleanup)
        for _ in range(100):
            if self.socket.exists(): return
            if self.daemon.poll() is not None:
                self.log.flush(); message = (self.root / 'daemon.log').read_text()
                if 'bind: Operation not permitted' in message:
                    self.skipTest('sandbox denies local Unix sockets; run this daemon/IPC check in a permitted runtime')
                self.fail(message)
            time.sleep(0.02)
        self.fail('daemon did not create socket')

    def test_authenticated_daemon_license_mutations_preserve_outputs(self):
        self.start_daemon()
        before = json.loads(self.cli('status','--json',check=True).stdout)
        self.cli('license','import',self.license,check=True)
        self.assertIn('valid',self.cli('license','status','--json',check=True).stdout)
        self.cli('license','reload',check=True)
        self.cli('license','remove',check=True)
        after = json.loads(self.cli('status','--json',check=True).stdout)
        for key in ('virtual','record','stream'):
            self.assertEqual(before[key]['state'], after[key]['state'])
        self.assertIn('license_missing',self.cli('license','status','--json',check=True).stdout)

    def test_signed_binary_bundle_end_to_end(self):
        self.cli('license','import',self.license,check=True)
        bundle = self.root / 'bundle'; bundle.mkdir()
        shutil.copy2(self.binary,bundle / 'cast-pro')
        claims = {'schema':1,'product':'cast-pro','key_id':'test-update-1',
                  'version':self.identity['version'],'platform':self.identity['platform'],
                  'media_profile':'lgpl','extension_api':self.identity['extension_api'],
                  # The fixture updater signs a positive nonproduction release
                  # claim; source builds intentionally carry timestamp zero.
                  'release_timestamp':max(250,self.identity['release_timestamp']),
                  'core_revision':self.identity['core_revision'],'private_revision':self.identity['private_revision'],
                  'sha256':hashlib.sha256((bundle / 'cast-pro').read_bytes()).hexdigest()}
        cp = self.root / 'claims.json'; cp.write_text(json.dumps(claims))
        subprocess.run(['python3',str(PRIVATE / 'tools/issuer.py'),'issue','--key',
                        str(FIXTURES / 'test-update-1.secret.json'),'--claims',str(cp),
                        '--out',str(bundle / 'manifest.json')],check=True,stdout=subprocess.DEVNULL)
        self.cli('update','--bundle',bundle,check=True)
        destination = self.root / 'cast-pro'; destination.write_bytes(b'previous eligible executable'); destination.chmod(0o755)
        self.cli('update','--bundle',bundle,'--install',destination,check=True)
        self.assertEqual((self.root / 'cast-pro.rollback').read_bytes(),b'previous eligible executable')
        self.assertEqual(json.loads(subprocess.check_output([str(destination),'edition','--json'],text=True)),self.identity)

    def test_current_workflow_help_and_dormant_schema(self):
        help_text = self.cli('--help', check=True).stdout
        for text in ('zoom cinematic', 'zoom focus', 'transcription', 'subtitles', 'notes'):
            self.assertIn(text, help_text)
        defaults = self.cli('config', 'defaults', check=True).stdout
        for text in ('[zoom]', '[cursor]', '[transcription]', '[subtitles]', '[notes]'):
            self.assertIn(text, defaults)
        config = self.root / 'dormant.conf'
        for minimum, maximum, factor in ((1, 1, 1), (3, 4, 3)):
            config.write_text(f'[zoom]\nmin = {minimum}\nmax = {maximum}\nfactor = {factor}\n'
                              'motion = legacy\nauto = off\nauto_factor = 2\n')
            self.cli('config', 'check', config, check=True)
            config.write_text(config.read_text().replace('auto = off', 'auto = click'))
            self.assertNotEqual(self.cli('config', 'check', config).returncode, 0)

    def shell_completion(self, shell, *tokens):
        words = [str(self.binary), *tokens]
        if shell == 'bash':
            code = 'source ' + shlex.quote(str(ROOT / 'completions/cast.bash')) + '\n'
            code += 'COMP_WORDS=(' + ' '.join(shlex.quote(x) for x in words) + ')\n'
            code += f'COMP_CWORD={len(words)-1}\n_cast_complete\n'
            code += 'printf "%s\\n" "${COMPREPLY[@]}"\n'
            command = ['bash', '--noprofile', '--norc', '-c', code]
        else:
            code = 'compadd() { shift; print -l -- "$@"; }\n'
            code += 'words=(' + ' '.join(shlex.quote(x) for x in words) + ')\n'
            code += f'CURRENT={len(words)}\nsource ' + shlex.quote(str(ROOT / 'completions/_cast')) + '\n'
            command = ['zsh', '-f', '-c', code]
        value = subprocess.run(command, env=self.env, text=True, capture_output=True, timeout=10)
        self.assertEqual(value.returncode, 0, value.stderr)
        return set(value.stdout.splitlines()) - {''}

    def completion_cases(self, shell):
        self.assertTrue({'transcription', 'transcribe', 'subtitles', 'notes'} <=
                        self.shell_completion(shell, ''))
        self.assertTrue({'toggle', 'in', 'out', 'reset', 'set', 'follow', 'motion', 'focus',
                         'auto', 'status', 'cinematic'} <= self.shell_completion(shell, 'zoom', ''))
        for tokens, expected in ((('zoom','motion',''), {'legacy','cinematic'}),
                (('zoom','auto',''), {'off','click'}), (('zoom','status',''), {'--json'}),
                (('zoom','focus','0.7','0.3',''), {'--factor'}),
                (('notes','mode',''), {'timed','speech'}), (('notes','goto',''), {'--line'}),
                (('transcription','source',''), {'mic','desktop','mix'}),
                (('subtitles','sidecar',''), {'none','srt','vtt','both'}),
                (('settings','notes.format',''), {'auto','plain','markdown'}),
                (('settings','cursor.smooth',''), {'true','false'}),
                (('update',''), {'--bundle','--install','--download-only','--rollback'})):
            with self.subTest(shell=shell, tokens=tokens):
                self.assertEqual(self.shell_completion(shell, *tokens), expected)

    def test_bash_private_command_completions(self):
        self.completion_cases('bash')

    @unittest.skipUnless(shutil.which('zsh'), 'optional zsh is not installed')
    def test_zsh_private_command_completions(self):
        self.completion_cases('zsh')

    @unittest.skipUnless(shutil.which('fish'), 'optional fish is not installed')
    def test_fish_private_command_completions(self):
        env = self.env.copy()
        env['PATH'] = str(self.binary.parent) + os.pathsep + env['PATH']
        code = 'source ' + shlex.quote(str(ROOT / 'completions/cast.fish')) + '\n'
        code += 'complete -C "cast-pro zoom "\n'
        value = subprocess.run(['fish', '--no-config', '-c', code], env=env,
                               text=True, capture_output=True, timeout=10)
        self.assertEqual(value.returncode, 0, value.stderr)
        tokens = {line.split('\t')[0] for line in value.stdout.splitlines()}
        self.assertTrue({'cinematic','motion','focus','auto','status'} <= tokens)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=pathlib.Path, required=True)
    parser.add_argument('--pro-root', type=pathlib.Path, required=True)
    parser.add_argument('--fixtures', type=pathlib.Path,
                        help='explicit matching nonproduction key/license fixtures')
    args, remaining = parser.parse_known_args()
    BINARY = args.binary.resolve()
    PRIVATE = args.pro_root.resolve()
    FIXTURES = args.fixtures.resolve() if args.fixtures else PRIVATE / 'tests/generated'
    unittest.main(argv=[__file__, *remaining], verbosity=2)
