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
requests worker finalization; status exposes completion and shutdown waits for it. Pause
flushes queued samples and excludes its interval, preserving one open container.
PipeWire provides audio capture and an optional virtual source; no physical playback
sink is created. Software encoding is the baseline. Matroska is the default container.

Configuration uses maintained BSD-3-Clause-licensed inih, vendored without local parser changes.
A strict schema layer validates sections, units and duplicate keys with source lines.
Precedence is defaults, config file, startup overrides, then session commands.

Live privacy pause and recording pause are independent. Group pause remembers only
running outputs it changes; an independent command supersedes that output's remembered
state. Repeated group pause is idempotent. Freeze keeps one composed live frame and
silences virtual audio; privacy pause overrides it. Composition reset/presets do not
change either output state. Runtime configuration is never persisted automatically.

Hardware validation is distinguished from synthetic acceptance. Optional Wayland
features report capabilities; global input and interactive selection can be unsupported.
