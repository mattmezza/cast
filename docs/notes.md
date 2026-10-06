# Speaker notes

Speaker notes are a separately licensed local presentation aid. Community lists
`[notes]` for configuration compatibility and reports `requires_pro`; its build
contains no parser, follower, speech worker or native notes renderer. Pro needs
an authoritative entitlement for `speech_teleprompter` before loading a script,
opening its window, starting motion/listening or preparing notes resources.
Cached licence labels in the UI do not grant that access.

Nothing opens a script, microphone or speech model at startup. Loading opens a
literal local regular file with a valid UTF-8 path, snapshots and parses it, then
leaves notes paused.
Use absolute paths: relative paths resolve in the daemon working directory.
Opening the native window leaves recording, streaming, virtual-camera output and
speech consent unchanged. Closing the main panel leaves notes open; closing notes
pauses only notes. Daemon loss closes the client and retires its local consumer.
Licence loss preserves the existing readable script and pauses motion/listening;
loading, resource changes, opening and starting require a current entitlement.

## Use

```sh
cast notes open /absolute/path/fictional-notes.md
cast notes mode timed
cast notes speed 18
cast notes start
cast notes pause
cast notes goto --line 14
cast notes center
cast notes reload
cast notes status --json
cast notes close
```

For an explicit speech-follow session after the timed example:

```sh
cast settings transcription.model_path /absolute/path/ggml-base.bin transcription.language en
cast audio list
cast audio mic source YOUR_EXPLICIT_MICROPHONE_NODE
cast audio mic on
cast notes open /absolute/path/fictional-notes.md
cast settings notes.font_size 40 notes.line_spacing 1.5
cast notes mode speech
cast notes start
cast notes status --json
# Read a passage, take a tangent not present in the notes, then read a distinctive later passage.
# If repeated passages remain ambiguous, choose a passage in the native window and press Play.
cast notes pause
cast notes close
```

Use your installed compatible local model and an explicitly selected microphone
node. Microphone enable and Notes Start are explicit opt-ins; these commands do
not download a model or start an output.

The main panel's Operate view has an Open/Show notes entry and Options, separate
from its four output cards, with an independent Close action. Notes Options has
Start/Pause/Close, file/format/typeface/mode/speed essentials and an All notes settings
disclosure for colors, layout and bounded matching controls. File/font browse stages
an edit; Apply commits a validated batch and Revert discards it. Choosing a font
file uses that actual readable font; a Fontconfig pattern can also be typed.
Model/language/source/VAD controls are the shared Audio → Speech recognition
settings, reachable through Audio options. Caption enablement is unnecessary.
Audio options show model/language essentials first; All speech settings reveals
the remaining bounded engine controls. The selected speech source applies to
captions; notes always use the permitted microphone.

The panel shows the actual loaded document path separately from `notes.file`,
the configured default used by Open notes. `notes load FILE`, `notes open FILE`
and the native Load button replace the current readable snapshot without
rewriting that default. Applying a changed `notes.file` validates and loads that
default as one settings transaction. Reload rereads the actual loaded path.

The native window has Play/Pause, Timed/Speech, speed −/+, Prev/Next paragraph,
Track (center on the committed cursor), Restart, Reload, Load and Close. Tab and
Shift-Tab move visible keyboard focus; Enter activates it. Space toggles Play,
arrows/Page Up/Page Down/wheel and dragging the text body inspect without moving the committed follower,
Home restarts, End goes to the final source line, Escape closes notes unless a
file dialog is open. Inspection pauses notes. Track returns to its saved cursor;
Play explicitly resumes. Local shortcuts act only in the focused owned notes
window and are suspended while its file dialog is open; there is no global
keyboard grab.
Dragging needs at least three logical pixels of movement; a body click alone does
not pause. Releasing the drag leaves inspection paused until explicit Play.
When repeated passages are ambiguous, up to four passage buttons show source line
and, in wider windows, alignment score. Click one, or Tab to it and press Enter, to choose a new paused
matching anchor. Selection checks the document generation before committing, so
an older button cannot select a line from a replaced script. These scores are
alignment similarities, not recognition probabilities.

Timed mode uses monotonic elapsed time and fractional **wrapped visual lines**
per minute. Pause holds exactly. Speed edits preserve the visible position.
Reflow/font/column edits preserve the logical source line and fractional position,
including cue, blank and excluded code lines. `goto --line` uses original 1-based
source lines, not wrapped rows. EOF stops. Font size and dimensions use logical
pixels, while glyphs rasterize at current display density. Initial window size is
used on creation; subsequent user resize is session state and never rewrites INI.

