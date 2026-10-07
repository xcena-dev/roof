#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# install-deps.sh — install OPA and SPIRE, and check what building ${DAEMON_NAME} needs.
#
# Usage:
#   sudo bash install-deps.sh                     # install OPA + SPIRE, check the build toolchain
#   sudo bash install-deps.sh --toolchain-only    # only check what building the daemon needs
#   sudo bash install-deps.sh --opa-only          # OPA only (skip SPIRE)
#   sudo bash install-deps.sh --spire-only        # SPIRE only (skip OPA)
#   sudo bash install-deps.sh --no-toolchain      # skip the build toolchain check
#   sudo bash install-deps.sh --force             # overwrite existing configs
#   sudo bash install-deps.sh --systemd           # also install systemd units
#   sudo bash install-deps.sh --bring-up          # spawn ephemeral SPIRE+OPA daemons (dev/e2e)
#   sudo bash install-deps.sh --teardown-bringup  # stop daemons started via --bring-up
#   sudo bash install-deps.sh --uninstall         # remove everything this installed
#   sudo bash install-deps.sh --skip-runtime-smoke  # skip SPIRE end-to-end check
#
# Env: OPA_VERSION (default: v0.69.0), SPIRE_VERSION (default: latest)
#
# The trust domain, the sockets, the policy file and the daemon account come from
# /etc/${FS_NAME}/config.yaml, or the example's defaults before install.sh has planted one.

set -euo pipefail

# ─── locate this script + deploy dir ──────────────────────────────────────
# First, because the tunables below are named after the filesystem.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${ROOT}/fsname"
. "${ROOT}/shell/config.sh"
SPIRE_CFG_SRC="$SCRIPT_DIR/identity"

# @key from the host config, or @fallback while there is none to read.
settingOr() {
  local key="$1" fallback="$2"
  if [[ -e "$CONFIG_FILE" && -x "$DAEMON_BIN" ]] && readSetting "$key" 2>/dev/null; then
    return
  fi
  printf '%s\n' "$fallback"
}

# ─── tunables ─────────────────────────────────────────────────────────────
OPA_VERSION="${OPA_VERSION:-v0.69.0}"
SPIRE_VERSION="${SPIRE_VERSION:-}"   # empty → query GitHub latest
TRUST_DOMAIN="$(settingOr spire.trust_domain "${FS_NAME}.local")"
SPIRE_SERVER_ADDRESS="$(settingOr spire.server_address 127.0.0.1)"
SPIRE_SERVER_PORT="$(settingOr spire.server_port 8081)"
SPIRE_ADMIN_SOCKET="$(settingOr spire.admin_socket /run/spire-agent/admin/api.sock)"
DAEMON_ACCOUNT="$(settingOr accounts.daemon "${FS_NAME}")"
POLICY_FILE_RUNTIME="$(settingOr policy.path "/etc/${FS_NAME}/policy.rego")"
OPA_SOCKET="$(settingOr policy.opa_socket "/run/${FS_NAME}-opa/api.sock")"

# ─── flags ────────────────────────────────────────────────────────────────
DO_OPA=1
DO_SPIRE=1
DO_TOOLCHAIN=1
FORCE_CFG=0
INSTALL_SYSTEMD=0
UNINSTALL=0
RUN_RUNTIME_SMOKE=1
DO_BRINGUP=0
DO_TEARDOWN_BRINGUP=0
for arg in "$@"; do
  case "$arg" in
    --opa-only)          DO_SPIRE=0 ;;
    # The local backends need neither OPA nor SPIRE, so a host serving those only wants the
    # toolchain check.
    --toolchain-only)    DO_OPA=0; DO_SPIRE=0; RUN_RUNTIME_SMOKE=0 ;;
    --spire-only)        DO_OPA=0 ;;
    --no-toolchain)      DO_TOOLCHAIN=0 ;;
    --force)             FORCE_CFG=1 ;;
    --systemd)           INSTALL_SYSTEMD=1 ;;
    --uninstall)         UNINSTALL=1 ;;
    --skip-runtime-smoke) RUN_RUNTIME_SMOKE=0 ;;
    --bring-up)          DO_BRINGUP=1 ;;
    --teardown-bringup)  DO_TEARDOWN_BRINGUP=1 ;;
    -h|--help)           sed -n '2,32p' "$0"; exit 0 ;;
    *) echo "unknown flag: $arg" >&2; exit 2 ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "must run as root (sudo)" >&2
  exit 1
fi

