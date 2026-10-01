# Build cast: a minimal Linux presentation camera and recorder

You are the implementation agent. Build the complete application described below, not merely a prototype, plan, or scaffold. Work incrementally until the requirements are implemented and verified. Inspect the repository and any AGENTS.md instructions first. If no project exists, initialize it locally. Do not publish a repository, deploy anything, or add unrelated services without authorization. If privileged dependency installation or kernel-module setup is needed, provide exact commands for the user to run; do not silently change their system.

## Product and philosophy

The product and CLI are named `cast`. It is a Linux desktop utility written in C, inspired by suckless software: small understandable code, minimal dependencies, a Makefile, runtime configuration in an XDG config file, and one job done well. Its job is to compose a screen capture with an actual webcam and optional presentation annotations, expose it as a virtual camera for conferencing, and optionally record it with audio.

Primary target: Arch Linux on Xorg. The user uses sxhkd for shortcuts. Support an optional Wayland capture backend without contaminating shared logic with X11 assumptions. Do not promise universal application compatibility or complete Wayland feature parity.

There is no scene editor, timeline editor, plugin system, streaming-service integration, background removal, account system, cloud component, or heavyweight GUI toolkit. Do not enforce an arbitrary source-line ceiling at the expense of correctness. Favor a few small modules and explicit state over a generalized multimedia framework.

A conference sees cast as a webcam. Multiple participants can each transmit a screen composition through their own camera tile; this does not create a shared device or change conference layouts, camera resolution limits, or compression. Explain that text readability depends on the conference app and viewers enlarging/pinning tiles.

## Architecture and dependencies

Use one executable. `cast` starts the foreground daemon/capture process. `cast <command>` connects to the running instance through a user-only Unix domain socket under XDG_RUNTIME_DIR. No separate castctl binary is necessary. Commands return acknowledged success or actionable errors and nonzero exit codes. Add bounded IPC messages, timeouts, argument validation, ownership checks, safe stale-socket cleanup, single-instance behavior, and predictable concurrent-command ordering. Do not invoke a shell to execute user-provided arguments. Handle SIGINT/SIGTERM with clean shutdown; use commands for rich controls rather than overloading signals.

Prefer X11 capture using Xlib or XCB consistently, with appropriate shared-memory capture and monitor discovery extensions. Webcam input is V4L2. Virtual camera output is an existing v4l2loopback device. Document exclusive_caps=1 and the fact that some consumers see the device only after the producer starts. Never write a custom kernel driver. Enumerate compatible formats and support realistic webcam formats, including commonly encountered MJPEG if needed through an existing decoder.

Use PipeWire for audio capture/routing, including an optional virtual microphone. Reuse existing FFmpeg libraries or an FFmpeg recording subprocess for codecs, scaling, and muxing as justified. Start with a short dependency/architecture decision document. A subprocess is acceptable only if pause/resume, timestamps, buffering, and error recovery are actually correct; suspending FFmpeg is not a recording-pause implementation. Avoid hand-writing video codecs. Support software encoding as the baseline; hardware encoding can be optional and must not be required for correctness.

Xorg global input observation may use XInput2/libXi or corresponding XCB support, without grabbing input or interfering with sxhkd. Validate event observation and keyboard translation with a prototype. Use lightweight text drawing (a bundled licensed bitmap font is acceptable; optional Xft is acceptable if justified). Do not use privileged /dev/input access as the default.

Keep three narrow platform boundaries: capture, input observation, and presentation UI (preview/selection). Shared composition, command parsing, configuration, webcam handling, audio, recording, and virtual-camera output must not expose X11 types. Frames carry format, dimensions, stride, ownership/lifetime, and monotonic timestamps. Define conversions among desktop coordinates, source-local pixels, and output pixels explicitly, including scaling and rotation.

