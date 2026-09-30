#!/usr/bin/env bash
# refetch.sh — pull the SPIRE delegated-identity proto files from upstream
# `spire-api-sdk`, flatten their imports, and drop them next to this script.
# Re-runs are safe. The build regenerates the C++ stubs on its own.
#
# Usage:
#   bash refetch.sh                # latest main
#   bash refetch.sh <git-ref>      # pin to a tag or a commit

set -euo pipefail

REF="${1:-main}"
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "[refetch] cloning spiffe/spire-api-sdk@$REF"
git clone --depth 1 --branch "$REF" \
    https://github.com/spiffe/spire-api-sdk.git "$TMP/sdk" 2>/dev/null \
    || git clone --depth 1 https://github.com/spiffe/spire-api-sdk.git "$TMP/sdk"

# spiffeid is here because x509svid and jwtsvid both import it, not because anything calls it.
FILES=(
    "spire/api/agent/delegatedidentity/v1/delegatedidentity.proto"
    "spire/api/types/selector.proto"
    "spire/api/types/x509svid.proto"
    "spire/api/types/jwtsvid.proto"
    "spire/api/types/spiffeid.proto"
)

for src in "${FILES[@]}"; do
    base="$(basename "$src")"
    cp "$TMP/sdk/proto/$src" "$HERE/$base"
    # Flatten every `import "spire/api/.../foo.proto";` to `import "foo.proto";`
    # so protoc resolves against this single directory.
    sed -i -E 's|import "spire/api/[^"]*/([^/"]+\.proto)";|import "\1";|g' "$HERE/$base"
    echo "[refetch] wrote $base"
done

COMMIT="$(cd "$TMP/sdk" && git rev-parse HEAD)"
DATE="$(date -u +%Y-%m-%d)"
echo "[refetch] upstream commit: $COMMIT  (fetched: $DATE)"
echo "[refetch] put that commit and date in SOURCE.md, then rebuild"
