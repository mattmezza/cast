#!/usr/bin/env python3
"""Exercise recording countdown lifecycle against a daemon on private Xvfb."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def run_tool(arguments, *, timeout=5, **options):
    # GNU timeout runs the test in a separate process group. Inheriting its
    # terminal lets FFmpeg's interactive input checks suspend the whole group.
    try:
        return subprocess.run(arguments, stdin=subprocess.DEVNULL, timeout=timeout,
                              **options)
    except subprocess.TimeoutExpired as error:
        raise AssertionError(f"tool timed out after {timeout}s: {arguments}") from error


def tool_output(arguments, *, text=False):
    return run_tool(arguments, stdout=subprocess.PIPE, check=True, text=text).stdout


def stage(message):
    print(f"Countdown check: {message}", flush=True)


def wait_until(predicate, message, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.03)
    raise AssertionError(message)


def guides():
    result = run_tool(['xdotool', 'search', '--onlyvisible', '--class', '^CastPreview$'],
                      capture_output=True, text=True)
    return result.stdout.split() if result.returncode == 0 else []


def panels():
    result = run_tool(['xdotool', 'search', '--onlyvisible', '--class', '^CastPanel$'],
                      capture_output=True, text=True)
    return result.stdout.split() if result.returncode == 0 else []


def main():
    assert os.environ.get('DISPLAY'), 'run through the private Xvfb check-xorg target'
    with tempfile.TemporaryDirectory(prefix='cast-countdown-') as directory:
        temporary = Path(directory)
        socket = temporary / 'control.sock'
        configuration = temporary / 'cast.conf'
        configuration.write_text('[output]\npause_color=#203040\n'
                                 '[capture]\nmask_color=#203040\n'
                                 '[screen]\nbackground=solid\nbackground_color=#13579b\n'
                                 '[record]\ncountdown=2\nvideo_codec=ffv1\n'
                                 f'directory={temporary}\n')
        base = [os.environ.get('CAST_XORG_TEST_BINARY', str(ROOT / 'cast')), '--config', str(configuration), '--socket', str(socket)]
        environment = dict(os.environ, XDG_RUNTIME_DIR=directory)
        panel = None
        with (temporary / 'daemon.log').open('w+') as log:
            daemon = subprocess.Popen(base + ['--headless', '--backend', 'xorg', '--no-virtual', '--no-camera',
                                             '--width', '320', '--height', '180'],
                                      env=environment, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            try:
                def command(*arguments):
                    result = run_tool(base + list(arguments), env=environment,
                                            capture_output=True, text=True, timeout=5)
                    assert result.returncode == 0, result.stderr + result.stdout
                    return result.stdout

                def state():
                    return json.loads(command('status', '--json'))

                stage('attach isolated daemon and panel')
                wait_until(socket.exists, 'private daemon did not create its socket')
                if os.environ.get('CAST_COUNTDOWN_WITH_PANEL') == '1':
                    ui_state = temporary / 'panel-state.json'
                    panel_environment = dict(environment, SDL_VIDEODRIVER='x11',
                                             CAST_PANEL_UI_STATE=str(ui_state))
                    panel = subprocess.Popen(base + ['panel'], env=panel_environment,
                                             stdin=subprocess.DEVNULL, stdout=log, stderr=log)

                    def attached():
                        try:
                            snapshot = json.loads(ui_state.read_text())
                        except (FileNotFoundError, json.JSONDecodeError):
                            return False
                        return bool(panels()) and snapshot['virtual_status'] != 'Disconnected'

                    wait_until(attached, 'native panel did not attach to private daemon')
                stage('cancel start countdown with Escape')
                command('record', 'start', str(temporary / 'escape.mkv'))
                wait_until(lambda: len(guides()) == 1, 'CLI countdown guide did not map')
                window = guides()[0]
                properties = tool_output(
                    ['xprop', '-id', window, 'WM_CLASS', '_NET_WM_WINDOW_TYPE'], text=True)
                assert 'CastPreview' in properties and '_NET_WM_WINDOW_TYPE_UTILITY' in properties
                screenshot = os.environ.get('CAST_COUNTDOWN_CAPTURE')
                if screenshot:
                    geometry = dict(line.split('=', 1) for line in tool_output(
                        ['xdotool', 'getwindowgeometry', '--shell', window], text=True).splitlines())
                    run_tool(['ffmpeg', '-nostdin', '-v', 'error', '-f', 'x11grab', '-video_size',
                                    f"{geometry['WIDTH']}x{geometry['HEIGHT']}", '-i',
                                    f"{os.environ['DISPLAY']}+{geometry['X']},{geometry['Y']}",
                                    '-frames:v', '1', '-threads', '1', '-y', screenshot], check=True)
                run_tool(['xdotool', 'windowfocus', '--sync', window, 'key', 'Escape'], check=True)
                wait_until(lambda: not state()['record']['countdown'], 'Escape did not cancel')
                assert not state()['record']['state'] == 'recording'
                assert not (temporary / 'escape.mkv').exists()
                assert not guides(), 'cancelled guide stayed visible'
                if panel:
                    assert panels(), 'cancelling the preview closed the separate panel'

                stage('cancel countdown with stop and global pause')
                for cancel in [('record', 'stop'), ('pause',)]:
                    path = temporary / ('-'.join(cancel) + '.mkv')
                    command('record', 'start', str(path))
                    wait_until(lambda: bool(guides()), 'next guide did not appear')
                    command(*cancel)
                    assert not state()['record']['countdown']
                    assert not guides(), 'guide remained visible after cancellation acknowledgement'
                    assert not path.exists()

                stage('start recording after countdown')
                command('resume')
                run_tool(['xdotool', 'mousemove', '0', '0'], check=True)
                command('settings', 'record.countdown', '1')
                path = temporary / 'started.mkv'
                command('record', 'start', str(path))
                wait_until(lambda: bool(guides()), 'successful countdown guide did not appear')
                wait_until(lambda: state()['record']['state'] == 'recording', 'countdown did not start recording')
                assert not guides(), 'guide remained visible at recording start'
                if panel:
                    assert panels(), 'recording admission closed the separate panel'
                time.sleep(.2)
                stage('cut and cancel resume countdown')
                command('record', 'cut')
                before = state()['record']['duration']
                command('record', 'resume')
                wait_until(lambda: bool(guides()), 'cut resume did not reopen the film preview')
                window = guides()[0]
                run_tool(['xdotool', 'windowfocus', '--sync', window, 'key', 'Escape'], check=True)
                wait_until(lambda: not state()['record']['countdown'], 'Escape did not cancel resume')
                assert state()['record']['state'] == 'cut' and not guides()
                assert abs(state()['record']['duration'] - before) < .02
                assert state()['record']['path'] == str(path)
                # Even an already-enabled preview must close at the admission boundary.
                stage('resume same recording and hide existing preview')
                command('preview', 'on')
                command('record', 'resume')
                wait_until(lambda: bool(guides()), 'second resume did not show countdown preview')
                wait_until(lambda: state()['record']['state'] == 'recording', 'cut resume never admitted media')
                assert not guides(), 'countdown preview remained on screen when recording resumed'
                time.sleep(.2)
                command('record', 'stop')
                wait_until(lambda: not state()['record']['finalizing'], 'recording did not finalize')
                stage('decode completed recording and inspect first frame')
                frames = tool_output(
                    ['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames',
                     '-show_entries', 'stream=nb_read_frames', '-of', 'default=nw=1:nk=1', str(path)],
                    text=True)
                assert int(frames.strip()) > 0, 'countdown start produced no recorded frames'
                first = tool_output(
                    ['ffmpeg', '-nostdin', '-v', 'error', '-i', str(path), '-frames:v', '1',
                     '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'])
                assert len(first) == 320 * 180 * 3
                # The centered guide covered this region. A fresh recording
                # contains the black desktop or the separate panel's neutral mask;
                # it must never contain the guide's number, ring or background.
                for y in range(70, 110):
                    for x in range(140, 180):
                        pixel = first[(y * 320 + x) * 3:(y * 320 + x + 1) * 3]
                        assert all(value <= 3 for value in pixel) or all(
                            abs(value - neutral) <= 3
                            for value, neutral in zip(pixel, (32, 48, 64))), \
                            'countdown guide leaked into the first recording frame'
                # Decode real recording frames with a mapped native preview. Exclusion
                # pixels survive media conversion without carrying UI chrome or recursion.
                stage('record and decode preview exclusion mask')
                command('settings', 'record.countdown', '0')
                command('layout', 'screen')
                command('zoom', 'reset')
                command('capture', 'mask-color', '#4b697d')
                expected = (75, 105, 125)
                video = temporary / 'exclusion-mask.mkv'
                command('record', 'start', str(video))
                command('record', 'resume')
                command('preview', 'on')
                wait_until(lambda: bool(guides()), 'native preview did not map')
                window = guides()[0]
                run_tool(['xdotool', 'windowsize', window, '640', '400',
                                'windowmove', window, '80', '100'], check=True)
                time.sleep(.5)
                command('record', 'stop')
                wait_until(lambda: not state()['record']['finalizing'],
                           'exclusion recording did not finalize')
                decoded = tool_output(
                    ['ffmpeg', '-nostdin', '-v', 'error', '-i', str(video), '-f', 'rawvideo',
                     '-pix_fmt', 'rgb24', 'pipe:1'])
                frame_bytes = 320 * 180 * 3
                assert len(decoded) >= 3 * frame_bytes and len(decoded) % frame_bytes == 0
                # Admission hides an existing preview; check settled frames after reopening it.
                for start in range(len(decoded) - 3 * frame_bytes, len(decoded), frame_bytes):
                    offset = start + (90 * 320 + 160) * 3
                    actual = decoded[offset:offset + 3]
                    assert all(abs(a - b) <= 3 for a, b in zip(actual, expected)), \
                        (tuple(actual), expected, state(),
                         tool_output(['xdotool', 'getwindowgeometry', '--shell', window], text=True))
                acknowledged = state()['source']
                assert 'exclusion' not in acknowledged
                assert acknowledged['mask_color'] == '#4b697d'
                command('preview', 'off')
                stage('shut down isolated daemon')
                command('quit')
                assert daemon.wait(timeout=5) == 0
                print('Xorg temporary film preview, start/cut-resume countdown, cancellation and recording exclusion passed')
            except BaseException:
                log.seek(0)
                print(log.read(), flush=True)
                raise
            finally:
                if panel and panel.poll() is None:
                    panel.terminate()
                    try:
                        panel.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        panel.kill()
                        panel.wait()
                if daemon.poll() is None:
                    daemon.terminate()
                    try:
                        daemon.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        daemon.kill()
                        daemon.wait()


if __name__ == '__main__':
    main()
