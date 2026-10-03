# Capture, composition and presentation

`cast` keeps the configured output dimensions and cadence when its capture source
changes. Screen, camera and annotations are composed as owned RGBA8 frames; capture
errors use the configured neutral frame and report an error in status. A destroyed
window, disconnected monitor or invalidated region never selects a broader source.
Camera-only composition can continue without a desktop source.

## Coordinates and fitting

Xorg monitor names and rectangles come from XRandR. Region `X Y WIDTH HEIGHT`
coordinates are relative to the selected monitor, including monitors whose desktop
origin is negative. Window capture contains the client drawable, excluding window
manager decorations. Moving the window updates its desktop origin; resizing changes
the source dimensions. The X server already presents monitor rotation in root
coordinates, so cast does not apply a second rotation to Xorg pixels.

At the platform boundary, cursor and click coordinates become source-local pixels
by subtracting the source's desktop origin. Composition crops a zoom viewport from
that source, then fits it to the screen's output rectangle. `contain` preserves all
viewport content with neutral-colored letterboxes; `cover` centers a crop that fills
the rectangle. A source point `(x,y)` maps to
`(dx + (x-sx)*dw/sw, dy + (y-sy)*dh/sh)` for the final source/output rectangles.
Cursor highlighting and click centers use that same transform; points outside the
visible source crop are ignored. Cursor/label sizes and ring radii use output pixels.
Screen scaling currently uses nearest-neighbor sampling.

Overlay puts the screen across the canvas and the camera over it. Split reserves
the configured width percentage on the left or right for the camera and fits the
screen to the remainder. Hiding the camera retains that split allocation and camera
geometry. Screen and camera layouts show one source. Camera content in split/camera
layouts fits inside its allocation; overlay geometry remains available when returning
to overlay.

## Camera geometry

Camera width is a percentage of canvas width; `+5%` adds five percentage points.
Corner anchoring fixes the chosen corner at the configured margin. Free positioning
uses top-left output coordinates, and the size command preserves its center where
bounds permit. Geometry is clamped to the canvas and its margins; impossible geometry
is rejected. Corner cycling follows `[camera] corner_order`.

Native, 16:9, 4:3 and 1:1 select a centered camera crop, with offsets measured in
camera source pixels and clamped at crop edges. Circle always uses a square crop.
Rectangle, rounded rectangle and circle determine the output mask independently
of crop aspect. Rounded radius and border width use output pixels. Mirroring changes
camera content only. Defaults are overlay, rounded, bottom-right, 22% width and a
24-pixel margin. Camera hide and layout changes preserve the stored overlay geometry.

## Selection and unavailable sources

On Xorg, `capture region select` temporarily grabs the pointer and keyboard while
the user drags within the selected monitor; `capture window select` uses a click.
Ordinary annotation observation uses passive XI2 events and does not grab input.
Escape releases selection grabs and retains the prior source and zoom. A successful
selection commits to the daemon's stable configuration object and increments the
source generation. Invalid numeric requests leave the current source unchanged.
Successful source changes clear annotations and queued media, and reset zoom;
starting or cancelling selection does not commit a source change.

Window capture uses an XComposite named pixmap so overlap by other windows does not
select or capture those windows. Resizing and remapping reacquire the backing pixmap.
Minimization/unmapping and destruction produce neutral output with actionable errors.
Monitor availability and region bounds are checked on each acquisition. Interactive
selection and capture do not change either output's privacy or recording state.

## Stage and screen styling

Stage gives the screen an inset frame, anchored opposite the camera. Screen size
uses `[screen] width_percent`; the camera retains its normal canvas-width sizing
and anchor. Both preserve their respective aspect ratios. Opposite placement puts
smaller sources beside each other and larger sources overlap with camera in front.
Corners move the screen toward the opposite corner; edge anchors move it across
that axis while keeping it centered on the other. The screen stays independent of
camera mirroring, and zoom/pointer/click positions use its actual screen transform.

`screen.radius`, `screen.border_width` and `screen.border_color` style the screen
layer in stage, overlay, split and screen layouts. Radius and border are output
pixels. Zero defaults keep the existing square unbordered screen. Screen styling
does not scale the camera, output labels or branding.

Screen/stage backgrounds have blurred, gradient and solid modes. Blurred mode
uses the current screen frame by default; `[background] source = camera` selects
the camera. Processing is reduced to a bounded working size before enlargement,
with configurable brightness, tint and blur, so the backdrop tracks fresh source
frames. Camera-slot backgrounds continue to use the camera. Missing sources use
an available neutral backdrop rather than keeping an old image.

Gradient backgrounds share two endpoints and an optional third middle stop,
with an angle and a percentage waypoint. Camera gradient backgrounds use the same
configuration. The configurable background fills the uncovered area beneath the
inset screen in stage.

## Logo and static text

