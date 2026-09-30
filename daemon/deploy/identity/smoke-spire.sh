#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# smoke-spire.sh — minimal end-to-end SPIRE sanity check (~5-10s).
# Usage: sudo bash deploy/identity/smoke-spire.sh [--keep]
# Env:   TRUST_DOMAIN (default: ${FS_NAME}.local)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
. "${ROOT}/fsname"

TRUST_DOMAIN="${TRUST_DOMAIN:-${FS_NAME}.local}"
SERVER_PORT=18081           # avoid 8081 in case prod SPIRE is up
KEEP=0
for arg in "$@"; do
  case "$arg" in
    --keep) KEEP=1 ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    *) echo "unknown flag: $arg" >&2; exit 2 ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "must run as root (sudo)" >&2
  exit 1
fi

[[ -x /usr/local/bin/spire-server ]] || { echo "spire-server missing — run install-deps.sh"; exit 1; }
[[ -x /usr/local/bin/spire-agent  ]] || { echo "spire-agent missing  — run install-deps.sh"; exit 1; }

TMP="$(mktemp -d -t spire-smoke.XXXXXX)"
# mktemp gives us 0700; the workload runs as a non-root user via sudo -u
# and needs to traverse $TMP and open the agent Workload API socket.
chmod 0755 "$TMP"
SERVER_PID=""
AGENT_PID=""

