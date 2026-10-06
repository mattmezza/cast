# Pro workflow configuration

The public core validates these 93 dormant settings in both editions. Only the
private Pro provider executes the workflows, after an eligible offline license.
Existing basic zoom, output/privacy controls and recordings remain free.

Defaults are in [pro-workflows.conf](../examples/pro-workflows.conf). Runtime edits
never rewrite the INI. A configured path does not load a model or start listening
at startup; downloads are explicit. All paths are literal, with no shell expansion.

## Cinematic zoom

```sh
cast-pro zoom cinematic
cast-pro zoom focus 0.65 0.35 --factor 2
cast-pro zoom follow on
cast-pro zoom auto click
cast-pro zoom status --json
cast-pro zoom motion legacy
```

Legacy is the default. In cinematic mode, `zoom.transition_ms` is the 99%-settling
interval of an analytic critically damped log-scale trajectory. Pan retains velocity
through retargets; legal viewport boundaries constrain state. Manual focus locks
until explicit follow/auto control resumes. Source replacement resets to 1×.

Automatic click zoom needs actual backend input events. Cursor smoothing needs
separately rendered cursor metadata; an embedded portal cursor is not doubled.
Camera avoidance can be constrained by source edges or camera geometry, with the
limitation reported in status. Temporal blur uses bounded transforms of the same
eligible frame, with no old-frame history. Requested/effective filter and sample
budget are reported. Default blur is off.

## Local captions and offline transcription

The inference-enabled Pro build uses `STT=1` and its separate structured helper.
Install a local Whisper GGML model explicitly; engine/VAD choices are shared with
notes. CPU is the baseline. An explicit unavailable GPU fails; `auto` can use the
available CPU backend. Energy VAD needs no extra model; Silero requires a compatible
local model. Translation means Whisper translation into English.

```sh
cast-pro settings transcription.model_path /absolute/path/ggml-base.bin
cast-pro audio mic on
cast-pro transcription on
cast-pro subtitles virtual on
cast-pro subtitles record on
cast-pro subtitles sidecar both
cast-pro transcription status --json
cast-pro record start /absolute/path/talk.mkv
cast-pro record cut
cast-pro record resume
cast-pro record stop
cast-pro transcription job status JOB_ID --json
cast-pro transcription transcribe /absolute/path/talk.mkv \
  --output /absolute/path/talk-copy --format both
```

Enabling transcription does not enable a microphone or output. Each lane needs its
own subtitle opt-in and permission epoch. Notes listening is independent and uses
only the already-selected microphone. Live caption delay is visible through latency
and backlog; stale work is dropped rather than delaying live video.

Automatic final sidecars require `transcription.enabled`, `auto_finalize` and a
non-`none` sidecar format. Finalization reads completed privacy-filtered recording
audio and leaves burned captions unchanged. `both` treats output as a basename and
writes `.srt` and `.vtt`; existing destinations require explicit `--overwrite`.
Standalone transcription works without a daemon and waits for completion without
opening capture devices. Daemon requests acknowledge job acceptance; job status
reports completion separately. Outputs are mode 0600 and contain no transcript logs.

The [Local speech guide](transcription.md) covers models, privacy and inference limits.
The [Speaker notes guide](notes.md) describes document parsing, timed navigation,
speech following, window controls and capture-exclusion limitations.

## Speaker notes

```sh
cast-pro notes open /absolute/path/talk.md
cast-pro notes speed 18
cast-pro notes start
cast-pro notes pause
cast-pro settings notes.font 'Noto Sans:style=Bold' notes.font_size 40
cast-pro notes mode speech
cast-pro notes start
cast-pro notes goto --line 42
cast-pro notes status --json
cast-pro notes close
```

Opening/loading/reloading/mode changes never start motion. Timed mode uses visual
lines per minute. Speech mode requires an enabled microphone, shared ready engine and sufficient
matching script text. Notes always transcribes the original speech, including when
the shared subtitle task is Translate. Markdown headings/emphasis
preserve source mapping; cue lines and fenced code are excluded from spoken matching
by default. Documents are bounded to 2 MiB and 200,000 spoken tokens. Unrelated speech,
silence and ambiguous repeated phrases hold progress; distinctive reacquisition
needs independent fresh evidence. General paraphrase understanding is not claimed.

Manual navigation retires old alignment and pauses. Center recenters without
resuming. Closing notes pauses/detaches only notes. Closing the panel leaves notes
and outputs alone. Global privacy suspends notes; broadcast resume does not restart
notes listening. Xorg masking hides covered pixels rather than reconstructing them;
Wayland portal monitor masking is not guaranteed. Requested/effective exclusion
and window-manager hints are separate diagnostics.

## Editing and resources

Independent values commit on selection/release/field completion; coupled model/VAD,
window/step/backlog and matching-threshold batches use atomic Apply/Revert. Failed
resource preparation leaves acknowledged state intact. Appearance/speed changes
preserve logical notes position. Initial notes dimensions are creation defaults;
resizing does not persist them. Inference engine changes retire consumer generations.
Notes mode changes pause and require explicit start.

