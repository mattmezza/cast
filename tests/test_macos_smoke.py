#!/usr/bin/env python3
"""Portable synthetic recorder smoke test; also runs on Mac without TCC grants."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    args = parser.parse_args()
    binary = str(args.binary.resolve(strict=True))
    # Keep AF_UNIX paths short even when macOS TMPDIR is deeply nested.
    with tempfile.TemporaryDirectory(prefix='cast-mac-', dir='/tmp') as temporary:
        root = Path(temporary)
        config = root / 'cast.conf'
        config.write_text('[record]\ncontainer=mp4\n')
        common = [binary, '--config', str(config), '--socket', str(root / 'cast.sock')]
        env = dict(os.environ, XDG_RUNTIME_DIR=temporary)

        def command(*arguments, success=True):
            result = subprocess.run([*common, *map(str, arguments)], env=env,
                                    text=True, capture_output=True, timeout=15)
            assert (result.returncode == 0) == success, (arguments, result.stdout, result.stderr)
            return result.stdout

        def status():
            return json.loads(command('status', '--json'))

        def probe_recording(path):
            probe = subprocess.run(['ffprobe', '-v', 'error', '-count_frames',
                                    '-show_streams', '-show_format', '-of', 'json', str(path)],
                                   capture_output=True, text=True, check=True, timeout=15)
            decoded = json.loads(probe.stdout)
            formats = decoded['format']['format_name'].split(',')
            assert ('mp4' if path.suffix == '.mp4' else 'matroska') in formats, (path, formats)
            streams = decoded['streams']
            for kind in ('video', 'audio'):
                assert any(stream['codec_type'] == kind and
                           int(stream.get('nb_read_frames', 0)) > 0
                           for stream in streams), (path, streams)
            return streams

        with (root / 'daemon.log').open('w') as log:
            process = subprocess.Popen([*common, '--headless', '--backend', 'synthetic',
                                        '--no-virtual', '--no-camera', '--width', '320',
                                        '--height', '240', '--fps', '20'], env=env,
                                       stdout=subprocess.DEVNULL, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while True:
                    if process.poll() is not None:
                        raise AssertionError((root / 'daemon.log').read_text())
                    try:
                        initial = status()
                        break
                    except AssertionError:
                        assert time.monotonic() < deadline, (root / 'daemon.log').read_text()
                        time.sleep(.05)
                assert initial['record']['state'] == 'stopped'
                if os.uname().sysname == 'Darwin':
                    command('preview', 'target', 'composition')
                    command('preview', 'on')
                    command('pause')
                    command('resume')
                    command('preview', 'target', 'record')
                    command('preview', 'off')
                target = root / 'smoke.mp4'
                command('record', 'start', target)
                time.sleep(.6)
                recording = status()
                assert recording['record']['path'] == str(target) + '.mkv'
                assert not target.exists(), 'MP4 must not be published during recording'
                with Path(str(target) + '.mkv').open('rb') as in_progress:
                    assert in_progress.read(4) == bytes.fromhex('1a45dfa3'), 'expected Matroska EBML header'
                command('record', 'pause')
                command('record', 'resume')
                time.sleep(.3)
                command('record', 'stop')
                deadline = time.monotonic() + 15
                while status()['record']['finalizing']:
                    assert time.monotonic() < deadline
                    time.sleep(.05)
                assert target.stat().st_size > 0
                assert status()['record']['path'] == str(target)
                assert not Path(str(target) + '.mkv').exists()
                probe_recording(target)
                command('settings', 'record.container', 'matroska')
                mkv = root / 'smoke.mkv'
                command('record', 'start', mkv)
                assert status()['record']['path'] == str(mkv)
                time.sleep(.6)
                command('record', 'stop')
                probe_recording(mkv)
                assert status()['record']['path'] == str(mkv)
                assert not Path(str(mkv) + '.mkv').exists()
                if os.uname().sysname == 'Darwin':
                    command('settings', 'record.countdown', '1')
                    command('preview', 'target', 'composition')
                    command('preview', 'on')
                    countdown = root / 'countdown.mkv'
                    command('record', 'start', countdown)
                    time.sleep(1.6)
                    assert status()['record']['state'] == 'recording'
                    command('record', 'stop')
                    command('preview', 'off')
                    probe_recording(countdown)
                    command('settings', 'record.countdown', '0')
                command('settings', 'record.container', 'mp4')
                if os.uname().sysname == 'Darwin':
                    command('virtual', 'start', success=False)
                    command('stream', 'start', success=False)
                command('quit')
                assert process.wait(timeout=15) == 0
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()

        # A separate disposable daemon must leave its in-progress MKV usable
        # even when no recording trailer or orderly shutdown can be written.
        common = [binary, '--config', str(config), '--socket', str(root / 'recovery.sock')]
        with (root / 'recovery.log').open('w') as log:
            process = subprocess.Popen([*common, '--headless', '--backend', 'synthetic',
                                        '--no-virtual', '--no-camera', '--width', '320',
                                        '--height', '240', '--fps', '20'], env=env,
                                       stdout=subprocess.DEVNULL, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while True:
                    assert process.poll() is None, (root / 'recovery.log').read_text()
                    try:
                        status()
                        break
                    except AssertionError:
                        assert time.monotonic() < deadline, (root / 'recovery.log').read_text()
                        time.sleep(.05)
                target = root / 'interrupted.mp4'
                recovery = Path(str(target) + '.mkv')
                command('record', 'start', target)
                assert status()['record']['path'] == str(recovery)
                deadline = time.monotonic() + 15
                while True:
                    assert process.poll() is None, (root / 'recovery.log').read_text()
                    try:
                        streams = probe_recording(recovery)
                        if any(stream['codec_type'] == 'video' and
                               int(stream.get('nb_read_frames', 0)) >= 20
                               for stream in streams):
                            break
                    except (AssertionError, subprocess.CalledProcessError):
                        pass
                    assert time.monotonic() < deadline, 'no recoverable recording data flushed'
                    time.sleep(.1)
                process.kill()
                assert process.wait(timeout=15) == -signal.SIGKILL
                assert not target.exists()
                assert recovery.stat().st_size > 0
                probe_recording(recovery)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=15)
    print('synthetic recorder smoke: IPC, pause/resume, MP4, MKV, crash recovery and shutdown passed')


if __name__ == '__main__':
    main()
