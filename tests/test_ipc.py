#!/usr/bin/env python3
"""Exercise the real daemon/client with synthetic media and actual Matroska files."""
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

BINARY = str(Path(__file__).resolve().parent.parent / 'cast')


def run(*args, env=None, ok=True):
    result = subprocess.run([BINARY, *map(str, args)], env=env, text=True,
                            capture_output=True, timeout=15)
    if ok:
        assert result.returncode == 0, (args, result.stdout, result.stderr)
    else:
        assert result.returncode != 0, (args, result.stdout, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix='cast-test-') as directory:
    root = Path(directory)
    config = root / 'cast.conf'
    sock = root / 'cast.sock'
    env = os.environ.copy()
    env['XDG_RUNTIME_DIR'] = directory
    env['XDG_CONFIG_HOME'] = str(root / 'missing-config')
    config.write_text('[camera]\nwidth_percent=25\n[ipc]\ntimeout_ms=400\n')
    common = ['--config', str(config), '--socket', str(sock)]
    defaults = run('config', 'defaults', env=env).stdout
    defaults_path = root / 'defaults.conf'
    defaults_path.write_text(defaults)
    run('config', 'check', defaults_path, env=env)
    run('--config', root / 'missing.conf', 'config', 'check', env=env, ok=False)
    run('--backend', 'garbage', 'doctor', env=env, ok=False)

    # An owned abandoned socket is safely removed while holding the instance lock.
    abandoned = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    abandoned.bind(str(sock))
    os.chmod(sock, 0o600)
    abandoned.close()
    process = subprocess.Popen([BINARY, *common, '--backend', 'synthetic',
                                '--camera-device', 'synthetic', '--output-device', 'none',
                                '--width', '320', '--height', '240', '--fps', '20'],
                               env=env, stdout=subprocess.DEVNULL,
                               stderr=(root / 'daemon.log').open('w'))

    def cmd(*args, ok=True):
        return run(*common, *args, env=env, ok=ok)

    def state():
        return json.loads(cmd('status', '--json').stdout)

    try:
        deadline = time.monotonic() + 10
        while True:
            if process.poll() is not None:
                raise AssertionError((root / 'daemon.log').read_text())
            if sock.exists():
                try:
                    first = state()
                    break
                except AssertionError:
                    pass
            assert time.monotonic() < deadline
            time.sleep(0.02)
        assert first['live']['state'] == 'paused'
        assert first['record']['state'] == 'stopped'
        assert os.stat(sock).st_mode & 0o777 == 0o600
        run(*common, '--backend', 'synthetic', '--no-live', '--no-camera', env=env, ok=False)
        assert state()['live']['state'] == 'paused'
        cmd('record', 'toggle', ok=False)
        cmd('live', 'resume')
        cmd('camera', 'size', '+5%')
        cmd('camera', 'size', '95%', ok=False)
        cmd('camera', 'position', '-10', '30')
        cmd('camera', 'anchor', 'next')
        cmd('camera', 'shape', 'circle')
        cmd('camera', 'aspect', '16:9')
        cmd('camera', 'crop', 'move', '20', '-10')
        cmd('camera', 'mirror', 'toggle')
        cmd('camera', 'hide')
        cmd('layout', 'next')
        cmd('split', 'ratio', '30%')
        cmd('split', 'side', 'right')
        cmd('zoom', 'set', '2.5')
        cmd('zoom', 'toggle')
        assert state()['zoom'] == 1
        cmd('zoom', 'toggle')
        assert state()['zoom'] == 2.5
        cmd('annotations', 'record', 'keys', 'off')
        cmd('keys', 'on', ok=False)  # Synthetic has no passive desktop input.
        cmd('cursor', 'highlight', 'toggle')
        cmd('preset', 'demo')
        cmd('preset', 'next')
        cmd('preview', 'on', ok=False)
        cmd('layout', 'overlay', 'extra', ok=False)
        cmd('camera', 'move', 'NaN', '0', ok=False)
        cmd('zoom', 'set', 'nan', ok=False)
        cmd('record', 'nonsense', ok=False)
        cmd('live', 'unknown', ok=False)
        cmd('audio', 'desktop', 'on', ok=False)

        path = root / 'presentation.mkv'
        cmd('record', 'start', path)
        time.sleep(0.35)
        cmd('pause')
        cmd('pause')
        paused = state()
        assert paused['live']['state'] == 'paused'
        assert paused['record']['state'] == 'paused'
        duration = paused['record']['duration']
        time.sleep(0.15)
        assert abs(state()['record']['duration'] - duration) < 0.03
        cmd('record', 'pause')  # Independent pause supersedes remembered restoration.
        cmd('resume')
        assert state()['live']['state'] == 'live'
        assert state()['record']['state'] == 'paused'
        cmd('record', 'resume')
        time.sleep(0.25)
        cmd('live', 'freeze')
        cmd('live', 'pause')
        cmd('live', 'unfreeze')
        assert state()['live']['state'] == 'paused'
        cmd('reset')
        assert state()['live']['state'] == 'paused'
        assert state()['record']['state'] == 'recording'

        # Failed reloads preserve state and prior composition; overrides survive reload.
        old = state()
        config.write_text('[composition]\nlayout=screen\n[output]\nwidth=640\n'
                          '[ipc]\ntimeout_ms=400\n')
        cmd('config', 'reload')
        new = state()
        assert new['layout'] == 'screen'
        assert new['live']['state'] == 'paused'
        assert new['record']['state'] == 'recording'
        config.write_text('[composition]\nlayout=camera\n[output]\nbackend=invalid\n')
        cmd('config', 'reload', ok=False)
        assert state()['layout'] == 'screen'
        config.write_text('[composition]\nlayout=camera\n[record]\nvideo_codec=ffv1\n')
        cmd('config', 'reload', ok=False)
        assert state()['layout'] == 'screen'
        assert state()['record']['state'] == 'recording'
        config.write_text('[composition]\nlayout=screen\n[ipc]\ntimeout_ms=400\n')
        stopped = cmd('record', 'stop')
        assert str(path) in stopped.stdout
        assert path.exists() and path.stat().st_size > 1000
        cmd('record', 'start', path, ok=False)  # Never implicitly overwrite.
        assert state()['record']['state'] == 'stopped'

        # Concurrent complete messages are independently acknowledged.
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            list(pool.map(lambda _: cmd('status', '--json'), range(12)))
        for payload in (b'garbage', b'CAST1\0\0', b'CAST1\0status', b'x' * 9000):
            peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            peer.settimeout(2)
            peer.connect(str(sock))
            peer.send(payload)
            response = peer.recv(8192)
            assert response.startswith(b'1 '), response
            peer.close()
        silent = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        silent.settimeout(2)
        silent.connect(str(sock))
        assert silent.recv(8192).startswith(b'1 ')
        silent.close()
        assert state()['record']['state'] == 'stopped'

        config.write_text('[record]\ncountdown=1\n[ipc]\ntimeout_ms=400\n')
        cmd('config', 'reload')
        cmd('record', 'start', root / 'cancelled.mkv')
        assert state()['record']['countdown']
        cmd('pause')
        time.sleep(1.2)
        assert not state()['record']['countdown']
        assert state()['record']['state'] == 'stopped'
        assert not (root / 'cancelled.mkv').exists()
        cmd('quit')
        assert process.wait(timeout=10) == 0
        assert not sock.exists()
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

print('daemon IPC, commands, privacy, recording, precedence and atomic reload tests passed')
