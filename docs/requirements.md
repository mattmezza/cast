# Requirements traceability

This checklist tracks cast-build-prompt.md. A feature is checked only after code and
verification exist; hardware-only acceptance is recorded separately.

- [ ] M1: foreground daemon, bounded/owned IPC, timeouts, stale socket, ordering, signals.
- [ ] M1: Xorg capture, RGBA frame contract, webcam formats, loopback negotiation.
- [ ] M1: independent output privacy state, neutral frames, status, barriers.
- [ ] M2: layouts, masks, crop, mirror, anchor geometry, fit and source controls.
- [ ] M2: monitor/region/window selection, cancellation and safe source failures.
- [ ] M3: PipeWire mic/desktop routing, source selection/gains and virtual microphone.
- [ ] M3: recording worker, bounded queues, pause AV timeline and failure isolation.
- [ ] M3: safe filenames, countdown/cancel, finalization and partial-file recovery.
- [ ] M4: presets, zoom/follow, cursor/click/key overlays, input privacy and filtering.
- [ ] M4: lightweight preview and local state indicators, recursion limitations.
- [ ] M5: optional portal backend, consent/revocation/renegotiation, capability matrix.
- [ ] M5: complete CLI/config, atomic reload and effective composition reset.
- [ ] M5: documentation, man page, sxhkd, Makefile/install/Arch package/license.
- [ ] Verify: geometry, state/group pause, config, IPC, media timeline/barriers/failures.
- [ ] Verify: sanitizers, actual encoded synthetic frames/audio, compiler diagnostics.
- [ ] Hardware: Xorg input/layouts, webcam/loopback consumer, PipeWire, portal.
- [ ] Hardware: 1080p30 CPU/RSS/latency/drops and conferencing compatibility.
