#!/usr/bin/env python3
"""Drive composition controls against an isolated daemon on a private Xvfb."""
import ctypes
import ctypes.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / 'ui'


def widget_id(name):
    value = 14695981039346656037
    for byte in name.encode():
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return str(value or 1)


def close_window(window):
    """Ask SDL to close through WM_DELETE_WINDOW, retaining its normal cleanup."""
    class Data(ctypes.Union):
        _fields_ = [('bytes', ctypes.c_char * 20), ('longs', ctypes.c_long * 5)]
    class ClientMessage(ctypes.Structure):
        _fields_ = [('type', ctypes.c_int), ('serial', ctypes.c_ulong),
                    ('send_event', ctypes.c_int), ('display', ctypes.c_void_p),
                    ('window', ctypes.c_ulong), ('message_type', ctypes.c_ulong),
                    ('format', ctypes.c_int), ('data', Data)]
    class Event(ctypes.Union):
        _fields_ = [('client', ClientMessage), ('padding', ctypes.c_long * 24)]
    x11 = ctypes.CDLL(ctypes.util.find_library('X11'))
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
                              ctypes.c_long, ctypes.POINTER(Event)]
    x11.XFlush.argtypes = [ctypes.c_void_p]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    assert display, 'Cannot open private display to close workspace'
    try:
        event = Event()
        event.client.type = 33  # ClientMessage
        event.client.display = display
        event.client.window = int(window)
        event.client.message_type = x11.XInternAtom(display, b'WM_PROTOCOLS', 0)
        event.client.format = 32
        event.client.data.longs[0] = x11.XInternAtom(display, b'WM_DELETE_WINDOW', 0)
        assert x11.XSendEvent(display, int(window), 0, 0, ctypes.byref(event))
        x11.XFlush(display)
    finally:
        x11.XCloseDisplay(display)