Implement a build-optional Wayland ScreenCast portal + PipeWire backend. It must handle asynchronous consent/selection, cancellation, session closure, permission revocation, and stream renegotiation. Add only the needed D-Bus/PipeWire dependencies; make Xorg dependencies removable in a Wayland-only build. Use portal-advertised cursor capabilities. Global key/click observation is optional and often unavailable: do not misuse InputCapture as a passive desktop logger or silently acquire elevated permissions. Report capabilities and return clear unsupported errors. Use compositor-native keybindings to invoke the unchanged CLI on Wayland; sxhkd is the Xorg workflow. Wayland preview/selection may be implemented lightly or explicitly reported unsupported where necessary; core capture/output/recording must work. A capability matrix must distinguish implemented, unsupported, and untested behavior per backend.

## Composition

Provide four layouts: overlay, split, screen, camera. Overlay fills the canvas with screen content and places a webcam over it. Split gives camera and screen separate areas, with left/right camera placement and configurable camera width percentage. Screen and camera layouts show just that source. Preserve camera geometry when hidden or switching layouts.

Camera masks: rectangle, rounded rectangle with configurable radius, circle. Aspect/cropping is separate: native, 16:9, 4:3, 1:1; circle uses a square crop. Provide centered crop with adjustable offsets, mirror of camera content only, configurable border width/color, size, position, margins, and corner anchors. No decorative shadow requirement.

Camera sizing uses canvas-width percentages. +5% means five percentage points, not multiplying the prior size by 1.05. Anchored resizing keeps the anchored corner fixed; free-position resizing keeps the center fixed. Clamp valid geometry within canvas bounds and reject impossible requests. Corner cycling uses configurable order.

Capture supports monitor selection/cycling, selected monitor region, and window selection/active-window-once on Xorg. Region x/y/w/h are relative to the selected monitor. Interactive drag selects a region; click selects a window; Escape cancels and retains the previous source. Handle window motion, resize, minimization, and destruction explicitly. Never fall back silently to another screen or full desktop. Monitor hotplug and window failures yield safe neutral content and visible errors.

Fit modes: contain (letterbox) and cover (crop). Keep output resolution and frame rate stable across source switches. Defaults: overlay, rounded rectangle, bottom-right camera at 22% width, modest edge margin, contain fitting. Support runtime-configured presets such as coding, demo, conversation; presets affect composition only and never resume outputs or alter recording state.

## Zoom, cursor, clicks, and keys

Zoom affects only the screen layer, not the webcam or annotation labels. Provide zoom toggle/in/out/set/reset, configurable limits/steps, and eased transitions initially around 200–300 ms. Follow mode tracks the cursor with smoothing and a dead zone; locked mode targets its position and then holds the view. Clamp at source edges, avoid jitter, and define behavior when the cursor leaves the source or changes monitors. Reset zoom on source change unless a preset explicitly specifies it. Cursor-follow requires cursor metadata; disable/report unsupported where absent. Transform cursor and click coordinates through exactly the same crop/zoom/scale pipeline as screen content.

Provide cursor visibility, configurable cursor size where separately rendered, and cursor highlighting. Avoid rendering two cursors when capture already embeds one. Click indicators are configurable expanding/fading rings with separate left/right colors and optionally middle click. Ignore unrelated source coordinates.

Keystroke overlays default off and default to shortcuts mode when enabled. Shortcuts mode shows modifier combinations and configured navigation keys; all mode explicitly enables printable-key display. Translate layouts correctly, avoid stale held-modifier state, collapse repeats, format combinations consistently, and exclude cast-control shortcuts through configurable filtering. Clearly document limitations of compose keys/IMEs; do not falsely claim raw key events reproduce committed text universally. Configurable placement/font/colors/timeout. Never store a raw input log.

Input observed during privacy pause must not be replayed afterward. Clear keys/click animations on pause and source changes. Do not rely on detecting password fields. All-key display can expose sensitive typing. Support annotation visibility independently for live and recording outputs, defaulting to the same composition. Unsupported backend features return errors rather than silently doing nothing.

