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
Private frame sharing requires Linux 5.1 or newer.

The daemon owns all session settings. The panel uses the acknowledged command
interface; CLI and key-binding changes appear in its state updates. Draft text
fields apply explicitly and commands never write the user's configuration. Live,
recording and group-pause controls preserve their independent semantics.

Home shows the current output and the controls appropriate to its state. Open
Source, Camera, Audio, Effects or Settings to edit one section at a time; Back or
Escape returns Home. Advanced options expand within their section. The preview
stays visible while navigating, becomes compact on settings screens and can be
dragged by its header. Essential navigation, privacy controls and capture warnings
remain unobscured. Select Live or Recording to inspect that output's composition.

Square buttons pair text with consistent drawn line icons; the Back control uses
a left arrow and a clear Back label. Icons never replace action names.

Tab and Shift+Tab move focus; Enter applies a field, Enter/Space activates buttons,
and arrow keys choose dropdown entries. Ctrl+Q closes only the panel. The optional
`make X11=1 WAYLAND=1 PANEL=1 check-panel` exercises these controls against a
synthetic daemon on a private display; it requires Xvfb, xdotool, xprop and xclip.

## Recording countdown

Set a recording countdown in Settings or use `cast --countdown 3` at startup.
Starting a recording shows a local numbered film-style guide in the persistent
preview on every screen. Cancel stops the pending start; `cast pause` also cancels
it. The guide disappears when recording begins and is never burned into outgoing
video or the recorded file. On Xorg, a separate centered guide appears for
CLI starts when the panel is closed, including when the preview is closed. Escape or closing
that guide cancels the countdown. Wayland has the panel guide; a standalone
countdown window is not implemented there.

## Custom paused message

```sh
./cast live message "Back in five minutes"
./cast live message ""  # neutral background without a label
```

In the panel's Settings section, edit the paused-message field and apply it.
Changing the message never resumes video or changes recording state. For a
persistent default, set `pause_text` in the configuration's `[output]` section.
The outgoing pause frame uses cast's bundled bitmap font; its supported glyphs
are more limited than the panel's Inter font. Messages fit the existing 127-byte
configuration limit.

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

Commands and frame transport are bounded; a slow or disconnected panel cannot
block the daemon's capture loop. The preview shows the selected live or recording
composition and clears when its daemon disconnects. Unsupported backend actions
are disabled or return actionable errors. Custom drawn controls provide keyboard
focus and interaction; screen-reader integration is not implemented.
