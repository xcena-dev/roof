# Vendored SPIRE proto files

These `.proto` sources are copied from upstream `spire-api-sdk` and re-flattened.
Their `import` paths are rewritten to be filename-only so `protoc` resolves them against this single directory.
The build runs `protoc` over them when `DAEMON_ENABLE_SPIRE` is on.

## Upstream

- Repository: https://github.com/spiffe/spire-api-sdk
- Commit: `b64b3bdfee2f8af0db6d52f7d6f22c30f44f3afb`
- Date fetched: 2026-05-15
- Files, at their original paths under `proto/`:
  - `spire/api/agent/delegatedidentity/v1/delegatedidentity.proto`
  - `spire/api/types/selector.proto`
  - `spire/api/types/x509svid.proto`
  - `spire/api/types/jwtsvid.proto`
  - `spire/api/types/spiffeid.proto`, which x509svid and jwtsvid both import

Licence: Apache-2.0, as the upstream repository states.

## The one local edit

Only the `import` lines differ from upstream.
A line such as `import "spire/api/types/selector.proto";` becomes `import "selector.proto";`.
No message or service definition is altered.

## Re-fetching

`refetch.sh` beside this file does the copy and the flattening.
It prints the upstream commit it took, which is what the Commit line above records.
