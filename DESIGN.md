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

The visual authority is the supplied panel and preview prototypes: a compact
three-output remote control with a pinned lane header and **Operate / Compose**
tabs. Operate and Compose are native Operate surfaces. They share neutral dark
colours, bundled Inter, square controls, authored line icons, visible focus and
acknowledged daemon state. This replaces the former Home-and-five-sections layout.

The header contains Cast/panel and connection status, three equal lane chips with
text state, contextual Pause all/Resume, Preview on/off, composition summary and a
permanent capture-exclusion line. It never scrolls. A disconnected banner displays
last-known state and explains disabled commands and automatic reconnect. Tabs are
plain full-width text with an accent underline. The only scrolling region is the
body, above a pinned pending/acknowledged/failed feedback line.

Operate has four disclosures: Virtual camera, Recording, Streaming and Audio.
Selecting a header chip opens the corresponding lane. Primary actions name their
output. States distinguish run/connection lifecycle from paused/frozen/blurred
presentation. Streaming setup edits only a stream-key file path, never a secret;
Start connects privacy-paused and explicit Resume reveals content. Recording has
countdown Cancel and omitted-time Cut/Resume. Global privacy restoration never
reveals independently paused outputs or starts stopped ones.

Compose groups Source & layout, Camera, Background & stage, Overlays, Annotations &
pointer, Audio and Settings. Advanced groups reveal controls progressively. Local
drafts include booleans/enums; Apply submits a validated atomic batch and Revert
discards unsent changes. Enter submits the focused field. Drafts survive navigation
and failed commands. Locked and unsupported fields explain why they are disabled.
Config-only output fonts display their selected names with reload guidance.

The panel fills 360–520 logical pixels and centers at wider sizes. It uses the
specified Inter slots: 13 secondary/chips, 16 controls/body, 19 section headings,
25 title/lane headings and 48 countdown numerals. Density changes rebuild raster
fonts and text textures. Controls retain square corners, consistent spacing and
visible pointer/keyboard states. No second UI font or system-font dependency is
introduced. Every status has text; colour is supplementary.

CastPanel and CastPreview remain separate utility windows that float under the
existing mwm rule. Preview target pills select Virtual camera/Recording/Streaming.
Local state chrome sits outside the actual frame. Film-leader countdown and Cancel
work when only the daemon runs, then hide before file recording admission. No panel
preview is embedded and no media frame enters the panel renderer.

Documented production handoff differences and backend limits are in
`docs/control-panel.md`. Verification uses private Xvfb/synthetic media at minimum
and wide widths, pointer and keyboard interaction, and native density. Visual
inspection is one batched round, followed by at most one confirmation after fixes.
