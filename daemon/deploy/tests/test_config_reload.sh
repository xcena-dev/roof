#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# test_config_reload -- an edit under /etc reaches a running daemon on SIGHUP.
#
# The daemon reads its identity rules and its policy where they are. Each half adds one comment to
# one file, sends SIGHUP to one instance and waits for that instance's load line to name the new size.
# It then puts the file back and waits for the old size. A comment changes the size and no decision.
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

CONFIG_DIR="/etc/${FS_NAME}"
[[ -d "$CONFIG_DIR" ]] || skip "${CONFIG_DIR} is not there, so this host carries no deployment"
sudo -n true 2> /dev/null || skip "editing ${CONFIG_DIR} needs sudo without a password"
UNIT="$(systemctl list-units --no-legend --plain --state=active "${DAEMON_NAME}@*.service" 2> /dev/null | awk 'NR == 1 {print $1}')"
[[ -n "$UNIT" ]] || skip "no ${DAEMON_NAME} instance is running"

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

# Sends SIGHUP to the unit's main process and checks that a load line tagged @tag names @bytes.
reloadAndCheck() {
  local tag="$1" bytes="$2" what="$3" cursor
  cursor="$(readCursor)"
  if [[ -z "$cursor" ]]; then
    fail "${what}: the journal holds no line for ${UNIT}, so no load line can be read"
    return
  fi
  sudo systemctl kill --kill-whom=main -s HUP "$UNIT"
  if awaitLine "$cursor" "^\[${tag}\] loaded .*\(${bytes} bytes\)"; then
    pass "$what"
  else
    fail "$what"
  fi
}

# One half: edit @target by a comment, reload, put it back, reload again.
checkFile() {
  local target="$1" tag="$2" original edited
  original="$(stat -c '%s' "$target")"
  if ! sudo cp -p "$target" "${SCRATCH}/$(basename "$target")"; then
    fail "${target}: could not keep a copy to put back"
    return
  fi
  CHANGED="$target"
  # The leading newline keeps the comment off a last line that has none, where it would join that value.
  printf '\n# test_config_reload %s\n' "$(date +%s%N)" | sudo tee -a "$target" > /dev/null
  edited="$(stat -c '%s' "$target")"
  reloadAndCheck "$tag" "$edited" "${target}: SIGHUP after an edit loads the edited file (${edited} bytes)"

  sudo cp -p "${SCRATCH}/$(basename "$target")" "$target"
  CHANGED=""
  reloadAndCheck "$tag" "$original" "${target}: SIGHUP after putting it back loads the original (${original} bytes)"
}

# Only the file-driven backends read these files, so a half whose backend is another one is not run.
backendIs() {
  sudo grep -qE "^[[:space:]]*$1:[[:space:]]*local" "${CONFIG_DIR}/daemon.yaml"
}

if backendIs identity; then
  checkFile "${CONFIG_DIR}/identity-rules.yaml" identity-local
fi
if backendIs policy; then
  checkFile "${CONFIG_DIR}/policy.rego" policy-local
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
