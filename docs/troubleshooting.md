# Troubleshooting and read-only diagnosis

Run `cast doctor` inside the graphical login. It checks backend access, webcam
formats, the virtual device, audio, recording codecs and destination storage.
It does not create kernel devices, open portal consent, change permissions, or
install packages. Start the producer before selecting its camera in a conference:
with `exclusive_caps=1`, consumers may only see the device after cast opens it.

## Arch Linux setup

These commands are examples for the user to run; the build does not run them:

```sh
sudo pacman -S --needed base-devel pkgconf ffmpeg pipewire libx11 libxext libxrandr libxi libxfixes libxcomposite
sudo pacman -S --needed v4l2loopback-dkms linux-headers
sudo modprobe v4l2loopback devices=1 video_nr=10 card_label=cast exclusive_caps=1
```

Match headers to the running kernel (`linux-lts-headers` for the LTS kernel,
and equivalent headers for custom kernels). Rebuild DKMS modules after kernel
updates and reboot into the corresponding kernel before diagnosis. If the module
fails to load, compare `uname -r`, installed headers and `dkms status`. Secure Boot
may require signing the module according to your machine's setup. Consult
[v4l2loopback's documentation](https://github.com/v4l2loopback/v4l2loopback)
for module options, DKMS and capabilities.

Configure `/dev/video10` as output and the actual camera node as input; never
use the same device for both. `v4l2-ctl --list-devices` and
`v4l2-ctl -d /dev/video0 --list-formats-ext` (from `v4l-utils`) help identify them.
If device access is denied, inspect `ls -l /dev/video*`, your graphical-session
ACLs (`getfacl`) and your distribution's device policy. Do not run cast as root.
Unplugged cameras produce safe neutral/absent content; choose the reconnected
node explicitly with `cast camera device /dev/videoN`.

For Wayland, install `glib2`, `xdg-desktop-portal` and the backend for your desktop
(e.g. `xdg-desktop-portal-gtk` plus the compositor-specific implementation where
needed). Run `systemctl --user status xdg-desktop-portal` and inspect its journal
when consent fails. Portal support differs by desktop. See [Wayland](wayland.md).

## Privacy and routing

`cast live resume` explicitly enables the live picture. `cast record resume`
continues an existing paused file. `cast resume` restores only lanes that the
previous group pause changed. A neutral frame is expected while live is paused.
Freeze deliberately repeats a composed frame; use pause for privacy.

cast cannot mute a physical microphone selected directly in the call application.
Choose cast's optional virtual audio source to use its mix and pause controls, or
use the application's own mute command. Desktop audio is off by default. A
speaker-monitor source can include other callers and system sounds; select an
application node when available. Keep the virtual source out of captured desktop
sources to avoid feedback. cast does not send its mix to physical speakers.

Run `cast audio list` and select explicit source names. If a source disappears,
its lane becomes silence; cast never substitutes a broader source. Node names may
change when devices reconnect. Inspect `wpctl status` or `pw-cli ls Node` to
understand your PipeWire graph. Audio and video are separate conference devices.

## Recording and compatibility

Recording pause removes the interruption from the same file's timeline; it does
not suspend FFmpeg. Check `cast status --json` for errors and frame drops. Storage
failure stops recording while the control/live loop remains operational. Keep
partial recordings: Matroska is the default for recoverability; recovery depends
on which packets and indexes reached storage. Try remuxing a copy with
`ffmpeg -i partial.mkv -c copy recovered.mkv`; successful recovery is not guaranteed.
`record stop` acknowledges finalization initiation with the filename; poll
record.finalizing until false before using it. Completion/errors also appear in
the foreground daemon. A filename that already exists is rejected. Check free space and output directory
permissions before long captures.

A conferencing application may restrict resolution or heavily compress camera
video. Text readability depends on viewers enlarging/pinning tiles. No conferencing
compatibility has been claimed without explicit acceptance testing. Try the
application's camera selector after starting cast; consumer-side caching may
require reopening the chooser or restarting that application.

Xorg preview can enter root capture; keep it outside the selected region or turn
it off. Arbitrary root windows cannot universally be excluded invisibly. When
selecting, cast hides its selection UI before capture. Escape retains the previous
source. A destroyed/minimized window or removed monitor never silently changes
to a whole-desktop capture; errors and neutral frames are deliberate.

If commands cannot connect, check the selected runtime socket and ensure daemon
and CLI use the same user and instance. XDG_RUNTIME_DIR must be absolute, owned
by you and private. Do not manually remove a socket belonging to a live daemon.
Only stale sockets safely owned by the current user are cleaned automatically.

If an invalid edited config prevents routing, use `cast --socket /absolute/path/cast.sock
status --json` or the same explicit socket for config reload. Daemon startup and local
configuration checks remain strict.

## No local camera view after starting cast

`cast` runs as a foreground terminal daemon. Preview defaults off and live output
starts privacy-paused. In another terminal, run `cast preview on` for the local
window, then `cast live resume` to enable the composition. Select the loopback
camera in the conferencing app after producer startup. The physical webcam LED
can be on while live is paused because cast has opened its input; privacy pause
controls transmitted video and the virtual audio mix. Use `cast quit` to release
the camera, then restart the daemon after rebuilding or upgrading its executable.

## Google Meet self-view looks mirrored

Check text in cast's preview and ask another participant to check the received image
before changing output orientation. `cast camera mirror` changes only the webcam
layer; screen content remains unmirrored. Meet can mirror its own camera tile locally.
Google [documents mirrored self-view backgrounds with correct remote orientation](https://support.google.com/meet/answer/13954947),
which is evidence for a display-side effect, not proof of a particular call's remote
receive path. The kernel-consumer orientation regression verifies screen direction
independently of Meet. Compensating for local self-view by reversing transmitted text
would reverse it for participants whose received image was already correct.

For a recursion-free presentation, select the application window or a region that
does not contain Meet's own camera tile. The preview footprint mask excludes cast's
preview; it cannot exclude a call application's self-view automatically.