## Output state and privacy

Live video and recording have independent state machines. Start virtual output privacy-paused with a neutral Paused frame at normal cadence; explicit resume is required. A live pause emits neutral content and silences cast's virtual audio. It never freezes potentially sensitive last content. Recording pause appends neither video nor audio, and resume continues the SAME file with the interruption removed from the media timeline. Maintain AV synchronization across repeated pauses and long interruptions.

`cast pause` pauses both applicable outputs immediately and remembers their prior states. `cast resume` restores only states changed by that group pause, without starting nonexistent recordings or resuming recordings already paused independently. Repeated group pauses are idempotent. Define how independent commands while group-paused supersede remembered state; test the chosen semantics. Privacy barriers must drop queued sensitive frames/audio/annotations so stale data cannot leak after acknowledgement or resume. An encoder's already accepted pre-pause data may finalize, but no post-pause samples may enter the recording.

Separate live freeze/unfreeze deliberately holds the current composed frame for explanation. Freeze is not privacy pause; define audio behavior explicitly, with silence as the safe default. Pause always overrides freeze. Freeze/unfreeze must not bypass privacy pause.

cast cannot mute a physical microphone selected directly by the conference app. Explain that the user needs the app's own mute command in that case. Off/paused audio lanes output silence, not stale buffers. Do not send any captured audio back into physical speakers by default or create feedback loops.

Reset changes composition defaults only, never output pause/recording states. Source/preset changes never automatically resume. Provide immediate status and error feedback.

## Audio and recording

Support microphone and selected desktop audio, independently enabled, with source selection and gain. Default desktop capture off; explicitly identify what is captured. Where PipeWire permits, select an application's audio rather than capturing the whole speaker monitor. Explain whether other call participants are included in a selected desktop source. List sources, handle disappearance safely, avoid accidental broad-source fallback, and prevent accidental feedback/duplicate mic routing.

Expose an optional named virtual audio source containing the selected mix for conference selection. Virtual video and audio are separate devices. Local recording uses the configured audio mix and composed video. Use monotonic timing, bounded queues, drift management, and a defined drop/backpressure policy. A slow encoder or preview must never stall the virtual camera/control loop. Test live-only, recording-only, and simultaneous operation.

Provide configurable encoding/container defaults and a robust recording format (consider Matroska for crash resilience). Choose safe filename generation, no implicit overwrite, clear finalization output with filename, optional recording-only countdown, and actionable failure reporting. Countdown never delays privacy pause and cancellation cannot start recording later unexpectedly. Disk-full/encoder failure stops recording cleanly as far as possible and leaves live output operational. Preserve recoverable partial files and explain recovery limitations. Never promise all containers survive crashes identically.

## CLI contract

Implement the following surface. Small grammar refinements are allowed only if documented consistently; do not silently drop functionality. Provide --help, --version, per-command help, listing commands, and machine-readable status. `on|off|toggle` and explicit set operations should have predictable semantics.

```
cast [startup options]
cast layout overlay|split|screen|camera
cast layout next
cast split side left|right
cast split ratio 25%
cast camera show|hide|toggle
cast camera size +5%|-5%|25%
cast camera move DX DY
cast camera position X Y
cast camera anchor top-left|top-right|bottom-left|bottom-right|next
cast camera shape rectangle|rounded|circle|next
cast camera aspect native|16:9|4:3|1:1
cast camera crop move DX DY
cast camera mirror on|off|toggle
cast camera list
cast camera device /dev/video2
cast screen list
cast screen select DP-1
cast screen next
cast capture monitor
cast capture region select
cast capture region X Y WIDTH HEIGHT
cast capture window select
cast capture window active
cast capture fit contain|cover
cast zoom toggle|in|out|reset
cast zoom set FACTOR
cast zoom follow on|off
cast cursor on|off|toggle
cast cursor highlight on|off|toggle
cast clicks on|off|toggle
cast keys on|off|toggle
cast keys mode shortcuts|all
cast keys clear
cast annotations live keys|clicks on|off
cast annotations record keys|clicks on|off
cast pause
cast resume
cast live pause|resume|toggle
cast live freeze|unfreeze
cast record start [PATH]
cast record stop
cast record pause|resume|toggle
cast audio list
cast audio mic on|off|toggle
cast audio desktop on|off|toggle
cast audio mic source NAME
cast audio desktop source NAME
cast audio mic gain 80%
cast audio desktop gain 50%
cast audio virtual on|off|toggle
cast preset NAME
cast preset next
cast preview on|off|toggle
cast status [--json]
cast doctor
cast config check [PATH]
cast config defaults
cast config reload
cast reset
cast quit
```

