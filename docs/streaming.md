# Streaming

Cast has three independent outputs: **Virtual camera** feeds local conferencing
software, **Recording** writes a file, and **Streaming** sends H.264/AAC to one
RTMP or RTMPS destination. They share the composition and selected PipeWire audio
mix. Recording and streaming encode separately, so running both needs more CPU.
Streaming works without a virtual camera device, panel, or preview.

## Set up the stream-key file

A stream key is the service-provided password allowing broadcasts to your channel.
Keep the actual value out of commands, the config, server URLs, and screenshots.
Create an empty file, then paste the value using your editor:

```sh
install -d -m 700 "$HOME/.config/cast"
install -m 600 /dev/null "$HOME/.config/cast/stream.key"
${EDITOR:-vi} "$HOME/.config/cast/stream.key"
```

Use one nonempty line, optionally followed by one newline. Cast requires an
absolute path, a regular file owned by your user, and no group/other permissions.
Symlinks are rejected. If necessary correct permissions with
`chmod 600 "$HOME/.config/cast/stream.key"`. Permissions protect local access;
they do not encrypt the file. Cast reads the file locally when you explicitly
start streaming. Its secret is sent only to the configured ingest service as
required for authentication; it stays out of CLI/IPC arguments and UI/status/logs.

## Configure and start safely

Add `[stream]` to your Cast config; the complete fields and defaults are in
[configuration.md](configuration.md). Supply a server address **without its stream
key** and the absolute stream-key file path:

```ini
[stream]
service = custom
server_url = rtmps://your-ingest.example/app
key_file = /home/you/.config/cast/stream.key
video_bitrate_kbps = 2500
audio_bitrate_kbps = 128
encoder_preset = veryfast
```

Shell expressions and `~` are not expanded in INI values. Start the daemon with
`cast --no-virtual` if you do not need conferencing output. Then:

```sh
cast doctor                 # read-only local checks; no endpoint is contacted
cast stream start           # accepts asynchronous startup; sends a solid pause screen
cast stream status --json   # inspect connecting/streaming/failed state
cast preview target stream  # optional floating preview
cast stream resume          # explicitly reveal the composition
cast stream pause           # solid pause screen and silence; connection continues
cast stream freeze          # held composition and silence
cast stream blur on         # blur above freeze; optional shared styled text
cast stream unfreeze        # moving blurred composition, still silent
cast stream resume          # clears solid pause only; does not clear blur or freeze
cast stream unblur          # clear blur separately
cast stream stop            # stop transmission and cancel outstanding retries
```

Start never implies publicly visible service acceptance. Connection state and
presentation state are separate: a connected session can still be privacy-paused.
Stop cannot retract data already accepted by the encoder/network/service, and
service buffering may delay the end of a public broadcast. Pause keeps continuous
media time. Streaming has no recording-style cut control.

`cast pause` includes an existing stream; `cast resume` restores only solid-pause
flags changed by that group action. It cannot start a stopped output or reveal an
independently paused one. Startup, reload, presets and opening/closing the panel
never start a broadcast.

Service presets fill session quality fields without changing the destination, key,
composition resolution or fps. Twitch selects 4500/128 kbps at up to 30 fps or
6000/160 above; YouTube selects 14000 kbps at 1080p30, 17000 above 30 fps or
8000 below 1080p, with 128 kbps audio. All use veryfast. Explicit quality values
after the service in a settings batch override it; explicit INI quality values
win regardless of file order. Custom keeps the current quality fields.

Runtime configuration uses `cast settings stream.KEY VALUE`, or the panel’s
**Streaming setup…** sheet. Apply is session-only and leaves the config file
untouched. Connection/encoder settings are locked while connecting, streaming,
reconnecting or stopping; stop the stream before applying those changes. Shared
pause/blur text and composition styling remain available.

## Twitch

Open Twitch Creator Dashboard and obtain your stream key and an ingest server
address. Put the key in the stream-key file; put only the server’s application
prefix in `server_url`. An example non-secret prefix is
`rtmp://sfo.contribute.live-video.net/app`. Prefer the service’s supplied RTMPS
address when available; Cast verifies its TLS certificate.

