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
