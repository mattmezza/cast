# cast native workspace preview

The CUI redesign now has a working workspace shell based on OpenDesign's
`Super Screencast Redesign / cast-app.html` and `cast control panel / DESIGN.md`.

```sh
make -C ui
./ui/build/cast-ui-preview
make -C ui check
make -C ui check-native
```

Virtual camera, Record and Stream share one composition model and a persistent
16:9 stage. Layout, fit, camera placement and size are local session choices.
Each workspace has its own source/output context. Side panels scroll independently,
can be hidden on desktop, and open as modal drawers at narrow widths. Settings
contains System/Light/Dark appearance and ten categories in a custom rail.
Only appearance changes locally; backend forms are disabled. Connected Settings
shows safe acknowledged backend, container, codec, frame-rate and service values. Unknown system appearance
falls back to dark. Preferences last for this process; no configuration is saved.

Keyboard: `1` / `2` / `3` switch workspaces; left/right arrows navigate focused
workspace tabs. `[` / `]` toggle the source/output panels. `Ctrl+,` (`Cmd+,` on
macOS) opens Settings. Escape closes a drawer or Settings and returns focus to
its trigger. Tab/Shift+Tab stay in an open modal.

The standalone executable is a disconnected fixture. To attach the same UI to an
existing daemon, build the optional integration and launch it explicitly:

Build the same executable for both processes, from the repository root:

```sh
# Linux: select the capture backends installed on your machine.
make CUI=1 PANEL=0 X11=1 WAYLAND=0
# macOS: uses Makefile.macos automatically; macOS >= 13.
# make CUI=1 PANEL=0
```

