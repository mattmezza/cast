# Runtime configuration

`cast config defaults` prints the complete supported default schema to stdout, without
requiring devices or a daemon. The identical ready-to-edit example is installed under
`share/doc/cast/cast.conf.example`; installation never creates or overwrites user config.

The usual path is `$XDG_CONFIG_HOME/cast/cast.conf`, or `$HOME/.config/cast/cast.conf`
when XDG_CONFIG_HOME is unset/empty. A relative XDG_CONFIG_HOME is ignored with a
warning; HOME must be absolute for the fallback. XDG_RUNTIME_DIR must be absolute,
owned by the current UID, and not group/other writable. There is no unsafe /tmp socket
fallback. Alternatively use `--socket /absolute/private/directory/cast.sock`.

A missing default file uses defaults. A missing `--config PATH` or `config check PATH`
is an error. Explicit file/recording paths may be relative; they are resolved by the
invoking process's working directory. No shell expressions, environment variables,
backticks or tilde are expanded inside config values or command arguments.

Assignments accept `key = value` or `key: value`.
Syntax is ordinary INI parsed by vendored inih r60 (BSD-3-Clause). Sections and keys
are case sensitive. Whitespace around names/values is trimmed. Only whole-line
`;`/`#` comments are allowed. Values are literal single-line strings without quoting
or escape processing; a quote or backslash stays part of the value. Color is #RRGGBB.
Booleans are true/false (on/off aliases). Integers and finite decimal numbers have no
unit suffix in configuration. Percent settings use percentage points (22 means 22%);
audio gains use multipliers (0.8 means 80%). Milliseconds are explicit in key names.
Lists use commas without spaces or empty elements; order/cycle lists reject duplicates.
Navigation/filter lists contain XKB key names and rendered combinations.
Modifier order is Ctrl+Alt+Super+Shift; Space is rendered by name. The default filter
excludes the supplied sxhkd bindings, including their shifted symbols. UTF-8 source
names and paths are accepted; the bundled annotation bitmap font has limited glyphs.

Maximum line length is 8190 bytes; section names are at most 49 bytes, and preset
names use letters/digits/underscore/hyphen, fitting in that section limit. There are
at most 24 presets and 512 explicitly supplied keys. Duplicate keys, unknown sections,
unknown settings and invalid/conflicting values fail with filename/line diagnostics.
Repeated sections are allowed if their keys do not repeat. No multiline values.

Precedence is built-in defaults → config file → startup flags → runtime commands.
Startup flags are reapplied on reload. Runtime changes affect only the current session;
cast never writes configuration automatically. `config check [PATH]` validates locally.
`config reload` parses the complete candidate before any application, then rejects
unsafe changes as a whole. Successful reload replaces prior runtime overrides for
reloadable settings and preserves each lane's solid/freeze/blur flags, recording cut, and group state.
It never starts/stops a recording or resumes an output. There is no automatic watcher.

Backend, socket, output device/dimensions/fps/enabled state and virtual microphone name
require restart. While a recording exists, changing its codecs/container/CRF/encoder
preset/queue is rejected until recording stops. A reload that needs restart returns
all such reasons and leaves the prior config intact. Audio and camera acquisitions
must succeed before replacing the current device; a missing source never broadens
capture. Portal backend captures require explicit renewed consent when changed.

`reset` restores effective composition settings from defaults/config/startup flags,
with zoom returning to 1. It does not change output or recording states. Named presets
are composition-only: `[preset.NAME]` accepts layout, camera_shape,
camera_width_percent, split_ratio, camera_visible, zoom_factor, camera_anchor and
camera_aspect. Omitted preset fields preserve current composition; zoom returns to 1
unless explicitly provided. Built-in presets coding/demo/conversation remain available.
Custom cycles are selected by composition.preset_order. Hidden camera geometry persists.

Zoom.factor configures the initial toggle target; capture starts unzoomed. Source
switches reset zoom and annotations. Camera move/position and crop move use pixels;
region x/y/width/height are relative to the selected monitor. Anchor sizing uses canvas
width and a native 4:3 fallback until the camera's dimensions are known. Explicit
aspect changes use centered cropping with bounded offsets. Circle uses a square crop.
Camera content is mirrored by default; the screen stays unmirrored. Set `mirror = false`
in `[camera]` to disable it persistently, or use `cast camera mirror off` for the session.

