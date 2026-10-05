# Native control panel

Build with `PANEL=1` and run `cast panel`. The panel is an independent client of the
existing daemon. Opening, navigating, closing or reconnecting the panel never
starts, stops, pauses or reveals an output. **Close**, beside **Preview**,
or `Ctrl+Q` closes only this window, including while disconnected or waiting for
a command acknowledgement.
The optional Clay/SDL3 renderer uses bundled Inter at native display density and
has no effect on a `PANEL=0` daemon’s streaming capability.

## Operate

The pinned header is one 36-pixel row: **Pause all / Resume**, **Preview on/off**,
and **Close**. Output state and actions live on four always-expanded, single-column
task cards: **Virtual camera**, **Recording**, **Streaming**, and **Audio**.
The capture-exclusion notice and Operate/Compose tabs remain pinned below the header.

Virtual camera and recording start directly from their cards. Virtual-camera start
is privacy-paused; Resume explicitly reveals the composition. Recording uses its
configured countdown, with Cancel. **Cut** removes media time; Resume continues the
same file after another countdown. Solid pause, freeze and blur keep their separate
flags and silence that lane's audio. Blur may cover freeze; solid pause overrides
both; Resume clears only solid pause.

An unconfigured Streaming card opens **Set up streaming…** as its primary action.
Configured sessions start connecting in solid pause. Successful connection does
not reveal composition: **Resume streaming** explicitly reveals it. Connecting,
retrying, failed, stopping and presentation states remain distinct. Failed sessions
show their reason and a Retry action. Streaming has no Cut.

Each card's **⋯** opens one in-panel options surface. Recording options contain
countdown, destination and encoding settings. Audio options contain discovered
sources and gains; microphone and desktop-audio toggles remain directly on the card.
Virtual-microphone controls stay with audio/output options. Streaming options show
server and key-file path and link to the dedicated setup view. No stream secret is
shown. Options never nest, close with Escape/outside click and close on navigation.

**Pause all / Resume** follows the daemon's remembered group restoration: stopped
outputs remain stopped and independently paused outputs remain paused. Preview is
a separate floating window; its toggle is unavailable while countdown owns it.

**Streaming setup…** exposes the service, RTMP/RTMPS server URL, stream-key file path,
bitrates and encoder preset. It never contains the secret. Connection-affecting
fields explain their locked state while a session is active. Advanced network
controls remain in `[stream]` of the config. Runtime Apply leaves the config file
untouched. See [streaming setup](streaming.md) for safe key-file creation and service
recipes.

## Compose

Compose opens six section pages: **Source & layout**, **Camera**, **Background &
stage**, **Overlays**, **Annotations & pointer**, and **Pause & blur screens**.
Audio exists only on Operate; recording options exist only on its card; streaming
connection fields exist only in Streaming setup.

Each page begins with Essentials and ends with **All settings**, which reveals the
remaining fields inline with labeled groups. This one-level disclosure retains drafts
and page scroll. Layout diagrams represent the five actual layouts. Camera position
uses eight points around a disabled center; no drag-in-preview or Free option is
advertised. Free coordinates remain advanced controls for CLI-selected free positioning.
Camera shape and backdrop source use chips. Size, zoom, gains, countdown and pixel-valued
settings use sliders. Pixel sliders also provide an exact numeric field; disabled
startup-only dimensions remain read-only.
Background fields change with the selected mode rather than exposing unrelated colors.
Per-output click/keystroke visibility appears once as a three-column matrix.

Enums and discovered devices use keyboard-operable, scrollable choice pickers.
Color pickers offer swatches, editable #RRGGBB text and **Pick from screen**.
Swatches apply immediately. Invalid hex stays visible and never reaches the daemon.
On X11, Pick from screen changes the pointer to a crosshair: left-click samples the
visible pixel, while Escape or right-click cancels without changing the setting.
On Wayland it asks the desktop portal for a color selection; availability depends on
the installed portal. Cancellation or an unavailable picker leaves the color unchanged
and reports an error when appropriate. No screenshot or sampled image is saved.
Boolean controls have explicit On/Off segments. Unavailable capabilities and active
output locks keep their controls visible with explanatory notes. Missing-resource
warnings do not substitute fake assets or silently change daemon behavior.

Selecting a section opens its dedicated page with a sticky **Compose** back control.
Advanced screen styling, geometry, gradient stops, overlay anchors, annotation
policies and presentation typography remain on the appropriate page. Every existing
setting remains reachable.

**Source & layout → Capture & exclusion** sets the color that masks panel/preview
overlaps independently of the pause screen. The same control is available as
`cast capture mask-color '#RRGGBB'`. Edits change the session only; persist the
color as `[capture] mask_color` in the config file. The control is disabled on
capture backends without panel exclusion. Selected application capture naturally
excludes both separate windows. For unobstructed monitor/region capture, keep the
controls outside the captured area.

