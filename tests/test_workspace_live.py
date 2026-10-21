#!/usr/bin/env python3
"""Real live-file lane -> synthetic daemon -> recorded/exported A/V, no devices."""
import array
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get('CAST_TEST_BINARY', ROOT / 'cast')).resolve()


def wait(predicate, description, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.03)
    raise AssertionError(description)


def main():
    assert shutil.which('ffmpeg'), 'ffmpeg is required to generate and inspect fixtures'
    with tempfile.TemporaryDirectory(prefix='cast-live-files-') as temporary:
        folder = Path(temporary)
        runtime = folder / 'runtime'
        runtime.mkdir(mode=0o700)
        config, sock = folder / 'cast.conf', runtime / 'cast.sock'
        config.write_text('[output]\nbackend=synthetic\ndevice=none\nenabled=false\n'
            'width=640\nheight=360\nfps=10\n[camera]\nenabled=false\n'
            '[composition]\nlayout=screen\n[cursor]\nenabled=false\n'
            '[audio]\nmic=false\ndesktop=false\nvirtual=false\n'
            '[record]\ncountdown=0\nvideo_codec=ffv1\naudio_codec=flac\ncontainer=matroska\n')
        source, take, exported = (folder / name for name in ('source.mkv', 'take.mkv', 'export.mkv'))
        subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', '-f', 'lavfi', '-i',
            'color=c=red:s=320x180:r=10', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
            '-t', '3', '-c:v', 'ffv1', '-pix_fmt', 'yuv420p', '-c:a', 'flac', str(source)],
            check=True, timeout=20)
        env = dict(os.environ, HOME=str(folder), XDG_RUNTIME_DIR=str(runtime),
                   XDG_CONFIG_HOME=str(folder / 'config'), XDG_DATA_HOME=str(folder / 'data'))
        for name in ('DISPLAY', 'WAYLAND_DISPLAY', 'PIPEWIRE_REMOTE', 'PULSE_SERVER'):
            env.pop(name, None)
        base = [str(BINARY), '--config', str(config), '--socket', str(sock)]

        def command(*args, success=True):
            result = subprocess.run(base + list(args), env=env, capture_output=True,
                                    text=True, timeout=5)
            assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
            return result.stdout.strip()

        def state():
            return int(re.search(r'state=(\d+)', command('media', 'status')).group(1))

        def project_done():
            text = command('project', 'status')
            return text if 'busy=0' in text else None

        with (folder / 'daemon.log').open('w+') as log:
            daemon = subprocess.Popen(base + ['--headless', '--backend', 'synthetic',
                '--no-camera', '--no-virtual', '--output-device', 'none'], env=env,
                stdout=log, stderr=log)
            try:
                wait(lambda: sock.exists() or daemon.poll() is not None, 'daemon socket')
                assert daemon.poll() is None
                command('project', 'new')
                command('timeline', 'source', str(source))
                imported = wait(project_done, 'source import')
                assert 'clips=0' in imported and 'Imported source media' in imported, imported
                # Allocate IDs through the production codec, rather than assume an ID sequence.
                document = folder / 'source.cast'
                command('project', 'save', str(document))
                wait(project_done, 'source project save')
                media_id = re.search(r'^M (\d+) ', document.read_text(), re.MULTILINE).group(1)
                assert command('media', 'list').split()[0] == media_id
                assert state() == 0
                command('record', 'start', str(take))
                time.sleep(.2)
                command('media', 'play', media_id)
                wait(lambda: state() == 2, 'live source playing')
                command('media', 'play', '18446744073709551614', success=False)
                assert state() == 2
                time.sleep(.45)
                command('media', 'pause')
                assert state() == 3
                time.sleep(.2)
                command('media', 'resume')
                wait(lambda: state() == 2, 'live source resumed')
                time.sleep(.45)
                command('media', 'stop')
                assert state() == 0
                time.sleep(.2)
                command('media', 'play', media_id)
                wait(lambda: state() == 2, 'second live playback')
                time.sleep(.2)
                command('pause')
                assert state() == 0
                command('media', 'play', media_id, success=False)
                command('media', 'resume', success=False)
                time.sleep(.2)
                command('resume')
                assert state() == 0, 'global privacy must not restart file playback'
                command('record', 'stop')
                wait(lambda: not json.loads(command('status', '--json'))['record']['finalizing'],
                     'record finalize')
                wait(lambda: 'clips=1' in command('project', 'status'), 'one flattened captured clip')
                command('timeline', 'export', str(exported))

                def export_done():
                    text = command('timeline', 'export', 'status')
                    value = int(re.search(r'state=(\d+)', text).group(1))
                    assert value not in (3, 4), text
                    return value == 2
                wait(export_done, 'flattened montage export', timeout=30)
                pixels = subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', '-i', str(take),
                    '-vf', 'format=rgb24,crop=1:1:320:180', '-an', '-f', 'rawvideo', '-pix_fmt',
                    'rgb24', '-'], check=True, capture_output=True, timeout=20).stdout
                colors = [pixels[i:i+3] for i in range(0, len(pixels)-2, 3)]
                red = sum(r > 200 and g < 45 and b < 45 for r, g, b in colors)
                assert red >= 3 and len(colors)-red >= 2, (red, len(colors))

                def pcm(path):
                    data = subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', '-i', str(path),
                        '-vn', '-ar', '48000', '-ac', '2', '-f', 'f32le', '-'], check=True,
                        capture_output=True, timeout=20).stdout
                    samples = array.array('f')
                    samples.frombytes(data)
                    assert samples and all(math.isfinite(x) for x in samples)
                    return samples
                recorded, montage = pcm(take), pcm(exported)
                chunk = 4800 * 2
                levels = [math.sqrt(sum(x*x for x in recorded[i:i+chunk])/len(recorded[i:i+chunk]))
                          for i in range(0, len(recorded), chunk)]
                assert max(levels) > .02 and min(levels) < .001, levels
                assert max(abs(x) for x in montage) <= max(abs(x) for x in recorded)+.0002, \
                    'source bin must not add its audio a second time during montage export'
                initial = json.loads(command('status', '--json'))
                assert not initial['audio']['mic']['enabled'] and not initial['audio']['desktop']['enabled']
                print('Live workspace: source-only bin, real recorded video/audio, pause/resume, privacy, '
                      'no automatic replay and no duplicated export mix passed')
            except Exception:
                log.flush()
                log.seek(0)
                print(log.read()[-6000:])
                raise
            finally:
                if daemon.poll() is None:
                    try:
                        command('quit')
                    except Exception:
                        daemon.terminate()
                try:
                    daemon.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait()


if __name__ == '__main__':
    main()
