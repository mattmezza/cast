#!/usr/bin/env python3
"""Exercise recording countdown lifecycle against a daemon on private Xvfb."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def wait_until(predicate, message, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.03)
    raise AssertionError(message)


def guides():
    result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--class', '^CastPreview$'],
                            capture_output=True, text=True)
    return result.stdout.split() if result.returncode == 0 else []


def main():
    assert os.environ.get('DISPLAY'), 'run through the private Xvfb check-xorg target'
    with tempfile.TemporaryDirectory(prefix='cast-countdown-') as directory:
        temporary = Path(directory)
        socket = temporary / 'control.sock'
        configuration = temporary / 'cast.conf'
        configuration.write_text('[output]\npause_color=#203040\n'
                                 '[record]\ncountdown=2\nvideo_codec=ffv1\n'
                                 f'directory={temporary}\n')
        base = [str(ROOT / 'cast'), '--config', str(configuration), '--socket', str(socket)]
        environment = dict(os.environ, XDG_RUNTIME_DIR=directory)
        with (temporary / 'daemon.log').open('w+') as log:
            daemon = subprocess.Popen(base + ['--backend', 'xorg', '--no-live', '--no-camera',
                                             '--width', '320', '--height', '180'],
                                      env=environment, stdout=log, stderr=log)
            try:
                def command(*arguments):
                    result = subprocess.run(base + list(arguments), env=environment,
                                            capture_output=True, text=True, timeout=5)
                    assert result.returncode == 0, result.stderr + result.stdout
                    return result.stdout

                def state():
                    return json.loads(command('status', '--json'))

                wait_until(socket.exists, 'private daemon did not create its socket')
                command('record', 'start', str(temporary / 'escape.mkv'))
                wait_until(lambda: len(guides()) == 1, 'CLI countdown guide did not map')
                window = guides()[0]
                properties = subprocess.check_output(
                    ['xprop', '-id', window, 'WM_CLASS', '_NET_WM_WINDOW_TYPE'], text=True)
                assert 'CastPreview' in properties and '_NET_WM_WINDOW_TYPE_UTILITY' in properties
                screenshot = os.environ.get('CAST_COUNTDOWN_CAPTURE')
                if screenshot:
                    geometry = dict(line.split('=', 1) for line in subprocess.check_output(
                        ['xdotool', 'getwindowgeometry', '--shell', window], text=True).splitlines())
                    subprocess.run(['ffmpeg', '-v', 'error', '-f', 'x11grab', '-video_size',
                                    f"{geometry['WIDTH']}x{geometry['HEIGHT']}", '-i',
                                    f"{os.environ['DISPLAY']}+{geometry['X']},{geometry['Y']}",
                                    '-frames:v', '1', '-threads', '1', '-y', screenshot], check=True)
                subprocess.run(['xdotool', 'windowfocus', '--sync', window, 'key', 'Escape'], check=True)
                wait_until(lambda: not state()['record']['countdown'], 'Escape did not cancel')
                assert not state()['record']['state'] == 'recording'
                assert not (temporary / 'escape.mkv').exists()
                assert not guides(), 'cancelled guide stayed visible'

                for cancel in [('record', 'stop'), ('pause',)]:
                    path = temporary / ('-'.join(cancel) + '.mkv')
                    command('record', 'start', str(path))
                    wait_until(lambda: bool(guides()), 'next guide did not appear')
                    command(*cancel)
                    assert not state()['record']['countdown']
                    assert not guides(), 'guide remained visible after cancellation acknowledgement'
                    assert not path.exists()

                command('resume')
                subprocess.run(['xdotool', 'mousemove', '0', '0'], check=True)
                command('settings', 'record.countdown', '1')
                path = temporary / 'started.mkv'
                command('record', 'start', str(path))
                wait_until(lambda: bool(guides()), 'successful countdown guide did not appear')
                wait_until(lambda: state()['record']['state'] == 'recording', 'countdown did not start recording')
                assert not guides(), 'guide remained visible at recording start'
                time.sleep(.2)
                command('record', 'cut')
                before = state()['record']['duration']
                command('record', 'resume')
                wait_until(lambda: bool(guides()), 'cut resume did not reopen the film preview')
                window = guides()[0]
                subprocess.run(['xdotool', 'windowfocus', '--sync', window, 'key', 'Escape'], check=True)
                wait_until(lambda: not state()['record']['countdown'], 'Escape did not cancel resume')
                assert state()['record']['state'] == 'cut' and not guides()
                assert abs(state()['record']['duration'] - before) < .02
                assert state()['record']['path'] == str(path)
                # Even an already-enabled preview must close at the admission boundary.
                command('preview', 'on')
                command('record', 'resume')
                wait_until(lambda: bool(guides()), 'second resume did not show countdown preview')
                wait_until(lambda: state()['record']['state'] == 'recording', 'cut resume never admitted media')
                assert not guides(), 'countdown preview remained on screen when recording resumed'
                time.sleep(.2)
                command('record', 'stop')
                wait_until(lambda: not state()['record']['finalizing'], 'recording did not finalize')
                frames = subprocess.check_output(
                    ['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames',
                     '-show_entries', 'stream=nb_read_frames', '-of', 'default=nw=1:nk=1', str(path)],
                    text=True)
                assert int(frames.strip()) > 0, 'countdown start produced no recorded frames'
                first = subprocess.check_output(
                    ['ffmpeg', '-v', 'error', '-i', str(path), '-frames:v', '1',
                     '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'])
                assert len(first) == 320 * 180 * 3
                # The centered guide covered this region. The first recording
                # may use the retained neutral footprint while WM chrome retires;
                # it must never contain the guide's number, ring or background.
                for y in range(70, 110):
                    for x in range(140, 180):
                        pixel = first[(y * 320 + x) * 3:(y * 320 + x + 1) * 3]
                        assert all(value <= 3 for value in pixel) or all(
                            abs(value - neutral) <= 3
                            for value, neutral in zip(pixel, (32, 48, 64))), \
                            'countdown guide leaked into the first recording frame'
                command('quit')
                assert daemon.wait(timeout=5) == 0
                print('Xorg temporary film preview, start/cut-resume countdown, cancellation and recording exclusion passed')
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
