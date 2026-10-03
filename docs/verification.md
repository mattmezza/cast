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

The media suite decodes real libx264/AAC Matroska files. Virtual camera+record and record-only
active timelines were approximately 1.06 seconds after repeated interruptions, with
AAC ends within about 20 ms; longest video intervals were 34–67 ms under test load.
Queue privacy contained zero queued green test frames and no sensitive test tone;
injected slow encoder work dropped bounded frames while status/virtual/stop/privacy
operations completed in under 1 ms. Injected ENOSPC preserved partial files and kept
virtual operational; subsequent recordings decoded successfully. These tests use
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

Equal effective annotations reuse one composed frame for virtual+record. The CPU budget
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
  exactly zero during startup pause, virtual freeze and group pause. Both mic and
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
| Virtual camera only | 30.00 (450 frames) | — | 117.80 | 111,536 / 128,456 | 0 | 0 |
| Recording only | — | 29.93 (456 frames / 15.234 s) | 188.93 | 155,796 / 188,700 | 0 | 0 |
| Virtual camera and recording | 29.47 (442 frames) | 29.43 (448 frames / 15.220 s) | 219.20 | 167,344 / 194,748 | 7 | 0 |

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
drafts across navigation, UTF-8 paste, numeric rejection and reconnect. Final virtual
synthetic renders at 480×760 and 360×640 were reviewed at both scales. Exclusion
warnings remain visible on Home and settings screens. This is isolated native UI
validation, not a claim about every physical display or compositor.

## v0.3 panel, preview and countdown validation

The full combined Xorg/Wayland/panel suite passes with `-Werror`; an isolated
Wayland-only/panel build passes the full suite too. Native panel interactions pass
at 1x and 2x, including square icon controls, Back, persistent movable preview,
countdown on every section, cancellation, privacy controls, drafts and reconnect.
A private Xvfb instance of the installed mwm verifies the panel's pre-map utility
type, truthful class, floating state and centered position. No user WM files or
user desktop were changed. Final synthetic UI captures update the README image.

Xorg checks pass with MIT-SHM and the XGetImage fallback. They exercise managed
preview/frame exclusion, latest-frame updates, old in-flight epoch rejection,
rapid `j` presses producing `j`, `jx2`, `jx3`, countdown Escape/WM-close cancellation,
and retained masking while a reparenting WM frame retires. A real daemon on private
Xvfb additionally verifies command cancellation acknowledgements, recording start,
finalization and decoded first-frame pixels without countdown graphics. Visual
compositor tests cover bounded recent history, independent expiry, repeat refresh,
eight-row eviction, geometry and privacy clearing; isolated ASan/UBSan passes.

The retained `test_xorg --preview-benchmark` submits 90 synthetic 1920x1080 frames
at 60 Hz on an 800x600x24 Xvfb display. The equivalent 640x320 preview comparison
improved from 20/90 visible updates and 45.16 ms mean displayed frame age to 89/90
and 16.67 ms. Final 640x360 runs show 89/90 updates and 16.67 ms age with both capture
paths; mean capture-thread preview calls take 2.000 ms with SHM and 1.878 ms with
fallback. The old limited path averaged 0.54 ms per call because it skipped most
frame copies. This measures isolated preview publication, not hardware or conference
latency. Keyboard software autorepeat that does not enter XI2's global raw stream
is not synthesized; actual observed presses and flagged repeats are counted.

## v0.4 presentation modes and camera backdrop

This version changes the old recording-pause contract deliberately: solid pause
writes the configured screen and silence, while cut/resume retains the prior
same-file active-time clock. Core, command, pixel and decoded media tests cover
independent solid/freeze/blur flags, precedence, empty and UTF-8 text, local-time
templates and bounded expansion, per-lane audio gates, cut admission and countdown
cancellation. The synthetic daemon test decodes actual FFV1 video, checks styled
segments, repeated frozen composition, blurred pixels and monotonic gapless PTS.

The standalone countdown uses the real floating CastPreview window. Native tests
exercise preview originally off and on, initial start and same-file cut-resume,
Escape/WM-close/privacy cancellation, capture exclusion and decoded first-frame
pixels. A synthetic reparenting WM that refuses to retire its visible decoration
causes the hide barrier to fail; recording cannot start/resume in that case.

