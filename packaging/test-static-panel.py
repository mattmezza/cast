#!/usr/bin/env python3
"""Paint the actual static software panel on private Xvfb; forbid .so opens."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, default=ROOT / 'build/static-linux/cast')
parser.add_argument('--inside', action='store_true')
args = parser.parse_args()
if not args.inside:
    subprocess.run(['xvfb-run', '-a', '-s', '-screen 0 1280x1024x24', sys.executable,
                    __file__, '--binary', str(args.binary.resolve()), '--inside'], check=True, timeout=45)
    raise SystemExit(0)
spec = importlib.util.spec_from_file_location('static_audio', Path(__file__).with_name('test-static-audio.py'))
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
with tempfile.TemporaryDirectory(prefix='cast-static-panel-') as temporary:
    work = Path(temporary)
    config = work / 'cast.conf'
    config.write_text('[output]\nbackend=synthetic\ndevice=none\nenabled=false\n'
                      'width=160\nheight=96\nfps=20\n[camera]\nenabled=false\n'
                      '[audio]\nmic=false\ndesktop=false\nvirtual=false\n')
    snapshot = work / 'panel.json'
    environment = dict(os.environ, XDG_RUNTIME_DIR=str(work), XDG_DATA_HOME=str(work / 'data'),
                       SDL_VIDEODRIVER='x11', CAST_PANEL_UI_STATE=str(snapshot))
    empty_plugins = work / 'empty-plugins'
    empty_plugins.mkdir()
    environment.update(SPA_PLUGIN_DIR=str(empty_plugins), PIPEWIRE_MODULE_DIR=str(empty_plugins))
    common = [str(args.binary.resolve()), '--config', str(config), '--socket', str(work / 'cast.sock')]
    traces, processes = [], []
    def command(*arguments):
        trace = work / ('trace-' + str(len(traces)) + '.log')
        traces.append(trace)
        return ['strace', '-f', '--kill-on-exit', '-qq', '-s', '4096', '-e', 'trace=openat',
                '-o', str(trace), '--', *common, *arguments]
    with (work / 'panel.log').open('w+') as log:
        try:
            daemon = subprocess.Popen(command('--headless', '--no-camera', '--no-virtual'),
                                      env=environment, stdout=log, stderr=log)
            processes.append(daemon)
            audit.wait(lambda: (work / 'cast.sock').exists() or daemon.poll() is not None, 'daemon did not start')
            assert daemon.poll() is None, 'static daemon exited'
            panel = subprocess.Popen(command('panel'), env=environment, stdout=log, stderr=log)
            processes.append(panel)
            def painted():
                if panel.poll() is not None:
                    raise AssertionError('static software panel exited before painting')
                try:
                    return json.loads(snapshot.read_text()).get('frame', 0) >= 3
                except (OSError, ValueError):
                    return False
            audit.wait(painted, 'static software panel did not paint')
            window = subprocess.check_output(['xdotool', 'search', '--onlyvisible', '--class', '^Cast(Pro)?Panel$'],
                                             env=environment, text=True).splitlines()[0]
            subprocess.run(['xdotool', 'windowclose', window], env=environment, check=True)
            panel.wait(timeout=10)
            subprocess.run(command('quit'), env=environment, stdout=log, stderr=log, check=True, timeout=10)
            daemon.wait(timeout=10)
            audit.audit_traces(traces)
            print('Actual static X11 software panel painted; no successful shared-library opens')
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
