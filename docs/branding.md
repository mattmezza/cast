# SuperScreencast branding

SuperScreencast is the public product name. The editions are **SuperScreencast
Community** and **SuperScreencast Pro**. The website is
`https://superscreencast.com`; buying the domain does not deploy the site or
configure DNS.

App-launcher entries, native window titles, panel edition labels and public
website copy use the public names. Repository and runtime identifiers remain
stable:

| Interface | Retained identifier |
|---|---|
| Community CLI / launcher | `cast` / `cast-app` |
| Pro CLI / launcher | `cast-pro` / `cast-pro-app` |
| GitHub repositories | `mattmezza/cast`, private `mattmezza/cast-pro` |
| Packages, icons, desktop files | `cast`, `cast-pro` |
| Community config | `~/.config/cast/cast.conf` |
| Pro config | `~/.config/cast/cast.conf` (shared default, or explicit `--config`) |
| Xorg window classes | `CastPanel`, `CastPreview`, `CastProPanel`, `CastProPreview` |
| Color-picker class | `CastColorPicker` |
| License/update payloads and signing domains | Existing `cast-pro` product and `CAST-*` domains |

Virtual-device names remain those configured on the host. For example, a
v4l2loopback camera created with `card_label=cast` continues to appear as `cast`
in conferencing apps. Renaming the application does not recreate devices, alter
outputs, write the user's configuration, or invalidate licenses.

Community remains MIT and includes the existing capture, camera, recording and
streaming features without a license. Pro adds its three licensed workflows:
cinematic zoom, local transcription/subtitles, and timed or speech-following
speaker notes. The brand change does not change edition capabilities.