Camera backdrop measurements use synthetic RGBA 1920×1080 output and camera,
4:3 foreground, fresh camera timestamps and the existing compositor benchmark.
Solid camera-only median 23.02 ms vs fresh blurred 28.27 ms (increment 5.26 ms); split
median 6.60 ms vs 8.05 ms (increment 1.45 ms). Repeating the same camera snapshot reuses
the cached backdrop with approximately zero added cost. Reduced backdrop plus
temporary blur storage is at most 675 KiB, and missing/hidden cameras discard it.
These are compositor-only timings on the development host; capture, actual webcam,
encoder and conference latency are outside that measurement. Per-frame refresh
is implemented rather than a slower animation timer.

Font/template/backdrop tests pass ASan/UBSan with leak checking. Native panel tests
cover the focused Outputs screen, new controls and drafts, automatic acknowledged
preview target, utility window placement, all sections and minimum width at 1×/2×.
These synthetic/private-display checks do not establish remote conference or
real Wayland compositor compatibility.

## v0.5 independent preview and footer typography

The panel no longer fetches or renders actual-output frames. Route tests poison
frame-fetch calls so reintroducing the old embedded preview fails the build. Home
contains independent Virtual camera and Recording controls; the five configuration screens
retain preview-window toggling and target selection. Native tests exercise utility
window placement, faster wheel movement and reversal, drafts, footer/gap controls,
preview target acknowledgement and the separate Xorg preview window at minimum and
default widths and 1×/2× display scaling.

Presentation pixel tests verify bottom-centered optional footers, exact configured
title/subtitle gaps independent of font size, retained center position, shared
wall-clock expansion, small-canvas fitting and disjoint clipping for pathological
multiline text. Actual system-font replacement and restoration are compared in
rendered pause/blur frames; production controller tests reload both fonts from a
config file and reject runtime font edits. Core/visual tests pass Werror and
ASan/UBSan with leak detection. Invalid legacy pause-message templates now fail
before session mutation.

Combined Xorg/Wayland/panel and isolated Wayland-only Werror checks pass. The latter
links Fontconfig/FreeType and the panel without Xorg source or direct Xorg library
dependencies. Xorg countdown checks run with a real panel attached in panel builds:
initial start, cancellation and same-file cut-resume leave CastPanel open, fully
hide CastPreview before media admission, and decode first-frame pixels to reject
the cinematic guide. These are synthetic/private-display checks, not a claim of
remote-conference or physical-camera coverage.

## v0.6 stage and presentation layers

The combined X11/Wayland/panel build passed `-Werror` with `make check check-xorg`.
This includes core configuration, output privacy, media, production commands,
sealed panel transport, mock portal, socket IPC, actual decoded output, helper
commands, installer checks and Xorg SHM/fallback/native countdown tests. The
Wayland-only panel build also passed its complete `make check` suite; source and
ELF checks confirm no Xorg source or direct Xorg/XCB dependency in that build.

New visual pixel tests cover all eight stage anchors and free placement,
size-derived overlap, preserved aspect and zoom, shared gradient endpoints,
angle and via waypoint, background cache refresh/replacement, rounded screen
masks, clipped cursor/clicks, transparent logos, text anchors and font changes.
The optimized mask spans are compared against the scalar rounded-mask formula
on odd canvases and fractional contain rectangles. Prepared image/font failures
retain previous resources. Core and visual ASan/UBSan checks pass with leak
scanning; controller ASan/UBSan passes with leak scanning disabled because this
host's PipeWire module loader retains 3,525 bytes in 34 allocations during test
initialization, outside the new composition resources.

`test_presentation_layers.py` starts a private synthetic daemon and decodes its
actual FFV1 Matroska file: stage gradient, transparent PNG and literal static
text survive encoding; invalid image replacements retain the old logo; solid
pause covers all source/branding pixels; session commands leave config untouched.
No real desktop, webcam, loopback device or conference is used by these checks.

The native panel passed the focused forms at 480/360 logical widths and 1×/2×
scales in private Xvfb/mwm sessions. Actual acknowledgements validate reverse
configured cycles, stage sizing, screen appearance, shared gradients, image path,
alpha controls and text/font changes. Utility centering, independent floating
preview and legacy privacy/countdown behavior remain verified. One batched visual
inspection and one copy-only confirmation completed; the mechanical detector
reported no findings. Home's screenshot remains unchanged.

