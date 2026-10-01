#!/bin/sh
# Collect the exact Arch dependency sources used for the 0.1.0 build.
set -eu
fail() { echo "dependency-sources: $*" >&2; exit 1; }
output=${1:-}
version=${2:-}
[ -n "$output" ] || fail 'expected output directory and cast version'
case "$version" in ''|*[!0-9A-Za-z.-]*) fail 'invalid cast version' ;; esac
for command in pacman curl git tar b2sum sha256sum mktemp makepkg gpg; do
    command -v "$command" >/dev/null || fail "required command missing: $command"
done
[ "$(pacman -Q ffmpeg)" = 'ffmpeg 2:9.0.2-1' ] || fail 'source pins require Arch ffmpeg 2:9.0.2-1'
[ "$(pacman -Q glib2)" = 'glib2 2.88.3-1' ] || fail 'source pins require Arch glib2 2.88.3-1'
mkdir -p "$output"
output=$(cd "$output" && pwd)
name=cast-$version-dependency-sources
[ ! -e "$output/$name.tar.gz" ] || fail 'existing dependency-source archives are never overwritten'
work=$(mktemp -d "$output/.dependency-sources-XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
source_dir=$work/$name
mkdir -p "$source_dir/ffmpeg" "$source_dir/glib2"
api=https://gitlab.archlinux.org/api/v4/projects/archlinux%2Fpackaging%2Fpackages%2F
ffmpeg_commit=ad9843ecbd7fa37fa07710932efb3b116c164956
glib_commit=8dd68d402cb150f0e4da628590bca92dbcd33bcd
gvdb_commit=2b42fc75f09dbe1cd1057580b5782b08f2dcb400
for package in ffmpeg glib2; do
    case "$package" in ffmpeg) commit=$ffmpeg_commit ;; glib2) commit=$glib_commit ;; esac
    curl --fail --location --silent --show-error --retry 2 --max-time 120 \
        "${api}${package}/repository/archive.tar.gz?sha=$commit" -o "$work/$package-packaging.tar.gz"
    tar -xzf "$work/$package-packaging.tar.gz" --strip-components=1 -C "$source_dir/$package"
done
git clone --bare --depth 1 --branch n9.0.2 https://git.ffmpeg.org/ffmpeg.git "$source_dir/ffmpeg/ffmpeg"
git clone --bare --depth 1 --branch 2.88.3 https://gitlab.gnome.org/GNOME/glib.git "$source_dir/glib2/glib"
git init --bare "$source_dir/glib2/gvdb" >/dev/null
git --git-dir="$source_dir/glib2/gvdb" remote add origin https://gitlab.gnome.org/GNOME/gvdb.git
git --git-dir="$source_dir/glib2/gvdb" fetch --depth 1 origin "$gvdb_commit"
git --git-dir="$source_dir/glib2/gvdb" update-ref refs/heads/main "$gvdb_commit"
git --git-dir="$source_dir/glib2/gvdb" symbolic-ref HEAD refs/heads/main

# Arch hashes Git sources as git archive output; verify the same content.
ffmpeg_hash=$(git --git-dir="$source_dir/ffmpeg/ffmpeg" archive --format=tar n9.0.2 | b2sum | cut -d ' ' -f 1)
glib_hash=$(git --git-dir="$source_dir/glib2/glib" archive --format=tar 2.88.3 | b2sum | cut -d ' ' -f 1)
[ "$ffmpeg_hash" = '97bd244a79088e862f349ef91ae75f7a02c1a0c8c1e21601abf6fa5acae1d721d104c2afa4e6e955d7caf18ffd511c175b1a3965b99cc1122754f191b9098bc5' ] || fail 'FFmpeg upstream content differs from Arch source checksum'
[ "$glib_hash" = '9239e87c1133864340fda4ba99c53cd1684f421f7fb9a90299b6a25f582db3f70e7aab6988a7943934ece0d48246bc61c764cd08ef98f6bec3d4383a3f1bf679' ] || fail 'GLib upstream content differs from Arch source checksum'
[ "$(git --git-dir="$source_dir/ffmpeg/ffmpeg" rev-parse 'n9.0.2^{commit}')" = 946fcce07b6dcd0331c8cc609192aeff5e1924f8 ] || fail 'unexpected FFmpeg tag commit'
[ "$(git --git-dir="$source_dir/glib2/glib" ls-tree 2.88.3 subprojects/gvdb | awk '{print $3}')" = "$gvdb_commit" ] || fail 'unexpected GLib gvdb submodule'

