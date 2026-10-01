# Verification record

This record distinguishes executable tests, read-only platform probes and pending
hardware acceptance. No conferencing or loopback-consumer compatibility is implied
by successful synthetic tests. Final results are updated after the full check run.

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
- `tests/test_wayland.c`: CPU buffer boundaries/formats, session revocation/capabilities
  and private mock D-Bus portal asynchronous consent cancellation.

## Available platform versus acceptance

This workspace has C/Xorg/FFmpeg/PipeWire development libraries, DISPLAY=:0,
Xvfb, and camera nodes. Loopback output and a real Wayland portal session must be
verified separately. Read-only probes and isolated Xvfb tests are recorded here as
agents finish them; unsupported features are reported through capabilities.

Run `make check` for Xorg and `make X11=0 WAYLAND=1 check` for the Wayland-only
build. `make sanitize` uses separate address/undefined sanitizer objects. Test-only
synthetic source/device/none-output options cannot establish integration compatibility.

## Hardware acceptance

The exact acceptance steps and measurement method are in
[hardware-acceptance.md](hardware-acceptance.md). Virtual camera consumer pixels,
real conferencing resolution/compression, real portal stream negotiation, user desktop
key layouts/IMEs and physical AV latency remain unverified unless listed with results.
