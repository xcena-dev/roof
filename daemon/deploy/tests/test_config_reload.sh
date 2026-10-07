#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# test_config_reload -- an edit under /etc reaches a running daemon on SIGHUP, or is refused whole.
#
# Each half edits one file, sends SIGHUP to one instance and waits for the line that reload logs,
# then puts the file back and waits again. A comment changes the size and no decision.
#
#   ctest --test-dir <build> -R test_config_reload

set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
DEPLOY="$(cd "${HERE}/.." && pwd)"
ROOT="$(cd "${DEPLOY}/../.." && pwd)"
. "${ROOT}/fsname"

pass() { printf 'PASS %s\n' "$*"; PASSED=$((PASSED + 1)); }
fail() { printf 'FAIL %s\n' "$*"; FAILED=$((FAILED + 1)); }
skip() { printf 'SKIP %s\n' "$*"; exit 77; }

CONFIG="/etc/${FS_NAME}/config.yaml"
[[ -f "$CONFIG" ]] || skip "${CONFIG} is not there, so this host carries no deployment"
sudo -n true 2> /dev/null || skip "editing ${CONFIG} needs sudo without a password"
UNIT="$(systemctl list-units --no-legend --plain --state=active "${DAEMON_NAME}@*.service" 2> /dev/null | awk 'NR == 1 {print $1}')"
[[ -n "$UNIT" ]] || skip "no ${DAEMON_NAME} instance is running"

DAEMON="$(command -v "${DAEMON_NAME}" || printf '/usr/local/bin/%s' "${DAEMON_NAME}")"
# The file's owner is the daemon account, the one account the daemon takes its config from.
OWNER="$(sudo stat -c '%U' "$CONFIG")"

PASSED=0
FAILED=0
SCRATCH="$(mktemp -d)"
# The file this run has changed and not put back yet, so an exit at any point restores it.
CHANGED=""

putBack() {
  if [[ -n "$CHANGED" ]]; then
    sudo cp -p "${SCRATCH}/$(basename "$CHANGED")" "$CHANGED"
    sudo systemctl kill --kill-whom=main -s HUP "$UNIT"
  fi
  rm -rf "$SCRATCH"
}
trap putBack EXIT

askDaemon() {
  sudo -u "$OWNER" "$DAEMON" --config "$CONFIG" "$@"
}

# The journal position of the unit's last line, so a wait reads only what the next reload logs.
readCursor() {
  sudo journalctl -u "$UNIT" -n 1 --show-cursor -o cat 2> /dev/null | sed -n 's/^-- cursor: //p'
}

# Waits up to five seconds for a line after @cursor that matches the extended regex @pattern.
awaitLine() {
  local cursor="$1" pattern="$2" attempt
  for attempt in $(seq 50); do
    if sudo journalctl -u "$UNIT" --after-cursor "$cursor" -o cat 2> /dev/null | grep -qE "$pattern"; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

# Sends SIGHUP to the unit's main process and checks that a line after it matches @pattern.
reloadAndCheck() {
  local pattern="$1" what="$2" cursor
  cursor="$(readCursor)"
  if [[ -z "$cursor" ]]; then
    fail "${what}: the journal holds no line for ${UNIT}, so no reload line can be read"
    return
  fi
  sudo systemctl kill --kill-whom=main -s HUP "$UNIT"
  if awaitLine "$cursor" "$pattern"; then
    pass "$what"
  else
    fail "$what"
  fi
}

keepCopy() {
  local target="$1"
  if ! sudo cp -p "$target" "${SCRATCH}/$(basename "$target")"; then
    fail "${target}: could not keep a copy to put back"
    return 1
  fi
  CHANGED="$target"
}

restore() {
  sudo cp -p "${SCRATCH}/$(basename "$1")" "$1"
  CHANGED=""
}

# @target edited by a comment and put back, each followed by a load line @prefix names at that size.
# The leading newline keeps the comment off a last line that has none, where it would join that value.
checkCommentEdit() {
  local target="$1" prefix="$2" original edited
  original="$(sudo stat -c '%s' "$target")"
  keepCopy "$target" || return
  printf '\n# test_config_reload %s\n' "$(date +%s%N)" | sudo tee -a "$target" > /dev/null
  edited="$(sudo stat -c '%s' "$target")"
  reloadAndCheck "${prefix}.*\(${edited} bytes\)" "${target}: SIGHUP after an edit loads the edited file (${edited} bytes)"
  restore "$target"
  reloadAndCheck "${prefix}.*\(${original} bytes\)" "${target}: SIGHUP after putting it back loads the original (${original} bytes)"
}

# A key the daemon was built from, set to a value it does not run with. The reload is refused and
# names the key, and putting the file back loads it again.
checkRestartKey() {
  local original
  if sudo grep -qE '^turn:' "$CONFIG"; then
    printf 'SKIP %s already holds a turn block, so no key can be added without editing one\n' "$CONFIG"
    return
  fi
  original="$(sudo stat -c '%s' "$CONFIG")"
  keepCopy "$CONFIG" || return
  printf '\nturn:\n  strategy: ticket\n' | sudo tee -a "$CONFIG" > /dev/null
  reloadAndCheck '^\[config\] reload refused \(turn\.strategy changed' \
    "${CONFIG}: SIGHUP after a restart key changes refuses the reload and names the key"
  restore "$CONFIG"
  reloadAndCheck "^\[identity-local\] loaded .*\(${original} bytes\)" \
    "${CONFIG}: SIGHUP after putting it back loads it again (${original} bytes)"
}

identityBackend="$(askDaemon --print-config identity.backend)"
policyBackend="$(askDaemon --print-config policy.backend)"

if [[ "$identityBackend" == local ]]; then
  # The rule count the file holds now, which a comment must leave as it is.
  rules="$(askDaemon --check-config 2>&1 | sed -n 's/^\[identity-local\] loaded \([0-9]*\) rules.*/\1/p' | head -n 1)"
  if [[ -z "$rules" ]]; then
    fail "${CONFIG}: --check-config logged no rule count"
  else
    checkCommentEdit "$CONFIG" "^\[identity-local\] loaded ${rules} rules "
  fi
  checkRestartKey
fi
if [[ "$policyBackend" == local ]]; then
  checkCommentEdit "$(askDaemon --print-config policy.path)" '^\[policy-local\] loaded '
fi

if [[ $FAILED -eq 0 && $PASSED -eq 0 ]]; then
  skip "neither backend is the file-driven one, so no file here is read by the daemon"
fi
if [[ $FAILED -eq 0 ]]; then
  printf 'every check made passed\n'
  exit 0
fi
printf '%d checks failed\n' "$FAILED"
exit 1
