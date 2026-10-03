You are implementing a UI/UX redesign of Cast's control panel and floating preview. This is a frontend reorganization of existing functionality — **not** a rewrite of the daemon, and **not** a feature-complete streaming implementation (streaming doesn't exist yet; build its UI as specified, behind the current capability surface).

## Read first (in this order)

1. `~/dev/cast/DESIGN.md` — visual system, invariants
2. `~/dev/cast/src/panel.c` — current panel implementation (single file, ~2560 lines, Clay layout)
3. `~/dev/cast/src/panel_transport.h` — IPC contract: `PanelSnapshot`, `panel_client_command()`, `panel_client_setting()`, `panel_client_snapshot()`, `panel_client_frame()`
4. `~/dev/cast/src/cast.h` — `Config` fields, `State` (`live_paused/live_frozen/live_blurred`, `recording/record_cut/record_paused/...`, `group_paused`)
5. `~/dev/cast/streaming-prompt.md` — planned streaming behavior (terminology + state machine)
6. The design spec package (next section)

## Design spec (source of truth)

The redesign is fully specified in these files. They are prototypes + written spec, not production code:

- `design-proposal.html` — capability inventory, information architecture (Concept B: header lanes + Operate/Compose tabs), annotated wireframes, visual system, implementation handoff table
- `cast-panel.html` — interactive panel prototype at 360/440/520 px widths; contains the exact layout, copy, states, and every workflow (use the ⋯ Demo menu to see each state)
- `cast-preview.html` — floating preview prototype: target pills, lane-state decorations, countdown overlay

Match **structure, hierarchy, copy, and state logic** from these. Do not copy the browser CSS/JS literally — re-express in Clay elements and the existing color/font constants in `panel.c` (lines ~340-350: `background`, `surface`, `control`, `hovered`, `foreground`, `secondary`, `muted`, `accent`, `danger`, `line`).

## What to build

### Panel window

Replace the current Home-and-sections arrangement with:

1. **Pinned header** (never scrolls): three output lane chips — Virtual camera, Recording, Streaming — each showing run state with text label (not color alone); a contextual global action (`Pause all` / `Resume`, reflecting `group_paused` semantics with correct per-output restore); a `Preview` toggle; a one-line composition summary. Opening/closing the panel must never start/stop/pause anything (daemon-independence is already guaranteed by transport design — preserve it).
2. **Two tabs — Operate / Compose**:
   - **Operate**: per-lane cards with primary action + secondary actions (`pause/resume`, `freeze/unfreeze`, `blur on/off`, `stop`, `Cut time` for recording, full streaming connect/retry/stop flow with explicit paused-start). Streaming setup sheet exposes the **stream-key file path** only — never the secret, no OAuth.
   - **Compose**: current `FE/FT/FI` field entries regrouped by what they change (Layout, Camera, Screen, Background, Overlays, Presentation text, Annotations, Audio). Draft-vs-applied behavior: edits stage locally, explicit `Apply` commits via `panel_client_setting()`, `Revert` discards; show locked/unsupported states while any output is active.
3. **Feedback surfaces**: pending → acknowledged/failed on every command (`command_queued`/`command_completed`/`command_failed` from `PanelSnapshot`); a persistent banner when `connected == false`; `exclusion` notice surfaced as a permanent informational line, not a modal.
4. **Resize behavior**: fluid width 360–520+ logical px, flex-column layout, only the body scrolls, header pinned. Register `SDL_SetWindowMinimumSize` at 360. Must work as a tiled WM window (no drag-handle assumptions).

### Preview window

