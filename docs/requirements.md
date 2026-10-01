# Requirements traceability

This maps `cast-build-prompt.md` to implementation and verification. Checked items
mean implementation with independent automated evidence. They do not establish
hardware compatibility. Integration-only acceptance remains unchecked below.

| Brief requirement | Implementation | Evidence / remaining acceptance |
|---|---|---|
| M1 foreground daemon, single executable, bounded IPC, ownership, stale socket, ordering/timeouts, signals | main.c, commands.c | Core/production command tests and full post-reboot socket transport integration pass |
| M1 narrow platform boundaries, RGBA ownership/stride/monotonic time | cast.h, platform.c, compositor.c | Owned-frame/stride tests; X11 types remain private |
| M1 independent privacy/freeze/group state and neutral barriers | state.c, commands.c, media.c | Every initial output state combination; command/pause/barrier tests; driver acceptance pending |
| M1 Xorg shared-memory capture and compatible V4L2 formats/output | x11.c, webcam.c | Xvfb smoke passes both paths; real FFmpeg V4L2 privacy/orientation and async camera checks pass |
| M2 overlay/split/screen/camera, masks/aspects/crop/mirror/border/anchors/fit | compositor.c, commands.c | Actual synthetic output pixels, geometry/clamping/scalar sampling tests |
| M2 monitors, regions, active/selected windows, cancellation/hotplug/failures | x11.c, platform.c | Isolated Xvfb smoke passes windows, selections/remap/errors; physical hotplug remains manual |
| M3 mic/explicit desktop sources/gain/disappearance and virtual source | audio.c, media.c | Synthetic mixing/clipping/stale tests; physical mic and virtual-source readiness/silence verified |
| M3 recording codecs/container, exclusive filenames, bounded queue/backpressure, failure isolation | record.c, media.c | Real encoded/decoded files; slow in-flight encoder, restart and injected ENOSPC tests |
| M3 same-file pause, monotonic active timeline, AV synchronization and privacy queues | record.c, audio.c | Repeated pause AV timestamps; sensitive queued pixels/audio absent after barriers |
| M3 countdown cancellation, async finalization and partial-file preservation | main.c, commands.c, record.c | Production commands, zero late starts, no overwrite, finalization/error tests |
| M4 presets/reset preserve output state, zoom easing/follow/locking, cursor/click/key transforms | commands.c, compositor.c | Source generation, same-frame lane consistency, coordinate/annotation pixel tests |
| M4 passive XI2/XKB, layouts/filter/repeat and privacy history | x11.c | Isolated Xvfb input/layout/filter/privacy passes; real user layouts/IME remain manual; no raw log |
| M4 preview actual lane and local status, bounded worker, recursion limitations | x11.c, main.c | Xvfb painted-preview/exclusion smoke passes; actual conference/desktop behavior remains manual |
| M5 optional portal consent/cancel/revocation/session lifetime and renegotiation | wayland.c | CPU buffer/revocation and expanded private mock D-Bus lifecycle suite pass |
| M5 Wayland-only build with explicit unsupported capabilities | platform.c, wayland.c, Makefile | Xorg removable at build; capability matrix in wayland.md; real compositor acceptance pending |
| M5 complete commands/help/version/status/doctor/config precedence | main.c, commands.c, config.c | Production command/reload/JSON tests and complete CLI/man reference |
| M5 maintained INI, complete schema/presets, atomic reload/restart reasons/privacy-preserving reset | config.c, commands.c, vendored inih r60 | Strict parser, all-output state tests and successful/failed production reloads |
| M5 install/uninstall/binary/source Arch packaging and compatible licensing | Makefile, packaging/PKGBUILD, LICENSE, licenses/ | Staged binary/source package with dependency manifest; no user config overwrite |

- [x] Independent core, geometry, annotations, production control and media validation.
- [x] Real synthetic video/audio encode/decode, queue privacy, slowdown/failure isolation.
- [x] Wayland CPU formats/bounds/revocation/capability validation.
- [x] Compiler diagnostics and address/undefined sanitizer checks of independent tests.
- [x] Full socket IPC integration in a normal user session.
- [x] Isolated Xvfb capture/input/selection/preview smoke execution, with both capture paths.
- [x] Expanded private D-Bus portal lifecycle execution in a normal session.
- [x] Kernel loopback consumer pixels and physical PipeWire virtual-source privacy silence.
- [ ] Real Wayland portal streams, physical hotplug and user keyboard/IME acceptance.
- [x] Short full-pipeline 1080p30 live/record/both CPU/RSS/fps/drop samples.
- [ ] Remote conferencing compatibility, endurance, physical AV drift and glass-to-glass latency.

See verification.md for measured results and environment restrictions, and
hardware-acceptance.md for the exact integration acceptance procedure. No privileged
system setup, deployment or repository publication has been performed.
