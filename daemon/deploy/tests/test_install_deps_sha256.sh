#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# test_install_deps_sha256 -- verify_sha256() in install-deps.sh actually blocks a bad download,
# discover_spire_asset_url() derives the SPIRE digest URL from the real asset name rather than the
# tarball name, and the ${FS_NAME} placeholders in the systemd units and OPA queries it builds
# resolve to the real filesystem name rather than surviving as literal text.
#
# Sources the real install-deps.sh so each check runs the real function, never a copy, then calls
# it directly. curl, the digest, /usr/local/bin and the policy directory all come from local
# fakes, so no network is touched. The script's own root check needs EUID 0; a user namespace
# supplies that without real privilege.
#
#   ctest --test-dir <build> -R test_install_deps_sha256

set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
INSTALL_DEPS="$(cd "${HERE}/.." && pwd)/install-deps.sh"
POLICY_DIR="$(cd "${HERE}/.." && pwd)/policy"
RENDER_FSNAME="$(cd "${HERE}/../../.." && pwd)/tools/render-fsname.sh"

pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; FAILED=$((FAILED + 1)); }
skip() { printf 'SKIP %s\n' "$*"; exit 77; }
skipCase() { printf 'SKIP %s\n' "$*"; }

command -v unshare >/dev/null 2>&1 || skip "no unshare — cannot fake EUID 0 for install-deps.sh's root check"
unshare --map-root-user --user -- true 2>/dev/null || skip "unprivileged user namespaces are not available here"

FAILED=0
WORKDIR="$(mktemp -d -t test-sha256.XXXXXX)"
trap 'rm -rf "$WORKDIR"' EXIT

# Runs verify_sha256(file, digestUrl) in its own process: a fresh EUID-0 namespace sources
# install-deps.sh with both OPA/SPIRE and the toolchain check turned off, so nothing beyond the
# function under test actually runs, then calls it. Prints its stdout+stderr, returns its exit code.
runVerify() {
  local targetFile="$1" digestUrl="$2"
  local runner="$WORKDIR/runner.sh"
  cat >"$runner" <<RUNNER
#!/usr/bin/env bash
set -euo pipefail
source "$INSTALL_DEPS" --toolchain-only --no-toolchain >/dev/null 2>&1
verify_sha256 "\$1" "\$2"
RUNNER
  chmod +x "$runner"
  unshare --map-root-user --user -- "$runner" "$targetFile" "$digestUrl"
}

caseMatch() {
  local file="$WORKDIR/match.bin" digest
  printf 'A%.0s' $(seq 1 4096) >"$file"
  digest="$(sha256sum "$file" | cut -d' ' -f1)"
  printf '%s  match.bin\n' "$digest" >"$WORKDIR/match.sha256"
  local out exitCode
  out="$(runVerify "$file" "file://$WORKDIR/match.sha256" 2>&1)"; exitCode=$?
  if [[ $exitCode -eq 0 ]]; then
    pass "matching digest lets the file through"
  else
    fail "matching digest lets the file through (exit=$exitCode): $out"
  fi
}

caseMismatch() {
  local file="$WORKDIR/mismatch.bin" digest
  printf 'A%.0s' $(seq 1 4096) >"$file"
  digest="$(sha256sum "$file" | cut -d' ' -f1)"
  printf '%s  mismatch.bin\n' "$digest" >"$WORKDIR/mismatch.sha256"
  # One byte changed after the digest was taken, same as a truncated or tampered download.
  printf 'B' | dd of="$file" bs=1 count=1 conv=notrunc status=none
  local out exitCode
  out="$(runVerify "$file" "file://$WORKDIR/mismatch.sha256" 2>&1)"; exitCode=$?
  if [[ $exitCode -ne 0 && "$out" == *"sha256 mismatch"* ]]; then
    pass "one changed byte stops the install"
  else
    fail "one changed byte stops the install (exit=$exitCode): $out"
  fi
}

caseNoDigest() {
  local file="$WORKDIR/nodigest.bin"
  printf 'irrelevant\n' >"$file"
  local out exitCode
  out="$(runVerify "$file" "file://$WORKDIR/does-not-exist.sha256" 2>&1)"; exitCode=$?
  if [[ $exitCode -ne 0 && "$out" == *"no published sha256"* ]]; then
    pass "an unreachable digest stops the install"
  else
    fail "an unreachable digest stops the install (exit=$exitCode): $out"
  fi
}

caseMalformedDigest() {
  local file="$WORKDIR/malformed.bin"
  printf 'irrelevant\n' >"$file"
  printf 'not a digest\n' >"$WORKDIR/malformed.sha256"
  local out exitCode
  out="$(runVerify "$file" "file://$WORKDIR/malformed.sha256" 2>&1)"; exitCode=$?
  if [[ $exitCode -ne 0 && "$out" == *"no published sha256"* ]]; then
    pass "a non-hex digest stops the install"
  else
    fail "a non-hex digest stops the install (exit=$exitCode): $out"
  fi
}

