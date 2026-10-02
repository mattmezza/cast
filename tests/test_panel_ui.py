#!/usr/bin/env python3
"""Exercise the native panel on a private Xvfb with synthetic media only.

Requires an optional PANEL=1 build, Xvfb, and xdotool. No desktop or hardware device
is captured. UI actions are checked through the daemon's ordinary CLI status.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


PROJECT = Path(__file__).resolve().parent.parent
BINARY = str(Path(os.environ.get("CAST_PANEL_TEST_BINARY", PROJECT / "cast")).resolve())


def wait_until(predicate, description, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.04)
    raise AssertionError(description)


def exercise():
    with tempfile.TemporaryDirectory(prefix="cast-panel-ui-") as directory:
        root = Path(directory)
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=directory, SDL_VIDEODRIVER="x11")
        config = root / "cast.conf"
        config.write_text("[record]\ndirectory=" + directory + "\ncountdown=0\n")
        common = ["--config", str(config), "--socket", str(root / "daemon.sock")]
        daemon = None
        panel = None
        log = (root / "native.log").open("w+")

        def cli(*args):
            result = subprocess.run([BINARY, *common, *args], env=env, text=True,
                                    capture_output=True, timeout=8)
            assert result.returncode == 0, (args, result.stdout, result.stderr)
            return result.stdout

        def state():
            return json.loads(cli("status", "--json"))

        def xdo(*args):
            return subprocess.run(["xdotool", *map(str, args)], env=env, check=True,
                                  text=True, capture_output=True, timeout=8).stdout.strip()

        def start_daemon():
            return subprocess.Popen([BINARY, *common, "--backend", "synthetic",
                                     "--camera-device", "synthetic", "--output-device", "none",
                                     "--width", "320", "--height", "240", "--fps", "20"],
                                    env=env, stdout=log, stderr=log)

        try:
            daemon = start_daemon()
            wait_until(lambda: (root / "daemon.sock").exists(), "synthetic daemon did not start")
            panel = subprocess.Popen([BINARY, *common, "panel"], env=env, stdout=log, stderr=log)
            windows = []

            def find_window():
                result = subprocess.run(["xdotool", "search", "--pid", str(panel.pid),
                                         "--class", "CastPanel"], env=env, text=True,
                                        capture_output=True, timeout=3)
                windows[:] = result.stdout.splitlines()
                return bool(windows)

            wait_until(find_window, "native panel window did not open")
            window = windows[0]
            xdo("windowfocus", "--sync", window)
            time.sleep(0.7)

            def click(x, y):
                xdo("mousemove", "--window", window, x, y, "click", 1)
                time.sleep(0.12)

            # Live output begins privacy paused. The UI reads the daemon's actual state.
            assert state()["live"]["state"] == "paused"
            click(564, 84)
            wait_until(lambda: state()["live"]["state"] == "live", "Resume live did not apply")
            click(652, 84)
            wait_until(lambda: state()["live"]["state"] == "frozen", "Freeze did not apply")
            click(652, 84)
            wait_until(lambda: state()["live"]["state"] == "live", "Unfreeze did not apply")

            click(240, 352)
            wait_until(lambda: state()["layout"] == "split", "Split layout did not apply")
            cli("layout", "camera")
            time.sleep(0.2)
            click(52, 352)
            wait_until(lambda: state()["layout"] == "overlay", "CLI/UI layout synchronization failed")

            # Start/pause/resume/stop are acknowledged asynchronously, preserving output state.
            click(565, 128)
            wait_until(lambda: state()["record"]["state"] == "recording", "Start record did not apply")
            click(667, 128)
            wait_until(lambda: state()["record"]["state"] == "paused", "Pause record did not apply")
            click(667, 128)
            wait_until(lambda: state()["record"]["state"] == "recording", "Resume record did not apply")
            click(565, 128)
            wait_until(lambda: state()["record"]["state"] == "stopped" and not state()["record"]["finalizing"],
                       "Stop record did not finalize")

            # Pause text is an explicit draft until Apply (Enter is its keyboard equivalent).
            click(382, 400)
            click(404, 537)
            xdo("key", "--clearmodifiers", "ctrl+a")
            xdo("type", "--clearmodifiers", "--delay", "12", "Private session")
            assert state()["live"]["message"] != "Private session", "Editing applied a draft"
            xdo("key", "--clearmodifiers", "Return")
            wait_until(lambda: state()["live"]["message"] == "Private session",
                       "Pause-message text edit did not apply")
            xdo("key", "--clearmodifiers", "Tab", "Tab", "shift+Tab", "shift+Tab")
            xdo("type", "--clearmodifiers", "--delay", "12", "Private café")
            xdo("key", "--clearmodifiers", "Return")
            wait_until(lambda: state()["live"]["message"] == "Private café",
                       "Tab/Shift+Tab focus or UTF-8 text editing failed")

            # A valid dropdown selection works with native keyboard focus and arrows.
            click(59, 400)
            click(307, 699)
            xdo("key", "--clearmodifiers", "Down", "Return")
            time.sleep(0.25)
            original_zoom = state()["zoom"]
            click(392, 753)
            xdo("key", "--clearmodifiers", "ctrl+a")
            xdo("type", "--clearmodifiers", "99")
            xdo("key", "--clearmodifiers", "Return")
            time.sleep(0.2)
            assert state()["zoom"] == original_zoom, "Out-of-range input changed the daemon"
            click(392, 753)
            xdo("key", "--clearmodifiers", "ctrl+a")
            xdo("type", "--clearmodifiers", "3.25")
            xdo("key", "--clearmodifiers", "Return")
            wait_until(lambda: state()["zoom"] == 3.25, "Valid numeric input did not apply")
            cli("layout", "screen")

            # Compact layout keeps pause/record controls available and the same window identity.
            xdo("windowsize", window, "540", "620")
            time.sleep(0.2)
            click(70, 253)
            wait_until(lambda: state()["live"]["state"] == "paused", "Compact pause control failed")

            # A lost daemon does not close the window; it reconnects to a new generation.
            cli("quit")
            daemon.wait(timeout=8)
            time.sleep(0.4)
            assert panel.poll() is None, "panel exited on daemon loss"
            daemon = start_daemon()
            wait_until(lambda: (root / "daemon.sock").exists(), "daemon restart did not open socket")
            time.sleep(0.9)
            click(70, 253)
            wait_until(lambda: state()["live"]["state"] == "live", "Panel did not reconnect")
            xdo("key", "--clearmodifiers", "ctrl+q")
            assert panel.wait(timeout=8) == 0, "panel window did not close cleanly"
            assert daemon.poll() is None, "closing the panel stopped the daemon"
            cli("quit")
            daemon.wait(timeout=8)
        except Exception:
            log.flush()
            print((root / "native.log").read_text(), file=sys.stderr)
            raise
        finally:
            if panel and panel.poll() is None:
                panel.terminate()
                panel.wait(timeout=8)
            if daemon and daemon.poll() is None:
                daemon.terminate()
                daemon.wait(timeout=8)
            log.close()


if __name__ == "__main__":
    for dependency in ("xvfb-run", "xdotool"):
        if not shutil.which(dependency):
            raise SystemExit(f"native panel check requires {dependency}")
    if "--inside" not in sys.argv:
        result = subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24",
                                 sys.executable, __file__, "--inside"], timeout=90)
        raise SystemExit(result.returncode)
    exercise()
    print("native panel: live/record, settings drafts, keyboard focus, resize, reconnect, clean close passed")
