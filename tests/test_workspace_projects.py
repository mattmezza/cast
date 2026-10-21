#!/usr/bin/env python3
"""Production daemon CLI/socket project smoke; synthetic pixels, no UI/devices."""
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
BINARY = Path(os.environ.get("CAST_TEST_BINARY", ROOT / "cast")).resolve()


def wait(predicate, description, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.04)
    raise AssertionError(description)


def main():
    assert BINARY.is_file(), f"missing matching daemon binary: {BINARY}"
    assert shutil.which("ffprobe") and shutil.which("ffmpeg"), "requires ffprobe and ffmpeg for inspection"
    with tempfile.TemporaryDirectory(prefix="cast-workspace-projects-") as directory:
        folder = Path(directory)
        runtime = folder / "runtime"
        runtime.mkdir(mode=0o700)
        home = folder / "home"
        home.mkdir(mode=0o700)
        config, sock = folder / "test.conf", runtime / "cast.sock"
        config.write_text(
            "[output]\nbackend=synthetic\ndevice=none\nenabled=false\nwidth=640\nheight=360\nfps=10\n"
            "[camera]\nenabled=false\n[composition]\nlayout=screen\n[cursor]\nenabled=false\n"
            "[audio]\nmic=false\ndesktop=false\nvirtual=false\n"
            "[record]\ncountdown=0\nvideo_codec=ffv1\naudio_codec=flac\ncontainer=matroska\n"
            f"directory={folder}\n")
        env = dict(os.environ, HOME=str(home), XDG_RUNTIME_DIR=str(runtime),
                   XDG_CONFIG_HOME=str(home / "config"), XDG_CACHE_HOME=str(home / "cache"),
                   XDG_DATA_HOME=str(home / "data"))
        # Do not inherit display/session routing from a user's desktop.
        for name in ("DISPLAY", "WAYLAND_DISPLAY", "PIPEWIRE_REMOTE", "PULSE_SERVER"):
            env.pop(name, None)
        base = [str(BINARY), "--config", str(config), "--socket", str(sock)]
        daemon = None
        with (folder / "daemon.log").open("w+") as log:
            try:
                daemon = subprocess.Popen(base + ["--headless", "--backend", "synthetic",
                    "--no-camera", "--no-virtual", "--output-device", "none"],
                    env=env, stdout=log, stderr=log)

                def command(*args, success=True):
                    result = subprocess.run(base + list(args), env=env, capture_output=True,
                                            text=True, timeout=5)
                    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
                    return result.stdout.strip()

                def status():
                    return json.loads(command("status", "--json"))

                def project_done():
                    text = command("project", "status")
                    return text if "busy=0" in text else None

                wait(lambda: sock.exists() or daemon.poll() is not None, "daemon socket did not appear")
                assert daemon.poll() is None, "daemon exited during isolated startup"
                initial = status()
                assert not initial["virtual"]["enabled"]
                assert not initial["audio"]["mic"]["enabled"]
                assert not initial["audio"]["desktop"]["enabled"]
                assert not initial["audio"]["virtual"]["enabled"]
                # Monitoring is inert until explicitly enabled; a disabled mic
                # must be rejected before any output device worker is created.
                assert "state=0 capture_ready=0" in command("monitor", "status")
                command("monitor", "off")
                command("monitor", "on", success=False)
                assert "state=0 capture_ready=0" in command("monitor", "status")
                assert not status()["audio"]["mic"]["enabled"]
                command("project", "new")
                take, project, movie = folder / "take.mkv", folder / "test.cast", folder / "export.mkv"
                command("record", "start", str(take))
                time.sleep(.8)
                command("record", "stop")
                wait(lambda: not status()["record"]["finalizing"], "recording finalization timed out")
                assert take.is_file() and take.stat().st_size > 0
                metrics = wait(lambda: (value if (value := status()["performance"])["ready"] else None),
                               "daemon timing window not published")
                assert math.isfinite(metrics["tick_hz"]) and metrics["tick_hz"] > 0
                assert metrics["maximum_tick_ms"] >= metrics["average_tick_ms"] >= 0
                assert metrics["window_ns"] >= 1_000_000_000
                assert metrics["loop_drops"] >= 0 and metrics["recording_drops"] >= 0
                wait(lambda: "clips=1" in command("project", "status"), "captured take missing from project")
                command("project", "save", str(project))
                assert "failed" not in wait(project_done, "project save timed out").lower()
                assert project.stat().st_mode & 0o777 == 0o600
                command("project", "new")
                command("project", "open", str(project))
                assert "clips=1" in wait(project_done, "project open timed out")
                command("timeline", "import", str(take), "1")
                assert "clips=2" in wait(project_done, "media import timed out")
                # Retry only the explicit asynchronous worker-retirement condition.
                def seek():
                    result = subprocess.run(base + ["timeline", "seek", "100000000"], env=env,
                                            capture_output=True, text=True, timeout=5)
                    if result.returncode:
                        assert "previous preview cancelling" in result.stderr + result.stdout
                        return False
                    return True
                wait(seek, "timeline seek failed")
                command("timeline", "play", "silent")
                time.sleep(.2)
                command("timeline", "pause")
                command("timeline", "live")
                command("timeline", "relink", "18446744073709551614", str(take), success=False)
                command("timeline", "export", str(movie))

                def exported():
                    text = command("timeline", "export", "status")
                    state = int(re.search(r"state=(\d+)", text).group(1))
                    assert state not in (3, 4), text
                    return state == 2
                wait(exported, "timeline export did not complete", timeout=30)
                probe = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams",
                    "-show_format", "-of", "json", str(movie)], text=True, timeout=10))
                video = next(s for s in probe["streams"] if s["codec_type"] == "video")
                assert (video["width"], video["height"]) == (640, 360)
                assert float(probe["format"]["duration"]) > .1
                subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", str(movie),
                    "-map", "0:v:0", "-f", "null", "-"], check=True, capture_output=True, timeout=15)
                # No overwrite is an asynchronous failure; previous movie remains intact.
                before = movie.read_bytes()
                command("timeline", "export", str(movie))
                wait(lambda: "state=4 " in command("timeline", "export", "status"),
                     "existing export destination was not rejected")
                assert movie.read_bytes() == before
                print("Workspace daemon projects: record/save/open/import/review/relink/export passed")
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read()[-12000:])
                raise
            finally:
                if daemon is not None and daemon.poll() is None:
                    daemon.terminate()
                    try:
                        daemon.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        daemon.kill()
                        daemon.wait(timeout=3)


if __name__ == "__main__":
    main()
