# Verification record

This record distinguishes executable tests, read-only platform probes and pending
hardware acceptance. Synthetic tests alone do not establish conferencing or hardware compatibility.
Initial results were collected on 2026-10-01; post-reboot integration was performed
on 2026-10-02 in the normal graphical session.

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
display/PipeWire connection or Unix socket binding. That earlier sandbox blocked test_ipc.py, Xvfb and the private mock D-Bus portal
(bind returned EPERM). After reboot, unrestricted access was restored and all three
integrations executed successfully. These were temporary environment restrictions.

Run `make check` for Xorg and `make X11=0 WAYLAND=1 check` for the Wayland-only
build. `make sanitize` uses separate address/undefined sanitizer objects. Test-only
synthetic source/device/none-output options cannot establish integration compatibility.

## Hardware acceptance

The exact acceptance steps and measurement method are in
[hardware-acceptance.md](hardware-acceptance.md). The FFmpeg V4L2 consumer and real PipeWire routing checks below now have hardware
evidence. Conference resolution/compression, real Wayland portal streams, user desktop
key layouts/IMEs and glass-to-glass latency remain unverified.


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

The earlier managed sandbox prevented commits through a read-only .git mount.
After reboot, the saved changes were committed in 0f6c5d5 and subsequent integration
fixes were committed separately. GitHub release artifacts build from the exact tagged
commit; local make package continues to archive the working tree.

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

## Post-reboot integration

The user loaded v4l2loopback with devices=1, video_nr=10, card_label=cast and
exclusive_caps=1. Read-only doctor confirmed Xorg eDP-1 (1920x1200), MIT-SHM,
XI2/XComposite, the MJPEG/YUYV integrated webcam, writable /dev/video10, software
encoders and PipeWire sources. Baseline: Intel Core Ultra 5 235U, Arch Linux
7.2.8-arch1-1, PipeWire 1.6.9 and FFmpeg 9.0.2 (package 2:9.0.2-1).

- make X11=1 WAYLAND=1 check passed the complete unit, media, production commands,
  Unix socket IPC and private mock-portal lifecycle suite.
- make X11=1 WAYLAND=1 check-xorg passed all isolated Xvfb capture/window/selection,
  input/layout/privacy and preview-exclusion checks, with MIT-SHM and XGetImage.
  This found and fixed black redirected-window images: image masks can be zero;
  conversion now uses the source window's actual visual.
- make X11=1 WAYLAND=1 check-loopback LOOPBACK_DEVICE=/dev/video10 passed at
  1920x1080/30 using synthetic sources through the real kernel device and FFmpeg
  consumer. Consumer pixels verified startup neutral, distinct resumed content,
  stable freeze, pause replacing freeze, unfreeze retaining pause and group pause.
  The same-file recording decoded 55 frames after repeated pauses. The test waits
  for a fresh cadence tick; it does not measure buffered conference-frame latency.
- A separate physical Xorg/webcam/PipeWire run opened the MJPEG camera, selected
  the named microphone and created a temporary virtual source. A pw-cat consumer
  received 24,000 stereo float frames for each privacy check; sample peaks were
  exactly zero during startup pause, live freeze and group pause. Both mic and
  virtual routes reported ready. FFmpeg decoded real H.264 video and 48 kHz AAC
  audio after independent and group pauses. Temporary user media was removed.

The initial simultaneous 1080p30 run exposed poor throughput: 265 recorded frames
in 35.158 seconds (about 7.5 fps), 1,070 reported drops, 117% of one CPU and RSS
138–194 MiB over a 44.84-second sample. A remembered initial webcam-freshness warning
remained in last_error. Profiling identified per-pixel Xorg channel normalization
and decoding every queued MJPEG camera frame. Subsequent measurements below document
the fixes; the initial run is not a successful 30 fps performance baseline.

## Capture performance fixes

Owned-frame measurements do not save or display desktop pixels. The Xorg test
now offers a read-only --benchmark mode. For 40 actual eDP-1 captures at 1920x1200,
channel conversion improved from 50.88 ms/frame (19.65 fps) to 8.86 ms/frame
(112.87 fps). Common 32-bit visuals use direct byte conversion; other visuals
normalize masks once per frame. Synthetic checks cover both byte orders, RGB565,
packed 24-bit pixels, padded rows and visual masks independent of XImage masks.
Both isolated MIT-SHM and forced XGetImage exercises pass after optimization.

The webcam path now drains bounded ready buffers and decodes only the newest one.
At 33 ms call spacing, average/max camera work changed from 15.34/51.03 ms to
14.97/31.53 ms; at 100 ms spacing it changed from 41.95/55.99 ms to 24.27/31.91 ms.
Unique frames improved from 16.21 to 18.46 fps in the 33 ms probe. A separate raw
V4L2 request for MJPEG 1920x1080/30 delivered 20.02 fps. Read-only inspection found
auto_exposure=AperturePriority and exposure_dynamic_framerate=1, so this physical
camera can reduce its cadence for exposure. Device controls were preserved.
The output cadence can reuse camera frames; unique camera fps and output fps are
different measurements. A physical regression also verified that queued frames
predating a privacy boundary are rejected.