# Use only the public keys in the pinned official packaging repositories.
mkdir -m 700 "$work/keyring"
gpg --homedir "$work/keyring" --batch --import "$source_dir"/*/keys/pgp/*.asc
for package in ffmpeg glib2; do
    if ! (cd "$source_dir/$package" && GNUPGHOME="$work/keyring" makepkg --verifysource --holdver) > "$source_dir/$package/source-verification.txt" 2>&1; then
        cat "$source_dir/$package/source-verification.txt" >&2
        fail "$package source checksum/signature verification failed"
    fi
done
mkdir "$source_dir/ffmpeg/upstream-licenses" "$source_dir/glib2/upstream-licenses"
git --git-dir="$source_dir/ffmpeg/ffmpeg" archive n9.0.2 COPYING.GPLv2 COPYING.GPLv3 COPYING.LGPLv2.1 COPYING.LGPLv3 LICENSE.md | tar -xf - -C "$source_dir/ffmpeg/upstream-licenses"
git --git-dir="$source_dir/glib2/glib" archive 2.88.3 COPYING LICENSES | tar -xf - -C "$source_dir/glib2/upstream-licenses"

for package in ffmpeg glib2; do
    case "$package" in
        ffmpeg) package_version=2:9.0.2-1 ;;
        glib2) package_version=2.88.3-1 ;;
    esac
    cache=/var/cache/pacman/pkg/$package-$package_version-x86_64.pkg.tar.zst
    [ -r "$cache" ] || fail "exact binary package metadata unavailable: $cache"
    tar -xOf "$cache" .BUILDINFO > "$source_dir/$package/ARCH-BUILDINFO.txt"
    tar -xOf "$cache" .PKGINFO > "$source_dir/$package/ARCH-PKGINFO.txt"
    pacman -Qi "$package" > "$source_dir/$package/installed-package.txt"
done
ffmpeg -version > "$source_dir/ffmpeg/installed-build.txt" 2>&1
ffmpeg -L > "$source_dir/ffmpeg/installed-license.txt" 2>&1
cat > "$source_dir/README.txt" <<EOF
cast $version dependency sources for this Arch Linux build

FFmpeg 2:9.0.2-1: upstream n9.0.2, commit
946fcce07b6dcd0331c8cc609192aeff5e1924f8, plus Arch Chromium patch.
Arch packaging commit: $ffmpeg_commit
https://gitlab.archlinux.org/archlinux/packaging/packages/ffmpeg/-/commit/$ffmpeg_commit

GLib 2.88.3-1: upstream tag 2.88.3 plus both Arch patches and
gvdb submodule at $gvdb_commit.
Arch packaging commit: $glib_commit
https://gitlab.archlinux.org/archlinux/packaging/packages/glib2/-/commit/$glib_commit

Each package directory includes the original PKGBUILD, .SRCINFO, patches,
hooks, packaging licenses/keys, shallow bare upstream Git repositories,
and exact Arch binary .BUILDINFO/.PKGINFO. Upstream Git archive content
matches the BLAKE2 checksums in the corresponding Arch PKGBUILD.
The tagged signature objects and Arch public keys are preserved. makepkg
verifies upstream signatures with those keys; the FFmpeg signature passes
with an expired-key warning, recorded in source-verification.txt. GLib's
signature passes. Original license texts are in upstream-licenses/.
SHA256SUMS validates every collected file.

Build on Arch Linux using the build dependencies listed in PKGBUILD and
the package/toolchain versions recorded in ARCH-BUILDINFO.txt. In each
package directory, import the provided upstream keys into your build
keyring, then run makepkg --holdver (with dependencies already installed).
The adjacent bare repositories are makepkg's Git source caches; holdver
retains their pinned commits. The standard PKGBUILD applies all patches,
selects the original configure/Meson flags, and builds the package.
Reproducing byte-identical binaries also requires the original Arch
toolchain/build environment; this collector does not perform that rebuild.

Licenses: Arch identifies FFmpeg as GPL-3.0-only; installed ffmpeg -L
reports GPL version 3 or later. GLib's package identifies LGPL-2.1-or-later.
Original upstream license files remain in the bare repositories and can
be read with git --git-dir=ffmpeg show n9.0.2:COPYING.GPLv3 (FFmpeg),
or git --git-dir=glib show 2.88.3:COPYING (GLib).

Scope: this archive includes FFmpeg, GLib, and GLib's gvdb source.
It does not include sources for FFmpeg's separately packaged optional
codec libraries, other transitive libraries, PipeWire, X11, the C library,
or build tools. Those packages are identified in the original PKGBUILD
and ARCH-BUILDINFO.txt. The cast binary archive contains no shared-library
binaries; its source and licenses are separate release assets. This
collection describes the supplied materials and is not a legal conclusion
that all possible redistribution obligations have been satisfied.
EOF
(cd "$source_dir" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar -czf "$work/$name.tar.gz" -C "$work" "$name"
mv "$work/$name.tar.gz" "$output/$name.tar.gz"
printf '%s\n' "$output/$name.tar.gz"
