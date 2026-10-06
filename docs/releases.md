# Publishing a release

## Automated Arch packages

`.github/workflows/release-arch.yml` runs when a GitHub release is published. It
builds the exact tagged commit in the official Arch Linux container with Xorg,
Wayland and the Clay/SDL3 panel enabled. A dedicated ordinary user runs makepkg;
unit, IPC, mock portal, Xorg and native panel checks must pass before asset upload.
Only the publishing job gets release write permission. External actions are pinned
to commits, and the installed Arch package versions are recorded in the build manifest.

For v0.10 (executable/package version 0.10.0):

```sh
git push origin main
git tag -a v0.10 -m 'cast v0.10'
git push origin refs/tags/v0.10
make release-ci RELEASE_TAG=v0.10 RELEASE_NOTES=docs/release-notes/0.10.0.md
gh run list --workflow release-arch.yml
```

`release-ci` validates the clean, pushed tagged revision and creates the release
using gh. It delegates the build and upload to Actions. A release page exists while
the build is running; wait for a successful workflow and inspect its assets before
announcing it. Existing releases and existing asset names are never overwritten.
Keep previously published releases unchanged. The tag must be `vVERSION`, or `vMAJOR.MINOR` for a zero patch
version; both the Makefile and executable version must match.

The six x86_64 assets use the prefix `cast-VERSION-archlinux-x86_64`:

- `.pkg.tar.zst`: installable pacman package with declared runtime dependencies.
- `.tar.gz`: staged `/usr` executable, documentation, examples and licenses.
- `-source.tar.gz`: exact project source, including vendored code and fonts.
- `-build-info.txt`: package build metadata and direct runtime libraries.
- `-source-commit.txt`: exact commit.
- `-SHA256SUMS`: checksums for the other five files.

These binaries target current Arch x86_64 library ABIs. Other distributions and
architectures can build from source. The package does not create a camera device,
start a service, or replace user configuration. `cast setup` explains the separate
camera/conferencing steps. `cast update [VERSION]` and the curl installer download
the package and verify its checksum before invoking pacman. A checksum from the
same HTTPS release checks download integrity; it is not a separate signing key.

Test the workflow without publishing or pushing a tag:

```sh
gh workflow run release-arch.yml --ref main
```

An empty `release_tag` input tests the selected commit and preserves downloadable
Actions artifacts for 14 days. To attach missing assets after a build/upload failure,
dispatch with `-f release_tag=v0.10`; this verifies the existing release and exact
tag before building. Any already-uploaded asset name causes publication to stop;
inspect partial uploads rather than silently replacing them.

## Local host release builds

GitHub releases contain four explicit assets: `cast-VERSION-source.tar.gz`,
`cast-VERSION-linux-ARCH.tar.gz`, `cast-VERSION-dependency-sources.tar.gz`, and
`SHA256SUMS`. The project source asset is a Git
archive of the tagged commit, including the project, vendored inih, licenses,
documentation, examples and packaging files. GitHub's automatic source downloads
are separate from this asset. The binary archive contains a staged `/usr` install
and a build manifest identifying the commit, enabled backends and runtime libraries.

Release requirements are Git, authenticated GitHub CLI (`gh auth login`), GNU make,
GNU tar, `sha256sum`, and the normal build dependencies. `origin` must point to
the intended repository on github.com. The publisher must have release write access.
The dependency source helper additionally requires its documented tools and
network access to fetch pinned sources; it refuses unsupported dependency builds
or downloads that fail verification. See [dependencies.md](dependencies.md).
The binary uses the build host's architecture and library ABI; this is not a
universal or statically linked Linux package. The first release uses `RELEASE_TAG=v0.1` with package/executable version `0.1.0`.
The tag defaults to `v$(VERSION)`; an explicit tag still must identify the exact
clean HEAD and pushed commit. Archive filenames retain `VERSION`.

Review the FFmpeg redistribution
requirements in [dependencies.md](dependencies.md) before distributing binaries.

1. Update `VERSION` in `Makefile`, `CAST_VERSION` in `src/cast.h`, and `pkgver`
   in `packaging/PKGBUILD` together. Write nonempty release notes covering changes,
   verification, runtime dependencies and known hardware/backend limitations.
   Commit the notes if they are stored in the repository.
2. Run the appropriate checks and hardware acceptance steps, and commit the final
   code, documentation and verification results. The release target builds packages;
   it does not rerun the acceptance suite.
3. Push the commit and an annotated version tag explicitly:

   ```sh
   git push origin HEAD
   git tag -a v0.1 -m 'cast v0.1'
   git push origin refs/tags/v0.1
   ```

4. Check and publish with the same backend settings you verified:

   ```sh
   make release-check VERSION=0.1.0 RELEASE_TAG=v0.1 RELEASE_NOTES=docs/release-notes/0.1.0.md X11=1 WAYLAND=1
   make release VERSION=0.1.0 RELEASE_TAG=v0.1 RELEASE_NOTES=docs/release-notes/0.1.0.md X11=1 WAYLAND=1
   ```

