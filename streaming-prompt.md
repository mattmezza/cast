# Add a reliable streaming output to Cast

You are the implementation agent. Build this feature completely, including CLI,
configuration, panel controls, documentation, and verification. This is a third
output for the existing application, not a replacement for its virtual camera or
recorder. Read the repository's applicable AGENTS.md files, cast-build-prompt.md,
PRODUCT.md, DESIGN.md, and current implementation before editing. This prompt
extends the original build brief where that brief excludes streaming services and
supersedes its `live` naming for virtual-camera output. Preserve existing features
and privacy guarantees, apart from the intentional naming break specified below.

Keep the implementation understandable: explicit state, descriptive names, small
modules, and comments explaining consequential choices. Avoid dense or obfuscated
code. Use narrowly assigned subagents when useful, with clear file ownership and
appropriate models/reasoning levels; avoid redundant investigations and reviews.
Commit coherent, verified increments. Do not install privileged dependencies,
change devices or window-manager configuration, use real account credentials, or
broadcast publicly without explicit user authorization. Prepare release-ready
changes, but publish or push only when authorized in the implementation session.

## Product scope and naming

Cast has three output purposes:

- **Virtual camera**: `cast virtual ...` replaces the existing `cast live ...`
  commands and controls video sent to
  the local virtual camera and its optional virtual microphone.
- **Recording**: `cast record ...` creates a local video file.
- **Streaming**: new `cast stream ...` commands broadcast to one configured
  destination, such as Twitch, YouTube, or a compatible server.

Rename the virtual-camera surface from `live` to `virtual` completely. Breaking
compatibility is explicitly authorized: Cast has one user and has not reached v1.
Do not retain `live` commands, aliases, deprecated options, configuration aliases,
or duplicate status fields. Old spellings must fail with an actionable message
pointing to the new spelling, without executing the requested operation.

Use **Virtual camera**, **Recording**, and **Streaming** consistently as visible
panel/output labels and explain those names in CLI help and documentation. Change
virtual-camera actions to **Start virtual camera**, **Pause virtual camera**, and
**Resume virtual camera**. Keep `camera` for physical webcam composition controls;
do not rename or conflate that command with the virtual-camera output.

Audit and update every relevant surface, including:

- Every `cast live ...` subcommand becomes `cast virtual ...`, preserving its
  behavior, arguments, and privacy semantics.
- `cast preview target live` becomes `cast preview target virtual`; the configured
  preview target and defaults use `virtual` as well.
- `annotations live ...` becomes `annotations virtual ...`, and configuration
  keys `annotations.live_keys` / `annotations.live_clicks` become
  `annotations.virtual_keys` / `annotations.virtual_clicks`.
- Startup option `--no-live` becomes `--no-virtual`; similarly rename any other
  virtual-camera-specific option using the old term.
- Machine-readable status uses `virtual` and corresponding `virtual_*` fields
  instead of virtual-camera fields named `live` / `live_*`; update all consumers.
- Panel labels, preview decorations, command replies, errors, defaults, help,
  shell completions, example configuration, key bindings, README, man page,
  maintained documentation, tests, fixtures, and CI expectations use the new names.
- Rename virtual-camera-specific internal identifiers consistently where useful
  to prevent the old term from misleading future implementation work.

This is a semantic rename, not a blind text replacement. Ordinary phrases such as
“live streaming,” service terminology, and historical release notes may still say
“live” when they do not name Cast's virtual-camera surface. Include a concise
migration note listing the changed spellings, without compatibility shims.

The first version supports one active streaming destination. Virtual camera,
recording, and streaming must work independently and simultaneously. Streaming must
also work with virtual-camera output disabled, without a v4l2loopback device, and
without an open panel or preview.

Implement a generic RTMP/RTMPS encoder output, with Twitch and YouTube documented
setup recipes and useful presets. Do not require service APIs, OAuth, or account
linking. The user creates their broadcast in the service's website and supplies its
server address and stream key. Exclude multi-destination broadcasting, chat, alerts,
scheduling, channel management, cloud relays, and a scene/timeline editor.

Verify service settings against current official Twitch and YouTube documentation
before selecting presets. Distinguish tested protocol behavior from actual service
acceptance; do not claim an account or service was tested when it was not.

## Architecture

Inspect the existing composition/output path in `src/main.c`, commands and state,
`src/media.c`, `src/media_internal.h`, `src/record.c`, audio routing, presentation
effects, panel transport, configuration schema, and optional backend boundaries.
The repository may have advanced since this prompt was written; use current code.

