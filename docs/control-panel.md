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

The daemon owns all session settings. The panel uses the acknowledged command
interface; CLI and key-binding changes appear in its state updates. Draft text
fields apply explicitly and commands never write the user's configuration. Live,
recording and group-pause controls preserve their independent semantics.

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
rule in a tiling window manager; the panel stays mapped throughout capture.

Wayland support for creating the panel does not imply capture exclusion. The
compositor/portal controls what is captured; exclusion is reported unsupported.
Place the panel on an uncaptured monitor or select only the application you present.

## Boundaries

Commands and frame transport are bounded; a slow or disconnected panel cannot
block the daemon's capture loop. The preview shows the selected live or recording
composition and clears when its daemon disconnects. Unsupported backend actions
are disabled or return actionable errors. Custom drawn controls provide keyboard
focus and interaction; screen-reader integration is not implemented.
