# Local speech and subtitles

Cast Pro uses one local speech service for live captions and speech-following Notes.
Each virtual-camera, recording, streaming and Notes consumer has independent consent,
permission epochs and results. Sharing a model does not share recognition history.
Only Notes uses the selected microphone regardless of the subtitle source setting.
Timed/manual Notes needs no speech model; speech mode needs explicit microphone
permission and an eligible speech-teleprompter entitlement.

The CPU helper is built with `STT=1`. `STT=0` reports speech unavailable while
cinematic motion and manual/timed Notes remain usable. `device=cpu` is the baseline;
`auto` currently uses that CPU backend. Explicit `gpu` fails rather than silently
promising acceleration. The existing selected audio routes are reused: enabling
transcription never opens a new source or enables microphone/desktop audio.

## Models and setup

Models are installed explicitly as local Whisper GGML files. The panel has a literal
path field and local file browser; browsing never downloads, enables audio or starts
outputs. `transcription models` lists bounded local candidates in the configured
model's parent or `/usr/share/cast-pro/models`; it does not scan the filesystem or
load them. The private `tools/speech-assets.py` is an explicit pinned-asset download
tool. Ordinary builds, startup, status and doctor never download a model.

Example session commands after starting an eligible Pro daemon:

```sh
cast-pro settings transcription.model_path /absolute/path/ggml-base.bin
cast-pro audio mic on
cast-pro transcription on
cast-pro subtitles virtual on
cast-pro subtitles record on
cast-pro subtitles sidecar both
cast-pro transcription status --json
```

Source can be microphone, desktop or an explicit mix for subtitle lanes. Translation
uses Whisper's English translation task; Notes always follows original transcribed
speech even when that shared task is Translate. Model/language changes can interrupt
recognition while the prepared replacement is acknowledged. Fonts use Fontconfig
patterns; panel typography stays bundled Inter. Caption and Notes appearance are
independent settings with common font-resolution infrastructure.

## Privacy and files

Solid pause, freeze and blur silence the affected lane and retire its caption work.
Cut omits recording media time, with independent subtitle timestamps on the retained
recording timeline. Explicit transcription Off immediately retires lane caption
work and cancels unaccepted automatic finalization; independent Notes listening is
separately controlled. Global privacy pauses Notes; global Resume does not restart
Notes. Already accepted offline jobs retain only their bounded grant.

Final recording transcription reads completed privacy-filtered recording audio; it
does not recognize a second unfiltered microphone recording. Recording-start model,
source/VAD and sidecar-format snapshots remain stable. Automatic finalization needs
master transcription, auto-finalize and a non-none sidecar format at recording start.
Burned captions are retained; final sidecars do not rewrite video. SRT/VTT creation
is atomic with mode 0600. Existing destinations need explicit overwrite.

```sh
cast-pro transcription transcribe /absolute/path/talk.mkv \
  --output /absolute/path/talk-final --format both
cast-pro transcription job status JOB_ID --json
cast-pro transcription job cancel JOB_ID
```

The daemon acknowledges job acceptance, with completion reported separately. A
standalone invocation waits for its local job without starting capture. `both` uses
a basename and writes `.srt` plus `.vtt`; single-format output is the chosen path.
Ordinary status/logging contains no transcript. Inference backlog and dropped work
are visible; stale live work is dropped instead of holding video frames.

## Acceptance limits

The current engineering build uses an explicitly identified installed CPU SDK. The
controlled offline recipe pins whisper.cpp 1.9.4 plus its bundled ggml; its source
cache is unavailable here, so that source build and production qualification remain
unrun. Default production trust is empty; generated fixture keys/licenses are test
inputs, excluded from source delivery. No production Pro package is claimed.

Actual recorded CPU checks recognize the public JFK fixture, return no segments for
silence, and complete decoded-audio SRT/VTT jobs. Speech-specific wall/CPU/peak-memory
measurements and model/hardware scope are recorded in the companion public
`docs/split-verification.md`. These bounded English fixtures do not establish live
microphone, multiple-accent, translation-language or long-talk accuracy. Actual GPU,
Silero and authenticated Xorg/Wayland desktop acceptance remain separate checks.
The local CPU profile is not a fully static, self-contained binary distribution.
