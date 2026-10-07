#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# install.sh — install ${DAEMON_NAME} on a single host (idempotent).
#
# Usage:
#   sudo bash install.sh                  # full install + start
#   sudo bash install.sh --skip-build     # skip the kernel and daemon builds
#   sudo bash install.sh --no-start       # install but don't start the service
#   sudo bash install.sh --start-only     # only restart the attended nodes and check what they serve
#   sudo bash install.sh --check          # diagnose only, change nothing
#   sudo bash install.sh --reset-cfg      # overwrite /etc/${FS_NAME}/* from examples
#   sudo bash install.sh --clean          # wipe artifacts + binaries first

set -euo pipefail

# ─── locate paths ──────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${ROOT}/fsname"
. "${ROOT}/shell/config.sh"
REPO_ROOT="$ROOT"
DAEMON_SRC="${REPO_ROOT}/daemon"
KERNEL_DIR="${REPO_ROOT}/kernel"

# ─── tunables ──────────────────────────────────────────────────────────────
HELPER_BUILD="${HELPER_BUILD:-${DAEMON_SRC}/build}"
# The lock the daemon takes is cme's, and the turn backend links it. Absent, the daemon still
# attests and answers access, and tells the kernel it holds no lock.
CME_DIR="${CME_DIR:-${HOME:-/root}/cme}"

# The nodes this host has mounted, read off the channels their mounts registered. One helper
# instance per channel, so this list is what the unit is started for.
daemon_nodes() {
  local dev
  for dev in /dev/${DAEMON_NAME}-*; do
    [[ -e "$dev" ]] || continue
    echo "${dev##*/${DAEMON_NAME}-}"
  done
}

daemon_devices() {
  local node
  for node in $(daemon_nodes); do
    echo "/dev/${DAEMON_NAME}-${node}"
  done
}

# What a config planted here starts with. The rule admits the account this ran for, and the mounts
# are what the host installer was given, one "point device" pair to a line.
APP_USER="${APP_USER:-${SUDO_USER:-$(id -un)}}"
APP_UID="${APP_UID:-$(id -u "$APP_USER")}"
DEPLOY_MOUNTS="${DEPLOY_MOUNTS:-/mnt/${FS_NAME} /dev/dax0.0}"

# ─── flags ─────────────────────────────────────────────────────────────────
SKIP_BUILD=0
NO_START=0
START_ONLY=0
CHECK_ONLY=0
RESET_CFG=0
CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --cme-dir=*)  CME_DIR="${arg#*=}" ;;
    --skip-build) SKIP_BUILD=1 ;;
    --no-start)   NO_START=1 ;;
    --start-only) START_ONLY=1 ;;
    --check)      CHECK_ONLY=1 ;;
    --reset-cfg)  RESET_CFG=1 ;;
    --clean)      CLEAN=1 ;;
    -h|--help)    sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "unknown flag: $arg" >&2; exit 2 ;;
  esac
done