`CUI=1` requires `PANEL=0`: the legacy panel and prototype use incompatible
Clay versions. On macOS, `make -C ui check-native` opens brief Cocoa windows on
the logged-in desktop and validates rendering at the display's pixel scale.
See [macOS prototype setup](../docs/macos.md#try-the-redesign-prototype).

In terminal 1, run a foreground daemon using your existing capture configuration:

```sh
./cast --config /absolute/path/to/cast.conf --headless
```

In terminal 2, attach using the same binary, configuration and socket:

```sh
./cast --config /absolute/path/to/cast.conf workspace
```

Omit `--config` in both terminals to use the default configuration. The default
socket uses a private runtime directory. A custom `--socket` must have a user-only
parent directory (mode 0700), and must match in both processes.

Stop the foreground daemon with Ctrl+C in terminal 1. Closing the workspace leaves
it running. Keep both processes on the same build; rebuild after stopping them.
`CUI=1` requires the native preview dependencies below in addition to the recorder's
platform dependencies. Linux Pro builds retain `EDITION=pro MEDIA_PROFILE=lgpl` and
the existing private-provider requirements. macOS Pro builds retain the controlled
LGPL media and private-provider requirements documented in the sibling repository.

For an isolated preview without a capture/output device, use the synthetic setup
from `test_live.py`. Create this configuration once:

```sh
mkdir -p /tmp/cast-workspace-demo
cat > /tmp/cast-workspace-demo/cast.conf <<'EOF'
[output]
backend=synthetic
width=640
height=480
fps=15
[camera]
enabled=false
[composition]
layout=screen
EOF
```

Terminal 1:

```sh
./cast --config /tmp/cast-workspace-demo/cast.conf \
  --socket /tmp/cast-workspace-demo/daemon.sock \
  --output-device none --no-camera --headless
```

Terminal 2:

```sh
./cast --config /tmp/cast-workspace-demo/cast.conf \
  --socket /tmp/cast-workspace-demo/daemon.sock \
  --output-device none --no-camera workspace
```

This uses a generated image; it does not enable recording or streaming. Ctrl+C in
terminal 1 stops the daemon. The automated Linux test uses a unique temporary
directory and a private Xvfb rather than these illustrative fixed paths.

The daemon and workspace must use the same build. `workspace` never starts a daemon
and never activates an output automatically. Layout, contain/cover, camera anchor
and camera size edits are sent only after explicit interaction, and selections
follow acknowledged daemon state. It shows a private live composition even while outputs
are paused or stopped; outgoing feeds remain privacy-gated. A missing webcam
does not block screen composition. It reads acknowledged composition, output/privacy and
edition state, and displays the same private composition across all workspace tabs.
This is a composition preview, rather than a monitor of each outgoing feed's
pause, freeze, blur or lane-specific annotations.
The private preview preserves source detail up to 1920×1080; outgoing preview slots
remain capped at 640×360. Source images smaller than the displayed stage still
require upscaling. Routine command barriers render the current composition, while
source/privacy invalidation clears the image until a fresh capture arrives.
Frames are removed on disconnect, target changes, failed validation and generation
or privacy changes. Snapshot reads serialize with short worker metadata updates;
ordinary lock contention does not clear the preview. Pixels use a read-only shared
memory mapping; the socket carries attachment and control messages. Connection attempts retry automatically; only one control panel
can attach to a daemon at a time. Composition controls disable while an edit is
pending or the daemon is disconnected. Failed edits display a notice; pending edits
are discarded across connection changes and are never replayed. The center camera
anchor is unavailable because the current daemon supports eight edge positions.
Capture source and camera selectors discover real devices asynchronously. Xorg and
macOS display lists can be selected; Wayland offers the desktop consent picker.
Synthetic capture has no display selector. Camera choices include No camera.
Selections change only after daemon acknowledgment; unavailable, failed and empty
lists remain explicit, with Retry on discovery failure. Linux V4L2 paths and macOS
camera identifiers are parsed separately. Remaining configuration forms and output
actions are still unavailable.

The Look section edits the actual screen backdrop: Blur, Gradient or Solid.
Solid fill and gradient stop colours accept `#RRGGBB` on Enter; the swatch shows
acknowledged colour. Existing gradients retain their optional middle stop and
angle, rather than silently replacing the configured gradient. Changes apply to
the running session; saving presets and persistent settings remain later work.
Backgrounds appear in the space around the captured screen, according to the
current composition geometry.

Basic zoom is free. The Director controls and stage Zoom popup use acknowledged
zoom factors and the daemon's minimum/maximum bounds. Pointer following is enabled
only when the capture backend supplies cursor metadata. Cinematic zoom status
comes from the actual edition capability and active motion state; installing a
Pro build alone does not imply entitlement. Cinematic motion configuration and
screen-only mode controls remain unfinished.

Linux/macOS capture and Community/Pro capabilities remain unchanged. Recording,
single-destination streaming and existing zoom remain free; cinematic zoom,
teleprompter and subtitles remain Pro. Embedded mode bundles the same Inter and IBM Plex Mono fonts
inside the executable. The existing panel remains the default application UI.

Settings opens as an in-window modal. File/Edit/View/Composition/Output/Help
menus support local panel visibility and layout choices, Settings navigation and
workspace Quit. Capture/output commands and other unavailable actions remain
disabled. macOS retains these in-window menus until a native global-menu adapter
exists. Detached Settings, OS title-bar integration and acknowledged backend
settings editing and output commands remain later milestones. The pinned SDL adapter excludes
native move/system-menu dispatch; its renderer also has no backdrop-blur command.
See [the fidelity audit](../docs/ui-fidelity.md) for the API evidence and remaining
differences from the browser reference.

## Build and verification

Dependencies: C11 compiler, GNU make, ar, pkg-config, SDL3 >= 3.2, FreeType and
HarfBuzz. Fonts (Inter Regular/Medium/SemiBold/Bold and IBM Plex Mono Regular/Medium/SemiBold)
and CUI sources are bundled with their licenses.
The build does not download dependencies or launch a window. The executable loads
fonts relative to its path, so it also works from another working directory.
Keep `assets/` beside `build/`, or pass `--font` and `--bold-font` explicitly for those two faces; medium, semibold
and mono faces still load from the bundled asset directory.

`check` exercises pointer/keyboard workspace changes, shared composition state,
scrolling, panel visibility, drawer resizing, modal focus and disabled output actions.
It checks stage/panel/status bounds at 480, 800, 1000 and 1360px widths and 360/820px
heights. `check-native` needs Python3, Xvfb and xauth on Linux; it always uses a
private Xvfb and SDL software renderer. Ten native cases cover all workspaces in
both themes and narrow/settings layouts, using real bundled fonts. Temporary
captures are deleted automatically after validation. macOS runtime and hardware
GPU validation remain outstanding for these new controls.

On Linux, after building `CUI=1`, `python3 ui/test_look.py ./cast` drives Background
and Zoom through real native clicks on a private Xvfb. It checks daemon
acknowledgment, reopening/external synchronization, idle output state while editing,
and a short isolated recording whose decoded backdrop matches the selected fill.
It needs xdotool and ffmpeg in addition to the native smoke dependencies. Temporary
captures and recordings are removed after the run.

`make -C ui clean` removes the preview build and any retained review image.

For a bounded visual inspection on a private display:

```sh
SDL_RENDER_DRIVER=software xvfb-run -a ./ui/build/cast-ui-preview \
  --theme dark --workspace record --size 1360x820 \
  --frames 5 --capture /tmp/cast-workspace.bmp
```

Other flags: `--theme system|light|dark`, `--workspace camera|record|stream`,
`--size WIDTHxHEIGHT` (480..3840 by 360..2160), `--settings`, and
`--debug-layout text|json`. Capture requires a positive frame limit. Debug layout
exports the first completed frame; capture saves the final rendered frame.

`app.c` owns portable workspace composition, `settings.c` owns the modal,
`menus.c` composes the six application menus, `cast_theme.c` maps the design tokens, and `main.c` handles the native host, fonts,
OS appearance and Ctrl/Cmd-comma shortcut. `daemon_bridge.c` sanitizes daemon state
and borrows preview pixels through `live.h`. It queues allowlisted composition
edits and source discovery/selection through the existing worker and observes command acknowledgments.
`source_picker.c` renders the reference combo trigger and floating list. No portable
UI module includes SDL.

## Pinned dependency

CUI is pinned to `9d4914830f29bcdd8b12517f21841fbdae17f631` using its standalone
native source bundle. `.cui-lock.json`, `.cui-resources.json`, `bundle-manifest.json`
and dependency manifests preserve the original snapshot's provenance, checksums
and matching API references. Application files, README and Makefile are intentionally
adapted, and the unused upstream mail demo was removed. `vendor/` and the original Inter regular/bold assets retain upstream bytes;
additional font instances/conversions are recorded in `assets/fonts/manifest.json`.
Keep application changes outside `vendor/`. The build uses
SDL_Renderer, FreeType/HarfBuzz and vendored SheenBidi; optional GPU, accessibility
platform adapters and regex providers are not linked. CUI remains experimental.

The real transport integration can be exercised with `python3 ui/test_live.py ./cast`
after a `CUI=1` build. It uses private Xvfb displays and temporary synthetic/Xorg daemons and sockets,
checks attach/frames/detach/re-attach/disconnect, and removes its temporary files.
`--debug-live` emits a small final connection/frame summary without configuration
secrets. macOS build integration is provided; runtime verification needs macOS.

The font manifest records provenance and hashes: Inter regular/bold are bundled
4.1 OTFs; Inter 500/600 are static instances of the browser reference's variable
4.001 font. IBM Plex Mono 400/500/600 are decompressed reference assets, registered
as family 1 with the monospace flag. Both families retain their OFL notices.
Missing required faces stop startup; browser and native text rasterization can
still differ even when their font source matches.

## Connection diagnostics

Connection failures appear in the workspace notice and output card, and are printed
once per changed error in the workspace terminal. A protocol/build mismatch means
both processes must restart from the same rebuilt executable. Another attached
control panel must close before a workspace can attach.

Xorg capture authenticates the workspace's X11 window class, type and owning PID
before excluding it from capture. The embedded host registers `CastPanel` (or
`CastProPanel`) and a normal window before attachment. Window managers can tile
it alongside other application windows. A webcam warmup message
such as “waiting for fresh frames” is independent of this attachment handshake.

The integration test checks default socket discovery in a private runtime directory.
Run its real Xorg path on a private Xvfb with
`CAST_UI_TEST_BACKEND=xorg python3 ui/test_live.py ./cast`; omit the environment
variable to run both capture backends. Both paths leave the user's desktop and devices alone.

Composition editing can be checked with `python3 ui/test_composition.py ./cast`
after building both `make CUI=1` and `make -C ui`. This requires `xdotool` and
uses a private Xvfb and temporary synthetic daemon. It exercises real clicks,
menu navigation, camera position/size, acknowledged values after reopening the
workspace, and external CLI changes while outputs remain idle.

Source selection integration (private Xvfb, no real webcam opened):

```sh
python3 ui/test_sources.py ./cast
```

The reference combo is 36px tall with a trailing chevron, a 4px popup inset,
32px rows and a maximum height of 264px. It supports keyboard selection, Escape
and outside dismissal, focus restoration, and selected checkmarks. Device commands
share the bounded pending queue; discovery and choices are discarded on reconnect.
