#!/bin/sh
# Build exact tagged source as an ordinary user; dependency installation belongs to CI setup.
# Usage: sh packaging/ci-build-arch.sh TAG OUTPUT_DIRECTORY
# CAST_CI_MAKEPKG_NODEPS=1 is only for local validation with an isolated dependency SDK.
set -eu

fail() { echo "ci-build-arch: $*" >&2; exit 1; }
[ "$#" -eq 2 ] || fail 'expected release tag and output directory'
[ "$(id -u)" -ne 0 ] || fail 'run makepkg as an ordinary build user, not root'
for command in python3 git makepkg make tar bsdtar sha256sum mktemp pacman readelf grep; do
    command -v "$command" >/dev/null || fail "required command missing: $command"
done

tag=$1
output=$2
[ -n "$output" ] || fail 'output directory must not be empty'
git check-ref-format "refs/tags/$tag" >/dev/null || fail 'invalid release tag'
root=$(git rev-parse --show-toplevel)
cd "$root"
python3 packaging/public-boundary.py
git diff --no-ext-diff --quiet && git diff --no-ext-diff --cached --quiet || fail 'tracked source changes must be committed'
commit=$(git rev-parse HEAD)
[ -z "$(git ls-files -- '*-prompt.md' 'prompt.md')" ] || fail 'local prompt files must not be tracked in release source'
official=0
case "${CAST_CI_OFFICIAL:-0}" in true|1) official=1 ;; false|0) ;; *) fail 'CAST_CI_OFFICIAL must be true/false or 1/0' ;; esac
release_timestamp=$(git show -s --format=%ct "$commit")
tag_commit=$(git rev-parse --verify "refs/tags/$tag^{commit}") || fail 'release tag is not available locally'
[ "$tag_commit" = "$commit" ] || fail 'release tag must identify the checked-out commit'
version=$(git show "$commit:src/cast.h" | sed -n 's/^#define CAST_VERSION "\([^"]*\)"/\1/p')
case "$version" in ''|*[!0-9A-Za-z.+_]*) fail 'invalid source version for an Arch package' ;; esac
alias_tag=
case "$version" in
    *.0)
        alias=${version%.0}
        if printf '%s\n' "$alias" | grep -Eq '^[0-9]+\.[0-9]+$'; then
            alias_tag=v$alias
        fi
        ;;
esac
if [ "$tag" != "v$version" ]; then
    [ -n "$alias_tag" ] && [ "$tag" = "$alias_tag" ] ||
        fail 'release tag must equal vCAST_VERSION or vMAJOR.MINOR for a zero patch version'
fi

make_version=$(git show "$commit:Makefile" | sed -n 's/^VERSION = //p')
[ "$make_version" = "$version" ] || fail 'Makefile VERSION differs from CAST_VERSION'
arch=$(uname -m)
case "$arch" in x86_64|aarch64) ;; *) fail "unsupported Arch architecture: $arch" ;; esac
case "${CAST_CI_MAKEPKG_NODEPS:-0}" in 0|1) ;; *) fail 'CAST_CI_MAKEPKG_NODEPS must be 0 or 1' ;; esac

mkdir -p "$output"
output=$(cd "$output" && pwd)
prefix=cast-$version-archlinux-$arch
for suffix in .pkg.tar.zst .tar.gz -source.tar.gz -build-info.txt -source-commit.txt -SHA256SUMS; do
    [ ! -e "$output/$prefix$suffix" ] || fail "artifact already exists: $prefix$suffix"
done
work=$(mktemp -d "$output/.cast-arch-build-XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
archive=cast-$version-source.tar.gz
git archive --format=tar.gz --prefix="cast-$version/" "$commit" > "$work/$archive"
checksum=$(sha256sum "$work/$archive" | cut -d ' ' -f 1)
git show "$commit:packaging/ci/PKGBUILD" > "$work/PKGBUILD.template"
sed -e "s/@VERSION@/$version/g" -e "s/@SHA256@/$checksum/g" -e "s/@COMMIT@/$commit/g" -e "s/@OFFICIAL@/$official/g" -e "s/@TIMESTAMP@/$release_timestamp/g" "$work/PKGBUILD.template" > "$work/PKGBUILD"

printf 'Building cast %s at %s for Arch %s with X11=1 WAYLAND=1 PANEL=1\n' "$version" "$commit" "$arch"
# Always execute check(), even if a user makepkg configuration disables it. No root escalation.
if [ "${CAST_CI_MAKEPKG_NODEPS:-0}" -eq 1 ]; then
    (cd "$work" && makepkg --check --nodeps --log)
else
    (cd "$work" && makepkg --check --log)
fi
package=$work/cast-$version-1-$arch.pkg.tar.zst
[ -s "$package" ] || fail 'makepkg did not produce the expected package'
mkdir "$work/stage" "$work/assets"
bsdtar -xf "$package" -C "$work/stage" usr
[ -x "$work/stage/usr/bin/cast" ] || fail 'installed cast executable is missing'
"$work/stage/usr/bin/cast" --help | grep '^  panel ' >/dev/null || fail 'package does not contain the control panel'
[ "$("$work/stage/usr/bin/cast" --version)" = "cast $version" ] || fail 'packaged executable version differs from source'
# Staging comes from the checked package itself, avoiding a second independent install.
tar -czf "$work/assets/$prefix.tar.gz" -C "$work/stage" usr
cp "$package" "$work/assets/$prefix.pkg.tar.zst"
cp "$work/$archive" "$work/assets/$prefix-source.tar.gz"
printf '%s\n' "$commit" > "$work/assets/$prefix-source-commit.txt"
{
    printf 'cast %s\nSource commit: %s\nRelease tag: %s\nArchitecture: %s\n' "$version" "$commit" "$tag" "$arch"
    printf 'Backend features: X11=1 WAYLAND=1 PANEL=1\nChecks: make check, check-xorg, check-panel\n'
    printf '\nArch build metadata:\n'
    bsdtar -xOf "$package" .BUILDINFO
    printf '\nDirect runtime libraries:\n'
    readelf -d "$work/stage/usr/bin/cast" | sed -n '/(NEEDED)/p'
} > "$work/assets/$prefix-build-info.txt"
(cd "$work/assets" && sha256sum "$prefix.pkg.tar.zst" "$prefix.tar.gz" "$prefix-source.tar.gz" \
    "$prefix-build-info.txt" "$prefix-source-commit.txt" > "$prefix-SHA256SUMS")
(cd "$work/assets" && sha256sum -c "$prefix-SHA256SUMS")
# A failed build leaves no release payloads. Existing payloads are never replaced.
for artifact in "$work/assets/"*; do
    name=${artifact##*/}
    [ ! -e "$output/$name" ] || fail "artifact appeared while building: $name"
    mv "$artifact" "$output/$name"
done
printf 'Arch release artifacts: %s/%s*\n' "$output" "$prefix"
