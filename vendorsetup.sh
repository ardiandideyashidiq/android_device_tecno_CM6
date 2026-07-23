#!/bin/bash

if [[ "${BASH_SOURCE[0]}" != "${0}" ]]; then
  _EXIT=return
else
  _EXIT=exit
fi

log()   { echo "[$(date '+%H:%M:%S')] $*"; }
error() { echo "[$(date '+%H:%M:%S')] ERROR: $*" >&2; }

PATCH_DIR=$(mktemp -d /tmp/fenrir_patches.XXXXXX)
trap 'rm -rf "$PATCH_DIR"' EXIT

RET=0

apply_patch() {
  local url="$1" name="$2"
  local patch="$PATCH_DIR/$name.patch"

  log "  Downloading $name …"
  curl -sSfL "$url" -o "$patch" || { error "Download failed for $name"; return 1; }

  # pre-check: reverse apply succeeds → already present
  if git -C "$repo" apply --check --reverse "$patch" &>/dev/null; then
    log "  $name: already applied (skipped)"
    return 0
  fi

  log "  Applying $name …"
  local output rc
  output=$(git -C "$repo" am "$patch" 2>&1) || rc=$?

  # success
  if [ -z "${rc:-}" ] || [ "$rc" -eq 0 ]; then
    local sha
    sha=$(git -C "$repo" log --oneline -1 2>/dev/null || true)
    log "  $name: OK ($sha)"
    return 0
  fi

  # already applied (git am may have failed, but reverse apply works)
  git -C "$repo" am --abort &>/dev/null || true
  if git -C "$repo" apply --check --reverse "$patch" &>/dev/null; then
    log "  $name: already applied (skipped)"
    return 0
  fi

  # real failure
  if echo "$output" | grep -qi "conflict"; then
    error "$name: merge conflict -- needs manual rebase"
  else
    error "$name: failed (rc=$rc)"
  fi
  echo "$output" | sed 's/^/    /' >&2

  return 1
}

# ── main ──

log "Applying fenrir compatibility patches"

repo="$PWD/system/core"

for url in \
  "https://raw.githubusercontent.com/MillenniumOSS/patches/refs/heads/sixteen/system/core/0001-libfs_avb-Allow-LKs-patched-with-fenrir-to-boot-on-A.patch" \
  "https://raw.githubusercontent.com/MillenniumOSS/patches/refs/heads/sixteen/system/core/0002-fastbootd-Always-return-false-for-GetDeviceLockStatu.patch"
do
  name=$(basename "$url" .patch)
  name="${name:0:50}"
  apply_patch "$url" "$name" || RET=1
done

echo ""

if [ "$RET" -ne 0 ]; then
  error "One or more patches failed to apply"
  eval "$_EXIT 1"
else
  log "All patches applied successfully"
fi
