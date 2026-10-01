# Publishing a release

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
