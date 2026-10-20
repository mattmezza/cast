#!/usr/bin/env python3
"""Exercise the workspace with synthetic or Xorg capture on a private Xvfb."""
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



def assert_workspace_window(process):
    """Inspect only the private X server while the real workspace is alive."""
    x11 = ctypes.CDLL(ctypes.util.find_library('X11'))
    window_type = ctypes.c_ulong
    class ClassHint(ctypes.Structure):
        _fields_ = [('name', ctypes.c_void_p), ('klass', ctypes.c_void_p)]
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XDefaultRootWindow.restype = window_type
    x11.XQueryTree.argtypes = [ctypes.c_void_p, window_type, ctypes.POINTER(window_type),
                              ctypes.POINTER(window_type), ctypes.POINTER(ctypes.POINTER(window_type)),
                              ctypes.POINTER(ctypes.c_uint)]
    x11.XGetClassHint.argtypes = [ctypes.c_void_p, window_type, ctypes.POINTER(ClassHint)]
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XInternAtom.restype = window_type
    x11.XGetWindowProperty.argtypes = [ctypes.c_void_p, window_type, window_type,
        ctypes.c_long, ctypes.c_long, ctypes.c_int, window_type, ctypes.POINTER(window_type),
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
    x11.XFree.argtypes = [ctypes.c_void_p]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    # SDL may destroy a probe window between XQueryTree and XGetClassHint.
    # Handle that ordinary race without Xlib's process-fatal default handler.
    class XError(ctypes.Structure):
        _fields_ = [('type', ctypes.c_int), ('display', ctypes.c_void_p),
                    ('resource', ctypes.c_ulong), ('serial', ctypes.c_ulong),
                    ('code', ctypes.c_ubyte), ('request', ctypes.c_ubyte),
                    ('minor', ctypes.c_ubyte)]
    unexpected = []
    @ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(XError))
    def error_handler(display, event):
        if event.contents.code != 3:  # BadWindow: child vanished during inspection.
            unexpected.append(event.contents.code)
        return 0
    x11.XSetErrorHandler.argtypes = [ctypes.c_void_p]
    x11.XSetErrorHandler.restype = ctypes.c_void_p
    previous_handler = x11.XSetErrorHandler(error_handler)
    display = x11.XOpenDisplay(None)
    assert display, 'Cannot open private display'
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and process.poll() is None:
            root, parent = window_type(), window_type()
            children = ctypes.POINTER(window_type)()
            count = ctypes.c_uint()
            x11.XQueryTree(display, x11.XDefaultRootWindow(display), ctypes.byref(root),
                           ctypes.byref(parent), ctypes.byref(children), ctypes.byref(count))
            windows = [children[i] for i in range(count.value)]
            if children:
                x11.XFree(children)
            for window in windows:
                hint = ClassHint()
                if not x11.XGetClassHint(display, window, ctypes.byref(hint)):
                    continue
                klass = ctypes.string_at(hint.klass).decode() if hint.klass else ''
                for value in (hint.name, hint.klass):
                    if value:
                        x11.XFree(value)
                if klass not in ('CastPanel', 'CastProPanel'):
                    continue
                actual, format_bits, items, remaining = window_type(), ctypes.c_int(), ctypes.c_ulong(), ctypes.c_ulong()
                data = ctypes.POINTER(ctypes.c_ubyte)()
                property_atom = x11.XInternAtom(display, b'_NET_WM_WINDOW_TYPE', 0)
                normal = x11.XInternAtom(display, b'_NET_WM_WINDOW_TYPE_NORMAL', 0)
                result = x11.XGetWindowProperty(display, window, property_atom, 0, 16, 0, 4,
                    ctypes.byref(actual), ctypes.byref(format_bits), ctypes.byref(items),
                    ctypes.byref(remaining), ctypes.byref(data))
                try:
                    assert result == 0 and format_bits.value == 32 and items.value, 'Missing EWMH window type'
                    atoms = ctypes.cast(data, ctypes.POINTER(window_type))
                    assert normal in [atoms[i] for i in range(items.value)], 'Workspace is not NORMAL'
                finally:
                    if data:
                        x11.XFree(data)
                assert not unexpected, unexpected
                return
            time.sleep(.03)
        raise AssertionError('Workspace WM_CLASS was not CastPanel or CastProPanel')
    finally:
        x11.XCloseDisplay(display)
        x11.XSetErrorHandler(previous_handler)


