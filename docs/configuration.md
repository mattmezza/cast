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
reloadable settings and preserves live pause/freeze, recording pause, and group state.
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

The following table is the supported schema. `examples/cast.conf` includes every key.

| Section | Setting | Default | Accepted values / units |
|---|---|---|---|
| output | backend | `xorg` | xorg,wayland,synthetic |
| output | device | `/dev/video10` | Literal string |
| output | width | `1920` | 64–7680 (integer) |
| output | height | `1080` | 64–4320 (integer) |
| output | fps | `30` | 1–120 (integer) |
| output | enabled | `true` | true / false |
| output | pause_color | `#20252b` | #RRGGBB |
| output | pause_text | `Paused` | Literal string |
| camera | device | `/dev/video0` | Literal string |
| camera | enabled | `true` | true / false |
| camera | visible | `true` | true / false |
| camera | shape | `rounded` | rectangle,rounded,circle |
| camera | anchor | `bottom-right` | top-left,top-right,bottom-left,bottom-right,free |
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
| camera | mirror | `false` | true / false |
| camera | corner_order | `bottom-right,bottom-left,top-left,top-right` | Literal string |
| composition | layout | `overlay` | overlay,split,screen,camera |
| composition | layout_order | `overlay,split,screen,camera` | Literal string |
| composition | split_side | `left` | left,right |
| composition | split_ratio | `25` | 5–95 |
| composition | fit | `contain` | contain,cover |
| composition | preset_order | `coding,demo,conversation` | Literal string |
| capture | monitor | `` | Literal string |
| capture | kind | `monitor` | monitor,region,window |
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
| keys | timeout_ms | `1800` | 100–30000 (integer) |
| keys | color | `#ffffff` | #RRGGBB |
| keys | background | `#20252b` | #RRGGBB |
| keys | filter | `Super+Pause,Super+F9,Super+F10,Super+F11,Super+F12,Super+Shift+P,Super+Shift+R,Super+P,Super+Shift+Space,Super+Shift+C,Super+=,Super+-,Super+Shift+A,Super+Shift+L,Super+Shift+D,Super+Shift+M,Super+Shift+S,Super+Z,Super+Shift++,Super+Shift+_,Super+Shift+K,Super+Shift+B,Super+Shift+V,Ctrl+Super+Shift+R,Ctrl+Super+Shift+S,Ctrl+Super+F,Ctrl+Super+Shift+F` | Literal string |
| keys | navigation | `Left,Right,Up,Down,Home,End,Page_Up,Page_Down,Escape,Tab,Return,BackSpace,Delete` | Literal string |
| annotations | live_keys | `true` | true / false |
| annotations | live_clicks | `true` | true / false |
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
| preview | enabled | `false` | true / false |
| preview | target | `live` | live,record |
| ipc | socket | `` | Literal string |
| ipc | timeout_ms | `5000` | 100–30000 (integer) |

Capture kind=window selects the active Xorg window once at startup; interactive window selection remains session state.