Logo and static text draw as output composition layers, after the screen and
camera. They support all four corners and middle edge anchors, independent
horizontal and vertical edge distances, and opacity. Image sizing preserves
aspect ratio and its own alpha; a transparent PNG is a suitable logo source.
Only local image files are accepted. Static UTF-8 text uses Fontconfig/FreeType,
with a runtime-selected font, output-pixel size and color. It stays literal;
pause/blur date and time templates remain specific to those effects.

Privacy effects operate on the complete composition: freeze holds branding,
blur includes it, and solid pause replaces it. Editing presentation settings
never resumes outputs or changes recording state. Session commands do not write
configuration files.

## Zoom and cursor

Zoom changes the screen layer only. The default easing is a 250 ms smoothstep
transition, with configured limits and steps. Locked mode targets the cursor when
the zoom factor changes, then holds that viewport. Follow mode uses a dead zone and
time-adjusted exponential smoothing; both clamp the viewport at source edges.
Leaving the source holds the view until valid cursor metadata returns. Following
advances once for a capture timestamp so live and recording retain identical screen
pixels even when their annotation switches differ.

Xorg root/pixmap capture excludes the server cursor; cast draws a simple arrow using
the separately queried pointer position. Cursor size/color and highlighting are
configurable. It does not reproduce each application's themed cursor shape. Backend
capabilities determine whether metadata is available or the cursor is embedded;
embedded cursor capture disables the separate arrow to avoid duplication. Refer to
[wayland.md](wayland.md) for portal-specific restrictions.

## Clicks and keys

Left/right click rings expand and fade, with independent colors; middle clicks are
optional. Xorg raw button events contain no desktop position, so cast queries the
pointer when draining the event. Very fast pointer motion before the drain can shift
the ring relative to the original click. Coordinates outside the selected source are
ignored. At most 32 animations are retained in memory.

Keys default off; enabling them defaults to shortcuts mode. That mode displays
Ctrl/Alt/Super combinations and configured navigation keys. All mode also displays
printable key symbols and can expose sensitive typing. Translation follows XKB map,
group and modifier notifications. Consecutive identical labels collapse into one
entry: `j`, `jx2`, `jx3`. Every observed raw press counts, including flagged repeats
where available. Server-generated software autorepeat may not reach the global
raw-event stream; cast does not invent events based on keyboard repeat rates.
Plain alphabetic symbols retain
their translated case in all mode; shortcut labels use uppercase alphabetic names.
Combination order is `Ctrl+Alt+Super+Shift+Key` and space is `Space`. `[keys] filter`
accepts complete labels such as `Super+Shift+Space`, or individual key labels.
Configure it for cast's own bindings and any other shortcuts that should not be shown.

A bounded recent-key queue retains independent labels for `[keys] timeout_ms`
(default 3000 ms). Repeating the newest key refreshes that entry's lifetime; older
entries expire independently. Entries stack inward from the configured corner,
with the newest at the anchor. The queue is temporary annotation state, not a raw
input log. The bundled original bitmap font covers ASCII; other XKB key symbols use
their symbol names or a `U+...` label. Compose/dead-key sequences and IMEs are not
committed text observation. All mode is not a faithful transcript of text entered
by an app, and cast does not inspect password fields.

Privacy/source boundaries synchronize and drain pending Xorg input, clear labels
and rings, and discard input on the resume boundary. Modifier state notifications
continue to be consumed while paused. If one independent output remains active,
annotations can continue for that output; privacy-paused output frames remain neutral.
Live and recording key/click switches are independently configurable. `keys clear`
clears existing annotation memory without changing the enabled switch.

## Preview and recursion

The Xorg preview shows the selected live/record target with a local header containing
LIVE/PAUSED/FROZEN and recording state. Its header is drawn in its X window after
composition and is not added to the output frame. Preview submission follows the
configured output cadence. Painting runs on a dedicated X connection/thread with
one replaceable pending frame; a busy worker drops preview updates instead of
building a delay queue or stalling the virtual camera or controls. Conversion uses
cached image storage and a fast path for common Xorg visuals. The preview is a
managed utility window with `WM_CLASS` `cast-preview` / `CastPreview`. Window
managers that float utility windows (including mwm) center it automatically. Other
window managers can use the class or utility type for a floating rule. Drag its
header with the left mouse button to move it; use `cast preview off` to hide it and
`cast preview on` to show it. Window creation, source acquisition and shutdown still
depend on a responsive X server.

The preview stays mapped during capture and live/record/privacy changes. On a state
or source boundary, its existing contents are cleared to a neutral background while
the new state is painted. The worker prepares a server pixmap; the main connection
installs it only if it belongs to the current state epoch. An older pending update
cannot restore stale visible content. Initial mapping also waits for a complete
current frame, so the first display has its local header and image together.

