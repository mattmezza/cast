# Architecture decisions

cast is one foreground C executable. Its client sends bounded NUL-separated arguments
through an owned Unix socket. A serial event loop orders commands and acknowledges
privacy transitions only after media queue barriers and neutral virtual-frame publication.
A separate lock protects single-instance ownership even when a socket is stale.

Shared data is RGBA8, with explicit dimensions, stride, ownership and monotonic time.
Platform modules hide Xlib/XRandR/XShm/XInput2 and optional portal D-Bus/PipeWire types.
Screen coordinates are source-local; composition applies crop, eased zoom, fitting and
output scaling identically to screen pixels, cursor and click markers. Camera cropping
and mirroring are independent. Backend failures produce neutral content and errors.

FFmpeg libraries provide camera MJPEG decoding, format conversion, codecs and muxing.
A camera worker owns V4L2 buffers and decoding, publishing only the latest owned RGBA
frame. Main-loop reads copy that snapshot; privacy epochs and per-buffer queue epochs
reject pre-boundary capture/decoding without waiting for device work.
Recording has a bounded worker queue, independent control/encoder locks and monotonic
active-time timestamps. Status and frame admission never wait for codec work. Stop
requests worker finalization; status exposes completion and shutdown waits for it. Cut
flushes queued samples and excludes its interval, preserving one open container.
Solid pause continues encoding neutral video and silence at the normal cadence.
Recording audio silence has its own epoch gate, independent of the cut clock.
PipeWire provides audio capture and an optional virtual source; no physical playback
sink is created. Software encoding is the baseline. Matroska is the default container.

Configuration uses maintained BSD-3-Clause-licensed inih, vendored without local parser changes.
A strict schema layer validates sections, units and duplicate keys with source lines.
Precedence is defaults, config file, startup overrides, then session commands.

Virtual camera, recording and streaming each have independent solid-pause, freeze and blur flags.
Composition is copied or frozen first, then blurred and tinted; solid pause selects
neutral content above both. Recording cut gates file admission above every visual
mode. Freeze captures screen and camera together without transient keys/clicks.
Privacy pause and source changes replace retained frozen pixels with neutral content.
Blur is a presentation effect, not a confidentiality guarantee. Pause, freeze, blur
and cut silence their audio lanes and discard queued audio before acknowledgement.
Group pause remembers only solid states it changes; independent solid commands
supersede remembered restoration. Orthogonal freeze/blur/cut controls preserve it.
Group resume cannot reopen a cut recording. Cut resume uses the configured start
countdown before reopening media admission on the same encoder/container; cancelling
leaves it cut. Composition reset/presets never change output flags. Runtime
configuration is never persisted automatically.

Fontconfig resolves the configured system fonts and FreeType renders optional UTF-8
titles/subtitles/footers. Font changes are file-only and prepared before applying reload.
The separate static composition text permits runtime font selection and caches its
transparent glyph frame until content/font/style changes. Local PNG/JPEG/WebP/BMP
logos are decoded through libavcodec into bounded RGBA frames in composition_assets;
no image protocols, shell or per-frame file reads are used. Candidate image/font
resources are prepared before configuration changes are committed.

Text templates expand bounded date/time placeholders against one local wall-clock
snapshot, while recording/cut timing remains monotonic. Blur uses bounded reduced
images and separable passes so the radius does not multiply full-frame work.

Hardware validation is distinguished from synthetic acceptance. Optional Wayland
features report capabilities; global input and interactive selection can be unsupported.

The optional panel is a client of the same executable. Clay lays out native SDL3
controls; SDL3_ttf renders the embedded font. An asynchronous bounded client queue
and read-only sealed shared memory carry acknowledged state and small actual-output
frames in the internal transport; the control-only panel does not request or render
frames. Preview uses the independent platform presentation window. The daemon never
waits for panel rendering. Xorg validates the peer's panel
window identity and neutral-masks its frame in monitor/region captures.

Setup and completion instructions are local commands, independent of configuration
and daemon availability. Completion scripts and the Arch installer are embedded so
relocated binaries retain those tools. The updater launches the fixed embedded
installer with validated argv through fork/exec; user arguments are never interpolated
into shell code. It verifies the selected GitHub package checksum before pacman.

Stage geometry places the screen opposite the camera anchor and allows overlap
based on their aspect-preserving sizes. Shared gradient stops and reduced blurred
backdrops fill uncovered regions. Screen masks also clip its pointer/click layers;
logo and text alpha-blend above source layers before lane pause/freeze/blur selection.

## Independent streaming lane

The main loop composes a third frame with its own pause/freeze/blur and annotation
flags. It shares source geometry and presentation resources, never the virtual-camera
whole-output mirror. Streaming uses continuous monotonic media time; recording cut
changes only the recording clock. Audio uses an independent streaming privacy gate.

A bounded supervisor admits owned raw frames without waiting for network I/O. A
`posix_spawn` child re-enters the same executable's private worker mode and owns FFmpeg
H.264/AAC/FLV and RTMP/RTMPS state. Process isolation makes cancellation bounded even
when a resolver or TLS implementation ignores FFmpeg's interrupt callback. There is
no fork into a multithreaded encoder. A sealed, bounded shared-memory handoff holds
`queue_frames` raw frames plus one current cadence frame (at most 512 MiB), and a
one-second stereo audio ring. Only the newest eligible raw frame is encoded. Video scheduling includes AAC
encoder priming so direct FLV submissions remain globally timestamp-ordered without
a mux interleave backlog that could retain retired privacy media. Codec
work, network writes and finalization never hold the daemon's status/control lock.

Privacy epochs retire queued and converted-but-unsubmitted old media, purge audio
and force a fresh keyframe. Ordinary presentation changes retain the connection.
Already accepted codec/network media may remain downstream; a pause acknowledgement
cannot retract bytes already sent. Lag/write deadlines retire a stalled child, and
finite exponential backoff starts a fresh codec/mux session at the current privacy
state. Stop cancels both the child and any pending retry.

Only an owned regular mode-0600 key file is accepted. Its secret is read transiently
inside the worker, never included in snapshots or diagnostics. TLS verifies peer
certificates and hostnames using system trust or the configured CA file. Public
status exposes lifecycle, privacy state, counters and sanitized authority information.

The panel retains the existing field table, button/glyph helpers and transport client.
Operate commands apply immediately; Compose stages per-field drafts and submits one
validated atomic settings batch. Acknowledgements advance only the matching draft
revision, so newer edits survive an older reply. Header, tabs, exclusion and feedback
remain pinned while the body scrolls. Native preview target and state controls use
cached bundled Inter geometry outside the outgoing composition.
