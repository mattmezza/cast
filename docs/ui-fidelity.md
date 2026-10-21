# Workspace fidelity audit

Reference: OpenDesign `cast-app.html`, `cast-app.css`, `cast-app.js` from project
`d3599a73-2edd-4d30-b822-4ea306ac120e`. These are reference inputs; this work does
not modify the OpenDesign project. The native implementation is `ui/app.c`,
`ui/settings.c`, `ui/menus.c`, `ui/brand.c`, `ui/cast_theme.c` and the SDL host in `ui/main.c`.

CUI is pinned to commit `9d4914830f29bcdd8b12517f21841fbdae17f631` in
`ui/.cui-lock.json`. The findings below describe that bundled API and this host,
not every CUI renderer or a newer upstream release.

| Reference detail | Available API and implementation evidence | Classification |
| --- | --- | --- |
| Square orange/ink controls, custom icon-and-text tab bodies | `cui_components.h` exposes button `content`, `leading_content`, `trailing_content`, explicit dimensions, custom colors and corners. `cui_choice.h` exposes `cui_tabs_options.render`, per-item parts and base tabs. `app.c` uses `workspace_tab_body` with branded SVG icons. | Application styling; CUI does not require stock tab/button artwork. |
| Brand marks and thin SVG icons | `cui_icon.h` and theme icon loaders accept application SVG. `brand.c` supplies the artwork and `app.c` retains `cui_icon_state`. | Application asset/render sizing responsibility; no requirement to substitute Unicode symbols. |
| Settings category rail and overlay | `cui_surface.h` supplies dialog/sheet state, bounds, close decisions and focus restoration. `settings.c` uses `cui_base_surface_begin`, custom portals and scroll regions for a 1020×720 desktop dialog, 38px header and 214px rail, with all ten reference categories. | Supported in-window composition. Appearance works locally; recording settings submit acknowledged edits, while other backend forms remain incomplete. Safe acknowledged values (backend, container, codec, frame rate and stream service) appear when connected; unavailable data is labelled accordingly. |
| File/Edit/View/Composition/Output/Help menus | `cui_choice.h` contains menu-tree state/options, anchored menu controls and base reducers; `menus.c` uses `cui_base_menu_tree` and `cui_menu_popup_paint` for all six menus with custom row content. | Implemented in-window menus support local panel/layout changes, Settings links and workspace Quit. Recording on/off and Cut are enabled with acknowledged state guards; remaining output/capture and unimplemented actions stay disabled. These menus remain in-window on macOS pending a native global-menu adapter. |
| Native title bar, detached Settings, macOS system menu | `cui_sdl3.h` provides `cui_sdl3_window_interface` and `cui_sdl3_set_decorated`. Its contract explicitly excludes native move/system-menu dispatch. `cui_sdl3.c` creates an ordinary resizable, high-density SDL window. `main.c` creates one host. | Host integration gap: the current in-content title bar does not replace OS decorations. Detached Settings and the macOS global application menu require additional native host integration. This is not a claim that additional SDL windows are impossible. |
| Inter plus IBM Plex Mono, medium/semibold weights and tracked uppercase labels | `cui_text.h` supports named family IDs, weights, style registration and shaping/measurement; `cui.h` also exposes `cui_text_style.letter_spacing`. `main.c` registers actual Inter regular/medium/semibold/bold and IBM Plex Mono regular/medium/semibold (family 1, monospace flag). Reference CSS specifies variable Inter, IBM Plex Mono, tabular numerals and `.085em` tracking for caps. | Font weights and the mono family are available in both standalone and embedded builds; application text styles select them. Font selection alone does not establish exact browser line wrapping or spacing. Different browser and FreeType/HarfBuzz rasterization also precludes assuming identical glyph pixels. |
| Blurred backdrop behind overlays | `cui.h` lists rectangle, border, text, clip, image, mesh, drop-shadow and inset-shadow draw commands; it exposes shadow blur sigma but no backdrop-filter command. `cui_sdl3.c` prepares shadow masks and composites their textures. | This pinned public draw list/SDL renderer has no backdrop-blur operation. A translucent overlay and blurred shadow do not blur already-rendered content behind a dialog. Matching browser `backdrop-filter` would require a host/renderer extension or an application-managed filtered image. |
| Composition preview, source cards, inspector and status | CUI boxes, images, custom drawing and independent scroll regions support the overall composition; the app already uses these for its stage/panels. | Remaining card content and visual detail are application scope. Live preview depends on the daemon bridge, not a widget limitation. |
| Recording/projects, streaming, prompter, presets and other dialogs | The HTML/JS contain more working demonstration controls and dialogs than the native workspace. The attached native mode reads acknowledged state and supports layout, fit, eight camera anchors and camera width edits; monitor/camera/audio selectors discover and select real devices. Look, basic zoom, recording lifecycle/settings and supported audio controls are connected. Streaming controls, editable projects, prompter and presets remain unfinished. | Functional implementation gaps; do not describe the current workspace as complete feature parity. |

The HTML reference has a desktop minimum width of 1020px and a desktop-window
wrapper with an outer shadow. The native app additionally supports narrow drawers.
Compare the desktop content at the same logical dimensions and theme; count OS
borders and the browser's outer desktop wrapper separately. Layout assertions and
successful interaction tests establish bounds/behavior, not pixel equality.