At synthetic RGBA 1920×1080, radius 24 and border 2, the optimized stage measured:

| Background | Median without overlays | Median with cached logo/text | p95 with overlays |
|---|---:|---:|---:|
| Solid | 7.29 ms | 7.44 ms | 8.09 ms |
| Cached gradient | 8.39 ms | 8.54 ms | 9.39 ms |
| Fresh blurred frame | 13.20 ms | 13.38 ms | 14.18 ms |

The earlier scalar per-pixel rounded mask measured 25.30 ms with fresh blur and
branding. Scanline spans remove that full-frame mask overhead without changing
pixels. The logo is decoded/scaled and static text rasterized during preparation;
steady-state compositing reuses them. These measurements exclude capture, codec,
audio, preview and device I/O and do not establish end-to-end hardware latency or FPS.

## v0.7 Operate/Compose and streaming

All new protocol automation runs against synthetic media and private local listeners;
no actual stream key, service account, user desktop or physical camera is involved.
The combined Xorg/Wayland/panel build uses `-Werror`. Isolated Wayland-only panel and
headless builds pass unit and decoded three-output tests without direct forbidden
Xorg/SDL dependencies. Distribution FFmpeg can still pull Xorg transitively.

Native panel checks cover paused start and explicit reveal, recording start/cut/resume/
finish/cancel, staged enums/booleans/text/numbers, Apply/Revert, invalid drafts, UTF-8
clipboard input, local streaming setup/connect/retry/stop, independent blur and group
restore, panel-close independence, disconnect/reconnect, keyboard navigation and
360/440/520/800 logical widths at 1×/2×. A SIGSTOP-held daemon makes Pending observable
while renderer frames continue; SIGCONT produces acknowledgement. No panel frame fetch
is permitted by route tests. One batched visual inspection plus one confirmation pass
covered the supplied hierarchy and native adaptation; there is no repeated polish loop.

Contrast checks apply the WCAG relative-luminance formula to production text colors
and actual default/hover/disabled/selected fills, requiring at least 4.5:1. Disabled
selected actions use the normal control fill; hovered chip metadata uses secondary
rather than muted. Native preview uses the same bundled Inter family via FreeType.
Xorg SHM/fallback tests exercise three target pills, local status, Cancel/Escape and
small-to-large countdown resizing; first-frame decode rejects countdown contamination.

The decoded private RTMP test checks H.264/AAC headers, cadence, continuous timestamps,
active audio and zero paused energy on the same TCP session. One representative short
run decoded 113 video/173 audio frames with a 34 ms largest video gap and 21 ms AV-end
difference. Pause/stop calls were below 1 ms; these are local fixture timings.

Fault tests retire queued and already-converted but unsubmitted sensitive pixels,
clear delayed audio and verify fresh headers/keyframes after reconnect. Actual receiver
SIGSTOP induces blocked writes; controls remain below 1 ms, queues remain bounded,
and a write deadline retires the worker. Killing an ingest mid-write reconnects with
zero retired red frames in the decoded new session. Finite exhaustion and stop during
backoff are covered. The TLS fixture accepts a trusted localhost certificate and
rejects untrusted/wrong-host certificates before application traffic. Stalled TLS and
a truly blocked `getaddrinfo` (test-only preload) remain cancellable and reaped. A
publish-denial fixture injects a random secret in an FFmpeg-originated diagnostic;
public error is sanitized and authentication rejection does not retry.

Official Twitch/YouTube settings were checked on 2026-10-04 and are linked in
[streaming.md](streaming.md). Local RTMP/RTMPS success does not establish acceptance
by a real account or end-to-end hardware latency. Those checks remain in
[hardware-acceptance.md](hardware-acceptance.md).

### Sustained streaming and sanitizer checks

On 2026-10-04, the fixture run by `make X11=0 check-stream-sustained` exercised a 65-second
synthetic moving 1280×720/30 stream at 4000 kbps H.264 CBR with stereo 48 kHz
AAC, including 55 seconds of simultaneous software recording and a final
five-second stream privacy pause. The host was Intel Core Ultra 5 235U with
FFmpeg n9.0.2 and an optimized GCC build. A private loopback FFmpeg listener
received the stream; no service account, physical input or public broadcast was used.