For speech, select a compatible local model on the shared transcription surface,
explicitly enable the permitted microphone, select Speech, and press Play/start.
Known missing speech support or an unselected local model fails before preparation
is acknowledged. The panel shows those prerequisites and keeps Timed mode usable.
A selected model still needs to pass actual helper preparation after explicit Start.
No desktop-audio fallback or second microphone exists. The NOTES consumer owns
its consent, epochs and result queue; a lane pause does not revoke it. Global
privacy, notes pause/close, mic/source changes, model changes, document changes,
manual anchors and daemon loss retire pending work. Resuming outputs does **not**
resume notes listening. A cancelled slow model preparation cannot start following
when its request returns, and it cannot stall timed motion chosen afterwards.

## Xorg exclusion and closing

The Xorg monitor/region capture path synchronizes Notes geometry and pixel readback
with a short server grab only while excluded Notes or a retired Notes frame needs
protection. The grab ends before pixel conversion, composition or encoding. This
prevents other X clients from moving or reparenting the client/WM frame between
its bounds query and the server readback. It does not certify compositor-retained
animation images or effects outside the tracked server bounds. It still paints neutral pixels; it does not recover
background hidden by the Notes window. Explicit exclusion opt-out disables this
Notes protection, and opting back in retires previously exposed held/source pixels
while preserving output mode flags. Opening or closing an already excluded window
preserves existing safe Freeze images.

On orderly close, the native client refreshes its existing authenticated geometry,
hides its window, synchronizes the X11 connection, destroys the renderer/window,
and synchronizes again before unregistering. Xorg retains one bounded cached outer
footprint plus its first-frame previous bounds until the WM's outer window is
actually hidden or gone. A buggy WM that leaves its frame visible can leave that
region masked, and a new Notes registration is deferred while it remains visible.
There is no timeout that unmasks still-visible remnants. Actual authenticated Xorg
movement/close/Freeze acceptance is prepared in the host harness but remains unrun
in the restricted session; normal SDL dummy tests do not prove those guarantees.

## Parsing and following

Plain UTF-8 and a small Markdown subset are supported: headings, list prefixes,
paired emphasis/backtick delimiters and fenced code. Inline `*italic*`/`_italic_`,
`**bold**`/`__bold__` and triple-marker bold italic are visibly rendered; nested
bold/italic retain both styles. Their actual glyph widths determine wrapping,
while original source/token spans remain stable. Unmatched delimiters and
underscores inside words stay literal. Backtick spans display their literal
contents without applying emphasis inside them. Whole bracketed cue lines
stay visible without matching. Fenced code stays visible and matches only with
`notes.match_code_blocks=true`. Unknown HTML/link syntax is literal text; nothing
executes, fetches a URL or expands a command. The original bytes, source spans,
line/paragraph identities and normalized spoken-token IDs remain separate from
visual wrapping. Reload preserves an unchanged snapshot or a unique surrounding
spoken phrase; otherwise it explicitly returns paused at the beginning. Failed
file/parser/font preparation retains the old document/configuration.

Bounds: 2 MiB UTF-8 regular-file snapshot, 100,000 source lines, 200,000 spoken
tokens and 95 UTF-8 bytes per normalized word. Over-limit or malformed text,
embedded NUL/control bytes and files changed during the snapshot are rejected.
There is no unbounded transcript/history, script logging or automatic export.
Native IPC transfers an existing snapshot in generation-checked chunks of at most
3,500 bytes; geometry is separately bounded and never grants listening consent.

Normalization covers whitespace/punctuation, ASCII/basic Latin-1 case, retained
accented UTF-8 words, common English contractions and English integer words up to
999. It does not guess dates, decimals, currencies, arbitrary localized numbers,
Unicode case folding or CJK word segmentation. SDL_ttf displays UTF-8 supported by
the installed fonts; full bidi/complex-script shaping is not claimed.
Noto Sans is resolved using Fontconfig and opened as an actual readable
FreeType/SDL_ttf file; it is separate from the main panel's bundled Inter font. The shared
path-only font renderer opens collection face zero; Fontconfig matches requiring
a different collection face are rejected instead of silently selecting another
face. Choose a standalone font file for those cases.
The native Notes renderer prepares up to eight useful installed Fontconfig
fallback faces for actual missing document glyphs and registers them with
SDL_ttf. Fallbacks use the same size/style as the selected primary, and row height
reserves the tallest retained face. All faces are created/used/closed on the SDL
thread; drawing opens no fonts and makes no network requests. Missing coverage
after the bounded selection shows an explicit glyph-count warning; installing a
font does not imply every script has supported shaping. The headless shared
path-only font contract still selects a single face for its separate consumers.