No complete complex-script shaping/tokenization or arbitrary paraphrase matching is
claimed. Panel fonts remain bundled Inter; output captions/notes use the selected
Fontconfig pattern. Native desktop masking and GPU checks need their actual backend;
a headless/dummy test does not establish them.


## Speech setting editability and restart effects

All 42 speech/subtitle fields are runtime-editable in an eligible Pro build; none
requires a daemon restart. The matrix distinguishes panel interaction from resource
replacement. Immediate means selection, slider release or field completion sends a
setting; acknowledgement still waits for validation/preparation. Coupled drafts use
the pinned Apply/Revert bar. CLI `settings` can send either kind in one atomic batch.
These changes affect the session; neither path rewrites the INI.

| Fields | Panel interaction | Runtime effect |
|---|---|---|
| `transcription.enabled` | Immediate | Enables lane transcription only. Off retires lane work and unaccepted automatic sidecars; independent Notes and already accepted bounded jobs remain separate. |
| `transcription.backend` | Immediate | Only whisper is supported; no alternative backend is silently selected. |
| `transcription.model_path`, `transcription.device`, `transcription.vad_backend`, `transcription.vad_model_path`, `transcription.window_ms`, `transcription.step_ms`, `transcription.max_backlog_ms` | Atomic draft / Apply | Prepares a replacement shared engine when entitled, then retires all consumer generations. Failure preserves the acknowledged engine. Window/step/backlog constraints validate together. |
| `transcription.language`, `transcription.task`, `transcription.source`, `transcription.threads`, `transcription.vad_threshold`, `transcription.min_speech_ms`, `transcription.min_silence_ms`, `transcription.show_partial` | Immediate | Also changes shared engine settings: replacement preparation and consumer-generation retirement apply. Notes remains microphone-only and always transcribes original speech. |
| `transcription.auto_finalize` | Immediate | Recording-start snapshot controls automatic final sidecars for that recording; it does not rewrite existing subtitles or modify an accepted job. |
| `subtitles.virtual`, `subtitles.record`, `subtitles.stream` | Immediate | Independent lane opt-ins. Off purges that lane; On does not enable audio or start an output. |
| `subtitles.sidecar` | Immediate | Sets format for future recording snapshots. None additionally cancels unaccepted automatic sidecar work; accepted jobs keep their bounded grants. |
| `subtitles.font` | Immediate | Prepares and validates a replacement caption font before acknowledgement; failure preserves working state. |
| `subtitles.size`, `subtitles.color`, `subtitles.opacity`, `subtitles.outline_width`, `subtitles.outline_color`, `subtitles.background`, `subtitles.background_color`, `subtitles.background_opacity`, `subtitles.background_radius`, `subtitles.padding_x`, `subtitles.padding_y`, `subtitles.anchor`, `subtitles.align`, `subtitles.margin_x`, `subtitles.margin_y`, `subtitles.max_width_percent`, `subtitles.max_lines`, `subtitles.line_spacing`, `subtitles.hold_ms` | Immediate | Updates live caption appearance/placement/retention. Existing video and completed sidecars are unchanged. No inference-engine replacement is required for these fields. |

A shared-engine replacement retires virtual-camera, recording, streaming and Notes
recognition generations, discarding stale PCM/results. It does not start stopped
outputs, enable a microphone or resume paused Notes. The recording's final-sidecar
settings snapshot is independent of later live model changes. Explicit master Off
or sidecar None cancels unaccepted automatic finalization; an already accepted job
can only finish within its existing five-minute wall/monotonic grant or be cancelled.

## VAD threshold meaning

`transcription.vad_threshold` is normalized to 0–1. Energy detection uses normalized
floating-point PCM RMS with cutoff `threshold × 0.05`; default 0.5 therefore means
RMS 0.025 (about −32 dBFS). This is an energy gate, not a probability. Speech and
silence durations use the configured millisecond values.

With Silero selected, the same threshold is additionally passed directly as its
0–1 speech-probability threshold. The live service's energy gate and the helper's
whole-window energy check still precede Silero; selecting Silero does not bypass
them. Silero requires a readable whisper.cpp-compatible GGML VAD model; ONNX files
are rejected. Actual same-model Silero recognition has not been accepted on this
host. Privacy-filtered final recording jobs also split neutral/silent spans before
inference independently of the user's VAD threshold.

## Complete added field inventory

### `zoom`