def inside(binary):
    with tempfile.TemporaryDirectory(prefix='cast-workspace-live-') as directory:
        temporary = Path(directory)
        config = temporary / 'cast.conf'
        runtime_environment = dict(os.environ, XDG_RUNTIME_DIR=directory)
        backend = os.environ.get('CAST_UI_TEST_BACKEND', 'synthetic')
        assert backend in ('synthetic', 'xorg')
        config.write_text(f'[output]\nbackend={backend}\nwidth=640\nheight=480\nfps=15\n'
                          '[camera]\nenabled=false\n[composition]\nlayout=screen\n')
        if backend == 'xorg':
            assert shutil.which('xsetroot'), 'xsetroot required for patterned private X11 source'
            subprocess.run(['xsetroot', '-mod', '16', '16', '-fg', '#ef3340', '-bg', '#2058c0'],
                           check=True, env=runtime_environment)
        base = [str(binary), '--config', str(config),
                '--output-device', 'none', '--no-camera']
        def status():
            result = subprocess.run([*base, 'status', '--json'], capture_output=True,
                                    text=True, timeout=5, env=runtime_environment)
            assert result.returncode == 0, result.stderr
            return json.loads(result.stdout)

        def assert_outputs_idle(state):
            assert state['virtual']['state'] in ('paused', 'stopped'), state
            assert state['record']['state'] == 'stopped', state
            assert not state['stream']['active'], state

        def workspace(lane="camera"):
            result = subprocess.run([*base, 'workspace', '--workspace', lane, '--frames', '60', '--debug-live'],
                                    capture_output=True, text=True, timeout=25, env=runtime_environment)
            assert result.returncode == 0, result.stderr
            return json.loads(result.stdout.strip().splitlines()[-1])
        offline = workspace()
        assert not offline['connected'] and not offline['has_frame'], offline
        with (temporary / 'daemon.log').open('w+') as log:
            daemon = subprocess.Popen([*base, '--headless'], stdout=log, stderr=log, env=runtime_environment)
            try:
                deadline = time.monotonic() + 8
                while not list(temporary.glob('*.sock')) and daemon.poll() is None and time.monotonic() < deadline:
                    time.sleep(.05)
                assert list(temporary.glob('*.sock')) and daemon.poll() is None, 'Isolated daemon failed to start'
                assert_outputs_idle(status())
                for lane in ("camera", "record", "stream"):
                    live = workspace(lane)
                    assert live['connected'] and live['has_frame'], live
                    assert live['frame_non_uniform'], ('Composition remained a neutral/privacy frame', live)
                    assert_outputs_idle(status())
                    assert daemon.poll() is None, 'Closing workspace stopped the daemon'
                # A disappearing daemon must invalidate the displayed frame.
                window = subprocess.Popen([*base, 'workspace', '--frames', '180', '--debug-live'],
                                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=runtime_environment)
                assert_workspace_window(window)
                time.sleep(.8)
                daemon.terminate()
                daemon.wait(timeout=10)
                output, errors = window.communicate(timeout=25)
                assert window.returncode == 0, errors
                disconnected = json.loads(output.strip().splitlines()[-1])
                assert not disconnected['connected'] and not disconnected['has_frame'], disconnected
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                    daemon.wait(timeout=10)
    print(f'Live workspace ({backend}, default socket): offline, attach, frames, detach/re-attach and disconnect passed')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--inside-private-xvfb':
        inside(Path(sys.argv[2]).resolve())
    else:
        binary = Path(sys.argv[1] if len(sys.argv) == 2 else ROOT / 'cast').resolve()
        assert shutil.which('xvfb-run') and shutil.which('xauth'), 'Xvfb and xauth required'
        environment = dict(os.environ, SDL_RENDER_DRIVER='software', SDL_VIDEODRIVER='x11')
        backends = [os.environ['CAST_UI_TEST_BACKEND']] if 'CAST_UI_TEST_BACKEND' in os.environ else ['synthetic', 'xorg']
        for backend in backends:
            environment['CAST_UI_TEST_BACKEND'] = backend
            subprocess.run(['xvfb-run', '-a', '-s', '-screen 0 1600x1200x24',
                            sys.executable, __file__, '--inside-private-xvfb', str(binary)],
                           env=environment, check=True, timeout=180)
