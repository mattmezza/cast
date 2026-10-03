#!/usr/bin/env python3
"""Opt-in kernel/V4L2 consumer test; all transmitted content is synthetic.

Requires an unused, writable v4l2loopback device and FFmpeg tools. No desktop,
physical camera or microphone is captured. Temporary recordings are removed.
"""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True, help='unused v4l2loopback output path')
    options = parser.parse_args()
    binary = str(Path(__file__).resolve().parent.parent / 'cast')

    with tempfile.TemporaryDirectory(prefix='cast-loopback-') as directory:
        root = Path(directory)
        config = root / 'cast.conf'
        config.write_text('[audio]\nmic=false\ndesktop=false\nvirtual=false\n')
        socket = root / 'cast.sock'
        common = [binary, '--config', str(config), '--socket', str(socket)]
        with (root / 'daemon.log').open('w') as log:
            daemon = subprocess.Popen(
                common + ['--backend', 'synthetic', '--camera-device', 'synthetic',
                          '--output-device', options.device, '--width', '1920',
                          '--height', '1080', '--fps', '30'],
                stdout=subprocess.DEVNULL, stderr=log)

            def command(*arguments):
                result = subprocess.run(common + list(arguments), text=True,
                                        capture_output=True, timeout=10)
                assert result.returncode == 0, (arguments, result.stdout, result.stderr)
                return result.stdout

            def status():
                return json.loads(command('status', '--json'))

            def consumer_frame():
                # Allow a fresh cadence tick before opening a new kernel consumer.
                # This does not measure already-buffered conference frames or latency.
                time.sleep(0.12)
                result = subprocess.run(
                    ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'video4linux2',
                     '-i', options.device, '-frames:v', '1', '-vf', 'scale=64:36',
                     '-pix_fmt', 'rgb24', '-f', 'rawvideo', 'pipe:1'],
                    capture_output=True, timeout=10)
                assert result.returncode == 0, result.stderr.decode(errors='replace')
                assert len(result.stdout) == 64 * 36 * 3
                return result.stdout

            try:
                deadline = time.monotonic() + 10
                while not socket.exists():
                    assert daemon.poll() is None, (root / 'daemon.log').read_text()
                    assert time.monotonic() < deadline, 'daemon startup timed out'
                    time.sleep(0.05)

                assert status()['virtual']['state'] == 'paused'
                neutral = consumer_frame()
                command('virtual', 'resume')
                resumed = consumer_frame()
                assert resumed != neutral
                # The synthetic screen red channel increases from left to right.
                # Check actual device pixels, independently of a call's self-view.
                def screen_orientation(frame):
                    left = frame[(18 * 64 + 4) * 3]
                    right = frame[(18 * 64 + 59) * 3]
                    assert right > left + 100, 'screen layer is mirrored in device output'

                screen_orientation(resumed)
                command('camera', 'mirror', 'on')
                screen_orientation(consumer_frame())
                command('camera', 'mirror', 'off')
                command('virtual', 'freeze')
                frozen = consumer_frame()
                assert consumer_frame() == frozen
                command('virtual', 'pause')
                assert consumer_frame() == neutral
                command('virtual', 'unfreeze')
                assert status()['virtual']['state'] == 'paused'
                assert consumer_frame() == neutral

                command('virtual', 'resume')
                recording = root / 'loopback.mkv'
                command('record', 'start', str(recording))
                time.sleep(0.8)
                command('record', 'pause')
                before = status()['record']['duration']
                time.sleep(0.7)
                assert abs(status()['record']['duration'] - before) < 0.03
                command('record', 'resume')
                time.sleep(0.8)
                command('pause')
                assert consumer_frame() == neutral
                assert status()['record']['path'] == str(recording)
                command('resume')
                time.sleep(0.3)
                command('record', 'stop')
                deadline = time.monotonic() + 10
                while status()['record']['finalizing']:
                    assert time.monotonic() < deadline, 'recording finalization timed out'
                    time.sleep(0.05)
                final = status()
                assert not final['last_error'], final

                result = subprocess.run(
                    ['ffprobe', '-v', 'error', '-count_frames', '-show_streams',
                     '-show_format', '-of', 'json', str(recording)],
                    capture_output=True, text=True, check=True, timeout=10)
                recorded = json.loads(result.stdout)
                video = next(stream for stream in recorded['streams']
                             if stream['codec_type'] == 'video')
                assert video['width'] == 1920 and video['height'] == 1080
                assert int(video['nb_read_frames']) > 20

                command('quit')
                daemon.wait(timeout=10)
                assert daemon.returncode == 0
                print(f'loopback consumer: orientation/neutral/resume/freeze/privacy/same-file recording '
                      f'passed at 1920x1080@30 on {options.device}; '
                      f'{video["nb_read_frames"]} decoded recording frames')
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                    daemon.wait(timeout=10)


if __name__ == '__main__':
    main()