record toggle means pause/resume an existing recording, NEVER start/stop. With no recording it returns an error. live toggle toggles privacy pause; zoom toggle restores the configured/last non-1 zoom factor or resets to 1. Document every toggle precisely. Startup options must cover device paths, output dimensions/fps, backend, audio sources, recording defaults, and socket/instance selection if supported. Configuration precedence is built-in defaults → runtime config file → startup CLI flags → runtime commands. Runtime commands alter only session state. No automatic persistent configuration writes. Support --config PATH.

## Preview and user feedback

Optional lightweight preview shows the actual output, with a selectable live/record target if annotations differ. Add visible LIVE/PAUSED/FROZEN and RECORDING/RECORDING-PAUSED indicators locally; status decorations should not unintentionally become output overlays. Make status --json useful for status bars: source, backend/capabilities, output state, layout, zoom, camera visibility, recording path/duration, audio routing, errors, dropped frames.

Avoid preview recursion. Investigate Xorg exclusion mechanisms honestly; don't claim arbitrary windows can always be invisibly removed from root capture. Hide/unmap selection UI during acquisition where needed, use an out-of-region preview or an explicit strategy, and document residual limitations. Test captured output, not just the preview.

doctor checks display/backend, camera formats/access, virtual output device/module and formats, audio access, codec/backend availability, recording directory writability/free space, relevant permissions, and portal capabilities. It is read-only and gives exact corrective advice. No automatic privilege escalation.

## Configuration and documentation

Use a simple INI-style cast.conf parsed by a small maintained C parser with compatible licensing. Avoid full TOML complexity and do not invent a configuration language. The normal path is $XDG_CONFIG_HOME/cast/cast.conf, falling back to ~/.config/cast/cast.conf when XDG_CONFIG_HOME is unset or empty. Respect XDG absolute-path requirements and document behavior for invalid relative values. --config PATH explicitly selects a file. A missing default file uses built-in defaults; a missing explicitly selected file is an error. Distribute a ready-to-run binary/package and example config; users must not recompile to change runtime settings. Compile-time settings are only for build features such as backend availability.

Configure geometry, masks, borders, layouts/preset cycle, monitor/source defaults, output resolution/fps, zoom motion, cursor/click/key appearance, key filtering, audio gains/routes, recording/countdown, pause frame, and socket behavior. Use named sections and named presets, e.g. [output], [camera], [zoom], [keys], [preset.coding]. Define the scalar/list syntax, duplicate-key policy, escaping, comments, units, booleans, and validation ranges explicitly. Do not expand shell expressions or execute config content.

Example syntax (ship a complete documented example using the implemented schema):

```ini
[output]
device = /dev/video10
width = 1920
height = 1080
fps = 30

[camera]
device = /dev/video0
shape = rounded
anchor = bottom-right
width_percent = 22
margin = 24

[zoom]
factor = 2
transition_ms = 250
follow = true

[keys]
enabled = false
mode = shortcuts

[preset.coding]
layout = screen

[preset.demo]
layout = overlay
camera_shape = circle
camera_width_percent = 18
```