Weighted semi-global edit alignment accepts skips, repeats and insertions. Common
words have less weight. Local alignment is bounded by `follow_window_words`; wider
reacquisition scans a bounded document and aligns at most 48 nearby candidates.
Partial results tint a tentative line lightly without committing a cursor. Final
results commit only after thresholds/ambiguity checks. Distant reacquisition needs
at least the configured phrase length and two independent non-overlapping audio
confirmations; opt-in backward reacquisition requires three and a stronger score.
Recent forward matches estimate local visual-line pace from their observed token
positions and audio endpoints. Between reliable updates, that estimate smooths only
the visible offset, capped at half a wrapped line and 500 ms of lookahead; it never
predicts a committed token. Weak partial/final speech and silence clear that estimate
and hold the view. A distant independently confirmed passage more than eight wrapped
lines away holds briefly, then recenters after 100 ms instead of scrolling through
hundreds of intervening lines. Pause/manual/document barriers cancel that reposition.
Repeated ambiguous phrases hold position and expose up to four distinct passage
lines for explicit navigation. Silence, tangent/weak speech and stale
results never drive predictive timed advance. Scores are algorithm similarities,
not calibrated speech-recognition probabilities.

## Configuration

All fields are session-editable. Appearance, speed and matching bounds do not
restart outputs. File/format/code changes prepare a new document and pause;
mode changes pause. Shared model changes retire consumers and require explicit
start again. A window must be closed/opened to use new initial dimensions.

| Key (`notes.` prefix) | Default | Bound / choices |
| --- | --- | --- |
| file | empty | literal local path, at most 1,023 bytes |
| format | auto | auto, plain, markdown (`auto` uses `.md`) |
| font | Noto Sans | Fontconfig pattern or font-file path |
| font_size | 36 | 12–128 logical pixels |
| foreground / background / highlight_color | #f8f9fb / #101113 / #80c9ff | #RRGGBB |
| read_opacity | 0.7 | 0.3–1; already-read text, body remains opaque |
| line_spacing | 1.45 | 1–2.5 |
| padding | 32 | 0–128 logical pixels |
| column_chars | 42 | 20–100 approximate M widths |
| align | left | left, center |
| window_width / window_height | 760 / 560 | 320–3840 / 240–2160 logical pixels |
| always_on_top | false | requested compositor/window-manager hint |
| reading_line_percent | 35 | 10–80 of the text-body height |
| mode | timed | timed, speech |
| speed_lines_per_minute | 12 | 1–120 wrapped lines/min |
| scroll_smooth_ms | 250 | 0–2000 |
| speech_max_lines_per_second | 6 | 0.5–30 |
| follow_threshold / reacquire_threshold | 0.75 / 0.85 | 0–1, reacquire ≥ follow |
| ambiguity_margin | 0.12 | 0–1 |
| reacquire_min_words | 5 | 2–30 |
| follow_window_words | 120 | 20–2000 |
| off_script_grace_ms | 1200 | 0–5000 |
| silence_hold_ms | 800 | 100–5000 |
| allow_backward_reacquire | false | stronger score + three confirmations |
| match_code_blocks | false | include fenced code in spoken matching |
| exclude_from_capture | true | requested Cast capture masking |

## Visibility and architecture

Xorg uses a separately tracked, authenticated `org.cast.Pro.Notes` utility-window
identity. Registration happens before showing it; public integration validates
kernel peer PID/current helper identity and masks its outer bounds independently
from the main panel. Covered pixels are hidden; underlying desktop content cannot
be reconstructed. Float utility windows in tiling WMs, or configure this exact
application class in the WM. An always-on-top hint is a request whose enforcement
belongs to the WM, not a guarantee of placement or privacy. Status exposes the
requested hint and explicitly reports support as `window_manager_hint_unverified`;
a backend that rejects the SDL hint shows an error. We do not equate a requested
SDL flag with verified compositor placement.

