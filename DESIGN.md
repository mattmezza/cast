---
name: cast control panel
description: Crisp compact native controls with focused section navigation and a separate floating preview
colors:
  background: "#101113"
  surface: "#181a1d"
  control: "#272a2e"
  hovered: "#34383e"
  foreground: "#f8f9fb"
  secondary: "#bbc1ca"
  muted: "#979ea8"
  accent: "#80c9ff"
  danger: "#ff9693"
  line: "#444951"
  selected: "#233f53"
  selected-border: "#3c6886"
  live: "#83ddb6"
  text-selection: "#33566f"
typography:
  title:
    fontFamily: "Inter"
    fontSize: "25px"
  heading:
    fontFamily: "Inter"
    fontSize: "19px"
  body:
    fontFamily: "Inter"
    fontSize: "16px"
  label:
    fontFamily: "Inter"
    fontSize: "13px"
rounded:
  control: "0px"
  field: "0px"
spacing:
  xs: "4px"
  sm: "8px"
  row: "10px"
  md: "12px"
  outer: "16px"
components:
  button:
    backgroundColor: "{colors.control}"
    textColor: "{colors.foreground}"
    typography: "{typography.body}"
    rounded: "{rounded.control}"
    padding: "8px 12px"
    height: "40px"
  button-selected:
    backgroundColor: "{colors.selected}"
    textColor: "{colors.accent}"
  field:
    backgroundColor: "{colors.background}"
    textColor: "{colors.foreground}"
    typography: "{typography.body}"
    rounded: "{rounded.field}"
    height: "40px"
  navigation:
    backgroundColor: "{colors.background}"
    textColor: "{colors.foreground}"
    typography: "{typography.body}"
    height: "46px"
---

# Design System: Cast control panel

The visual authority is the approved round-three `cast-panel.html` prototype in the
OpenDesign project. This is an Operate surface: direct task actions first, essential
composition controls next, advanced controls one disclosure away. The existing dark
palette, bundled Inter family, square buttons and acknowledged daemon state remain.

The header is one 36px row containing Preview on/off, Close panel, Stop/Start daemon
and Quit, in that order. Text-only compact actions share the available width. It contains no lane chips. The permanent capture-exclusion notice and two equal
Operate/Compose tabs remain pinned. Only the body scrolls. Close always closes the
panel alone, including while disconnected or waiting for an acknowledgement. Quit
opens a centered, focus-trapped confirmation with Cancel selected. Confirmed shutdown
waits for the daemon to finalize recordings before closing the application.

Global privacy remains a 30px action at the top of the Operate body.
Operate consists of four always-expanded single-column cards: Virtual camera,
Recording, Streaming and Audio. Each lane has a 25px title, state dot and text,
quiet context, direct actions and one overflow menu. Starts remain privacy-safe:
virtual camera and streaming start paused and require explicit reveal. Recording
cut removes media time and resumes the same file after countdown. Failed streaming
keeps its reason and Retry. Audio source/gain controls appear only on the Audio card.
Recording configuration appears only in its overflow; six streaming connection fields
appear only in the dedicated setup view, which exposes a key-file path rather than a
secret. No browser demo inventory or state is treated as real.

Compose is a flat six-row list: Source & layout, Camera, Background & stage, Overlays,
Annotations & pointer, Pause & blur screens. Dedicated pages retain a pinned Back
header and per-page scroll. Essentials follow the prototype ordering; one All settings
toggle reveals the remaining fields inline. Disclosure never nests. Camera anchors
use eight selectable positions around a disabled center, with an honest layout note;
no Free/drag-in-preview promise is shown. Five drawn thumbnails represent layouts.
Backdrop chips switch the visible mode-specific fields. Annotation visibility uses
one output matrix rather than repeated per-lane fields.

Reusable controls share existing drafts and IPC: full-width 36px choice triggers,
scrollable 32px popup rows, 30px On/Off segments and chips, sliders with unit-bearing
values, swatch/hex/screen color pickers, focusable anchor grid and layout diagrams. Invalid
colors show a warning border/message. Unsupported controls and output locks explain
why they are unavailable. Output fonts remain separate from the panel typeface.
Independent controls apply on selection, slider release, or text-field completion.
All numeric controls pair practical slider ranges with precise numeric inputs. The pinned
draft bar retains Apply/Revert for coupled settings and invalid or failed edits.
Acknowledgement preserves later typing, page and scroll; all changes are session only.
Navigation never writes config.

The 46px status bar has two centered 16px rows of 11px Inter: composition and lane
links above connection state and marked command feedback. At narrow widths it drops
preset, zoom, then lane tokens; remaining links and feedback ellipsize. Hovered links
shift to foreground with dotted underlines. Alt+1…6 opens sections, 1/2 switches tabs,
Escape closes the active choice/menu/sheet before returning to the list, and keyboard
controls operate choices, sliders and spatial grids. Per-view navigation/scroll and
All settings state persist only in session memory for the same daemon socket session.

The panel has a centered single column from 360 to 520 logical pixels. Wheel input
advances 60 logical pixels per notch with fractional/natural motion retained. Inter's
existing 13/16/19/25/48 roles remain: secondary/body/group/title/countdown respectively;
compact status and semibold meta actions reuse the existing quiet raster roles.

CastPanel and CastPreview remain separate utility windows that float under the
existing mwm rule. Preview target pills select Virtual camera/Recording/Streaming.
Local state chrome sits outside the actual frame. Film-leader countdown and Cancel
work when only the daemon runs, then hide before file recording admission. No panel
preview is embedded and no media frame enters the panel renderer.

Documented production handoff differences and backend limits are in
`docs/control-panel.md`. Verification uses private Xvfb/synthetic media at minimum
and wide widths, pointer and keyboard interaction, and native density. Visual
inspection is one batched round, followed by at most one confirmation after fixes.