Strictly validate unknown sections/settings, invalid values, and conflicting combinations, reporting filename and line number. `cast config check [PATH]` validates locally without needing the daemon or capture devices. `cast config defaults` prints a complete parseable default configuration without creating files or needing the daemon. `cast config reload` asks the running daemon to reread its selected file: parse and validate the entire candidate first, then atomically apply supported changes. A failed reload leaves current settings intact. Do not add automatic file watching. Preserve startup CLI overrides during reload; document that reload replaces prior runtime overrides for reloadable settings. Reload never resumes privacy-paused/frozen outputs, starts/stops recordings, or overwrites output-state controls. Identify restart-required settings (e.g. socket/backend/output format or recording encoder changes during an active recording); if any changed setting cannot be applied safely, reject the entire reload with a list of reasons instead of partially applying it. Source changes obey the existing safe-switch rules. Reset restores effective composition defaults from built-ins/config/startup overrides, not output-state changes.

Ship the example file without overwriting user edits during installation/upgrades. Never automatically write runtime changes to the user's config. Keep every runtime control and its matching config setting documented. Add tests for precedence, missing/invalid files, unknown keys, preset parsing, atomic reload failure, restart-required settings, and privacy preservation.

Ship README, complete CLI reference/man page, architecture/state description, dependency rationale, configuration guide, troubleshooting/doctor guide, Wayland capability matrix, and an sxhkd example. Include bindings for pause-all/resume, live toggle, recording pause/resume, camera hide/size/corner, layout/preset cycling, monitor cycling, region selection, zoom, clicks/keys, and preview. Include status-bar JSON integration guidance and a deliberate command for recording start/stop. Explain module setup and kernel-update/DKMS considerations for Arch, without claiming setup commands were executed. Provide Makefile build/install/uninstall targets, optional backend flags, an Arch PKGBUILD example, and an explicit project license with dependency/font-license compatibility checked.

## Implementation and validation workflow

First produce a requirements checklist tied to this document and short architecture decisions. Validate risky primitives early: real V4L2 consumer output, webcam format decoding, independent output paths, recording pause timestamp continuity, Xorg input observation, and portal capture. Then implement in milestones, continuing through the complete scope rather than stopping after the first milestone:

1. Core daemon/IPC, capture/composition, virtual camera, safe pause, status.
2. All layouts/shapes/camera/capture controls and region/window selection.
3. Audio, optional virtual mic, recording with correct independent state and failure isolation.
4. Preview, indicators, presets, zoom/cursor/click/key annotations.
5. Optional Wayland capture/backend capabilities, packaging/docs, verification.

Add meaningful automated tests for geometry/clamping/transforms, state transitions, group-pause restoration, recording timeline/AV sync, IPC errors, queue privacy barriers, and failure isolation. Use synthetic frames/audio to inspect actual recording/output behavior. Test repeated pauses, recording-only operation, encoder slowdown/failure, camera/monitor removal, selection cancellation, and keyboard layout changes. Use compiler warnings and sanitizers where available. Keep dependencies/testing tooling out of normal runtime where possible.

Measure performance on a documented baseline such as 1920x1080 at 30 fps; report CPU/memory, latency and drops when actual hardware is available. Verify in available conferencing consumers and distinguish tested compatibility from expectations. If running remotely without Xorg/camera/kernel module/Wayland portal, complete all independent implementation and synthetic tests, then provide an exact hardware acceptance checklist. Never fabricate visual checks, performance, compatibility, or working integrations.

Done means usable code, no placeholder implementation for advertised supported features, sensible defaults, complete commands/configuration/documentation, and a traceable requirements checklist. Unsupported Wayland global-input/selection features must be identified explicitly through capabilities rather than represented as stubs claiming success. Final handoff must state what works, what was tested, remaining platform limitations, build/run instructions, and the user's exact system setup steps.
