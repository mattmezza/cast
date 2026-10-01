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
`camera anchor top-left|top-right|bottom-left|bottom-right|next` follows camera.corner_order.
`camera shape rectangle|rounded|circle|next` cycles rectangle→rounded→circle.
`camera aspect native|16:9|4:3|1:1` controls centered crop independently of mask;
circle forces square crop. `camera crop move DX DY` adjusts source-pixel crop offsets.
`camera mirror on|off|toggle` affects camera content only. `camera list` enumerates
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
best effort. Configure keys.filter to exclude every cast-control shortcut you bind.
`annotations live|record keys|clicks on|off` selects annotation visibility per lane.

`pause` privacy-pauses both applicable outputs, remembers only states it changes,
and cancels pending recording countdown. It emits neutral live frames and silence.
Repeated pause is idempotent. `resume` restores only remembered states; it cannot
start a recording or resume one paused independently. Any independent live/record
state command supersedes that lane's remembered restoration, even an idempotent pause.
`live pause|resume|toggle` controls live privacy state; toggle reverses pause.
`live freeze|unfreeze` deliberately holds/releases composed content and silences
cast virtual audio. Privacy pause overrides freeze and replaces retained content
with neutral. Neither freeze nor unfreeze can bypass privacy pause.

`record start [PATH]` starts one new file, optionally after configured countdown.
Automatic names are generated in record.directory; existing paths are never
overwritten. Starting during group pause creates a paused recording; group resume
does not resume it because it was not a running output changed by that pause.
`record stop` finalizes the file and prints its filename, or cancels countdown.
`record pause|resume|toggle` appends no media while paused and resumes the SAME
file with interruption time removed. Toggle NEVER starts/stops and errors without
a recording. Recording is independent of live privacy/freeze. Encoder/disk failures
stop that recording and retain recoverable partial data; live controls continue.

`audio list` lists PipeWire sources. `audio mic|desktop|virtual on|off|toggle` controls
lanes/virtual source; `audio mic|desktop source NAME` selects explicitly;
`audio mic|desktop gain 80%` sets gain (0..400%). Desktop capture requires an explicit
source; it starts off. A speaker-monitor source can include call participants;
application capture avoids broad mixes where PipeWire supports it. Disappearing
sources silence the lane and report errors; there is no broad-source fallback.
Live pause/freeze silences cast's optional virtual microphone. A physical mic selected
directly by a conference app needs the app's own mute control. cast creates no
physical-speaker playback route; video and audio devices are selected separately.

`preset NAME|next` applies composition-only named settings and configured cycle.
`preview on|off|toggle` controls local preview; `preview target live|record` chooses
its lane. Local LIVE/PAUSED/FROZEN/RECORDING labels stay out of exported frames.
`status [--json]` reports capabilities, source, layout, camera visibility, zoom,
live/record state, path/active duration/countdown, audio, errors and drops.
`doctor` performs read-only dependency/device/directory checks with setup advice.
`config check [PATH]`, `config defaults`, `config reload` validate/print/reload config.
`reset` restores effective composition defaults only; `quit` cleanly shuts down
and finalizes an existing recording. No command persists session changes to config.

For a status bar, poll `cast status --json` and parse JSON, for example
`cast status --json | jq -r '.live.state + " / " + .record.state'`.
The last_error field retains the last reported failure; errors are also printed
by the foreground daemon. A successful unrelated command does not erase evidence.
