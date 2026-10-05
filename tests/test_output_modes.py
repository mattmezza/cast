#!/usr/bin/env python3
"""Decode actual output modes and timeline cuts from an isolated synthetic daemon."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get('CAST_TEST_BINARY', ROOT / 'cast')).resolve()
WIDTH, HEIGHT, FPS = 160, 96, 20


def wait_until(predicate, message, timeout=6):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError(message)


def main():
    with tempfile.TemporaryDirectory(prefix='cast-output-modes-') as directory:
        temporary = Path(directory)
        socket = temporary / 'cast.sock'
        configuration = temporary / 'cast.conf'
        configuration.write_text(
            '[output]\nbackend=synthetic\nenabled=false\nwidth=160\nheight=96\nfps=20\n'
            'pause_title=\npause_subtitle=\npause_color=#123456\n'
            'blur_title=\nblur_subtitle=\nblur_opacity=0\nblur_radius=24\n'
            '[camera]\nenabled=false\n[composition]\nlayout=screen\n'
            '[cursor]\nenabled=false\n[record]\nvideo_codec=ffv1\ncountdown=0\n'
            f'directory={temporary}\n')
        base = [str(BINARY), '--socket', str(socket)]
        env = dict(os.environ, XDG_RUNTIME_DIR=directory)
        with (temporary / 'daemon.log').open('w+') as log:
            daemon = subprocess.Popen([str(BINARY), '--headless', '--config', str(configuration),
                                       '--socket', str(socket)], env=env, stdout=log, stderr=log)
            try:
                def command(*arguments, success=True):
                    result = subprocess.run(base + list(arguments), env=env, capture_output=True,
                                            text=True, timeout=5)
                    assert (result.returncode == 0) == success, (arguments, result.stdout, result.stderr)
                    return result.stdout

                def state():
                    return json.loads(command('status', '--json'))

                wait_until(socket.exists, 'synthetic daemon did not start')
                for anchor in ('top', 'bottom', 'left', 'right'):
                    command('camera', 'anchor', anchor)
                command('settings', 'output.pause_font', 'Noto Sans', success=False)
                command('settings', 'output.pause_title', '{unknown}', success=False)
                path = temporary / 'modes.mkv'
                command('record', 'start', str(path))
                wall_start = time.monotonic()
                time.sleep(.4)
                command('record', 'freeze')
                time.sleep(.4)
                command('record', 'blur', 'on')
                time.sleep(.4)
                command('record', 'pause')
                duration = state()['record']['duration']
                time.sleep(.45)
                assert state()['record']['duration'] > duration + .25, 'solid pause stopped file time'
                command('record', 'cut')
                cut_start = time.monotonic()
                duration = state()['record']['duration']
                time.sleep(.4)
                assert abs(state()['record']['duration'] - duration) < .02, 'cut still advanced file time'
                command('settings', 'record.countdown', '1')
                command('record', 'resume')
                assert state()['record']['countdown'], 'cut resume omitted configured countdown'
                time.sleep(.15)
                command('record', 'cut')
                assert not state()['record']['countdown'], 'cut did not cancel pending resume'
                time.sleep(.15)
                assert abs(state()['record']['duration'] - duration) < .02
                command('record', 'resume')
                wait_until(lambda: not state()['record']['countdown'], 'resume countdown never finished')
                cut_end = time.monotonic()
                resumed = state()['record']
                assert resumed['path'] == str(path), 'resume selected another file'
                # A resume from cut preserves the solid pause; it never reveals frozen/blurred content.
                assert resumed['state'] == 'paused', resumed
                time.sleep(.3)
                command('record', 'resume')
                command('record', 'unfreeze')
                time.sleep(.3)
                command('record', 'blur', 'off')
                time.sleep(.3)
                duration = state()['record']['duration']
                wall_duration = time.monotonic() - wall_start
                assert abs(duration - (wall_duration - (cut_end - cut_start))) < .25
                command('record', 'stop')
                wait_until(lambda: not state()['record']['finalizing'], 'recording did not finalize')
                assert list(temporary.glob('*.mkv')) == [path]
                raw = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(path),
                                               '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'])
                frame_size = WIDTH * HEIGHT * 3
                assert len(raw) % frame_size == 0
                frames = [raw[i:i + frame_size] for i in range(0, len(raw), frame_size)]
                assert len(frames) >= 35, f'too few output frames: {len(frames)}'
                solid = bytes((0x12, 0x34, 0x56)) * (WIDTH * HEIGHT)
                def is_solid(frame):
                    return all(abs(value - expected) <= 3 for value, expected in zip(frame, solid))

                solid_count = sum(is_solid(frame) for frame in frames)
                assert solid_count >= 10, f'solid pause frames missing: {solid_count}'
                # Original synthetic blue is 30/100 in 32px squares. Blur must mix those levels.
                mixed = sum(1 for frame in frames if not is_solid(frame) and
                            any(42 <= frame[(y * WIDTH + x) * 3 + 2] <= 88
                                for x, y in ((31, 31), (63, 63), (95, 31))))
                assert mixed >= 5, 'blur was not applied to recorded composition'
                run = longest = 1
                for a, b in zip(frames, frames[1:]):
                    run = run + 1 if a == b and not is_solid(a) else 1
                    longest = max(longest, run)
                assert longest >= 5, 'freeze did not retain the entire composition'
                packets = json.loads(subprocess.check_output(
                    ['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_packets',
                     '-show_entries', 'packet=pts_time', '-of', 'json', str(path)], text=True))
                timestamps = [float(packet['pts_time']) for packet in packets['packets']]
                assert all(a < b for a, b in zip(timestamps, timestamps[1:]))
                assert max(b - a for a, b in zip(timestamps, timestamps[1:])) < .2, \
                    'cut/countdown leaked a gap into media timestamps'
                command('quit')
                assert daemon.wait(timeout=5) == 0
                print('decoded solid pause, frozen+blurred composition, same-file cut/resume countdown and gapless PTS passed')
            except BaseException:
                log.seek(0)
                print(log.read())
                raise
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                    try:
                        daemon.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        daemon.kill()
                        daemon.wait()


if __name__ == '__main__':
    main()
