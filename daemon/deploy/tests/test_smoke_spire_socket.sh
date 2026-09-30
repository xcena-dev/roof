#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# test_smoke_spire_socket -- smoke-spire.sh hands the workload API socket to one account at
# mode 0600, not to the world.
#
# Needs a real spire-server/spire-agent and real root, so it only runs on a host that has both;
# elsewhere it skips. The socket is gone by the time smoke-spire.sh exits (the agent removes it
# on shutdown), so this reads smoke-spire.sh's own step 3 line as it streams through a FIFO and
# stats the socket while the run is still live.
#
#   ctest --test-dir <build> -R test_smoke_spire_socket

set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
SMOKE_SPIRE="$(cd "${HERE}/../identity" && pwd)/smoke-spire.sh"

skip() { printf 'SKIP %s\n' "$*"; exit 77; }
pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; FAILED=$((FAILED + 1)); }

missing=()
[[ $EUID -eq 0 ]] || missing+=("real root (EUID=$EUID)")
[[ -x /usr/local/bin/spire-server ]] || missing+=("/usr/local/bin/spire-server")
[[ -x /usr/local/bin/spire-agent ]] || missing+=("/usr/local/bin/spire-agent")
[[ ${#missing[@]} -eq 0 ]] || skip "smoke-spire.sh needs: ${missing[*]}"

FAILED=0
PIPE="$(mktemp -u)"
mkfifo "$PIPE"
trap 'rm -f "$PIPE"' EXIT

"$SMOKE_SPIRE" >"$PIPE" 2>&1 &
smokePid=$!

SOCKET=""
ACTUAL_MODE=""
ACTUAL_UID=""
while IFS= read -r line; do
  printf '%s\n' "$line"
  if [[ -z "$SOCKET" && "$line" == *"Workload API socket:"* ]]; then
    # smoke-spire.sh colors this line, and the socket is gone once it moves past step 3.
    SOCKET="$(printf '%s' "$line" \
      | sed -e 's/\x1b\[[0-9;]*m//g' -e 's/.*Workload API socket: //' -e 's/[[:space:]]*$//')"
    ACTUAL_MODE="$(stat -c '%a' "$SOCKET" 2>/dev/null)"
    ACTUAL_UID="$(stat -c '%u' "$SOCKET" 2>/dev/null)"
  fi
done <"$PIPE"
wait "$smokePid"
smokeStatus=$?

if [[ $smokeStatus -ne 0 ]]; then
  fail "smoke-spire.sh exited $smokeStatus"
elif [[ -z "$SOCKET" ]]; then
  fail "never saw a Workload API socket line from smoke-spire.sh"
else
  EXPECTED_UID="${SUDO_UID:-$(id -u)}"
  if [[ "$ACTUAL_MODE" == "600" ]]; then
    pass "socket mode is 0600"
  else
    fail "socket mode is 0600, read $ACTUAL_MODE"
  fi
  if [[ "$ACTUAL_UID" == "$EXPECTED_UID" ]]; then
    pass "socket owner uid is $EXPECTED_UID"
  else
    fail "socket owner uid is $EXPECTED_UID, read $ACTUAL_UID"
  fi
fi

if [[ $FAILED -eq 0 ]]; then
  printf 'every check passed\n'
  exit 0
fi
printf '%d checks failed\n' "$FAILED"
exit 1
