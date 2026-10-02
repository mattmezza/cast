# Native control panel

The optional control panel uses Clay for layout, SDL3 for its native window and
rendering, and SDL3_ttf for font rendering. It runs as a separate client process
from the same executable. Closing the panel leaves the capture daemon running.

## Build and launch

On Arch Linux, install the optional development/runtime dependencies yourself:

```sh
sudo pacman -S --needed sdl3 sdl3_ttf
make -j X11=1 WAYLAND=1 PANEL=1
./cast --config examples/cast.conf
# In another terminal, using the same instance/config selection:
./cast --config examples/cast.conf panel
```

`PANEL=0` remains the default and removes all SDL dependencies. `PANEL=1` enables
the panel on either backend. `--socket PATH` selects the same daemon instance as
other commands. The panel does not start capture or resume live output by opening.
Its font is embedded, so installation and relocated packages do not depend on an
asset path. `make PANEL=1 install` and `make PANEL=1 package` include the relevant
license notices. Distribution packages may opt into the panel explicitly.

The Arch package is named `sdl3_ttf`; its pkg-config module is `sdl3-ttf`.
If the build reports that module missing, install the package above and check
`pkg-config --modversion sdl3 sdl3-ttf`. SDL2_ttf cannot satisfy this dependency.

The daemon owns all session settings. The panel uses the acknowledged command
interface; CLI and key-binding changes appear in its state updates. Draft text
fields apply explicitly and commands never write the user's configuration. Live,
recording and group-pause controls preserve their independent semantics.

Home provides Live and Recording control groups and Source, Camera, Audio,
Effects and Settings navigation. Back or Escape returns Home. Advanced options
expand within their section. Essential privacy, recording and navigation actions
remain outside scrolling content. A wheel notch scrolls three times faster than
before; precise touchpad deltas and natural-scroll direction remain supported.

The panel contains no embedded video preview. **Floating preview** controls the
separate movable CastPreview window, with On/Off and Live/Recording targets on
every screen. Target changes use the same daemon settings as the CLI. A successful
recording action selects Recording automatically; Start live selects Live. Failed
commands preserve the target, and a manual selection overrides a pending automatic
change. Unsupported backends disable the window toggle and show a capability
explanation; the target selection can still be saved.

Home contains independent lane freeze/blur and recording pause/cut controls.
Solid recording pause writes a screen with silence; cut removes time from the file.

Square buttons pair text with consistent drawn line icons; the Back control uses
a left arrow and a clear Back label. Icons never replace action names.

Tab and Shift+Tab move focus; Enter applies a field, Enter/Space activates buttons,
and arrow keys choose dropdown entries. Ctrl+Q closes only the panel. The optional
`make X11=1 WAYLAND=1 PANEL=1 check-panel` exercises these controls against a
synthetic daemon on a private display; it requires Xvfb, xdotool, xprop and xclip.

## Recording countdown

Set a recording countdown in Settings or use `cast --countdown 3` at startup.
The panel shows seconds remaining on every screen. Cancel stops the pending
start; `cast pause` also cancels it. Cut-resume uses the same countdown without
creating a new file; Cancel resume leaves the recording cut.

On Xorg, the separate CastPreview window temporarily opens with the composition
beneath a film-style guide, even with preview off and the panel open. Its normal
panel On/Off toggle is locked and labeled **Countdown preview** until the guide
finishes. The guide is never burned into outgoing video or the file. At the end
it disappears, including WM decorations, before media admission starts/resumes.
This also closes a preview that was previously enabled. Escape or closing the
window cancels the pending action. A WM frame that fails to hide within the
bounded barrier cancels start/resume. Wayland has the panel's plain countdown
status; a native floating preview/countdown window is not implemented there.

## Pause and blur styles

```sh
./cast live message "Back in five minutes"  # shared solid-pause title
./cast settings output.pause_subtitle "{date:%A} · {time:%H:%M}"
./cast settings output.blur_title "Break" output.blur_subtitle "{datetime}"
```

In Settings, expand **Pause screen** or **Blur screen** to edit titles/subtitles/footers,
colours, text sizes, text gap, blur radius and tint opacity. Apply or Enter submits a draft;
blank titles, subtitles and footers are valid. Style edits never change output flags or
write the config file. Templates support [local date/time formats](configuration.md#pause-and-blur-text).
System Noto Sans renders the exported text by default. Font choices are available
only through `pause_font` / `blur_font` in the `[output]` config section, followed
by config reload. Each disclosure shows the selected font and reload guidance.
The panel itself continues using bundled Inter.

Camera appearance includes a camera-slot background selector. The default uses
an enlarged, heavily blurred and dimmed camera frame behind uncovered camera-slot
pixels; Gradient is the subdued fallback, and Solid restores a uniform backdrop.
These controls leave screen-layer letterboxing and the foreground camera unchanged.

## Capture visibility

On Xorg, selected-application capture excludes the separate panel. For monitor or
region capture, place the panel outside the source when possible. Overlap is
covered with the configured neutral colour, including window-manager decorations;
the underlying obscured screen content cannot be reconstructed. The panel reports
the exclusion state. A stable `CastPanel` window class permits an optional floating
rule in a tiling window manager. The panel also declares the Xorg utility window
type before mapping. Window managers such as mwm that float utility windows
automatically center it without an extra class rule. The panel stays mapped
throughout capture.

Wayland support for creating the panel does not imply capture exclusion. The
compositor/portal controls what is captured; exclusion is reported unsupported.
Place the panel on an uncaptured monitor or select only the application you present.

## Boundaries

Commands and state updates are bounded; a slow or disconnected panel cannot
block the daemon's capture loop. The panel consumes snapshots and commands only,
without requesting output frames. Unsupported backend actions are disabled or
return actionable errors. Custom drawn controls provide keyboard focus and
interaction; screen-reader integration is not implemented.
