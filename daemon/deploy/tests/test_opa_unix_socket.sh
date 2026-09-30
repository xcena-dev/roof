#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# test_opa_unix_socket -- the deployment puts the policy server on a path, not on a loopback port.
#
# A port above 1024 is any account's to hold first and nothing in the exchange names the server, so
# what the daemon relies on is the endpoint sitting under /run, which is root's. These checks read
# the shipped unit, the shipped config and the bring-up path, and a live host's rendered unit and
# socket when there is one.
#
#   ctest --test-dir <build> -R test_opa_unix_socket

set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
DEPLOY="$(cd "${HERE}/.." && pwd)"
ROOT="$(cd "${DEPLOY}/../.." && pwd)"
. "${ROOT}/fsname"

INSTALL_DEPS="${DEPLOY}/install-deps.sh"
CONFIG_TEMPLATE="${DEPLOY}/daemon/daemon.example.yaml.in"
HELPER_UNIT_TEMPLATE="${DEPLOY}/daemon/daemon@.service.in"

pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; FAILED=$((FAILED + 1)); }
skip() { printf 'SKIP %s\n' "$*"; exit 77; }

for needed in "$INSTALL_DEPS" "$CONFIG_TEMPLATE" "$HELPER_UNIT_TEMPLATE"; do
  [[ -f "$needed" ]] || skip "missing $needed"
done

FAILED=0
SOCKET_PATH="/run/${FS_NAME}-opa/api.sock"

# The unit install-deps.sh writes for OPA, rendered the way that function renders it.
caseUnitNamesASocket() {
  local block
  block="$(sed -n '/^install_systemd()/,/^}/p' "$INSTALL_DEPS")"

  # The name is still a shell variable in the source, so these match the text and not the value.
  if [[ "$block" == *'--addr unix:///run/${FS_NAME}-opa/api.sock'* ]]; then
    pass "the OPA unit listens on ${SOCKET_PATH}"
  else
    fail "the OPA unit listens on ${SOCKET_PATH}"
  fi

  if [[ "$block" != *"--addr 127.0.0.1"* ]]; then
    pass "the OPA unit names no loopback port"
  else
    fail "the OPA unit names no loopback port"
  fi

  if [[ "$block" == *'RuntimeDirectory=${FS_NAME}-opa'* ]]; then
    pass "systemd owns the directory the endpoint sits in"
  else
    fail "systemd owns the directory the endpoint sits in"
  fi
}

# The bring-up path is a second way onto this host, so it has to reach the same endpoint.
caseBringUpNamesTheSameSocket() {
  local block
  block="$(sed -n '/^bringup_opa()/,/^}/p' "$INSTALL_DEPS")"

  if [[ "$block" == *'--addr "unix://$BRINGUP_OPA_SOCK"'* ]]; then
    pass "the bring-up listens on a socket"
  else
    fail "the bring-up listens on a socket"
  fi

  if [[ "$block" == *"--unix-socket"* ]]; then
    pass "the bring-up health check goes over that socket"
  else
    fail "the bring-up health check goes over that socket"
  fi

  if grep -qF 'BRINGUP_OPA_DIR="${BRINGUP_OPA_DIR:-/run/${FS_NAME}-opa}"' "$INSTALL_DEPS"; then
    pass "the bring-up endpoint is the one the config names"
  else
    fail "the bring-up endpoint is the one the config names"
  fi
}

