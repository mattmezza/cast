# Native control panel

Build with `PANEL=1` and run `cast panel`. The panel is an independent client of the
existing daemon. Opening, navigating, closing or reconnecting the panel never
starts, stops, pauses or reveals an output. `Ctrl+Q` closes only this window.
The optional Clay/SDL3 renderer uses bundled Inter at native display density and
has no effect on a `PANEL=0` daemon’s streaming capability.

## Operate

The pinned header has three lane chips: **Virtual camera**, **Recording**, and
**Streaming**. Each shows a text state, independent of colour. Selecting a chip
opens that output’s controls in Operate. **Pause all** / **Resume** follows the
daemon’s remembered group-pause restoration; independently paused outputs remain
paused and stopped outputs remain stopped. **Preview on/off** controls the separate
floating preview. A one-line composition summary and permanent capture-exclusion
notice remain above the tabs.

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

Compose follows the supplied design’s focused disclosures: **Source & layout**,
**Camera**, **Background & stage**, **Overlays**, **Annotations & pointer**, **Audio**,
and **Settings**. Advanced screen styling, geometry, gradient stops, overlay anchors,
annotation policies, recording formats and presentation typography stay within the
appropriate disclosure. Every existing setting remains reachable.

Text, numeric, enum and boolean composition edits are local drafts. **Apply** sends
one validated settings batch; **Revert** discards unsent drafts. Enter applies the
focused field. Drafts survive navigation. A failed acknowledgement keeps the draft,
and an acknowledgement cannot discard typing performed after submission. Explicit
capture selection, cycling, preset, reload and output actions are commands, with
pending/acknowledged/failed feedback. Unavailable backend controls are disabled and
explain the missing capability.

Pause/blur fonts are displayed with config-file and reload guidance; the user’s
output font choices remain separate from the panel’s single bundled Inter family.
Optional titles, subtitles and footers accept the existing date/time placeholders.
Static composition text uses a runtime-selectable font and literal text.

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
pixels, square controls, visible keyboard focus, and accelerated wheel scrolling.
Only the body scrolls; the header, tabs and feedback stay pinned. This works in both
floating and tiled windows, without assuming a window drag handle. The X11 class
is truthfully CastPanel with the utility window type, matching the existing mwm
floating rule; no window-manager configuration is changed.

A persistent disconnect banner shows last-known state, disables commands and
explains automatic reconnection. Every submitted command shows Pending followed by
its acknowledgement or actionable failure. Capture exclusion is informational and
permanent; neutral masking cannot reconstruct content covered by either window.

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
Unicode icons, unsupported preview dragging and illustrative secrets are not
production controls. Native typography uses the specified Inter slots
13/16/19/25/48 rather than browser fractional sizes. The setup copy correctly says
the key stays out of UI/IPC/logs; authentication necessarily sends it to the ingest
service. The setup disclosure keeps global privacy controls accessible while
editing, instead of blocking them behind browser demo modal chrome.
