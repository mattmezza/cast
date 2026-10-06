# Local edition split verification

## Owner-host acceptance on 2026-10-06

The permitted host subsequently passed the complete Community `make check`,
edition contracts, Xorg/preview/countdown checks and native panel workflows,
including capture exclusion and application lifecycle. Bash/Zsh pass; Fish remains
unavailable on this host. This supersedes the sandbox-only desktop/network
limitations below, without qualifying a production Pro distribution.

The private signed-fixture build passed cinematic integration, Notes native
sanitizers, real Whisper inference/silence, decoded-audio SRT/VTT export, actual
daemon license mutations and signed update installation. The recorded English
Whisper fixture took 2,519ms of inference; its model-readiness, recording/silence
and shutdown session took 2.681s wall time, with 248,240KiB helper peak RSS.
The 60s cinematic 1920×1080 recording submitted and encoded 1,799 frames, with
zero reported drops and 24.642ms mean composition time. The file fully decoded;
timestamp passthrough avoids rescaling variable frame timestamps onto the null
muxer's default frame-rate grid.

Authenticated Notes/Xvfb acceptance passes spoof rejection, monitor-source
masking, continuous movement, synthetic WM decoration, closing-frame retention,
Freeze preservation and explicit exclusion opt-in. Opt-in discards an exposed
held snapshot and shows the whole neutral pause tile while Freeze stays active;
the fixture checks every replacement pixel and a changed live desktop to reject
accidental recapture. Panel processes are tracked for their window lifetimes.
Recording privacy sanitizer acceptance separately verifies decoded silence,
omitted cut time, absence of forbidden audio and SRT/VTT content, and mode0600.

These use isolated synthetic desktop/network fixtures and a recorded speech
sample. They do not establish real microphone, long-talk or multiple-accent
accuracy, GPU acceleration, Wayland compositor exclusion, commercial production
keys, or controlled dependency-source qualification. No daemon behavior changed
to repair the two Notes acceptance-fixture errors.

The initial 0.9.0 evidence below is retained as history. The first 0.10.0 follow-up
records the resolved baseline 1080p throughput failure. The final section records
the later three-workflow implementation; earlier identities, inventories and
scaffold counts do not describe that expanded build.

This is an engineering record from the 2026-10-05 working tree. It is not a
production release, legal clearance or desktop/hardware acceptance.

Community uses public sources alone and system media. Local Pro builds use
`PRO_ROOT=/tmp/cast-pro-private`, explicit nonproduction test trust
`PRO_TRUST_HEADER=/tmp/cast-pro-private/tests/generated/trusted_keys.h`,
`MEDIA_PROFILE=lgpl` and
`LGPL_ROOT=/home/matteo/dev/cast/build/deps-lgpl-candidate`. The candidate tree uses
verified cached FFmpeg 9.0.2 source with installed OpenH264 2.6.0, OpenSSL 3.6.5
and zlib 1.3.2. Its `release_eligible:false` manifest explicitly prevents official
release. The production recipe pins OpenSSL 3.5.4 and zlib 1.3.1; source caches for
those and pinned OpenH264 were unavailable. The candidate software baseline failed
the sustained 1080p30 throughput/drop target; this is an unresolved encoder/performance
acceptance issue. No GPL software baseline was restored.

Observed checks:

| Check | Result |
|---|---|
| Edition validation/public-only dry-run/archive and unknown-license policy | 7 tests pass |
| Community secure store and MIT fake-provider API/schema/gate/grant contract | Both C suites pass |
| License worker queue/replies, physical-store ownership, mutation acknowledgements and path changes | Pass without socket binding |
| Existing Community installer/embedded update behavior | 13 tests pass |
| Private-signature public updater: platform/hash/domain/entitlement/lock/rollback/symlink | 10 tests pass |
| Actual Pro CLI local import/reload/remove and invalid replacement preservation | Pass |
| Actual Pro signed binary bundle verify/install/execute identity | Pass |
| Community split CLI/config migration/completions | 18 pass; fish and Pro-only loader test skipped |
| Pro split CLI/config migration/completions and injected system GPL rejection | 19 pass; fish unavailable skip |
| Embedded help/installer/completion relocation checks | Pass for both final binaries |
| Actual Pro synthetic daemon licensing mutation via Unix IPC | Skipped: sandbox denies `bind` with `Operation not permitted` |
| Pro headless build using candidate media | Pass |
| Pro full X11 + Wayland + SDL3/SDL3_ttf panel build | Pass |
| Candidate sustained 720p30 software recording, 60 seconds | Pass: 1799/1800 frames encoded, 1 dropped |
| Candidate sustained 1080p30 software recording, 60 seconds | Fail: 1435/1800 frames encoded; unresolved throughput/drop target |
| Community full native core/visual/commands/panel route suites | Pass on final build identity |
| Pro full native core/commands/panel route suites | Pass on final build identity with OpenH264 |
| Public edition session/config/dispatch/lifecycle sanitizers | ASan/UBSan pass; LeakSanitizer unavailable in sandbox |
| Private verifier/issuer/session sanitizers | ASan/UBSan pass; LeakSanitizer unavailable in sandbox |
| Side-by-side install IDs, MIT/private notices, relative media resolution and independent uninstall | Pass |
| Actual headless Pro resolved dependency inventory | 25 shared libraries; no unresolved chosen copyright license |
| Actual native Pro resolved dependency inventory | 48 shared libraries; Graphite2 LGPL alternative selected explicitly |
| Local native Pro package with notices/inventory/public-source boundary | Pass; visibly nonproduction |
| Production build with local test trust | Rejected before compilation |
| Official profile with candidate media provenance | Rejected before compilation |
| Public source archive contains private provider/issuer/trust/key artifacts | Rejected by guard; generated archive clean |

