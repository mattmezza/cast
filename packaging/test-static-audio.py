#!/usr/bin/env python3
"""Prove the static payload captures nonzero audio through real PipeWire plugins."""
import argparse
import array
import json
import math
import os
import re
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / 'build/static-linux/cast'


def wait(predicate, message, seconds=15):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        result = predicate()
        if result:
            return result
        time.sleep(.1)
    raise AssertionError(message)


def audit_traces(paths):
    loaded = []
    for path in paths:
        pending = {}
        for line in path.read_text().splitlines():
            process = re.match(r'^(\d+)\s+', line)
            pid = process.group(1) if process else 'main'
            library = re.search(r'"([^"\n]+\.so(?:\.[^"/]+)*)"', line)
            if 'openat(' in line and library:
                pending[pid] = library.group(1)
            if ('openat(' in line or '<... openat resumed>' in line) and '<unfinished ...>' not in line:
                name = pending.pop(pid, None)
                if name and re.search(r'=\s+[0-9]+(?:\s|$)', line):
                    loaded.append(name)
    assert not loaded, 'Cast opened shared libraries: ' + ', '.join(sorted(set(loaded)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=BINARY)
    parser.add_argument('--trace', action='store_true', help='reject successful shared-library opens by Cast')
    args = parser.parse_args()
    binary = args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix='cast-static-audio-') as directory:
        work = Path(directory)
        runtime = work / 'runtime'
        runtime.mkdir(mode=0o700)
        environment = dict(os.environ, XDG_RUNTIME_DIR=str(runtime),
                           PULSE_SERVER='unix:' + str(runtime / 'pulse/native'))
        processes = []
        traces = []

        def traced(command):
            if args.trace and command[0] == str(binary):
                path = work / ('cast-trace-' + str(len(traces)) + '.log')
                traces.append(path)
                return ['strace', '-f', '--kill-on-exit', '-qq', '-s', '4096',
                        '-e', 'trace=openat', '-o', str(path), '--', *command]
            return command
        with (work / 'services.log').open('w+') as log:
            def start(*command):
                process = subprocess.Popen(traced(command), env=environment, stdout=log, stderr=log)
                processes.append(process)
                return process

            def call(*command, check=True):
                return subprocess.run(traced(command), env=environment, capture_output=True,
                                      text=True, check=check, timeout=10)

            try:
                start('pipewire')
                wait(lambda: (runtime / 'pipewire-0').exists(), 'PipeWire server did not start')
                start('wireplumber')
                start('pipewire-pulse')
                wait(lambda: call('pactl', 'info', check=False).returncode == 0,
                     'Pulse protocol did not start')
                call('pactl', 'load-module', 'module-null-sink', 'sink_name=cast_static_test')
                wait(lambda: 'cast_static_test' in call('pactl', 'list', 'short', 'sinks').stdout,
                     'Null sink did not appear')
                signal = array.array('f', (.2 * math.sin(2 * math.pi * 440 * (i // 2) / 48000)
                                           for i in range(48000 * 2 * 30)))
                tone = work / 'tone.raw'
                tone.write_bytes(signal.tobytes())
                start('paplay', '--device=cast_static_test', '--raw', '--format=float32le',
                      '--rate=48000', '--channels=2', str(tone))
                config = work / 'cast.conf'
                config.write_text('[output]\nbackend=synthetic\ndevice=none\nenabled=false\n'
                                  'width=160\nheight=96\nfps=20\n[camera]\nenabled=false\n'
                                  '[audio]\nmic=false\ndesktop=true\n'
                                  'desktop_source=cast_static_test\nvirtual=false\n'
                                  '[record]\nvideo_codec=ffv1\ncountdown=0\n')
                common = [str(binary), '--config', str(config), '--socket', str(runtime / 'cast.sock')]
                daemon = start(*common, '--headless', '--no-camera', '--no-virtual')
                wait(lambda: (runtime / 'cast.sock').exists() or daemon.poll() is not None,
                     'Static daemon did not start')
                assert daemon.poll() is None, 'Static daemon exited during PipeWire initialization'
                wait(lambda: 'cast_static_test' in call(*common, 'audio', 'list').stdout,
                     'Static PipeWire client cannot enumerate null sink')
                wait(lambda: json.loads(call(*common, 'status', '--json').stdout)['audio']['readiness'].get('desktop_ready'),
                     'Static PipeWire capture route did not become ready')
                recording = work / 'capture.mkv'
                call(*common, 'record', 'start', str(recording))
                time.sleep(3)
                call(*common, 'record', 'stop')
                wait(lambda: not json.loads(call(*common, 'status', '--json').stdout)['record']['finalizing'],
                     'Audio test recording did not finalize')
                decoded = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(recording),
                                                   '-map', '0:a:0', '-f', 'f32le', '-'])
                samples = array.array('f')
                samples.frombytes(decoded)
                assert len(samples) > 48000, 'Captured audio too short'
                rms = math.sqrt(sum(x * x for x in samples) / len(samples))
                assert rms > .005, 'Static audio capture produced silence: RMS=' + str(rms)
                print('Static PipeWire null-sink recording passed; RMS=' + str(rms))
                call(*common, 'quit')
                daemon.wait(timeout=10)
                if args.trace:
                    audit_traces(traces)
                    print('Cast audio/CLI trace passed: no successful shared-library opens')
            finally:
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                for process in processes:
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                log.seek(0)
                print(log.read())


if __name__ == '__main__':
    main()
