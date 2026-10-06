# Cinematic motion

Cast Pro provides an opt-in cinematic screen transform. Existing Community zoom,
and Pro with `zoom.motion=legacy`, keep their existing settings and behavior.
The screen, synthetic cursor and click markers share one timestamped transform;
camera, captions, key labels and other composition layers remain independent.

Start a verified Pro daemon and apply a session setup:

```sh
cast-pro zoom cinematic
cast-pro zoom focus 0.65 0.35 --factor 2
cast-pro zoom status --json
```

The setup selects cinematic motion, 450 ms zoom settling, 400 ms pan settling,
a 35% follow region with 5% hysteresis, bilinear resampling and blur off. It does
not persist the configuration, start outputs, change capture or assume separately
controllable cursor metadata. Existing configs default to legacy. Use
`cast-pro zoom motion legacy` to return to free zoom without changing output state.

`zoom focus` accepts normalized selected-source coordinates from 0 to 1. Its
optional factor and point commit together. Focus stays locked until an explicit
`zoom follow on` or `zoom auto click`. Existing `zoom toggle|in|out|set|reset` and
`zoom follow on|off` remain available. Manual control suppresses click automation
for `manual_override_ms`; zero holds until explicit automatic control resumes.

```sh
cast-pro settings zoom.motion cinematic zoom.transition_ms 450 zoom.pan_settle_ms 400
cast-pro settings zoom.follow_region_percent 35 zoom.follow_hysteresis_percent 5
cast-pro settings cursor.smooth true cursor.settle_ms 60 cursor.max_lag_ms 80
cast-pro settings zoom.motion_blur true zoom.shutter_angle 120 zoom.blur_samples 4
cast-pro zoom auto click
```

The panel exposes motion, factor, follow and auto under Source & layout, cursor
controls under Annotations & pointer, and detailed motion controls under All
settings. Apply/Revert uses the same atomic validation as CLI settings.

| Setting | Default | Range or behavior |
|---|---|---|
| `zoom.motion` | `legacy` | `legacy`, `cinematic` |
| `zoom.follow_region_percent` | 35 | 5–90; viewport-relative safe region |
| `zoom.follow_hysteresis_percent` | 5 | 0–20; must nest inside the safe region |
| `zoom.pan_settle_ms` | 400 | 0–3000; zero is immediate |
| `zoom.focus_margin_percent` | 12 | 0–40 |
| `zoom.avoid_camera` | true | Actual camera rectangle; no reserved phantom camera |
| `zoom.auto` | `off` | `off`, `click`; actual selected-source input required |
| `zoom.auto_factor` | 2 | Existing zoom limits when automation is enabled |
| `zoom.auto_hold_ms` | 1600 | 0–10000 |
| `zoom.auto_cooldown_ms` | 700 | 0–10000 |
| `zoom.auto_idle_ms` | 2500 | 100–30000; must accommodate hold |
| `zoom.auto_cluster_radius_percent` | 12 | 0–50; selected-source coordinates |
| `zoom.manual_override_ms` | 5000 | 0–60000; zero requires explicit auto re-enable |
| `zoom.filter` | `bilinear` | `bilinear`, `bicubic` |
| `zoom.motion_blur` | false | Same eligible source frame; no retained source pixels |
| `zoom.shutter_angle` | 120 | 0–180 degrees |
| `zoom.blur_samples` | 4 | 2–8 requested; effective budget is reported |
| `cursor.smooth` | false | Requires separate cursor and metadata |
| `cursor.settle_ms` | 60 | 0–200 |
| `cursor.max_lag_ms` | 80 | 0–250; hard temporal history envelope |
| `cursor.hide_idle_ms` | 0 | Zero disables; otherwise 100–30000 |

In cinematic mode `zoom.transition_ms` is zoom settling time. The engine uses
analytic critically damped second-order trajectories in log scale and source
coordinates. A rest-to-target step reaches approximately 99% at the configured
settling time; interrupted motion carries its velocity into the next target.
Viewport edges constrain both position and outward velocity. Settled 1x is the
exact full frame. Existing `deadzone` and `smoothing` remain legacy controls.
Main-thread command timestamps are preserved, so the first capture after a
committed focus includes its elapsed motion time. Future observations wait for
an eligible source timestamp; late observations cannot rewind an already rendered
trajectory. Repeated rendering at one timestamp does not advance it again.

An explicit focus outside the visible viewport is reframed into view even when the
factor stays unchanged. Source edges can limit the requested readable margin; status
reports that limitation. The existing free `zoom follow` command rejects a full
cinematic event queue before changing configuration, and stays available when the
cinematic entitlement is absent.