The experimental transparent mode has been removed: it only revealed Cast's
background, rather than the desktop hidden behind the control windows. Remove
`[capture] exclusion` from configurations that used that unreleased setting.

Most controls apply immediately: camera anchors, layout diagrams, boolean segments,
enum choices, annotation cells and color swatches send one acknowledged update per
selection. Sliders keep the drag local and commit once on release; keyboard steps
commit individually. Text, exact numeric values and hex colors commit on Enter or
when focus leaves the field, never on each keystroke. Invalid values remain editable
with an error and leave the daemon unchanged. Geometry changes affect active outputs
as well as the floating preview; there is no separate preview-only IPC.

Settings that must change together retain **Apply/Revert**: streaming setup, capture
region coordinates and dimensions, zoom minimum/maximum, layout/preset/corner cycle
orders, and recording container/codec/preset choices. Apply sends one validated batch
for the current section. The pinned bar also lets you correct, retry or revert an
invalid or failed edit. Unsent batch drafts survive page navigation; navigation and
runtime edits never write the config file.

A failed acknowledgement keeps its edit, and a successful acknowledgement cannot
discard typing performed after submission. Capture selection, cycling, preset,
reload and output actions keep pending/acknowledged/failed feedback. Unavailable
backend controls are disabled with an explanation.

Pause/blur fonts are displayed with config-file and reload guidance; the user’s
output font choices remain separate from the panel’s single bundled Inter family.
Optional titles, subtitles and footers accept the existing date/time placeholders.
Static composition text uses a runtime-selectable font and literal text.

Use **1** / **2** to switch between Operate and Compose, and **Alt+1…6** to jump
directly to the six sections in the order above. **Escape** dismisses the current
sheet or choice picker first, then returns from a section page to the Compose list.
Tab/Shift+Tab moves focus through controls.

Each view remembers its own scroll position. Closing and reopening the panel
restores the current page, Essentials/All state and scroll positions while the same
daemon session runs. A new daemon session resets navigation to Operate. This
navigation memory does not save drafts or write configuration; runtime Apply
continues to leave the config file untouched.

## Floating preview

The preview is a separate native **CastPreview** utility window. Its target pills
select **Virtual camera**, **Recording**, or **Streaming**. They select the actual
lane’s presentation effects and annotations before encoding. State strips and
local chrome are drawn outside the composition frame and never enter output
frames. Blur and solid pause inside the frame are the actual outgoing effects.
Native 1/2/3 keys also select preview targets.

Successful start/resume commands select the corresponding target in the daemon;
a later manual choice takes precedence. On Xorg, recording start and cut-resume
show a temporary full-frame countdown preview even when normal preview was off.
Cancel works without a panel. The guide hides completely before recording admits
new frames. Normal preview toggling is locked while the countdown owns the window.
Wayland reports native preview limitations instead of pretending it has a window.

## Window and feedback behaviour

The panel uses fluid widths from 360 logical pixels, a centered column up to 520
pixels, square controls, visible keyboard focus, and wheel scrolling at 60 logical
pixels per notch (twice the previous speed). Fractional trackpad motion and natural
scroll direction are preserved.
Only the body scrolls; the header, tabs, page Back control, draft bar and status
stay pinned. This works in both floating and tiled windows, without assuming a
window drag handle. The X11 class
is truthfully CastPanel with the utility window type, matching the existing mwm
floating rule; no window-manager configuration is changed.

The 46-pixel status bar has two centered rows in 11-pixel text. Its first row shows
source, layout and preset links to the relevant Compose pages; the zoom token
resets zoom. Lane tokens jump directly to Operate cards, with recording elapsed time.
At narrow widths the bar drops preset, then zoom, then lane tokens to preserve readable
source/layout links. Its second row shows connection state and marked pending, acknowledged
or failed command feedback. Long tokens and messages fit with ellipses. During
disconnect, last-known state remains visible, commands are disabled and the panel
reconnects automatically. The permanent exclusion notice reports the active
masking status; masking cannot reconstruct covered desktop content.

## Implementation handoff

The production design comes from the approved round-three `cast-panel.html` prototype.
The native implementation preserves existing daemon commands, atomic setting batches,
transport snapshots and preview behavior. Browser demo inventories and simulated state
are replaced with acknowledged daemon state and discovered resources. The panel uses
bundled Inter only: existing 13/16/19/25/48 slots plus compact status/meta roles. Output
text font configuration is independent of the panel typeface.

Panel-owned shared memory stores only navigation, per-page disclosure state and scroll
for the current daemon session; no drafts, secrets, output state or config are persisted.
No configuration keys or IPC commands are added by this redesign.
