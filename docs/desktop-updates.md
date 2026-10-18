# Desktop updates

Free and Pro share the signed desktop updater protocol. macOS replaces the whole
application bundle. Linux installs an entire verified runtime, preserving helper
programs and sibling resources. An archive contains `payload/`, with either the
app contents or the Linux runtime inside. The entry point is
`Contents/MacOS/cast` on macOS and `cast` on Linux (Pro uses `cast-pro`).

## Using updates

The panel checks in the background when opened, then once a day. **Update available**
replaces the About button when a newer eligible release is found. Open it to see
the version and choose **Install and restart**. **Check for updates** is also in
Community/About or Pro/About. Network checks never block recording or the UI.

Installation is disabled during recording (including paused recordings), countdowns,
finalization or streaming. The daemon checks again after downloading and reserves
installation before stopping. All recorder sessions of the same edition share a
runtime lock, so another session cannot start recording while files are replaced.
The installer never kills a recorder. If another session still owns its runtime,
installation fails safely and the app reopens.

Updates verify signed metadata, the entire archive and the new executable's version
before shutdown. The installer authenticates the staged download again in a separate
process, waits for the old app and daemon to exit, then replaces the whole runtime.
A startup check after replacement restores the prior runtime if the new executable
cannot load. Successful updates retain a `.cast-update-previous-*` backup beside
the installed app/runtime. Configuration, licenses, speech models and recordings
are stored separately and are preserved.

On Linux, a loose user-owned executable migrates to
`~/.local/share/cast/desktop/community/runtime` (or `pro/runtime`). Its original
command path becomes a symlink to that complete installation; the original file
is retained with `.before-desktop-update`. Package-managed `/usr` and `/opt`
installations continue to use their package manager. On macOS, install the app in
a writable Applications folder first; the complete `.app` is replaced in place.
No administrative privilege is requested by the updater.

CLI equivalents are `cast update --check` and `cast update --install`, or
`cast-pro update --check` / `cast-pro update --install`. The install command also
restarts the app. Existing Arch `cast update [VERSION]` and Pro offline
`update --bundle` commands remain available.

The stable Free discovery URLs are:

- `https://downloads.superscreencast.com/cast/desktop/community/macos-arm64/stable.manifest`
- `https://downloads.superscreencast.com/cast/desktop/community/linux-x86_64/stable.manifest`

Pro uses `pro` in place of `community`. Ed25519 signatures authenticate edition,
platform, version, release timestamp, archive URL, SHA-256, size and binary path.
Archives use immutable version/hash URLs. A separate signature domain prevents
license or previous Pro update signatures from authorizing desktop updates.

Release maintainers first publish and verify the normal Free release using the
macOS or static Linux release workflows. The protected `Sign and publish accepted
desktop updates` workflow in the private release repository then retrieves those
exact assets, verifies GitHub asset digests, release checksums and source commit,
packages the complete runtime, and signs it with existing production update trust.
Pro publication starts from an authenticated accepted standalone kit. All signing
and storage credentials remain on the existing protected production runner.

An hourly protected workflow automatically discovers newly published Free/Pro
releases and advances each eligible edition/platform independently. It waits for
complete release assets or authenticated accepted kit links, and skips drafts,
prereleases and channels already at an equal/newer version. Manual staging and
publication remain available. Publication writes and reads back immutable
artifacts before advancing the channel with an ETag condition and strictly
increasing stable version.
Versions through 0.10.5 are rejected because they do not implement this updater.
Implementing this flow does not publish an application or populate those channels.
