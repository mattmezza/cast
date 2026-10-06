# Cast Community and Cast Pro

Cast Community is the public MIT product. Every currently shipping capture,
webcam/composition, virtual-camera, ordinary recording, single-destination RTMP/
RTMPS, manual zoom/follow, overlay, annotation, preset, audio, panel, CLI,
configuration, privacy and reliability feature remains available without payment.
Community has no watermark, duration/resolution restriction or commercial-use ban.
MIT permits commercial use and forks. Dependency obligations still apply to binaries.

Cast Pro is a distinct proprietary binary linked with a separately licensed private
provider. An unlicensed Pro installation retains Community functionality. A signed
license can enable only implemented, compiled, supported, dependency-ready modules.
The private provider implements three workflows. Editable recording projects remain
unimplemented. Local speech requires the optional inference helper and an explicitly
installed model; timed notes and cinematic motion do not require inference.

| Capability ID | Workflow | Current Community | Current Pro |
|---|---|---|---|
| `editable_projects` | Layer/source recording, editor, reframing/export | Absent | Not implemented |
| `cinematic_zoom` | Advanced animation, automatic zoom, cursor smoothing | Absent; existing zoom remains free | Implemented; license required |
| `transcription_subtitles` | Local transcription, styled captions, exports | Absent | Implemented; `STT=1`, model and license required |
| `speech_teleprompter` | Notes window, timed/speech following | Absent | Implemented; native window needs `PANEL=1`, speech mode needs shared inference |

## Build and install

The default build requires only this public checkout and system dependencies:

```sh
make EDITION=community X11=1 WAYLAND=1 PANEL=1
make deps-lgpl
make EDITION=pro PRO_ROOT=/absolute/path/cast-pro MEDIA_PROFILE=lgpl X11=1 WAYLAND=1 PANEL=1
make check-editions
make check-licenses EDITION=pro MEDIA_PROFILE=lgpl PRO_ROOT=/absolute/path/cast-pro
```

`make deps-lgpl` is the explicit network/build operation; ordinary make never
fetches dependencies, models or licenses. `make deps-lgpl DEPS_ARGS=--offline`
uses verified cached sources only. The controlled profile pins FFmpeg, OpenH264,
OpenSSL and zlib, disables GPL/nonfree/autodetection, and records hashes, configuration,
patch/build materials and shared libraries in `build/deps-lgpl/profile.json`.
Missing sources or invalid profile provenance cause a hard failure.
`LGPL_ROOT=/absolute/path/profile-root` selects an alternate controlled profile.
A separately documented candidate SDK is for nonproduction engineering checks only;
it cannot satisfy official release provenance checks.

`PRO_ROOT` defaults to the sibling `../cast-pro`; an explicit override must be
absolute and outside this worktree. The private monorepo has the desktop provider,
verifier, issuer and tests in `pro/`, with a `server/README.md` placeholder for future
Go licensing/payment/site work. No server is implemented. The earlier bare provider
layout remains accepted for compatibility. The sandbox cannot write the sibling:
the prepared checkout is `/tmp/cast-pro`, selected explicitly. Preserve it in a
permanent private checkout before removing `/tmp`. The private provider, verifier, issuer, trusted key headers, draft
EULA and premium modules must stay outside public archives and CI artifacts.
Generated test key files remain outside Git. The private publishing handoff verifies
GitHub ownership and the repository's private flag before sending any source.

`make install` and `make package` follow the selected edition. Convenience targets
`install-community`, `install-pro` and `package-pro` are also available. Community
installs `cast` and `cast-app`; Pro installs `cast-pro` and `cast-pro-app`, separate
application/icon/package IDs and edition-specific documentation/licenses. Installing
one edition never overwrites the other's executable or starts capture. Their default
sockets differ; shared configuration does not merge daemon sessions.

Objects are partitioned by edition, X11, Wayland, panel, media profile, release identity
and private provider/trust content. The executable records public core/private
revisions, extension API, platform, media profile, release timestamp and production
status. `edition --json` and staged `integration-manifest.json` expose the identity.
Local revisions may be explicitly labelled unversioned; official metadata requires
real reviewed revisions and keys. `OFFICIAL_RELEASE=1` alone is insufficient to package.

## Extension and future feature work

`src/pro_extension.h` defines the bounded versioned static link contract. Community
contains edition-neutral capabilities, schema/command/UI affordances and store logic.
The public edition boundary selects and validates the private provider. Missing or incompatible Pro code fails
explicitly. Snapshot values belong to the caller; verification and mutation run outside
capture/audio/compositor timing threads. No general plugin loader is introduced.

Execute each feature brief with both checkouts available and a recorded integration
manifest. Private modules register the stable ID and versioned schema/commands/UI
contributions; implement daemon/service gates before resource acquisition, project
creation or exports, and use the shared reviewed media/speech service. The three
implemented workflows share a bounded declarative schema in the public core.
Community validates and retains dormant values without executing private code.
Unknown config keys remain errors.
Public project specifications and ordinary media access remain open; recording files
must never depend on encryption or a proprietary wrapper to retain access.

Downgrade always rejects new premium work while keeping stop/pause/save/close/cancel/
recover available. Each actual module must define bounded existing-job grants, privacy
barriers and safe finalization. Live recognition retires conservatively on downgrade;
an accepted offline job has a bounded five-minute completion grant. No new job or
listening consumer starts during that grace. Cinematic mode hands its framing back
to legacy zoom without starting or revealing a lane.

macOS, Windows and WASM currently have format/interface integration points only,
not completed ports. Native capture/audio/virtual-camera APIs, OS minimums, SDK terms,
loader security, signing and LGPL replacement/relinking need platform-specific work.
A worker process or MIT ffmpeg.wasm wrapper does not relicense GPL media code.
