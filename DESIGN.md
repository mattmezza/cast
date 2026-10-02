---
name: cast control panel
description: Crisp compact native controls with focused section navigation and a persistent preview
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
  preview: "#050607"
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
  control: "6px"
  field: "4px"
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
  preview:
    backgroundColor: "{colors.preview}"
  preview-compact:
    backgroundColor: "{colors.surface}"
    width: "208px"
    height: "146px"
---

# Design System: cast control panel

## Overview

**Creative North Star: "A compact remote control"**

The panel is an Operate surface used beside a presentation or call. Home provides actual output, current state, the next relevant output actions, and five section links. Opening a section replaces the navigation list with that section's controls. This direction replaces the former dashboard of simultaneous tabs, presets, layouts, and settings.

The visual character is crisp and quiet: neutral near-black backgrounds, clear white labels, flat controls and one font family. The preview carries the visual interest. Colour signals state, selection and focus.

## Colors

Neutral canvas and controls replace the blue-grey layered dashboard. Primary text and supporting text remain high contrast. Focus blue marks keyboard focus and selected values; live green and recording coral accompany explicit state words. The preview uses a near-black well and preserves the captured frame's aspect ratio.

## Typography

Bundled Inter uses fixed logical sizes for titles, section headings, controls and supporting labels. Layout measures logical-size fonts. Separate raster fonts render at the actual display density; cached textures are rebuilt when density changes. Text lands on physical pixel boundaries and uses nearest texture sampling, avoiding enlargement of low-resolution glyphs. X11 content scaling and high-density window pixel scaling are both accounted for, including pointer coordinates.

## Layout

The default window is 480 × 760 logical pixels; the minimum is 360 × 640. Content stays within a 520-pixel-wide centered column in larger windows. State, preview, relevant output actions and the capture-exclusion notice remain outside the scrolling section content.

Home uses a large preview followed by output controls and Source, Camera, Audio, Effects and Settings navigation rows. At short window heights, navigation scrolls rather than shrinking type or hiding controls. Source owns composition layouts and presets.

Section screens have Back and the section title, the persistent output state, a compact preview, output actions and a scrolling form. The compact preview starts in the upper right and can be dragged horizontally there or over settings content. Its movement is constrained so it cannot obscure the heading, Back, output actions or footer notice. Position and Live/Recording target survive section navigation. Home always restores the large preview.

Text and numeric field labels sit above full-width inputs. Boolean and enum controls share a row with their labels. Advanced controls use explicit Show/Hide disclosure buttons. Camera device selection is in its own disclosure. Dirty fields reveal Apply; Enter also submits. Drafts survive navigation.

## Elevation & Depth

Flat fills and thin dividers provide structure. There are no decorative shadows or blurs. The compact preview uses a single outline and sits above settings; dropdown choices sit above the preview. State changes are immediate and mirror acknowledged daemon state.

## Shapes

Controls have small rounded corners. Field and focus outlines are one logical pixel. Navigation is a plain list separated by thin horizontal lines, with a full outline for keyboard focus. Dropdown chevrons are native drawn geometry.

## Components

### Buttons

Action labels name the operation. Start record is distinct from the Recording preview target. Freeze appears when live output can freeze; recording pause and stop appear during recording. Hover and keyboard focus remain visible. Unavailable actions use muted text, while selections pair colour with text.

### Inputs / Fields

Each field has a persistent label, a visible focus outline, text selection and caret. Apply appears for edited drafts. Read-only settings state that a restart is required. Recording-locked fields explain why they cannot be edited.

### Navigation

Home lists five sections. Back returns Home, and Escape returns Home when it is not dismissing a dropdown or cancelling text editing. The scrolling section contains only relevant settings and advanced disclosures. Navigation does not alter outputs or preview target.

### Cards / Containers

There are no decorative cards. The section is a scrolling form on the window canvas. Native command results appear inline. Footer messages wrap to the available width.

### Output preview and status

Both preview sizes show actual daemon frames, with explicit disconnected and waiting states. Live and recording state are independent. Active recording includes elapsed time. The daemon's capture-exclusion statement remains visible on every screen; unavailable or neutral-masked exclusion is never implied to reconstruct covered content.

## Do's and Don'ts

- Keep one focused section visible and make advanced controls reachable through disclosure.
- Keep preview, output state, privacy actions and exclusion information visible while navigating.
- Test actual frames and pointer/keyboard interaction at the minimum window width and native display scaling.
- Never stretch the interface into a wide dashboard or shrink text to fit more controls.
- Never rasterize text at logical resolution and scale the bitmap up for high-density output.
