#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# test_deploy_state -- the decisions the deploy scripts record and read back, without root.
#
# Which group install.sh records for the device, which one uninstall.sh puts back, which device
# group leftovers.sh counts, and which mount points uninstall.sh removes. Each is a function the
# scripts share, so this drives them on scratch files.
#
#   ctest --test-dir <build> -R test_deploy_state

set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
. "${ROOT}/fsname"
. "${ROOT}/shell/deploy_state.sh"

PASSED=0
FAILED=0

check() {
  local what="$1" want="$2" got="$3"
  if [[ "$got" == "$want" ]]; then
    printf 'PASS %s\n' "$what"
    PASSED=$((PASSED + 1))
  else
    printf 'FAIL %s: want [%s], read [%s]\n' "$what" "$want" "$got"
    FAILED=$((FAILED + 1))
  fi
}

# Whether @path exists, as yes or no, so a check can compare it.
readPresence() {
  [[ -e "$1" ]] && printf 'yes' || printf 'no'
}

# A gid no group names, found rather than assumed, since a host can hand out any number.
findUnnamedGid() {
  local gid
  for gid in $(seq 64000 65000); do
    getent group "$gid" > /dev/null || { printf '%s' "$gid"; return; }
  done
}

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT
NAMELESS="$(findUnnamedGid)"
OWN_GID="$(id -g)"

# ── the group uninstall.sh puts back ─────────────────────────────────────
check "a record of this tree's own group puts root back" 0 "$(chooseRestoredGroup "$FS_NAME")"
check "a record of UNKNOWN puts root back" 0 "$(chooseRestoredGroup UNKNOWN)"
check "a record of a gid no group names puts root back" 0 "$(chooseRestoredGroup "$NAMELESS")"
check "a record of a named gid puts that gid back" "$OWN_GID" "$(chooseRestoredGroup "$OWN_GID")"
check "a record of a group name puts that name back" root "$(chooseRestoredGroup root)"

# ── the group install.sh records ─────────────────────────────────────────
touch "${SCRATCH}/device"
check "a device's named gid is recorded as itself" "$(stat -c '%g' "${SCRATCH}/device")" \
  "$(readOriginalGroup "${SCRATCH}/device")"
check "a device with no node records root" 0 "$(readOriginalGroup "${SCRATCH}/absent")"
# Only root can hand a file a gid it is not a member of, so this case needs root.
if [[ "$(id -u)" -eq 0 && -n "$NAMELESS" ]]; then
  chgrp "$NAMELESS" "${SCRATCH}/device"
  check "a device whose gid no group names records root" 0 "$(readOriginalGroup "${SCRATCH}/device")"
fi

# ── the device groups leftovers.sh counts ────────────────────────────────
leftover=no
isLeftoverGroup "$FS_NAME" && leftover=yes
check "this tree's own group on a device is a leftover" yes "$leftover"
leftover=no
isLeftoverGroup UNKNOWN && leftover=yes
check "a gid whose group is gone is a leftover" yes "$leftover"
leftover=no
isLeftoverGroup root && leftover=yes
check "root on a device is not a leftover" no "$leftover"

# ── the mount points uninstall.sh removes ────────────────────────────────
mkdir "${SCRATCH}/empty" "${SCRATCH}/full" "${SCRATCH}/operator"
touch "${SCRATCH}/full/kept"
report="$(removeCreatedMounts test "${SCRATCH}/empty" "${SCRATCH}/full" "${SCRATCH}/never")"
check "a created mount point that is empty is removed" no "$(readPresence "${SCRATCH}/empty")"
check "a created mount point with something in it stays" yes "$(readPresence "${SCRATCH}/full")"
check "a mount point nobody listed stays" yes "$(readPresence "${SCRATCH}/operator")"
check "the report says the full one stayed" yes \
  "$(grep -q "${SCRATCH}/full is not empty; it stays" <<< "$report" && printf 'yes' || printf 'no')"
check "a listed path that is not there is passed over in silence" no \
  "$(grep -q "${SCRATCH}/never" <<< "$report" && printf 'yes' || printf 'no')"

printf '%d passed, %d failed\n' "$PASSED" "$FAILED"
[[ $FAILED -eq 0 ]]