# ─── coloring ─────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
  C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'; C_RST=$'\033[0m'
  C_GRN=$'\033[32m'; C_YLW=$'\033[33m'; C_RED=$'\033[31m'; C_CYN=$'\033[36m'
else
  C_BOLD=""; C_DIM=""; C_RST=""; C_GRN=""; C_YLW=""; C_RED=""; C_CYN=""
fi
step() { printf '\n%s┌─ %s%s\n'    "$C_BOLD$C_CYN" "$*" "$C_RST"; }
ok()   { printf '%s└─ ok%s   %s\n' "$C_GRN" "$C_RST" "$*"; }
info() { printf '%s│  %s%s\n'      "$C_DIM" "$*" "$C_RST"; }
warn() { printf '%s└─ warn%s %s\n' "$C_YLW" "$C_RST" "$*"; }
fail() { printf '%s└─ FAIL%s %s\n' "$C_RED" "$C_RST" "$*"; }
die()  { fail "$1"; exit 1; }

# ─── arch detection ───────────────────────────────────────────────────────
case "$(uname -m)" in
  x86_64|amd64)  ARCH=amd64 ;;
  aarch64|arm64) ARCH=arm64 ;;
  *) die "unsupported arch: $(uname -m)" ;;
esac
info "host arch: $ARCH"

# ─── tmp scratch ──────────────────────────────────────────────────────────
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# ─── uninstall path ───────────────────────────────────────────────────────
do_uninstall() {
  step "uninstall: stop services + remove artifacts"
  systemctl stop opa spire-agent spire-server 2>/dev/null || true
  systemctl disable opa spire-agent spire-server 2>/dev/null || true
  rm -f /etc/systemd/system/{opa,spire-server,spire-agent}.service
  systemctl daemon-reload 2>/dev/null || true
  rm -f /usr/local/bin/{opa,spire-server,spire-agent}
  rm -rf /etc/spire /var/lib/spire
  ok "uninstalled OPA + SPIRE (state dirs wiped)"
  exit 0
}
[[ $UNINSTALL -eq 1 ]] && do_uninstall

# Neither download is signed, so the digest the project publishes beside it is the only
# thing that tells a tampered byte stream from the real one.
verify_sha256() {
  local file="$1" digest_url="$2" expected actual
  # A fetch that fails leaves this empty rather than ending the run on curl's own status, so the
  # refusal below is the one the operator reads.
  expected="$(curl -fsSL "$digest_url" 2>/dev/null | tr -s ' ' | cut -d' ' -f1 | head -1 || true)"
  [[ "$expected" =~ ^[0-9a-f]{64}$ ]] || die "no published sha256 at $digest_url"
  actual="$(sha256sum "$file" | cut -d' ' -f1)"
  [[ "$actual" == "$expected" ]] || die "sha256 mismatch for $file: got $actual, expected $expected"
  info "sha256 matches $digest_url"
}

# ─── OPA install ──────────────────────────────────────────────────────────
install_opa() {
  step "OPA $OPA_VERSION"
  local url="https://openpolicyagent.org/downloads/${OPA_VERSION}/opa_linux_${ARCH}_static"
  if [[ -x /usr/local/bin/opa ]] \
     && /usr/local/bin/opa version 2>/dev/null | grep -q "${OPA_VERSION#v}"; then
    info "OPA $OPA_VERSION already installed"
  else
    info "fetching $url"
    curl -fsSL -o "$TMPDIR/opa" "$url" || die "OPA download failed"
    verify_sha256 "$TMPDIR/opa" "${url}.sha256"
    install -m 755 "$TMPDIR/opa" /usr/local/bin/opa
  fi
  ok "$(/usr/local/bin/opa version | head -1)"
}