Twitch’s current official guidance recommends H.264 CBR, two-second keyframes,
4500 kbps for 1080p30, 6000 kbps for 1080p60, and AAC-LC up to 160 kbps. Cast’s
portable baseline is software x264, with `veryfast` as a sensible starting point.
Use your actual composition resolution/fps; service presets do not rescale it.
[Official broadcasting guidelines](https://help.twitch.tv/s/article/broadcasting-guidelines?language=en_US),
[official ingest URL format](https://dev.twitch.tv/docs/video-broadcast/).

For a private bandwidth test, Twitch documents the non-secret server query
`?bandwidthtest=true`; this prevents live viewing and can be checked in Twitch
Inspector. Cast preserves supported server query components when joining the key.
An ordinary start can broadcast publicly depending on your account settings, so
perform the manual acceptance checklist below before using a real audience.

## YouTube

Create the broadcast in YouTube Studio’s Live Control Room. Select its privacy
level deliberately and copy the server address and key separately. Store the key
in the stream-key file. Use the supplied RTMPS server prefix in `server_url`;
YouTube recommends encrypted RTMPS ingestion.

YouTube currently recommends H.264 CBR, two-second keyframes and 128 kbps stereo
AAC. Its current H.264 recommendations are 14,000 kbps for 1080p30, 17,000 kbps
for 1080p60 and 8,000 kbps for 720p. Choose settings your sustained upload and CPU
can support; the conservative generic default is not a quality guarantee at every
resolution. [Official encoder settings](https://support.google.com/youtube/answer/2853702?hl=en),
[Live Control Room setup](https://support.google.com/youtube/answer/9854503?hl=en).

## Troubleshooting and acceptance

Run `cast doctor`, then `cast stream status --json`. Available lifecycle states
are stopped, connecting, streaming, reconnecting, stopping and failed. Status
reports the local session, outgoing writes and queue/drop/retry information;
it does not report viewer counts or prove that viewers received the content.
Missing keys, rejected connections, TLS errors and exhausted retries have safe
messages without credentials. Check the service’s broadcast dashboard separately.

Leave upload bandwidth headroom above video + audio bitrate and protocol overhead.
If drops or retries grow, reduce bitrate/fps or choose a faster software encoder
preset after stopping the stream. Avoid competing large uploads. Silence can mean
a privacy effect is active, a selected PipeWire source disappeared, or its gain is
zero. Cast never falls back to a broader desktop audio source.

Protocol tests use synthetic media and private local ingest fixtures. No real
Twitch/YouTube account or public broadcast is used for automatic verification.
Before relying on either service, manually verify:

- Use a private/unlisted test or the service’s private bandwidth test.
- Connect paused; verify the styled pause frame and silence in the service preview.
- Explicitly resume; check text orientation, camera-only mirroring and selected audio.
- Pause/freeze/blur each lane independently while local recording and virtual camera run.
- Check stream health, audio/video synchronization and CPU/upload headroom over time.
- Disconnect/reconnect deliberately; verify fresh media, retained privacy state and bounded retries.
- Stop from CLI and panel; confirm the service’s buffered broadcast ends as expected.

Official settings above were checked on 2026-10-04. Service acceptance and hardware
performance remain user-run checks, distinct from local protocol verification.

## Measured resource guidance

A 65-second synthetic 720p30/4000 kbps local stream on a Core Ultra 5 235U used
about 14% of one CPU core and 52 MiB peak summed parent/worker RSS. Adding software
recording for 55 seconds raised those measurements to about 70% and 86 MiB.
The decoded stream's A/V end difference was 22 ms; a privacy transition retired
one frame, producing a single 66 ms interval. The final paused audio was silent.
See [the verification record](verification.md#sustained-streaming-and-sanitizer-checks)
for the commands, measurement scope and full results.

These figures include synthetic frame production and encoder work; physical
capture, composition, preview, virtual camera, service delivery and receiver
decoding need separate headroom. Shared mailbox pages count in both processes'
RSS. Higher resolution/fps, more complex content and slower presets can cost
more. Start conservatively, measure your actual combined workload and leave CPU
and upload capacity spare before relying on a live session. The measured profile
stayed below one CPU core and 128 MiB summed RSS; those figures are a local
comparison budget, not resource limits or a hardware performance guarantee.
