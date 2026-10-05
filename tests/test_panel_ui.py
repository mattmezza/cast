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
import struct
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


def exercise(focused_setup_parent=False, focused_exclusion=False):
    with tempfile.TemporaryDirectory(prefix="cast-panel-redesign-") as directory:
        root = Path(directory)
        environment = os.environ.copy()
        environment.update(XDG_RUNTIME_DIR=directory, SDL_VIDEODRIVER="x11",
                           CAST_PANEL_UI_STATE=str(root / "ui.json"))
        scale = float(environment.get("SDL_VIDEO_X11_SCALING_FACTOR", "1"))
        config = root / "cast.conf"
        config.write_text(f"[output]\nenabled=false\n[record]\ndirectory={directory}\ncountdown=0\n")
        original_config = config.read_bytes()
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
            backend = "xorg" if focused_exclusion else "synthetic"
            return spawn(BINARY, *common, "--backend", backend, "--camera-device",
                         "synthetic", "--output-device", "none", "--width", "320",
                         "--height", "240", "--fps", "20")

        def start_panel():
            nonlocal window, last_ui
            (root / "ui.json").unlink(missing_ok=True)
            last_ui = {"frame": 0, "connected": False, "widgets": []}
            child = spawn(BINARY, *common, "panel")
            wait_until(lambda: ui()["frame"] and ui()["connected"], "native renderer did not initialize")
            window = xdo("search", "--onlyvisible", "--pid", child.pid, "--class", "CastPanel").splitlines()[0]
            xdo("windowfocus", "--sync", window)
            xdo("windowmove", window, 0, 0)
            return child

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
            wait_until(lambda: widget(identifier), f"control {identifier} unavailable")
            for _ in range(80):
                item = widget(identifier)
                if not item:
                    time.sleep(.04)
                    continue
                before = ui()["frame"]
                wait_until(lambda: ui()["frame"] >= before+3, "renderer stopped while reaching control")
                fresh = widget(identifier)
                if not fresh or fresh["box"] != item["box"]:
                    continue
                item = fresh
                x, y, width, height = item["box"]
                area = ui()["scroll"]
                pinned = (item["id"] in (5, 6, 7, 10, 11, 12, 22, 34, 99)
                          or 160 <= item["id"] <= 166
                          or 400 <= item["id"] < 500 or 700 <= item["id"] <= 717
                          or 6000 <= item["id"] <= 6500
                          or ui().get("open_menu", 0) and bool(item["key"]))
                if pinned or y >= area[1] and y+height <= area[1]+area[3]:
                    wait_until(lambda: widget(identifier) and widget(identifier)["enabled"],
                               f"visible control {identifier} unavailable")
                    xdo("mousemove", "--window", window, round((x+width/2)*scale),
                        round((y+height/2)*scale), "click", 1)
                    time.sleep(.08)
                    return
                xdo("mousemove", "--window", window, round((area[0]+area[2]/2)*scale),
                    round((area[1]+area[3]/2)*scale), "click", 4 if y < area[1] else 5)
                time.sleep(.06)
            raise AssertionError(f"control {identifier} could not be reached by scrolling")

        def navigate(section):
            xdo("key", "--clearmodifiers", f"alt+{section+1}")
            wait_until(lambda: ui()["tab"] == 1 and ui()["open_section"] == section,
                       "Compose page did not open")
            assert widget(99), "Compose page has no Back control"
            assert not any(200 <= item["id"] < 300 for item in ui()["widgets"]), "nested group buttons remain"
            assert not any(100 <= item["id"] <= 105 for item in ui()["widgets"]), "Compose list remained beside page"

        def operate(lane):
            if ui()["tab"] != 0:
                click_widget(5)
            wait_until(lambda: ui()["tab"] == 0, "Operate did not open")
            assert not any(widget(i) for i in (10, 11, 12)), "redundant header chips remain"

        def all_settings(section):
            navigate(section)
            if not ui()["all_settings"][section]:
                click_widget(180+section)

        def drag_geometry_slider(key, unchanged_draft):
            acknowledge()
            for _ in range(60):
                item = widget(key)
                x, y, width, height = item["box"]
                area = ui()["scroll"]
                if y >= area[1] and y+height <= area[1]+area[3]:
                    break
                wheel(4 if y < area[1] else 5, 1)
            else:
                raise AssertionError(f"slider {key} could not be reached")
            assert item["type"] == 5, "geometry field is not a pointer slider"
            before = item["value"]
            queued = ui()["command_queued"]
            xdo("mousemove", "--window", window, round((x+width*.2)*scale),
                round((y+height/2)*scale), "mousedown", 1)
            try:
                xdo("mousemove", "--window", window, round((x+width*.7)*scale),
                    round((y+height/2)*scale))
                wait_until(lambda: widget(key)["dirty"] and widget(key)["draft"] != before,
                           "slider drag did not update local geometry draft")
                time.sleep(.2)  # Several authoritative snapshot polls while the pointer is held.
                assert widget(key)["value"] == before, "slider changed daemon before release"
                assert ui()["command_queued"] == queued, "slider sent IPC while pointer held"
                assert widget(unchanged_draft)["dirty"], "drag discarded another field draft"
            finally:
                xdo("mouseup", 1)
            wait_until(lambda: ui()["command_queued"] == queued+1,
                       "slider release did not send exactly one setting")
            acknowledge()
            wait_until(lambda: widget(key)["value"] != before and not widget(key)["dirty"],
                       "slider release did not acknowledge geometry")
            assert widget(unchanged_draft)["dirty"], "geometry ack discarded another draft"
            cli("settings", key, before)
            wait_until(lambda: widget(key)["value"] == before, "CLI geometry reset did not synchronize")

        def edit(key, value, apply=False):
            click_widget(key)
            if widget(6500):
                click_widget(6500)
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
            if focused_setup_parent:
                return
            destination = os.environ.get("CAST_PANEL_TEST_SCREENSHOTS")
            if destination:
                Path(destination).mkdir(parents=True, exist_ok=True)
                subprocess.run(["import", "-window", window,
                                str(Path(destination) / (name+".png"))], env=environment, check=True)

        def pinned_accessible():
            bounds = dict(line.split("=", 1) for line in
                          xdo("getwindowgeometry", "--shell", window).splitlines() if "=" in line)
            width, height = int(bounds["WIDTH"])/scale, int(bounds["HEIGHT"])/scale
            for identifier in (5, 6, 7, 22, 34):
                item = widget(identifier)
                assert item, identifier
                x, y, w, h = item["box"]
                assert x >= 0 and y >= 0 and x+w <= width+1 and y+h <= height+1, (identifier, item, bounds)
                if identifier in (7, 22, 34):
                    assert h == 36, ("privacy/preview height", item)
                if identifier == 7:
                    assert item["enabled"], ("Close button unavailable", item)
            assert abs(widget(22)["box"][2]-widget(34)["box"][2]) < 1, "meta-actions have unequal widths"
            close, preview = widget(7)["box"], widget(22)["box"]
            assert close[1] == preview[1] and close[0] >= preview[0]+preview[2], "Close is not beside Preview"
            assert close[2] == 76 and preview[2] >= 110, "Close displaced preview state text"
            status = ui()["status_bar"]
            assert status[3] == 46, "status rows changed height"
            assert status[0] >= 0 and status[0]+status[2] <= width+1
            assert status[1] >= 0 and status[1]+status[3] <= height+1, "footer extends outside window"
            for row in ui()["status_rows"]:
                assert row[3] == 16, ("status row wrapped", row)
                assert row[0] >= status[0] and row[0]+row[2] <= status[0]+status[2]+1, (row, status)
                assert row[1] >= status[1] and row[1]+row[3] <= status[1]+status[3]+1, (row, status)
                assert abs(row[0]+row[2]/2-(status[0]+status[2]/2)) < 1, ("status row not centered", row, status)
            assert "has_preview" not in ui() and "preview_panel" not in ui()

        def pinned_draft(section):
            identifier = 700 + section*2
            wait_until(lambda: widget(identifier) and widget(identifier)["enabled"], "dirty page has no Apply")
            area = ui()["scroll"]
            for button_id in (identifier, identifier+1):
                item = widget(button_id)
                assert item and item["enabled"], button_id
                x, y, width, height = item["box"]
                assert y >= area[1]+area[3]-1, ("draft action scrolls with body", item, area)
            return tuple(widget(identifier)["box"])

        def wheel(button, repeat):
            area = ui()["scroll"]
            xdo("mousemove", "--window", window, round((area[0]+area[2]/2)*scale),
                round((area[1]+area[3]/2)*scale), "click", "--repeat", repeat, "--delay", 25, button)
            time.sleep(.2)

        def compose_list():
            wait_until(lambda: ui()["tab"] == 1 and ui()["open_section"] == -1, "Compose list did not open")
            assert all(widget(100+section) for section in range(6)), "Compose list is incomplete"
            assert not widget(99) and not any(item["key"] for item in ui()["widgets"]), "list contains page controls"

        try:
            daemon = start_daemon()
            wait_until(lambda: (root / "daemon.sock").exists(), "synthetic daemon did not start")
            initial = state()
            panel = start_panel()
            assert state()["virtual"] == initial["virtual"]
            assert state()["record"] == initial["record"] and state()["stream"]["state"] == "stopped"
            assert ui()["density"] == scale and ui()["raster_font_size"] == 16*scale
            properties = subprocess.run(["xprop", "-id", window, "WM_CLASS", "_NET_WM_WINDOW_TYPE"],
                                        env=environment, text=True, capture_output=True, check=True).stdout
            assert '"cast-panel", "CastPanel"' in properties and "_NET_WM_WINDOW_TYPE_UTILITY" in properties
            pinned_accessible()
            assert widget(30) and widget(32) and widget(150), "start actions are hidden"
            capture("operate-task-cards")
            if focused_exclusion:
                navigate(0)
                click_widget(180)
                initial_mask = state()["source"]["mask_color"]
                # Hex validation remains local and cannot silently clamp or mutate capture.
                edit("capture.mask_color", "#zzzzzz")
                xdo("key", "--clearmodifiers", "Escape")
                pinned_draft(0)
                queued_before = ui()["command_queued"]
                click_widget(700)
                wait_until(lambda: "#RRGGBB" in ui()["error"], "invalid color feedback missing")
                assert ui()["command_queued"] == queued_before
                assert state()["source"]["mask_color"] == initial_mask
                edit("capture.mask_color", "#314159")
                xdo("key", "--clearmodifiers", "Escape")
                click_widget(700)
                wait_until(lambda: state()["source"]["mask_color"] == "#314159", "mask color did not apply")
                # Matrix writes one draft and uses the single Annotations Apply.
                navigate(4)
                assert state()["capabilities"]["input"], "Xorg fixture lacks input annotations"
                previous = widget("annotations.virtual_clicks")["value"]
                click_widget(8700)
                pinned_draft(4)
                assert widget("annotations.virtual_clicks")["value"] == previous
                click_widget(708)
                acknowledge()
                wait_until(lambda: widget("annotations.virtual_clicks")["value"] != previous,
                           "matrix cell did not apply")
                cli("annotations", "virtual", "clicks", "on" if previous == "true" else "off")
                wait_until(lambda: widget("annotations.virtual_clicks")["value"] == previous,
                           "matrix did not synchronize authoritative CLI edit")
                capture("annotations-matrix-xorg")
                assert config.read_bytes() == original_config
                click_widget(7)
                assert panel.wait(timeout=8) == 0
                cli("quit")
                daemon.wait(timeout=8)
                return
            xdo("key", "--clearmodifiers", "2")
            compose_list()
            capture("compose-list")
            for section in range(6):
                navigate(section)
                capture(f"compose-section-{section}-essentials")
            xdo("key", "--clearmodifiers", "Escape")
            compose_list()
            for identifier, section in ((160, 0), (161, 0)):
                click_widget(identifier)
                wait_until(lambda: ui()["open_section"] == section, "status link opened wrong page")
            for width in (360, 440, 520, 800):
                xdo("windowsize", window, round(width*scale), round(760*scale))
                time.sleep(.15)
                pinned_accessible()
            xdo("windowsize", window, round(440*scale), round(760*scale))
            wait_until(lambda: ui()["status_bar"][2] == 440, "resize did not settle")
            if focused_setup_parent:
                operate(2)
                click_widget(150)
                wait_until(lambda: ui()["stream_setup"], "setup did not open")
                parent_tab=ui()["tab"]
                click_widget(7)
                assert panel.wait(timeout=8)==0
                panel=start_panel()
                wait_until(lambda: ui()["stream_setup"] and ui()["tab"]==parent_tab, "setup restore failed")
                xdo("key", "--clearmodifiers", "Escape")
                wait_until(lambda: not ui()["stream_setup"] and ui()["tab"]==0, "setup parent lost")
                click_widget(7)
                assert panel.wait(timeout=8)==0
                cli("quit")
                daemon.wait(timeout=8)
                return

            # Call join: explicit reveal, independent freeze/blur/pause precedence.
            operate(0)
            click_widget(30)
            wait_until(lambda: state()["virtual"]["enabled"], "one-click virtual start failed")
            assert state()["virtual"]["paused"], "virtual camera start revealed composition"
            click_widget(30)
            wait_until(lambda: not state()["virtual"]["paused"], "Resume virtual camera failed")
            # A genuinely stopped daemon makes Pending observable without slowing rendering.
            os.kill(daemon.pid, signal.SIGSTOP)
            try:
                click_widget(31)
                wait_until(lambda: ui().get("command_pending"), "Pending feedback missing", timeout=1)
                assert widget(7)["enabled"], "Close disabled while waiting for daemon"
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
            click_widget(8601)
            wait_until(lambda: widget("record.countdown"), "Recording settings menu did not open")
            assert not widget("record.countdown")["enabled"], "countdown editable during recording"
            assert not widget("record.directory")["enabled"], "destination editable during recording"
            capture("record-overflow-locked")
            xdo("key", "--clearmodifiers", "Escape")
            wait_until(lambda: not widget("record.countdown"), "Escape did not close recording menu")
            click_widget(39)
            wait_until(lambda: state()["record"]["cut"], "Cut time failed")
            capture("operate-record-cut")
            held_duration = state()["record"]["duration"]
            time.sleep(.25)
            assert abs(state()["record"]["duration"]-held_duration) < .02, "cut advanced media time"
            click_widget(33)
            wait_until(lambda: not state()["record"]["cut"], "same-file resume failed")
            assert state()["record"]["path"] == path
            wait_until(lambda: state()["record"]["duration"] > held_duration+.1,
                       "resumed recording clock did not advance")
            click_widget(140)
            wait_until(lambda: state()["record"]["state"] == "stopped" and not state()["record"]["finalizing"], "recording finalization failed")
            click_widget(8601)
            edit("record.directory", str(root)+"/.")
            pinned_draft(8)
            capture("record-overflow-dirty")
            click_widget(716)
            acknowledge()
            wait_until(lambda: not widget(716), "recording overflow Apply did not clear draft")
            assert ui()["open_menu"] == 2, "recording Apply closed the overflow"
            xdo("key", "--clearmodifiers", "Escape")
            cli("settings", "record.countdown", "3")
            click_widget(32)
            wait_until(lambda: ui()["countdown"], "countdown not visible")
            capture("operate-countdown")
            click_widget(35)
            wait_until(lambda: not ui()["countdown"], "Cancel did not cancel countdown")
            cli("settings", "record.countdown", "0")

            # Layout diagrams and dropdowns edit the existing settings path.
            navigate(0)
            click_widget(8501)  # Stage, in canonical Overlay/Stage/Split/Screen/Camera order.
            pinned_draft(0)
            click_widget(700)
            acknowledge()
            all_settings(0)
            fit_before = widget("composition.fit")["value"]
            click_widget("composition.fit")
            capture("combo-popup")
            xdo("key", "--clearmodifiers", "Down", "Return")
            wait_until(lambda: widget("composition.fit")["dirty"], "keyboard dropdown did not stage selection")
            assert widget("composition.fit")["value"] == fit_before, "dropdown bypassed draft Apply"
            click_widget(700)
            acknowledge()
            assert widget("composition.fit")["value"] != fit_before, "dropdown Apply failed"
            # Background reveals only the current mode's fields.
            navigate(2)
            mode_base = widget("screen.background")["id"]
            click_widget(mode_base+1)  # Gradient.
            wait_until(lambda: widget("background.gradient_from"), "Gradient fields missing")
            assert not widget("background.source") and not widget("screen.background_color")
            click_widget(mode_base+2)  # Solid.
            wait_until(lambda: widget("screen.background_color"), "Solid field missing")
            assert not widget("background.gradient_from") and not widget("background.source")
            click_widget("screen.background_color")
            wait_until(lambda: widget(6500), "color picker hex editor missing")
            capture("color-popup")
            xdo("key", "--clearmodifiers", "Escape")
            click_widget(mode_base)  # Blurred.
            wait_until(lambda: widget("background.source"), "Blurred source missing")
            assert not widget("background.gradient_from") and not widget("screen.background_color")
            click_widget(705)
            all_settings(2)
            edit("screen.margin", "19")
            drag_geometry_slider("screen.width_percent", "screen.margin")
            click_widget(705)

            operate(3)
            click_widget(8603)
            wait_until(lambda: widget("audio.mic_source"), "Audio settings overflow did not open")
            capture("audio-overflow")
            xdo("key", "--clearmodifiers", "Escape")

            # Spatial controls are local drafts; Apply/Revert preserve the page.
            navigate(1)
            original_anchor=ui()["camera_anchor"]
            click_widget("camera.anchor")
            drag_geometry_slider("camera.width_percent", "camera.anchor")
            pinned_draft(1)
            assert ui()["camera_anchor"]==original_anchor, "anchor draft applied without Apply"
            click_widget(703)
            wait_until(lambda: not widget(702), "Revert kept draft bar")
            capture("compose-camera")
            # The eight-point grid is reachable by arrows and changes only a draft.
            click_widget(8400)
            xdo("key", "--clearmodifiers", "Right", "Return")
            pinned_draft(1)
            assert ui()["camera_anchor"] == original_anchor
            click_widget(702)
            acknowledge()
            wait_until(lambda: ui()["camera_anchor"] == "top", "grid keyboard selection failed")
            all_settings(1)
            original_radius=widget("camera.radius")["value"]
            edit("camera.radius", "37")
            click_widget(181)
            assert not widget("camera.radius") and widget(702), "Essentials discarded advanced draft"
            click_widget(181)
            assert widget("camera.radius")["draft"] == "37", "All settings forgot draft"
            pinned_box=pinned_draft(1)
            wheel(5, 8)
            assert tuple(widget(702)["box"])==pinned_box, "draft bar scrolled"
            click_widget(702)
            wait_until(lambda: not widget(702), "Apply did not clear draft")
            assert ui()["open_section"]==1, "Apply changed page"
            capture("compose-camera-all")
            navigate(5)
            edit("output.pause_text", "Private session")
            pinned_draft(5)
            assert state()["virtual"]["message"]!="Private session"
            click_widget(710)
            wait_until(lambda: state()["virtual"]["message"]=="Private session", "Pause title did not apply")
            click_widget(185)
            unicode_footer="Back soon · café ☕ — {time:%H:%M}"
            edit("output.pause_footer", "")
            subprocess.run(["xclip", "-selection", "clipboard"], input=unicode_footer,
                           env=environment, text=True, stdout=log, stderr=log, check=True)
            wait_until(lambda: ui()["clipboard_text_available"], "clipboard unavailable")
            xdo("key", "--clearmodifiers", "ctrl+v")
            wait_until(lambda: ui()["edit_text"]==unicode_footer, "UTF-8 paste failed")
            xdo("key", "--clearmodifiers", "Return")
            acknowledge()
            wait_until(lambda: widget("output.pause_footer")["value"]==unicode_footer, "footer Apply failed")
            capture("compose-pause-blur")

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
            xdo("key", "--clearmodifiers", "Escape")
            wait_until(lambda: not ui()["stream_setup"], "Escape did not dismiss setup first")
            click_widget(150)
            wait_until(lambda: ui()["stream_setup"], "Streaming setup did not reopen")
            edit("stream.server_url", f"rtmp://127.0.0.1:{port}/ingest")
            edit("stream.key_file", str(key))
            assert not state()["stream"]["active"], "setup started a broadcast"
            pinned_draft(7)
            capture("streaming-setup-dirty")
            click_widget(714)
            acknowledge()
            capture("streaming-setup")
            xdo("key", "--clearmodifiers", "ctrl+q")
            assert panel.wait(timeout=8) == 0
            panel = start_panel()
            wait_until(lambda: ui()["stream_setup"], "process restart forgot the setup sheet")
            xdo("key", "--clearmodifiers", "Escape")
            wait_until(lambda: not ui()["stream_setup"] and ui()["tab"] == 0  ,
                       "restored setup sheet forgot its parent")
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
            # Close owns only the panel process, including while both media lanes run.
            before_close = state()
            preview_before_close = ui()["preview_enabled"]
            click_widget(7)
            assert panel.wait(timeout=8) == 0
            after_close = state()
            assert after_close["stream"]["state"] == "streaming" and after_close["stream"]["blurred"]
            assert after_close["record"]["state"] == "recording" and after_close["record"]["path"] == before_close["record"]["path"]
            assert after_close["virtual"] == before_close["virtual"] and daemon.poll() is None
            panel = start_panel()
            assert ui()["preview_enabled"] == preview_before_close, "Close changed preview state"
            click_widget(141)
            wait_until(lambda: state()["stream"]["state"] == "stopped", "Stop streaming failed")
            cli("record", "stop")
            ingest.wait(timeout=8)

            # Refused connection -> failed -> explicit retry, without revealing.
            cli("settings", "stream.reconnect_attempts", "0")
            click_widget(130)
            wait_until(lambda: state()["stream"]["state"] == "failed", "refused connection did not fail")
            wait_until(lambda: ui()["stream_status"] == "Failed", "failure did not reach the panel")
            capture("operate-stream-failed")
            xdo("windowsize", window, round(360*scale), round(760*scale))
            time.sleep(.15)
            pinned_accessible()
            capture("operate-stream-failed-360")
            completed = ui()["command_completed"]
            click_widget(130)
            wait_until(lambda: ui()["command_completed"] > completed, "retry was not acknowledged")
            wait_until(lambda: state()["stream"]["state"] == "failed", "retry did not return failure")
            cli("stream", "stop")
            wait_until(lambda: ui()["stream_status"] == "Ready", "stopped stream layout did not update")
            # A private TCP listener that withholds RTMP handshake bytes keeps
            # Connecting observable without broadcasting or changing the backend.
            with socket.socket() as stalled:
                stalled.bind(("127.0.0.1", 0))
                stalled.listen(1)
                cli("settings", "stream.server_url",
                    f"rtmp://127.0.0.1:{stalled.getsockname()[1]}/ingest",
                    "stream.connect_timeout_ms", "10000")
                wait_until(lambda: ui()["stream_status"] == "Ready", "configured stream layout did not update")
                click_widget(130)
                wait_until(lambda: ui()["stream_status"] == "Connecting", "stalled handshake did not show Connecting")
                assert state()["stream"]["paused"], "connecting session revealed composition"
                pinned_accessible()
                capture("operate-stream-connecting-360")
                cli("stream", "stop")
                wait_until(lambda: state()["stream"]["state"] == "stopped", "connecting stream did not stop")
            # Fluid width and pinned privacy controls at each shipped width.
            for width in (360, 440, 520, 800):
                xdo("windowsize", window, round(width*scale), round(760*scale))
                time.sleep(.15)
                navigate(3)
                pinned_accessible()
                capture(f"compose-overlays-{width}")

            # A long page keeps dirty actions pinned; process restart remembers
            # navigation/scroll only, discarding drafts and leaving config untouched.
            xdo("windowsize", window, round(440*scale), round(760*scale))
            navigate(1)
            original_radius = widget("camera.radius")["value"]
            edit("camera.radius", "37" if original_radius != "37" else "38")
            pinned_box = pinned_draft(1)
            wheel(5, 12)
            assert ui()["scroll_offset"] < -20, "long Camera page did not scroll"
            assert tuple(widget(702)["box"]) == pinned_box, "Apply moved with long page"
            capture("compose-camera-long-dirty")
            click_widget(703)
            wait_until(lambda: not widget(702), "Revert did not remove pinned draft bar")
            edit("camera.radius", "37" if original_radius != "37" else "38")
            wheel(4, 40)
            wait_until(lambda: abs(ui()["scroll_offset"]) < 1, "Camera page did not return to top")
            wheel(5, 2)
            remembered_scroll = ui()["scroll_offset"]
            assert remembered_scroll < -20
            before_close = state()
            xdo("key", "--clearmodifiers", "ctrl+q")
            assert panel.wait(timeout=8) == 0
            panel = start_panel()
            wait_until(lambda: ui()["tab"] == 1 and ui()["open_section"] == 1,
                       "panel process restart forgot the page")
            wait_until(lambda: abs(ui()["scroll_offset"]-remembered_scroll) < 2,
                       "panel process restart forgot scroll position")
            assert widget("camera.radius") and not widget("camera.radius")["dirty"]
            assert state()["virtual"] == before_close["virtual"] and state()["record"] == before_close["record"]
            assert config.read_bytes() == original_config, "panel wrote the config file"
            # Disconnection preserves last-known state; reconnect restores controls.
            cli("quit")
            daemon.wait(timeout=8)
            wait_until(lambda: not ui()["connected"], "disconnect banner state did not appear")
            assert not widget(34)["enabled"]
            assert widget(7)["enabled"], "Close disabled while disconnected"
            capture("daemon-disconnected")
            daemon = start_daemon()
            wait_until(lambda: ui()["connected"], "panel did not reconnect")
            wait_until(lambda: ui()["tab"] == 0 and ui()["open_section"] == -1,
                       "new daemon session inherited prior navigation memory")
            before_close = state()
            click_widget(7)
            assert panel.wait(timeout=8) == 0
            after_close = state()
            for lane in ("virtual", "record"):
                assert before_close[lane] == after_close[lane], "closing panel changed output state"
            assert daemon.poll() is None and after_close["stream"]["state"] == "stopped"
            assert config.read_bytes() == original_config, "panel wrote the config file"
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
            # Remove only this isolated fixture's POSIX navigation memory object.
            memory_hash = 5381
            mask = (1 << (struct.calcsize("L")*8))-1
            for byte in os.fsencode(root / "daemon.sock"):
                memory_hash = (memory_hash*33+byte) & mask
            Path(f"/dev/shm/cast-panel-flow-view-{os.getuid()}-{memory_hash:x}").unlink(missing_ok=True)


if __name__ == "__main__":
    for dependency in ("xvfb-run", "xdotool", "xprop", "xclip", "ffmpeg"):
        if not shutil.which(dependency):
            raise SystemExit(f"native panel check requires {dependency}")
    if "--inside" not in sys.argv:
        result = subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 2048x2048x24",
                                 sys.executable, __file__, "--inside", *sys.argv[1:]], timeout=300)
        raise SystemExit(result.returncode)
    focused = "--setup-parent-only" in sys.argv
    exclusion = "--exclusion-only" in sys.argv
    exercise(focused_setup_parent=focused, focused_exclusion=exclusion)
    if focused:
        print("native panel: streaming setup parent/view/reopen passed")
    elif exclusion:
        print("native Xorg panel: mask color popup validation/Apply, annotation matrix drafts/Apply and authoritative CLI synchronization passed")
    else:
        print("native panel: nine workflows, Compose pages/back/keys, pinned drafts, session navigation memory, compact status/header, privacy restore, private streaming, disconnect/reconnect and 360–800px density passed")