Headless reference captures can use the installed Chromium-compatible
`/opt/helium-browser-bin/chrome` and its adjacent `chromedriver`, without installing
Playwright or opening a desktop window. Capture the original local project so its
relative fonts/images resolve; copying only the three reference source files omits
those assets. Native captures use the private-Xvfb path documented in `ui/README.md`.

On this Linux run, `python3 ui/test_live.py ./cast` passed offline, attach, frames,
detach/re-attach and disconnect checks. Headless Chromium reference captures used
a CDP viewport of 1360×820 at device scale 1 after `document.fonts.status` reported
`loaded`, for the default workspace and opened Settings. This does not establish
macOS runtime or pixel-identical native rendering.

## Font provenance

`ui/assets/fonts/manifest.json` records the original and derived font hashes.
Inter regular/bold remain the original bundled 4.1 OTF files. Medium (500) and
semibold (600) are static instances of the reference project's Inter variable
WOFF2, whose internal version is `4.001;git-66647c0bb`; they are deliberately not
claimed to be identical to the bundled 4.1 outlines. IBM Plex Mono 400/500/600
are decompressed from the reference WOFF2 files without glyph/name changes.
Inter's existing OFL notice and the separate IBM Plex OFL notice ship beside the
assets. All seven faces are registered from files in the standalone preview and
from embedded bytes in `CUI=1` builds. Missing required fonts fail startup before
creating the SDL host.

The current result is a visual workspace with live composition editing. Layout,
contain/cover, eight camera anchors and camera width changes use acknowledged
daemon commands; pending or rejected edits do not change authoritative selections.
Settings category navigation and session appearance work. Monitor/camera selection uses the reference combo styling and acknowledged daemon state. Other
capture configuration, recording/streaming actions, project management and prompter
operation remain unfinished. Those gaps belong to application/service integration, not to
the custom-body, modal or menu capabilities demonstrated by the pinned toolkit.

## Settings control corrections

All settings sliders use the configurable `cui_slider_events` API with an explicit
track width inside the 160px value column. The legacy `cui_slider` wrapper fixes
its outer width at 260px; using that wrapper in a narrower column caused overflow.
The close button uses `icon_only` and a dedicated icon callback; the earlier rail
callback included a growing text slot and displaced the glyph. Both issues were
application composition mistakes; neither requires a toolkit patch. Regression
checks cover all seven sliders and close-icon centering at three window widths.

## Source and camera selectors

The local reference `cast-app.css` combo rules (lines 775–809) and `cast-app.js`
combo behavior (lines 855–893) define the 36px trigger, trailing chevron, 32px
rows, 4px popup padding/gap, selected check and 264px maximum height. The native
picker composes these parts from CUI overlay, list, text-ellipsis and icon APIs;
it does not substitute a platform dropdown. Real device data replaces the browser
demo names. Linux/macOS parsing and reconnect/failure handling are bridge concerns.
Native macOS runtime verification remains outstanding.

## Preview contention and Look controls

Preview pixels use a read-only shared-memory mapping, with socket messages for
attachment and control. A failed metadata `trylock` previously made the workspace
clear its frame, producing intermittent flashes. Snapshot reads now briefly
serialize with worker metadata updates; network connection and command waits run
outside that mutex. Frame-copy contention still skips an update. Disconnect,
generation and privacy invalidation continue to clear pixels. A concurrent-reader
regression test exercises 40,000 snapshot reads alongside the connection worker.

Menu custom bodies explicitly compose label and shortcut in a horizontal row;
regression checks compare their painted positions and vertical centres.

Look and basic Zoom controls use the existing compositor and acknowledged command
queue, including colour validation, gradient stop state and zoom capability bounds.
The reference's two-stop gradient must not erase an existing third stop; an extra
middle-stop control preserves that backend feature. Basic zoom remains free while
cinematic status follows the daemon's actual capability. These are application
integrations using pinned CUI APIs, without a toolkit patch.


## Recording and audio controls

Recording now uses real acknowledged start, countdown cancellation, pause, cut,
resume and stop commands. The inspector retains the reference card padding,
spacing, SVG headers, action rows and mono format values. Recording settings use
the same modal rail and bordered groups; numeric sliders use explicit track widths
inside the value column. Directory edits submit on Enter and reset across daemon
sessions. Menu recording actions use the same intent validation as buttons.

The reference mixer includes demonstration Music/Master channels and animated
meters. The daemon currently exposes microphone/desktop routing, on/off, gain and
readiness, with no peak telemetry or extra mixer buses. Native controls expose
those actual capabilities and do not fabricate live levels. On macOS the backend
supports only system-selected default audio sources and no virtual microphone.
These differences are application/backend capabilities, not CUI limitations.

Editable projects, autosaved cut lists, persistent defaults and hardware encoder
discovery remain unfinished. Frame rate changes require daemon restart. The
reference's encoders and formats are replaced with real backend configuration
values; incompatible or unavailable encoders report daemon errors.


The bundled IBM Plex Mono subset lacks U+221E (infinity). Its zero-gain `−∞ dB`
label initially made native rendering fail. The host now registers bundled Inter
as an explicit fallback through `cui_font_set_fallbacks`, which CUI already
supports. This was missing application font setup, not a Unicode limitation.
Toolkit diagnostic feedback: `CUI_TEXT_LAYOUT_MISSING_GLYPH` from the text layout
path becomes a generic false return from SDL rendering; the app then displayed a
stale SDL error, “That operation is not supported”. Surfacing the text-layout
status/codepoint would make this failure substantially easier to diagnose.
