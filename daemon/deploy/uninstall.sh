#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# uninstall.sh — undo what install.sh did.
#
# Stops the service and removes the binaries, the unit, the udev rule, the modprobe pin and the
# runtime directory.
#
# The configs under /etc stay, because an operator may have edited them, and so does the
# helper account, because another service may share its group.
#
# Usage:
#   sudo bash uninstall.sh
#   sudo bash uninstall.sh --purge      # the configs, the audit log and the account too

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
. "${ROOT}/fsname"
. "${ROOT}/shell/units.sh"

PURGE=0
for arg in "$@"; do
  case "$arg" in
    --purge) PURGE=1 ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown flag: $arg" >&2; exit 2 ;;
  esac
done

[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 1; }

log() { printf '[uninstall] %s\n' "$*"; }

# A mount clears its rows on unmount under a turn only its daemon can take, so no daemon goes first.
remaining="$(mounted_points)"
if [[ -n "$remaining" ]]; then
  echo "${FS_NAME} is still mounted at:" >&2
  echo "$remaining" | sed 's/^/  /' >&2
  echo "unmount it first; the umount helper stops its daemon" >&2
  exit 1
fi

if systemctl list-unit-files "${DAEMON_NAME}@.service" &>/dev/null; then
  log "stopping + disabling every ${DAEMON_NAME} instance"
  systemctl stop "${DAEMON_NAME}@*.service" 2>/dev/null || true
  systemctl disable "${DAEMON_NAME}@*.service" 2>/dev/null || true
fi

for f in \
  /etc/systemd/system/${DAEMON_NAME}@.service \
  /etc/systemd/system/${DAEMON_NAME}.service \
  /etc/udev/rules.d/99-${DAEMON_NAME}.rules \
  /etc/modprobe.d/${FS_NAME}.conf \
  /usr/local/bin/${DAEMON_NAME}
do
  if [[ -e "$f" ]]; then
    log "rm $f"
    rm -f "$f"
  fi
done
udevadm control --reload >/dev/null 2>&1 || true

systemctl daemon-reload

# runtime sockets
for f in /run/${FS_NAME}/spire-bridge.sock /tmp/${FS_NAME}-spire-fallback.sock; do
  [[ -e "$f" ]] && { log "rm $f"; rm -f "$f"; }
done

if [[ -d /run/${FS_NAME} ]]; then
  log "rmdir /run/${FS_NAME} (best-effort)"
  rmdir /run/${FS_NAME} 2>/dev/null || true
fi

if [[ $PURGE -eq 1 ]]; then
  log "--purge: the configs, the audit log and the helper user"
  rm -f /etc/${FS_NAME}/daemon.yaml /etc/${FS_NAME}/policy.rego /etc/${FS_NAME}/identity-rules.yaml
  # The examples an earlier revision of install.sh moved aside when it could not serve on them.
  rm -f /etc/${FS_NAME}/*.bak
  rmdir /etc/${FS_NAME} 2>/dev/null || true
  if id ${FS_NAME} &>/dev/null; then
    userdel ${FS_NAME} || true
  fi
fi

log "done."
