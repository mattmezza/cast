# Project container foundation

`cast_project_save` / `cast_project_load` persist a versioned document and a
bounded `CastTimeline`. They do not record, decode media, export an edit, activate
an output, grant runtime consent, or implement paid workflow algorithms. Actual
recorded takes are flattened composition media. A media reference or extra clip
track does not imply separately recorded screen/camera tracks.

Version 1 starts with `CAST_PROJECT 1`, stores schema-keyed configuration values,
presets, stable media IDs and clips, and ends with `END`. Strings are hex-encoded
UTF-8/filename bytes so whitespace/newlines cannot inject records. The document
is at most 1 MiB, with the timeline's 64-media/128-clip/16-track/seven-day limits.
Unknown versions, records, settings, duplicate settings and malformed numbers are
rejected. There is no pre-existing project format to migrate: future versions
need an explicit migration, rather than guessing at incompatible layouts.
Configuration assignment uses `config_set_value` and the complete candidate is
checked with `config_validate`; timeline references/ranges use `timeline_validate`.

The explicit setting allowlist covers composition, appearance/annotations, frame
geometry, audio gain, encoder preferences and named presets. Registered shared
extension metadata for zoom, cursor, transcription, subtitles and notes can
preserve safe numeric/enum/color and font values without linking private feature
implementations. Extension paths and arbitrary text are excluded. In particular,
script/model paths and script contents are not embedded. Socket/device routes,
recording directory, credentials/server URLs, licensing identity, capture/output
activation and transcription activation are excluded. Loading overlays the
caller's local base configuration, preserving those machine/session settings.
It must still be applied through the application's normal permission and safety
checks; loading a file alone does not apply it to a running capture session.

Media and logo references inside the project directory are stored relatively;
external absolute references remain absolute. Relative references resolve against
the project file's directory, including when an entire folder is moved. Missing
media remain in the timeline with `missing=true`; `CastProjectReport` lists their
IDs and counts missing logo assets. Resolution checks local regular-file existence
without reading media contents. Media symlinks may resolve to regular files; the
project container itself must be a regular file opened without following symlinks.
The path scheme is local filesystem data, not URLs or an embedded asset archive.

Saving validates before writing, creates a same-directory 0600 temporary, writes
and syncs it, then atomically renames it. Invalid input or a write failure before
rename leaves the committed document intact. Existing destinations must be owned,
regular, singly linked files. The parent directory is synced best effort after
commit; a filesystem that rejects directory sync cannot provide a stronger crash
durability guarantee. Loading is bounded and transactional: configuration, timeline
and report destinations remain unchanged on failure. Runtime capture locks are
never restored from a document.

`cast_project_autosave` uses the same container/writer at an explicitly chosen
recovery path. Recovery is an explicit load; this layer does not schedule writes,
select a "latest" file, or overwrite the main project automatically. Undo/redo
history is session-local; committed clip/media state, next ID and revision persist.

`tests/test_project.c` covers roundtrip configuration/presets and Unicode/newlines,
relative media paths, missing media, recovery, mode 0600, invalid-save preservation,
symlink/directory rejection, oversized files, unsupported versions, forbidden and
duplicate settings, invalid clip bounds/numbers and unchanged outputs on failures.

## Daemon session integration

`project_session.c` keeps the timeline/history on the daemon thread and uses one
bounded worker slot for project save/open/recover and imported-media probing.
Commands acknowledge queuing; `CastProjectSnapshot.busy`, `status` and `error`
report the eventual result. Closing the session joins its worker before freeing
storage. Snapshot publication and clip edits perform no filesystem I/O.

Opening/recovering a project requires recording, countdown, finalization and
streaming to be idle. Completion checks that outputs, configuration and project
revision did not change during loading, then uses `app_apply_candidate`; stale
results are discarded. Saving snapshots document/configuration for the worker.
Edits can continue during saving, and a completion only clears dirty state when
that exact revision/configuration remains current. A saved project with unsaved changes receives a sibling `.autosave` snapshot
every thirty seconds through the same bounded worker. This never changes the main
project path or clears dirty state. Unnamed projects need an initial Save As to
establish a recovery location. Symlink/unowned/nonregular recovery destinations
are refused by the same atomic writer. Recovery remains an explicit command.

`timeline import PATH [TRACK]` probes an already-opened local regular file through
custom AVIO, with a ten-second interrupt deadline, bounded probing/analysis, file
protocol only and a mov/matroska/webm/wav/mp3/flac/ogg/aac/png/jpeg format allowlist.
Playlists, network inputs, special files and symlink inputs are rejected. The
worker checks that the path still names the probed inode before committing a
reference. No decoder/export pipeline or network fetch is started by importing.
Video/audio need a known duration; still images receive a five-second initial
clip. Imported clips append to their chosen track.

