#!/usr/bin/env python3
"""Exercise the native panel on a private Xvfb with synthetic media only.

Requires an optional PANEL=1 build, Xvfb, xdotool, and xclip. No desktop or hardware device
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
        env.update(XDG_RUNTIME_DIR=directory, SDL_VIDEODRIVER="x11",
                   CAST_PANEL_UI_STATE=str(root / "ui.json"))
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

        scale = float(env.get("SDL_VIDEO_X11_SCALING_FACTOR", "1"))

        def xdo(*args):
            args = list(args)
            for index, arg in enumerate(args):
                if arg == "mousemove" and args[index+1] == "--window":
                    args[index+3] = round(float(args[index+3])*scale)
                    args[index+4] = round(float(args[index+4])*scale)
                elif arg == "windowsize":
                    args[index+2] = round(float(args[index+2])*scale)
                    args[index+3] = round(float(args[index+3])*scale)
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

            def renderer_ready():
                assert panel.poll() is None, "panel exited before renderer initialization"
                try:
                    ready = json.loads((root / "ui.json").read_text())
                except (FileNotFoundError, json.JSONDecodeError):
                    return False
                return ready["frame"] > 0 and ready["has_preview"] and bool(ready["widgets"])

            # SDL_CreateRenderer may replace the initial X11 window for a GL visual.
            # A real rendered preview proves renderer/font/layout initialization finished.
            wait_until(renderer_ready, "panel did not render its first daemon preview")

            def find_window():
                result = subprocess.run(["xdotool", "search", "--onlyvisible", "--pid", str(panel.pid),
                                         "--class", "CastPanel"], env=env, text=True,
                                        capture_output=True, timeout=3)
                windows[:] = result.stdout.splitlines()
                return bool(windows)

            wait_until(find_window, "native panel window did not open")
            window = windows[0]
            xdo("windowfocus", "--sync", window)

            def click(x, y):
                xdo("mousemove", "--window", window, x, y, "click", 1)
                time.sleep(0.12)

            def ui():
                for attempt in range(20):
                    try:
                        return json.loads((root / "ui.json").read_text())
                    except (FileNotFoundError, json.JSONDecodeError):
                        time.sleep(.03)
                raise AssertionError("No native layout diagnostics")

            def widget(identifier):
                return next((w for w in ui()["widgets"]
                             if w["id"] == identifier or w["key"] == identifier), None)

            def await_panel_ack(expected=None):
                # Verify a new rendered frame after the acknowledged command generation.
                before = ui()
                expected = before["command_queued"] if expected is None else expected
                def acknowledged():
                    current = ui()
                    return (current["frame"] > before["frame"] and
                            current["command_completed"] >= expected and
                            current["command_queued"] == current["command_completed"])
                wait_until(acknowledged, "Panel command was not acknowledged in a new frame")

            def click_widget(identifier):
                await_panel_ack()
                wait_until(lambda: widget(identifier) and widget(identifier)["enabled"],
                           f"Control {identifier} is unavailable")
                for attempt in range(25):
                    item = widget(identifier)
                    x, y, w, h = item["box"]
                    area = ui()["scroll"]
                    inside_scroll = (item["id"] >= 1000 or 40 <= item["id"] < 90 or
                                     100 <= item["id"] < 105 or 200 <= item["id"] < 300)
                    if not inside_scroll or (y >= area[1] and y+h <= area[1]+area[3]):
                        click(x+w/2, y+h/2)
                        return
                    xdo("mousemove", "--window", window, int(area[0]+area[2]/2),
                        int(area[1]+area[3]/2), "click", 4 if y < area[1] else 5)
                    time.sleep(.10)
                raise AssertionError(f"Control {identifier} could not be reached by scrolling")

            def navigate(tab):
                if ui()["tab"] >= 0:
                    click_widget(99)
                    wait_until(lambda: ui()["tab"] == -1, "Back did not reach home")
                click_widget(100+tab)
                wait_until(lambda: ui()["tab"] == tab, "Section did not open")

            def assert_preview():
                wait_until(lambda: ui()["has_preview"], "Actual preview frame missing")
                x, y, w, h = ui()["preview"]
                geometry = xdo("getwindowgeometry", "--shell", window)
                dimensions = dict(line.split("=", 1) for line in geometry.splitlines() if "=" in line)
                assert w > 80 and h > 50 and x >= 0 and y >= 0
                assert x+w <= int(dimensions["WIDTH"])/scale and y+h <= int(dimensions["HEIGHT"])/scale

            def capture(name):
                destination = os.environ.get("CAST_PANEL_TEST_SCREENSHOTS")
                if destination:
                    Path(destination).mkdir(parents=True, exist_ok=True)
                    subprocess.run(["import", "-window", window,
                                    str(Path(destination) / (name+".png"))], env=env, check=True)

            # Home contains navigation and only currently relevant output actions.
            assert ui()["tab"] == -1
            assert ui()["density"] == scale
            assert ui()["raster_font_size"] == 16*scale
            assert state()["live"]["state"] == "paused"
            assert widget(31) is None and widget(33) is None
            assert_preview()
            click_widget(30)
            wait_until(lambda: state()["live"]["state"] == "live", "Resume live did not apply")
            capture("home")
            click_widget(31)
            wait_until(lambda: state()["live"]["state"] == "frozen", "Freeze did not apply")
            click_widget(31)
            wait_until(lambda: state()["live"]["state"] == "live", "Unfreeze did not apply")

            # Every screen keeps the actual preview and output controls accessible.
            for tab in range(5):
                navigate(tab)
                assert_preview()
                assert widget(30) and widget(32)
            navigate(1)
            capture("camera")
            old = ui()["preview_panel"]
            x, y, w, h = old
            xdo("mousemove", "--window", window, int(x+w/2), int(y+h-15), "mousedown", 1,
                "mousemove", "--window", window, int(x+w/2-60), int(y+h+20), "mouseup", 1)
            wait_until(lambda: ui()["preview_panel"][0] < x-40, "Preview could not be dragged")
            assert_preview()
            moved = ui()["preview_panel"]
            for identifier in (99, 30, 32):
                bx, by, bw, bh = widget(identifier)["box"]
                px, py, pw, ph = moved
                assert px+pw <= bx or px >= bx+bw or py+ph <= by or py >= by+bh, "Preview obscured protected controls"
            navigate(2)
            assert ui()["preview_panel"] == moved, "Preview position reset during navigation"
            # Return it to the reserved top corner before testing controls underneath.
            x, y, w, h = moved
            xdo("mousemove", "--window", window, int(x+w/2), int(y+h-15), "mousedown", 1,
                "mousemove", "--window", window, int(old[0]+w/2), int(old[1]+h-15), "mouseup", 1)
            time.sleep(.15)
            click_widget(21)
            wait_until(lambda: ui()["record_preview"], "Record preview was not selected")
            assert_preview()
            navigate(0)
            assert ui()["record_preview"], "Preview target reset during navigation"
            click_widget(20)
            click_widget(51)
            wait_until(lambda: state()["layout"] == "split", "Split layout did not apply")
            cli("layout", "camera")
            time.sleep(.15)
            click_widget(50)
            wait_until(lambda: state()["layout"] == "overlay", "CLI/UI layout synchronization failed")

            # Start/pause/resume/stop preserve independent live state and the same file.
            click_widget(32)
            wait_until(lambda: state()["record"]["state"] == "recording", "Record did not start")
            click_widget(33)
            wait_until(lambda: state()["record"]["state"] == "paused", "Pause record did not apply")
            click_widget(33)
            wait_until(lambda: state()["record"]["state"] == "recording", "Resume record did not apply")
            click_widget(34)
            wait_until(lambda: state()["live"]["state"] == "paused" and
                       state()["record"]["state"] == "paused", "Pause all did not apply")
            click_widget(34)
            wait_until(lambda: state()["record"]["state"] == "recording", "Resume all did not apply")
            click_widget(32)
            wait_until(lambda: state()["record"]["state"] == "stopped" and not state()["record"]["finalizing"],
                       "Stop record did not finalize")

            # Drafts survive navigation and only Apply/Enter submits them.
            navigate(4)
            click_widget("output.pause_text")
            xdo("key", "--clearmodifiers", "ctrl+a")
            xdo("type", "--clearmodifiers", "--delay", "12", "Private session")
            assert state()["live"]["message"] != "Private session", "Editing applied a draft"
            navigate(1)
            navigate(4)
            click_widget("output.pause_text")
            submitted = ui()["command_queued"] + 1
            xdo("key", "--clearmodifiers", "Return")
            wait_until(lambda: state()["live"]["message"] == "Private session", "Draft did not survive navigation")
            await_panel_ack(submitted)
            click_widget("output.pause_text")
            xdo("key", "--clearmodifiers", "Tab", "shift+Tab")
            subprocess.run(["xclip", "-selection", "clipboard", "-loops", "2"], env=env,
                           input="Private café", text=True, stdout=subprocess.DEVNULL,
                           stderr=log, check=True, timeout=3)
            submitted = ui()["command_queued"] + 1
            xdo("key", "--clearmodifiers", "ctrl+v", "Return")
            wait_until(lambda: state()["live"]["message"] == "Private café", "UTF-8 paste or keyboard focus failed")
            await_panel_ack(submitted)
            xdo("key", "--clearmodifiers", "Escape")
            wait_until(lambda: ui()["tab"] == -1, "Escape did not return home")

            navigate(0)
            click_widget("composition.fit")
            xdo("key", "--clearmodifiers", "Down", "Return")
            original_zoom = state()["zoom"]
            click_widget("zoom.factor")
            xdo("key", "--clearmodifiers", "ctrl+a")
            xdo("type", "--clearmodifiers", "99")
            xdo("key", "--clearmodifiers", "Return")
            time.sleep(.2)
            assert state()["zoom"] == original_zoom, "Invalid numeric value reached daemon"
            click_widget("zoom.factor")
            xdo("key", "--clearmodifiers", "ctrl+a")
            xdo("type", "--clearmodifiers", "3.25")
            xdo("key", "--clearmodifiers", "Return")
            wait_until(lambda: state()["zoom"] == 3.25, "Valid numeric edit did not apply")

            # Minimum width keeps state, preview, controls, and section back navigation.
            xdo("windowsize", window, "360", "640")
            time.sleep(.2)
            navigate(1)
            assert_preview()
            capture("narrow-camera")
            click_widget(30)
            wait_until(lambda: state()["live"]["state"] == "paused", "Narrow pause control failed")
            click_widget(99)
            capture("narrow-home")
            assert_preview()

            # A lost daemon does not close the window; it reconnects to a new generation.
            cli("quit")
            daemon.wait(timeout=8)
            time.sleep(0.4)
            assert panel.poll() is None, "panel exited on daemon loss"
            daemon = start_daemon()
            wait_until(lambda: (root / "daemon.sock").exists(), "daemon restart did not open socket")
            time.sleep(0.9)
            click_widget(30)
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
    for dependency in ("xvfb-run", "xdotool", "xclip"):
        if not shutil.which(dependency):
            raise SystemExit(f"native panel check requires {dependency}")
    if "--inside" not in sys.argv:
        result = subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 2048x2048x24",
                                 sys.executable, __file__, "--inside"], timeout=120)
        raise SystemExit(result.returncode)
    exercise()
    print("native panel: navigation, persistent draggable preview, live/record, drafts, UTF-8, keyboard, 360px resize, reconnect passed")