# The daemon has to be pointed at the endpoint, and has to be allowed to reach it.
caseDaemonIsPointedAtIt() {
  if grep -q "^policy_socket: *${SOCKET_PATH//\//\\/}$" \
       <(sed "s|@FS_NAME@|${FS_NAME}|g" "$CONFIG_TEMPLATE"); then
    pass "the shipped config names ${SOCKET_PATH}"
  else
    fail "the shipped config names ${SOCKET_PATH}"
  fi

  # A path exception is a bind mount of its own, and it would keep pointing at the directory object
  # the policy server's unit removes and remakes on every restart.
  if ! grep -qE '^(ReadWritePaths|ReadOnlyPaths|BindPaths|BindReadOnlyPaths)=.*@FS_NAME@-opa' \
         "$HELPER_UNIT_TEMPLATE"; then
    pass "the daemon's unit mounts nothing of its own over that directory"
  else
    fail "the daemon's unit mounts that directory into its namespace, so a restarted policy server leaves it on the old one"
  fi

  if grep -qE '^After=.*opa\.service' "$HELPER_UNIT_TEMPLATE"; then
    pass "the daemon starts after the policy server"
  else
    fail "the daemon starts after the policy server"
  fi
}

# Who may create the endpoint and who may query it. Widening either undoes what the path buys.
caseEndpointIsNotWorldReachable() {
  local block
  block="$(sed -n '/^install_systemd()/,/^}/p' "$INSTALL_DEPS")"

  if [[ "$block" == *'RuntimeDirectoryMode=${opaDirMode}'* && "$block" == *'UMask=${opaMask}'* ]]; then
    pass "the unit sets the directory mode and the socket's umask"
  else
    fail "the unit sets the directory mode and the socket's umask"
  fi

  # Every value the branch can choose, so a widening edit shows up here and not on a host.
  local widestDir
  widestDir="$(printf '%s\n' "$block" | sed -n 's/^ *opaDirMode="\([0-7]*\)"$/\1/p' | sort | tail -1)"
  if [[ "$widestDir" == 0700 || "$widestDir" == 0750 ]]; then
    pass "no branch leaves the directory open to every account (widest ${widestDir})"
  else
    fail "a branch leaves the directory at mode '${widestDir}'"
  fi

  local weakestMask
  weakestMask="$(printf '%s\n' "$block" | sed -n 's/^ *opaMask="\([0-7]*\)"$/\1/p' | sort | head -1)"
  if [[ "$weakestMask" == 0007 || "$weakestMask" == 0077 ]]; then
    pass "no branch leaves the socket open to every account (weakest umask ${weakestMask})"
  else
    fail "a branch leaves the socket at umask '${weakestMask}'"
  fi
}

# What is actually installed here, when anything is. A host with no OPA reports the checks above
# and says nothing about a unit it does not carry.
caseInstalledUnit() {
  local installed=/etc/systemd/system/opa.service
  if [[ ! -f "$installed" ]]; then
    printf 'note: no %s on this host, so only the shipped files were checked\n' "$installed"
    return
  fi

  if grep -q -- "--addr unix://" "$installed"; then
    pass "the installed OPA unit listens on a socket"
  else
    fail "the installed OPA unit still listens on a port; reinstall with --systemd --force"
  fi
}

# The directory above the endpoint is what keeps another account from answering first.
caseDirectoryIsRoots() {
  local home="/run/${FS_NAME}-opa"
  if [[ ! -d "$home" ]]; then
    printf 'note: %s is not present, so its ownership was not checked\n' "$home"
    return
  fi

  local owner mode
  owner="$(stat -c '%U' "$home")"
  mode="$(stat -c '%a' "$home")"
  if [[ "$owner" == root ]]; then
    pass "${home} belongs to root"
  else
    fail "${home} belongs to ${owner}, so that account can answer in OPA's place"
  fi

  # The last digit is what an account outside the owner and the group may do there.
  if [[ "${mode: -1}" != *[2367]* ]]; then
    pass "${home} is not writable by every account (mode ${mode})"
  else
    fail "${home} is writable by every account (mode ${mode})"
  fi
}

caseUnitNamesASocket
caseBringUpNamesTheSameSocket
caseDaemonIsPointedAtIt
caseEndpointIsNotWorldReachable
caseInstalledUnit
caseDirectoryIsRoots

if [[ $FAILED -eq 0 ]]; then
  printf 'every check passed\n'
  exit 0
fi
printf '%d checks failed\n' "$FAILED"
exit 1
