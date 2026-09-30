#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# test_config_mode -- who can read the daemon's config, its identity rules and its policy.
#
# The daemon reads the three files under /etc itself, so each is owned by the daemon's own account
# at mode 0400 with root as its group. What keeps them shut is no group bit, no world bit and no ACL
# entry, so this checks the absence of all three: a wider mode or one named entry would open them,
# and a check that read only the owner would not see it.
#
#   ctest --test-dir <build> -R test_config_mode

set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
DEPLOY="$(cd "${HERE}/.." && pwd)"
ROOT="$(cd "${DEPLOY}/../.." && pwd)"
. "${ROOT}/fsname"

pass() { printf 'PASS %s\n' "$*"; PASSED=$((PASSED + 1)); }
fail() { printf 'FAIL %s\n' "$*"; FAILED=$((FAILED + 1)); }
note() { printf 'NOTE %s\n' "$*"; SKIPPED=$((SKIPPED + 1)); }
skip() { printf 'SKIP %s\n' "$*"; exit 77; }

CONFIG_DIR="/etc/${FS_NAME}"
CONFIG_NAMES=(daemon.yaml identity-rules.yaml policy.rego)

[[ -d "$CONFIG_DIR" ]] || skip "${CONFIG_DIR} is not there, so this host carries no deployment"

PASSED=0
FAILED=0
SKIPPED=0

# The account the daemon runs as, which is the one account these files may open to. A loaded unit
# answers first, and the shipped template's account is the fallback for a host with none loaded.
expectedUser() {
  local unit named=""
  unit="$(systemctl list-units --no-legend --plain "${DAEMON_NAME}@*.service" 2> /dev/null | awk 'NR == 1 {print $1}')"
  [[ -n "$unit" ]] && named="$(systemctl show -p User --value "$unit" 2> /dev/null)"
  [[ -n "$named" ]] && printf '%s' "$named" && return
  printf '%s' "${FS_NAME}"
}
WANTED="$(expectedUser)"

# ── 1. owner, group and mode ─────────────────────────────────────────────
# stat needs no read permission on the file itself, so this half answers without root.
for name in "${CONFIG_NAMES[@]}"; do
  target="${CONFIG_DIR}/${name}"
  if [[ ! -e "$target" ]]; then
    note "${target} is not there"
    continue
  fi
  read -r owner group mode <<< "$(stat -c '%U %G %a' "$target")"
  if [[ "$owner" == "$WANTED" && "$group" == root && "$mode" == 400 ]]; then
    pass "${target} is 0400 ${WANTED}:root"
  else
    fail "${target} is 0400 ${WANTED}:root, read ${mode} ${owner}:${group}"
  fi
done

# ── 2. the ACL on each file ──────────────────────────────────────────────
if ! command -v getfacl > /dev/null; then
  note "getfacl is not installed, so no ACL was read"
else
  for name in "${CONFIG_NAMES[@]}"; do
    target="${CONFIG_DIR}/${name}"
    [[ -e "$target" ]] || continue
    if ! acl="$(getfacl -p "$target" 2> /dev/null)" || [[ -z "$acl" ]]; then
      note "the ACL on ${target} could not be read"
      continue
    fi
    # A named entry carries a name between the two colons, where an owning entry carries nothing.
    named="$(printf '%s\n' "$acl" | sed -n 's/^\(user\|group\):\([^:][^:]*\):.*$/\1:\2/p' | paste -sd' ')"
    groupEntry="$(printf '%s\n' "$acl" | sed -n 's/^group::\(.*\)$/\1/p')"
    otherEntry="$(printf '%s\n' "$acl" | sed -n 's/^other::\(.*\)$/\1/p')"
    if [[ -z "$named" && "$groupEntry" == "---" && "$otherEntry" == "---" ]]; then
      pass "${target} opens to nobody beyond its owner"
    else
      fail "${target} opens to nobody beyond its owner, read named=[${named}] group::${groupEntry} other::${otherEntry}"
    fi
  done
fi

if [[ $SKIPPED -gt 0 ]]; then
  printf '%d check(s) were not made, so this run did not cover everything\n' "$SKIPPED"
fi
# A config directory with nothing in it reaches no check at all, and that is a skip: a green here
# would claim a permission this run never read.
if [[ $FAILED -eq 0 && $PASSED -eq 0 ]]; then
  skip "no check was reached, so nothing here was measured"
fi
if [[ $FAILED -eq 0 ]]; then
  printf 'every check made passed\n'
  exit 0
fi
printf '%d checks failed\n' "$FAILED"
exit 1
