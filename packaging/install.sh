#!/bin/sh
# Community only: never install/upgrade Cast Pro from this channel.
# Install the official Arch Linux release package; also embedded in `cast update`.
set -eu
umask 077

fail() { printf 'cast install: %s\n' "$*" >&2; exit 1; }
usage() {
    printf '%s\n' 'Usage: install.sh [vMAJOR.MINOR[.PATCH]] [--download-only DIRECTORY]' \
        'Defaults to the latest GitHub release. Requires Arch Linux x86_64 to install.' \
        'Stop the cast daemon before installing; start it again after the update.'
}
valid_tag() {
    printf '%s\n' "$1" | awk 'NR > 1 { invalid=1 }
        length($0) <= 64 && /^v[0-9]+\.[0-9]+(\.[0-9]+)?$/ { ok=1 }
        END { exit !ok || invalid }'
}

tag=latest
tag_set=0
destination=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --download-only)
            [ "$#" -ge 2 ] && [ -n "$2" ] || fail '--download-only requires a directory'
            [ -z "$destination" ] || fail '--download-only may only appear once'
            destination=$2
            shift 2 ;;
        *)
            [ "$tag_set" = 0 ] || fail 'expected one version and optional --download-only DIRECTORY'
            valid_tag "$1" || fail 'version must be vMAJOR.MINOR or vMAJOR.MINOR.PATCH'
            tag=$1
            tag_set=1
            shift ;;
    esac
done

for tool in curl sha256sum awk uname mktemp; do
    command -v "$tool" >/dev/null 2>&1 || fail "required command missing: $tool"
done
[ "$(uname -m)" = x86_64 ] || fail 'release packages support x86_64 only'
if [ -z "$destination" ]; then
    [ -f /etc/arch-release ] || fail 'installation requires Arch Linux (use --download-only DIRECTORY to fetch the package)'
    command -v pacman >/dev/null 2>&1 || fail 'required command missing: pacman'
    if [ "$(id -u)" != 0 ]; then
        command -v sudo >/dev/null 2>&1 || fail 'installing requires sudo or running this installer as root'
    fi
    # `curl ... | sh` supplies the script on stdin. Pacman must confirm on the tty.
    ( : </dev/tty ) 2>/dev/null || fail 'installation needs an interactive terminal; use --download-only DIRECTORY, then install the verified package with sudo pacman -U'
fi

repository=https://github.com/mattmezza/cast
if [ "$tag" = latest ]; then
    resolved=$(curl --fail --location --proto '=https' --proto-redir '=https' \
        --silent --show-error --output /dev/null --write-out '%{url_effective}' \
        "$repository/releases/latest") || fail 'could not resolve the latest GitHub release'
    case "$resolved" in
        "$repository/releases/tag/"*) tag=${resolved#"$repository/releases/tag/"} ;;
        *) fail 'latest release did not resolve to an official release tag' ;;
    esac
    valid_tag "$tag" || fail 'latest release has an unsupported version tag'
fi
version=${tag#v}
case "$version" in *.*.*) ;; *) version=$version.0 ;; esac
prefix=cast-$version-archlinux-x86_64
package=$prefix.pkg.tar.zst
manifest=$prefix-SHA256SUMS
url=$repository/releases/download/$tag
work=$(mktemp -d "${TMPDIR:-/tmp}/cast-install.XXXXXXXX") || fail 'could not create a private download directory'
trap 'rm -rf "$work"' 0
trap 'exit 130' INT
trap 'exit 143' HUP TERM
printf 'Downloading cast %s from GitHub…\n' "$tag"
curl --fail --location --proto '=https' --proto-redir '=https' --show-error \
    --output "$work/$manifest" "$url/$manifest" || fail 'checksum download failed; this release may not have an Arch package'
# Select one exact filename. Never execute or check paths supplied by the manifest.
expected=$(awk -v package="$package" '
    $2 == package {
        count++
        if (NF != 2 || length($1) != 64 || $1 !~ /^[0-9a-fA-F]+$/) invalid=1
        hash=$1
    }
    END { if (count != 1 || invalid) exit 1; print tolower(hash) }
' "$work/$manifest") || fail 'checksum manifest must contain exactly one valid checksum for the release package'
curl --fail --location --proto '=https' --proto-redir '=https' --show-error \
    --output "$work/$package" "$url/$package" || fail 'package download failed'
actual=$(sha256sum "$work/$package") || fail 'could not calculate the package checksum'
actual=${actual%% *}
[ "$actual" = "$expected" ] || fail 'package checksum mismatch; nothing was installed'
printf '%s  %s\n' "$expected" "$package" > "$work/$manifest"
printf 'Verified SHA-256 for %s\n' "$package"

if [ -n "$destination" ]; then
    mkdir -p -- "$destination" || fail 'could not create the download directory'
    cp -- "$work/$package" "$work/$manifest" "$destination/" || fail 'could not save the verified package'
    printf 'Saved verified package and checksum in %s\n' "$destination"
    exit 0
fi
printf '%s\n' 'Stop the cast daemon before installing. Start it again after the update.'
if [ "$(id -u)" = 0 ]; then
    pacman -U -- "$work/$package" </dev/tty || fail 'pacman installation failed'
else
    sudo pacman -U -- "$work/$package" </dev/tty || fail 'pacman installation failed'
fi
printf 'Installed cast %s. Start cast again to use the new version.\n' "$tag"
resolved_cast=$(command -v cast || true)
if [ -n "$resolved_cast" ] && [ "$resolved_cast" != /usr/bin/cast ]; then
    printf 'Your PATH currently selects %s; the release package installs /usr/bin/cast.\n' "$resolved_cast"
    printf '%s\n' 'Remove an older manual installation from its source checkout with sudo make uninstall PREFIX=/usr/local, or use /usr/bin/cast explicitly.'
fi
