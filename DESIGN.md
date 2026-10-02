---
name: cast control panel
description: Restrained dark native controls for cast output and settings
colors:
  background: "#111820"
  surface: "#19232d"
  control: "#253440"
  hovered: "#2f4352"
  foreground: "#edf3f7"
  secondary: "#adbfcb"
  muted: "#8195a3"
  accent: "#80c9ff"
  danger: "#ff9693"
  line: "#354756"
  selected: "#233f53"
  selected-border: "#3c6886"
  field-active: "#1f2b2e"
  live: "#83ddb6"
  preview: "#090d12"
  text-selection: "#33566f"
typography:
  title:
    fontFamily: "Inter"
    fontSize: "25px"
  body:
    fontFamily: "Inter"
    fontSize: "15px"
  label:
    fontFamily: "Inter"
    fontSize: "13px"
rounded:
  control: "6px"
  field: "5px"
spacing:
  xs: "4px"
  sm: "8px"
  row: "10px"
  md: "12px"
  lg: "16px"
  outer: "24px"
components:
  button:
    backgroundColor: "{colors.control}"
    textColor: "{colors.foreground}"
    typography: "{typography.body}"
    rounded: "{rounded.control}"
    padding: "8px 12px"
    height: "36px"
  button-hover:
    backgroundColor: "{colors.hovered}"
  button-selected:
    backgroundColor: "{colors.selected}"
    textColor: "{colors.accent}"
  button-disabled:
    backgroundColor: "{colors.control}"
    textColor: "{colors.muted}"
  field:
    backgroundColor: "{colors.background}"
    textColor: "{colors.foreground}"
    typography: "{typography.body}"
    rounded: "{rounded.field}"
    padding: "7px 10px"
    height: "34px"
  field-active:
    backgroundColor: "{colors.field-active}"
  settings:
    backgroundColor: "{colors.surface}"
    rounded: "{rounded.control}"
    padding: "12px 16px"
  preview:
    backgroundColor: "{colors.preview}"
    rounded: "{rounded.control}"
    height: "164px"
  preview-compact:
    height: "88px"
---

# Design System: cast control panel

## Overview

**Creative North Star: "Restrained dark native control panel"**

The approved direction is a restrained dark native control panel. This is an operating surface: state recognition and accurate controls take priority. Flat surfaces and compact controls keep output state, actions and settings easy to scan.

Inter supplies one consistent text voice. Colour identifies selection, keyboard focus and output state; status words carry the meaning alongside it.

**Key Characteristics:**

- Flat dark surfaces with small rounded corners.
- Compact controls and readable labels.
- Colour reserved for state, focus and selection.

## Colors

The palette uses cool dark surfaces, pale text and restrained state colours. The frontmatter records the implemented values from `src/panel.c`.

### Primary

- **Focus blue** (`accent`): keyboard focus, selected controls and the applying-command message.
- **Selection blue** (`selected`, `selected-border`, `text-selection`): selected button fill and border, and text selection.

### Secondary

- **Live green** (`live`): live output only when enabled, connected, unpaused and unfrozen.
- **Recording coral** (`danger`): recording or countdown status and actionable error messages.

### Neutral

- **Canvas** (`background`) and **settings surface** (`surface`): distinguish the window from its scrolling settings region.
- **Control** (`control`) and **hovered control** (`hovered`): button rest and enabled pointer-hover states.
- **Primary text** (`foreground`), **secondary text** (`secondary`) and **muted text** (`muted`): labels, supporting messages and unavailable controls.
- **Divider** (`line`): settings-row dividers, field outlines and dropdown outlines.
- **Active field** (`field-active`): text field being edited.
- **Preview well** (`preview`): background around aspect-ratio-preserving captured output.

Paused, frozen, stopped and disconnected status use secondary text; no separate warning colour is currently implemented.

## Typography

The native renderer uses bundled Inter through SDL3_ttf. Font metrics determine line height; the implementation does not set separate weight, tracking or tabular-number features.

- **Title:** application name, using the title token.
- **Body:** controls, field values and output status, using the body token.
- **Label:** supporting text, footer messages and restart hints, using the label token.

## Layout

The default window is (760 × 900 logical pixels), with a minimum of (540 × 620). The outer layout has horizontal padding (24px), top padding (16px) and bottom padding (12px). It runs vertically: status, output preview and actions, preset selection, composition layout, section tabs, scrolling settings and footer.

Below (680px) width, the preview and actions stack, the preview becomes (88px) tall, and the primary actions form two rows. At wider widths the preview is (164px) tall and sits beside a (224px) action column. Main vertical gaps shrink from (12px) to (8px) in the compact layout. Settings field labels shrink from (218px) to (174px); field rows remain (46px) tall.

Only the settings region scrolls. Footer messages wrap and the footer fits its content. The compact layout keeps live, recording and group-pause actions reachable at the minimum window size. Dropdowns anchor to their controls, reposition to fit the window and clip vertically when necessary.

## Elevation & Depth

The panel uses flat tonal layering and thin borders, with no shadows. Settings surfaces, controls and the preview well differ by fill; the dropdown overlays content with a surface fill and divider outline. State changes apply directly, without animated visual transitions.

## Shapes

Buttons, settings containers, preview and dropdowns share the control radius. Text fields use the slightly smaller field radius. One-pixel outlines mark focus and selection; one-pixel bottom dividers separate field rows. Dropdown chevrons are drawn geometry rather than font glyphs.

## Components

### Buttons

Compact text controls share one base treatment. Selected buttons use selection fill, focus-blue text and the selected border; enabled hover changes the base fill. Keyboard focus changes the outline to focus blue. Disabled buttons retain the base fill and use muted text. Buttons use the frontmatter height and padding.

### Inputs / Fields

Editable fields use canvas fill, divider outline and the field radius. Editing changes the fill to active-field colour; keyboard focus changes the outline to focus blue. Values retain a draft and the adjacent Apply button indicates when it can be submitted. Read-only settings show a Restart hint where applicable.

### Navigation

Source, Camera, Audio, Effects and Settings use the same selected-button treatment. Preview Live/Record selection and composition layout controls reuse it. Group disclosure controls use ordinary buttons with Show/Hide wording.

### Cards / Containers

The settings region uses the settings component tokens and vertical clipping. Multiline command results appear in a canvas-filled inset container. Containers remain flat; they do not imply clickable cards.

### Output preview and status

The preview preserves the frame aspect ratio inside its well. Disconnection replaces output with a daemon-start prompt; waiting for a frame has its own text placeholder. The status row reports live and recording state independently, with elapsed recording time when connected.

## Do's and Don'ts

### Do:

- Do pair state colour with explicit status words.
- Do keep output state and primary actions outside the scrolling settings area.
- Do use the existing button and field treatments for new controls.
- Do wrap footer messages and inspect the minimum window size.

### Don't:

- Don't introduce decorative gradients or shadows.
- Don't treat a preview placeholder as captured output.
- Don't describe paused or frozen live output as active live output.
