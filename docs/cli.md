# Command reference

`cast` runs one foreground instance. `cast COMMAND` sends a bounded request to its
user-only Unix socket and prints acknowledged success or an actionable error. Exit 0
means success; errors exit nonzero. `--help`, `--version` and `COMMAND --help` run locally.
`config check/defaults` and `doctor` also run locally without a daemon. Runtime arguments
are never passed to a shell. Concurrent commands are ordered by accepted connection;
messages are atomic and a silent peer has a bounded timeout. A client timeout means
completion is unknown: inspect status before repeating a non-idempotent command.

Startup flags appear before the command: `--config PATH`, `--socket PATH`,
`--backend xorg|wayland`, `--output-device PATH`, `--camera-device PATH`,
`--width N`, `--height N`, `--fps N`, `--no-live`, `--no-camera`,
`--mic-source NAME`, `--desktop-source NAME`, `--record-dir PATH`,
`--container NAME`, `--video-codec NAME`, `--audio-codec NAME`, `--countdown SECONDS`.
Selecting an audio source does not enable its lane: configure or explicitly enable it.
Build features decide backend availability. `synthetic` backend/camera and `none` output
are explicitly test-only options; they do not represent working desktop/device access.

`layout overlay|split|screen|camera` selects composition; `layout next` follows
composition.layout_order. `split side left|right` chooses camera side; `split ratio 25%`
sets its share. Camera hidden state and geometry persist when changing layouts.

`camera show|hide|toggle` sets visibility; toggle reverses only visibility.
`camera size +5%|-5%|25%` adjusts five percentage points or sets 25% of canvas width.
Anchored resizing fixes that corner, free sizing fixes center, and valid positions
are clamped within the canvas. Impossible sizes fail. `camera move DX DY` is relative;
`camera position X Y` is absolute output pixels, and both switch to free positioning.
`camera anchor top-left|top-right|bottom-left|bottom-right|top|bottom|left|right|next`
selects a corner or the middle of an edge. Edge anchors keep the corresponding
margin and remain centered on the other axis while resizing. `next` follows
camera.corner_order; that list may include edge anchors.
`camera shape rectangle|rounded|circle|next` cycles rectangle→rounded→circle.
`camera aspect native|16:9|4:3|1:1` controls centered crop independently of mask;
circle forces square crop. `camera crop move DX DY` adjusts source-pixel crop offsets.
`camera mirror on|off|toggle` affects camera content only and defaults to on; the screen
stays unmirrored. Use `cast camera mirror off` to disable it for the session, or set
`mirror = false` in `[camera]` for a persistent default. `camera list` enumerates
camera formats/devices. `camera device PATH` acquires the candidate before replacing
current camera; permissions/format errors are actionable.

`screen list`, `screen select NAME` and `screen next` discover/select/cycle Xorg
RandR monitors. `capture monitor` selects the current monitor; on Wayland it opens
portal consent again. `capture region X Y WIDTH HEIGHT` uses selected-monitor pixels.
`capture region select` starts an interactive drag; `capture window select` starts a
click selection; `capture window active` selects the currently active window once.
Interactive commands acknowledge acquisition start, keep the prior source until
commit, and Escape cancels. `status` shows resulting source/errors. Window resize,
movement and failures are handled without silently returning to the desktop.
`capture fit contain|cover` selects letterboxing or cropping. Source changes reset
zoom and annotation history, and do not resume any output. Unsupported backend
selection commands fail explicitly.