Run targeted checks again with explicit private/nonproduction inputs:

```sh
make check-editions
make check-split-cli
python3 tests/test_install.py
make check-update-offline PRO_ROOT=/tmp/cast-pro-private
python3 tests/test_pro_cli.py --binary cast-pro --pro-root /tmp/cast-pro-private
make check-edition-install PRO_ROOT=/tmp/cast-pro-private LGPL_ROOT=/home/matteo/dev/cast/build/deps-lgpl-candidate
```

The candidate matching FFmpeg source kit includes the verified original 9.0.2 archive,
actual configuration/build/license materials and an explicit nonrelease README; the
installed external dependency provenance remains incomplete.

The final full native engineering binaries were built with `-Werror`, X11, Wayland
and the SDL3 panel enabled. Their build identities are Community
`4959f35f9b12ab5ecb5e7daeaee91926` and Pro
`9d44a13ff2b7b1b491ca45910235de11`; both report `official:false` and release time zero.
The recorded public revision is `fff72db87982bca3935817c1b8043c84d49efe2d` with working-tree
changes; the private scaffold is labelled `local-unversioned`. These are engineering
identities, not claimed commits of the completed work.

Local artifacts are `dist/cast-0.9.0-linux-x86_64-system.tar.gz`,
`dist/cast-pro-0.9.0-linux-x86_64-lgpl.tar.gz`,
`dist/cast-pro-0.9.0-linux-x86_64-lgpl-dependency-sources.tar.gz` and
`dist/cast-0.9.0-source.tar.gz`, with edition-qualified checksum files. The dependency
source kit hash is verified against the media profile before staging. The Pro staged
inventory records 48 resolved shared libraries and their selected copyright licenses;
Community records the complete 125-library system closure with distribution declarations
and explicit unresolved selections where a package alternative needs review.

The public archive guard excludes private files even when ignored and normalizes archive
timestamps/ownership so both edition packages can share a stable public-source checksum.
Preserve `/tmp/cast-pro-private` through a private-only archive or repository; public
source archives cannot recover the private verifier/issuer. Its source manifest records
all private source hashes and labels generated test keys separately as nonproduction.

The private verifier/issuer fixture generator has additional malformed-envelope,
time, key rotation and store tests documented in the private tree. Media encode/
decode, sustained packet count/drop/AV sync and streaming/TLS checks are recorded
separately by the media implementation; a successful dependency profile does not
substitute for those tests.

The dynamic SDL/PipeWire/plugin/graphics-driver closure needs validation in a clean,
permitted runtime image. Unix-socket IPC/desktop/panel integration must run outside
this socket-denying sandbox. No claim of completed macOS/Windows/WASM ports, hosted
Pro release discovery, storefront, patent/SDK review or production signing exists.
Exact source-built controlled dependencies, clean-runtime evidence, production license/
update keys, authenticated owner release metadata, entity/EULA review and exact patent/
SDK distribution review remain production inputs. The Git metadata directory is
read-only in this session; no commits, tags, remotes or releases were created.

## 0.10.0 follow-up

The user's actual `~/.config/cast/cast.conf` now contains the five missing schema
entries: `record.bitrate_kbps=6000`, `record.rate_control=auto`,
`stream.video_encoder=auto`, and empty `licensing.file`/`licensing.upgrade_url`.
The old explicit `record.video_codec=libx264` was migrated to `auto`; Community
still initializes x264, while Pro initializes the compatible OpenH264 baseline.
All other values and presets were preserved. Stray text after the `[screen]`
header was removed. The user ran the hash-checked atomic update helper outside
the sandbox; the root agent verified the installed hash, backup and config check.

