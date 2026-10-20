#!/usr/bin/env python3
"""Render the native workspace on a private Xvfb, never on the desktop."""
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
BINARY = ROOT / "build/cast-ui-preview"


def run_inside(captures):
    cases = [(f"{workspace}-{theme}", "1360x820", workspace, theme, False)
             for workspace in ("camera", "record", "stream") for theme in ("light", "dark")]
    cases += [
        ("narrow-camera", "480x360", "camera", "dark", False),
        ("medium-stream", "800x600", "stream", "light", False),
        ("settings-wide", "1360x820", "camera", "dark", True),
        ("settings-narrow", "480x360", "record", "light", True),
    ]
    for name, size, workspace, theme, settings in cases:
        capture = captures / f"{name}.bmp"
        command = [str(BINARY), "--size", size, "--workspace", workspace,
                   "--theme", theme, "--frames", "5", "--capture", str(capture),
                   "--debug-layout", "json"]
        if settings:
            command.append("--settings")
        result = subprocess.run(command, capture_output=True, text=True, timeout=20,
                                cwd=ROOT.parent)
        assert result.returncode == 0, (name, result.stderr)
        assert not result.stderr.strip(), (name, result.stderr)
        layout = json.loads(result.stdout)
        assert layout["boxes"], name
        for box in layout["boxes"]:
            assert all(math.isfinite(n) for n in box["bounds"] + box["clip"]), (name, box)
        data = capture.read_bytes()
        assert data[:2] == b"BM", name
        width, height = struct.unpack_from("<ii", data, 18)
        assert (width, abs(height)) == tuple(map(int, size.split("x"))), (name, width, height)

    # CLI validation must reject bad input before starting a native host.
    for args in (["--size", "479x360"], ["--size", "480x359"],
                 ["--size", "480x360oops"], ["--theme", "purple"],
                 ["--workspace", "invalid"], ["--frames", "0"],
                 ["--capture", "unbounded.bmp"]):
        result = subprocess.run([str(BINARY), *args], capture_output=True, text=True, timeout=5)
        assert result.returncode != 0 and result.stderr, args
    print(f"Native workspace: {len(cases)} SDL software captures and CLI validation passed")


def main():
    if sys.argv[1:] == ["--inside-private-xvfb"]:
        with tempfile.TemporaryDirectory(prefix="cast-ui-native-") as directory:
            run_inside(Path(directory))
        return 0
    if sys.argv[1:]:
        raise SystemExit("No arguments expected")
    if sys.platform != "linux":
        raise SystemExit("Native smoke uses Linux Xvfb; run the workspace manually on macOS")
    for dependency in ("xvfb-run", "xauth"):
        if not shutil.which(dependency):
            raise SystemExit(f"Native smoke requires {dependency}")
    env = dict(os.environ, SDL_RENDER_DRIVER="software", SDL_VIDEODRIVER="x11")
    return subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1600x1200x24",
                           sys.executable, str(Path(__file__).resolve()),
                           "--inside-private-xvfb"], env=env, timeout=180).returncode


if __name__ == "__main__":
    raise SystemExit(main())