Reuse the existing composed frames, PipeWire audio mix, and FFmpeg libraries.
Prefer an in-process streaming worker using libavcodec/libavformat and the FLV
muxer over RTMP/RTMPS. Do not handwrite codecs or the RTMP protocol. Keep streaming
independent of X11/Wayland types and panel libraries.

Use a separate bounded streaming queue and worker. Network connection setup,
encoding, writes, reconnects, and finalization must not block capture, IPC, preview,
virtual-camera output, or file recording. Keep the control/status mutex separate
from blocking encoder/network operations. Cancellation, stop, privacy commands,
and shutdown must have bounded response times, including during DNS resolution,
connection setup, TLS negotiation, stalled writes, and reconnect delay. Verify
which blocking operations FFmpeg interruption callbacks actually cover; add an
appropriate isolation boundary if that is insufficient. Never claim bounded
cancellation on the strength of an untested callback alone.

Use software H.264 and AAC as the initial portable baseline, with stream-specific
video bitrate, audio bitrate, encoder speed preset, and a two-second keyframe
interval. Implement appropriate constant-bitrate/rate-control limits for the
chosen encoder and service presets. Recording keeps its own quality and codec
settings. Hardware encoding is optional, not a prerequisite or an excuse to leave
the software path incomplete. Validate encoder, muxer, protocol, and TLS support in
the installed FFmpeg build, with actionable errors.

Independent encoding for recording and streaming is acceptable and preferable to
a complex shared encoder in this first version. Explain the additional CPU cost.
Do not introduce a generic multimedia framework or broad unrelated refactor.

## Streaming state and presentation effects

Define observable lifecycle states: stopped, connecting, streaming, reconnecting,
stopping, and failed. Keep presentation flags (paused/frozen/blurred) separate from
connection state. Maintain an operation/session generation so late worker results
cannot revive a stopped session, replace newer settings, or admit old media.

No broadcast starts at daemon startup, config reload, panel opening, preset/layout
selection, group resume, or reconnection after an explicit stop. `stream start` is
the explicit start action. Start connected sessions in solid pause by default,
with a clear acknowledgement and panel indication; `stream resume` deliberately
reveals the composition. Document this two-step behavior and make the panel's
Resume streaming action obvious. Starting an already active/connecting/reconnecting
session must return an actionable error rather than create another worker.

Expose the same presentation effects independently for the streaming lane:

- Pause: continuously send the existing configured solid background and optional
  title/subtitle/footer, with silence. Maintain the connection and media cadence.
- Freeze: hold the streaming lane's complete screen/camera/overlay composition;
  use silence as the default audio behavior.
- Blur: apply the existing configured moving blur/tint with optional styled text;
  use silence, and retain the underlying freeze state when present.
- Precedence: solid pause overrides blur, and blur applies above freeze.
- Resume clears solid pause only; it does not clear freeze or blur.
- Stop ends transmission. Do not describe it as instantly ending the service's
  public broadcast; service buffering and end-of-broadcast behavior can differ.

Reuse existing pause/blur typography, colors, placeholders, fonts, spacing, and
footer settings. Do not create a separate styling system. Do not apply a global
mirror to streamed screen content or text; retain camera-only mirroring.

Do not add recording-style cut/resume to streaming. Public viewers experience
continuous real time; paused/frozen/blurred streams keep valid increasing audio
and video timestamps and continue producing frames/silence.

Extend `cast pause` / `cast resume` to include an existing streaming session while
preserving the established group-pause rules. Remember only solid-pause states
changed by the group action. Group resume never starts a stopped stream or reveals
one already independently paused. Independent lane commands supersede remembered
restoration consistently. Reset and configuration changes preserve output state.

Privacy acknowledgement must invalidate queued/unsubmitted sensitive video,
audio, and annotations without waiting for a stalled network write. Previously
accepted or transmitted media cannot be retracted; explain that limit accurately.
Generation checks and resume barriers must prevent old content from appearing
after pause, source changes, reconnect, or resume. A reconnect retains the current
presentation flags and sends fresh appropriately styled content, never a backlog.

## Audio, clocks, and backpressure

Read the existing selected microphone/desktop mix; do not capture new physical
sources or route sound to speakers. Add independent streaming audio privacy and
reader/timing state so pausing/stopping streaming cannot silence or consume audio
needed by recording or the virtual microphone, and vice versa. Preserve the
existing explicit desktop-source and no-broad-fallback policies.

Use monotonic clocks, valid increasing timestamps, a bounded audio buffer, and
defined AV synchronization/drift behavior. On reconnect, create a clean encoder/
muxer session with fresh codec headers and a decodable first keyframe. Drop stale
queued work rather than broadcasting seconds-old content after a slow connection.
Prefer dropping video before encoding; do not indiscriminately drop encoded
reference packets and leave the stream undecodable. Define what happens when
lag exceeds the budget, and keep audio/video timing coherent when recovering.