# ─── colors ────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
  C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'; C_RED=$'\033[31m'; C_GRN=$'\033[32m'
  C_YLW=$'\033[33m'; C_BLU=$'\033[34m'; C_CYN=$'\033[36m'; C_RST=$'\033[0m'
else
  C_BOLD=""; C_DIM=""; C_RED=""; C_GRN=""; C_YLW=""; C_BLU=""; C_CYN=""; C_RST=""
fi

step()  { printf '\n%s┌─ %s%s\n' "$C_BOLD$C_CYN" "$*" "$C_RST"; }
ok()    { printf '%s└─ ok%s   %s\n' "$C_GRN"  "$C_RST" "$*"; }
info()  { printf '%s│  %s%s\n'    "$C_DIM"  "$*" "$C_RST"; }
warn()  { printf '%s└─ warn%s %s\n' "$C_YLW" "$C_RST" "$*"; }
fail()  { printf '%s└─ FAIL%s %s\n' "$C_RED" "$C_RST" "$*"; }
hint()  { printf '   %s↳ hint:%s %s\n' "$C_BLU" "$C_RST" "$*"; }
die()   { fail "$1"; [[ -n "${2:-}" ]] && hint "$2"; exit 1; }

# ─── must be root ──────────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
  die "must run as root" "use: sudo bash $0 $*"
fi

declare -a CHECK_RESULTS
note() { CHECK_RESULTS+=("$1"); }

# ─── start: one daemon per attended node, then what it can serve ──────────
# Active is not serving. The daemon comes up whatever its config selects, and a selection it cannot
# reach shows only per request: every attest answers ERR, and the mount turns that into -EAGAIN on
# every create. So it is checked here rather than in a filesystem fault.
start_and_check() {
  step "start: one ${DAEMON_NAME} per mounted node"
  if [[ -z "$(daemon_nodes)" ]]; then
    info "no channel to attend yet, so there is nothing to start"
    hint "bring a mount up: systemctl start mnt-${FS_NAME}.mount"
  fi
  local node unit
  for node in $(daemon_nodes); do
    unit="${DAEMON_NAME}@${node}"
    if ! systemctl restart "$unit"; then
      fail "systemctl restart $unit failed"
      hint "journalctl -u $unit -n 50 --no-pager"
      exit 1
    fi
    sleep 1
    if systemctl is-active --quiet "$unit"; then
      ok "$unit active"
    else
      fail "$unit not active"
      hint "journalctl -u $unit -n 50 --no-pager"
      exit 1
    fi
  done

  step "what the config selects"
  local missing=() identityBackend policyBackend socket
  identityBackend="$(readSetting identity.backend)"
  policyBackend="$(readSetting policy.backend)"
  info "identity=${identityBackend}, policy=${policyBackend}"
  if ! checkSettings > /dev/null 2>&1; then
    missing+=("${CONFIG_FILE} does not load: $(describeCheck)")
  fi
  if [[ "$identityBackend" == spire ]]; then
    socket="$(readSetting spire.admin_socket)"
    [[ -S "$socket" ]] || missing+=("identity=spire but no SPIRE agent socket at ${socket}")
  fi
  if [[ "$policyBackend" == opa ]]; then
    socket="$(readSetting policy.opa_socket)"
    [[ -S "$socket" ]] || missing+=("policy=opa but no OPA socket at ${socket}")
  fi
  if [[ ${#missing[@]} -eq 0 ]]; then
    ok "everything the selection needs is there"
    return
  fi
  local line
  for line in "${missing[@]}"; do fail "$line"; done
  fail "the daemon is running but cannot decide; every create would answer -EAGAIN"
  exit 1
}

if [[ $START_ONLY -eq 1 ]]; then
  start_and_check
  exit 0
fi

# ─── 0. pre-flight ─────────────────────────────────────────────────────────
step "preflight: paths and prerequisites"
[[ -f "$DAEMON_SRC/CMakeLists.txt" ]] || die "daemon source not found under $DAEMON_SRC"
[[ -d "$KERNEL_DIR"  ]] || die "kernel dir not found: $KERNEL_DIR"
info "repo root  = $REPO_ROOT"
info "daemon dir = $DAEMON_SRC"
info "kernel dir = $KERNEL_DIR"
info "build dir  = $HELPER_BUILD"
info "cme dir    = $CME_DIR"
if [[ $SKIP_BUILD -eq 0 && $CHECK_ONLY -eq 0 ]]; then
  command -v cmake >/dev/null || die "cmake missing" "install-deps.sh installs what a build needs"
fi
ok "preflight passed"

# ─── 1. clean (--clean) ───────────────────────────────────────────────────
if [[ $CLEAN -eq 1 && $CHECK_ONLY -eq 0 ]]; then
  step "clean: wipe build artifacts + installed binaries"
  info "stopping helper if running"
  systemctl stop "${DAEMON_NAME}@*.service" 2>/dev/null || true
  info "kernel: make clean"
  make -C "$KERNEL_DIR" clean >/tmp/${FS_NAME}-kclean.log 2>&1 || warn "kernel make clean failed (see /tmp/${FS_NAME}-kclean.log)"
  info "daemon: rm -rf $HELPER_BUILD"
  rm -rf "$HELPER_BUILD"
  info "binaries: rm ${DAEMON_BIN}"
  rm -f "${DAEMON_BIN}"
  info "systemd: rm unit"
  rm -f /etc/systemd/system/${DAEMON_NAME}@.service /etc/systemd/system/${DAEMON_NAME}.service
  systemctl daemon-reload 2>/dev/null || true
  ok "clean complete"
fi

# ─── 2. build (unless --skip-build / --check) ──────────────────────────────
if [[ $SKIP_BUILD -eq 0 && $CHECK_ONLY -eq 0 ]]; then
  step "build: kernel + daemon"

  info "make -C $KERNEL_DIR -j128"
  if ! make -C "$KERNEL_DIR" -j128 >/tmp/${FS_NAME}-kbuild.log 2>&1; then
    tail -20 /tmp/${FS_NAME}-kbuild.log >&2
    die "kernel build failed" "see /tmp/${FS_NAME}-kbuild.log"
  fi
  info "kernel module: $KERNEL_DIR/build/${FS_NAME}.ko"

  # Release, because this is the binary a host runs and the tree's own probes cover the rest.
  cmake_args=(-S "$DAEMON_SRC" -B "$HELPER_BUILD" -DCMAKE_BUILD_TYPE=Release)
  # The turn backend is what carries FS_DAEMON_CAP_LOCK, and the kernel refuses a create when the
  # helper does not offer it. So a missing cme is a warning here and a helper that cannot lock.
  if [[ -d "$CME_DIR" ]]; then
    cmake_args+=(-DDAEMON_ENABLE_TURN=ON -DDAEMON_CME_DIR="$CME_DIR")
    info "turn backend on, against $CME_DIR"
  else
    warn "no cme tree at $CME_DIR; the daemon will serve without a lock"
    hint "pass --cme-dir=/path/to/cme, or set CME_DIR"
  fi
  info "cmake configure: $HELPER_BUILD"
  if ! cmake "${cmake_args[@]}" >/tmp/${FS_NAME}-cmake.log 2>&1; then
    tail -20 /tmp/${FS_NAME}-cmake.log >&2
    die "daemon configure failed" "see /tmp/${FS_NAME}-cmake.log"
  fi
  info "cmake build"
  if ! cmake --build "$HELPER_BUILD" -j"$(nproc)" >>/tmp/${FS_NAME}-cmake.log 2>&1; then
    tail -20 /tmp/${FS_NAME}-cmake.log >&2
    die "daemon build failed" "see /tmp/${FS_NAME}-cmake.log"
  fi
  info "daemon: $HELPER_BUILD/daemon"

  ok "build complete"
elif [[ $CHECK_ONLY -eq 0 ]]; then
  info "skipping build (--skip-build)"
fi

# ─── 3. install binaries ──────────────────────────────────────────────────
step "binaries: ${DAEMON_BIN}"
need_bin() {
  local src="$1" dst="$2"
  if [[ ! -x "$src" ]]; then
    fail "missing source: $src"
    hint "rerun without --skip-build, or build manually"
    return 1
  fi
  if [[ $CHECK_ONLY -eq 1 ]]; then
    if [[ -x "$dst" ]] && cmp -s "$src" "$dst"; then
      ok "$dst up to date"
    else
      note "would install $dst"
      warn "$dst differs / missing"
    fi
    return 0
  fi
  install -m 0755 "$src" "$dst"
  info "installed $dst"
}
need_bin "$HELPER_BUILD/daemon" "${DAEMON_BIN}"
[[ $CHECK_ONLY -eq 0 ]] && ok "binaries installed"

# ─── 4. directories ───────────────────────────────────────────────────────
step "directories: /etc/${FS_NAME}"
if [[ -d /etc/${FS_NAME} ]]; then
  info "/etc/${FS_NAME} exists"
elif [[ $CHECK_ONLY -eq 1 ]]; then
  note "would create /etc/${FS_NAME} (0755 root:root)"
  warn "/etc/${FS_NAME} missing"
else
  install -d -m 0755 -o root -g root /etc/${FS_NAME}
  info "created /etc/${FS_NAME} (0755 root:root)"
fi
# /run/${FS_NAME} is not here: it lives on a tmpfs, so one made now is gone after a reboot and
# the unit then refuses its namespace. RuntimeDirectory= in the unit remakes it on every start.
[[ $CHECK_ONLY -eq 0 ]] && ok "directories ready"

# ─── 5. config, account, policy ───────────────────────────────────────────
step "config: ${CONFIG_FILE}"
# The example's mounts block, one entry per "point device" line.
render_mounts() {
  local point device
  while read -r point device; do
    [[ -n "$point" ]] || continue
    printf '  - point: %s\n    device: %s\n' "$point" "$device"
  done <<< "$DEPLOY_MOUNTS"
}

# Planted root's until the account it names exists, which is also what lets root read it back here.
plant_config() {
  local rendered mounts
  rendered="$(mktemp)"
  mounts="$(mktemp)"
  "$ROOT/tools/render-fsname.sh" "$DAEMON_SRC/deploy/daemon/config.example.yaml.in" "$rendered"
  # The rule names a uid, and a rule for a uid nobody on this host has refuses every attest.
  sed -i "s|@APP_UID@|${APP_UID}|g; s|@APP_USER@|${APP_USER}|g" "$rendered"
  render_mounts > "$mounts"
  sed -i -e "/^@MOUNTS@\$/{r ${mounts}" -e 'd}' "$rendered"
  install -m 0400 -o root -g root "$rendered" "$CONFIG_FILE"
  rm -f "$rendered" "$mounts"
  info "installed ${CONFIG_FILE}"
}

if [[ -e "$CONFIG_FILE" && $RESET_CFG -eq 0 ]]; then
  info "kept ${CONFIG_FILE} (use --reset-cfg to start from this tree's example)"
elif [[ $CHECK_ONLY -eq 1 ]]; then
  [[ -e "$CONFIG_FILE" ]] && warn "would overwrite ${CONFIG_FILE}" || warn "${CONFIG_FILE} missing"
  note "would install ${CONFIG_FILE}"
else
  plant_config
fi

if [[ ! -e "$CONFIG_FILE" ]]; then
  [[ $CHECK_ONLY -eq 1 ]] || die "no ${CONFIG_FILE} to read the daemon account from"
  DAEMON_USER=""
else
  DAEMON_USER="$(readSetting accounts.daemon)" || die "${CONFIG_FILE} does not load" "$(describeCheck)"
fi

if [[ -n "$DAEMON_USER" ]] && ! id "$DAEMON_USER" &>/dev/null; then
  if [[ $CHECK_ONLY -eq 1 ]]; then
    warn "user $DAEMON_USER missing"
  else
    info "creating system user $DAEMON_USER"
    # A teardown removes the account but leaves its group when another account is still in it, so
    # the group is often already here. useradd refuses to guess between the two.
    if getent group "$DAEMON_USER" > /dev/null; then
      useradd -r -s /usr/sbin/nologin -g "$DAEMON_USER" "$DAEMON_USER"
    else
      useradd -r -s /usr/sbin/nologin --user-group "$DAEMON_USER"
    fi
  fi
fi
DAEMON_GROUP="$(id -gn "$DAEMON_USER" 2>/dev/null || echo "$DAEMON_USER")"
ok "daemon account ${DAEMON_USER:-<none>} (group ${DAEMON_GROUP:-<none>})"

# The daemon's own account reads these and nobody else does. Root still writes them, so an edit
# takes sudo and reaches the daemon on a SIGHUP.
hand_to_daemon() {
  local path="$1"
  if [[ $CHECK_ONLY -eq 1 ]]; then
    [[ "$(stat -c '%U %G %a' "$path" 2>/dev/null)" == "${DAEMON_USER} root 400" ]] ||
      note "would set ${path} to 0400 ${DAEMON_USER}:root"
    return
  fi
  chown "${DAEMON_USER}:root" "$path"
  chmod 0400 "$path"
}

if [[ -e "$CONFIG_FILE" ]]; then
  POLICY_FILE="$(readSetting policy.path)"
  if [[ -e "$POLICY_FILE" && $RESET_CFG -eq 0 ]]; then
    info "kept ${POLICY_FILE}"
  elif [[ $CHECK_ONLY -eq 0 ]]; then
    rendered="$(mktemp)"
    "$ROOT/tools/render-fsname.sh" "$DAEMON_SRC/deploy/policy/policy.example.rego.in" "$rendered"
    install -m 0400 -o root -g root "$rendered" "$POLICY_FILE"
    rm -f "$rendered"
    info "installed ${POLICY_FILE}"
  fi
  hand_to_daemon "$CONFIG_FILE"
  [[ -e "$POLICY_FILE" ]] && hand_to_daemon "$POLICY_FILE"
fi
[[ $CHECK_ONLY -eq 0 ]] && ok "config, account and policy in place"

# ─── 6. kernel module wiring ──────────────────────────────────────────────
# A host with no channel yet is the ordinary first install, not a failure. The channel is a mount's
# to register, the mount refuses to come up until the unit below exists, and the unit is this
# script's to write. So everything that does not need a channel is installed either way, and only
# the per-channel work below is skipped.
step "kernel module: /dev/${DAEMON_NAME}-<node> permissions"
if [[ -z "$(daemon_nodes)" ]]; then
  warn "no /dev/${DAEMON_NAME}-<node> channel yet"
  note "mount the filesystem; the udev rule below starts a helper for each channel it registers"
fi
# udev rule — persistent chown across module unload/reload.
UDEV_RULE_DST="/etc/udev/rules.d/99-${DAEMON_NAME}.rules"
if [[ $CHECK_ONLY -eq 1 ]]; then
  if [[ -f "$UDEV_RULE_DST" ]]; then
    ok "udev rule installed at $UDEV_RULE_DST"
  else
    warn "udev rule missing at $UDEV_RULE_DST"
  fi
else
  renderForAccount "$DAEMON_SRC/deploy/daemon/99-daemon.rules.in" "$DAEMON_USER" > "$UDEV_RULE_DST"
  chmod 0644 "$UDEV_RULE_DST"
  udevadm control --reload >/dev/null
  udevadm trigger --subsystem-match=misc >/dev/null 2>&1 || true
  info "udev rule installed at $UDEV_RULE_DST"
fi

EXPECTED_OWN="$DAEMON_USER $DAEMON_GROUP 600"
for dev in $(daemon_devices); do
  if [[ $CHECK_ONLY -eq 1 ]]; then
    if [[ "$(stat -c '%U %G %a' "$dev")" == "$EXPECTED_OWN" ]]; then
      ok "$dev $DAEMON_USER:$DAEMON_GROUP 0600"
    else
      warn "$dev needs chown $DAEMON_USER:$DAEMON_GROUP + chmod 0600"
    fi
  else
    # One-shot chown for the channels already registered; the udev rule above covers the ones a
    # later mount registers.
    chown "$DAEMON_USER:$DAEMON_GROUP" "$dev"
    chmod 0600 "$dev"
    info "$dev → $DAEMON_USER:$DAEMON_GROUP 0600"
  fi
done
if [[ -z "$(daemon_devices)" ]]; then
  warn "no channel present — is a ${FS_NAME} filesystem mounted?"
fi
[[ $CHECK_ONLY -eq 0 ]] && ok "kernel wiring done"

# ─── 7. systemd unit ──────────────────────────────────────────────────────
step "systemd unit: ${DAEMON_NAME}@.service"
SVC_RENDERED="$(mktemp)"
trap 'rm -f "${SVC_RENDERED}"' EXIT
renderForAccount "$DAEMON_SRC/deploy/daemon/daemon@.service.in" "$DAEMON_USER" > "$SVC_RENDERED"
SVC_DST=/etc/systemd/system/${DAEMON_NAME}@.service
if [[ $CHECK_ONLY -eq 1 ]]; then
  if [[ -e "$SVC_DST" ]] && cmp -s "$SVC_RENDERED" "$SVC_DST"; then
    ok "unit installed and up to date"
  else
    warn "unit missing or stale"
    note "would install $SVC_DST"
  fi
else
  install -m 0644 "$SVC_RENDERED" "$SVC_DST"
  systemctl daemon-reload
  info "unit installed + daemon-reloaded"
  ok "systemd ready"
fi

# ─── 8. start ─────────────────────────────────────────────────────────────
if [[ $CHECK_ONLY -eq 1 ]]; then
  step "summary (check mode)"
  if [[ ${#CHECK_RESULTS[@]} -eq 0 ]]; then
    ok "all checks pass — system is fully configured"
  else
    warn "actions required:"
    for r in "${CHECK_RESULTS[@]}"; do printf '   • %s\n' "$r"; done
    hint "rerun without --check to apply"
  fi
  exit 0
fi

if [[ $NO_START -eq 1 ]]; then
  step "start: skipped (--no-start)"
  exit 0
fi

start_and_check

# ─── 9. post-install summary ──────────────────────────────────────────────
cat <<EOF

${C_BOLD}${C_GRN}install complete.${C_RST}

Inspect:
  ${C_DIM}systemctl status "${DAEMON_NAME}@*"${C_RST}
  ${C_DIM}journalctl -u "${DAEMON_NAME}@*" -f${C_RST}
  ${C_DIM}sudo cat ${CONFIG_FILE}${C_RST}

Try an edit before a SIGHUP:
  ${C_DIM}$(describeCheck)${C_RST}

Decisions stream to the journal — watch with:
  ${C_DIM}journalctl -u "${DAEMON_NAME}@*" -f --output=cat${C_RST}

Diagnose anytime:
  ${C_DIM}sudo bash $0 --check${C_RST}

Uninstall:
  ${C_DIM}sudo bash $(dirname "$0")/uninstall.sh${C_RST}

EOF
