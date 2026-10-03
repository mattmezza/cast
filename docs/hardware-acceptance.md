# Hardware acceptance and measurement

Hardware-dependent integrations are untested until these checks are recorded on
an actual graphical Linux login. Synthetic tests do not establish conference,
portal, webcam or v4l2loopback compatibility. Record kernel, desktop/compositor,
PipeWire/portal/FFmpeg versions, camera format, consumer and output settings.

1. Load a v4l2loopback device with `exclusive_caps=1`; start cast at 1920x1080/30.
   Open it in a V4L2 consumer and inspect the captured output, including the neutral
   startup picture. Test the application's camera chooser after producer start.
2. Select a camera offering YUYV and an MJPEG camera. Check decoded colors,
   mirroring, centered/native/aspect crop, offset limits, rectangle/rounded/circle
   masks, borders and all layouts. Resize at each anchor; test bounds.
3. On Xorg select every RandR monitor, numeric region and dragged region. Escape
   selection without changing source. Select and move/resize/minimize/destroy a
   window; unplug a monitor. Confirm neutral frames and source-specific errors.
4. Check real XI2 clicks, left/right modifiers, navigation keys, repeat collapsing
   and two keyboard layouts. Pause while typing, then resume; no paused input may
   replay. All mode can expose sensitive typing; do not test using real secrets.
5. Test zoom easing, follow dead zone and monitor edges. Cursor and click rings
   must follow the same image transform. Check out-of-source cursor behavior.
6. Test virtual only, recording only (`virtual_enabled=false`), and simultaneous lanes.
   Pause independently and in groups, change state during group pause, repeatedly
   pause/resume. Inspect recorded timestamps and listening tests for sync after
   both short and long pauses. Recording must remain the same file.
7. Watch the actual consumer while issuing pause and while an encoder is slow.
   No sensitive queued video/audio may leak after pause acknowledgement. Test
   silence on off/paused/frozen virtual virtual audio; unfreeze must retain privacy
   pause. Test record countdown cancellation and immediate group pause.
8. List audio nodes, select a microphone and one explicit desktop/application
   source. Disconnect each. Verify silence with no automatic monitor fallback.
   Select cast's virtual mic in a call and ensure no physical-speaker loop occurs.
9. Exercise unwritable/full recording storage and encoder errors in a disposable
   test directory; virtual camera output and IPC must remain responsive and partial files
   must be retained. Never fill the machine's main disk for this test.
10. Preview virtual and recording targets when annotations differ. Confirm local
    status labels do not appear in the transmitted composition. Check preview
    recursion for root, window and region capture; document desktop limitations.
11. Build `X11=0 WAYLAND=1`; consent, cancellation, rejection, selected-source
    resize, portal termination/restart and permission revocation. No stale frames
    may continue after revocation. Check cursor metadata/embedded/hidden modes
    individually where advertised. Unsupported commands must return nonzero.
12. Test at least two actual conferencing consumers. Record their names/versions,
    detected resolutions, text readability and whether producer-before-consumer
    startup is required. Multiple camera tiles do not create a shared device.
13. Measure 1920x1080 at 30 fps for virtual-only, record-only and both for 10 minutes.
    Record CPU (`pidstat -p PID 1`), RSS, dropped frames, encoder queue depth and
    measured glass-to-glass latency using a visible clock in the captured source.
    Report hardware, encoding preset and achieved fps. No baseline numbers are
    asserted in this repository without such a measurement.
14. Install into a staging DESTDIR, verify packaged binary/docs/license/example,
    and uninstall. Confirm installation never modifies the user's config.

## Streaming service acceptance

Use a private/unlisted YouTube event or Twitch bandwidth-test mode, with your own
mode-0600 key file, following [streaming.md](streaming.md). Check privacy-paused
connection first; explicitly resume and verify received text orientation, resolution,
H.264/AAC quality, timestamps and audio. Test pause/freeze/blur and group restore,
then run recording and virtual camera simultaneously. Interrupt the network and verify
finite retries, fresh media after reconnect and responsive stop. These account/device
checks are deliberately separate from synthetic local RTMP/RTMPS acceptance.