- Keep as separate floating window (existing pattern).
- Target pills: Virtual camera / Recording / Streaming — selecting a target changes what's displayed; pills are the only always-visible chrome.
- Lane-state decorations render **outside** the composition frame (corner labels, banner strip) so they can never enter outgoing video — mirror how `panel_transport_publish()` frames are already clean.
- Countdown overlay for recording start and cut-resume: full-frame, large numeral, `Cancel` button. Must work when only the daemon runs (temporary native countdown preview — reuse `platform_command` staging in `panel.c:1880` region, don't reinvent).
- Blur/pause states visibly transform the frame content itself (these ARE the outgoing video).

### State & semantics (do not break)

- `Pause` = solid background + optional text + silence. `Freeze` = held composition + silence. `Blur` = blurred composition + optional text + silence. Blur stacks above freeze; solid pause overrides both. `Resume` clears solid pause only.
- Recording cut omits media time; resume continues same file after countdown. Streaming has continuous media time — no cut control in its UI.
- Global resume must not start stopped outputs or reveal independently-paused ones (`group_live_restore`/`group_record_restore` exist for this).
- Streaming start enters solid pause; revealing requires explicit Resume. No confirmation dialogs — make the paused-start visually obvious instead (banner + button copy).
- Presentation state and connection state are separate concerns — never conflate "connected" with "viewers can see content".

## Implementation notes

- Everything routes through `panel_client_command()` / `panel_client_setting()` — no direct daemon calls, no new IPC unless a control genuinely has no backend (document these in the handoff).
- Keep the existing command ids/args exactly (`"record" "start"`, `"live" "pause"`, `"record" "cut"`, etc. — see `panel.c:755-843`) and the `FE/FT/FI` field-table pattern for Compose.
- Icons: extend the existing `ICON_*` switch (`panel.c:1313-1417`) with any new glyphs; pair every icon with a text label.
- Clay rendering: use only `Clay_*` element APIs already present; respect the render-command switch at `panel.c:1609+`.
- Persistence: session-only vs config-file changes — follow the existing `preview_target` and settings-save behavior; don't add new config writes without a deliberate decision.
- Never store or transmit the stream key secret; path only.

## Typography

The typeface is fixed: the app bundles **Inter** as its only UI font (`cast_panel_font_data`, `panel.c:2266`). Do not add font files, do not load system fonts, do not introduce a second family.

Bind every text element to one of the five existing font slots — `font_sizes[] = {13, 16, 19, 25, 48}` (`panel.c:351`) — by role:

| fontId | size | role in the redesign |
|---|---|---|
| 4 | 48 | countdown numerals in the preview, full-screen states |
| 3 | 25 | panel title, lane card titles |
| 2 | 19 | section headers, sheet titles |
| 1 | 16 | default: body text, control labels, buttons, inputs |
| 0 | 13 | secondary: meta info, status detail, helper text, chips |

Rules:
- Hierarchy comes from size + weight of the same family, never from mixing families.
- Numerals in status chips/counters use the same Inter slots (no mono font is bundled); keep digit strings short so they don't jitter layout.
- Pause-screen and blur-screen overlay text (title/subtitle/footer) use the **config-file fonts** (`pause_font`, `blur_font`) — that's the user's choice, not a panel font; the panel only displays their names with reload guidance (existing behavior, `panel.c:1101-1106`).
- Raster text is re-rendered per density; never cache text textures across density changes (existing `TextCache` handles this — reuse it).

## Constraints

- C, Clay/SDL3, same build system. No new dependencies.
- Do not modify daemon semantics, privacy guarantees, or `PANEL_PROTOCOL_VERSION`.
- All new user-visible text matches the prototype copy verbatim where given.
- Accessibility of states: never color-only; every state has a text label.
- Work in small commits; each commit should leave `make` and existing tests passing (`tests/`).

## Verification checklist before declaring done

1. `make` clean build, zero warnings.
2. Panel opens/closes with zero output state change (verify via daemon logs).
3. Every command button shows pending → ack/failed transition on a slow/laggy daemon simulation.
4. Disconnect daemon mid-session → banner appears, controls disable, reconnect restores.
5. All 9 workflows from the Demo menu work end-to-end against the real daemon (virtual-camera call join, record start/finish, cut/resume, stream setup→paused connect→reveal, failed connect→retry, three outputs with one-lane blur, global pause→correct restore, restyle Stage/background/logo/text, preview-hidden & panel-closed operation).
6. Resize 360→800 wide: no clipping, no overlap, header pinned.
7. Preview countdown visible at both small and large window sizes; Cancel works; window disappears fully before recording admission.
8. Contrast: verify foreground/background pairs for every state (default/hover/disabled/selected) meet 4.5:1 for text.