The decoded stream contained 1,943 video frames through 64.767 seconds, with
22 ms audio/video end difference and a 66 ms maximum video interval when a
privacy epoch retired one unsubmitted frame. Late active audio RMS was 0.2048;
paused audio RMS was exactly zero. The simultaneous recording decoded 1,650
frames and retained active audio during the stream's privacy pause. Nine pending
stream frames were retired by the bounded queue/privacy policy; the connection
did not reconnect during the run.

| Measured mode | Sample duration | CPU (% of one core) | Peak summed parent/worker RSS |
|---|---:|---:|---:|
| Synthetic streaming | 9.94 s | 13.98% | 51.67 MiB |
| Streaming plus software recording | 54.85 s | 70.32% | 85.84 MiB |

CPU and RSS include the synthetic producer, raw frame copies, selected-mix ring
reader, stream encoder and, in the second row, recording encoder. The private
receiver and subsequent artifact decoding are excluded. Summed RSS counts
shared mailbox pages in both processes. This profile stayed within a local
one-core/128 MiB summed-RSS comparison budget, with less than 1% frame retirement
and less than 100 ms A/V end difference. These are measured bounds for this
synthetic profile, not a runtime memory cap or a hardware/capture/composition,
service delivery, glass-to-glass latency or long-duration endurance claim.

The sustained exercise exposed a supervisor deadline race: a worker can publish
a newer phase timestamp after the supervisor samples its clock. Subtracting
that timestamp from the older unsigned clock falsely expired a healthy write.
The deadline now requires the observed clock to be later before subtracting.
A deterministic test recreates this clock ordering while encoder work is held
and verifies that the session remains connected. Direct FLV packet scheduling
also accounts for AAC's initial encoder delay, preserving ordered submissions
without retaining media in an interleaver across privacy epochs.

The final C and network fixtures passed AddressSanitizer and UndefinedBehaviorSanitizer
with LeakSanitizer enabled; no synthetic test-parent leaks were reported. Stop/fault
paths deliberately terminate isolated workers, so dependency cleanup inside those
terminated processes is not established by leak scanning. The tests include converted-but-unsubmitted
privacy, decoded silence, fresh reconnect headers/keyframes, actual stopped-reader
backpressure, finite retry exhaustion, stop during backoff, TLS trust/hostname,
permanent publish rejection and real blocked DNS cancellation. Representative
privacy/stop calls were 0.108/0.127 ms; stopped-reader privacy was 0.103 ms.
The test-only audio constructor uses the production ring readers and privacy
logic with a PipeWire thread loop, but opens no server connection or input
modules. This avoids the previously documented external module-loader allocations;
dependency-wide PipeWire leak freedom is still not claimed. Reproduce with:

```sh
make X11=0 check-stream check-stream-sustained
make X11=0 BUILD=/tmp/cast-stream-asan \
  CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  /tmp/cast-stream-asan/test_stream
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/cast-stream-asan/test_stream
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/test_stream_network.py --binary /tmp/cast-stream-asan/test_stream
```

## v0.7 panel drill-in refinement

The second UI round changes only the native panel. Private Xvfb workflows verify
all seven dedicated Compose pages, sticky Back, `1`/`2`, `Alt+1…7`, sheet-first
Escape, composition deep links and zoom reset. Dirty fields stay local; Enter
applies the current section as one batch, keeps the page/scroll, and acknowledged
edits remove the pinned draft bar. A long Camera page keeps Apply/Revert visible.
Panel process close/reopen restores page, parent sheet and per-view scroll from
session shared memory; unsent drafts are discarded on process exit. A new daemon
socket session resets navigation. Config bytes are unchanged throughout.

The native checks cover 360–800 logical pixel widths and 1x/2x density: equal
44px lane chips, equal 30px meta-actions, a pinned 46px status bar, single-line
16px status rows, bounded long connection/failure messages and whole-row clipping
on the Compose list. A batched visual inspection and one confirmation cover
Operate, the section list, long dirty pages, setup, failure and disconnection.
No preview, daemon, IPC, command identifier, configuration schema, palette or
bundled-font slot is changed by this refinement.
