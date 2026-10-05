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

The header has exactly two action rows: three equal 44px lane chips with fixed
state indicators and ellipsized text, then 30px Pause all/Resume, Preview and Close
buttons in 11.5px semibold Inter. Privacy and preview share equal widths; Close is
compact to preserve preview state text at 360px. The titlebar contains only the
window name.
The permanent capture-exclusion line and full-width Operate/Compose tabs remain
pinned. Connection state and acknowledged feedback live in a pinned 46px surface
status bar: two centered 16px rows of quiet 11px text, with composition deep links
above the connection label and marked, ellipsized feedback. Hovered summary links
shift to foreground with dotted underlines. The body is the only scrolling region.
An always-available square Close control beside Preview closes only the panel;
wheel scrolling advances 60 logical pixels per notch with fractional motion preserved.

Operate has four disclosures: Virtual camera, Recording, Streaming and Audio.
Selecting a header chip opens the corresponding lane. Primary actions name their
output. States distinguish run/connection lifecycle from paused/frozen/blurred
presentation. Streaming setup edits only a stream-key file path, never a secret;
Start connects privacy-paused and explicit Resume reveals content. Recording has
countdown Cancel and omitted-time Cut/Resume. Global privacy restoration never
reveals independently paused outputs or starts stopped ones.

Compose is a static seven-row section list: Source & layout, Camera, Background &
stage, Overlays, Annotations & pointer, Audio and Settings. Each row summarizes
live values and opens a dedicated page with a sticky Compose back button. Every
control is inline beneath visible group labels; no nested disclosures. List rows
that straddle the scroll edge are hidden as a whole. Current-section dirty edits
show a pinned Apply/Revert bar above the status bar. Apply and Enter submit a
validated atomic batch without changing page or scroll; Revert discards unsent
changes. Drafts survive navigation and failed commands. Alt+1…7 jump to sections,
1/2 switch tabs and Escape closes a sheet first, otherwise returning to the list.
Composition tokens deep-link to source/layout or background/stage; zoom has a reset
link. Each view remembers its scroll in session memory across panel close/reopen
against the same daemon. Navigation never writes config. Locked and unsupported
fields explain why they are disabled; config-only output fonts show reload guidance.
Source & layout includes an inline overlap mask-color control, applied through
the same draft bar. Guidance explains unobstructed capture through a selected
application window or moving controls outside the captured monitor/region;
unsupported backends disable the field.

The panel fills 360–520 logical pixels and centers at wider sizes. It uses the
existing Inter slots: 13 secondary/chips, 16 controls/body, 19 section headings,
25 lane headings and 48 countdown numerals. Compact secondary roles reuse Inter
at their actual 11px and 11.5px raster sizes. Density changes rebuild raster
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