Follow hysteresis avoids repeatedly moving the viewport for pointer noise. Click
automation clusters nearby selected-source clicks, respects cooldown/hold/idle
intervals, ignores filtered Cast UI and stays causal. During the configured hold,
far-away pointer movement does not reframe the viewport; the separately rendered
cursor remains responsive. Manual control cancels the hold. Camera avoidance chooses a
small legal viewport shift while preserving the focal point. If camera coverage
and source edges make that impossible, valid framing wins and status reports the
conflict; the camera itself stays in place.

The cursor limiter constrains displayed coordinates to actual cursor positions
from the most recent `max_lag_ms`. Once an input is older than that interval it
cannot keep the cursor behind the latest position. A click snaps the cursor to
the actual click for the current source interval; click markers always use actual
coordinates. Embedded cursors are never drawn a second time. Explicit focus works
without pointer metadata; follow, synthetic smoothing and automatic clicks require
their advertised platform capabilities.

Bilinear and Catmull–Rom bicubic filters use subpixel geometry, clamped source
edges and premultiplied alpha. They preserve Cast's existing encoded RGB color
semantics; they do not invent extra source resolution or perform linear-light
resampling. Blur evaluates bounded shutter transforms from the current eligible
frame only, caps the source interval at 50 ms, and uses one sample at rest.
Source generation/dimension changes reset to 1x. Privacy and appearance barriers
discard histories while preserving the active trajectory; cleared or paused
sources cannot resurrect older pixels. Losing entitlement hands the current
viewport to legacy motion without a position jump.

At 1080p above 30 fps the live adapter explicitly selects bilinear and one sample.
At 1080p at or below 30 fps it limits bicubic to one sample and bilinear to two.
`zoom status --json` exposes requested/effective filter and samples, evaluated
viewport, follow/focus/auto/manual state, capability/conflict limitations and
screen-render count/last/max time. These timings describe screen composition,
not capture or encoder throughput. The bounded offline engine implements the full
requested 2–8 samples independently of this live policy.

Reproduce deterministic tests, sanitizer checks, the composition benchmark and
the synthetic comparison using the private checkout:

```sh
python3 /absolute/path/cast-pro/pro/tests/run_motion.py \
  --core /absolute/path/cast --lgpl-root /absolute/path/controlled-media-sdk \
  --output /tmp/cinematic-verification --sanitize --benchmark --demo
```

The output contains `benchmark.json`, exact `commands.txt`, the event CSV and
configuration, seven stills and `demo-artifacts/side-by-side.mp4`. Its comparison
is Cast's smoothstep/exponential legacy reference against Cast's own cinematic
model on synthetic code-like text, grid, cursor noise, reversals, edge travel and
camera overlap. The demo encoder is an external development tool; it does not
change the Pro dependency profile. Representative stills were inspected for crisp
text, framing and camera independence. Live desktop/portal input and full video
playback remain host acceptance checks, and these results make no Screen Studio
parity claim.

The CPU benchmark used an Intel Core Ultra 5 235U, 1920×1080, 60 measured
frames per filter/sample case and a persistent pool of three workers plus caller.

| Filter / effective samples | p50 / p95 / p99 (ms) | Missed 30 / 60 fps deadlines |
|---|---|---|
| Bilinear / 1 | 9.852 / 11.489 / 11.913 | 0 / 0 |
| Bilinear / 4 | 23.369 / 26.292 / 27.523 | 0 / 60 |
| Bicubic / 1 | 23.075 / 26.780 / 27.410 | 0 / 60 |
| Bicubic / 4 | 68.849 / 72.944 / 73.427 | 60 / 60 |

The separate 600-frame analytic/shutter advance benchmark measured
142.212 / 260.634 / 289.776 microseconds at p50 / p95 / p99.
The benchmark pool reserved 2,703,856 heap bytes, two source/destination frames
used 16,588,800 bytes, and process peak RSS was 19,412 KiB. The pool measurement
excludes thread stack virtual reservations and allocator overhead. Detailed
results are recorded in the private verification artifact. The engine reserves
130,352 bytes; the application pool reserves
23,069,168 heap bytes for the supported 16,384-pixel width, without retained frame
buffers. Thread stacks are additional virtual reservations. Composition benchmarks
exclude capture, conversion, encoding and multiplication by output lanes; a
composition deadline result is not evidence of full 1080p60 output.

The versioned motion event/snapshot interface supports project integration without
implementing an editor. Store ordered source timestamps, generation/dimensions,
factor/focal/follow/auto events and validated parameters, then replay from a known
checkpoint for arbitrary seeking. Normalized source coordinates remain independent
of crop/fit and portrait output. Native checkpoints are trusted, same-build
in-memory state; a durable project format should serialize fields explicitly and
validate them, rather than persist compiler-layout bytes.
