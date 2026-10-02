# cast

<!-- impeccable:product-schema 1 -->

## Platform

Native Linux desktop. Arch Linux on Xorg is the primary target; optional Wayland
capture and panel support must report their actual capabilities.

## Stack

C, GNU Make, V4L2, PipeWire and FFmpeg. The user approved an optional Clay/SDL3
control panel with SDL3_ttf text rendering, launched from the same cast executable.

## Users and purpose

The user presents screen content with a webcam in conferencing calls and records
presentations. They use a tiling window manager and sxhkd shortcuts. The panel
provides visible controls for the same daemon already controlled through the CLI.

## Operating context

The daemon runs independently of its control clients. Live output starts paused.
Live and recording have independent state machines; recording pause resumes the
same file. Composition changes never implicitly resume either output. The panel
must remain stable while live, paused or frozen, and closing it leaves cast running.

## Capabilities and constraints

The panel uses a compact Home screen and reachable Source, Camera, Audio, Effects
and Settings screens. Reveal settings progressively instead of displaying the
whole control surface at once. Actual output preview remains visible on every
screen, becomes compact while editing, and can be moved out of the way. Output
state and privacy actions remain easy to reach. Render crisp text at native display
density, with clear contrast and little wasted screen space.
Camera mirroring defaults on and affects only the webcam. A custom paused message
must be editable through both CLI and panel. Runtime changes affect the session;
the program never automatically writes user configuration.

Controls use acknowledged user-only socket commands and daemon state. Slow panel
rendering must not stall capture. Xorg monitor/region exclusion masks overlapping
panel pixels with neutral colour; hidden content cannot be reconstructed. Selected
application capture excludes the separate panel naturally. Wayland exclusion must
not be promised without compositor support.

## Evidence

The build brief is `cast-build-prompt.md`; current implementation and verification
are documented under `docs/`. The user initially approved an ASCII panel sketch,
then rejected the implemented all-at-once layout after using it. Their current
direction is mobile-style focused navigation with an always-visible movable
preview and crisp typography.

## Product principles

- Keep the daemon authoritative and CLI/key bindings fully usable.
- Make output state and privacy actions obvious.
- Keep dependencies optional and code understandable.
- Report limitations and errors with actionable wording.

## Accessibility

Provide visible keyboard focus and keyboard operation for panel controls. Custom
drawn controls do not yet promise screen-reader integration.
