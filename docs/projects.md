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
that exact revision/configuration remains current. This module does not currently
schedule periodic autosaves; an explicit recovery file uses the same validated
project container.

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
and never restores a history snapshot containing capture locks. Timeline seek
currently moves the document playhead only; it does not claim media playback.

`tests/test_project_session.c` exercises the real worker/codec and FFmpeg probe
with isolated application/recording handoff stubs. It covers dirty-save revisions,
stale and idle project open, WAV import, playlist/network rejection, captured-clip
locking/finalization, concurrent editing on another track and undo/redo. These are
not claims of end-to-end application command routing or separately captured tracks.
