#!/usr/bin/env python3
"""Native Background/Zoom edits against an isolated daemon and private Xvfb."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from test_composition import ROOT, UI, close_window, widget_id


def wait_for(read, predicate, description, timeout=5):
    deadline = time.monotonic() + timeout
    value = None
    while time.monotonic() < deadline:
        value = read()
        if predicate(value):
            return value
        time.sleep(.04)
    raise AssertionError((description, value))


def inside(binary, fixture):
    with tempfile.TemporaryDirectory(prefix='look-', dir=UI / 'build/tmp') as directory:
        temporary = Path(directory)
        config = temporary / 'cast.conf'
        config.write_text('[output]\nbackend=xorg\nwidth=640\nheight=480\nfps=15\n'
                          '[camera]\nenabled=false\n[composition]\nlayout=stage\nfit=contain\n'
                          '[screen]\nwidth_percent=70\nmargin=48\n'
                          '[background]\ngradient_via_enabled=false\n'
                          '[audio]\nmic=false\ndesktop=false\n[zoom]\nfollow=false\n')
        environment = dict(os.environ, XDG_RUNTIME_DIR=directory,
                           XDG_CONFIG_HOME=str(temporary / 'config'),
                           XDG_CACHE_HOME=str(temporary / 'cache'),
                           XDG_DATA_HOME=str(temporary / 'data'))
        base = [str(binary), '--config', str(config), '--output-device', 'none', '--no-camera']

        def command(*args):
            result = subprocess.run([*base, *args], env=environment, capture_output=True,
                                    text=True, timeout=10)
            assert result.returncode == 0, (args, result.stderr, result.stdout)
            return result.stdout

        def status():
            return json.loads(command('status', '--json'))

        def idle():
            state = status()
            assert state['virtual']['state'] in ('paused', 'stopped'), state
            assert state['record']['state'] == 'stopped', state
            assert not state['stream']['active'], state
            return state

        def xdo(*args):
            subprocess.run(['xdotool', *map(str, args)], env=environment,
                           capture_output=True, check=True, timeout=5)

        layouts = {}
        for mode in ('default', 'camera', 'blurred', 'gradient', 'solid', 'zoom'):
            result = subprocess.run([str(UI / 'build/test_preview'), '--look-layout', mode],
                                    env=environment, capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, result.stderr
            layouts[mode] = {box['id']: box for box in json.loads(result.stdout)['boxes']}

        def bounds(name, mode='blurred'):
            value = layouts[mode].get(widget_id(name))
            assert value and value['found'], ('Missing fixture geometry', name, mode)
            return value['bounds']

        workspace = None
        with (temporary / 'daemon.log').open('w+') as daemon_log:
            daemon = subprocess.Popen([*base, '--headless'], env=environment,
                                      stdout=daemon_log, stderr=daemon_log)
            try:
                wait_for(lambda: (daemon.poll(), list(temporary.glob('*.sock'))),
                         lambda state: state[0] is None and bool(state[1]), 'Daemon startup', 8)
                idle()

                def session(actions=None):
                    nonlocal workspace
                    workspace = subprocess.Popen([*base, 'workspace', '--size', '1360x820', '--debug-live'],
                        env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

                    def find_window():
                        assert workspace.poll() is None, 'Workspace exited before creating window'
                        result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--all', '--pid',
                            str(workspace.pid), '--class', 'Cast(Pro)?Panel'], env=environment,
                            capture_output=True, text=True, timeout=2)
                        return result.stdout.strip().splitlines()[-1] if result.returncode == 0 and result.stdout.strip() else None

                    window = wait_for(find_window, bool, 'Workspace window')
                    xdo('windowfocus', '--sync', window)
                    # A distinct zoom command sent through the UI fences attachment
                    # and later edits via the daemon's ordered acknowledgement queue.
                    time.sleep(.25)
                    current_mode = 'blurred'
                    scroll_offset = 0

                    def click(name, mode=None, fraction=.5):
                        nonlocal scroll_offset
                        chosen = mode or current_mode
                        x, y, width, height = bounds(name, chosen)
                        if name.startswith('background-'):
                            viewport = bounds('sources-scroll', chosen)
                            content = bounds('source-content', chosen)
                            offset = max(0, content[1] + content[3] - viewport[1] - viewport[3])
                            xdo('mousemove', '--window', window, round(viewport[0] + viewport[2] / 2), round(viewport[1] + viewport[3] / 2))
                            xdo('click', '--repeat', '40', '--delay', '3', '5')
                            scroll_offset = offset
                            time.sleep(.08)
                            y -= scroll_offset
                        assert 0 <= y and y + height <= 820, (name, y, height)
                        xdo('mousemove', '--window', window, round(x + width * fraction), round(y + height / 2))
                        xdo('mousedown', '1')
                        time.sleep(.05)
                        xdo('mouseup', '1')
                        idle()

                    def fence(index):
                        value = (1, 1.15, 1.3, 1.5, 1.8)[index]
                        click('zoom-preset-' + str(index))
                        wait_for(idle, lambda state: abs(state['zoom'] - value) < .001, 'UI zoom acknowledgement')

                    def color(name, value, mode):
                        click(name, mode)
                        xdo('key', '--clearmodifiers', 'ctrl+a')
                        xdo('type', '--clearmodifiers', '--delay', '20', value)
                        xdo('key', '--clearmodifiers', 'Return')
                        time.sleep(.12)

                    if actions:
                        fence(3 if abs(idle()['zoom'] - 1.5) > .001 else 4)
                    click('section-composition', 'default')
                    time.sleep(.08)
                    click('section-camera', 'camera')
                    time.sleep(.08)
                    click('section-look')
                    time.sleep(.08)
                    if actions:
                        actions(click, fence, color, window)
                    # Poll status rather than assuming the daemon is responsive after edits.
                    idle()
                    time.sleep(.15)
                    close_window(window)
                    output, errors = workspace.communicate(timeout=10)
                    assert workspace.returncode == 0, errors
                    state = json.loads(output.strip().splitlines()[-1])
                    assert state['connected'] and state['has_frame'], state
                    assert not state['pending'] and not state['edit_error'], state
                    assert state['virtual_paused'] and not state['recording'] and not state['stream_active'], state
                    workspace = None
                    idle()
                    return state

                def capture(window, name):
                    if shutil.which('import'):
                        subprocess.run(['import', '-window', str(window), str(UI / 'build' / name)],
                                       env=environment, capture_output=True, check=True, timeout=5)

                def gradient(click, fence, color, window):
                    click('background-gradient')
                    time.sleep(.15)
                    fence(1)
                    color('background-from', '#1256A0', 'gradient')
                    fence(2)
                    color('background-to', '#A03472', 'gradient')
                    fence(3)
                    capture(window, 'look-review.png')

                state = session(gradient)
                assert state['screen_background'] == 'gradient', state
                assert state['gradient_from'] == '#1256a0' and state['gradient_to'] == '#a03472', state
                reopened = session()
                assert reopened['screen_background'] == 'gradient' and reopened['gradient_from'] == '#1256a0', reopened
                assert abs(reopened['zoom_factor'] - state['zoom_factor']) < .001, reopened

                def solid(click, fence, color, window):
                    click('background-solid')
                    time.sleep(.15)
                    fence(1)
                    color('background-fill', '#247AC4', 'solid')
                    fence(2)
                    # Slider commits through actual mouse input, separately from presets.
                    click('director-zoom', fraction=.5)
                    wait_for(idle, lambda value: value['zoom'] > 2, 'Zoom slider acknowledgement')
                    click('stage-zoom')
                    time.sleep(.1)
                    click('zoom-follow', 'zoom')
                    time.sleep(.15)
                    capture(window, 'zoom-review.png')
                    xdo('key', '--clearmodifiers', 'Escape')

                state = session(solid)
                assert state['screen_background'] == 'solid' and state['screen_background_color'] == '#247ac4', state
                assert state['zoom_factor'] > 2 and state['zoom_follow'], state
                reopened = session()
                assert reopened['screen_background_color'] == '#247ac4' and reopened['zoom_follow'], reopened
                assert abs(reopened['zoom_factor'] - state['zoom_factor']) < .001, reopened

                def blurred(click, fence, color, window):
                    click('background-blurred')
                    time.sleep(.15)
                    fence(1)

                assert session(blurred)['screen_background'] == 'blurred'

                def external(click, fence, color, window):
                    command('settings', 'screen.background', 'solid', 'screen.background_color', '#247ac4',
                            'background.gradient_from', '#102030', 'background.gradient_to', '#908070')
                    command('zoom', 'set', '1.3')
                    command('zoom', 'follow', 'off')
                    wait_for(idle, lambda value: abs(value['zoom'] - 1.3) < .001, 'External zoom')

                state = session(external)
                assert state['screen_background'] == 'solid' and state['screen_background_color'] == '#247ac4', state
                assert state['gradient_from'] == '#102030' and state['gradient_to'] == '#908070', state
                assert abs(state['zoom_factor'] - 1.3) < .001, state
                assert state['can_zoom_follow'] and not state['zoom_follow'], state

                # Verify the same selected solid fill reaches encoded output. The
                # private Xorg source is inset, leaving this corner outside its tile.
                recording = temporary / 'solid.mkv'
                command('record', 'start', str(recording))
                wait_for(status, lambda value: value['record']['state'] == 'recording' and value['record']['duration'] > .4,
                         'Short test recording', 10)
                command('record', 'stop')
                wait_for(status, lambda value: value['record']['state'] == 'stopped' and not value['record']['finalizing'],
                         'Recording finalization', 10)
                decoded = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(recording), '-frames:v', '1',
                    '-vf', 'crop=2:2:0:0', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                    capture_output=True, timeout=10)
                assert decoded.returncode == 0 and len(decoded.stdout) == 12, decoded.stderr
                assert all(abs(actual - expected) <= 12 for actual, expected in zip(decoded.stdout[:3], (0x24, 0x7a, 0xc4))), decoded.stdout
                idle()
            except Exception:
                failed = UI / 'build/tmp/look-failure'
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
        shutil.rmtree(UI / 'build/tmp/look-failure', ignore_errors=True)
        print('Background/Zoom: native edits, acknowledgement, reopen/external sync, idle outputs and encoded fill passed')


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--inside-private-xvfb':
        inside(Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve())
        return 0
    assert len(sys.argv) <= 3, 'Usage: test_look.py [cast-binary [preview-binary]]'
    for dependency in ('xvfb-run', 'xauth', 'xdotool', 'ffmpeg'):
        assert shutil.which(dependency), f'Look integration requires {dependency}'
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'cast').resolve()
    fixture = Path(sys.argv[2] if len(sys.argv) > 2 else UI / 'build/cast-ui-preview').resolve()
    assert binary.is_file() and fixture.is_file(), 'Build embedded cast and standalone preview first'
    temporary = UI / 'build/tmp'
    temporary.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, TMPDIR=str(temporary), SDL_RENDER_DRIVER='software', SDL_VIDEODRIVER='x11')
    return subprocess.run(['xvfb-run', '-a', '-s', '-screen 0 1600x1200x24', sys.executable,
        str(Path(__file__).resolve()), '--inside-private-xvfb', str(binary), str(fixture)],
        env=environment, timeout=150).returncode


if __name__ == '__main__':
    raise SystemExit(main())