Recording adds a single flattened composition take to the end of track 0. Its
clip stays locked through finalization, grows with the recorder's active duration,
and receives the final recording path when finalized. The rest of the timeline
can be edited while recording; the future part of track 0 remains reserved so
edits cannot obstruct the growing take. Undo/redo is unavailable while recording
and never restores a history snapshot containing capture locks. Timeline seek enters asynchronous still-frame review while outputs are idle;
`timeline live` returns to live composition. Review does not mix live capture or
start audio playback.

`tests/test_project_session.c` exercises the real worker/codec and FFmpeg probe
with isolated application/recording handoff stubs. It covers dirty-save revisions,
stale and idle project open, WAV import, playlist/network rejection, captured-clip
locking/finalization, concurrent editing on another track and undo/redo. These are
not claims of end-to-end application command routing or separately captured tracks.

`timeline_render.h` provides a thread-confined CPU renderer and an asynchronous
movie exporter. Visual tracks composite in track order, contain fitted against a
black canvas; decoded RGBA alpha is preserved during blending. Video embedded
audio and audio clips mix at stereo 48 kHz with clip gain/mute and final limiting.
Trim/source offsets and document positions use the timeline's nanosecond clock.
Missing sources fail with a media ID. Input decoders use regular local file
handles, a format allowlist, bounded probing, and cancellation/deadline callbacks.
The bounded reader cache holds eight decoder instances rather than entire media.

Exports use the configured dimensions, frame rate, video/audio encoders and
Matroska/MP4/MOV container. A worker owns conversion, encoding and muxing through
linked FFmpeg libraries; no external executable, capture device or audio device
is started. Output remains a mode-0600 sibling temporary until its trailer and
file synchronization succeed. Failure/cancellation removes that temporary and
preserves an existing destination; source media cannot be an export destination.
An optional worker frame callback allows application/private overlays without
putting private feature code into the renderer. Callback state must remain alive
until the export is joined. These APIs are a rendering foundation; application
playback/export controls require their own integration.

`tests/test_timeline_render.c` generates stereo WAV data, exports colored FFV1
frames and FLAC audio, reopens that movie, and checks trim/split position, gain,
forward/reverse seeking, frame color, cancellation preservation and missing input.

The project controller exposes `timeline export PATH`, `timeline export status`
and `timeline export cancel`. It copies configuration and timeline state into an
independent worker; later document edits do not change that export. Captured clips
still being recorded/finalized and known missing references are rejected. Input
opening/probing and all output writes run on the export worker. Progress is
available as `CastProjectSnapshot.export` with an `exporting` flag. Session close
cancels and joins the export. This path never replaces an existing output: final
publication uses an atomic hard link, so even a destination created during export
causes publication to fail without changing that destination.

Permanent isolated targets on Linux and macOS are `check-timeline`,
`check-project`, `check-project-session`, and `check-timeline-render`. They exercise
public document and renderer code independently of edition feature providers.

Review workers keep immutable document/configuration snapshots and coalesce seek
requests. The session caps review frames to a 1920×1080 contain fit, copies each
completed generation once on the controller tick, and exposes a borrowed cached
frame for composition-lane transport. Review clears before timeline edits,
project replacement, configuration changes, recording or streaming can publish
stale pixels. Invalidation cancels workers without joining on the controller;
finished workers are reaped on later ticks. A replacement seek may need to retry
while the previous worker finishes cancellation. `check-timeline-preview` tests
real generated video, generation coalescing, color/position, capture-lock refusal,
missing media and shutdown. Session tests cover review/live and privacy barriers.

`timeline play` starts video review from the document playhead (or restarts
at zero after the end); `timeline pause` retains its current frame and playhead.
Explicit play also requests bounded timeline audio playback on the default output;
seek alone does not open an output. `timeline play silent` explicitly retains
video-only review and immediately retires any previously queued audio. Audio failure leaves video review active and
is reported separately. No live microphone monitoring is implied. The controller
uses monotonic elapsed time and requests at most the configured frame rate capped
at 30 fps. It waits for each decoder generation to complete before requesting the
latest clock position, skipping late frames instead of creating a decode backlog
or continually cancelling slow decoders. At the end it stops at the duration and
renders the final valid instant. Seek pauses transport; live, edits, project/config
replacement and active outputs invalidate its clocks and cached review. Snapshot
`playing` describes transport separately from `reviewing` and `preview_busy`.
A test-only injected clock verifies advancement, pause, resume and end behavior.

