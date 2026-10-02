#!/bin/sh
# Build from the exact published tag, then attach explicit assets with gh.
set -eu

fail() { echo "release: $*" >&2; exit 1; }

action=${1:-}
version=${2:-}
notes=${3:-}
x11=${4:-1}
wayland=${5:-0}
panel=${7:-0}
case "$action" in check|release) ;; *) fail 'expected check or release' ;; esac
case "$version" in ''|*[!0-9A-Za-z.-]*) fail 'invalid release version' ;; esac
case "$x11:$wayland" in 0:0|0:1|1:0|1:1) ;; *) fail 'X11 and WAYLAND must be 0 or 1' ;; esac
case "$panel" in 0|1) ;; *) fail 'PANEL must be 0 or 1' ;; esac
tag=${6:-v$version}
git check-ref-format "refs/tags/$tag" >/dev/null || fail 'invalid RELEASE_TAG'
for command in git gh make tar sha256sum mktemp; do
    command -v "$command" >/dev/null || fail "required command missing: $command"
done
root=$(git rev-parse --show-toplevel)
cd "$root"
[ -n "$notes" ] && [ -f "$notes" ] && [ -s "$notes" ] || fail 'set RELEASE_NOTES to a nonempty release notes file'
case "$notes" in /*) ;; *) notes=$root/$notes ;; esac

check_release() {
    [ -z "$(git status --porcelain)" ] || fail 'commit or remove all tracked and untracked changes first'
    header_version=$(sed -n 's/^#define CAST_VERSION "\([^"]*\)"/\1/p' src/cast.h)
    [ "$version" = "$header_version" ] || fail 'VERSION must match CAST_VERSION in src/cast.h'
    commit=$(git rev-parse HEAD)
    tag_commit=$(git rev-parse --verify "refs/tags/$tag^{commit}") || fail "create the local $tag tag first"
    [ "$commit" = "$tag_commit" ] || fail "$tag must point to HEAD"
    remote_url=$(git remote get-url origin)
    # Resolve GitHub's repository explicitly; GH_REPO cannot redirect publication.
    case "$remote_url" in
        git@github.com:*) repo=${remote_url#git@github.com:} ;;
        https://github.com/*) repo=${remote_url#https://github.com/} ;;
        ssh://git@github.com/*) repo=${remote_url#ssh://git@github.com/} ;;
        *) fail 'origin must be a GitHub SSH or HTTPS URL' ;;
    esac
    repo=${repo%.git}
    repo=${repo%/}
    gh repo view "https://github.com/$repo" --json nameWithOwner --jq .nameWithOwner >/dev/null
    remote_tags=$(git ls-remote --tags origin "refs/tags/$tag" "refs/tags/$tag^{}")
    remote_commit=$(printf '%s\n' "$remote_tags" | awk '$2 ~ /\^\{\}$/ { print $1; found=1 } END { if (!found) print first } NR == 1 { first=$1 }')
    [ "$remote_commit" = "$commit" ] || fail "push $tag to origin with the same commit first"
    releases=$(gh api "repos/$repo/releases" --hostname github.com --paginate --jq '.[].tag_name')
    if printf '%s\n' "$releases" | grep -Fx "$tag" >/dev/null; then
        fail "$tag already has a release; existing releases and assets are never replaced"
    fi
}

check_release
printf 'Release checks passed: %s at %s (%s), X11=%s WAYLAND=%s PANEL=%s\n' "$tag" "$commit" "$repo" "$x11" "$wayland" "$panel"
[ "$action" = release ] || exit 0

arch=$(uname -m)
output=$root/dist/releases/$tag/linux-$arch
[ ! -e "$output" ] || fail "$output already exists; preserve it or move it aside before rebuilding"
mkdir -p "$root/dist/releases/$tag"
work=$(mktemp -d "$root/dist/releases/.build-XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
source_name=cast-$version-source.tar.gz
binary_name=cast-$version-linux-$arch.tar.gz
dependencies_name=cast-$version-dependency-sources.tar.gz
git archive --format=tar.gz --prefix="cast-$version/" "$commit" > "$work/$source_name"
tar -xzf "$work/$source_name" -C "$work"
cp "$notes" "$work/release-notes.md"
# This fresh tree cannot reuse objects or binaries from the developer checkout.
make -C "$work/cast-$version" X11="$x11" WAYLAND="$wayland" PANEL="$panel" VERSION="$version" SOURCE_COMMIT="$commit" package
mkdir "$work/assets"
sh "$work/cast-$version/packaging/dependency-sources.sh" "$work/assets" "$version"
mv "$work/cast-$version/dist/$binary_name" "$work/assets/$binary_name"
mv "$work/$source_name" "$work/assets/$source_name"
(cd "$work/assets" && sha256sum "$binary_name" "$source_name" "$dependencies_name" > SHA256SUMS)
# Recheck after building in case HEAD, the tag, or the remote changed meanwhile.
check_release
mkdir "$output"
cp "$work/assets/$binary_name" "$work/assets/$source_name" "$work/assets/$dependencies_name" "$work/assets/SHA256SUMS" "$output/"
cp "$work/release-notes.md" "$output/release-notes.md"
printf '%s\n' "$commit" > "$output/source-commit.txt"
(cd "$output" && sha256sum -c SHA256SUMS)
gh release create "$tag" "$output/$binary_name" "$output/$source_name" "$output/$dependencies_name" "$output/SHA256SUMS" \
    --repo "https://github.com/$repo" --verify-tag --fail-on-no-commits --title "cast $tag" --notes-file "$output/release-notes.md"
printf 'Published %s with source, binary, dependency sources and checksums. Local artifacts: %s\n' "$tag" "$output"
