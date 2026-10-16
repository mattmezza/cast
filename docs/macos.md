# macOS development port

This is the first recorder-focused macOS port. It targets Apple Silicon with macOS
13 or later and keeps the existing C compositor, FFmpeg recording engine and
SDL3/Clay control panel. The Apple-specific adapters compile against Apple SDKs
and the synthetic checks pass on Apple Silicon. Hardware capture still requires
permission and runtime validation; synthetic checks cannot establish that it works.
Intel builds have not been validated.

## Build on your MacBook

Install Apple's Command Line Tools and the Homebrew dependencies, then build from
this checkout. Run these commands in Terminal as your normal user:

```sh
xcode-select --install
brew install pkgconf ffmpeg fontconfig freetype sdl3 sdl3_ttf
brew upgrade ffmpeg # if an older installation is present; requires FFmpeg 7.1+
make -j4
make check-macos
make macos-app
open build/macos/SuperScreencast.app
```

If Command Line Tools are already installed, skip the first command. Homebrew's
SDL3_ttf formula is named `sdl3_ttf`; its pkg-config module is `sdl3-ttf`.
The bundle is ad-hoc signed for local development. Its libraries remain in
Homebrew, so it runs on the build machine and is not a standalone distribution.
The macOS 13 deployment target applies to Cast's sources. Homebrew libraries can
require newer macOS versions; this local bundle inherits their requirements.
Do not use the Arch installer or Linux `make install` instructions on macOS.
`make PANEL=0` builds a CLI-only binary if you want to isolate capture from UI.

`make check-macos` runs core/visual tests and a synthetic end-to-end recorder
check: daemon/client IPC, pause/resume, MKV capture, MP4 finalization and shutdown.
It does not request capture permissions or test Apple hardware APIs.

## Initial behavior and permissions

The macOS defaults use `backend=macos`, disable virtual output and camera, and
save recordings in `~/Movies`. Create that directory if needed. Microphone and
system audio remain off until enabled. Linux configuration files may contain
unsupported devices or features; start with the platform defaults instead:

```sh
./cast config defaults
./cast config check
./cast doctor
```

Unless `XDG_CONFIG_HOME` is set, the configuration lives in
`~/Library/Application Support/cast/cast.conf`. The application creates its
private runtime socket directory when no explicit socket or XDG runtime directory
is supplied. Commands from Terminal use the same daemon as the app.

Allow SuperScreencast in **System Settings > Privacy & Security > Screen & System
Audio Recording**. macOS may require quitting and reopening the app after the
first grant. Enable camera and microphone permissions only when using those
features. Their first enable requests permission and returns an error; after
approving the prompt, enable the device again. Use the `.app` for hardware testing so macOS can identify the app from
its bundle and usage descriptions. Rebuilding an ad-hoc signed app may require
permission to be granted again.

The capture backend supports the main display or numeric display IDs from
`cast screen list`. It captures at the display's pixel resolution, including
Retina displays, then uses the existing compositor to fit the output dimensions.
Cast's own application is excluded from screen capture when launched as a bundle.
This exclusion is not an arbitrary-window masking feature. Other applications
on the display remain visible.

Camera devices use AVFoundation unique IDs or `default`, rather than `/dev/videoN`.
Microphone capture uses the input selected in macOS Sound settings; its source
must be empty or `default`. System audio uses `desktop_source=default`.

## Verify on the MacBook

1. Run `make check-macos`; preserve its output if a build or check fails.
2. Open the app, grant screen permission, and verify its composition preview.
3. Record a display to MKV and to MP4. Stop and wait for finalization, then open
   each result in a player. MP4 uses a sibling `.mp4.mkv` during capture.
4. Enable the microphone and system audio separately, then together. Check that
   both are audible, gains work, and speech stays synchronized with video.
5. Enable the camera and exercise hide/show, layouts, and privacy pause/resume.
6. Check that the app panel is excluded from the captured display. Test an
   external display, Retina scaling, permission revocation, and sleep/wake.
7. During a disposable recording, terminate the capture daemon. Confirm the MKV
   recovery file remains; recovery of every frame is not guaranteed.

Share the first compiler error and the output of `sw_vers` and `uname -m` if the
build fails. Runtime diagnostics are available through the panel and `cast doctor`.
No SSH connection is required for this manual validation loop.

## Scope and remaining work

### Native validation checkpoint (2026-10-16)

On macOS 27.0.1 (26A434), arm64, using Command Line Tools and the macOS SDK,
`make -j4`, `make check-macos`, and `make macos-app` pass with Homebrew FFmpeg
9.0.2, SDL3 3.4.18, and SDL3_ttf 3.2.2. Core configuration/privacy tests,
compositor visual tests, Darwin IPC tests, and the synthetic recorder smoke test
pass. The app bundle passes `codesign --verify --strict` and opens its SDL panel.

Native testing found and fixed unavailable secure-wipe API use, a
platform-dependent invalid-config fixture, text centering with positive glyph
bearings, and virtual-camera activation incorrectly succeeding with null output.
The build now checks dependencies before compiling and invalidates object files
when dependency versions or flags change.

Hardware validation remains pending the Screen Recording permission grant:
the daemon correctly refuses capture without it. Display frames, panel exclusion,
camera, microphone, system audio, physical A/V sync, permission revocation, and
sleep/wake have not yet been established by these synthetic checks. Linux tests
have not been rerun on this Mac; Linux's secure-wipe path remains unchanged.

Available adapters: display capture through ScreenCaptureKit, camera through
AVFoundation, default microphone through AVAudioEngine, system audio through
ScreenCaptureKit, composition, recording, and the existing control panel.

This port does not implement virtual camera, virtual microphone, RTMP streaming,
window/region selection, global input annotations/hotkeys, or the separate native
preview/countdown overlay. The control panel's composition preview is separate
from that overlay. Unsupported operations report errors. Pro is not part of this
initial build.

Before distributing beyond a local developer build, validate on hardware, bundle
and audit dylibs/licenses, add a proper app icon and permission onboarding, and
add Developer ID signing/notarization. Virtual camera requires a separate Core
Media I/O camera extension; virtual audio also needs a separate device solution.

References: [ScreenCaptureKit](https://developer.apple.com/documentation/screencapturekit),
[Homebrew SDL3_ttf](https://formulae.brew.sh/formula/sdl3_ttf), and
[Apple capture authorization](https://developer.apple.com/documentation/avfoundation/requesting-authorization-to-capture-and-save-media).
