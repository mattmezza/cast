#!/usr/bin/env python3
"""Record through native controls using only private Xvfb and isolated audio IPC."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from test_composition import ROOT, UI, close_window, widget_id
from test_look import wait_for


def inside(binary):
    with tempfile.TemporaryDirectory(prefix='recording-', dir=UI / 'build/tmp') as directory:
        temporary = Path(directory)
        media_directory = temporary / 'takes'
        media_directory.mkdir()
        config = temporary / 'cast.conf'
        config.write_text('[output]\nbackend=xorg\nwidth=640\nheight=480\nfps=15\n'
            '[camera]\nenabled=false\n[composition]\nlayout=stage\nfit=contain\n'
            '[screen]\nwidth_percent=70\nmargin=48\nbackground=solid\nbackground_color=#247ac4\n'
            '[audio]\nmic=false\ndesktop=false\nvirtual=false\n'
            f'[record]\ndirectory={directory}\ncountdown=0\ncontainer=matroska\n'
            '[zoom]\nfollow=false\n')
        environment = dict(os.environ, XDG_RUNTIME_DIR=directory,
            XDG_CONFIG_HOME=str(temporary / 'config'), XDG_CACHE_HOME=str(temporary / 'cache'),
            XDG_DATA_HOME=str(temporary / 'data'), PIPEWIRE_REMOTE='cast-test-no-audio-server',
            PULSE_SERVER='unix:' + str(temporary / 'no-pulse-server'),
            CAST_TEST_FONT_DIR=str(UI / 'assets/fonts'))
        base = [str(binary), '--config', str(config), '--output-device', 'none', '--no-camera']

        def command(*args):
            result = subprocess.run([*base, *args], env=environment, capture_output=True,
                                    text=True, timeout=10)
            assert result.returncode == 0, (args, result.stderr, result.stdout)
            return result.stdout

        def status():
            state = json.loads(command('status', '--json'))
            assert state['virtual']['state'] in ('paused', 'stopped'), state
            assert not state['stream']['active'], state
            return state

        def xdo(*args):
            subprocess.run(['xdotool', *map(str, args)], env=environment,
                           capture_output=True, check=True, timeout=5)

        layouts = {}
        def load_layouts(path=None):
            geometry_environment = dict(environment)
            if path:
                geometry_environment['CAST_TEST_RECORD_PATH'] = str(path)
            for mode in ('idle', 'recording', 'paused', 'cut', 'countdown', 'settings', 'settings-container'):
                result = subprocess.run([str(UI / 'build/test_native_layout'), '--record-layout', mode],
                    env=geometry_environment, capture_output=True, text=True, timeout=15)
                assert result.returncode == 0, result.stderr
                layouts[mode] = {box['id']: box for box in json.loads(result.stdout)['boxes']}
        load_layouts()

        def bounds(name, mode):
            value = layouts[mode].get(widget_id(name))
            assert value and value['found'], ('Missing fixture geometry', name, mode)
            return value['bounds']

        workspace = None
        with (temporary / 'daemon.log').open('w+') as daemon_log:
            daemon = subprocess.Popen([*base, '--headless'], env=environment,
                                      stdout=daemon_log, stderr=daemon_log)
            try:
                wait_for(lambda: (daemon.poll(), list(temporary.glob('*.sock'))),
                    lambda value: value[0] is None and bool(value[1]), 'Daemon startup', 8)
                assert status()['record']['state'] == 'stopped'

                def session(actions=None, expected_error=False):
                    nonlocal workspace
                    workspace = subprocess.Popen([*base, 'workspace', '--size', '1360x1000',
                        '--workspace', 'record', '--debug-live'], env=environment,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

                    def find_window():
                        assert workspace.poll() is None, 'Workspace exited before creating window'
                        result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--all', '--pid',
                            str(workspace.pid), '--class', 'Cast(Pro)?Panel'], env=environment,
                            capture_output=True, text=True, timeout=2)
                        return result.stdout.strip().splitlines()[-1] if result.returncode == 0 and result.stdout.strip() else None

                    window = wait_for(find_window, bool, 'Workspace window')
                    xdo('windowfocus', '--sync', window)
                    time.sleep(.25)

                    def click(name, mode='idle', fraction=.5):
                        x, y, width, height = bounds(name, mode)
                        assert width > 0 and height > 0 and 0 <= y and y + height <= 1000, (name, y, height)
                        xdo('mousemove', '--window', window, round(x + width * fraction), round(y + height / 2))
                        time.sleep(.08)
                        xdo('mousedown', '1')
                        time.sleep(.05)
                        xdo('mouseup', '1')
                        assert workspace.poll() is None, ('Workspace exited during click', name)

                    if actions:
                        actions(click, window)
                    status()
                    time.sleep(.15)
                    close_window(window)
                    output, errors = workspace.communicate(timeout=10)
                    assert workspace.returncode == 0, errors
                    state = json.loads(output.strip().splitlines()[-1])
                    assert state['connected'] and state['has_frame'], state
                    assert not state['pending'], state
                    if not expected_error:
                        assert not state['edit_error'], state
                    assert state['virtual_paused'] and not state['stream_active'], state
                    workspace = None
                    return state

                def wait_record(expected):
                    return wait_for(status, lambda value: value['record']['state'] == expected,
                                    'Native recording ' + expected, 10)

                def hold_record(expected, seconds):
                    deadline = time.monotonic() + seconds
                    def read():
                        value = status()
                        assert value['record']['state'] == expected, value
                        return time.monotonic()
                    wait_for(read, lambda now: now >= deadline, 'Recording interval', seconds + 3)

                def capture(window, name):
                    if shutil.which('import'):
                        subprocess.run(['import', '-window', str(window), str(UI / 'build' / name)],
                            env=environment, capture_output=True, check=True, timeout=5)

                def settings(click, window):
                    click('settings-open')
                    time.sleep(.1)
                    click('settings-category-2', 'settings')
                    time.sleep(.1)
                    click('record-container', 'settings')
                    time.sleep(.1)
                    capture(window, 'recording-container-review.png')
                    option = str(int(widget_id('record-container')) ^ 0x6f7074696f6e)
                    row = layouts['settings-container'][option]['bounds']
                    xdo('mousemove', '--window', window, round(row[0] + row[2] / 2), round(row[1] + row[3] / 2))
                    time.sleep(.08)
                    xdo('mousedown', '1')
                    time.sleep(.05)
                    xdo('mouseup', '1')
                    time.sleep(.2)
                    click('record-video-codec', 'settings')
                    time.sleep(.1)
                    for key in ('Home', 'Down', 'Return'):
                        xdo('key', '--clearmodifiers', key)
                        time.sleep(.08)
                    time.sleep(.2)
                    click('record-crf', 'settings', fraction=.65)
                    time.sleep(.2)
                    click('record-directory', 'settings')
                    xdo('key', '--clearmodifiers', 'ctrl+a')
                    xdo('type', '--clearmodifiers', '--delay', '5', str(media_directory))
                    xdo('key', '--clearmodifiers', 'Return')
                    time.sleep(.25)
                    capture(window, 'recording-settings-review.png')
                    xdo('key', '--clearmodifiers', 'Escape')

                configured = session(settings)
                assert configured['record_container'] == 'mp4' and configured['record_codec'] == 'libx264', configured
                assert configured['record_crf'] != 23, configured
                assert configured['record_countdown'] == 0, configured
                assert configured['record_directory'] == str(media_directory), configured

                def unavailable_mic(click, window):
                    click('audio-mic-enabled')
                    # The private runtime has no audio server; the backend must
                    # reject the request and retain the muted configuration.
                    time.sleep(.35)
                    audio = status()['audio']
                    assert not audio['mic']['enabled'] and not audio['readiness']['mic_ready'], audio

                rejected = session(unavailable_mic, expected_error=True)
                assert rejected['edit_error'] and 'rejected' in rejected['edit_error'].lower(), rejected
                assert not rejected['mic'] and not rejected['recording'], rejected

                command('settings', 'record.countdown', '2')
                files_before = set(media_directory.glob('cast-*'))

                def cancel_countdown(click, window):
                    click('output-start')
                    wait_for(status, lambda value: value['record']['countdown'], 'Native recording countdown')
                    click('record-cancel', 'countdown')
                    wait_for(status, lambda value: not value['record']['countdown'] and
                        value['record']['state'] == 'stopped', 'Native countdown cancellation')

                cancelled = session(cancel_countdown)
                assert not cancelled['record_countdown_active'] and not cancelled['recording'], cancelled
                assert set(media_directory.glob('cast-*')) == files_before, 'Cancelled countdown created media'
                command('settings', 'record.countdown', '0')

                def record(click, window):
                    nonlocal recording_path, elapsed
                    # Muted gain changes are safe even when no PipeWire server exists.
                    click('audio-mic-gain', fraction=.2)
                    wait_for(status, lambda value: abs(value['audio']['mic']['gain'] - 1) > .05,
                             'Microphone gain acknowledgement')
                    click('audio-desktop-gain', fraction=.3)
                    wait_for(status, lambda value: abs(value['audio']['desktop']['gain'] - 1) > .05,
                             'Desktop gain acknowledgement')
                    click('output-start')
                    started = time.monotonic()
                    first = wait_record('recording')
                    recording_path = Path(first['record']['path'])
                    assert recording_path.parent == media_directory, first
                    load_layouts(recording_path)
                    hold_record('recording', .35)
                    click('record-pause', 'recording')
                    wait_record('paused')
                    hold_record('paused', .3)
                    click('record-resume', 'paused')
                    wait_record('recording')
                    click('record-cut', 'recording')
                    wait_record('cut')
                    hold_record('cut', 1)
                    click('record-resume', 'cut')
                    resumed = wait_record('recording')
                    assert resumed['record']['path'] == str(recording_path), resumed
                    hold_record('recording', .35)
                    capture(window, 'recording-review.png')
                    click('output-start', 'recording')
                    wait_record('stopped')
                    finished = wait_for(status, lambda value: not value['record']['finalizing'], 'Recording finalization', 10)
                    # MP4 captures into a recovery MKV, then publishes the final MP4.
                    recording_path = Path(finished['record']['path'])
                    assert recording_path.parent == media_directory and recording_path.suffix == '.mp4', finished
                    elapsed = time.monotonic() - started

                recording_path = None
                elapsed = None
                final = session(record)
                assert not final['recording'], final
                audio = status()['audio']
                assert not audio['mic']['enabled'] and not audio['desktop']['enabled'], audio
                assert not audio['virtual']['enabled'], audio
                assert recording_path and recording_path.is_file() and recording_path.stat().st_size > 0, (str(recording_path), list(temporary.iterdir()), final)
                probed = subprocess.run(['ffprobe', '-v', 'error', '-show_streams', '-show_format',
                    '-of', 'json', str(recording_path)], capture_output=True, text=True, timeout=10)
                assert probed.returncode == 0, probed.stderr
                media = json.loads(probed.stdout)
                video = [stream for stream in media['streams'] if stream['codec_type'] == 'video']
                assert len(video) == 1 and (video[0]['width'], video[0]['height']) == (640, 480), media
                # Muting retains the recorder's audio stream, containing silence.
                audio_streams = [stream for stream in media['streams'] if stream['codec_type'] == 'audio']
                assert audio_streams, media
                pcm = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(recording_path),
                    '-map', '0:a:0', '-f', 's16le', '-acodec', 'pcm_s16le', '-'],
                    capture_output=True, timeout=10)
                assert pcm.returncode == 0 and pcm.stdout and len(pcm.stdout) % 2 == 0, pcm.stderr
                peak = max(abs(value[0]) for value in struct.iter_unpack('<h', pcm.stdout))
                assert peak <= 8, ('Muted audio must decode to silence', peak)
                duration = float(media['format']['duration'])
                assert .6 < duration < elapsed - .4, (duration, elapsed, 'Cut interval must be omitted')
                decoded = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(recording_path),
                    '-vf', 'crop=2:2:0:0', '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                    capture_output=True, timeout=10)
                assert decoded.returncode == 0 and len(decoded.stdout) == 12, decoded.stderr
                assert all(abs(actual - expected) <= 12 for actual, expected in zip(decoded.stdout[:3],
                    (0x24, 0x7a, 0xc4))), decoded.stdout
                reopened = session()
                assert not reopened['recording'] and not reopened['record_finalizing'], reopened
            except Exception:
                failed = UI / 'build/tmp/recording-failure'
                failed.mkdir(exist_ok=True)
                (failed / 'fixture-layout.json').write_text(json.dumps(layouts, indent=2))
                if shutil.which('import'):
                    subprocess.run(['import', '-window', 'root', str(failed / 'screen.png')],
                        env=environment, capture_output=True, timeout=5)
                if workspace is not None:
                    if workspace.poll() is None:
                        workspace.terminate()
                    output, errors = workspace.communicate(timeout=5)
                    (failed / 'workspace.log').write_text(output + '\n' + errors)
                    workspace = None
                daemon_log.flush()
                shutil.copyfile(temporary / 'daemon.log', failed / 'daemon.log')
                print(f'Failure diagnostics: {failed}', file=sys.stderr)
                raise
            finally:
                if workspace is not None and workspace.poll() is None:
                    workspace.terminate()
                    workspace.communicate(timeout=5)
                if daemon.poll() is None:
                    daemon.terminate()
                    daemon.wait(timeout=10)
        shutil.rmtree(UI / 'build/tmp/recording-failure', ignore_errors=True)
        print('Recording/audio: native settings/destination, countdown cancellation, '
              'start/pause/cut/resume/stop, gain/rejection acknowledgements, '
              'encoded cut output, silent audio and reopen passed')


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--inside-private-xvfb':
        inside(Path(sys.argv[2]).resolve())
        return 0
    assert len(sys.argv) <= 2, 'Usage: test_recording.py [cast-binary]'
    for dependency in ('xvfb-run', 'xauth', 'xdotool', 'ffmpeg', 'ffprobe'):
        assert shutil.which(dependency), f'Recording integration requires {dependency}'
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'cast').resolve()
    assert binary.is_file() and (UI / 'build/test_native_layout').is_file(), 'Build cast and ui test_native_layout first'
    temporary = UI / 'build/tmp'
    temporary.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, TMPDIR=str(temporary), SDL_RENDER_DRIVER='software', SDL_VIDEODRIVER='x11')
    return subprocess.run(['xvfb-run', '-a', '-n', '171', '-s', '-screen 0 1600x1200x24', sys.executable,
        str(Path(__file__).resolve()), '--inside-private-xvfb', str(binary)], env=environment, timeout=150).returncode


if __name__ == '__main__':
    raise SystemExit(main())
