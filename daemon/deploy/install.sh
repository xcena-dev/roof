#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# install.sh — install ${DAEMON_NAME} on a single host (idempotent).
#
# Usage:
#   sudo bash install.sh                  # full install + start
#   sudo bash install.sh --skip-build     # skip the kernel and daemon builds
#   sudo bash install.sh --no-start       # install but don't start the service
#   sudo bash install.sh --check          # diagnose only, change nothing
#   sudo bash install.sh --reset-cfg      # overwrite /etc/${FS_NAME}/* from examples
#   sudo bash install.sh --clean          # wipe artifacts + binaries first

set -euo pipefail

# ─── locate paths ──────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${ROOT}/fsname"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." &>/dev/null && pwd)"
HELPER_DIR="${REPO_ROOT}/daemon"
KERNEL_DIR="${REPO_ROOT}/kernel"

# ─── tunables ──────────────────────────────────────────────────────────────
HELPER_USER="${HELPER_USER:-${FS_NAME}}"
HELPER_GROUP="${HELPER_GROUP:-${FS_NAME}}"
HELPER_BUILD="${HELPER_BUILD:-${HELPER_DIR}/build}"
# The lock the daemon takes is cme's, and the turn backend links it. Absent, the daemon still
# attests and answers access, and tells the kernel it holds no lock.
CME_DIR="${CME_DIR:-${HOME:-/root}/cme}"
# The one the unit's ExecStart names, so the check after the start reads what the daemon read.
CONF_FILE=/etc/${FS_NAME}/daemon.yaml

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
# Who the applications run as. Not the daemon's own account: the rules say whom to admit, and
# admitting the policy decision point would tell it to judge itself.
APP_USER="${APP_USER:-${SUDO_USER:-$(id -un)}}"
APP_UID="${APP_UID:-$(id -u "$APP_USER")}"

# ─── flags ─────────────────────────────────────────────────────────────────
SKIP_BUILD=0
NO_START=0
CHECK_ONLY=0
RESET_CFG=0
CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --cme-dir=*)  CME_DIR="${arg#*=}" ;;
    --skip-build) SKIP_BUILD=1 ;;
    --no-start)   NO_START=1 ;;
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

# ─── 0. pre-flight ─────────────────────────────────────────────────────────
step "preflight: paths and prerequisites"
[[ -f "$HELPER_DIR/CMakeLists.txt" ]] || die "daemon source not found under $HELPER_DIR"
[[ -d "$KERNEL_DIR"  ]] || die "kernel dir not found: $KERNEL_DIR"
info "repo root  = $REPO_ROOT"
info "helper dir = $HELPER_DIR"
info "kernel dir = $KERNEL_DIR"
info "build dir  = $HELPER_BUILD"
info "cme dir    = $CME_DIR"
if [[ $SKIP_BUILD -eq 0 && $CHECK_ONLY -eq 0 ]]; then
  command -v cmake >/dev/null || die "cmake missing" "install-deps.sh installs what a build needs"
fi
ok "preflight passed"

# ─── helper that summarizes findings (--check mode) ────────────────────────
declare -a CHECK_RESULTS
note() { CHECK_RESULTS+=("$1"); }

step "user: $HELPER_USER"
if ! id "$HELPER_USER" &>/dev/null; then
  if [[ $CHECK_ONLY -eq 1 ]]; then
    warn "user $HELPER_USER missing"
  else
    info "creating system user $HELPER_USER"
    # A teardown removes the account but leaves its group when another account is still in it, so
    # the group is often already here. useradd refuses to guess between the two.
    if getent group "$HELPER_GROUP" > /dev/null; then
      useradd -r -s /usr/sbin/nologin -g "$HELPER_GROUP" "$HELPER_USER"
    else
      useradd -r -s /usr/sbin/nologin --user-group "$HELPER_USER"
    fi
  fi
fi
HELPER_UID="$(id -u "$HELPER_USER" 2>/dev/null || echo 0)"
ok "user $HELPER_USER (uid=$HELPER_UID)"

