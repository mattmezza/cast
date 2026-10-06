# Native control panel

Build with `PANEL=1` and launch `cast` (or **Cast** from the application menu).
Ordinary startup opens the panel and starts a daemon unless one is already running.
`cast --headless` runs only the foreground daemon; `cast panel` attaches a panel
without starting capture automatically. Navigating, resizing or reconnecting never
starts, pauses or reveals an output.
**Close panel**, beside **Preview**, and the window-manager close action close only
this window. The daemon, recording, streaming and preview continue running.
**Stop daemon** stops outputs, finalizes recordings and leaves the panel available;
**Start daemon** restarts it using the last acknowledged session configuration.
Stopped recordings and streams do not restart automatically. Virtual camera startup
retains its privacy pause. **Quit** or `Ctrl+Q` opens a warning with Cancel selected;
**Stop and quit** shuts down all outputs, saves recording trailers, closes preview,
and waits for daemon cleanup before closing the panel. Cancel/Escape change no output.
The optional Clay/SDL3 renderer uses bundled Inter at native display density and
has no effect on a `PANEL=0` daemon’s streaming capability.

## Operate

The pinned header is one 36-pixel row: **Preview on/off**, **Close panel**,
**Stop/Start daemon**, and **Quit**. Output state and actions live on four always-expanded, single-column
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

**Pause all / Resume outputs**, above the Operate cards, and
`cast pause` / `cast resume` follow the daemon's remembered group restoration: stopped
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
Camera shape and backdrop source use chips. All 64 numeric settings use sliders
with exact editable inputs, including sizes, opacity, brightness, duration, bitrates,
zoom, gains and countdown. Slider ranges cover practical values; precise inputs
retain the full supported range. Disabled
startup-only dimensions remain read-only.
Background fields change with the selected mode rather than exposing unrelated colors.
Per-output click/keystroke visibility appears once as a three-column matrix.

Enums and discovered devices use keyboard-operable, scrollable choice pickers.
Color pickers offer swatches, editable #RRGGBB text and **Pick from screen**.
Swatches apply immediately. Invalid hex stays visible and never reaches the daemon.
On X11, Pick from screen changes the pointer to a crosshair with a small swatch
beside it showing the hovered pixel color. Left-click samples that visible pixel, while Escape or right-click cancels without changing the setting.
On Wayland it asks the desktop portal for a color selection; availability depends on
the installed portal; its picker owns the global UI and any hover preview. Cancellation or an unavailable picker leaves the color unchanged
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

## License/About

A compact pinned Community/Pro badge opens License/About, separate from the four
Operate cards and six Compose pages. Back/Escape returns to the previous view.
The page shows the daemon-acknowledged edition/license state, perpetual or
subscription policy, masked ID, update cutoff/release eligibility, pending downgrade
and availability reasons. Planned workflows are labelled not implemented; purchase
cannot make missing code usable. Disconnected state never grants active capabilities.

The import source-path entry is panel-only scratch data, not an INI key. Import,
Reload and confirmed Remove use the authenticated command queue. Invalid replacement
preserves the previous file. Copy diagnostics excludes payloads/signatures and a full
license ID. The optional purchase/info action appears only for a configured HTTPS
URL and opens the system browser on an explicit click without adding identifiers.
Licensing does not start/resume lanes and adds no account login or startup dialog.
`licensing.file` and `licensing.upgrade_url` are session settings; configuring them
never automatically saves the INI.

Recording's overflow exposes encoder-neutral bitrate/rate-control alongside the
existing codec/container/legacy quality fields. Streaming setup exposes its video
encoder and the existing bitrate. Sliders keep exact inputs. Encoder-specific legacy
CRF/preset values are labelled inactive on OpenH264. Actual initialized encoder
choices and their selection/inactive-setting details are reported from cached worker
state rather than inferred from `auto`.

Pro uses separate `CastProPanel` and `CastProPreview` native Xorg classes and
application/package identity. A custom window manager can add these class names to
its utility/floating rules. Community retains `CastPanel` and `CastPreview`.

## Pro workflows

Pro keeps the same four output/audio cards and six live Compose pages. Source &
layout adds a Zoom group with legacy/cinematic mode, factor, follow and optional
click auto-zoom. Advanced geometry, timing, framing/filter/blur controls remain under
All settings. Annotations & pointer contains synthetic-cursor smoothing and captions.

Audio options shows shared model/language essentials and an All speech settings
disclosure for the advanced engine/VAD controls. Its compact status reports actual
model readiness, inference/queue time and errors. Browsing a model picker never
downloads one or enables audio. The selected source applies to captions; notes
always use the explicitly permitted microphone.
Recording options shows the last transcription job's state, progress and sidecar
paths, then exposes sidecar format and automatic final transcription. A
separate Speaker notes strip opens/closes the native notes window and its options
view with Start/Pause/Close and Essentials/All settings; notes mode, speed,
appearance and matching settings are session edits. Compact Close and Pause remain
available for an existing Notes window after entitlement loss.
Notes options links to the same transcription engine surface and displays the
actual loaded document path separately from the configured default file. Explicit
load/open replaces the readable snapshot without rewriting that default.

Typed independent choices commit on selection or slider release; a precise text
input commits on completion. Coupled engine/window/threshold edits use the existing
pinned Apply/Revert transaction. Every workflow field is reachable through its
metadata-defined group. Unavailable builds/licenses/capabilities show an explanation
and retain safety stop/close operations. Panel navigation never starts listening,
recording or speech following. Closing the panel leaves the notes window alone.
