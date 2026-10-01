# Media, recording and privacy

cast uses FFmpeg libraries inside the process for webcam decoding, pixel conversion,
software encoding and muxing. It uses PipeWire for microphone/desktop capture and an
optional virtual microphone. Video and audio are separate conference devices.

## Webcam and virtual camera

The webcam accepts streaming, single-plane V4L2 capture devices. It enumerates formats
and prefers MJPEG, followed by YUYV, UYVY, NV12, planar YUV420, RGB24 and BGR24. FFmpeg
decodes MJPEG and converts selected input to owned RGBA frames. Deprecated JPEG
YUV aliases are normalized with explicit color range/matrix metadata, preserving
colors without repeated swscale warnings. One worker owns real-device polling,
dequeue/requeue, decoding and conversion. It drains bounded ready buffers and decodes
only the newest. Main-loop reads copy the latest published RGBA snapshot; they never
wait for device capture or decoding. Two owned frames bound the handoff memory. Missing, unplugged or stale input produces an absent
camera layer with an error; it never substitutes another camera. Explicitly select a
reconnected device again, including the same path, to retry a failed handle.

Privacy/source barriers immediately clear publication and advance its epoch without
waiting for the worker. In-flight older work cannot publish afterward. The worker
drains ready buffers at each boundary; monotonic V4L2 timestamps reject late older
captures. Unknown, malformed or future timestamps additionally require that the
buffer was queued in the current epoch, so a late old exposure cannot bypass the
boundary drain. Hardware latency still needs acceptance testing. A camera temporarily awaiting its next frame remains available for
later acquisition.

Live output opens an existing V4L2 output device and negotiates the exact configured
canvas size as YUYV, RGB24 or BGR32. It converts the composed RGBA frame and writes in
nonblocking mode. Exclusive-caps loopback advertisement is dynamic; the known
v4l2loopback driver can negotiate OUTPUT even while advertising CAPTURE. A busy
producer, disappeared device or locked/incompatible format returns an actionable
error. Physical capture-only cameras are rejected. Busy/short writes are reported and counted as drops. The output size
and frame rate stay fixed across source/layout changes. The `none` device discards output
for synthetic testing and cannot establish virtual-camera consumer compatibility.

Create v4l2loopback separately with `exclusive_caps=1`; see
[setup commands](troubleshooting.md). Some consumers discover the device only after the
producer starts. No conferencing application's resolution, compression or text
readability is guaranteed by capture or recording tests.

## Audio sources and timing

Microphone, desktop capture and virtual audio all default off. Enabling a microphone
uses the selected source, or resolves PipeWire's default microphone when no name is
specified. Desktop capture requires an explicit node name, id or serial. `cast audio
list` identifies source classes and descriptions. An Audio/Sink selection captures that
sink's monitor, which can include other call participants and system audio. A
Stream/Output/Audio selection targets one application's playback where the graph permits
it. Source names/ids may change after reconnection.

Disappearance, stream errors and unsupported renegotiation clear that lane's buffered
samples and produce silence with an actionable error. Nothing automatically selects a
broader source. Explicit source selection can reconnect a dead lane. Selecting the same
node for microphone and desktop is rejected to avoid duplicate microphone audio;
selecting cast's own virtual source is rejected to avoid feedback. cast creates no
physical-speaker playback route.

Each capture lane negotiates 48 kHz stereo interleaved float audio. PipeWire handles
conversion from device formats. A bounded two-second ring stores samples with monotonic
capture timing estimated from callback arrival and buffer duration. Adjacent callbacks
within 20 ms join continuously to absorb scheduling jitter. Missing/expired samples are
silence. Gains are independent and the final sum is clipped to [-1, 1]. Recording reads
a 30 ms delayed horizon and replaces unavailable audio rather than moving video timing
to a device clock. This limits queue growth; measured physical AV latency/drift over a
long session remains a hardware acceptance item.

The optional named virtual source defaults to `cast-microphone`. Its callbacks read the
same mix, but live pause and live freeze force silence independently of local recording.
Off/failed lanes contribute silence. Each barrier clears capture rings, trims buffers
older than the new monotonic boundary and flushes PipeWire stream queues. Already
consumed audio in a conferencing application lies outside cast's control. cast cannot
mute a physical microphone selected directly by that application; use its own mute
control or select cast's virtual source.

## Recording state and queues

One recording uses the configured container/video/audio encoders. Defaults are Matroska,
software libx264 with CRF 23/veryfast, and stereo AAC at 128 kbit/s. Encoders initialize
when starting a file; unavailable codecs or incompatible codec/container combinations
return errors. Hardware encoding is not required.