Implement bounded reconnect attempts and capped backoff, interruptible immediately
by stop/shutdown. Report retry progress and final failure. Avoid uncontrolled
retries for invalid configuration, missing credentials, or clearly rejected keys.
A new explicit start can retry after failure. Streaming failures affect only the
streaming lane; recordings remain recoverable and the virtual camera stays usable.

## Configuration and stream-key file

Add a documented `[stream]` section to the existing INI schema. Use explicit,
validated fields for service preset (custom/twitch/youtube), server URL, stream-key
file path, video/audio bitrate with named units, encoder preset, queue/lag budget,
connection/write timeouts, and bounded reconnect policy. Choose conservative,
documented defaults. Reuse the configured composition resolution/frame rate for
this version; do not add an unrelated per-output scaling system.

The stream key is the service-provided password allowing broadcasts to the user's
channel. It is not an encryption key. Always call its storage a **stream-key file**,
not a private encryption key file.

Store only its file path in Cast configuration. Do not accept the actual secret as
a command-line argument, IPC setting, panel field, or server-URL component. Read
the secret locally in the daemon/streaming worker when explicitly starting a new
session. Require a bounded, nonempty single-line value, optionally terminated by
one newline; reject malformed/oversized content. Require a regular file owned by
the current user with no group/other access, protect against unsafe symlink/file
replacement races, and give exact corrective instructions on error. Use an
absolute key-file path to avoid ambiguity between daemon and client directories.

Do not expand shell commands, environment variables, or `~` inside configuration.
Read with file APIs, never a shell. Keep the secret out of Config, State, panel
snapshots, status JSON, logs, error messages, shell history, process arguments,
release artifacts, and test fixtures. In particular, sanitize FFmpeg and network
errors/log messages that might contain the assembled credential-bearing URL.
Clear transient secret buffers when retiring a session where practical.

Accept only supported RTMP/RTMPS server addresses with bounded, validated syntax.
Do not let arbitrary FFmpeg URL schemes or user-supplied options escape that scope.
Preserve required non-secret service URL components and join keys correctly.
Prefer RTMPS where supported; certificate verification must remain enabled.

Provide simple safe setup instructions for creating/editing a mode-600 stream-key
file without typing the actual secret into a shell command. Use placeholder-only
examples. File permissions help protect local storage; do not claim they encrypt
the file. Mask sensitive destination components in displayed status.

Runtime setting batches and reload must validate atomically. Changes requiring a
new connection/encoder are rejected while the session is active, connecting, or
reconnecting, with instructions to stop first. Harmless presentation changes
remain available. Never persist runtime edits automatically. Failed validation or
resource preparation leaves current settings and unrelated outputs intact.

## CLI, status, preview, and doctor

Implement and document at least:

```text
cast stream start
cast stream stop
cast stream pause|resume|toggle
cast stream freeze|unfreeze
cast stream blur [on|off|toggle]
cast stream unblur
cast stream status [--json]
cast preview target virtual|record|stream
```

`stream toggle` toggles solid pause on an existing session; it never starts or
stops a stream. Pause/effect commands require an existing session, including a
connecting/reconnecting one. Stop is safe and idempotent, including during
connection/reconnect, and cancels every outstanding retry. Keep standard bounded
IPC, acknowledgement, argument validation, and nonzero error behavior. Start
acknowledges accepted asynchronous startup, not confirmed public broadcast; users
can inspect status for connection success/failure.

Use `cast settings stream.KEY VALUE` for session configuration, plus configuration
reload for persistent file changes. Extend top-level and per-command help, Bash,
Zsh, and Fish completions, and examples. Never suggest completion of secret values.

Add a `stream` object to existing `status --json`, alongside the renamed `virtual`
object. Preserve unrelated existing fields; the `live` to `virtual` status change
is intentionally incompatible and must not leave duplicate legacy fields.
Include lifecycle state, pause/freeze/blur flags, safe destination/service name,
connected/session duration as clearly defined values, actual outgoing bitrate,
queue/drop information, retry attempt/next retry, and a sanitized actionable error.
Distinguish encoder acceptance, successful network writes, and service/viewer
delivery. Do not invent viewer counts or claim that an accepted network connection
proves a publicly visible broadcast.

Extend the independent floating preview with a Streaming target showing the
actual streaming lane's presentation effects/annotations before encoding. Keep
local status decorations out of transmitted frames. Add `annotations.stream_keys`
and `annotations.stream_clicks` using the existing per-lane policy. One lane's
privacy history must never leak through another lane's queue.