`release-check` reads Git/GitHub state without building or publishing. Both targets
require a clean checkout (including untracked files), a version matching the header,
and a local tag whose commit equals both HEAD and the pushed tag. They reject an
existing release. Notes can also live outside the repository; `RELEASE_NOTES` is
required so publication never falls back to an interactive prompt or implicit notes.

`release` extracts the exact tagged source to a temporary directory, builds there
without reusing checkout objects, prepares the matching dependency source archive,
creates checksums, then rechecks the Git/GitHub
state before publication. The resulting files and a copy of the notes are preserved
under `dist/releases/RELEASE_TAG/linux-ARCH/`. `gh release create --verify-tag` attaches
the binary archive, both explicit source archives and checksums. It cannot create a
tag implicitly. Version/commit provenance is checked; byte-for-byte reproducibility
across different compilers or dependency builds is not claimed.

Existing artifact directories are never overwritten. If upload fails, preserve
the artifacts, inspect `gh release view RELEASE_TAG` for a partial draft, and determine
whether to finish that draft or retry publication. Do not replace an existing
published release or its assets silently. After a successful release, inspect it
with `gh release view RELEASE_TAG` and check downloaded files using `sha256sum -c SHA256SUMS`.

For local development, `make package X11=1 WAYLAND=1` still packages the working
tree under `dist/`; it does not create a GitHub release. `VERSION` must match the
header here as well. Use the release target for published artifacts so the source
and binary refer to a clean, tagged revision.

Set `PANEL=1` on package/release targets to include the optional Clay/SDL3 panel.
The selected value is propagated into the isolated release build; that environment
needs SDL3 and SDL3_ttf development packages. Published v0.1 assets remain unchanged.

## Separate Pro staging and offline updates

The public Arch/AUR recipes and workflow remain Community-only; they never check
out private code or access issuer keys. Public archival scans run before release
archives. `make package EDITION=community` creates the existing Community aliases
plus an edition/media-qualified Linux archive, an explicit public-source archive and
checksums. Neither local package target publishes. Public source roots are an
allowlist in `packaging/public-boundary.py`; forbidden private directories, key
material and private provider files fail the scan even when ignored by Git.

Pro staging uses `make package EDITION=pro MEDIA_PROFILE=lgpl
PRO_ROOT=/absolute/path/cast-pro`. Artifacts are named
`cast-pro-VERSION-linux-ARCH-lgpl.tar.gz`, with separate package/application IDs,
application-local shared media, notices, machine-readable dependency inventory and
integration manifest. Private application source/issuer keys never enter those
archives. Local stages are visibly nonproduction. The private source tree has its
own explicit source/artifact manifest and must be preserved through a private channel.
The matching `cast-pro-VERSION-linux-ARCH-lgpl-dependency-sources.tar.gz` contains
controlled dependency source/build materials and is included in package checksums.

Official Pro additionally requires compiled production trust keys and authenticated
release identity, a private `release/production.json` matching version, core/private
revision, UTC release timestamp, platform and media profile, its trusted-header hash,
legal and patent/SDK review references, and clean-runtime acceptance. Exact controlled
dependency sources/build materials and every runtime chosen-license review are required.
Test keys, candidate SDKs, missing metadata and unresolved dynamic loads fail closed.
`OFFICIAL_RELEASE=1` is an input, never an exemption. Production key generation and
signing happen outside public CI/repositories. Private CI must receive read-only
private source and separately protected signing inputs; no private CI is fabricated
in this public repository. Existing published releases stay immutable.

No hosted Pro channel/storefront has been configured. `cast-pro update` reports that
status honestly and never falls back to GitHub Community assets. The working local
channel consumes a directory containing `manifest.json` and `cast-pro`. The manifest
uses the private issuer's update domain and separate update trust keys. Its exact
signed payload binds product/edition, version, platform, LGPL profile, extension API,
release timestamp, core/private revisions and SHA-256 of the artifact.

```sh
cast-pro update --bundle /absolute/path/bundle
cast-pro update --bundle /absolute/path/bundle --download-only /absolute/private/download-directory
cast-pro update --bundle /absolute/path/bundle --install /absolute/path/cast-pro
cast-pro update --bundle /absolute/path/older-bundle --install /absolute/path/cast-pro --rollback
```

The first command verifies without installation. Download-only may save a signed
ineligible release with a clear warning; implicit install cannot replace an eligible
installation with an ineligible release. Paths are caller-chosen, never manifest
commands. Bounded regular-file snapshots are copied before authentication, preventing
artifact mutation between hash checking and install. Installation requires an owned
non-writable-by-others parent, stops at the daemon session lock if running, preserves
config/license/media, stages/fsyncs and atomically renames. Existing binaries are
retained as `cast-pro.rollback`; preserve/remove an old backup deliberately before
another update. Downgrades require explicit `--rollback` and still require signed
identity and license eligibility. No capture starts during updates.

Offline bundles currently replace only the native executable; compatible reviewed
shared-media libraries must already be installed. A media ABI/profile change needs
an independently staged complete edition package and clean runtime validation.
Newest-eligible hosted release discovery and owner endpoints remain production
inputs, not successful simulated network operations. Archive checksums provide
integrity; only the signed Pro manifest authenticates Pro release provenance.