`cast virtual message "TEXT"` changes `output.pause_text` for the session immediately
without resuming video. `cast settings SECTION.KEY VALUE [SECTION.KEY VALUE ...]`
applies a validated atomic session batch through the same schema and restart rules
as reload. Settings and panel controls do not write the selected config file.

The following table is the supported schema. `examples/cast.conf` includes every key.

| Section | Setting | Default | Accepted values / units |
|---|---|---|---|
| output | backend | `xorg` | xorg,wayland,synthetic |
| output | device | `/dev/video10` | Literal string |
| output | width | `1920` | 64–7680 (integer) |
| output | height | `1080` | 64–4320 (integer) |
| output | fps | `30` | 1–120 (integer) |
| output | enabled | `true` | true / false |
| output | pause_background | `#20252b` | #RRGGBB; pause_color alias |
| output | pause_title | `Paused` | Optional UTF-8 template, ≤127 bytes; pause_text alias |
| output | pause_subtitle | `` | Optional UTF-8 template, ≤255 bytes |
| output | pause_footer | `` | Optional UTF-8 template, ≤255 bytes; bottom centered |
| output | pause_font | `Noto Sans` | Fontconfig font name/pattern; config-file-only |
| output | pause_foreground | `#ffffff` | #RRGGBB |
| output | pause_title_size | `48` | 8–256 output pixels; scales down to fit |
| output | pause_subtitle_size | `24` | 8–256 output pixels; scales down to fit |
| output | pause_footer_size | `18` | 8–256 output pixels; scales down to fit |
| output | pause_text_gap | `12` | 0–512 output pixels between title and subtitle |
| output | blur_title | `Blurred` | Optional UTF-8 template, ≤127 bytes |
| output | blur_subtitle | `` | Optional UTF-8 template, ≤255 bytes |
| output | blur_footer | `` | Optional UTF-8 template, ≤255 bytes; bottom centered |
| output | blur_font | `Noto Sans` | Fontconfig font name/pattern; config-file-only |
| output | blur_color | `#101113` | Tint #RRGGBB |
| output | blur_foreground | `#ffffff` | Text #RRGGBB |
| output | blur_radius | `32` | 1–128 output pixels |
| output | blur_opacity | `0.6` | Tint opacity 0–1; blur still applies at 0 |
| output | blur_title_size | `48` | 8–256 output pixels; scales down to fit |
| output | blur_subtitle_size | `24` | 8–256 output pixels; scales down to fit |
| output | blur_footer_size | `18` | 8–256 output pixels; scales down to fit |
| output | blur_text_gap | `12` | 0–512 output pixels between title and subtitle |
| camera | device | `/dev/video0` | Literal string |
| camera | enabled | `true` | true / false |
| camera | visible | `true` | true / false |
| camera | shape | `rounded` | rectangle,rounded,circle |
| camera | anchor | `bottom-right` | top-left,top-right,bottom-left,bottom-right,top,bottom,left,right,free |
| camera | aspect | `native` | native,16:9,4:3,1:1 |
| camera | width_percent | `22` | 1–100 |
| camera | margin | `24` | 0–4096 (integer) |
| camera | radius | `24` | 0–2048 (integer) |
| camera | border_width | `2` | 0–256 (integer) |
| camera | border_color | `#ffffff` | #RRGGBB |
| camera | x | `0` | -7680–7680 (integer) |
| camera | y | `0` | -4320–4320 (integer) |
| camera | crop_x | `0` | -16384–16384 (integer) |
| camera | crop_y | `0` | -16384–16384 (integer) |
| camera | mirror | `true` | true / false |
| camera | background | `blurred` | blurred,gradient,solid; camera slot only |
| camera | background_color | `#20252b` | #RRGGBB tint/base |
| camera | background_blur_radius | `96` | 1–128 output pixels |
| camera | background_brightness | `0.25` | 0–1 brightness multiplier |
| camera | corner_order | `bottom-right,bottom-left,top-left,top-right` | Literal string |
| composition | layout | `overlay` | overlay,stage,split,screen,camera |
| composition | layout_order | `overlay,stage,split,screen,camera` | Unique comma-separated layout names |
| composition | split_side | `left` | left,right |
| composition | split_ratio | `25` | 5–95 |
| composition | fit | `contain` | contain,cover |
| composition | preset_order | `coding,demo,conversation` | Literal string |
| screen | width_percent | `78` | 1–100% of canvas width; stage only |
| screen | margin | `32` | 0–4096 output pixels; stage inset |
| screen | radius | `0` | 0–2048 output pixels; all screen layers |
| screen | border_width | `0` | 0–256 output pixels |
| screen | border_color | `#ffffff` | #RRGGBB |
| screen | background | `blurred` | blurred,gradient,solid |
| screen | background_color | `#20252b` | Solid fill or blurred tint |
| screen | background_blur_radius | `96` | 1–128 output pixels |
| screen | background_brightness | `0.25` | 0–1 brightness multiplier |
| background | source | `screen` | screen,camera for the screen/stage blurred backdrop |
| background | gradient_from | `#101827` | Start color, #RRGGBB |
| background | gradient_via | `#26354a` | Optional middle color, #RRGGBB |
| background | gradient_to | `#080b12` | End color, #RRGGBB |
| background | gradient_via_enabled | `true` | false selects a two-color gradient |
| background | gradient_angle | `135` | 0–360 degrees |
| background | gradient_waypoint | `50` | 1–99% middle-stop position |
| logo | enabled | `false` | true / false |
| logo | path | `` | Local image file; PNG transparency supported |
| logo | anchor | `bottom-right` | Corners or top,bottom,left,right edge centers |
| logo | width_percent | `12` | 1–100% canvas width, aspect preserved |
| logo | margin_x | `24` | 0–7680 output pixels from horizontal edge |
| logo | margin_y | `24` | 0–4320 output pixels from vertical edge |
| logo | opacity | `1` | 0–1, multiplied by image alpha |
| text | enabled | `false` | true / false |
| text | content | `` | Static UTF-8 text, up to 255 bytes |
| text | font | `Noto Sans` | Fontconfig font name/pattern, runtime editable |
| text | size | `28` | 8–256 output pixels |
| text | color | `#ffffff` | #RRGGBB |
| text | anchor | `bottom-left` | Corners or top,bottom,left,right edge centers |
| text | margin_x | `24` | 0–7680 output pixels from horizontal edge |
| text | margin_y | `24` | 0–4320 output pixels from vertical edge |
| text | opacity | `1` | 0–1 |
| capture | monitor | `` | Literal string |
| capture | kind | `monitor` | monitor,region,window |
| capture | mask_color | `#20252b` | #RRGGBB; independent of pause background |
| capture | x | `0` | 0–16384 (integer) |
| capture | y | `0` | 0–16384 (integer) |
| capture | width | `0` | 0–16384 (integer) |
| capture | height | `0` | 0–16384 (integer) |
| zoom | factor | `2` | 1–20 |
| zoom | min | `1` | 1–20 |
| zoom | max | `8` | 1–20 |
| zoom | step | `0.25` | 0.01–10 |
| zoom | transition_ms | `250` | 0–3000 (integer) |
| zoom | follow | `true` | true / false |
| zoom | deadzone | `80` | 0–2000 (integer) |
| zoom | smoothing | `0.18` | 0.001–1 |
| cursor | enabled | `true` | true / false |
| cursor | highlight | `false` | true / false |
| cursor | size | `24` | 1–256 (integer) |
| cursor | color | `#ffcc33` | #RRGGBB |
| clicks | enabled | `false` | true / false |
| clicks | duration_ms | `600` | 50–10000 (integer) |
| clicks | radius | `40` | 1–512 (integer) |
| clicks | left_color | `#ffcc33` | #RRGGBB |
| clicks | right_color | `#3399ff` | #RRGGBB |
| clicks | middle_color | `#cc66ff` | #RRGGBB |
| clicks | middle | `false` | true / false |
| keys | enabled | `false` | true / false |
| keys | mode | `shortcuts` | shortcuts,all |
| keys | position | `bottom-left` | top-left,top-right,bottom-left,bottom-right |
| keys | font_size | `24` | 8–96 (integer) |
| keys | timeout_ms | `3000` | 100–30000 (integer) |
| keys | color | `#ffffff` | #RRGGBB |
| keys | background | `#20252b` | #RRGGBB |
| keys | filter | `Super+Pause,Super+F9,Super+F10,Super+F11,Super+F12,Super+Shift+P,Super+Shift+R,Super+P,Super+Shift+Space,Super+Shift+C,Super+=,Super+-,Super+Shift+A,Super+Shift+L,Super+Shift+D,Super+Shift+M,Super+Shift+S,Super+Z,Super+Shift++,Super+Shift+_,Super+Shift+K,Super+Shift+B,Super+Shift+V,Ctrl+Super+Shift+R,Ctrl+Super+Shift+S,Ctrl+Super+F,Ctrl+Super+Shift+F` | Literal string |
| keys | navigation | `Left,Right,Up,Down,Home,End,Page_Up,Page_Down,Escape,Tab,Return,BackSpace,Delete` | Literal string |
| annotations | virtual_keys | `true` | true / false |
| annotations | virtual_clicks | `true` | true / false |
| annotations | record_keys | `true` | true / false |
| annotations | record_clicks | `true` | true / false |
| audio | mic | `false` | true / false |
| audio | desktop | `false` | true / false |
| audio | mic_source | `` | Literal string |
| audio | desktop_source | `` | Literal string |
| audio | mic_gain | `1` | 0–4 |
| audio | desktop_gain | `1` | 0–4 |
| audio | virtual | `false` | true / false |
| audio | virtual_name | `cast-microphone` | Literal string |
| record | directory | `.` | Literal string |
| record | container | `matroska` | Literal string |
| record | video_codec | `libx264` | Literal string |
| record | audio_codec | `aac` | Literal string |
| record | crf | `23` | 0–51 (integer) |
| record | preset | `veryfast` | Literal string |
| record | countdown | `0` | 0–60 (integer) |
| record | queue | `8` | 1–120 (integer) |
| stream | service | `custom` | custom,twitch,youtube |
| stream | server_url | `` | Bounded rtmp:// or rtmps:// server prefix; no key/userinfo |
| stream | key_file | `` | Absolute owned regular mode-600 stream-key file; no symlink |
| stream | tls_ca_file | `` | Optional absolute CA trust file; empty uses system trust |
| stream | video_bitrate_kbps | `2500` | 100–50000 kbps |
| stream | audio_bitrate_kbps | `128` | 32–320 kbps |
| stream | encoder_preset | `veryfast` | ultrafast,superfast,veryfast,faster,fast,medium,slow,slower,veryslow |
| stream | queue_frames | `3` | 1–120 raw frames; freshest frame selected before encoding |
| stream | lag_ms | `250` | 50–5000 ms |
| stream | connect_timeout_ms | `5000` | 100–30000 ms |
| stream | write_timeout_ms | `3000` | 100–30000 ms |
| stream | reconnect_attempts | `3` | 0–10 retries after initial attempt |
| stream | reconnect_initial_ms | `500` | 100–10000 ms |
| stream | reconnect_max_ms | `4000` | 100–30000 ms; at least initial delay |
| annotations | stream_keys | `true` | true / false |
| annotations | stream_clicks | `true` | true / false |
| preview | enabled | `false` | true / false |
| preview | target | `virtual` | virtual,record,stream |
| ipc | socket | `` | Literal string |
| ipc | timeout_ms | `5000` | 100–30000 (integer) |