# ─── 1.5. clean (--clean) ─────────────────────────────────────────────────
if [[ $CLEAN -eq 1 && $CHECK_ONLY -eq 0 ]]; then
  step "clean: wipe build artifacts + installed binaries"
  info "stopping helper if running"
  systemctl stop "${DAEMON_NAME}@*.service" 2>/dev/null || true
  info "kernel: make clean"
  make -C "$KERNEL_DIR" clean >/tmp/${FS_NAME}-kclean.log 2>&1 || warn "kernel make clean failed (see /tmp/${FS_NAME}-kclean.log)"
  info "daemon: rm -rf $HELPER_BUILD"
  rm -rf "$HELPER_BUILD"
  info "binaries: rm /usr/local/bin/${DAEMON_NAME}"
  rm -f /usr/local/bin/${DAEMON_NAME}
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
  cmake_args=(-S "$HELPER_DIR" -B "$HELPER_BUILD" -DCMAKE_BUILD_TYPE=Release)
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
step "binaries: /usr/local/{sbin,bin}"
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
need_bin "$HELPER_BUILD/daemon"       /usr/local/bin/${DAEMON_NAME}
[[ $CHECK_ONLY -eq 0 ]] && ok "binaries installed"

# ─── 4. directories ───────────────────────────────────────────────────────
step "directories: /etc/${FS_NAME}"
ensure_dir() {
  local path="$1" mode="$2" owner="$3" group="$4"
  if [[ -d "$path" ]]; then
    info "$path exists"
    return
  fi
  if [[ $CHECK_ONLY -eq 1 ]]; then
    note "would create $path ($mode $owner:$group)"
    warn "$path missing"
    return
  fi
  install -d -m "$mode" -o "$owner" -g "$group" "$path"
  info "created $path ($mode $owner:$group)"
}
ensure_dir /etc/${FS_NAME}              0755 root             root
# /run/${FS_NAME} is not here: it lives on a tmpfs, so one made now is gone after a reboot and
# the unit then refuses its namespace. RuntimeDirectory= in the unit remakes it on every start.
[[ $CHECK_ONLY -eq 0 ]] && ok "directories ready"

# ─── 5. config files ──────────────────────────────────────────────────────
step "config files in /etc/${FS_NAME}"
# Each example is a template: the paths and the rego package in it carry the filesystem's name.
maybe_install_cfg() {
  local src="$1" dst="$2" label="$3"
  if [[ -e "$dst" && $RESET_CFG -eq 0 ]]; then
    # A file kept from an earlier install can carry a mode another account reads, or be root's alone
    # and so closed to the daemon. The content stays, and the ownership and the mode do not.
    if [[ $CHECK_ONLY -eq 1 ]]; then
      note "would fix ownership on $dst (0400 ${HELPER_USER}:root)"
    else
      chown "${HELPER_USER}:root" "$dst"
      chmod 0400 "$dst"
      info "kept $dst, fixed ownership (use --reset-cfg to overwrite content)"
    fi
    return
  fi
  if [[ $CHECK_ONLY -eq 1 ]]; then
    [[ -e "$dst" ]] && warn "would overwrite $dst" || warn "$dst missing"
    note "would install $dst"
    return
  fi
  local rendered
  rendered="$(mktemp)"
  "$ROOT/tools/render-fsname.sh" "$src" "$rendered"
  # The identity rules name a uid, and a rule for a uid nobody on this host has refuses every
  # attest. APP_UID is the account this ran for, which is who the applications are.
  sed -i "s|@APP_UID@|${APP_UID}|g; s|@APP_USER@|${APP_USER}|g" "$rendered"
  # The daemon's own account reads them and nobody else does. Root still writes them, so an edit
  # takes sudo and reaches the daemon on a SIGHUP.
  install -m 0400 -o "${HELPER_USER}" -g root "$rendered" "$dst"
  rm -f "$rendered"
  info "installed $dst ($label)"
}
RULES_FILE=/etc/${FS_NAME}/identity-rules.yaml
POLICY_FILE=/etc/${FS_NAME}/policy.rego

