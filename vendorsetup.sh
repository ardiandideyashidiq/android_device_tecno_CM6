#!/bin/bash

if [[ "${BASH_SOURCE[0]}" != "${0}" ]]; then
  _EXIT=return
else
  _EXIT=exit
fi

log()   { echo "[$(date '+%H:%M:%S')] $*"; }
error() { echo "[$(date '+%H:%M:%S')] ERROR: $*" >&2; }

RET=0

apply_patch() {
  local patch="$1" name="$2"

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

  # Self-heal: git am failed. Restore the repo to its original head (the HEAD that was current
  # before we touched anything -- this preserves any earlier patches in the series while
  # discarding the dirty working-tree / index state that made git am fail, e.g.
  # "file does not match index"), then retry the patch once so a transient failure never
  # leaves the tree in a stuck/conflicted state.
  local base
  base=$(git -C "$repo" rev-parse HEAD 2>/dev/null || true)
  log "  $name: git am failed; resetting $repo to $base and retrying …"
  git -C "$repo" am --abort &>/dev/null || true
  if [ -n "$base" ]; then
    git -C "$repo" reset --hard "$base" &>/dev/null || true
  fi
  # A failed git am can leave a stale .git/rebase-apply, which makes every subsequent
  # git am fail with "previous rebase directory still exists". Remove it so the retry
  # below runs from a genuinely clean state.
  rm -rf "$repo/.git/rebase-apply"

  # Reset rc so a successful retry (which does not set rc) is detected as success,
  # instead of carrying over the previous failure's rc=128.
  unset rc || true
  output=$(git -C "$repo" am "$patch" 2>&1) || rc=$?
  if [ -z "${rc:-}" ] || [ "$rc" -eq 0 ]; then
    local sha
    sha=$(git -C "$repo" log --oneline -1 2>/dev/null || true)
    log "  $name: OK after reset+retry ($sha)"
    return 0
  fi
  git -C "$repo" am --abort &>/dev/null || true

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
for patch_file in "$PWD/device/tecno/CM6/patches/system/core/"*.patch; do
  [ -e "$patch_file" ] || continue
  name=$(basename "$patch_file" .patch)
  apply_patch "$patch_file" "$name" || RET=1
done

repo="$PWD/system/sepolicy"
for patch_file in "$PWD/device/tecno/CM6/patches/system/sepolicy/"*.patch; do
  [ -e "$patch_file" ] || continue
  name=$(basename "$patch_file" .patch)
  apply_patch "$patch_file" "$name" || RET=1
done

repo="$PWD/frameworks/base"
for patch_file in "$PWD/device/tecno/CM6/patches/frameworks/base/"*.patch; do
  [ -e "$patch_file" ] || continue
  name=$(basename "$patch_file" .patch)
  apply_patch "$patch_file" "$name" || RET=1
done

echo ""

if [ "$RET" -ne 0 ]; then
  error "One or more patches failed to apply"
  eval "$_EXIT 1"
else
  log "All patches applied successfully"
fi