A separate recording worker owns encoder and muxer resources. The video queue owns
copies of composed frames and defaults to eight frames; configuration permits 1..120.
When full, admission drops the oldest queued frame to keep recent content. Admission
also drops on brief queue-lock contention. Status, duration, filename lookup, frame
admission and stop do not acquire the codec lock. A slow encoder therefore does not
hold those paths or the independent virtual-camera writer. The worker bounds regular
audio catch-up batches to 100 ms. Lost or expired audio becomes silence.

Recording timestamps use accumulated active monotonic time. Pause closes admission,
invalidates queued frames, clears unsubmitted audio and replaces the short unencoded
active edge with silence. Resume starts a new active segment in the same open file;
wall-clock time spent paused is removed from both media timelines. Frames older than
the current admission boundary are rejected. Already accepted codec work before a
privacy boundary may finish. A recording privacy barrier waits for any such in-flight
codec call before acknowledgement; live neutral video and virtual-audio silence are
published first. Finalization never holds the queue/status lock, and live privacy does
not wait for a recording that is already finalizing.

`record stop` closes admission immediately and acknowledges `recording finalizing: PATH`.
The worker drains previously accepted active frames, flushes codecs, writes the trailer,
flushes the file and fsyncs it. Poll `record.finalizing` in `cast status --json`; wait for
false before using the file or starting another recording. The foreground daemon prints
completion or a recording-specific error. Quit waits for completion. A paused recording
is stopped without adding its paused interval. Countdown and group-pause restoration
are described in the [CLI contract](cli.md).

Explicit paths are created with exclusive creation, mode 0600 and no final-component
symlink following. Existing files are never overwritten. Automatic names include local
date/time, process id and a collision counter in the configured recording directory.
Runtime paths are interpreted by the daemon, so relative paths use its working directory.

## Failure and recovery

Encoder, write, final-I/O and fsync failures stop only the recording and report the
filename/error. They preserve the started partial file and allow a later recording;
live output keeps its independent path. Failure during initial setup removes the new,
unstarted file. Matroska uses clusters limited to one second or one MiB to improve
partial-file recovery opportunities. This does not promise that a crash or full disk
leaves every frame decodable. Ordinary MP4 also depends on completed indexes/trailer.

Work on a copy when attempting recovery:

```sh
ffmpeg -i partial.mkv -c copy recovered.mkv
```

Success depends on bytes actually written and cannot be guaranteed. Check destination
permissions/free space with `cast doctor` before long captures.

## Executed verification and remaining acceptance

`tests/test_media.c` opens real FFmpeg encoders/muxers, feeds synthetic RGBA frames and
480-sample tone buffers, then decodes both streams through FFmpeg. It verifies:

- Simultaneous live/recording and recording-only paths, repeated same-file pauses, removal
  of a 600 ms interruption, audio energy and AV timing after resume.
- Privacy invalidation of queued green frames, rejection of an old frame after resume,
  and absence of an injected sensitive-audio pulse in decoded output.
- A bounded full queue and a 300 ms delay while the encoder mutex is held, with status,
  filename, duration, live writing, stop and live privacy during finalization remaining
  responsive; new recording is rejected until finalization ends.
- Injected encoder failure and ENOSPC in the recording write callback, retention of the
  partial file, operational live output and a successfully decoded next recording.
- Clipped microphone/desktop mixing, independent virtual silence, stale-buffer rejection,
  disabled lanes, no overwrite and preservation of the camera after failed reconfigure.

An optimized run on 2026-10-01 decoded 32 video frames for about 1.060 seconds of active
recording; AAC ended at 1.066 seconds and the largest video timestamp gap was 34 ms.
The delayed encoder dropped 16 burst frames; the combined status/live/stop/privacy calls
completed in 0.41 ms. These are small 160x90 synthetic acceptance results, not 1080p30
performance or a physical latency measurement. Address/undefined sanitizer verification
of the media suite also passed; the complete record is in [verification.md](verification.md).

Post-reboot checks exercised the physical webcam, loopback consumer and PipeWire
mic/virtual-source readiness and privacy silence. The optional physical camera test
is `CAST_TEST_CAMERA=/dev/video0 build/x1-w0/test_media`: it requests 1920x1080,
checks fresh owned-frame dimensions/stride/timestamps, and reports unique-frame
counts and mean/max main-read latency without exporting images. Device access must
be free; do not run this while another cast daemon owns the camera.

Deterministic worker tests hold decoding across a privacy barrier and release late
unknown/malformed-timestamp buffers after the boundary drain. They also cover stale
frames, disconnects and bounded shutdown. The isolated --camera-worker sanitizer
run passed address, undefined-behavior and leak checks. The full media sanitizer
reported PipeWire module allocations at process exit; a clean full dependency-leak
check is not claimed.

Long-run physical AV drift, conference receive-path compatibility and glass-to-glass
latency remain acceptance work. Follow [hardware-acceptance.md](hardware-acceptance.md).
Actual short-run throughput and memory are recorded in [verification.md](verification.md).
No kernel module or system audio configuration was changed by these tests.
