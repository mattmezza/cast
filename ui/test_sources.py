#!/usr/bin/env python3
"""Exercise source discovery/selection using a private Xvfb and isolated daemon."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from test_composition import ROOT, UI, close_window, widget_id


def inside(binary, fixture):
    with tempfile.TemporaryDirectory(prefix='sources-', dir=UI / 'build/tmp') as directory:
        temporary = Path(directory)
        config = temporary / 'cast.conf'
        # Never open a host camera: the only configured device is nonexistent.
        config.write_text('[output]\nbackend=xorg\nwidth=640\nheight=480\nfps=15\n'
                          '[camera]\nenabled=true\ndevice=/dev/video999999\nwidth_percent=26\n'
                          'anchor=bottom-right\n[composition]\nlayout=overlay\nfit=contain\n')
        environment = dict(os.environ, XDG_RUNTIME_DIR=directory,
                           XDG_CONFIG_HOME=str(temporary / 'config'),
                           XDG_CACHE_HOME=str(temporary / 'cache'),
                           XDG_DATA_HOME=str(temporary / 'data'))
        base = [str(binary), '--config', str(config), '--output-device', 'none']

        def command(*args):
            result = subprocess.run([*base, *args], env=environment, capture_output=True,
                                    text=True, timeout=5)
            assert result.returncode == 0, (args, result.stderr, result.stdout)
            return result.stdout

        def idle():
            state = json.loads(command('status', '--json'))
            assert state['virtual']['state'] in ('paused', 'stopped'), state
            assert state['record']['state'] == 'stopped', state
            assert not state['stream']['active'], state
            return state

        def xdo(*args):
            subprocess.run(['xdotool', *map(str, args)], env=environment, check=True,
                           capture_output=True, text=True, timeout=5)

        layout = subprocess.run([str(fixture), '--size', '1360x820', '--frames', '3',
                                 '--debug-layout', 'json'], env=environment,
                                capture_output=True, text=True, timeout=15)
        assert layout.returncode == 0, layout.stderr
        boxes = {box['id']: box for box in json.loads(layout.stdout)['boxes']}

        def box(name):
            value = boxes.get(widget_id(name))
            assert value and value['found'], f'Missing fixture geometry: {name}'
            return value['bounds']

        for name in ('source-choose', 'camera-choose'):
            assert abs(box(name)[3] - 36) < 1, (name, box(name))

        with (temporary / 'daemon.log').open('w+') as daemon_log:
            daemon = subprocess.Popen([*base, '--headless'], env=environment,
                                      stdout=daemon_log, stderr=daemon_log)
            workspace = None
            try:
                deadline = time.monotonic() + 8
                while not list(temporary.glob('*.sock')) and daemon.poll() is None and time.monotonic() < deadline:
                    time.sleep(.05)
                assert daemon.poll() is None and list(temporary.glob('*.sock')), 'Isolated Xorg daemon failed to start'
                idle()
                screens = command('screen', 'list')
                monitors = [line[2:].split()[0] for line in screens.splitlines() if line.startswith(('* ', '  '))]
                assert monitors, ('Private Xvfb did not expose an XRandR monitor', screens)
                cameras = command('camera', 'list')
                camera_empty = cameras.startswith('No accessible V4L2 capture devices.')

                def session(actions):
                    nonlocal workspace
                    workspace = subprocess.Popen([*base, 'workspace', '--size', '1360x820', '--debug-live'],
                        env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    deadline = time.monotonic() + 5
                    window = None
                    while time.monotonic() < deadline and workspace.poll() is None:
                        found = subprocess.run(['xdotool', 'search', '--onlyvisible', '--all', '--pid',
                            str(workspace.pid), '--class', 'Cast(Pro)?Panel'], env=environment,
                            capture_output=True, text=True, timeout=2)
                        if found.returncode == 0 and found.stdout.strip():
                            window = found.stdout.splitlines()[-1]
                            break
                        time.sleep(.03)
                    assert window, 'Workspace did not create a private window'
                    xdo('windowfocus', '--sync', window)
                    time.sleep(.5)

                    def click(name):
                        x, y, width, height = box(name)
                        xdo('mousemove', '--window', window, round(x + width / 2), round(y + height / 2))
                        xdo('mousedown', '1')
                        time.sleep(.06)
                        xdo('mouseup', '1')
                        time.sleep(.25)
                        assert workspace.poll() is None, ('Workspace exited', name)
                        idle()

                    def key(*keys):
                        # SDL dispatches against the previous frame's focus. Allow
                        # Escape/Space and Home/Return to complete separate frames.
                        for name in keys:
                            xdo('key', '--clearmodifiers', name)
                            time.sleep(.15)
                            idle()

                    def select_first(name):
                        click(name)
                        # Discovery is asynchronous and only this private daemon is queried.
                        time.sleep(.5)
                        if name == 'source-choose' and shutil.which('import'):
                            top = max(0, round(box(name)[1]) - 45)
                            subprocess.run(['import', '-window', str(window), '-crop',
                                f'360x240+0+{top}', str(UI / 'build/source-picker.png')],
                                env=environment, timeout=5, check=True, capture_output=True)
                        key('Home', 'Return')
                        time.sleep(.3)

                    actions(click, key, select_first, window)
                    close_window(window)
                    output, errors = workspace.communicate(timeout=10)
                    assert workspace.returncode == 0, errors
                    state = json.loads(output.strip().splitlines()[-1])
                    assert state['connected'], state
                    assert state['virtual_paused'] and not state['recording'] and not state['stream_active'], state
                    idle()
                    workspace = None
                    return state

                def choose_screen(click, key, select_first, window):
                    select_first('source-choose')
                    deadline = time.monotonic() + 3
                    while idle()['source']['monitor'] != monitors[0] and time.monotonic() < deadline:
                        time.sleep(.04)
                    assert idle()['source']['monitor'] == monitors[0], (monitors, idle())
                    # Escape restores trigger focus. Reopening with Space proves focus
                    # restoration through real SDL/X11 keyboard input, then Return confirms.
                    click('source-choose')
                    key('Escape', 'space')
                    time.sleep(.3)
                    key('Home', 'Return')

                screen_state = session(choose_screen)
                assert screen_state['screen_id'] == monitors[0], screen_state
                assert screen_state['camera_enabled'] and screen_state['camera_id'] == '/dev/video999999', screen_state
                assert not screen_state['picker_open'], screen_state
                assert screen_state['devices_count'] == len(monitors), screen_state
                assert not screen_state['devices_loading'] and not screen_state['devices_error'], screen_state

                # Startup tolerates this missing camera; No camera must acknowledge
                # the transition from enabled-but-failed capture to disabled.
                def no_camera(click, key, select_first, window):
                    select_first('camera-choose')

                camera_state = session(no_camera)
                assert not camera_state['camera_enabled'], camera_state
                assert not camera_state['picker_open'] and not camera_state['devices_loading'], camera_state
                assert not camera_state['devices_error'], camera_state
                if camera_empty:
                    assert camera_state['devices_count'] == 0, camera_state

                def dismiss_camera(click, key, select_first, window):
                    click('camera-choose')
                    time.sleep(.4)
                    key('Escape')
                    key('space')
                    time.sleep(.3)
                    # Outside release dismisses the popup without selecting a device.
                    xdo('mousemove', '--window', window, 650, 150)
                    xdo('click', '1')
                    time.sleep(.2)

                dismissed = session(dismiss_camera)
                assert not dismissed['picker_open'] and not dismissed['camera_enabled'], dismissed
                assert idle()['source']['monitor'] == monitors[0]
            except Exception:
                failed = UI / 'build/tmp/sources-failure'
                failed.mkdir(exist_ok=True)
                (failed / 'fixture-layout.json').write_text(json.dumps(boxes, indent=2))
                if shutil.which('import'):
                    subprocess.run(['import', '-window', 'root', str(failed / 'screen.png')],
                                   env=environment, timeout=5, check=False)
                if workspace is not None:
                    if workspace.poll() is None:
                        workspace.terminate()
                    output, errors = workspace.communicate(timeout=5)
                    (failed / 'workspace.log').write_text(output + '\n' + errors)
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
        shutil.rmtree(UI / 'build/tmp/sources-failure', ignore_errors=True)
        print('Source pickers: native discovery, monitor acknowledgement, no-camera acknowledgement, '
              f'keyboard/outside dismissal and idle outputs passed (camera list {"empty" if camera_empty else "populated"})')


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--inside-private-xvfb':
        inside(Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve())
        return 0
    assert len(sys.argv) <= 3, 'Usage: test_sources.py [cast-binary [preview-binary]]'
    for dependency in ('xvfb-run', 'xauth', 'xdotool'):
        assert shutil.which(dependency), f'Source integration requires {dependency}'
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'cast').resolve()
    fixture = Path(sys.argv[2] if len(sys.argv) > 2 else UI / 'build/cast-ui-preview').resolve()
    assert binary.is_file() and fixture.is_file(), 'Build embedded cast and standalone preview first'
    temporary = UI / 'build/tmp'
    temporary.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, TMPDIR=str(temporary), SDL_RENDER_DRIVER='software', SDL_VIDEODRIVER='x11')
    return subprocess.run(['xvfb-run', '-a', '-s', '-screen 0 1600x1200x24', sys.executable,
        str(Path(__file__).resolve()), '--inside-private-xvfb', str(binary), str(fixture)],
        env=environment, timeout=120).returncode


if __name__ == '__main__':
    raise SystemExit(main())
