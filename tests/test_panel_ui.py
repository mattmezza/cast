#!/usr/bin/env python3
"""Native pointer/keyboard workflows on a private Xvfb, using synthetic media.

No user desktop, webcam, real credential, or public ingest is used. Screenshots
are opt-in and collected together so design review stays bounded.
"""
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

PROJECT = Path(__file__).resolve().parent.parent
BINARY = str(Path(os.environ.get("CAST_PANEL_TEST_BINARY", PROJECT / "cast")).resolve())


def wait_until(predicate, description, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.04)
    raise AssertionError(description)


def exercise():
    with tempfile.TemporaryDirectory(prefix="cast-panel-redesign-") as directory:
        root = Path(directory)
        environment = os.environ.copy()
        environment.update(XDG_RUNTIME_DIR=directory, SDL_VIDEODRIVER="x11",
                           CAST_PANEL_UI_STATE=str(root / "ui.json"))
        scale = float(environment.get("SDL_VIDEO_X11_SCALING_FACTOR", "1"))
        config = root / "cast.conf"
        config.write_text(f"[record]\ndirectory={directory}\ncountdown=0\n")
        common = ["--config", str(config), "--socket", str(root / "daemon.sock")]
        processes = []
        log = (root / "native.log").open("w+")
        window = None
        last_ui = {"frame": 0, "connected": False, "widgets": []}

        def cli(*arguments, success=True):
            result = subprocess.run([BINARY, *common, *arguments], env=environment,
                                    text=True, capture_output=True, timeout=8)
            assert (result.returncode == 0) == success, (arguments, result.stdout, result.stderr)
            return result.stdout

        def state():
            return json.loads(cli("status", "--json"))

        def ui():
            nonlocal last_ui
            try:
                last_ui = json.loads((root / "ui.json").read_text())
            except (FileNotFoundError, json.JSONDecodeError):
                pass
            return last_ui

        def spawn(*arguments):
            child = subprocess.Popen(arguments, env=environment, stdout=log, stderr=log)
            processes.append(child)
            return child

        def start_daemon():
            return spawn(BINARY, *common, "--backend", "synthetic", "--camera-device",
                         "synthetic", "--output-device", "none", "--width", "320",
                         "--height", "240", "--fps", "20")

        def xdo(*arguments):
            return subprocess.run(["xdotool", *map(str, arguments)], env=environment,
                                  check=True, text=True, capture_output=True, timeout=8).stdout.strip()

        def widget(identifier):
            return next((item for item in ui()["widgets"]
                         if item["id"] == identifier or item["key"] == identifier), None)

        def acknowledge():
            wait_until(lambda: not ui().get("command_pending", True), "daemon did not acknowledge panel command")

        def click_widget(identifier):
            acknowledge()
            wait_until(lambda: widget(identifier) and widget(identifier)["enabled"], f"control {identifier} unavailable")
            for _ in range(80):
                item = widget(identifier)
                x, y, width, height = item["box"]
                area = ui()["scroll"]
                pinned = item["id"] in (5, 6, 10, 11, 12, 22, 34) or 400 <= item["id"] < 500
                if pinned or y >= area[1] and y+height <= area[1]+area[3]:
                    xdo("mousemove", "--window", window, round((x+width/2)*scale),
                        round((y+height/2)*scale), "click", 1)
                    time.sleep(.08)
                    return
                xdo("mousemove", "--window", window, round((area[0]+area[2]/2)*scale),
                    round((area[1]+area[3]/2)*scale), "click", 4 if y < area[1] else 5)
                time.sleep(.06)
            raise AssertionError(f"control {identifier} could not be reached by scrolling")

        def navigate(section):
            if ui()["tab"] != 1:
                click_widget(6)
            if ui()["open_section"] != section:
                click_widget(100+section)
            wait_until(lambda: ui()["tab"] == 1 and ui()["open_section"] == section,
                       "Compose disclosure did not open")

        def operate(lane):
            if ui()["tab"] != 0:
                click_widget(5)
            if ui()["open_lane"] != lane:
                click_widget(10+lane if lane < 3 else 123)
            wait_until(lambda: ui()["tab"] == 0 and ui()["open_lane"] == lane,
                       "Operate lane did not open")

        def edit(key, value, apply=False):
            click_widget(key)
            xdo("key", "--clearmodifiers", "ctrl+a")
            if value:
                xdo("type", "--clearmodifiers", value)
            else:
                xdo("key", "--clearmodifiers", "BackSpace")
            wait_until(lambda: ui().get("edit_text") == value, "typing did not update local draft")
            if apply:
                before = ui()["frame"]
                xdo("key", "--clearmodifiers", "Return")
                wait_until(lambda: ui()["frame"] > before, "Enter was not handled")
                acknowledge()

        def capture(name):
            destination = os.environ.get("CAST_PANEL_TEST_SCREENSHOTS")
            if destination:
                Path(destination).mkdir(parents=True, exist_ok=True)
                subprocess.run(["import", "-window", window,
                                str(Path(destination) / (name+".png"))], env=environment, check=True)

        def pinned_accessible():
            bounds = dict(line.split("=", 1) for line in
                          xdo("getwindowgeometry", "--shell", window).splitlines() if "=" in line)
            width, height = int(bounds["WIDTH"])/scale, int(bounds["HEIGHT"])/scale
            for identifier in (5, 6, 10, 11, 12, 22, 34):
                item = widget(identifier)
                assert item, identifier
                x, y, w, h = item["box"]
                assert x >= 0 and y >= 0 and x+w <= width+1 and y+h <= height+1, (identifier, item, bounds)
            assert "has_preview" not in ui() and "preview_panel" not in ui()

        try:
            daemon = start_daemon()
            wait_until(lambda: (root / "daemon.sock").exists(), "synthetic daemon did not start")
            initial = state()
            panel = spawn(BINARY, *common, "panel")
            wait_until(lambda: ui()["frame"] and ui()["connected"], "native renderer did not initialize")
            window = xdo("search", "--onlyvisible", "--pid", panel.pid, "--class", "CastPanel").splitlines()[0]
            xdo("windowfocus", "--sync", window)
            xdo("windowmove", window, 0, 0)
            assert state()["virtual"] == initial["virtual"]
            assert state()["record"] == initial["record"] and state()["stream"]["state"] == "stopped"
            assert ui()["density"] == scale and ui()["raster_font_size"] == 16*scale
            properties = subprocess.run(["xprop", "-id", window, "WM_CLASS", "_NET_WM_WINDOW_TYPE"],
                                        env=environment, text=True, capture_output=True, check=True).stdout
            assert '"cast-panel", "CastPanel"' in properties and "_NET_WM_WINDOW_TYPE_UTILITY" in properties
            pinned_accessible()
            capture("operate-collapsed")

            # Call join: explicit reveal, independent freeze/blur/pause precedence.
            operate(0)
            click_widget(30)
            wait_until(lambda: not state()["virtual"]["paused"], "Resume virtual camera failed")
            # A genuinely stopped daemon makes Pending observable without slowing rendering.
            os.kill(daemon.pid, signal.SIGSTOP)
            try:
                click_widget(31)
                wait_until(lambda: ui().get("command_pending"), "Pending feedback missing", timeout=1)
                before = ui()["frame"]
                wait_until(lambda: ui()["frame"] > before, "panel blocked on daemon acknowledgement", timeout=1)
            finally:
                os.kill(daemon.pid, signal.SIGCONT)
            acknowledge()
            click_widget(36)
            wait_until(lambda: state()["virtual"]["frozen"] and state()["virtual"]["blurred"], "stacked effects failed")
            click_widget(30)
            wait_until(lambda: state()["virtual"]["paused"], "solid pause failed")
            capture("operate-virtual-paused")
            click_widget(30)
            assert state()["virtual"]["frozen"] and state()["virtual"]["blurred"]
            click_widget(36)
            click_widget(31)

            # Recording start/finish, cut/resume, and countdown cancellation.
            operate(1)
            click_widget(32)
            wait_until(lambda: state()["record"]["state"] == "recording", "Start recording failed")
            path = state()["record"]["path"]
            click_widget(39)
            wait_until(lambda: state()["record"]["cut"], "Cut time failed")
            capture("operate-record-cut")
            click_widget(33)
            wait_until(lambda: not state()["record"]["cut"], "same-file resume failed")
            assert state()["record"]["path"] == path
            click_widget(140)
            wait_until(lambda: state()["record"]["state"] == "stopped" and not state()["record"]["finalizing"], "recording finalization failed")
            cli("settings", "record.countdown", "3")
            click_widget(32)
            wait_until(lambda: ui()["countdown"], "countdown not visible")
            capture("operate-countdown")
            click_widget(35)
            wait_until(lambda: not ui()["countdown"], "Cancel did not cancel countdown")
            cli("settings", "record.countdown", "0")

            # Compose enum/boolean drafts stay local; Apply/Revert and validation.
            navigate(1)
            original_anchor = ui()["camera_anchor"]
            click_widget("camera.anchor")
            click_widget(401)
            assert ui()["camera_anchor"] == original_anchor, "enum draft applied without Apply"
            assert widget("camera.anchor")["dirty"]
            click_widget(703)
            assert not widget("camera.anchor")["dirty"]
            click_widget("camera.anchor")
            click_widget(401)
            click_widget(702)
            wait_until(lambda: ui()["camera_anchor"] == "top", "Apply did not commit camera anchor")
            capture("compose-camera")
            navigate(0)
            original_zoom = state()["zoom"]
            edit("zoom.factor", "99", apply=True)
            wait_until(lambda: ui()["error"], "invalid draft error did not arrive")
            assert state()["zoom"] == original_zoom
            edit("zoom.factor", "3.25", apply=True)
            wait_until(lambda: state()["zoom"] == 3.25, "Enter did not apply valid draft")
            navigate(2)
            click_widget(205)
            edit("background.gradient_from", "#153b52")
            click_widget(704)
            acknowledge()
            capture("compose-background")
            navigate(6)
            click_widget(228)
            edit("output.pause_text", "Private session")
            assert state()["virtual"]["message"] != "Private session"
            click_widget(712)
            wait_until(lambda: state()["virtual"]["message"] == "Private session", "presentation draft Apply failed")
            edit("output.pause_text", "", apply=True)
            wait_until(lambda: state()["virtual"]["message"] == "", "optional title could not be blank")
            edit("output.pause_footer", "Returns {date:%A, %d %B} at {time:%H:%M}", apply=True)
            # Exercise SDL's actual clipboard path, including UTF-8 and optional footer.
            unicode_footer = "Back soon · café ☕ — {time:%H:%M}"
            edit("output.pause_footer", "")
            subprocess.run(["xclip", "-selection", "clipboard"], input=unicode_footer,
                           env=environment, text=True, stdout=log, stderr=log, check=True)
            xdo("key", "--clearmodifiers", "ctrl+v", "Return")
            wait_until(lambda: widget("output.pause_footer")["value"] == unicode_footer,
                       "UTF-8 clipboard footer did not apply")
            capture("compose-presentation")
            # Keyboard navigation uses the same retained field draft.
            edit("output.pause_text", "Keyboard draft")
            xdo("key", "--clearmodifiers", "Tab", "shift+Tab")
            assert panel.poll() is None
            xdo("key", "--clearmodifiers", "Escape")

            # Private ingest: setup sheet drafts, paused-start, reveal and stop.
            listener = socket.socket()
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
            listener.close()
            ingest = spawn("ffmpeg", "-hide_banner", "-loglevel", "quiet", "-listen", "1",
                           "-i", f"rtmp://127.0.0.1:{port}/ingest", "-c", "copy", "-f", "flv", str(root / "stream.flv"))
            time.sleep(.3)
            key = root / "stream-key"
            key.write_text("native-local-fixture\n")
            key.chmod(0o600)
            operate(2)
            click_widget(150)
            wait_until(lambda: ui()["stream_setup"], "Streaming setup did not open")
            edit("stream.server_url", f"rtmp://127.0.0.1:{port}/ingest")
            edit("stream.key_file", str(key))
            assert not state()["stream"]["active"], "setup started a broadcast"
            click_widget(714)
            acknowledge()
            capture("streaming-setup")
            click_widget(152)
            operate(2)
            click_widget(130)
            wait_until(lambda: state()["stream"]["state"] == "streaming", "local stream did not connect")
            assert state()["stream"]["paused"] and ui()["preview_target"] == "stream"
            capture("operate-stream-paused")
            click_widget(130)
            wait_until(lambda: not state()["stream"]["paused"], "Resume streaming failed")
            # Three lanes, privacy restoration and one-lane blur.
            cli("record", "start")
            cli("stream", "blur", "on")
            assert not state()["virtual"]["blurred"] and not state()["record"]["blurred"]
            cli("virtual", "pause")
            click_widget(34)
            wait_until(lambda: state()["group_paused"], "global privacy failed")
            click_widget(34)
            wait_until(lambda: not state()["group_paused"], "global restore failed")
            assert state()["virtual"]["paused"] and not state()["stream"]["paused"]
            assert state()["stream"]["blurred"] and not state()["record"]["paused"]
            click_widget(141)
            wait_until(lambda: state()["stream"]["state"] == "stopped", "Stop streaming failed")
            cli("record", "stop")
            ingest.wait(timeout=8)

            # Refused connection -> failed -> explicit retry, without revealing.
            cli("settings", "stream.reconnect_attempts", "0")
            click_widget(130)
            wait_until(lambda: state()["stream"]["state"] == "failed", "refused connection did not fail")
            capture("operate-stream-failed")
            click_widget(130)
            wait_until(lambda: state()["stream"]["state"] == "failed", "retry did not return failure")
            cli("stream", "stop")
            # Fluid width and pinned privacy controls at each shipped width.
            for width in (360, 440, 520, 800):
                xdo("windowsize", window, round(width*scale), round(760*scale))
                time.sleep(.15)
                navigate(3)
                pinned_accessible()
                capture(f"compose-overlays-{width}")
            # Disconnection preserves last-known state; reconnect restores controls.
            cli("quit")
            daemon.wait(timeout=8)
            wait_until(lambda: not ui()["connected"], "disconnect banner state did not appear")
            assert not widget(34)["enabled"]
            capture("daemon-disconnected")
            daemon = start_daemon()
            wait_until(lambda: ui()["connected"], "panel did not reconnect")
            before_close = state()
            xdo("key", "--clearmodifiers", "ctrl+q")
            assert panel.wait(timeout=8) == 0
            after_close = state()
            for lane in ("virtual", "record"):
                assert before_close[lane] == after_close[lane], "closing panel changed output state"
            assert daemon.poll() is None and after_close["stream"]["state"] == "stopped"
            cli("quit")
            daemon.wait(timeout=8)
        except Exception:
            log.flush()
            print("Panel evidence:", json.dumps(ui(), ensure_ascii=False), file=sys.stderr)
            print("Native log tail:\n" + "\n".join((root / "native.log").read_text().splitlines()[-50:]), file=sys.stderr)
            raise
        finally:
            for child in reversed(processes):
                if child.poll() is None:
                    child.terminate()
                try:
                    child.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=3)
            log.close()


if __name__ == "__main__":
    for dependency in ("xvfb-run", "xdotool", "xprop", "xclip", "ffmpeg"):
        if not shutil.which(dependency):
            raise SystemExit(f"native panel check requires {dependency}")
    if "--inside" not in sys.argv:
        result = subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 2048x2048x24",
                                 sys.executable, __file__, "--inside"], timeout=300)
        raise SystemExit(result.returncode)
    exercise()
    print("native panel: prototype lanes/tabs, nine workflows, atomic drafts, keyboard, privacy restore, private streaming, disconnect/reconnect, density and 360–800px pinned controls passed")
