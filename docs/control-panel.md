# Native control panel

Build with `PANEL=1` and run `cast panel`. The panel is an independent client of the
existing daemon. Opening, navigating, closing or reconnecting the panel never
starts, stops, pauses or reveals an output. **Close**, beside the pinned tabs,
or `Ctrl+Q` closes only this window, including while disconnected or waiting for
a command acknowledgement.
The optional Clay/SDL3 renderer uses bundled Inter at native display density and
has no effect on a `PANEL=0` daemon’s streaming capability.

## Operate

The pinned header uses two rows: 44-pixel lane chips for **Virtual camera**,
**Recording**, and **Streaming**, then 30-pixel privacy and preview actions with
compact 11.5-pixel semibold labels. Each lane shows a text state, independent of
colour. Selecting a chip opens that output’s controls in Operate.
**Pause all** / **Resume** follows the daemon’s remembered group-pause restoration;
independently paused outputs remain
paused and stopped outputs remain stopped. **Preview on/off** controls the separate
floating preview. The permanent capture-exclusion notice remains above the tabs;
composition and connection feedback stay in the bottom status bar.

Operate has focused output disclosures and an Audio disclosure. Virtual-camera
start is privacy-paused; Resume virtual camera explicitly reveals the composition.
Recording start uses its configured countdown, with Cancel. Cut time removes media
time while preserving the same file; Resume uses the countdown again. Freeze and
Blur retain their independent flags under solid Pause. Streaming starts connecting
in solid pause; connection success never silently reveals content. **Resume
streaming** is the explicit reveal action. Connecting, retrying, failed, stopping
and paused states remain distinct; **Retry connection** after failure requests a
new session. There is no streaming Cut control.

**Streaming setup…** exposes the service, RTMP/RTMPS server URL, stream-key file path,
bitrates and encoder preset. It never contains the secret. Connection-affecting
fields explain their locked state while a session is active. Advanced network
controls remain in `[stream]` of the config. Runtime Apply leaves the config file
untouched. See [streaming setup](streaming.md) for safe key-file creation and service
recipes.

## Compose

Compose opens a flat list of seven sections: **Source & layout**,
**Camera**, **Background & stage**, **Overlays**, **Annotations & pointer**, **Audio**,
and **Settings**. Selecting a section opens its dedicated page with a sticky
**Compose** back control and all fields inline in labeled groups. Advanced screen
styling, geometry, gradient stops, overlay anchors, annotation policies, recording
formats and presentation typography remain on the appropriate page. Every existing
setting remains reachable.

**Source & layout → Capture & exclusion** selects **mask** or **transparent** for
panel/preview overlaps and sets the mask color independently of the pause screen.
Mask is the default. Transparent reveals Cast's configured screen background in
the overlap; it does not recover desktop content hidden behind the windows. The
same controls are available as `cast capture exclusion mask|transparent` and
`cast capture mask-color '#RRGGBB'`. Apply changes the session only; persist them
as `[capture] exclusion` and `mask_color` in the config file. The controls are
disabled on capture backends without panel exclusion. Selected application
capture naturally excludes both separate windows.

Text, numeric, enum and boolean composition edits are local drafts. **Apply** sends
one validated settings batch for the current section; **Revert** discards its unsent
drafts. A pinned draft bar above the status bar shows the current page's edit count,
Apply/Revert and **session only**. Enter applies the current section's draft.
Drafts survive page navigation. A failed acknowledgement keeps the draft,
and an acknowledgement cannot discard typing performed after submission. Explicit
capture selection, cycling, preset, reload and output actions are commands, with
pending/acknowledged/failed feedback. Unavailable backend controls are disabled and
explain the missing capability.

Pause/blur fonts are displayed with config-file and reload guidance; the user’s
output font choices remain separate from the panel’s single bundled Inter family.
Optional titles, subtitles and footers accept the existing date/time placeholders.
Static composition text uses a runtime-selectable font and literal text.

Use **1** / **2** to switch between Operate and Compose, and **Alt+1…7** to jump
directly to the seven sections in the order above. **Escape** dismisses the current
sheet or choice picker first, then returns from a section page to the Compose list.
Tab/Shift+Tab moves focus through controls.

Each view remembers its own scroll position. Closing and reopening the panel
restores the current page, output disclosure and scroll positions while the same
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
resets zoom. Its second row shows connection state and marked pending, acknowledged
or failed command feedback. Long tokens and messages fit with ellipses. During
disconnect, last-known state remains visible, commands are disabled and the panel
reconnects automatically. The permanent exclusion notice reports the active
capture policy; neither policy reconstructs covered desktop content.

## Implementation handoff

The production design comes from `redesign-prompt.md` and the supplied
`design-proposal.html`, `cast-panel.html`, and `cast-preview.html` artifacts.
Components reuse the existing Clay field tables, button/icon helpers, text cache,
bounded panel transport and renderer. Settings use the existing atomic settings
command; the panel does not call daemon/media functions directly. The private
protocol remains version 1 and requires the same build on both ends, as before.

The streaming implementation intentionally supersedes the prototype’s unavailable
capability and the incumbent `live` command names. It supplies real backend states
and `virtual` commands. Browser demo chrome, simulated device/preset inventories,
Unicode icons, browser window simulation and illustrative secrets are not
production controls. Native typography uses Inter slots
13/16/19/25/48 plus compact 11-pixel status and 11.5-pixel semibold meta-action
roles, rasterized at actual display density. The setup copy correctly says
the key stays out of UI/IPC/logs; authentication necessarily sends it to the ingest
service. The setup sheet keeps global privacy controls accessible while
editing, instead of blocking them behind browser demo modal chrome.

Panel-owned shared memory retains only navigation and per-view scroll numbers
for the current daemon session. This panel refinement uses the existing daemon
commands and snapshots, with overlap policy/color added to the capture schema;
the floating preview retains its existing controls and behavior.
