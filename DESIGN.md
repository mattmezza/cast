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

# Design System: cast control panel

## Overview

**Creative North Star: "A compact remote control"**

The panel is an Operate surface used beside a presentation or call. Home provides current state, output actions and modes, and five section links. Opening a section replaces the navigation list with that section's controls. This direction replaces the former dashboard of simultaneous tabs, presets, layouts, and settings.

The visual character is crisp and quiet: neutral near-black backgrounds, clear white labels, flat controls and one font family. The separate preview window carries the visual interest. Colour signals state, selection and focus.

## Colors

Neutral canvas and controls replace the blue-grey layered dashboard. Primary text and supporting text remain high contrast. Focus blue marks keyboard focus and selected values; live green and recording coral accompany explicit state words. The separate CastPreview window preserves the output frame’s aspect ratio.

## Typography

Bundled Inter uses fixed logical sizes for titles, section headings, controls and supporting labels. Layout measures logical-size fonts. Separate raster fonts render at the actual display density; cached textures are rebuilt when density changes. Text lands on physical pixel boundaries and uses nearest texture sampling, avoiding enlargement of low-resolution glyphs. X11 content scaling and high-density window pixel scaling are both accounted for, including pointer coordinates.

## Layout

The default window is 480 × 760 logical pixels; the minimum is 360 × 640. Content stays within a 520-pixel-wide centered column in larger windows. State, floating-preview controls, relevant output actions and the capture-exclusion notice remain outside scrolling content.

Home begins with compact Live and Recording mode groups, followed by Source, Camera, Audio, Effects and Settings links. Freeze and blur are independent in each lane. Recording has solid pause and omitted-time cut controls. At short window heights the mode groups and navigation scroll without shrinking type. Source owns composition layouts and presets.

Section screens have Back and the section title, output state, floating-preview controls, essential output actions and a scrolling form. The panel never embeds, fetches, textures or drags an output image. The separate native CastPreview window has On/Off and Live/Recording target controls on every screen. Target selection reflects acknowledged daemon configuration and survives navigation. Unsupported backends disable the window toggle and explain the limitation. A physical wheel notch moves content 30 logical pixels; fractional touchpad deltas and natural-scroll direction remain intact.

Text and numeric field labels sit above full-width inputs. Boolean and enum controls share a row with their labels. Advanced controls use explicit Show/Hide disclosure buttons. Camera device selection is in its own disclosure; Appearance also controls the fitted camera's blurred, gradient or solid backdrop, its color, blur strength and brightness. Source disclosures contain Stage layer sizes, Screen appearance and the shared Background gradient. Effects disclosures contain Logo and Text overlay controls, including runtime editable static-text fonts. Settings has separate Pause screen and Blur screen disclosures for optional titles/subtitles/footers, template text, colors, sizes, text gap, blur radius and tint opacity. Pause and blur fonts remain configurable through the config file; each disclosure displays its selected font and reload guidance. Dirty fields reveal Apply; Enter also submits. Drafts survive navigation.

## Elevation & Depth

Flat fills and thin dividers provide structure. There are no decorative shadows or blurs. Dropdown choices float above the form. State changes are immediate and mirror acknowledged daemon state. The native window keeps the truthful CastPanel class and advertises the utility type before its first map; mwm floats and centers it using its existing utility rule.

## Shapes

Controls and fields have square corners. The Back action is a drawn left arrow and label on a plain surface; its hover and focus states match the other controls. All action and navigation icons use one authored 16-pixel line family at a consistent 1.4-pixel logical stroke; no icon depends on Unicode font coverage. Field and focus outlines are one logical pixel. Navigation is a plain list separated by thin horizontal lines, with a full outline for keyboard focus. Dropdown chevrons are native drawn geometry.

## Components

### Buttons

Action labels name the operation. Start recording is distinct from the Recording preview target. Live privacy and record Start/Stop/Cancel remain outside the scrolling form. Home holds independent freeze/blur controls and recording Pause/Cut/Resume. Pause writes a solid screen and silence; Cut removes media time and Resume continues the same file after the countdown. A resume countdown offers Cancel resume without finalizing the file. Hover and keyboard focus remain visible. Unavailable actions use muted text, while selections pair colour with text.

### Inputs / Fields

Each field has a persistent label, a visible focus outline, text selection and caret. Apply appears for edited drafts. Read-only settings state that a restart is required. Recording-locked fields explain why they cannot be edited.

### Navigation

Home lists five sections beneath output mode controls. Back returns Home, and Escape returns Home when it is not dismissing a dropdown or cancelling text editing. The scrolling section contains only relevant settings and advanced disclosures. Navigation does not alter outputs or preview target. Successful acknowledged record actions select the Recording preview; Resume live selects Live. Failed actions and replaced daemon generations preserve the selected preview. Manual preview selection takes precedence over a pending automatic change.

### Cards / Containers

There are no decorative cards. The section is a scrolling form on the window canvas. Native command results appear inline. Footer messages wrap to the available width.

### Output preview and status

The separate CastPreview window shows the acknowledged Live or Recording output. Live and recording state are independent. Active recording includes elapsed time. Visible mode labels follow precedence: paused overrides blur, which overrides freeze; record cut overrides all three while its media clock is stopped. Freeze/blur settings remain remembered underneath pause or cut. Countdown is a plain seconds-remaining status in the panel on every section. On Xorg the separate preview window temporarily displays a film leader even when preview was off, then hides before recording starts or resumes. The normal window toggle is locked and labeled Countdown preview while the countdown owns it. Unsupported backends retain the plain countdown status. Cancel, Stop record, Cancel resume and Pause all remain reachable outside scrolling content. The daemon's capture-exclusion statement remains visible on every screen; unavailable or neutral-masked exclusion is never implied to reconstruct covered content.

## Do's and Don'ts

- Keep one focused section visible and make advanced controls reachable through disclosure.
- Keep floating-preview controls, output state, privacy actions and exclusion information visible while navigating.
- Test separate preview-window actions and pointer/keyboard interaction at the minimum window width and native display scaling.
- Never stretch the interface into a wide dashboard or shrink text to fit more controls.
- Never rasterize text at logical resolution and scale the bitmap up for high-density output.
