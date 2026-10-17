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

If permission appears enabled but the app still reports it is required after a
rebuild, quit the app, remove its old entry with the minus button in Screen &
System Audio Recording, then add the current `build/macos/SuperScreencast.app`
with the plus button and enable it again. Reopen the app. The privacy log may
report `Failed to match existing code requirement` when a grant belongs to an
older ad-hoc signature. Keep the bundle unchanged during hardware validation;
granting an older build while a new build is being packaged will not authorize
the replacement. Camera and microphone permissions can also need renewed grants
after the executable changes.

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

### Preview window

Click **Preview off** in the panel to open the native, resizable preview window,
or run `cast preview on`. On macOS the default target is **Composition**: an
explicit local view for framing before recording starts. It does not start a
recording or enable a virtual output. Group privacy pause replaces it with the
neutral Paused frame. **Recording** selects the actual recording lane, including
its stopped, paused, frozen, and blurred states; it stays neutral when stopped.
Use the two controls in the preview window, or `cast preview target composition`
and `cast preview target record`, to select the mode. Closing the window turns
preview off, and the panel button opens it again. Countdown uses the same window
temporarily; closing it cancels countdown. A preview enabled before countdown
stays enabled afterward. Preview windows are excluded with the app's other
windows when using the bundle.

`make check-macos` includes native AppKit tests for copied frame ownership,
RGBA orientation, letterboxing, privacy replacement, target selection,
close/reopen, and countdown. Synthetic daemon checks exercise preview commands
and recording countdown without requesting hardware permissions.

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

Hardware checks on the MacBook established display capture, the built-in camera,
microphone, system audio, MKV recording, and lossless MP4 finalization. The panel
was kept visible during a camera/microphone test and was absent from the recorded
desktop. A visible clap and its audio transient aligned within about 100 ms in a
short recording; this is not a long-duration drift measurement. System audio was
also tested separately with the microphone off. Privacy pause produced the
neutral Paused frame and exactly silent decoded audio while a tone was playing;
resume restored audio. Closing the panel left the daemon alive, and reopening it
reattached. SIGKILL during a disposable hardware recording left a decodable MKV
with 179 video frames and audio.

Native testing also found that CGDisplayPixelsWide/High returned logical Retina
dimensions (1512x982). The capture configuration and screen list now use the
active display mode's physical pixels (3024x1964), with the existing 8192-pixel
bound. The fixed build was rebuilt, checked, packaged, granted screen permission,
and used for another hardware recording. Reapplying microphone settings now
restarts an engine that has stopped after an interruption.
After renewing camera/microphone and Documents-folder grants for that same final
executable, a further seven-second MP4 finalized with 210 decoded video frames,
329 decoded AAC frames, visible camera video, and a captured system test tone.
Both audio inputs and the camera were disabled after verification.

Subsequent decoded-waveform inspection found intermittent gaps in the recorded
system test tone, including with the microphone disabled. Audio presence and
privacy checks passed, but continuous audio quality is not yet validated; this
macOS issue remains open. The short clap alignment check does not establish
continuous audio delivery.

The daemon correctly refuses capture without a matching permission grant. Live
permission revocation, sleep/wake, external displays, sustained performance, and
long-duration A/V drift remain untested. Linux tests have not been rerun on this
Mac; Linux's secure-wipe path remains unchanged. Homebrew runtime libraries here
require newer macOS than Cast's nominal source deployment target.

Available adapters: display capture through ScreenCaptureKit, camera through
AVFoundation, default microphone through AVAudioEngine, system audio through
ScreenCaptureKit, composition, recording, and the existing control panel.

This port does not implement virtual camera, virtual microphone, RTMP streaming,
window/region selection, or global keyboard annotations/hotkeys. Native preview and
countdown windows are supported. Unsupported operations report errors. Pro is supported for local development with controlled LGPL media and CPU
Whisper inference; see the sibling private repository’s docs/development.md.
Manual cinematic zoom/focus uses the shared Pro renderer. The capture adapter
also provides selected-display pointer coordinates, including Retina scaling,
for cursor following. Automatic click zoom uses a listen-only mouse event tap;
grant Input Monitoring to the app and restart capture to enable it. Permission
denial leaves manual zoom and pointer following available. Physical click
capture still needs permission-granted hardware acceptance. The captured cursor
is embedded in the screen image, so independent cursor smoothing is unavailable.

Before distributing beyond a local developer build, validate on hardware, bundle
and audit dylibs/licenses, add a proper app icon and permission onboarding, and
add Developer ID signing/notarization. Virtual camera requires a separate Core
Media I/O camera extension; virtual audio also needs a separate device solution.

References: [ScreenCaptureKit](https://developer.apple.com/documentation/screencapturekit),
[Homebrew SDL3_ttf](https://formulae.brew.sh/formula/sdl3_ttf), and
[Apple capture authorization](https://developer.apple.com/documentation/avfoundation/requesting-authorization-to-capture-and-save-media).

## Pro development and standalone bundles

The sibling Pro checkout now integrates through `Makefile.macos`. Build the
controlled SDKs explicitly, then use `make pro-dev` from the private checkout.
The default macOS development SDK locations are `build/deps-lgpl-macos` and
`build/deps-stt-macos/profile.json` in the public checkout. System/GPL FFmpeg is
rejected for Pro, and macOS official Pro distribution remains disabled.

`make macos-app MACOS_STANDALONE=1` copies the complete linked dylib closure,
rewrites library paths, includes dependency notices, creates the website icon,
and ad-hoc signs `build/macos/Super Screencast.app`. The app records the highest
minimum macOS version among its runtime libraries, which can exceed the source
deployment target. For Pro, pass the same `EDITION=pro MEDIA_PROFILE=lgpl` and
speech SDK variables as the executable build; its bundle also includes the speech
helper and controlled CPU module. These apps are not notarized.