Capture kind=window selects the active Xorg window once at startup; interactive window selection remains session state.

## Pause and blur text

Solid pause and blur share their configured styles across virtual, recording and streaming;
the output flags remain independent. Set title, subtitle and footer to empty values
for a screen with no text. `pause_text` aliases `pause_title`, and `pause_color`
aliases `pause_background`; specifying either spelling twice is a duplicate.

```ini
[output]
pause_title = Back soon
pause_subtitle = {date:%A, %d %B} · {time:%H:%M}
pause_footer = {datetime:%A, %d %B · %H:%M}
pause_footer_size = 18
pause_text_gap = 12
pause_font = Noto Sans
pause_background = #20252b
pause_foreground = #ffffff
blur_title = Taking a break
blur_subtitle = {datetime}
blur_footer = Back shortly
blur_footer_size = 18
blur_text_gap = 20
blur_font = Noto Sans
blur_radius = 48
blur_opacity = 0.6
```

`{date}` expands to YYYY-MM-DD, `{time}` to HH:MM:SS, and `{datetime}` to
YYYY-MM-DD HH:MM:SS in the daemon's local timezone. Add a colon and a strftime
format to any of them: `{time:%H:%M}`, `{date:%A}`, or `{datetime:%Y-%m-%d %H:%M}`.
`{{` and `}}` emit literal braces. Placeholders update while the screen is shown;
title/subtitle/footer use the same time snapshot. Unknown placeholders, unmatched braces,
invalid formats and expansion beyond the bounded text capacity fail validation.
Other config strings remain literal and never expand shell expressions.

