# Verification record

This record distinguishes executable tests, read-only platform probes and pending
hardware acceptance. No conferencing or loopback-consumer compatibility is implied
by successful synthetic tests. Results below were collected on 2026-10-01.

## Automated tests

- `tests/test_core.c`: strict configuration, unknown/duplicate values and sections,
  presets, missing-file behavior, atomic parse rejection, output privacy transitions,
  idempotent group pause and independent overrides.
- `tests/test_visual.c`: owned frames, camera crop/geometry/clamping, transforms,
  layouts/masks, cursor and annotation placement using synthetic RGBA frames.
- `tests/test_media.c`: real FFmpeg encoded/muxed synthetic media, independent outputs,
  pause timeline, bounded queues, privacy barriers, slowdown and failure isolation.
- `tests/test_ipc.py`: actual foreground daemon/CLI, owned/stale socket/single instance,
  malformed/oversized/idle/concurrent peers, complete controls, paused/frozen outputs,
  recording same file/no overwrite, countdown cancellation, precedence and atomic reload.
- `tests/test_commands.c`: actual production command handler and daemon helpers without
  socket binding; precedence, atomic reload/restart rejection, privacy/freeze/group state,
  recording/countdown, argument errors, JSON routing/escaping and status size limits.
- `tests/test_wayland.c`: CPU buffer boundaries/formats, session revocation/capabilities
  and private mock D-Bus portal asynchronous consent cancellation.

## Available platform versus acceptance

The initial unrestricted environment exposed DISPLAY=:0 and camera nodes. Read-only
probes found an MJPEG-capable webcam and PipeWire 1.6.8; no loopback output was found.
After the user resumed, the managed sandbox no longer exposed camera nodes or permitted
display/PipeWire connection or Unix socket binding. Consequently test_ipc.py, Xvfb and
the private mock D-Bus portal cannot execute here (bind returns EPERM). The Xorg smoke
compiles warning-clean; it is not recorded as executed. Expanded D-Bus tests fail promptly
when permission is absent. CPU Wayland buffer/revocation/capability tests do execute.

Run `make check` for Xorg and `make X11=0 WAYLAND=1 check` for the Wayland-only
build. `make sanitize` uses separate address/undefined sanitizer objects. Test-only
synthetic source/device/none-output options cannot establish integration compatibility.

## Hardware acceptance

The exact acceptance steps and measurement method are in
[hardware-acceptance.md](hardware-acceptance.md). Virtual camera consumer pixels,
real conferencing resolution/compression, real portal stream negotiation, user desktop
key layouts/IMEs and physical AV latency remain unverified unless listed with results.


## Recorded independent results

`make X11=1 WAYLAND=1 check-unit` passed core, visual, production command and media
checks plus Wayland CPU tests. Address/undefined sanitizers passed those independent
checks with `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`; leak detection
cannot run under this environment's ptrace restrictions and is not claimed.

The media suite decodes real libx264/AAC Matroska files. Live+record and record-only
active timelines were approximately 1.06 seconds after repeated interruptions, with
AAC ends within about 20 ms; longest video intervals were 34–67 ms under test load.
Queue privacy contained zero queued green test frames and no sensitive test tone;
injected slow encoder work dropped bounded frames while status/live/stop/privacy
operations completed in under 1 ms. Injected ENOSPC preserved partial files and kept
live operational; subsequent recordings decoded successfully. These tests use
synthetic audio/frames and the explicit `none` test output, not a kernel consumer.

## Composition microbenchmark

`make benchmark` measures only RGBA composition, excluding capture, camera decoding,
audio, encoding, virtual-device I/O, preview and conference latency. It ran on Intel
Core Ultra 5 235U, Linux 7.1.10-arch1-1 x86_64, with GCC -O2, 1920x1080 overlay,
a 640x480 synthetic camera, and 90 iterations per case.

| Work per iteration | Median | p95 | Throughput | Peak RSS | CPU budget estimated at 30 fps |
|---|---:|---:|---:|---:|---:|
| One composition | 6.70 ms | 7.33 ms | 147.34/s | 19,792 KiB | 20.1% of one CPU |
| Two compositions, differing annotation paths | 13.53 ms | 18.88 ms | 68.77/s | 27,984 KiB | 43.2% of one CPU |

Equal effective annotations reuse one composed frame for live+record. The CPU budget
is an extrapolation from measured render CPU time; it is not a full-pipeline CPU
measurement. No glass-to-glass latency, sustained device FPS, consumer drops or actual
hardware conferencing performance is asserted. The hardware acceptance checklist
requires measuring those separately.

Further commits were prevented after resumption by the managed sandbox's read-only
.git mount. Source changes remain in the working tree; `make package` archives that
actual working tree, so the source checkpoint matches its binary independently of HEAD.

## Build and package checkpoint

Final Xorg-only, Wayland-only and combined builds passed with -Werror; the combined
binary is retained as ./cast. The final independent suite (including production
commands and shutdown privacy) passed address/undefined sanitizers after all fixes.
`cast config check examples/cast.conf` passed and groff rendered docs/cast.1.

Binary/source archives were created by make package. Their staged executable matched
./cast and ran --version; the extracted source src/ tree matched the working tree.
Staged uninstall removed the executable and example without touching user config.
Checksums are in dist/SHA256SUMS. No test cast daemon or recording remains running.
The code and these artifacts are safe to retain through a normal computer restart.

After creating /dev/video10 and restarting, run `./cast doctor`, `make check`,
`make check-xorg`, then the hardware-acceptance.md checks in the normal graphical
session. Full IPC, desktop capture/input/preview, real PipeWire routing, the actual
portal and conferencing consumers still need their recorded acceptance results.