install_configs() {
  maybe_install_cfg "$HELPER_DIR/deploy/daemon/daemon.example.yaml.in"          "${CONF_FILE}"   "helper config"
  maybe_install_cfg "$HELPER_DIR/deploy/policy/policy.example.rego.in"        "${POLICY_FILE}" "policy (Rego)"
  maybe_install_cfg "$HELPER_DIR/deploy/identity/local-rules.example.yaml.in" "${RULES_FILE}"  "identity rules (local backend)"
}
install_configs

[[ $CHECK_ONLY -eq 0 ]] && ok "configs in place"

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
UDEV_RULE_SRC="$HELPER_DIR/deploy/daemon/99-daemon.rules.in"
UDEV_RULE_DST="/etc/udev/rules.d/99-${DAEMON_NAME}.rules"
if [[ $CHECK_ONLY -eq 1 ]]; then
  if [[ -f "$UDEV_RULE_DST" ]]; then
    ok "udev rule installed at $UDEV_RULE_DST"
  else
    warn "udev rule missing at $UDEV_RULE_DST"
  fi
else
  sed -e "s|@HELPER_USER@|$HELPER_USER|g" \
      -e "s|@HELPER_GROUP@|$HELPER_GROUP|g" \
      -e "s|@FS_NAME@|$FS_NAME|g" \
      -e "s|@DAEMON_NAME@|$DAEMON_NAME|g" \
      "$UDEV_RULE_SRC" > "$UDEV_RULE_DST"
  chmod 0644 "$UDEV_RULE_DST"
  udevadm control --reload >/dev/null
  udevadm trigger --subsystem-match=misc >/dev/null 2>&1 || true
  info "udev rule installed at $UDEV_RULE_DST"
fi

EXPECTED_OWN="$HELPER_USER $HELPER_GROUP 600"
for dev in $(daemon_devices); do
  if [[ $CHECK_ONLY -eq 1 ]]; then
    if [[ "$(stat -c '%U %G %a' "$dev")" == "$EXPECTED_OWN" ]]; then
      ok "$dev $HELPER_USER:$HELPER_GROUP 0600"
    else
      warn "$dev needs chown $HELPER_USER:$HELPER_GROUP + chmod 0600"
    fi
  else
    # One-shot chown for the channels already registered; the udev rule above covers the ones a
    # later mount registers.
    chown "$HELPER_USER:$HELPER_GROUP" "$dev"
    chmod 0600 "$dev"
    info "$dev → $HELPER_USER:$HELPER_GROUP 0600"
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
sed -e "s|@FS_NAME@|$FS_NAME|g" -e "s|@DAEMON_NAME@|$DAEMON_NAME|g" \
    "$HELPER_DIR/deploy/daemon/daemon@.service.in" > "$SVC_RENDERED"
SVC_DST=/etc/systemd/system/${DAEMON_NAME}@.service
SVC_SRC="${SVC_RENDERED}"
if [[ $CHECK_ONLY -eq 1 ]]; then
  if [[ -e "$SVC_DST" ]] && cmp -s "$SVC_SRC" "$SVC_DST"; then
    ok "unit installed and up to date"
  else
    warn "unit missing or stale"
    note "would install $SVC_DST"
  fi
else
  install -m 0644 "$SVC_SRC" "$SVC_DST"
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

step "start: one ${DAEMON_NAME} per mounted node"
if [[ -z "$(daemon_nodes)" ]]; then
  info "no channel to attend yet, so there is nothing to start"
  hint "bring a mount up: systemctl start mnt-${FS_NAME}.mount"
fi
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

# ─── 8b. what the config selects, and whether it can serve ────────────────
# Active is not serving. The daemon comes up whatever its config selects, and a selection it cannot
# reach shows only per request: every attest answers ERR, and the mount turns that into -EAGAIN on
# every create. So it is checked here rather than in a filesystem fault.
step "what the config selects"

selected() {
  sed -n '/^backends:/,/^[^ #]/p' "${CONF_FILE}" |
    sed -n "s/^[[:space:]]*$1:[[:space:]]*\\([a-z]*\\).*/\\1/p" | head -1
}

