# Licensing and offline commercial rights

Public project-owned source remains MIT under `LICENSE`; prior releases retain their
licenses. Pro original private code reserves commercial rights. Publicly readable
restricted source would be source available, not open source. Dependencies retain
their own licenses and notices; the Pro EULA does not claim them as proprietary.
See [edition scope](editions.md), [dependencies](dependencies.md),
[contribution rights](../CONTRIBUTING.md) and [branding](../BRANDING.md).

The baseline draft commercial policy is per user, two owned desktop devices across
supported OSes. An offline portable file cannot reliably count devices or prevent
copying; this policy is contractual. Device transfer is file import. No fingerprint,
hardware lock, telemetry, online activation or periodic connection is required.

Only trusted public verification keys ship in the Pro binary. Ed25519 verification
uses pinned libsodium (ISC); private issuer keys belong outside repositories,
packages, logs and crash artifacts. Production provisioning is an owner action.
Local fixtures use visibly nonproduction keys; no committed demo key is a production
fallback. Key IDs support explicit compiled trust sets and rotation. Runtime environment,
configuration and license-supplied keys cannot add trust or entitlements.

The private verifier accepts a versioned bounded envelope with key ID, base64 exact
payload bytes and detached signature. License and update signatures have distinct
trust sets and domain strings. Verification authenticates original bytes before
semantic parsing. Envelopes are at most 16 KiB, payloads at most 8 KiB, identifiers
at most 128 bytes and feature arrays at most 64 entries. Duplicate fields, malformed
UTF-8, incorrect types, unknown required schema versions, overflowed timestamps and
contradictory claims are errors. Unknown future feature IDs are inert and grant no code.
The payload includes product `cast-pro`, schema/key/license IDs, issue/not-before UTC
integer times, kind, platform/feature scope and contractual seats/devices.

Perpetual usage does not expire. This release is eligible when its authenticated
release timestamp is on/before `updates_until`; an eligible binary stays usable
thereafter offline. A newer ineligible release retains Community features and reports
`release_not_eligible`; it never deletes a license, project or recording. Subscription
usage requires `not_before <= now < expires_at`; subscriptions do not become perpetual
merely by supplying an update cutoff. Clock before not-before fails closed. Trial
tracking is absent. Offline subscriptions cannot conclusively detect clock rollback,
and locally patchable executables cannot enforce perfect DRM or offline revocation.

`edition`, `features` and `license status` are local, read-only operations and need
no camera, daemon or network. General diagnostics mask license identifiers and never
print complete payloads/signatures/customer data. Mutations route through the owning
running daemon's authenticated IPC; daemon-absent operations hold its exclusive state
lock and the shared physical license-store ownership lock. Community and Pro, custom
socket names and ordinary path aliases cannot create independent writers for the same
store. A second daemon retains Community diagnostics but cannot mutate that store or
start premium work. A disconnected client must not write a file behind an active daemon. Imported
files are verified snapshots copied atomically using bounded regular-file reads,
restrictive permissions and fsync/rename. Invalid replacement preserves accepted state.
Explicit configured paths are literal; no shell/tilde expansion. Empty paths resolve
internally to per-user storage: Linux XDG data `cast/license.json` (HOME fallback),
future macOS Application Support and Windows LocalAppData. Platform paths outside
Linux are a contract, not a port claim.

Community has no crypto/provider dependency. It reports verifier unavailable rather
than claiming unverified files valid. Inspection/import for later Pro use requires an
installed compatible Pro verification path; even successful import cannot add code
absent from Community. See the CLI's authoritative current unavailable diagnostic.

Removing/reloading/importing never starts/resumes capture, changes output privacy,
writes ordinary configuration, or erases media, models or notes. Future implemented
modules must keep bounded accepted work safe during entitlement changes and reject
new jobs immediately. Panel acknowledgement comes from the daemon. Expired or absent
licenses never prevent safety operations.

The private scaffold contains a draft EULA explicitly pending owner/legal review,
with placeholders for entity/jurisdiction, perpetual/update/subscription/device
terms, third-party carve-outs and LGPL replacement/debugging reverse-engineering
rights. Production requires entity/EULA review, production license and update trust
keys, signed release metadata, reviewed download/storefront endpoints if desired,
exact dependency/source notices and patent/SDK distribution review. No storefront,
legal clearance or completed macOS/Windows/WASM release is fabricated here.
