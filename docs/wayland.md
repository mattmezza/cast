# Wayland backend

Build `make X11=0 WAYLAND=1` to omit all Xorg dependencies, or
`make X11=1 WAYLAND=1` for both backends. GDBus (GLib `gio-unix-2.0`) and
PipeWire implement the optional backend; FFmpeg and PipeWire audio remain shared.
Start `cast --backend wayland`. A desktop consent dialog chooses one screen.
The daemon continues servicing controls while consent is pending. Live output
starts privacy paused, including after source selection; explicitly resume it.
Configure `zoom.follow=false` if the portal does not advertise cursor metadata.
Keep `capture.kind=monitor`, `capture.monitor` empty, and keys/clicks/preview off.

The [ScreenCast portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)
provides session consent and an isolated PipeWire remote. cast subscribes to
request responses before starting each operation, watches session closure and
portal disappearance, and stops using captured frames after permission loss.
Cancelling the consent dialog leaves capture unavailable and neutral; it never
falls back to another source. `cast capture monitor` requests a fresh chooser
once the previous request has completed. `cast quit` closes outstanding requests
and the session. No permission persistence token is stored.

PipeWire negotiates packed RGB/BGR CPU buffers and changes dimensions without
changing output dimensions or frame rate. Old frames are discarded on
renegotiation, invalid buffers, revocation and disconnect; mapped memory is
copied before each buffer returns to PipeWire. A compositor that supplies only
DMA-BUF must provide a CPU fallback: DMA-BUF import is unsupported. On v6 portals,
cast targets the stable `pipewire-serial` when supplied; older portals use their
node ID inside the session's isolated remote.

Cursor mode is selected from the advertised mask: metadata first, otherwise
embedded when visible or hidden when requested. Metadata coordinates are
source-local pixels and feed the shared screen transform. The separately drawn
cursor uses cast's glyph and size rather than the compositor cursor artwork.
Embedded cursors cannot be independently moved, scaled or hidden without a new
portal session; changing that mode requires restart. Cursor-follow and highlighting
require metadata. No second cursor is drawn over an embedded cursor.

| Feature | Xorg | Wayland |
|---|---|---|
| Screen capture | Implemented; hardware acceptance pending | Implemented portal/PipeWire; compositor acceptance pending |
| Monitor list/select/cycle | Implemented RandR | Portal chooser only; `capture monitor` selects again |
| Numeric and interactive regions | Implemented | Unsupported |
| Window selection/active-window-once | Implemented | Unsupported as CLI controls; some portals offer windows |
| Cursor metadata/follow/highlight | Implemented | Implemented when portal advertises metadata; otherwise unsupported |
| Embedded cursor | Desktop capture excludes cursor; drawn separately | Implemented when portal advertises it |
| Passive keys and click observation | Implemented XI2; desktop acceptance pending | Unsupported |
| Preview | Implemented; recursion constraints apply | Unsupported |
| V4L2 camera/output, layouts, recording/audio | Shared implementation | Shared implementation |

Implemented describes code paths, not a claim of hardware or conferencing
validation. See [hardware acceptance](hardware-acceptance.md) for required tests.
Global input is never requested through InputCapture. Bind the existing CLI in
your compositor configuration; sxhkd is for Xorg. A Sway binding can use
`bindsym $mod+Shift+p exec cast pause`; Hyprland can use
`bind = SUPER SHIFT, P, exec, cast pause`. Consult your compositor version's
keybinding documentation if its syntax differs. Wayland selection and preview
commands return actionable unsupported errors rather than success.