# Feeds discover_spire_asset_url() a fixed GitHub release payload via a curl() override, so the
# derivation is checked against real asset names without a network call.
caseSpireDigestUrl() {
  local assetsFile="$WORKDIR/spire_assets.json"
  cat >"$assetsFile" <<'ASSETS'
"browser_download_url": "https://github.com/spiffe/spire/releases/download/v1.15.3/spire-1.15.3-linux-amd64-musl.tar.gz"
"browser_download_url": "https://github.com/spiffe/spire/releases/download/v1.15.3/spire-1.15.3-linux-amd64-musl_sha256sum.txt"
"browser_download_url": "https://github.com/spiffe/spire/releases/download/v1.15.3/spire-extras-1.15.3-linux-amd64-musl.tar.gz"
"browser_download_url": "https://github.com/spiffe/spire/releases/download/v1.15.3/spire-extras-1.15.3-linux-amd64-musl_sha256sum.txt"
ASSETS

  local runner="$WORKDIR/runner_discover.sh"
  cat >"$runner" <<RUNNER
#!/usr/bin/env bash
set -euo pipefail
source "$INSTALL_DEPS" --toolchain-only --no-toolchain >/dev/null 2>&1
curl() { cat "$assetsFile"; }
ARCH=amd64
SPIRE_VERSION=v1.15.3
discover_spire_asset_url
printf 'ASSET=%s\nDIGEST=%s\n' "\$SPIRE_ASSET_URL" "\$SPIRE_SHA_URL"
RUNNER
  chmod +x "$runner"
  local out exitCode asset digest
  out="$(unshare --map-root-user --user -- "$runner" 2>&1)"; exitCode=$?
  asset="$(printf '%s\n' "$out" | sed -n 's/^ASSET=//p')"
  digest="$(printf '%s\n' "$out" | sed -n 's/^DIGEST=//p')"
  if [[ $exitCode -eq 0 \
        && "$asset" == *"/spire-1.15.3-linux-amd64-musl.tar.gz" \
        && "$digest" == *"/spire-1.15.3-linux-amd64-musl_sha256sum.txt" \
        && "$asset" != *extras* && "$digest" != *extras* ]]; then
    pass "digest URL derived from the asset's own stem, extras skipped"
  else
    fail "digest URL derived from the asset's own stem, extras skipped (exit=$exitCode): $out"
  fi
}

# Shadows install_systemd_unit and systemctl before calling the real install_systemd(), so the
# real heredocs render into $WORKDIR instead of /etc/systemd/system.
caseSystemdUnitFsName() {
  local unitsDir="$WORKDIR/units"
  mkdir -p "$unitsDir"
  local runner="$WORKDIR/runner_systemd.sh"
  cat >"$runner" <<RUNNER
#!/usr/bin/env bash
set -euo pipefail
source "$INSTALL_DEPS" --toolchain-only --no-toolchain >/dev/null 2>&1
install_systemd_unit() {
  local unitName="\$1" unitContent="\$2"
  printf '%s' "\$unitContent" >"$unitsDir/\${unitName}.service"
}
systemctl() { :; }
install_systemd
RUNNER
  chmod +x "$runner"
  local out exitCode
  out="$(unshare --map-root-user --user -- "$runner" 2>&1)"; exitCode=$?
  # opa.service must resolve ${FS_NAME}. spire-agent.service's ${FS_NAME} sits in an
  # operator-facing example comment and must NOT resolve, so both are checked.
  if [[ $exitCode -eq 0 ]] \
        && grep -qF "/etc/${FS_NAME:-rooffs}/policy.rego" "$unitsDir/opa.service" 2>/dev/null \
        && ! grep -qF '${FS_NAME}' "$unitsDir/opa.service" \
        && grep -qF '${FS_NAME}' "$unitsDir/spire-agent.service"; then
    pass "opa.service resolves \${FS_NAME}; spire-agent.service's example comment keeps it literal"
  else
    fail "opa.service resolves \${FS_NAME}; spire-agent.service's example comment keeps it literal (exit=$exitCode): $out"
  fi
}

# Runs the real smoke_opa() with /usr/local/bin and the policy dir bind-mounted from local fakes
# in a private mount namespace, so nothing on the real host moves. Skips rather than fetching one
# when this host has no opa binary to borrow.
caseSmokeOpaFsName() {
  local realOpa=""
  [[ -x /usr/local/bin/opa ]] && realOpa=/usr/local/bin/opa
  [[ -z "$realOpa" ]] && command -v opa >/dev/null 2>&1 && realOpa="$(command -v opa)"
  if [[ -z "$realOpa" ]]; then
    skipCase "smoke_opa's own query strings (no opa binary on this host)"
    return
  fi
  if ! unshare --map-root-user --user --mount -- true 2>/dev/null; then
    skipCase "smoke_opa's own query strings (no private mount namespace on this host)"
    return
  fi

  local fakeBin="$WORKDIR/opa-fakebin" fakePolicy="$WORKDIR/opa-fakepolicy"
  mkdir -p "$fakeBin" "$fakePolicy"
  cp "$realOpa" "$fakeBin/opa"
  chmod +x "$fakeBin/opa"
  bash "$RENDER_FSNAME" "$POLICY_DIR/policy.example.rego.in" "$fakePolicy/policy.example.rego"

  local runner="$WORKDIR/runner_smoke_opa.sh"
  cat >"$runner" <<RUNNER
#!/usr/bin/env bash
set -euo pipefail
mount --bind "$fakeBin" /usr/local/bin
mount --bind "$fakePolicy" "$POLICY_DIR"
source "$INSTALL_DEPS" --toolchain-only --no-toolchain >/dev/null 2>&1
smoke_opa
RUNNER
  chmod +x "$runner"
  local out exitCode
  out="$(unshare --map-root-user --user --mount -- "$runner" 2>&1)"; exitCode=$?
  if [[ $exitCode -eq 0 && "$out" == *"OPA policy semantics match expectations across 3 scenarios"* ]]; then
    pass "smoke_opa's own query strings resolve against the real, rendered policy"
  else
    fail "smoke_opa's own query strings resolve against the real, rendered policy (exit=$exitCode): $out"
  fi
}

caseMatch
caseMismatch
caseNoDigest
caseMalformedDigest
caseSpireDigestUrl
caseSystemdUnitFsName
caseSmokeOpaFsName

if [[ $FAILED -eq 0 ]]; then
  printf 'every check passed\n'
  exit 0
fi
printf '%d checks failed\n' "$FAILED"
exit 1