Fontconfig resolves the font pattern through system fonts; use `fc-match 'Noto Sans'`
to inspect it. FreeType draws the resolved font at the requested output pixel size,
scaling down to fit the canvas. The Arch package includes noto-fonts as a dependency.
The footer is centered near the bottom with an adaptive safe margin. Title and
subtitle remain a centered block; `pause_text_gap` / `blur_text_gap` are independent
of font size and can be zero. Text and spacing shrink together if needed to fit,
reserving room for the footer.

Fonts may be chosen only in the config file and applied with `cast config reload`;
CLI/panel session settings cannot change them. A failed font load rejects the reload.
For example, set `blur_font = Noto Serif` or `pause_font = Noto Sans:style=Bold`
under `[output]`, then reload. `fc-list : family` lists installed families.

This renderer supports UTF-8 glyphs provided by the selected face; it does not
implement complex-script shaping or an input-method editor.

Camera edge anchors `top`, `bottom`, `left`, `right` center the camera on that edge;
`top-center`, `bottom-center`, `center-left`, `center-right` are equivalent aliases.
They retain the configured margin and keep that edge centered when resized.

## Camera-slot background

In split and camera-only layouts, uncovered pixels in the camera's allocated area
use `camera.background`. The default `blurred` backdrop enlarges the current camera
frame, blurs a reduced image and dims/tints it. It refreshes with each camera frame
used in composition; a repeated snapshot can reuse cached work. `gradient` uses a
subdued static gradient derived from `background_color`, and `solid` fills that
colour uniformly. Missing/hidden camera input uses fresh static content, never a
previous camera frame. The foreground webcam and screen-layer fitting are unchanged.