# Prints one line per thing the selection needs and does not have. Silent when it can serve.
what_is_missing() {
  local identityBackend policyBackend spireSocket policyUrl
  identityBackend="$(selected identity)"
  policyBackend="$(selected policy)"
  : "${identityBackend:=local}"
  : "${policyBackend:=local}"

  case "$identityBackend" in
  spire)
    spireSocket="$(sed -n 's|^spire_workload_api_socket:[[:space:]]*||p' "${CONF_FILE}" | head -1)"
    : "${spireSocket:=/run/spire-agent/admin/api.sock}"
    [[ -S "$spireSocket" ]] || echo "identity=spire but no SPIRE agent socket at ${spireSocket}"
    ;;
  local)
    # The rules admit by uid, and one naming a uid nobody here has refuses every attest.
    if ! grep -qE "^[[:space:]]*uid:[[:space:]]*${APP_UID}([^0-9]|$)" "${RULES_FILE}" 2>/dev/null; then
      echo "identity=local but ${RULES_FILE} admits no rule for uid ${APP_UID} (${APP_USER})"
    fi
    ;;
  esac

  case "$policyBackend" in
  opa)
    policyUrl="$(sed -n 's|^policy_url:[[:space:]]*||p' "${CONF_FILE}" | head -1)"
    : "${policyUrl:=http://127.0.0.1:8181/v1/data/${FS_NAME}/authz}"
    curl -fs -o /dev/null --max-time 3 "$policyUrl" 2> /dev/null ||
      echo "policy=opa but nothing answers at ${policyUrl}"
    ;;
  local)
    [[ -f "${POLICY_FILE}" ]] || echo "policy=local but ${POLICY_FILE} is not there"
    ;;
  esac
}

info "identity=$(selected identity), policy=$(selected policy)"
mapfile -t missing < <( what_is_missing )

# One repair, and only by moving what is there aside: a config from an older revision of this script
# cannot serve, and a config the operator wrote is not ours to discard. The copy makes both safe.
if [[ ${#missing[@]} -gt 0 ]]; then
  for m in "${missing[@]}"; do warn "$m"; done
  stamp="$(date +%Y%m%d%H%M%S)"
  for path in "${CONF_FILE}" "${RULES_FILE}" "${POLICY_FILE}"; do
    [[ -e "$path" ]] || continue
    mv "$path" "${path}.${stamp}.bak"
    info "kept the old one at ${path}.${stamp}.bak"
  done
  warn "installing this tree's examples over them, which select the file-driven backends"
  install_configs
  for node in $(daemon_nodes); do systemctl restart "${DAEMON_NAME}@${node}"; done
  sleep 1
  info "identity=$(selected identity), policy=$(selected policy)"
  mapfile -t missing < <( what_is_missing )
fi

if [[ ${#missing[@]} -eq 0 ]]; then
  ok "everything the selection needs is there"
else
  for m in "${missing[@]}"; do fail "$m"; done
  fail "the daemon is running but cannot decide; every create would answer -EAGAIN"
  exit 1
fi

# ─── 9. post-install summary ──────────────────────────────────────────────
cat <<EOF

${C_BOLD}${C_GRN}install complete.${C_RST}

Inspect:
  ${C_DIM}systemctl status "${DAEMON_NAME}@*"${C_RST}
  ${C_DIM}journalctl -u "${DAEMON_NAME}@*" -f${C_RST}
  ${C_DIM}sudo cat ${CONF_FILE}${C_RST}

Test:
  ${C_DIM}# create a region as owner (any user with rw on /mnt/${FS_NAME})
  python3 -c 'import os, mmap; fd = os.open("/mnt/${FS_NAME}/region-A", os.O_RDWR|os.O_CREAT, 0o660); mm = mmap.mmap(fd, 4096); mm[:5]=b"hello"'

  # consume from another shell — mmap triggers helper upcall
  python3 -c 'import os, mmap; fd = os.open("/mnt/${FS_NAME}/region-A", os.O_RDWR); print(mmap.mmap(fd, 4096)[:5])'${C_RST}

Decisions stream to the journal — watch with:
  ${C_DIM}journalctl -u "${DAEMON_NAME}@*" -f --output=cat${C_RST}

Diagnose anytime:
  ${C_DIM}sudo bash $0 --check${C_RST}

Uninstall:
  ${C_DIM}sudo bash $(dirname "$0")/uninstall.sh${C_RST}

EOF
