#!/usr/bin/env python3
"""Production CLI/socket streaming smoke, loopback only; no UI or input devices.

Run CAST_TEST_BINARY=/absolute/path/to/cast python3 tests/test_workspace_stream.py.
Requires ffmpeg/ffprobe with RTMP listening support. All children and media are owned
by this test; a TCP connection is deliberately never used to probe ingest readiness.
"""
import errno
import json
import math
import os
from pathlib import Path
import secrets
import shutil
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get("CAST_TEST_BINARY", ROOT / "cast")).resolve()


def wait(predicate, description, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.04)
    raise AssertionError(description)


def stop(child):
    if child is not None and child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=3)


def main():
    assert BINARY.is_file(), f"missing daemon binary: {BINARY}"
    assert shutil.which("ffmpeg") and shutil.which("ffprobe"), "requires ffmpeg and ffprobe"
    # /tmp keeps AF_UNIX paths below macOS's small sockaddr_un limit.
    with tempfile.TemporaryDirectory(prefix="cast-stream-", dir="/tmp") as directory:
        folder = Path(directory)
        home, runtime = folder / "home", folder / "run"
        home.mkdir(mode=0o700)
        runtime.mkdir(mode=0o700)
        secret = secrets.token_hex(24)
        key = folder / "key"
        with open(key, "x", opener=lambda p, flags: os.open(p, flags, 0o600)) as output:
            output.write(secret + "\n")
        assert key.stat().st_mode & 0o777 == 0o600
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        movie, config, sock = folder / "received.flv", folder / "test.conf", runtime / "cast.sock"
        config.write_text(
            "[output]\nbackend=synthetic\ndevice=none\nenabled=false\nwidth=320\nheight=180\nfps=10\n"
            "[camera]\nenabled=false\n[composition]\nlayout=screen\n[cursor]\nenabled=false\n"
            "[audio]\nmic=false\ndesktop=false\nvirtual=false\n"
            "[stream]\nservice=custom\n"
            f"server_url=rtmp://127.0.0.1:{port}/ingest\nkey_file={key}\n"
            "video_bitrate_kbps=300\naudio_bitrate_kbps=64\nreconnect_attempts=0\n")
        env = dict(os.environ, HOME=str(home), XDG_RUNTIME_DIR=str(runtime),
                   XDG_CONFIG_HOME=str(home / "config"), XDG_CACHE_HOME=str(home / "cache"),
                   XDG_DATA_HOME=str(home / "data"))
        for name in ("DISPLAY", "WAYLAND_DISPLAY", "PIPEWIRE_REMOTE", "PULSE_SERVER"):
            env.pop(name, None)
        base = [str(BINARY), "--config", str(config), "--socket", str(sock)]
        daemon = ingest = None
        replies = []
        with (folder / "daemon.log").open("w+") as daemon_log, (folder / "ingest.log").open("w+") as ingest_log:
            def command(*args):
                before = time.monotonic()
                result = subprocess.run(base + list(args), env=env, capture_output=True,
                                        text=True, timeout=5)
                elapsed = time.monotonic() - before
                replies.extend((result.stdout, result.stderr))
                assert secret not in result.stdout + result.stderr, "stream key leaked in CLI reply"
                assert result.returncode == 0, (args, result.stdout, result.stderr)
                assert elapsed < 2, ("unresponsive CLI", args, elapsed)
                return result.stdout.strip()

            def status():
                return json.loads(command("stream", "status", "--json"))

            try:
                ingest = subprocess.Popen(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                    "-y", "-listen", "1", "-i", f"rtmp://127.0.0.1:{port}/ingest",
                    "-c", "copy", "-f", "flv", str(movie)], stdout=ingest_log, stderr=ingest_log)

                def listening():
                    assert ingest.poll() is None, "loopback ingest exited before binding"
                    with socket.socket() as probe:
                        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                        try:
                            probe.bind(("127.0.0.1", port))
                        except OSError as error:
                            if error.errno == errno.EADDRINUSE:
                                return True
                            raise
                    return False

                wait(listening, "loopback ingest did not bind", 5)
                daemon = subprocess.Popen(base + ["--headless", "--backend", "synthetic", "--no-camera",
                    "--no-virtual", "--output-device", "none"], env=env,
                    stdout=daemon_log, stderr=daemon_log)
                wait(lambda: sock.exists() or daemon.poll() is not None, "daemon socket absent")
                assert daemon.poll() is None, "daemon exited during startup"
                initial = json.loads(command("status", "--json"))
                assert not initial["virtual"]["enabled"]
                for source in ("mic", "desktop", "virtual"):
                    assert not initial["audio"][source]["enabled"]
                command("stream", "start")
                connected = wait(lambda: (s if (s := status())["state"] == "streaming" else None),
                                 "stream did not connect")
                assert connected["paused"], "startup must remain privacy-paused"
                time.sleep(.5)
                command("stream", "resume")
                assert not status()["paused"]
                time.sleep(1)
                command("stream", "pause")
                paused = status()
                assert paused["paused"] and paused["active"]
                time.sleep(.7)
                assert status()["video_frames"] > paused["video_frames"], "privacy must keep media clock running"
                command("stream", "resume")
                time.sleep(1)
                healthy = status()
                assert healthy["video_frames"] >= 15 and healthy["audio_samples"] >= 48000
                assert healthy["bytes_written"] > 0 and not healthy["error"]
                command("stream", "stop")
                wait(lambda: status()["state"] == "stopped", "stream stop did not settle")
                wait(lambda: ingest.poll() is not None, "ingest did not finalize", 8)
                # ffmpeg can report EOF as an RTMP input error; decoded output is authoritative.
                probe = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-count_frames",
                    "-show_streams", "-show_packets", "-of", "json", str(movie)], text=True, timeout=15))
                streams = probe["streams"]
                video = next(s for s in streams if s["codec_type"] == "video")
                audio = next(s for s in streams if s["codec_type"] == "audio")
                assert video["codec_name"] == "h264" and audio["codec_name"] == "aac"
                assert (video["width"], video["height"]) == (320, 180)
                assert int(video["nb_read_frames"]) >= 15 and int(audio["nb_read_frames"]) >= 40
                ends = []
                for stream in (video, audio):
                    packets = [p for p in probe["packets"] if p["stream_index"] == stream["index"]]
                    stamps = [float(p["dts_time"]) for p in packets]
                    assert all(math.isfinite(t) for t in stamps)
                    assert all(b > a for a, b in zip(stamps, stamps[1:])), "nonmonotonic media clock"
                    assert stamps[-1] - stamps[0] >= 2
                    ends.append(stamps[-1])
                assert abs(ends[0] - ends[1]) < .5, "AAC/video clocks diverged"
                subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", str(movie),
                    "-map", "0:v:0", "-f", "null", "-"], check=True, capture_output=True, timeout=15)
                pcm = subprocess.check_output(["ffmpeg", "-nostdin", "-v", "error", "-i", str(movie),
                    "-map", "0:a:0", "-f", "f32le", "-"], timeout=15)
                assert pcm and all(abs(x[0]) < .0001 for x in struct.iter_unpack("<f", pcm)), "disabled sources emitted audio"
                command("quit")
                daemon.wait(timeout=8)
                print("Workspace daemon stream: loopback H.264/AAC, privacy/resume, clocks and redaction passed")
            finally:
                stop(daemon)
                stop(ingest)
                for log in (daemon_log, ingest_log):
                    log.flush()
                    log.seek(0)
                    contents = log.read()
                    assert secret not in contents, "stream key leaked in child log"


if __name__ == "__main__":
    main()
