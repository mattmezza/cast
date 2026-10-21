# Native workspace workflows

The Record workspace opens a docked timeline. Its controls operate on acknowledged
project snapshots: selecting a clip does not mutate daemon state. Record and Cut
use the existing recording lifecycle; the active recording clip stays locked.
Recorded takes contain the flattened composition, rather than separate camera
and screen recordings.

The left project panel supports new/open/save and native file pickers. Opening or
creating a project with unsaved changes requires a local discard confirmation.
Known project paths autosave to a sibling `.autosave` file; explicit recovery is
available through `project recover PATH`. Project containers exclude output
activation, device routing, licensing identity and streaming credentials.

Import video or audio through the timeline footer. Select a clip to split at the
playhead, remove or mute it. The left panel exposes start, source in and duration
in seconds, track and audio gain. Conflicting spans on one track and edits to an
active recording are rejected by the controller. Undo/redo apply to document
edits when recording is idle. Timeline ruler buttons seek an idle montage review;
Live returns to the current composition. Review decoding runs on an independent
worker and never activates an output or opens an audio playback device.

Export writes a new Matroska montage using the existing native FFmpeg libraries;
it reports progress and supports cancellation. Existing destinations are never
overwritten. Preview and export include imported video audio and audio-only
clips with their gain/mute settings. The timeline currently has coarse ruler
seeking and numeric editing; continuous transport, drag editing, waveform/thumbnail
caches and live clip insertion/mixing during capture remain separate work.

Streaming destination and encoding settings now use the existing backend,
including lifecycle/connection statistics and reconnect options. Secret contents
are not copied into UI snapshots. Microphone and desktop meters report measured
post-gain input, including freshness and clipping; monitoring playback is not yet
implemented.

Interactive workspace launches retain theme, panels, timeline visibility and
window dimensions in a separate private workspace preference file. Automated
frame/capture runs do not read or write these preferences. Unknown or invalid
preference files are preserved. Linux and macOS use the same UI/controller code;
macOS runtime qualification still requires a macOS host.

Pro settings, speech extraction, cue correction and subtitle rendering live only
in the private companion repository. Public code contains generic transport and
export hooks, not those implementations.