| Key | Default | Accepted values |
|---|---|---|
| `motion` | `legacy` | legacy, cinematic |
| `follow_region_percent` | `35` | 5–90 |
| `follow_hysteresis_percent` | `5` | 0–20 |
| `pan_settle_ms` | `400` | 0–3000 |
| `focus_margin_percent` | `12` | 0–40 |
| `avoid_camera` | `true` | true, false |
| `auto` | `off` | off, click |
| `auto_factor` | `2` | 1–20 |
| `auto_hold_ms` | `1600` | 0–10000 |
| `auto_cooldown_ms` | `700` | 0–10000 |
| `auto_idle_ms` | `2500` | 100–30000 |
| `auto_cluster_radius_percent` | `12` | 0–50 |
| `manual_override_ms` | `5000` | 0–60000 |
| `filter` | `bilinear` | bilinear, bicubic |
| `motion_blur` | `false` | true, false |
| `shutter_angle` | `120` | 0–180 |
| `blur_samples` | `4` | 2–8 |

### `cursor`

| Key | Default | Accepted values |
|---|---|---|
| `smooth` | `false` | true, false |
| `settle_ms` | `60` | 0–200 |
| `max_lag_ms` | `80` | 0–250 |
| `hide_idle_ms` | `0` | 0–30000 |

### `transcription`

| Key | Default | Accepted values |
|---|---|---|
| `enabled` | `false` | true, false |
| `backend` | `whisper` | whisper |
| `model_path` | empty | Literal text/path; at most 1023 bytes |
| `device` | `cpu` | cpu, auto, gpu |
| `language` | `auto` | Literal text/path; at most 1023 bytes |
| `task` | `transcribe` | transcribe, translate |
| `source` | `mic` | mic, desktop, mix |
| `threads` | `4` | 1–64 |
| `vad_backend` | `energy` | energy, silero |
| `vad_model_path` | empty | Literal text/path; at most 1023 bytes |
| `vad_threshold` | `0.5` | 0–1 |
| `min_speech_ms` | `250` | 50–5000 |
| `min_silence_ms` | `500` | 50–5000 |
| `window_ms` | `6000` | 1000–30000 |
| `step_ms` | `500` | 100–5000 |
| `max_backlog_ms` | `12000` | 1000–60000 |
| `show_partial` | `true` | true, false |
| `auto_finalize` | `true` | true, false |

### `subtitles`

| Key | Default | Accepted values |
|---|---|---|
| `virtual` | `false` | true, false |
| `record` | `false` | true, false |
| `stream` | `false` | true, false |
| `sidecar` | `none` | none, srt, vtt, both |
| `font` | `Noto Sans` | Literal text/path; at most 1023 bytes |
| `size` | `36` | 8–256 |
| `color` | `#ffffff` | #RRGGBB |
| `opacity` | `1` | 0–1 |
| `outline_width` | `2` | 0–16 |
| `outline_color` | `#000000` | #RRGGBB |
| `background` | `true` | true, false |
| `background_color` | `#101113` | #RRGGBB |
| `background_opacity` | `0.75` | 0–1 |
| `background_radius` | `8` | 0–256 |
| `padding_x` | `16` | 0–512 |
| `padding_y` | `10` | 0–512 |
| `anchor` | `bottom` | top, bottom |
| `align` | `center` | left, center, right |
| `margin_x` | `48` | 0–7680 |
| `margin_y` | `48` | 0–4320 |
| `max_width_percent` | `85` | 10–100 |
| `max_lines` | `2` | 1–6 |
| `line_spacing` | `1.15` | 0.8–2 |
| `hold_ms` | `2000` | 0–10000 |

### `notes`

| Key | Default | Accepted values |
|---|---|---|
| `file` | empty | Literal text/path; at most 1023 bytes |
| `format` | `auto` | auto, plain, markdown |
| `font` | `Noto Sans` | Literal text/path; at most 1023 bytes |
| `font_size` | `36` | 12–128 |
| `foreground` | `#f8f9fb` | #RRGGBB |
| `background` | `#101113` | #RRGGBB |
| `highlight_color` | `#80c9ff` | #RRGGBB |
| `read_opacity` | `0.7` | 0.3–1 |
| `line_spacing` | `1.45` | 1–2.5 |
| `padding` | `32` | 0–128 |
| `column_chars` | `42` | 20–100 |
| `align` | `left` | left, center |
| `window_width` | `760` | 320–3840 |
| `window_height` | `560` | 240–2160 |
| `always_on_top` | `false` | true, false |
| `reading_line_percent` | `35` | 10–80 |
| `mode` | `timed` | timed, speech |
| `speed_lines_per_minute` | `12` | 1–120 |
| `scroll_smooth_ms` | `250` | 0–2000 |
| `speech_max_lines_per_second` | `6` | 0.5–30 |
| `follow_threshold` | `0.75` | 0–1 |
| `reacquire_threshold` | `0.85` | 0–1 |
| `ambiguity_margin` | `0.12` | 0–1 |
| `reacquire_min_words` | `5` | 2–30 |
| `follow_window_words` | `120` | 20–2000 |
| `off_script_grace_ms` | `1200` | 0–5000 |
| `silence_hold_ms` | `800` | 100–5000 |
| `allow_backward_reacquire` | `false` | true, false |
| `match_code_blocks` | `false` | true, false |
| `exclude_from_capture` | `true` | true, false |