Extend read-only `doctor` with local streaming checks: available encoder/muxer/
protocol/TLS support, valid settings, and key-file accessibility/permissions,
without displaying or transmitting the key. Doctor must not start a broadcast,
consume devices, or probe a public endpoint with credentials.

## Panel UX

Follow the existing Clay/SDL3 panel, PRODUCT.md, and DESIGN.md. Preserve crisp
native-density text, square controls, focus/keyboard support, narrow-window
layout, fast scrolling, progressive disclosures, and separate floating preview.
Do not embed preview again or broaden Home into a dense dashboard.

Add a compact Streaming output group on Home beside the existing output groups.
Clearly label **Virtual camera**, **Recording**, and **Streaming**; keep action
names explicit: **Start streaming**, **Resume streaming**, **Pause streaming**,
and **Stop streaming**. Use existing freeze/blur conventions and show connecting,
retrying, failed, and paused states distinctly. Start streaming opens the
connection in solid pause; do not silently follow it with resume. Show enough
destination/audio information to make Resume streaming an informed action.

Put destination, stream-key file path, service/quality settings, and advanced
network controls in a focused Streaming setup screen or disclosure. Explain the
key file plainly. Expose only its path, never its contents. Edits remain drafts
until Apply/Enter and reflect acknowledged daemon settings. Explain locked
settings while active instead of silently ignoring edits.

The floating preview's target selector includes Streaming. Successful streaming
start/resume actions select that target using the established acknowledgement and
manual-choice rules. Preview remains optional; no new streaming countdown is
required. Preserve recording's existing cinematic countdown and hide barriers.
Output privacy/stop controls must remain reachable at the minimum window size.
Closing the panel never stops, starts, or resumes any output.

## Verification and completion

Use synthetic media and local/private ingest fixtures. Do not use the user's
desktop, webcam, real credentials, or public service account for automatic tests.
Add meaningful tests to existing build/check targets, including:

- Decode received H.264/AAC and check composition pixels, audio energy, frame
  cadence, timestamp monotonicity, and AV sync over sustained operation.
- Streaming alone and all three outputs together; independently toggle effects
  and stop/fail each output without disrupting the others.
- Solid pause, freeze, moving blur, precedence, annotations, shared styled text,
  group-pause restoration, and no unsolicited start/resume.
- Sensitive queued frames/audio across pause, resume, source changes, stalled
  writes, and reconnect; prove retired generations cannot leak or restart.
- Rejected connections, invalid credentials, disconnect mid-write, a server that
  stops reading, unavailable DNS/connection, reconnect success/exhaustion, fresh
  keyframe/headers, and stop/shutdown while blocked or backing off.
- Bounded memory/queue growth and capture/control responsiveness under network
  backpressure; document measured CPU/memory cost of simultaneous encoding.
- Stream-key validation, permissions/ownership/race protection, URL validation,
  atomic configuration failure, and secret redaction across every display/log/
  error path, including FFmpeg-originated diagnostics.
- RTMPS against a local TLS fixture, including trusted-certificate success and
  invalid-certificate failure; do not substitute disabled verification.
- The complete `live` to `virtual` migration, including help/completions, startup
  options, configuration, annotations, preview targets, status consumers, and
  panel actions. Assert obsolete spellings fail without mutating output state;
  no compatibility aliases or duplicate legacy status fields remain.
- Native panel pointer/keyboard actions, drafts, connection/error states, preview
  selection, and minimum-width/high-density layout in bounded visual passes.
- Xorg, optional Wayland-only, and PANEL=0 builds; streaming must not introduce
  hidden X11 or SDL dependencies into shared/media code.

Integrate required fixture dependencies into the documented test environment and
CI without adding unnecessary runtime dependencies. Run existing regression
checks as appropriate and sanitizers for new concurrency/resource ownership.
Do not leave release-critical paths as TODOs or label unexecuted tests as passed.

Update README, complete example configuration/default schema, CLI/configuration/
media/panel documentation, man page, completions, capability/requirements matrix,
and verification notes. Include Twitch and YouTube setup instructions, the
three-output naming explanation, stream-key file creation, pause vs stop
semantics, CPU/upload requirements, troubleshooting, and a manual service
acceptance checklist. Preserve the existing installation/release pipeline and
license notices; document any justified dependency changes.

The feature is complete when a user can configure one destination, start safely,
resume, apply effects, stop, and recover from network failure through CLI and
panel while the other outputs remain responsive and correct. Report concrete
changes, executed evidence, remaining service/hardware acceptance limits, and
release readiness. Do not stop at a plan, prototype, or recorder-URL shortcut.
