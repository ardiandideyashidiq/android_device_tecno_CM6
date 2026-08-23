#!/bin/bash
# Apply/revert device-specific framework patches listed in patches.list.
# Idempotent: skips patches already applied; errors if a patch does not fit.
#
# Usage:
#   apply-patches.sh            apply all
#   apply-patches.sh --revert   revert all (reverse order)
#   ANDROID_BUILD_TOP must point to the ROM tree root (or pass it as $1)

set -u

DT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="apply"
TREE=""
for arg in "$@"; do
    case "$arg" in
        --revert) MODE="revert" ;;
        *) [ -z "$TREE" ] && TREE="$arg" ;;
    esac
done
TREE="${TREE:-${ANDROID_BUILD_TOP:-$DT_DIR/../..}}"
LIST="$DT_DIR/patches/patches.list"

if [ ! -f "$LIST" ]; then echo "ERROR: $LIST not found" >&2; exit 1; fi
if [ ! -d "$TREE/frameworks/base" ]; then echo "ERROR: ROM tree not found at $TREE" >&2; exit 1; fi

rc=0
while read -r repo patch; do
    case "$repo" in ''|\#*) continue;; esac
    pfile="$DT_DIR/patches/$patch"
    [ -f "$pfile" ] || { echo "MISSING  $patch" >&2; rc=1; continue; }
    gitcmd=(git -c core.autocrlf=false -C "$TREE/$repo")
    "${gitcmd[@]}" apply --check "$pfile" 2>/dev/null
    if [ $? -eq 0 ]; then
        if [ "$MODE" = "apply" ]; then
            "${gitcmd[@]}" apply "$pfile" && echo "APPLIED  $repo $patch"
        else
            echo "CLEAN    $repo $patch (not applied)"
        fi
        continue
    fi
    "${gitcmd[@]}" apply --reverse --check "$pfile" 2>/dev/null
    if [ $? -eq 0 ]; then
        if [ "$MODE" = "revert" ]; then
            git -C "$TREE/$repo" apply "${flags[@]}" --reverse "$pfile" && echo "REVERTED $repo $patch"
        else
            echo "SKIP     $repo $patch (already applied)"
        fi
        continue
    fi
    echo "FAILED   $repo $patch (does not fit; tree changed?)" >&2
    rc=1
done < "$LIST"
exit $rc