Cast prefers preview placement on a monitor outside the captured root rectangle.
If it overlaps monitor/region capture, cast replaces its full window rectangle,
including the header and border, with the configured `pause_color` in the owned
capture frame before composition. Its current screen position and dimensions are
queried for each capture, so moving or resizing it moves that neutral rectangle.
The mask follows the source's zoom and fit into each output. This deliberately
covers the desktop content beneath the preview; cast does not recover hidden pixels.
Place the preview outside the captured source when all source content must remain
visible. The preview's status labels and image are never copied into this rectangle,
preventing recursive preview capture without per-frame unmapping or desktop flashes.
An XComposite application-window capture reads that application's named pixmap,
which excludes cast's separate preview window and requires no neutral rectangle.
The optional separate-process control panel uses the same source neutral masking on
Xorg. Only the authenticated panel connection can register an X window; cast checks
its `_NET_WM_PID` against the socket peer PID, its `CastPanel` class and its normal
window type. X11 window properties are integrity checks within the user's desktop,
not a security boundary against another client of that X server. The current enclosing
root child supplies the whole managed window footprint, including WM decorations and
border. Moving, resizing, hiding, reparenting or destroying the panel updates its
footprint; monitor and numeric-region edges clip the rectangle. Panel status reports
whether exclusion is available, the window is hidden or outside the source, or covered
content is being replaced. Separate application-window capture retains the application's
pixmap pixels even beneath the panel. The panel cannot be selected as a source while
registered, including selecting its enclosing WM frame. Closing its authenticated connection removes registration.

Panel and preview masking occurs before composition, so live, recording, zoom, fit,
freeze and the panel's program preview receive already-masked source pixels. This
covers underlying desktop content; it does not recover that content or remove arbitrary
windows. The panel remains mapped throughout ordinary acquisition. Wayland portals
provide no equivalent exclusion guarantee; see [wayland.md](wayland.md).

The interactive region-selection outline is still temporarily hidden during capture.
Inspect actual virtual-camera or recording pixels on the target desktop.

## Verification limits and commands

`tests/test_visual.c` checks actual synthetic frame pixels for geometry, layouts,
masks/borders, aspect crop/mirror, contain/cover, zoom, click transforms, key expiry,
annotation lane switches, neutral content and consistent dual-output following.
The visual suite passed address/undefined sanitizers in the earlier managed
workspace with `ASAN_OPTIONS=detect_leaks=0`; that environment prevented leak checks.
Post-reboot preview changes also passed isolated ASan/UBSan Xvfb exercises.

`tests/x11_smoke.c --exercise` is an acceptance test for a disposable Xvfb server.
It creates application windows and injects keys/clicks only into that isolated
display. It checks root/window pixels, resize/remap/minimize/destruction, numeric
and interactive regions, Escape cancellation, keymap/modifier/repeat/filter behavior,
input privacy, unrelated click rejection and preview exclusion. It compiles with
the project's warning flags. After reboot this complete suite passed with MIT-SHM enabled and disabled,
including stable mapped preview, dragging, neutral footprint/geometry and application
pixmap exclusion. The panel regressions also check authenticated PID/class/type rejection,
managed frame/border coverage, moved/resized and clipped geometry, exclusion in both
composed output lanes, reparenting, property identity revocation, disappearance and
unregister. Earlier sandbox socket/display restrictions no longer apply. Do not run
`--exercise` on the user's desktop.

To run on a system permitting a disposable X server, install Xvfb and the XTest
development package, then run `make X11=1 WAYLAND=1 check-xorg`, or compile the
Xorg-only smoke test directly:

```sh
cc -Isrc -D_GNU_SOURCE -DWITH_X11 -O2 -g -Wall -Wextra \
  -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -std=gnu11 \
  tests/x11_smoke.c src/x11.c src/platform.c src/compositor.c \
  $(pkg-config --cflags --libs x11 xext xrandr xi xfixes xcomposite xtst) \
  -lm -lpthread -o /tmp/cast-x11-smoke
xvfb-run -a -s '-screen 0 1024x768x24' /tmp/cast-x11-smoke --exercise
```

With no arguments, the smoke program performs only capture/capability readback,
saves no pixels and injects no input. Real conferencing, loopback consumers,
compositor-specific preview behavior and hardware capture performance remain
subject to [hardware-acceptance.md](hardware-acceptance.md). No 1080p30 CPU, memory,
latency or frame-drop measurements are asserted without a usable capture session.

## Recording countdown

A configured `[record] countdown` or startup `--countdown SECONDS` delays only
recording. The panel shows a numbered film-style guide over its local preview on
all screens. Without an attached panel, Xorg opens a centered utility guide
(`cast-countdown` / `CastCountdown`); Escape or closing it cancels the pending start.
`cast record stop` and `cast pause` also cancel. The guide is hidden before the
encoder starts and is excluded from Xorg monitor/region capture together with its
window-manager frame. It is not an output overlay. Neutral masking covers screen
content underneath the guide while it is visible; application-window capture
excludes it naturally. Wayland uses the panel guide; it has no standalone guide.
