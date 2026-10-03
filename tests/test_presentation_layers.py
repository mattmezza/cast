#!/usr/bin/env python3
"""Decode new presentation layers and privacy from a private synthetic daemon."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get('CAST_TEST_BINARY', ROOT / 'cast')).resolve()
WIDTH, HEIGHT = 320, 192


def transparent_logo(path):
    """Four-pixel logo: transparent left half, opaque magenta right half."""
    def chunk(kind, payload):
        return struct.pack('!I', len(payload)) + kind + payload + \
            struct.pack('!I', zlib.crc32(kind + payload) & 0xffffffff)

    row = b'\0' + bytes((0, 255, 0, 0)) * 2 + bytes((255, 0, 255, 255)) * 2
    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('!2I5B', 4, 4, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(row * 4)) + chunk(b'IEND', b''))


def wait_until(predicate, message):
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError(message)


def main():
    with tempfile.TemporaryDirectory(prefix='cast-presentation-layers-') as directory:
        temporary = Path(directory)
        socket = temporary / 'cast.sock'
        logo = temporary / 'logo with spaces.png'
        transparent_logo(logo)
        config = temporary / 'cast.conf'
        config.write_text(
            '[output]\nbackend=synthetic\ndevice=none\nwidth=320\nheight=192\nfps=20\n'
            'pause_title=\npause_subtitle=\npause_footer=\npause_background=#123456\n'
            '[camera]\nenabled=false\n[composition]\nlayout=stage\n'
            '[screen]\nwidth_percent=60\nmargin=20\nradius=12\nborder_width=2\n'
            'background=gradient\n[background]\ngradient_from=#103020\n'
            'gradient_to=#103020\ngradient_via_enabled=false\n'
            '[cursor]\nenabled=false\n[record]\nvideo_codec=ffv1\ncountdown=0\n'
            f'directory={temporary}\n')
        original_config = config.read_bytes()
        env = dict(os.environ, XDG_RUNTIME_DIR=directory)
        base = [str(BINARY), '--socket', str(socket)]
        with (temporary / 'daemon.log').open('w+') as log:
            daemon = subprocess.Popen([str(BINARY), '--config', str(config),
                                       '--socket', str(socket)], env=env, stdout=log, stderr=log)
            try:
                def command(*arguments, success=True):
                    result = subprocess.run(base + list(arguments), env=env, capture_output=True,
                                            text=True, timeout=5)
                    assert (result.returncode == 0) == success, \
                        (arguments, result.stdout, result.stderr)
                    return result.stdout

                def state():
                    return json.loads(command('status', '--json'))

                wait_until(socket.exists, 'presentation daemon did not start')
                command('logo', 'path', str(logo))
                command('logo', 'anchor', 'top-left')
                command('logo', 'margin', '2', '2')
                command('logo', 'size', '20%')
                command('logo', 'on')
                command('text', 'set', 'DEMO {literal}')
                command('text', 'font', 'Noto Sans')
                command('text', 'size', '16')
                command('text', 'anchor', 'top-right')
                command('text', 'margin', '2', '2')
                command('text', 'on')
                assert state()['live']['state'] == 'paused', 'styling resumed live output'
                path = temporary / 'presentation.mkv'
                command('record', 'start', str(path))
                time.sleep(.4)
                command('logo', 'path', str(temporary / 'absent.png'), success=False)
                command('settings', 'screen.radius', '24', 'logo.path',
                        str(temporary / 'absent.png'), success=False)
                time.sleep(.35)
                command('record', 'pause')
                command('text', 'set', 'CHANGED WHILE PAUSED')
                command('logo', 'opacity', '50%')
                assert state()['record']['state'] == 'paused', 'branding resumed recording'
                time.sleep(.35)
                command('record', 'stop')
                wait_until(lambda: not state()['record']['finalizing'], 'presentation did not finalize')
                raw = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(path),
                                               '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'])
                frame_size = WIDTH * HEIGHT * 3
                assert len(raw) % frame_size == 0
                frames = [raw[i:i + frame_size] for i in range(0, len(raw), frame_size)]

                def pixel(frame, x, y):
                    start = (y * WIDTH + x) * 3
                    return frame[start:start + 3]

                def near(actual, expected):
                    return all(abs(a - b) <= 4 for a, b in zip(actual, expected))

                active = [frame for frame in frames if near(pixel(frame, 1, 1), (16, 48, 32))]
                assert len(active) >= 8, 'stage gradient not present in decoded media'
                assert all(near(pixel(frame, 10, 10), (16, 48, 32)) for frame in active), \
                    'transparent logo pixels replaced their background'
                assert all(near(pixel(frame, 50, 10), (255, 0, 255)) for frame in active), \
                    'logo lost alpha/path rollback or placement in recorded video'
                assert all(any(all(value > 220 for value in pixel(frame, x, y))
                               for y in range(2, 26) for x in range(180, WIDTH - 2))
                           for frame in active), 'font-selected text not recorded'
                assert sum(all(near(frame[i:i + 3], (18, 52, 86))
                               for i in range(0, frame_size, 3)) for frame in frames) >= 4, \
                    'solid privacy pause leaked branding or screen pixels'
                assert config.read_bytes() == original_config, 'session controls rewrote config'
                command('quit')
                assert daemon.wait(timeout=5) == 0
                print('decoded stage, transparent logo, static text, atomic path rejection and privacy passed')
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
