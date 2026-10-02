# cast control panel

The user approved the panel sketch and a restrained dark native control panel.
This is an operating surface: state recognition and accurate controls take priority.

## Layout

Default window approximately 760 by 900 logical pixels; narrower windows stack
preview and primary actions. Live/record state and pause controls stay visible.
An actual-output preview sits beside live/record controls, followed by preset and
layout selection. Source, Camera, Audio, Effects and Settings tabs share a scrollable
settings area. Closing the panel leaves the daemon running.

## Tokens

- Canvas: `#111820`; settings surface: `#19232d`; controls: `#253440`.
- Primary text: `#edf3f7`; secondary text: `#adbfcb`; disabled: `#8195a3`.
- Focus/selection: `#80c9ff`; live: `#83ddb6`; recording: `#ff9693`;
  paused/warning: `#f1cd84`.
- Inter, bundled under SIL OFL. Body and controls approximately 15–16 px;
  section labels 18 px; state labels 14 px with tabular timing.
- Spacing units: 4, 8, 12, 16, 24 px. Controls at least 32 px tall.
- Small control radii around 6 px; avoid decorative gradients and shadows.

## Behavior

All displayed settings come from the daemon. Commands acknowledge success or show
their actionable error. Fields retain drafts while edited and apply explicitly;
sliders coalesce changes. Unsupported actions explain the backend limitation.
Tab and Shift+Tab traverse controls; focus is visible; Enter/Space activate actions.
Status uses words as well as colour. Disconnection clears the preview and tells the
user to start/reconnect to cast. Exclusion wording distinguishes application capture,
out-of-source placement, neutral masking and unsupported Wayland exclusion.

## Acceptance

Inspect normal, narrow and disconnected views on isolated displays. Exercise panel
controls against a synthetic daemon and verify CLI changes appear in the panel.
Inspect real captured pixels for Xorg exclusion and privacy transitions. No fake
recording, device availability or audio level is displayed as real state.