On Wayland general monitor exclusion is not guaranteed by the ScreenCast portal.
Requested and effective exclusion are separate status fields. The window/panel
shows a warning while requested exclusion is ineffective. Use an unshared display,
selected application/region, or hide notes. A separate window does not imply privacy.
The client reports backend failures instead of silently claiming a private window.

The capture tick only publishes atomics and obtains a cached snapshot with
`trylock`; it never waits on notes alignment, fonts, document parsing or model
preparation. Main-thread configuration commits publish bounded settings/doc
ownership through a small mailbox; engine/command workers apply them. Pause/close
have a bounded main-thread stop hook: atomic barrier and NOTES retirement happen
before ACK, with immediate cached-close state. An atomic window lifecycle generation/CAS
prevents an older file/font Open preparation from reversing a later Close ACK. One auxiliary lazy-model worker
rechecks request, document, anchor, mic, permission and entitlement before following.
SDL rendering has its own bounded transport thread and texture cache for visible
rows; scripts are reflowed only on document/font/width/layout changes.

## Validation and remaining platform checks

`python3 pro/tests/run_notes.py --core /path/to/community --native --sanitize`
checks strict parsing and source spans, contractions/numbers/accents, exact monotonic
timing at 24/30/60/120 Hz, pause/speed/EOF, cue-line reflow anchors, weighted partial
and final alignment, overlapping/duplicate results, tangents, silence, ambiguity,
manual/reload epoch barriers, independent consumer retirement, licence/resource
failure and cancelled slow readiness with continuing timed motion. It exercises
actual installed SDL3/TTF/Noto through the software dummy backend, runtime transport
fixtures, keyboard focus/rendering, font/spacing reflow and independent close.
The native input fixture verifies owned body drag pauses without committing
tokens, foreign-window input cannot pause/start, and candidate-button keyboard
selection retires earlier recognition. The engine limits ambiguous choices to
four distinct passage lines; stale-generation selections leave the current
anchor unchanged. A separate actual SDL_ttf raster test compares bold/italic
pixels with direct styled font rendering, verifies mixed-style wrapping and
retains UTF-8/source spans across reflow. Font tests verify missing-glyph raster
against an actual fallback face, style propagation, non-Latin-primary control
coverage, failed-candidate retention and repeated owner cleanup.
`--width 320 --capture /tmp/fictional.bmp` exercises compact layout; screenshots
exist only in test builds and contain the fictional fixture. ASan/UBSan are enabled;
external Fontconfig/SDL process caches are excluded from leak checking.

This environment cannot bind an Xvfb X socket. Dummy screenshots therefore do not
prove real Xorg/Wayland identity, decoration masking, compositor placement, hardware
HiDPI or live-microphone accuracy. Those remain explicit platform acceptance checks.
The private opt-in host test runs a real same-build daemon and native client on
its own Xvfb display, without a microphone or model:

```sh
python3 /tmp/cast-pro/pro/tests/test_notes_host.py \
  --binary /home/matteo/dev/cast/cast-pro \
  --license /tmp/cast-pro/pro/tests/generated/perpetual.json \
  --artifacts /tmp/cast-notes-host-acceptance
```

The binary must be built with X11, panel and controlled LGPL media support and
the matching nonproduction test trust header; the signed fixture must grant
`speech_teleprompter`. The artifact directory must not already exist. Host tools
are Xvfb, xdotool, xprop, ffmpeg and ffprobe. The test checks actual executable,
window class/PID/utility hints, effective authenticated exclusion, matching-build
registration rejection from a different kernel peer PID, and decoded full-size
FFV1 recordings whose masked pixels match `#13579b` within three RGB levels.
It moves continuously while recording and rejects bright script glyphs escaping
the mask, reparents Notes under a synthetic decoration ancestor, and verifies old
footprints clear. It checks every decoded closing frame, keeps a still-visible
retired ancestor masked until it is hidden/destroyed, and verifies main-panel/Notes
close independence while recording continues. Additional held-frame checks verify
that ordinary Open/Close preserves Freeze and explicit exclusion opt-in retires an
exposed held script without changing the Freeze state. Missing tools, failed admission, missing
frames, ineffective masking and failed identity checks fail closed. Only Python
syntax/argument checks ran here; this Xvfb host acceptance remains **unrun** in
the listener-restricted sandbox. It does not claim real-window-manager, Wayland
or hardware-display acceptance.