`timeline relink MEDIA_ID PATH` repairs a moved/missing media reference using the
same bounded local-file probe as import. It requires idle recording/streaming and
no active captured take. The probe runs asynchronously; applying it rechecks the
document revision, configuration and output state. Replacement media must have
the same audio/video/image kind, and its known duration must cover every existing
clip's source offset plus duration. Failed, short, incompatible or stale probes
leave the document unchanged. Success retains the media ID, clip IDs and all clip
edits, clears the missing flag, checkpoints undo history and marks the project
dirty. Undo/redo restores the previous reference and missing state. Snapshots
continue to expose only media IDs, basename labels and missing flags, without
full paths. Session tests cover missing references after a file move, short-file
rejection, stale-generation rejection, successful relink and undo/redo.

`make check-workspace-projects` runs the production daemon and CLI against a
private socket/configuration/HOME with synthetic 640×360 video at 10 fps, camera
and virtual output disabled, and microphone/desktop/virtual audio disabled. It
records a short flattened take, saves/reopens a project, imports that actual movie,
exercises seek/silent-play/pause/live and invalid relink, then exports and fully decodes
the exported video. It also checks no-overwrite behavior. FFmpeg/FFprobe programs
are test inspection dependencies only; the application export uses linked
libraries. The test terminates only its owned daemon and removes all temporary
media. It verifies CLI/socket routing and export progress, without opening a UI,
requesting device permissions, or claiming UI snapshot/visual integration.

Timeline audio review decodes the immutable project mix on a dedicated worker via
`timeline_renderer_audio`, including embedded video audio and audio-only clips,
trim offsets, clip gain and mute. It reuses the bounded 48 kHz stereo monitor sink
with 10 ms chunks, approximately 20 ms scheduling lead and a 100 ms maximum queue.
Default-output opening, decoding, health checks and route closing stay off the
controller thread. Late decode skips to current transport time rather than
replaying an accumulated backlog. A failed route is not retried automatically.

Pause, seek, live, global privacy, edits, replacement and cancellation retire the
PCM epoch immediately. Global privacy does not resume audio when released;
another explicit play is required. Native buffers already handed to hardware
cannot be retracted, and output latency depends on the host. Service retirement
and route close happen asynchronously; session shutdown joins owned workers.
Snapshot fields `playback_audio_requested`, `playback_audio_running` and
`playback_audio_error` distinguish a request from an acknowledged output route.
No output picker, headphone detection, live clip insertion into recording, or
live microphone/desktop mixing is implemented by timeline review.

`check-timeline-audio` uses actual generated WAV media with a fake output backend,
including gain, source seek, rapid play/pause/seek, immediate epoch silence, pause
during slow startup, failure/no-retry and asynchronous cancellation. It opens no
real output device. Linux and macOS native playback, route changes, audible A/V
sync and device permissions still require hardware acceptance.

`timeline trim-left ID NEW_START_NS` moves a clip's left edge to an absolute
document position, adjusting its source offset and duration together while keeping
the right edge fixed. Extending requires available source material; zero-length
clips, track collisions and capture-locked clips are rejected without changes.
The complete trim is one undoable edit. `timeline trim` continues to set the
source offset and duration directly.

Project snapshots expose up to four decoded asset views for the first four
non-capture-locked document clips. Each carries clip/media IDs, document revision,
generation and exact source span. Video thumbnails are box-filtered to 80×45 RGBA;
waveforms contain up to 256 mono peaks (maximum of the decoded stereo peaks),
covering at most the first thirty seconds of that clip's source range. Images
have no waveform; unsupported, missing and audio-absent inputs report errors
instead of invented content. `timeline assets focus CLIP_ID` prioritizes a selected finalized clip in the first
slot, followed by up to three other clips in document order. This read-only
request changes no document revision, dirty state or undo history. Repeating the
same focus is inert; interfaces should request once per selection change rather
than retry each frame. Visible-window prioritization remains future work.

A bounded persistent worker owns all asset probing and decoding. Document edits,
new/open and source replacement immediately invalidate old views. Active outputs
cancel pending work and defer new decoding while retaining already decoded views
for unchanged clips; idle resumes requests. Snapshot publication only copies
bounded memory and never probes files or opens audio devices.

`timeline source PATH` imports an independently usable source into the project's
media bin through the same bounded asynchronous local-file probe. It adds no
montage clip and is allowed during capture without changing the captured take or
its track. Source references persist in the project, with duration and missing
state in snapshots; the import is one undoable document edit.

`project_session_live_source` creates a caller-owned immutable single-source
timeline using memory only. It rejects unknown/missing sources and active capture
media, and checks offset/duration against the probed source (maximum seven days).
A zero duration selects the remaining audio/video source; still images start at
zero and default to five seconds, with a five-second maximum. Failed calls leave
the output unchanged. This API prepares a source document; it does not insert
media into live capture or open playback/output devices.