The Pro code is arranged as a private monorepo at `/tmp/cast-pro`, with `pro/`
holding the provider, bounded verifier, issuer, tests and commercial notices,
and `server/README.md` reserving space for future Go services. No web server,
payment system or website was implemented. That initial scaffold had 15 reviewed
source files, excluding generated fixture keys and binaries; this historical count
precedes the three-workflow implementation. The default compiled trust set remains
empty. `PRO_ROOT` supports this monorepo and the original flat provider checkout.
The host publisher verifies the intended private GitHub repository and preserves
the owner's configured commit signing. It copies to `~/dev/cast-pro` before
publication; retry handling does not overwrite a different existing checkout.

The original 1080p failure was single-thread RGBA conversion in the no-x86-assembly
candidate, followed by OpenH264 encoding, exceeding the 33.33 ms frame budget.
Supported four-thread FFmpeg conversion retains dimensions, filter and colors,
with exact Y/U/V comparisons against the original path. The real 60-second
1920x1080/30 recording now encodes **1800/1800 frames with zero drops**, averaging
13.331 ms conversion plus 9.856 ms encode/mux at the unchanged 6000 kbit/s target.
Independent decoding confirms all 1800 frames remain 1920x1080.

The separate 90-frame decoded-detail test passes full-resolution dimensions,
fixed luma/chroma PSNR, fine-stripe contrast and text-error thresholds. Its
deliberate 720p-to-1080p negative control fails those thresholds. LGPL and system
media regressions and ASan/UBSan pass; LeakSanitizer remains unavailable. GPU
device nodes are not exposed, so hardware encoding is not claimed. Full results
and exact limits are in [media-profile.md](media-profile.md).

The full native Community build passes `-Werror`, core, visual, production command,
edition/store/worker/contract, CLI and SDL dummy panel-route checks. The installer
passes all 13 tests; embedded help and Bash/Zsh completion tests pass. Fish remains
unavailable. The existing Wayland color-picker fixture cannot start its private
D-Bus socket in this sandbox and was interrupted; it is not recorded as a pass.
Socket/desktop/network checks need the permitted host or release CI environment.

Current public source archives exclude every local prompt file, in addition to
private sources and keys; an independent inventory and extracted public-only
build dry-run pass. The original prompts remain on disk. Actual Git untracking
and local `.git/info/exclude` changes require the host completion script because
the sandbox's public `.git` is read-only.

The fully static dependency audit found 81 missing Community and 36 missing Pro
static archives with all native backends/panel selected. It verified that Pro
uses the controlled SDK rather than system GPL encoders. No fully static binary
was produced. [static-delivery.md](static-delivery.md) records the runtime services,
data resources and Pro relinking materials that static linking cannot bypass.

0.10.0 release preparation is local. The user created `mattmezza/cast-pro`; this
session's `gh` cannot connect to GitHub. No remote publication is inferred from
local builds or archive checks. The host scripts verify account/repository state,
commit signing, source hashes, tags and release CI before reporting publication.


## Three-workflow implementation verification

The later 0.10.0 working tree uses extension API 2 and the private monorepo layout
`/tmp/cast-pro/pro`. Cinematic zoom, local transcription/subtitles and speech notes
are implemented private modules; editable projects remain planned. Community has
public metadata and generic host interfaces, without those private algorithms or
speech SDK. Generated signed test fixtures are explicit nonproduction inputs and
remain excluded from source delivery. These checks do not establish a published
release or a final binary identity.

Cinematic tests pass deterministic 24/30/60/120 Hz and irregular-cadence motion,
retargeting/reversal, timestamp ordering, source resets, focus/follow/click/camera
constraints, cursor lag bounds, filtering/alpha and same-frame privacy. Real public
compositor/private adapter tests pass immutable output lanes, seeded legacy handoff,
rounded clipping, cursor/click geometry, bounded event queues and appearance-clear
compatibility. Both suites pass ASan/UBSan. The actual complete Pro host fixture
also passes atomic activation, first-frame elapsed motion, immutable lanes, manual
notes, privacy and revocation under ASan/UBSan. Preserving host commit timestamps
fixed the observed one-frame focus start delay; the host shutdown barrier now runs
only on the first stop, avoiding the diagnosed repeated-stop use-after-free.