`zoom toggle` alternates 1 and the last/configured non-1 factor; `zoom in|out`
adds/subtracts zoom.step within limits; `zoom set FACTOR` sets a finite factor;
`zoom reset` returns to 1. `zoom follow on|off` selects smoothed cursor tracking or
locks the target. Screen zoom leaves camera and annotation labels unscaled.
`cursor on|off|toggle` controls the separately rendered pointer;
`cursor highlight on|off|toggle` changes its highlight. Embedded cursor policy
needs portal restart; there is no second pointer. Cursor metadata is required for
follow/highlighting. `clicks on|off|toggle` controls mouse rings. `keys on|off|toggle`
controls key labels; `keys mode shortcuts|all` selects shortcuts/navigation or
explicit printable events; `keys clear` discards labels. All keys can expose private
text. Raw observations are never written to an input log. Compose/IME events do
not universally reproduce committed text; keyboard names/layout translation are
best effort. Consecutive identical labels collapse as `j`, `jx2`, `jx3`. A bounded
queue retains recent labels for `[keys] timeout_ms` (3000 ms by default); repeat
counts refresh that row, and older rows expire independently.
Configure keys.filter to exclude every cast-control shortcut you bind.
`annotations live|record keys|clicks on|off` selects annotation visibility per lane.

`pause` solid-pauses both applicable outputs, remembers only states it changes,
and cancels a pending recording start or cut-resume countdown. Live emits neutral
frames and recording continues writing neutral frames, with silence in both lanes.
Repeated pause is idempotent. `resume` restores only remembered solid states;
it never starts a recording or clears recording cut. Independent solid commands
supersede that lane's remembered restoration; freeze/blur/cut are orthogonal.

`live pause|resume|toggle` selects solid privacy pause; toggle reverses that flag.
`live freeze|unfreeze` holds/releases the complete screen and camera composition.
`live blur [on|off|toggle]` applies/removes/toggles a configurable blurred, tinted
composition with optional title/subtitle/footer. Bare `blur` enables it; `unblur` disables
it. Blur is above freeze, and solid pause overrides both. Blur alone keeps capturing
moving content. None of these effects resumes solid pause. Freeze and blur silence
cast virtual audio. Blur can leave information recognizable; use solid pause for privacy.

`live message "TEXT"` changes the shared solid-pause title without resuming any
output. Pass `""` to remove the title. The limit is 127 UTF-8 bytes. Use the shared
settings below to edit titles/subtitles and colours in either output:

```sh
cast settings output.pause_title "Back soon" output.pause_subtitle "{date:%A} {time:%H:%M}"
cast settings output.pause_background '#20252b' output.pause_foreground '#ffffff'
cast settings output.blur_title "Break" output.blur_subtitle "{datetime}"
cast settings output.blur_radius 48 output.blur_opacity 0.6
cast settings output.blur_footer "Back shortly" output.blur_text_gap 20
cast live footer "{datetime:%A, %d %B · %H:%M}"
```

