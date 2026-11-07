#!/bin/bash
#
# Download Freedoom 0.13.0, verify it against the pinned SHA-256 in
# tools/freedoom.sha256, and place freedoom1.wad plus its licence and credits
# in the git-ignored wads/ folder, where the CMake build picks them up.
#
# Usage: tools/fetch-freedoom.sh
#
# Freedoom is BSD-3-Clause, (c) Contributors to the Freedoom project.
# https://freedoom.github.io/
#
set -euo pipefail

readonly VERSION="0.13.0"
readonly ZIP_NAME="freedoom-${VERSION}.zip"
readonly URL="https://github.com/freedoom/freedoom/releases/download/v${VERSION}/${ZIP_NAME}"

die() {
    printf 'fetch-freedoom: %s\n' "$*" >&2
    exit 1
}

# Refuse any path that is empty, relative or the root, or that lacks a marker.
check_dir() {
    local dir="$1" marker="$2"
    [[ -n "$dir" ]] || die "empty path"
    [[ "$dir" == /* ]] || die "not an absolute path: $dir"
    [[ "$dir" != "/" ]] || die "refusing the root directory"
    [[ -e "$dir/$marker" ]] || die "$dir does not contain $marker"
}

sha256_of() {
    shasum -a 256 "$1" | awk '{print $1}'
}

pinned_sha256() {
    local name="$1" value
    value="$(awk -v n="$name" '$2 == n {print $1}' "$PINS")"
    [[ "$value" =~ ^[0-9a-f]{64}$ ]] || die "no pinned SHA-256 for $name in $PINS"
    printf '%s\n' "$value"
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
check_dir "$ROOT" "tools/fetch-freedoom.sh"
check_dir "$ROOT" "CMakeLists.txt"
readonly PINS="$ROOT/tools/freedoom.sha256"
readonly WADS="$ROOT/wads"

ZIP_SHA256="$(pinned_sha256 "$ZIP_NAME")"
WAD_SHA256="$(pinned_sha256 freedoom1.wad)"

mkdir -p "$WADS"
check_dir "$WADS" "."

if [[ -f "$WADS/freedoom1.wad" && -f "$WADS/freedoom-COPYING.txt" \
      && -f "$WADS/freedoom-CREDITS.txt" && -f "$WADS/freedoom-CREDITS-MUSIC.txt" \
      && "$(sha256_of "$WADS/freedoom1.wad")" == "$WAD_SHA256" ]]; then
    printf 'fetch-freedoom: %s already present and verified (sha256 %s)\n' \
        "$WADS/freedoom1.wad" "$WAD_SHA256"
    exit 0
fi

# Cleanup removes only the files this script writes, then the empty dir.
cleanup() {
    rm -f "$WORK/$ZIP_NAME" "$WORK/freedoom1.wad" "$WORK/COPYING.txt" \
        "$WORK/CREDITS.txt" "$WORK/CREDITS-MUSIC.txt"
    rmdir "$WORK" 2>/dev/null || true
}
WORK="$(mktemp -d "$WADS/.fetch-freedoom.XXXXXX")"
trap cleanup EXIT
check_dir "$WORK" "."
[[ "$WORK" == "$WADS"/.fetch-freedoom.* ]] || die "unexpected work dir: $WORK"

printf 'fetch-freedoom: downloading %s\n' "$URL"
curl --fail --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
    --silent --show-error --output "$WORK/$ZIP_NAME" "$URL"

got="$(sha256_of "$WORK/$ZIP_NAME")"
printf 'fetch-freedoom: %s sha256 %s\n' "$ZIP_NAME" "$got"
[[ "$got" == "$ZIP_SHA256" ]] \
    || die "checksum mismatch for $ZIP_NAME: expected $ZIP_SHA256, got $got"

# Extract only the four named members, by exact archive path, to stdout:
# nothing else in the archive is written anywhere.
for member in freedoom1.wad COPYING.txt CREDITS.txt CREDITS-MUSIC.txt; do
    unzip -p "$WORK/$ZIP_NAME" "freedoom-${VERSION}/$member" > "$WORK/$member"
    [[ -s "$WORK/$member" ]] || die "$member missing from $ZIP_NAME"
done

got="$(sha256_of "$WORK/freedoom1.wad")"
[[ "$got" == "$WAD_SHA256" ]] \
    || die "checksum mismatch for freedoom1.wad: expected $WAD_SHA256, got $got"

mv -f "$WORK/COPYING.txt" "$WADS/freedoom-COPYING.txt"
mv -f "$WORK/CREDITS.txt" "$WADS/freedoom-CREDITS.txt"
mv -f "$WORK/CREDITS-MUSIC.txt" "$WADS/freedoom-CREDITS-MUSIC.txt"
mv -f "$WORK/freedoom1.wad" "$WADS/freedoom1.wad"
printf 'fetch-freedoom: installed %s (sha256 %s) with its licence and credits\n' \
    "$WADS/freedoom1.wad" "$WAD_SHA256"