The final composition-only benchmark on an Intel Core Ultra 5 235U measures
1920×1080, 60 frames per case, with three persistent workers plus caller:

| Filter / samples | p50 / p95 / p99 (ms) | Missed 30 / 60 fps deadlines |
|---|---|---|
| Bilinear / 1 | 9.852 / 11.489 / 11.913 | 0 / 0 |
| Bilinear / 4 | 23.369 / 26.292 / 27.523 | 0 / 60 |
| Bicubic / 1 | 23.075 / 26.780 / 27.410 | 0 / 60 |
| Bicubic / 4 | 68.849 / 72.944 / 73.427 | 60 / 60 |

Analytic/shutter advance measures 142.212 / 260.634 / 289.776 µs at p50/p95/p99.
Engine storage is 130,352 bytes; the 1920-pixel benchmark pool uses 2,703,856 heap
bytes and the application pool supports 16,384-pixel width using 23,069,168 bytes.
Thread stacks and caller-owned frames are additional. Benchmark peak RSS is
19,412 KiB. The live adapter reports requested/effective quality and limits 1080p
above 30 fps to bilinear/one sample, and at or below 30 fps to bicubic/one or
bilinear/two. These measurements exclude capture, conversion, encoding and output
lane multiplication. They do not prove full 1080p60 output or a combined 60-second
cinematic recording. That fresh-build acceptance is recorded separately when run.

`/tmp/cinematic-final` preserves exact reproduction commands, benchmark JSON,
event CSV/parameters, seven stills and `demo-artifacts/side-by-side.mp4`.
Representative stills were inspected for text, edge travel and independent camera
placement. Full video playback, real source/cursor backends and desktop acceptance
remain host checks. Details and reproduction are in [cinematic-motion.md](cinematic-motion.md).

Notes parser, timed cadence, weighted/ambiguous following, epoch/consent retirement,
bounded status, runtime commands and public panel metadata tests pass. Native SDL
software-dummy tests use actual SDL3/TTF/Noto rendering, reflow and independent
close; they do not prove authenticated Xorg exclusion. Recorded CPU-model checks
include zero-word-error following and independent-evidence reacquisition, with
silence/duplicate/manual holds. The refreshed helper also passes the Notes
transcribe-only override while the shared task is Translate: score 1.000,
zero-word error, 2,954 ms inference and 68 µs alignment. These bounded recorded
fixtures do not establish live-mic, long-talk or multiple-accent accuracy.
Decoded recording privacy tests pass normal and ASan/UBSan runs: forbidden PCM is
absent, cuts remove more than 0.5 seconds, SRT/VTT avoid hidden intervals and output
permissions are 0600. [notes.md](notes.md) records the narrower fixture scope and
unrun real Xvfb movement/decoration/Freeze/close acceptance.

The controlled offline STT recipe pins whisper.cpp 1.9.4 commit
`927cfce34f31707e17f2bff35c349632fb9e2c3a`, with bundled ggml 0.23.0 and a generic
CPU-only static SDK. No ordinary build downloads sources or models. Ten profile
and builder guard tests pass, including changed hashes/options/source kits and
review qualification. The exact source cache is unavailable, so the controlled
source build, source inventory and model acceptance remain unrun. Current signed
application/helper tests use an explicitly guarded installed engineering SDK
(whisper 1.9.4-dev, external ggml 0.25.3 and a named CPU backend); its profile is
ineligible for official release. Qualification requires actual owner acceptance
files bound to the source-built SDK identity, beyond the structural test fixtures.
See [speech-profile.md](speech-profile.md) for the recipe and release gates.

Current Pro CLI checks pass five executed tests, including signed license
replacement preservation, signed offline bundle install/rollback, dormant schema
compatibility and Bash/Zsh workflow completions. Two checks explicitly skip here:
daemon Unix-socket admission and absent optional Fish. Final rebuilt binaries must
repeat embedded help/completion checks. The owner-host workflow uses full native
Community flags and real private Notes/privacy test parsers; required tools are
available, but Unix/X11 and local TCP listeners need a permitted host. Fresh full
build identities, aggregate acceptance and publication status are deliberately
not inferred from these focused results.


### Final source checks on 2026-10-06

The final native Pro engineering identity is
`9723f054f1c8cafb0f4390722c2372fd` (public base
`fff72db87982bca3935817c1b8043c84d49efe2d`, API 2; private staged source is
uncommitted). Both native backends, Clay/SDL panel and explicit CPU STT are enabled.
The final Community identity is `39a655309f8f70eea67b7dd7df8eb5e0`; both binaries'
three embedded completion scripts exactly match their source bytes. The final Pro
CLI suite passes five tests with two explicit environment skips.