`output.pause_text` and `output.pause_color` remain aliases. `output.pause_font`
and `output.blur_font` are config-file-only; edit the file and use `config reload`.
Title, subtitle and footer are optional. `live footer "TEXT"` and
`record footer "TEXT"` change the same shared solid-pause footer; use
`settings output.blur_footer "TEXT"` for the blur footer. Their limit is 255 UTF-8
bytes. `output.pause_text_gap` / `output.blur_text_gap` set title/subtitle distance
in output pixels (0–512, default 12), independently of the font sizes.
`output.pause_footer_size` / `output.blur_footer_size` set footer sizes (8–256,
default 18). [Text placeholders](configuration.md#pause-and-blur-text)
expand in local time, including custom strftime formats.

`record start [PATH]` starts one new file, optionally after configured countdown.
On Xorg, the independent floating preview shows a numbered countdown even when
the panel is attached and preview was off. Escape or closing it cancels the pending
action. The panel keeps countdown status and Cancel available.
The guide disappears before media admission and is excluded from screen capture.
Automatic names are generated in record.directory; existing paths are never
overwritten. Starting during group pause creates a solid-paused recording; group
resume does not change it because group pause did not pause that new recording.
`record stop` requests finalization or cancels an initial start countdown.
During a cut-resume countdown it finalizes the existing file. `status --json`
reports record.finalizing; wait for false before using the file or starting another
recording. The foreground daemon prints completion/error, and quit waits for it.

`record pause` writes the configured solid screen plus silence. `record resume`
clears solid pause unless the recording is cut. `record toggle` toggles only solid
pause, never cut/start/stop. `record freeze|unfreeze` and
`record blur [on|off|toggle]` / `record unblur` match live presentation effects.
Recording uses the same style settings but has independent visual flags.

`record cut` interrupts file writing without ending the encoder/container.
`record resume` from cut reuses the configured start countdown, then continues the
same file with interruption and countdown time removed. It preserves the existing
solid/freeze/blur flags. `record cut` again cancels a pending resume; Escape/closing
the standalone guide also leaves the file cut. All controls require an existing
recording. Pause/freeze/blur record silence; cut admits neither video nor audio.
**Migration from v0.3:** use `record cut` where you previously used `record pause`
to remove interruption time. Encoder/disk failures retain recoverable partial data
and leave live controls operational.

`audio list` lists PipeWire sources. `audio mic|desktop|virtual on|off|toggle` controls
lanes/virtual source; `audio mic|desktop source NAME` selects explicitly;
`audio mic|desktop gain 80%` sets gain (0..400%). Desktop capture requires an explicit
source; it starts off. A speaker-monitor source can include call participants;
application capture avoids broad mixes where PipeWire supports it. Disappearing
sources silence the lane and report errors; there is no broad-source fallback.
Live pause/freeze/blur silences cast's optional virtual microphone. A physical mic selected
directly by a conference app needs the app's own mute control. cast creates no
physical-speaker playback route; video and audio devices are selected separately.

`preset NAME|next` applies composition-only named settings and configured cycle.
`preview on|off|toggle` controls local preview; `preview target live|record` chooses
its lane. Local LIVE/PAUSED/FROZEN/BLURRED/CUT/RECORDING labels stay out of exported frames.
`status [--json]` reports capabilities, source, layout, camera visibility, zoom,
live/record state, path/active duration/countdown/finalization, audio routing, errors and drops.
`doctor` performs read-only dependency/device/directory checks with setup advice.
`setup` prints virtual-camera, configuration and conferencing instructions without
changing the system. `completions [bash|zsh|fish]` prints shell setup instructions;
`completions --script SHELL` emits the embedded completion script. Both work without
a daemon or configuration file.
`update [vMAJOR.MINOR[.PATCH]] [--download-only DIRECTORY]` downloads an official
Arch x86_64 release package and verifies its SHA-256 checksum. With no version it
selects the latest release. Installation uses pacman and asks for sudo when needed;
download-only saves a checked package and its checksum without installation.
Stop the daemon before updating and restart it afterwards. `cast update v0.4`
selects the release tag v0.4 and package version 0.4.0.
`panel` opens the optional Clay/SDL3 control panel (`PANEL=1` build). Closing it
leaves the daemon running. See [panel behavior and exclusion](control-panel.md).
`settings SECTION.KEY VALUE [SECTION.KEY VALUE ...]` applies an atomic batch of
INI-schema values to this session. Validation and restart restrictions match reload;
failed batches leave settings intact. Preset definitions are changed in the file.
For example: `cast settings camera.radius 16 camera.border_color "#80c9ff"`.

`config check [PATH]`, `config defaults`, `config reload` validate/print/reload config.
`reset` restores effective composition defaults only; `quit` cleanly shuts down
and finalizes an existing recording. No command persists session changes to config.

For a status bar, poll `cast status --json` and parse JSON, for example
`cast status --json | jq -r '.live.state + " / " + .record.state'`.
The last_error field retains the last reported failure; errors are also printed
by the foreground daemon. A successful unrelated command does not erase evidence.

When an edited configuration is invalid, explicit `--socket PATH` runtime commands
use that routing directly, so status and config reload remain reachable. Local
config check/doctor and daemon startup still strictly validate the selected file.
