# Desktop updates

Free and Pro share the signed desktop updater protocol. macOS replaces the whole
application bundle. Linux installs an entire verified runtime, preserving helper
programs and sibling resources. An archive contains `payload/`, with either the
app contents or the Linux runtime inside. The entry point is
`Contents/MacOS/cast` on macOS and `cast` on Linux (Pro uses `cast-pro`).

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

The publication workflow stages by default. After application/update acceptance,
explicit publication writes and reads back immutable artifacts before advancing
the channel with an ETag condition and strictly increasing stable version.
Versions through 0.10.5 are rejected because they do not implement this updater.
Implementing this flow does not publish an application or populate those channels.
