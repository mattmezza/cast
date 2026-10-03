#!/usr/bin/env python3
"""Exercise daemon streaming privately with synthetic pixels and a loopback ingest."""
import errno
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get('CAST_TEST_BINARY', ROOT / 'cast')).resolve()


def wait_until(predicate, message, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError(message)


def bound(port):
    with socket.socket() as probe:
        try:
            probe.bind(('127.0.0.1', port))
        except OSError as error:
            if error.errno == errno.EADDRINUSE:
                return True
            raise
    return False


def main():
    with tempfile.TemporaryDirectory(prefix='cast-three-outputs-') as directory:
        temporary = Path(directory)
        key = temporary / 'stream-key'
        key.write_text('synthetic-test-only\n')
        key.chmod(0o600)
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        socket_path = temporary / 'cast.sock'
        config = temporary / 'cast.conf'
        config.write_text(
            '[output]\nbackend=synthetic\ndevice=none\nenabled=false\n'
            'width=160\nheight=96\nfps=20\npause_title=\npause_background=#123456\n'
            'blur_title=\nblur_subtitle=\nblur_opacity=0\nblur_radius=12\n'
            '[camera]\nenabled=false\n[composition]\nlayout=screen\n'
            '[cursor]\nenabled=false\n[record]\nvideo_codec=ffv1\n'
            f'directory={temporary}\n[stream]\nserver_url=rtmp://127.0.0.1:{port}/app\n'
            f'key_file={key}\nreconnect_attempts=0\n')
        environment = dict(os.environ, XDG_RUNTIME_DIR=directory)
        base = [str(BINARY), '--config', str(config), '--socket', str(socket_path)]
        received = temporary / 'received.flv'
        recording = temporary / 'recording.mkv'
        with (temporary / 'ingest.log').open('w+') as ingest_log, \
                (temporary / 'daemon.log').open('w+') as daemon_log:
            ingest = subprocess.Popen([
                'ffmpeg', '-nostdin', '-v', 'error', '-listen', '1', '-i',
                f'rtmp://127.0.0.1:{port}/app/synthetic-test-only',
                '-c', 'copy', '-f', 'flv', str(received)], stdout=ingest_log, stderr=ingest_log)
            daemon = None
            try:
                wait_until(lambda: bound(port), 'local ingest did not listen')
                daemon = subprocess.Popen(base, env=environment, stdout=daemon_log, stderr=daemon_log)
                wait_until(socket_path.exists, 'synthetic daemon did not start')

                def command(*arguments, success=True):
                    started = time.monotonic()
                    result = subprocess.run(base + list(arguments), env=environment,
                                            capture_output=True, text=True, timeout=4)
                    assert (result.returncode == 0) == success, (arguments, result.stdout, result.stderr)
                    assert time.monotonic() - started < 1.5, 'control blocked on streaming network'
                    return result.stdout

                def status():
                    return json.loads(command('status', '--json'))

                initial = status()
                assert initial['stream']['state'] == 'stopped' and not initial['virtual']['enabled']
                command('stream', 'toggle', success=False)
                command('stream', 'start')
                command('stream', 'start', success=False)
                wait_until(lambda: status()['stream']['state'] == 'streaming', 'stream did not connect')
                assert status()['stream']['paused'], 'stream start revealed composition'
                time.sleep(.4)
                command('stream', 'resume')
                time.sleep(.4)
                command('virtual', 'start')
                command('virtual', 'resume')
                command('record', 'start', str(recording))
                time.sleep(.4)
                before = status()
                command('settings', 'camera.radius', '37', 'stream.video_bitrate_kbps', '4321',
                        success=False)
                assert status()['stream']['generation'] == before['stream']['generation']
                command('stream', 'freeze')
                time.sleep(.35)
                command('stream', 'blur', 'on')
                time.sleep(.35)
                frozen = status()
                assert frozen['stream']['frozen'] and frozen['stream']['blurred']
                assert frozen['record']['state'] == 'recording' and frozen['virtual']['state'] == 'virtual'
                command('stream', 'pause')
                time.sleep(.35)
                command('pause')
                command('resume')
                restored = status()
                assert restored['stream']['paused'], 'group resume revealed independent stream pause'
                assert restored['record']['state'] == 'recording' and restored['virtual']['state'] == 'virtual'
                command('stream', 'resume')
                assert status()['stream']['frozen'] and status()['stream']['blurred']
                command('stream', 'unfreeze')
                command('stream', 'unblur')
                time.sleep(.4)
                for obsolete in [('live', 'pause'), ('preview', 'target', 'live'),
                                 ('annotations', 'live', 'keys', 'off')]:
                    command(*obsolete, success=False)
                assert not status()['stream']['paused']
                command('virtual', 'stop')
                assert status()['record']['state'] == 'recording' and status()['stream']['active']
                command('record', 'stop')
                assert status()['stream']['active']
                command('stream', 'stop')
                command('stream', 'stop')
                command('resume')
                assert not status()['stream']['active'] and not status()['virtual']['enabled']
                wait_until(lambda: not status()['record']['finalizing'], 'recording did not finalize')
                command('quit')
                assert daemon.wait(timeout=5) == 0
                ingest.wait(timeout=5)
                daemon_log.seek(0)
                assert 'synthetic-test-only' not in daemon_log.read(), 'key leaked in daemon logs'
                info = json.loads(subprocess.check_output([
                    'ffprobe', '-v', 'error', '-show_streams', '-show_packets', '-of', 'json',
                    str(received)], text=True))
                assert {stream['codec_name'] for stream in info['streams']} == {'h264', 'aac'}
                for stream in info['streams']:
                    pts = [float(packet['pts_time']) for packet in info['packets']
                           if packet['stream_index'] == stream['index'] and 'pts_time' in packet]
                    assert len(pts) > 30 and all(a < b for a, b in zip(pts, pts[1:])), pts
                    assert max(b - a for a, b in zip(pts, pts[1:])) < .16, 'stream effect cut media time'
                pixels = subprocess.check_output([
                    'ffmpeg', '-v', 'error', '-i', str(received), '-f', 'rawvideo', '-pix_fmt',
                    'rgb24', 'pipe:1'])
                frame_size = 160 * 96 * 3
                frames = [pixels[i:i + frame_size] for i in range(0, len(pixels), frame_size)]
                def solid(frame):
                    return all(abs(frame[i] - (0x12, 0x34, 0x56)[i % 3]) <= 4
                               for i in range(len(frame)))
                assert sum(map(solid, frames)) >= 8, 'solid streaming pause missing'
                assert any(not solid(frame) for frame in frames), 'stream resume never revealed pixels'
                audio = subprocess.check_output([
                    'ffmpeg', '-v', 'error', '-i', str(received), '-vn', '-f', 'f32le', 'pipe:1'])
                samples = struct.unpack(f'<{len(audio) // 4}f', audio)
                assert len(samples) > 48000 and max(map(abs, samples)) < .00001
                print('Three output daemon integration: private H.264/AAC ingest, paused start, '
                      'effects, continuous timestamps, group restoration, atomic locks, '
                      'independent stop and secret redaction passed')
            finally:
                for process in (daemon, ingest):
                    if process and process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()


if __name__ == '__main__':
    main()