## v0.1 camera, preview and pipeline checks

Real V4L2 polling/decoding now runs in a separate worker with a bounded latest-frame
handoff. Epochs reject in-flight older work; per-buffer queue epochs reject late
unknown/malformed timestamps even after the ready-buffer drain. Worker tests passed
ASan/UBSan/LeakSanitizer in isolation, including bounded shutdown. Full media leak
checking reported allocations inside PipeWire modules, so dependency-wide leak
freedom is not claimed. The full optimized combined check and both Xvfb capture
paths passed after the worker and preview changes.

The preview stays mapped as an unmanaged floating window (WM_CLASS cast-preview /
CastPreview), supports header dragging, and clears to neutral across privacy epochs.
Its geometric overlap is neutral-masked in root/region frames; named application
pixmaps exclude it without that mask. Tests verify no map/unmap notifications across
capture or output state changes, exact opaque mask pixels including borders, moved/
resized/clipped geometry and privacy-safe worker publication. No tiling rule is needed.

The final kernel-loopback test also verified the asymmetric synthetic screen's
left-to-right orientation before and after camera mirror on/off. The screen remains
unmirrored in actual FFmpeg consumer pixels. The user reported selecting cast in a
Google Meet call and observing a mirrored self-view; remote orientation, negotiated
resolution and text readability have not been independently confirmed.

Final hardware samples used eDP-1 1920x1200, physical MJPEG webcam /dev/video0, explicit
physical microphone, an optional virtual source, 1920x1080/30 output, libx264 veryfast
CRF 23 with AAC 48 kHz, and FFmpeg V4L2 consumption. Each CPU/drop observation lasts
15 seconds after initial setup; decoded recording counts include brief setup/stop
intervals, so their durations are slightly longer. No user media was retained.

| Mode | Consumer fps | Decoded recording fps | CPU (% of one core) | RSS start/end (KiB) | Drops during sample | Deprecated-format warnings |
|---|---:|---:|---:|---:|---:|---:|
| Live only | 30.00 (450 frames) | — | 117.80 | 111,536 / 128,456 | 0 | 0 |
| Recording only | — | 29.93 (456 frames / 15.234 s) | 188.93 | 155,796 / 188,700 | 0 | 0 |
| Live and recording | 29.47 (442 frames) | 29.43 (448 frames / 15.220 s) | 219.20 | 167,344 / 194,748 | 7 | 0 |

The samples demonstrate near-target operation after removing synchronous webcam work;
they do not establish ten-minute endurance, physical AV drift or glass-to-glass latency.
A remembered initial webcam-freshness warning remains in last_error as historical
feedback. Automatic camera exposure still limits unique camera frames separately.

The final optional physical camera probe returned owned RGBA 1920x1080 on all
90 reads, with 62 unique frames, mean main-read time 1.768 ms and max 3.784 ms;
pre-boundary backlog was rejected. No camera pixels were exported by this probe.
Final Xorg-only and Wayland-only builds passed -Werror in an isolated source tree;
the combined executable remains installed in the checkout. Staged install ran
--version and matched ./cast exactly, and staged uninstall removed it. Man page,
configuration example and license files were present; no system install or user
configuration modification was performed.

## v0.2 panel and distribution validation

The combined Xorg/Wayland/panel build passed `-Werror`, the full unit/IPC/mock
portal suite, and native panel interaction checks on a private Xvfb display.
Transport checks cover peer ownership, sealed bounded frame mappings, pause/freeze
privacy barriers, session settings, stale generations, reconnect and cancellable
shutdown. Camera edits preserve centered resizing and free positioning; zoom
toggle remembers the factor selected through the panel. Both Xorg capture paths
verify authenticated panel and window-manager frame exclusion. A Wayland-only
panel build passed in an isolated source tree. No panel tests used the user's
desktop or physical camera; actual Wayland panel/capture integration remains manual.

Arch packaging was checked through ordinary-user makepkg with distribution
hardening and LTO, using an isolated official SDL3_ttf SDK without system installation.
The CI recipe passed all checks and emitted six matching Arch-qualified release
assets with verified checksums, declared runtime dependencies and no bundled SDK.
This local check is not a clean Arch chroot result; the release workflow provides
the fresh Arch environment. Installer tests use offline stubs and real private
terminals, checking package integrity, malformed metadata, privilege routing,
download failures and relocated embedded execution without invoking real sudo.

The revised panel's native checks also pass with real SDL X11 content scaling at
2×: 16 logical-pixel text rasterizes at 32 pixels, and scaled pointer input reaches
the same controls. Tests cover all five focused screens, Home/Back/Escape, an
always-visible actual preview, retained target/drag position, protected controls,
drafts across navigation, UTF-8 paste, numeric rejection and reconnect. Final live
synthetic renders at 480×760 and 360×640 were reviewed at both scales. Exclusion
warnings remain visible on Home and settings screens. This is isolated native UI
validation, not a claim about every physical display or compositor.