def inside(binary, fixture):
    with tempfile.TemporaryDirectory(prefix='composition-', dir=UI / 'build/tmp') as directory:
        temporary = Path(directory)
        config = temporary / 'cast.conf'
        config.write_text('[output]\nbackend=synthetic\nwidth=640\nheight=480\nfps=15\n'
                          '[camera]\nenabled=false\nwidth_percent=26\nanchor=bottom-right\n'
                          '[composition]\nlayout=overlay\nfit=contain\n')
        environment = dict(os.environ, XDG_RUNTIME_DIR=directory)
        base = [str(binary), '--config', str(config), '--output-device', 'none', '--no-camera']

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

        result = subprocess.run([str(fixture), '--size', '1360x820', '--frames', '3',
                                 '--debug-layout', 'json'], env=environment,
                                capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, result.stderr
        boxes = {box['id']: box for box in json.loads(result.stdout)['boxes']}

        def box(name):
            value = boxes.get(widget_id(name))
            assert value and value['found'], f'Missing fixture geometry: {name}'
            return value['bounds']

        scroll = box('sources-scroll')
        content = box('source-content')
        maximum_scroll = max(0, content[1] + content[3] - scroll[1] - scroll[3])

        def xdo(*args):
            subprocess.run(['xdotool', *map(str, args)], env=environment, check=True,
                           capture_output=True, text=True, timeout=5)

        with (temporary / 'daemon.log').open('w+') as daemon_log:
            daemon = subprocess.Popen([*base, '--headless'], env=environment,
                                      stdout=daemon_log, stderr=daemon_log)
            try:
                deadline = time.monotonic() + 8
                while not list(temporary.glob('*.sock')) and daemon.poll() is None and time.monotonic() < deadline:
                    time.sleep(.05)
                assert daemon.poll() is None and list(temporary.glob('*.sock')), 'Daemon failed to start'
                idle()

                def session(actions=None):
                    process = subprocess.Popen([*base, 'workspace', '--size', '1360x820',
                        '--debug-live'], env=environment,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    try:
                        deadline = time.monotonic() + 5
                        window = None
                        while time.monotonic() < deadline and process.poll() is None:
                            found = subprocess.run(['xdotool', 'search', '--onlyvisible', '--all', '--pid', str(process.pid),
                                '--class', 'Cast(Pro)?Panel'], env=environment,
                                capture_output=True, text=True, timeout=2)
                            if found.returncode == 0 and found.stdout.strip():
                                window = found.stdout.splitlines()[-1]
                                break
                            time.sleep(.03)
                        assert window, 'Workspace did not create its private window'
                        xdo('windowfocus', '--sync', window)
                        time.sleep(.5)  # Initial attach is asynchronous.
                        scrolled = False

                        def click(name, bottom=False):
                            nonlocal scrolled
                            if bottom != scrolled:
                                xdo('mousemove', '--window', window,
                                    round(scroll[0] + scroll[2] / 2), round(scroll[1] + scroll[3] / 2))
                                xdo('click', '--repeat', '35', '--delay', '5', '5' if bottom else '4')
                                time.sleep(.15)
                                scrolled = bottom
                            x, y, width, height = box(name)
                            if bottom:
                                y -= maximum_scroll
                            assert width > 0 and height > 0 and 0 <= y and y + height <= 820, (name, y, height)
                            xdo('mousemove', '--window', window, round(x + width / 2), round(y + height / 2))
                            time.sleep(.08)
                            xdo('mousedown', '1')
                            time.sleep(.08)
                            xdo('mouseup', '1')
                            time.sleep(.15)
                            idle()
                            assert process.poll() is None, ('Workspace exited during click', name)

                        if actions:
                            actions(click)
                        close_window(window)
                        output, errors = process.communicate(timeout=10)
                        assert process.returncode == 0, errors
                        state = json.loads(output.strip().splitlines()[-1])
                        assert state['connected'] and state['has_frame'], state
                        assert state['virtual_paused'] and not state['recording'] and not state['stream_active'], state
                        idle()
                        return state
                    except Exception:
                        failed = UI / 'build/tmp/composition-failure'
                        failed.mkdir(exist_ok=True)
                        (failed / 'fixture-layout.json').write_text(json.dumps(boxes, indent=2))
                        if shutil.which('import'):
                            subprocess.run(['import', '-window', 'root', str(failed / 'screen.png')],
                                           env=environment, timeout=5, check=False)
                        if process.poll() is None:
                            process.terminate()
                        output, errors = process.communicate(timeout=5)
                        (failed / 'workspace.log').write_text(output + '\n' + errors)
                        daemon_log.flush()
                        shutil.copyfile(temporary / 'daemon.log', failed / 'daemon.log')
                        print(f'Failure diagnostics: {failed}', file=sys.stderr)
                        raise
                    finally:
                        if process.poll() is None:
                            process.terminate()
                            process.communicate(timeout=5)

                def agrees(state, layout, fit, anchor, size):
                    assert state['layout'] == state['ui_layout'] == layout, state
                    assert state['fit'] == fit and state['ui_fit_cover'] == (fit == 'cover'), state
                    assert state['anchor'] == anchor and state['ui_camera_anchor'] == {
                        'top-left': 0, 'left': 3, 'bottom-right': 8}[anchor], state
                    assert state['camera_width_percent'] == state['ui_camera_size'] == size, state

                def first(click):
                    for layout in ('split', 'screen', 'camera', 'stage', 'overlay'):
                        click('layout-' + layout)
                        deadline = time.monotonic() + 3
                        while idle()['layout'] != layout and time.monotonic() < deadline:
                            time.sleep(.03)
                        assert idle()['layout'] == layout, ('sidebar layout', layout, idle(), box('layout-' + layout))
                    click('stagebar-layout-stage')
                    assert idle()['layout'] == 'stage'
                    click('menu-composition')
                    xdo('key', '--clearmodifiers', 'Home', 'Down', 'Return')
                    time.sleep(.2)
                    assert idle()['layout'] == 'split'
                    click('fit-cover')
                    click('anchor-0', bottom=True)
                    click('camera-size-1', bottom=True)
                    click('camera-size-0', bottom=True)

                agrees(session(first), 'split', 'cover', 'top-left', 20)
                # A new UI process must reflect the daemon, including non-default controls.
                agrees(session(), 'split', 'cover', 'top-left', 20)

                def second(click):
                    click('fit-contain')
                    click('anchor-8', bottom=True)
                    click('camera-size-2', bottom=True)

                agrees(session(second), 'split', 'contain', 'bottom-right', 38)

                def external(click):
                    command('layout', 'stage')
                    command('settings', 'composition.fit', 'cover')
                    command('camera', 'anchor', 'left')
                    command('camera', 'size', '26%')
                    time.sleep(.3)
                    idle()

                agrees(session(external), 'stage', 'cover', 'left', 26)
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                    daemon.wait(timeout=10)
        shutil.rmtree(UI / 'build/tmp/composition-failure', ignore_errors=True)
        print('Composition edits: real UI clicks, daemon acknowledgement, restart/external sync and idle outputs passed')


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--inside-private-xvfb':
        inside(Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve())
        return 0
    assert len(sys.argv) <= 3, 'Usage: test_composition.py [cast-binary [preview-binary]]'
    for dependency in ('xvfb-run', 'xauth', 'xdotool'):
        assert shutil.which(dependency), f'Composition integration requires {dependency}'
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'cast').resolve()
    fixture = Path(sys.argv[2] if len(sys.argv) > 2 else UI / 'build/cast-ui-preview').resolve()
    assert binary.is_file() and fixture.is_file(), 'Build embedded cast and standalone preview first'
    temporary = UI / 'build/tmp'
    temporary.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, TMPDIR=str(temporary), SDL_RENDER_DRIVER='software',
                       SDL_VIDEODRIVER='x11')
    return subprocess.run(['xvfb-run', '-a', '-s', '-screen 0 1600x1200x24', sys.executable,
                           str(Path(__file__).resolve()), '--inside-private-xvfb', str(binary),
                           str(fixture)], env=environment, timeout=120).returncode


if __name__ == '__main__':
    raise SystemExit(main())