```sh
cast settings camera.background blurred camera.background_brightness 0.25
cast settings camera.background_blur_radius 96 camera.background_color '#20252b'
```

These session controls are available in **Camera → Camera appearance** too.

## Stage, backgrounds and branding

`composition.layout = stage` uses `screen.width_percent` and camera width to
position two aspect-preserving sources. Camera anchoring moves the screen to the
opposite side. A small screen and camera can fit alongside each other; increasing
sizes naturally produces overlap with camera in front. Screen border and radius
also apply in overlay, split and screen layouts; zero defaults preserve square
unbordered screen content. The screen's camera-independent zoom remains available.

`screen.background` controls the uncovered screen/stage area. Its blurred mode
uses a scaled-up, subdued current frame chosen by `background.source`; camera
backgrounds keep their existing camera source. Both gradient modes share the
`background.gradient_*` controls. Disable the via stop for a two-color gradient;
otherwise `gradient_waypoint` places it between the start and end colors. Angle
0 runs left to right, 90 top to bottom, 180 right to left and 270 bottom to top;
intermediate values make diagonals. Colors
are literal `#RRGGBB`, without quotes in the INI file.

Logo and static text are composition overlays, so solid pause replaces them and
freeze/blur include them. They never change output state. Logos preserve image
alpha and aspect ratio. Supported logo files are PNG, JPEG, WebP and BMP, up to 8 MiB, 4096 pixels
per edge and 4,194,304 total pixels. Files are decoded locally; animated logo
playback is not supported. Use a local image path, not a URL; file paths are resolved
by the daemon and do not expand `~` or environment variables. Static text is
literal (its braces are not pause/blur placeholders). Unlike pause/blur fonts,
`text.font` is runtime editable. Use an installed family such as `Noto Serif`, or
a Fontconfig pattern such as `:file=/absolute/path/font.ttf`. Fontconfig may choose
a fallback for an unavailable family; an explicit unreadable font file is rejected.
Runtime updates remain session-only.

## Streaming configuration

Only the stream-key file path is stored in configuration. Server URLs may use
RTMP or RTMPS, never arbitrary FFmpeg schemes, credentials or embedded stream keys.
No tilde, environment or command expansion occurs. An empty server/key configuration
keeps streaming unconfigured and never starts it. Optional tls_ca_file selects a CA trust file
for a private ingest; certificate and hostname verification stay
enabled. Key-file preparation and official service settings are in [streaming.md](streaming.md).