The ordinary suite uses fake recognition and no microphone/model/network; optional
real-model accuracy must use recorded fictional audio and ground-truth positions,
without pretending a smooth scroll alone proves speech following quality.

An opt-in real-model check is `test_notes_real.c`, linked with the same private
speech backend and notes document/engine sources, `CAST_WITH_STT`, libsodium,
pthread and libm. Arguments are a local compatible model, independently verified
NOTES-entitled signed licence, same-build helper and 16 kHz mono float32 recording.
On the installed CPU helper/base model and upstream recorded JFK fixture, one
ground-truth phrase passed with **0-word cursor error**, score **1.000**, inference
**2,403 ms**, alignment **38 µs**; silence, duplicate result and manual-anchor epoch
holds passed. This fixture is public recorded speech rather than the fictional
ordinary-suite script, and the result does not establish live mic accuracy,
continuous speaker pacing, multiple accents or long-talk reacquisition rates.
No recognized speech is printed in its diagnostic log. The local WAV fixture SHA256
is `59dfb9a4acb36fe2a2affc14bacbee2920ff435cb13cc314a08c13f66ba7860e`;
its upstream v1.9.4 attribution is engineering context, not a network-verified
release provenance claim.

An earlier version of `test_notes_reacquire_real.c` replayed four nonoverlapping intervals
from that recording: `[0,3]`, `[3,3.75]`, `[3.75,7]` and `[7,11]` seconds. The script
contains an opening, fictional intervening paragraphs and the two resumed clauses;
the short middle clause is absent. With the current helper/base model and its
matching signed caption-and-Notes test fixture, the opening committed with zero
word error, the omitted middle crop held the cursor with **zero false jumps**,
the first resumed crop held after one confirmation (**score 0.864**, runner 0.558),
and the second independently recorded crop committed with **zero word error**
(**score 1.000**, runner 0.648). Normal thresholds and the two-confirmation rule
remained unchanged. Final inference was **2,262 ms**, alignment **61 µs**, and the
two-request replay wall time **4,905 ms**; the independent audio evidence spanned
**7,250 ms**. The middle crop recognized only one normalized word, so this proves
holding through a very short omitted clause, not sustained free-form tangents.
An earlier resumed crop scored 0.838 and correctly held below the default 0.85
reacquisition threshold. An exploratory 350 ms crop was rejected by the helper/backend; its exact
error was not retained in that run. The final test uses longer grounded crops.

These are opt-in recorded crop checks without microphone or recognized-text logs.
They do not establish long-talk, multiple-accent, continuous speaker-pacing or
live-mic quality. The current full-feature test fixture initially failed the older
helper's signed-license verification; the successful runs used its independently
matching signed Notes-entitled fixture, without bypassing validation.
`test_notes_real.c` also accepts an optional `translate` argument to exercise the
NOTES transcribe-only override with the shared task configured to translate; that
path passed against the refreshed helper and its matching full-feature signed test
fixture: **0-word cursor error**, score **1.000**, inference **2,954 ms**, alignment
**68 µs**, plus silence/duplicate/manual-epoch holds. The shared task was Translate,
while NOTES remained transcribe-only. This English recording verifies initialization
and request routing; it does not establish foreign-language transcription accuracy.

The current `test_notes_reacquire_real.c` strengthens the tangent check using a
constructed replay of the same recorded fixture: original `[0,3]` seconds is the
scripted opening, `[3,7.25]` is a complete omitted multiword clause, `[7.25,11]`
resumes, then the recorded opening is replayed as a later scripted reprise at
replay timestamps `[11,14]`. The opening committed with zero word error. The
omitted clause produced **nine real normalized words**, held the cursor, and made
**zero false jumps**. The resumed crop produced nine words, score **0.908**, and
held after one confirmation. The recorded reprise produced five words, score
**1.000**, and committed with zero word error after the second fresh nonoverlapping
replay interval. Defaults, including the 0.85 reacquisition threshold and two
independent-audio-interval confirmation guard, were unchanged. Last inference was
**2,177 ms**, alignment **35 µs**, and two-request replay wall time **4,491 ms**;
the resumed replay evidence spanned **6,750 ms**. A `[7,11]` boundary scored 0.834
and correctly held below threshold. The fourth crop reuses recorded opening audio;
this validates a meaningful tangent plus resumed/reprise routing, not four
independent recorded utterances, sustained free-form speech or long-talk accuracy.