The actual licensed Pro host passes atomic motion activation, immutable lanes,
manual Notes, privacy, revocation and saturated follow-queue rollback. An explicit
`zoom follow on` releases focus even when follow was already enabled; the existing
basic follow setting remains available without the cinematic entitlement. Offscreen
focus and click-hold tests pass in the engine and real compositor under ASan/UBSan.
The updated camera-avoidance demonstration was regenerated and inspected; renderer
algorithms were unchanged, so no new composition benchmark is claimed.

Fresh combined recording acceptance passes the complete host fixture at
1920×1080/30 fps and 6000 kbit/s: 1,800 submitted, 1,799 encoded, one dropped,
60.021 seconds, 24.214 ms mean cinematic composition. Its strict acceptance requires
at least 1,790 submitted/encoded and at most ten drops. Independent full decoding
reports all 1,799 frames at 1920×1080 with no decode errors. This is cinematic
composition plus LGPL recording on synthetic source pixels; it does not establish
1080p60, GPU encoding, real capture performance or simultaneous speech throughput.

Final Notes native sanitizer checks include seven cases: parser/alignment, runtime,
Markdown styled raster/layout, actual glyph fallback, bounded status, public panel
metadata and native interactions. Drag scrolling suspends motion; four ambiguity
choices use the existing generation-guarded goto command. The bounded Fontconfig
fallback handles missing glyphs and style propagation without adding panel fonts.
The meaningful recorded replay has nine unrelated tangent words and zero false
jumps; the next nine-word grounded clause holds for independent confirmation. A
recorded opening repeated later in the replay commits to the exact scripted cursor.
Final inference is 2,177 ms, alignment 35 µs, and the two-request replay takes
4,491 ms. The repeated opening is constructed recorded evidence, not a separate
utterance or general live-talk accuracy result. Authenticated Xorg movement,
decoration, hide/close and freeze checks still require the owner-host environment.

A saturated virtual-camera speech consumer no longer starves recording, streaming
or Notes: the round-robin stress regression passes under ASan/UBSan and fails with
the prior fixed-order scheduler. The source and acceptance publisher both remain
local until permitted Git, GitHub and desktop checks complete.


Final speech ASan/UBSan coverage passes service/disabled-build boundaries, fairness
and actual caption pixels. The refreshed matching helper recognizes the public JFK
fixture in 3,373 ms and returns no hallucinated segments for silence. The complete
helper session (model readiness, recorded JFK, silence, shutdown) measures 3.546063 s
wall time, 12.697959 s user CPU plus 0.198845 s system CPU, and 251,888 KiB peak RSS,
on Intel Core Ultra 5 235U with the explicit multilingual base GGML model, English
language and four threads. Child usage is collected after the sole helper is reaped,
without including compiler processes. This single workstation fixture is not a
live-microphone latency benchmark or a GPU/multiple-accent accuracy claim. Real
FFmpeg-decoded offline transcription writes SRT and VTT with 0600 permissions and
preserves existing destinations.

The final production recording privacy fixture also passes ASan/UBSan: decoded
silence covers 1.201–2.402 s, cut removes over 0.5 s, forbidden PCM is absent, and
sidecars contain no hidden words or cues crossing the protected interval. The
actual complete Pro host passes ASan/UBSan as well. Custom sanitizer link flags
initially suppressed development SDK lookup, and the media profile correctly
refused the system media runtime. Mandatory LGPL lookup flags now survive caller
`LDFLAGS`; running against the explicit candidate SDK passes the fixture.

A separate `STT=0`, `PANEL=0` Pro build passes `-Werror` and the actual complete
host fixture (identity `d45beb77f8c17dd93877dc558876b935`). Its read-only doctor
accurately reports unavailable speech and native Notes window, and opens no audio
or model. The full native STT-enabled identity is restored afterwards.


The added speech runtime regression passes ASan/UBSan with actual shared Config,
metadata and captions code: configured opt-ins stay dormant without a grant; a
recording retains its start-time model snapshot; atomic fast disable clears
unaccepted automatic finalization and its admission generation; mixed ordinary
settings stay on the normal transaction path. Shared caption-or-Notes entitlement
controls cleanup grace, with an additional monotonic deadline. Both 1,900-byte and
8,192-byte adversarial escaped status responses parse as JSON. The structured real
Whisper helper also builds and runs under ASan/UBSan with the recorded fixture and
silence; the external installed inference libraries are not sanitizer-instrumented.
The helper link now preserves caller sanitizer flags as well as mandatory SDK paths.
