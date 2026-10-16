#!/usr/bin/env python3
"""Portable synthetic recorder smoke test; also runs on Mac without TCC grants."""
import argparse
import json
import os
from pathlib import Path
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
                target = root / 'smoke.mp4'
                command('record', 'start', target)
                time.sleep(.6)
                recording = status()
                assert recording['record']['path'] == str(target) + '.mkv'
                command('record', 'pause')
                command('record', 'resume')
                time.sleep(.3)
                command('record', 'stop')
                deadline = time.monotonic() + 15
                while status()['record']['finalizing']:
                    assert time.monotonic() < deadline
                    time.sleep(.05)
                assert target.stat().st_size > 0
                assert not Path(str(target) + '.mkv').exists()
                probe = subprocess.run(['ffprobe', '-v', 'error', '-show_streams',
                                        '-of', 'json', str(target)], capture_output=True,
                                       text=True, check=True, timeout=15)
                streams = json.loads(probe.stdout)['streams']
                assert any(stream['codec_type'] == 'video' for stream in streams)
                assert any(stream['codec_type'] == 'audio' for stream in streams)
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
    print('synthetic macOS recorder smoke: IPC, pause/resume, safe MP4 and shutdown passed')


if __name__ == '__main__':
    main()
