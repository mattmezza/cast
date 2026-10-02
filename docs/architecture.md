# Architecture decisions

cast is one foreground C executable. Its client sends bounded NUL-separated arguments
through an owned Unix socket. A serial event loop orders commands and acknowledges
privacy transitions only after media queue barriers and neutral live-frame publication.
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

Live and recording each have independent solid-pause, freeze and blur flags.
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