The service preset fills useful quality defaults; explicit INI bitrate/preset keys
win regardless of file order. Runtime batch values after stream.service override its
quality selection. Resolution and fps remain shared with the composition. Stop a
connecting/streaming/reconnecting/stopping session before changing [stream] settings.
All resource validation is atomic; failed preparation preserves the prior settings
and unrelated outputs. Runtime edits never write the configuration automatically.

The configured raw video queue plus one cadence frame is bounded to 512 MiB per
streaming session. Oversized resolution/queue combinations fail with instructions to
reduce them. The audio ring is bounded to one second of stereo 48 kHz samples.
Backpressure drops older raw frames before encoding, and excessive network lag
retires/reconnects the session rather than replaying a stale backlog.

Migration: annotations.live_keys/live_clicks become virtual_keys/virtual_clicks,
and preview.target=live becomes virtual. Old names are rejected with guidance.
Add stream_keys/stream_clicks under [annotations] and the [stream] fields above;
omitted values already use their documented defaults.

`capture.mask_color` controls Xorg panel, preview and countdown masking before
composition, independently of `output.pause_background`. Covered desktop content
cannot be reconstructed by the current capture path. There is no transparent
exclusion setting; remove `[capture] exclusion` if an unreleased configuration
contains it. Unknown settings fail validation without replacing the active config.

## Edition and licensing settings

Both editions read the same public schema. No setting selects the edition, trusted
keys, entitlement, devices, release timestamp or feature grants. The following
fields are new; existing `record.video_codec` now defaults to `auto`.

| Setting | Default | Supported values and behavior |
|---|---|---|
| `licensing.file` | empty | Literal bounded license-file path. Empty selects the imported per-user Pro license store internally. Reads reject symlinks and nonregular files; imports copy a verified snapshot. |
| `licensing.upgrade_url` | empty | Optional HTTPS info/purchase URL; empty hides the action. Opened only on an explicit panel click. |
| `record.bitrate_kbps` | `6000` | Integer `100..100000`; used for encoder-neutral bitrate control. |
| `record.rate_control` | `auto` | `auto`, `bitrate`, `crf`. Auto keeps x264/x265 CRF behavior when available and uses bitrate for OpenH264. Explicit unsupported CRF fails; no numeric CRF translation. |
| `stream.video_encoder` | `auto` | FFmpeg encoder name, or `auto`. Uses the existing stream video bitrate; explicit selection never silently changes encoder. |

`auto` initializes candidate encoders to test readiness. The controlled LGPL
profile uses OpenH264; Community system builds retain x264 when available. Legacy
`record.crf`, `record.preset`, and `stream.encoder_preset` remain readable. They
are inactive with OpenH264 and are reported as such. Encoder acquisition does not
start capture or change presentation privacy. Codec initialization fails before
starting a recording/stream when an explicit selection cannot run.

`cast config check [PATH]` checks schema syntax and cross-field constraints without
requiring commercial entitlement. Add `--availability` to initialize local codecs
and report media-profile/capability readiness; it does not acquire devices or
connect to streaming services. Planned Pro workflows have no invented dormant
configuration keys. Unknown keys remain strict errors.

`cast config migrate [PATH] --edition pro` prints a dry-run of the original INI
with encoder-neutral selections, preserving unrelated fields/comments and legacy
quality values. `--write OUTPUT` writes a new mode-0600 file; an existing output is
not overwritten. In-place replacement requires `--write PATH --backup NEW_FILE`.
Startup/reload never runs migration or saves session changes automatically.

## Optional Pro workflow fields

Both editions parse and preserve the 93 declarative cinematic/cursor, transcription,
subtitle and notes values. They stay dormant in Community. Existing configs retain
legacy zoom and do not start local speech or notes motion. The full new field/range
inventory is in [Pro workflow configuration](pro-workflows.md); merge-safe defaults
are in [pro-workflows.conf](../examples/pro-workflows.conf). Unknown keys still fail.

Model/language/device/VAD settings are shared by captions and speech-following
notes. Subtitle and notes visual styles are separate because they render on
different surfaces. Session edits never save navigation, consent or running state
to the config.