cleanup() {
  local rc=$?
  set +e
  [[ -n "$AGENT_PID"  ]] && kill "$AGENT_PID"  2>/dev/null
  [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null
  wait "$AGENT_PID" "$SERVER_PID" 2>/dev/null
  # Keep tmp tree on failure (or explicit --keep) so the operator can
  # inspect server.log / agent.log / fetch.log. Only nuke on clean exit.
  if [[ $KEEP -eq 1 || $rc -ne 0 ]]; then
    echo "[smoke] kept tmp tree for inspection: $TMP" >&2
  else
    rm -rf "$TMP"
  fi
}
trap cleanup EXIT INT TERM

step() { printf '\n\033[1m\033[36m── %s\033[0m\n' "$*"; }
ok()   { printf '\033[32mok\033[0m   %s\n' "$*"; }
info() { printf '\033[2m..   %s\033[0m\n' "$*"; }
fail() { printf '\033[31mFAIL\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$TMP/server-data" "$TMP/agent-data" "$TMP/sock"
chmod 0755 "$TMP/sock"

cat >"$TMP/server.conf" <<EOF
server {
    bind_address = "127.0.0.1"
    bind_port    = "$SERVER_PORT"
    trust_domain = "$TRUST_DOMAIN"
    data_dir     = "$TMP/server-data"
    socket_path  = "$TMP/server-api.sock"
    log_level    = "WARN"
}
plugins {
    DataStore "sql" {
        plugin_data {
            database_type     = "sqlite3"
            connection_string = "$TMP/server-data/datastore.sqlite3"
        }
    }
    KeyManager   "memory"     { plugin_data = {} }
    NodeAttestor "join_token" { plugin_data {} }
}
EOF

cat >"$TMP/agent.conf" <<EOF
agent {
    data_dir          = "$TMP/agent-data"
    log_level         = "WARN"
    server_address    = "127.0.0.1"
    server_port       = "$SERVER_PORT"
    trust_bundle_path = "$TMP/bundle.crt"
    trust_domain      = "$TRUST_DOMAIN"
    socket_path       = "$TMP/sock/api.sock"
}
plugins {
    KeyManager       "memory"     { plugin_data {} }
    NodeAttestor     "join_token" { plugin_data {} }
    WorkloadAttestor "unix"       { plugin_data {} }
}
EOF

step "[1/5] starting spire-server (trust_domain=$TRUST_DOMAIN, port=$SERVER_PORT)"
/usr/local/bin/spire-server run -config "$TMP/server.conf" >"$TMP/server.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 30); do
  if /usr/local/bin/spire-server healthcheck \
       -socketPath "$TMP/server-api.sock" >/dev/null 2>&1; then
    break
  fi
  sleep 0.2
done
/usr/local/bin/spire-server healthcheck \
  -socketPath "$TMP/server-api.sock" >/dev/null 2>&1 \
    || fail "spire-server did not become healthy — see $TMP/server.log"
info "admin API socket: $TMP/server-api.sock"
ok "spire-server healthy"

step "[2/5] bootstrap: trust bundle + agent join token"
/usr/local/bin/spire-server bundle show \
  -socketPath "$TMP/server-api.sock" >"$TMP/bundle.crt" \
    || fail "could not fetch trust bundle"
info "trust bundle: $TMP/bundle.crt ($(wc -c <"$TMP/bundle.crt") bytes)"

AGENT_SPIFFE_ID="spiffe://$TRUST_DOMAIN/agent/smoke"
TOKEN="$(/usr/local/bin/spire-server token generate \
  -spiffeID "$AGENT_SPIFFE_ID" -ttl 120 \
  -socketPath "$TMP/server-api.sock" \
  | awk '{print $2}')"
[[ -n "$TOKEN" ]] || fail "token generate produced no output"
info "join token for $AGENT_SPIFFE_ID (TTL=120s): ${TOKEN:0:8}…"
ok "bootstrap material ready"

step "[3/5] starting spire-agent (attestor=join_token)"
/usr/local/bin/spire-agent run -config "$TMP/agent.conf" -joinToken "$TOKEN" \
  >"$TMP/agent.log" 2>&1 &
AGENT_PID=$!
for _ in $(seq 1 30); do
  if /usr/local/bin/spire-agent healthcheck \
       -socketPath "$TMP/sock/api.sock" >/dev/null 2>&1; then
    break
  fi
  sleep 0.2
done
/usr/local/bin/spire-agent healthcheck -socketPath "$TMP/sock/api.sock" >/dev/null 2>&1 \
  || fail "spire-agent did not become healthy — see $TMP/agent.log"
SMOKE_UID="${SUDO_UID:-$(id -u)}"
SMOKE_USER="${SUDO_USER:-root}"

# Agent forces umask 0027 → workload API socket comes up 0750 root:root and rejects the
# non-root workload below. The socket goes to that one account rather than to the world;
# production sites put the workload in the same group as the Agent.
chown "$SMOKE_UID" "$TMP/sock/api.sock" 2>/dev/null || true
chmod 0600 "$TMP/sock/api.sock" 2>/dev/null || true
info "Workload API socket: $TMP/sock/api.sock"
ok "spire-agent healthy and attested as $AGENT_SPIFFE_ID"
WORKLOAD_SPIFFE_ID="spiffe://$TRUST_DOMAIN/group/${FS_NAME}"
step "[4/5] registering workload entry (selector: unix:uid:$SMOKE_UID → $WORKLOAD_SPIFFE_ID)"
/usr/local/bin/spire-server entry create \
  -parentID "$AGENT_SPIFFE_ID" \
  -spiffeID "$WORKLOAD_SPIFFE_ID" \
  -selector "unix:uid:$SMOKE_UID" \
  -socketPath "$TMP/server-api.sock" >/dev/null \
    || fail "entry create failed"
info "parent: $AGENT_SPIFFE_ID  (Agent that may issue this SVID)"
info "user:   $SMOKE_USER (uid=$SMOKE_UID)"
ok "entry registered"

step "[5/5] fetching SVID via Workload API (as uid=$SMOKE_UID)"
# The Agent serves an entry only after its next sync with the Server, so the budget has to outlast
# one sync interval rather than one round trip.
for attempt in $(seq 1 60); do
  if sudo -u "$SMOKE_USER" -- \
       /usr/local/bin/spire-agent api fetch x509 \
         -socketPath "$TMP/sock/api.sock" >"$TMP/fetch.log" 2>&1 \
     && grep -q "SPIFFE ID:" "$TMP/fetch.log"; then
    break
  fi
  sleep 0.3
done

grep -q "SPIFFE ID:" "$TMP/fetch.log" \
  || fail "no SVID produced — see $TMP/fetch.log"
SPIFFE_ID="$(awk '/SPIFFE ID:/ {print $3; exit}' "$TMP/fetch.log")"
# spire-agent prints these labels followed by a raw tab, not a space, before the value.
VALID_AFTER="$(awk -F':\t+' '/SVID Valid After:/  {print $2; exit}' "$TMP/fetch.log")"
VALID_UNTIL="$(awk -F':\t+' '/SVID Valid Until:/  {print $2; exit}' "$TMP/fetch.log")"
info "issued for: $SPIFFE_ID"
info "valid:      $VALID_AFTER  →  $VALID_UNTIL"
ok "SVID delivered to workload"

printf '\n\033[1m\033[32mPASS\033[0m  SPIRE end-to-end smoke OK\n'