# ─── SPIRE install ────────────────────────────────────────────────────────
discover_spire_version() {
  [[ -n "$SPIRE_VERSION" ]] && return
  info "querying GitHub for latest SPIRE release"
  SPIRE_VERSION="$(curl -fsSL https://api.github.com/repos/spiffe/spire/releases/latest \
                    | grep -oE '"tag_name": *"[^"]*"' | head -1 | cut -d'"' -f4)"
  [[ -n "$SPIRE_VERSION" ]] || die "could not discover SPIRE version (set SPIRE_VERSION env)"
}

discover_spire_asset_url() {
  local api="https://api.github.com/repos/spiffe/spire/releases/tags/${SPIRE_VERSION}"
  local assets
  assets="$(curl -fsSL "$api" | grep -oE '"browser_download_url": *"[^"]*"' | cut -d'"' -f4)"
  SPIRE_ASSET_URL="$(printf '%s\n' "$assets" \
    | grep -E "linux[-_]${ARCH}.*\.tar\.gz$" \
    | grep -vE 'extras|sha256|checksum' \
    | head -1)"
  [[ -n "$SPIRE_ASSET_URL" ]] || die "no matching linux-${ARCH} asset for ${SPIRE_VERSION}"
  # The digest sits beside the tarball under its stem, not under the full name. Matched as a fixed
  # whole line, because a URL carries characters a regex reads as its own.
  local digest_name="${SPIRE_ASSET_URL%.tar.gz}_sha256sum.txt"
  SPIRE_SHA_URL="$(printf '%s\n' "$assets" | grep -Fx "$digest_name" | head -1)"
  [[ -n "$SPIRE_SHA_URL" ]] || die "no published digest beside ${SPIRE_ASSET_URL}"
}

install_spire() {
  discover_spire_version
  step "SPIRE $SPIRE_VERSION"
  local v="${SPIRE_VERSION#v}"
  if [[ -x /usr/local/bin/spire-server && -x /usr/local/bin/spire-agent ]] \
     && /usr/local/bin/spire-server --version 2>&1 | grep -q "$v"; then
    info "SPIRE $SPIRE_VERSION already installed"
  else
    discover_spire_asset_url
    info "fetching $SPIRE_ASSET_URL"
    curl -fsSL -o "$TMPDIR/spire.tgz" "$SPIRE_ASSET_URL" || die "SPIRE download failed"
    verify_sha256 "$TMPDIR/spire.tgz" "$SPIRE_SHA_URL"
    tar xzf "$TMPDIR/spire.tgz" -C "$TMPDIR"
    local bin_dir
    bin_dir="$(find "$TMPDIR" -type d -name bin -path "*/spire-*" | head -1)"
    [[ -d "$bin_dir" ]] || die "extracted tarball missing bin/ dir"
    install -m 755 "$bin_dir/spire-server" /usr/local/bin/spire-server
    install -m 755 "$bin_dir/spire-agent"  /usr/local/bin/spire-agent
  fi

  # config files
  install -d -m 755 /etc/spire/server /etc/spire/agent
  install -d -m 750 /var/lib/spire/server /var/lib/spire/agent
  mkdir -p /tmp/spire-agent/public
  for pair in \
        "$SPIRE_CFG_SRC/spire-server.example.conf:/etc/spire/server/server.conf" \
        "$SPIRE_CFG_SRC/spire-agent.example.conf:/etc/spire/agent/agent.conf"; do
    local src="${pair%:*}" dst="${pair#*:}" rendered
    if [[ -f "$dst" && $FORCE_CFG -ne 1 ]]; then
      info "preserving existing $dst (use --force to overwrite)"
    else
      rendered="$(mktemp)"
      sed -e "s|@TRUST_DOMAIN@|${TRUST_DOMAIN}|g" \
          -e "s|@SERVER_ADDRESS@|${SPIRE_SERVER_ADDRESS}|g" \
          -e "s|@SERVER_PORT@|${SPIRE_SERVER_PORT}|g" \
          -e "s|@ADMIN_SOCKET@|${SPIRE_ADMIN_SOCKET}|g" \
          -e "s|@DAEMON_NAME@|${DAEMON_NAME}|g" \
          -e "s|@FS_NAME@|${FS_NAME}|g" \
          "$src" > "$rendered"
      install -m 644 "$rendered" "$dst"
      rm -f "$rendered"
      info "installed $dst"
    fi
  done

  ok "spire-server $(/usr/local/bin/spire-server --version 2>&1)"
}

# ─── what building the daemon needs ──────────────────────────────────────
# Checked rather than installed: a distribution package name differs per host, and a wrong apt line
# is worse than a named missing tool. cargo is here because the rego evaluator is a Rust crate the
# daemon links, and its absence only shows as a cmake failure deep in install.sh.
check_toolchain() {
  step "build toolchain"
  # rustup puts cargo under the caller's home, which sudo's secure_path does not reach, so a host
  # that has a toolchain reads as having none.
  if ! command -v cargo > /dev/null 2>&1; then
    local caller="${SUDO_USER:-}"
    local callerCargo=""
    [[ -n "$caller" ]] && callerCargo="$(getent passwd "$caller" | cut -d: -f6)/.cargo/bin"
    if [[ -n "$callerCargo" && -x "${callerCargo}/cargo" ]]; then
      info "cargo at ${callerCargo}, which sudo's secure_path does not carry"
      hint "pass it through: sudo env PATH=\"${callerCargo}:\$PATH\" bash install.sh"
    fi
  fi

  local missing=()
  local tool
  for tool in cmake cargo; do
    command -v "$tool" > /dev/null 2>&1 || missing+=("$tool")
  done
  command -v g++ > /dev/null 2>&1 || command -v clang++ > /dev/null 2>&1 || missing+=("a C++17 compiler")
  # libcurl is the OPA backend's transport and libssl digests a caller's exe for the sha256 selector.
  # A multiarch install puts the headers under the triplet, so pkg-config answers before a path does.
  pkg-config --exists libcurl 2>/dev/null \
    || [[ -e /usr/include/curl/curl.h ]] \
    || compgen -G "/usr/include/*/curl/curl.h" > /dev/null \
    || missing+=("libcurl4-openssl-dev")
  pkg-config --exists openssl 2>/dev/null \
    || [[ -e /usr/include/openssl/evp.h ]] \
    || missing+=("libssl-dev")

  if [[ ${#missing[@]} -eq 0 ]]; then
    ok "everything the daemon build needs is here"
    return 0
  fi
  for tool in "${missing[@]}"; do warn "missing: $tool"; done
  info "on Debian and Ubuntu: apt install cmake g++ cargo libcurl4-openssl-dev libssl-dev"
  return 1
}

# ─── bring-up (ephemeral SPIRE + OPA daemons for dev / e2e) ──────────────
# PID/log inventory under /run/${FS_NAME}-bridges/; survives script exit.
BRINGUP_STATE_DIR="${BRINGUP_STATE_DIR:-/run/${FS_NAME}-bridges}"
BRINGUP_SERVER_DIR="${BRINGUP_SERVER_DIR:-/tmp/spire-server}"
BRINGUP_AGENT_DIR="${BRINGUP_AGENT_DIR:-/tmp/spire-agent}"
BRINGUP_SERVER_PORT="${BRINGUP_SERVER_PORT:-${SPIRE_SERVER_PORT}}"
# A path and not a port, for the same reason the unit uses one: /run is root's, so no other
# account can answer in OPA's place. The one the config names.
BRINGUP_OPA_SOCK="$OPA_SOCKET"
BRINGUP_OPA_DIR="$(dirname "$OPA_SOCKET")"
BRINGUP_SERVER_SOCK="$BRINGUP_SERVER_DIR/private/api.sock"
# Use /run, not /tmp: the helper unit has PrivateTmp=true so sockets
# under /tmp would be invisible to it.
# SPIRE 1.14+: Delegated Identity API is on admin_socket_path only.
BRINGUP_WORKLOAD_DIR="/run/spire-agent/public"
BRINGUP_WORKLOAD_SOCK="$BRINGUP_WORKLOAD_DIR/api.sock"
BRINGUP_ADMIN_SOCK="$SPIRE_ADMIN_SOCKET"
BRINGUP_ADMIN_DIR="$(dirname "$SPIRE_ADMIN_SOCKET")"
BRINGUP_AGENT_PARENT_ID="spiffe://$TRUST_DOMAIN/agent/$(hostname)"

bringup_pidalive() { [[ -f "$1" ]] && kill -0 "$(cat "$1")" 2>/dev/null; }

bringup_spire_server() {
  step "bring-up: spire-server"
  mkdir -p "$BRINGUP_SERVER_DIR/private" "$BRINGUP_SERVER_DIR/data" "$BRINGUP_STATE_DIR"
  if bringup_pidalive "$BRINGUP_STATE_DIR/spire-server.pid"; then
    info "already running (pid=$(cat "$BRINGUP_STATE_DIR/spire-server.pid"))"
  else
    cat >"$BRINGUP_SERVER_DIR/server.conf" <<EOF
server {
  bind_address = "127.0.0.1"
  bind_port = $BRINGUP_SERVER_PORT
  socket_path = "$BRINGUP_SERVER_SOCK"
  trust_domain = "$TRUST_DOMAIN"
  data_dir = "$BRINGUP_SERVER_DIR/data"
  log_level = "WARN"
  ca_ttl = "24h"
  default_x509_svid_ttl = "1h"
}
plugins {
  DataStore "sql" { plugin_data {
    database_type = "sqlite3"
    connection_string = "$BRINGUP_SERVER_DIR/data/datastore.sqlite3"
  } }
  NodeAttestor "join_token" { plugin_data {} }
  KeyManager "memory" { plugin_data {} }
}
EOF
    /usr/local/bin/spire-server run -config "$BRINGUP_SERVER_DIR/server.conf" \
      >"$BRINGUP_STATE_DIR/spire-server.log" 2>&1 &
    echo $! >"$BRINGUP_STATE_DIR/spire-server.pid"
    info "pid=$(cat "$BRINGUP_STATE_DIR/spire-server.pid")"
  fi
  for _ in {1..20}; do
    /usr/local/bin/spire-server healthcheck \
      -socketPath "$BRINGUP_SERVER_SOCK" >/dev/null 2>&1 \
      && { ok "healthy"; return; }
    sleep 0.5
  done
  tail -20 "$BRINGUP_STATE_DIR/spire-server.log" >&2
  die "spire-server failed to become healthy"
}

bringup_helper_uid() {
  # The uid comes from the account install.sh creates and the unit's User= names.
  # A process name proves nothing, because any user can set it.
  local helperUser="$DAEMON_ACCOUNT" helperUid
  helperUid=$(id -u "$helperUser" 2>/dev/null) || die "daemon account $helperUser does not exist"
  [[ "$helperUid" != 0 ]] || die "daemon account $helperUser resolves to root"
  printf '%s\n' "$helperUid"
}

bringup_allow_helper_sockets() {
  command -v setfacl >/dev/null || die "setfacl is required for SPIRE socket access (install acl)"
  local helperUid socketDir socketPath
  helperUid=$(bringup_helper_uid) || return 1

  # Only the helper traverses these directories or connects to the agent's two APIs.
  for socketDir in "$BRINGUP_WORKLOAD_DIR" "$BRINGUP_ADMIN_DIR"; do
    chown root:root "$socketDir" || return 1
    chmod 0700 "$socketDir" || return 1
    setfacl -b "$socketDir" || return 1
    setfacl -m "u:${helperUid}:--x" "$socketDir" || return 1
  done
  for socketPath in "$BRINGUP_WORKLOAD_SOCK" "$BRINGUP_ADMIN_SOCK"; do
    [[ -S "$socketPath" ]] || die "SPIRE socket missing: $socketPath"
    chown root:root "$socketPath" || return 1
    chmod 0600 "$socketPath" || return 1
    setfacl -b "$socketPath" || return 1
    setfacl -m "u:${helperUid}:rw-" "$socketPath" || return 1
  done
}

bringup_spire_agent() {
  step "bring-up: spire-agent"
  mkdir -p "$BRINGUP_AGENT_DIR/data" "$BRINGUP_WORKLOAD_DIR" "$BRINGUP_ADMIN_DIR" "$BRINGUP_STATE_DIR"
  if bringup_pidalive "$BRINGUP_STATE_DIR/spire-agent.pid"; then
    info "already running (pid=$(cat "$BRINGUP_STATE_DIR/spire-agent.pid"))"
    bringup_allow_helper_sockets || die "cannot grant daemon access to SPIRE sockets"
    return
  fi
  /usr/local/bin/spire-server bundle show \
    -socketPath "$BRINGUP_SERVER_SOCK" \
    >"$BRINGUP_AGENT_DIR/data/bundle.pem"
  cat >"$BRINGUP_AGENT_DIR/agent.conf" <<EOF
agent {
  data_dir = "$BRINGUP_AGENT_DIR/data"
  log_level = "WARN"
  server_address = "127.0.0.1"
  server_port = $BRINGUP_SERVER_PORT
  trust_domain = "$TRUST_DOMAIN"
  socket_path = "$BRINGUP_WORKLOAD_SOCK"
  admin_socket_path = "$BRINGUP_ADMIN_SOCK"
  trust_bundle_path = "$BRINGUP_AGENT_DIR/data/bundle.pem"
  authorized_delegates = ["spiffe://$TRUST_DOMAIN/${DAEMON_NAME}"]
  # Pull entries aggressively so e2e bring-up doesn't race the default 5s sync.
  experimental {
    sync_interval = "500ms"
  }
}
plugins {
  NodeAttestor "join_token" { plugin_data {} }
  KeyManager "memory" { plugin_data {} }
  WorkloadAttestor "unix" { plugin_data { discover_workload_path = true } }
}
EOF
  info "generating join token (parent=$BRINGUP_AGENT_PARENT_ID)"
  local token
  token=$(/usr/local/bin/spire-server token generate \
    -socketPath "$BRINGUP_SERVER_SOCK" \
    -spiffeID "$BRINGUP_AGENT_PARENT_ID" -ttl 600 \
    | awk -F': *' '/Token/ {print $2}')
  [[ -n "$token" ]] || die "join token generation failed"
  /usr/local/bin/spire-agent run -config "$BRINGUP_AGENT_DIR/agent.conf" \
    -joinToken "$token" \
    >"$BRINGUP_STATE_DIR/spire-agent.log" 2>&1 &
  echo $! >"$BRINGUP_STATE_DIR/spire-agent.pid"
  info "pid=$(cat "$BRINGUP_STATE_DIR/spire-agent.pid")"
  for _ in {1..20}; do
    if [[ -S "$BRINGUP_WORKLOAD_SOCK" && -S "$BRINGUP_ADMIN_SOCK" ]]; then
      bringup_allow_helper_sockets || die "cannot grant daemon access to SPIRE sockets"
      ok "workload API: $BRINGUP_WORKLOAD_SOCK"
      ok "admin API:    $BRINGUP_ADMIN_SOCK"
      return
    fi
    sleep 0.5
  done
  tail -20 "$BRINGUP_STATE_DIR/spire-agent.log" >&2
  die "spire-agent sockets never appeared (workload + admin)"
}

bringup_opa() {
  step "bring-up: opa"
  mkdir -p "$BRINGUP_STATE_DIR"
  if bringup_pidalive "$BRINGUP_STATE_DIR/opa.pid"; then
    info "already running (pid=$(cat "$BRINGUP_STATE_DIR/opa.pid"))"
    return
  fi
  [[ -f "$POLICY_FILE_RUNTIME" ]] || die "$POLICY_FILE_RUNTIME missing — run install.sh first"

  # root's and traversable by the daemon's group alone, so nothing else on this host can put an
  # endpoint at that name or query the one that is there.
  local opaGroup=root opaDirMode=0700
  if id "$DAEMON_ACCOUNT" >/dev/null 2>&1; then
    opaGroup="$(id -gn "$DAEMON_ACCOUNT")"
    opaDirMode=0750
  fi
  # A socket a killed run left behind would make the bind fail, so it goes first.
  install -d -o root -g "$opaGroup" -m "$opaDirMode" "$BRINGUP_OPA_DIR"
  rm -f "$BRINGUP_OPA_SOCK"

  local previousMask
  previousMask="$(umask)"
  umask 0007
  /usr/local/bin/opa run --server --watch \
    --addr "unix://$BRINGUP_OPA_SOCK" --log-level=error \
    "$POLICY_FILE_RUNTIME" >"$BRINGUP_STATE_DIR/opa.log" 2>&1 &
  echo $! >"$BRINGUP_STATE_DIR/opa.pid"
  umask "$previousMask"

  for _ in {1..20}; do
    if curl -sf --unix-socket "$BRINGUP_OPA_SOCK" "http://localhost/health" >/dev/null; then
      chgrp "$opaGroup" "$BRINGUP_OPA_SOCK"
      chmod 0660 "$BRINGUP_OPA_SOCK"
      ok "healthy on $BRINGUP_OPA_SOCK"
      return
    fi
    sleep 0.3
  done
  tail -20 "$BRINGUP_STATE_DIR/opa.log" >&2
  die "opa never became healthy"
}

bringup_register_helper_entry() {
  # Register ${DAEMON_NAME}'s own workload entry (required for Delegated Identity API).
  # The uid is the daemon account's, never a running process's.
  local helper_uid
  helper_uid=$(bringup_helper_uid) || return 1
  /usr/local/bin/spire-server entry create \
    -socketPath "$BRINGUP_SERVER_SOCK" \
    -parentID "$BRINGUP_AGENT_PARENT_ID" \
    -spiffeID "spiffe://$TRUST_DOMAIN/${DAEMON_NAME}" \
    -selector "unix:uid:$helper_uid" >/dev/null 2>&1 || true
  info "helper entry: spiffe://$TRUST_DOMAIN/${DAEMON_NAME} ← unix:uid:$helper_uid"
  # Wait for agent cache to reflect the new entry before returning. The
  # entry is registered with unix:uid:$helper_uid, so the workload API
  # must be queried as that uid (caller's uid 0 wouldn't match the
  # selector and the SVID wouldn't surface in the cache check).
  local probe_user
  probe_user=$(getent passwd "$helper_uid" | cut -d: -f1)
  probe_user="${probe_user:-${FS_NAME}}"
  local waited=0
  while ! runuser -u "$probe_user" -- /usr/local/bin/spire-agent api fetch x509 \
            -socketPath "$BRINGUP_WORKLOAD_SOCK" 2>/dev/null \
            | grep -q "spiffe://$TRUST_DOMAIN/${DAEMON_NAME}"; do
    sleep 0.2
    waited=$((waited + 1))
    if (( waited >= 50 )); then
      warn "helper entry not visible in agent cache after 10s"
      break
    fi
  done
  if (( waited < 50 )); then
    info "helper entry propagated to agent cache (${waited}*0.2s)"
  fi
}

bringup_verify() {
  step "bring-up: verify"
  /usr/local/bin/spire-server healthcheck \
    -socketPath "$BRINGUP_SERVER_SOCK" >/dev/null \
    && info "spire-server: healthy" \
    || { fail "spire-server unhealthy"; return 1; }
  [[ -S "$BRINGUP_WORKLOAD_SOCK" ]] \
    && info "spire-agent workload socket: $BRINGUP_WORKLOAD_SOCK" \
    || { fail "workload socket missing"; return 1; }
  [[ -S "$BRINGUP_ADMIN_SOCK" ]] \
    && info "spire-agent admin socket:    $BRINGUP_ADMIN_SOCK" \
    || { fail "admin socket missing (Delegated Identity API will fail)"; return 1; }
  curl -sf --unix-socket "$BRINGUP_OPA_SOCK" "http://localhost/health" >/dev/null \
    && info "opa: healthy on $BRINGUP_OPA_SOCK" \
    || { fail "opa unhealthy"; return 1; }
  local n
  n=$(/usr/local/bin/spire-server entry show \
    -socketPath "$BRINGUP_SERVER_SOCK" 2>/dev/null \
    | grep -c "^Entry ID" || true)
  info "entries registered: $n"
  ok "verify passed"
}

bringup_teardown() {
  step "teardown bring-up daemons"
  # 1. PID-file driven kill (the clean path)
  for f in "$BRINGUP_STATE_DIR"/spire-agent.pid \
           "$BRINGUP_STATE_DIR"/spire-server.pid \
           "$BRINGUP_STATE_DIR"/opa.pid; do
    [[ -f "$f" ]] || continue
    local pid
    pid=$(cat "$f")
    if kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
      info "stopped pid=$pid ($(basename "$f" .pid))"
    fi
    rm -f "$f"
  done
  # pkill fallback for orphans from a crashed prior bring-up.
  pkill -TERM -f "^/usr/local/bin/spire-server" 2>/dev/null || true
  pkill -TERM -f "^/usr/local/bin/spire-agent"  2>/dev/null || true
  pkill -TERM -f "^/usr/local/bin/opa run "     2>/dev/null || true
  sleep 0.3
  pkill -KILL -f "^/usr/local/bin/spire-server" 2>/dev/null || true
  pkill -KILL -f "^/usr/local/bin/spire-agent"  2>/dev/null || true
  pkill -KILL -f "^/usr/local/bin/opa run "     2>/dev/null || true
  rm -rf "$BRINGUP_STATE_DIR" "$BRINGUP_SERVER_DIR" "$BRINGUP_AGENT_DIR" \
         "$BRINGUP_OPA_DIR" /run/spire-agent
  rm -rf /run/${FS_NAME}-test-spire
  ok "teardown complete"
}

install_systemd_unit() {
  local name="$1" content="$2"
  local dst="/etc/systemd/system/${name}.service"
  if [[ -f "$dst" && $FORCE_CFG -ne 1 ]]; then
    info "preserving existing $dst"
    return
  fi
  printf '%s' "$content" >"$dst"
  info "installed $dst"
}

install_systemd() {
  step "systemd units"
  # The socket is reachable by the daemon's own account and by nobody else. Without that group it
  # stays root's alone, so the daemon is refused rather than the decision opened to every account.
  local opaGroupLine="# Group: run tools/deploy/install.sh, then reinstall this with --force"
  local opaDirMode="0700"
  local opaMask="0077"
  if id "$DAEMON_ACCOUNT" >/dev/null 2>&1; then
    opaGroupLine="Group=$(id -gn "$DAEMON_ACCOUNT")"
    opaDirMode="0750"
    opaMask="0007"
  else
    warn "no ${DAEMON_ACCOUNT} account yet, so OPA's socket stays root's and the daemon cannot reach it"
  fi
  # systemd makes the directory under /run that the socket sits in.
  [[ "$BRINGUP_OPA_DIR" == /run/* ]] || die "policy.opa_socket ${OPA_SOCKET} is not under /run"
  local opaRuntimeDir="${BRINGUP_OPA_DIR#/run/}"

  # Unquoted delimiter: every $ in this unit (FS_NAME and the opa* locals above) must resolve
  # before the file is written, unlike the quoted heredocs below.
  install_systemd_unit opa "$(cat <<UNIT
[Unit]
Description=Open Policy Agent (${DAEMON_NAME} policy backend)
After=network.target

[Service]
# root, because the policy file opens to root and the daemon's account alone. A User= here would
# have to be granted that file as well.
${opaGroupLine}
# A path under /run and not a port: /run is root's, so no other account can hold the endpoint
# first, and nothing in the exchange would tell the real server from one that did.
RuntimeDirectory=${opaRuntimeDir}
RuntimeDirectoryMode=${opaDirMode}
UMask=${opaMask}
ExecStart=/usr/local/bin/opa run --server --addr unix://${OPA_SOCKET} ${POLICY_FILE_RUNTIME}
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
UNIT
)"

  install_systemd_unit spire-server "$(cat <<'UNIT'
[Unit]
Description=SPIRE Server
After=network.target

[Service]
ExecStart=/usr/local/bin/spire-server run -config /etc/spire/server/server.conf
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
UNIT
)"

  install_systemd_unit spire-agent "$(cat <<'UNIT'
[Unit]
Description=SPIRE Agent
After=network.target spire-server.service
Requires=spire-server.service

[Service]
# Set SPIRE_JOIN_TOKEN before starting:
#   sudo systemctl set-environment SPIRE_JOIN_TOKEN=$(spire-server token generate \
#     -spiffeID spiffe://${FS_NAME}.local/agent/$(hostname) -ttl 600 | awk '{print $2}')
ExecStartPre=/bin/sh -c '/usr/local/bin/spire-server bundle show > /var/lib/spire/agent/bundle.crt'
ExecStart=/usr/local/bin/spire-agent run -config /etc/spire/agent/agent.conf -joinToken ${SPIRE_JOIN_TOKEN}
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
UNIT
)"

  systemctl daemon-reload
  ok "systemd units installed (enable + start manually after configuring trust)"
}

# ─── basic smoke ─────────────────────────────────────────────────────────
_opa_eval_check() {
  local rego="$1" label="$2" input="$3" query="$4" expected="$5"
  local out
  out="$(/usr/local/bin/opa eval -d "$rego" --input <(echo "$input") \
        --format=raw "$query" 2>&1)" || {
    fail "opa eval crashed: $out"
    return 1
  }
  info "[$label] $query → $out"
  if [[ "$out" == "$expected" ]]; then
    return 0
  fi
  fail "[$label] expected '$expected', got '$out'"
  return 1
}

smoke_opa() {
  step "smoke: OPA policy eval"
  local rego="$SCRIPT_DIR/policy/policy.example.rego"
  [[ -f "$rego" ]] || { warn "missing $rego (skip)"; return; }
  info "OPA version: $(/usr/local/bin/opa version | head -1)"
  info "policy file: $rego"

  _opa_eval_check "$rego" "same-group llm-worker (ALLOW)" \
    '{"consumer":{"group":"prod","role":"llm-worker"},"owner":{"group":"prod","role":"llm-worker"}}' \
    'data.'"${FS_NAME}"'.authz.allow' 'true' || return 1

  _opa_eval_check "$rego" "cross-group attempt (DENY)" \
    '{"consumer":{"group":"prod","role":"llm-worker"},"owner":{"group":"other","role":"llm-worker"}}' \
    'data.'"${FS_NAME}"'.authz.allow' 'false' || return 1

  _opa_eval_check "$rego" "admin-tools rule name" \
    '{"consumer":{"group":"prod","role":"'"${FS_NAME}"'-admin"},"owner":{"group":"x","role":"y"}}' \
    'data.'"${FS_NAME}"'.authz.rule' 'admin-tools' || return 1

  ok "OPA policy semantics match expectations across 3 scenarios"
}

smoke_spire() {
  step "smoke: SPIRE binaries"
  local sv av
  sv="$(/usr/local/bin/spire-server --version 2>&1 || true)"
  av="$(/usr/local/bin/spire-agent  --version 2>&1 || true)"
  [[ -n "$sv" ]] && info "spire-server: $sv" || { fail "spire-server --version failed"; return 1; }
  [[ -n "$av" ]] && info "spire-agent:  $av" || { fail "spire-agent --version failed";  return 1; }
  if /usr/local/bin/spire-server validate -config /etc/spire/server/server.conf >/dev/null 2>&1; then
    ok "spire-server config parses"
  else
    warn "spire-server validate skipped or failed (older SPIRE may lack this subcommand)"
  fi
}

# ─── post-install hint ────────────────────────────────────────────────────
print_next_steps() {
  cat <<EOF

${C_BOLD}Next steps${C_RST}
  • Confirm trust_domain matches:        grep trust_domain /etc/spire/*/*.conf
  • Policy file at /etc/${FS_NAME}/policy.rego is planted by ${DAEMON_NAME}'s
    install.sh from deploy/policy/policy.example.rego — edit in place after
    install or rerun install.sh --reset-cfg to overwrite.
  • Local policy backend uses the in-process ``regorus`` library (no opa
    binary needed). Skip OPA install with --spire-only unless you actually
    plan to run ``policy.backend: opa`` against an OPA daemon.
  • Start OPA (only if running the bridge backend):
                                         sudo systemctl start opa
  • Start SPIRE server then agent (after generating a join token — see the
    \`ExecStartPre\` comment in /etc/systemd/system/spire-agent.service).
  • Register a workload entry:
      sudo spire-server entry create \\
        -parentID spiffe://$TRUST_DOMAIN/agent/\$(hostname) \\
        -spiffeID spiffe://$TRUST_DOMAIN/group/${FS_NAME} \\
        -selector unix:uid:\$(id -u)
  • Verify SVID:                         spire-agent api fetch x509 \\
                                             -socketPath /tmp/spire-agent/public/api.sock
EOF
}

# ─── runtime smoke (SPIRE end-to-end, ephemeral) ─────────────────────────
smoke_spire_runtime() {
  step "smoke: SPIRE end-to-end"
  if [[ ! -x "$SCRIPT_DIR/identity/smoke-spire.sh" ]]; then
    warn "identity/smoke-spire.sh missing — skipped"
    return 0
  fi
  bash "$SCRIPT_DIR/identity/smoke-spire.sh" \
    || die "SPIRE runtime smoke failed — see /tmp/spire-smoke.*"
}

# ─── dispatch ─────────────────────────────────────────────────────────────
if [[ $DO_TEARDOWN_BRINGUP -eq 1 ]]; then
  bringup_teardown
  exit 0
fi
if [[ $DO_BRINGUP -eq 1 ]]; then
  [[ $DO_OPA     -eq 1 && ! -x /usr/local/bin/opa ]]           && install_opa
  [[ $DO_SPIRE   -eq 1 && ! -x /usr/local/bin/spire-server ]]  && install_spire
  [[ $DO_SPIRE   -eq 1 ]] && bringup_spire_server
  [[ $DO_SPIRE   -eq 1 ]] && bringup_spire_agent
  [[ $DO_SPIRE   -eq 1 ]] && bringup_register_helper_entry
  [[ $DO_OPA     -eq 1 ]] && bringup_opa
  bringup_verify
  exit 0
fi
[[ $DO_OPA     -eq 1 ]] && install_opa
[[ $DO_SPIRE   -eq 1 ]] && install_spire
[[ $DO_TOOLCHAIN -eq 1 ]] && check_toolchain
[[ $INSTALL_SYSTEMD -eq 1 ]] && install_systemd
[[ $DO_OPA     -eq 1 ]] && smoke_opa
[[ $DO_SPIRE   -eq 1 ]] && smoke_spire
[[ $DO_SPIRE   -eq 1 && $RUN_RUNTIME_SMOKE -eq 1 ]] && smoke_spire_runtime
print_next_steps
