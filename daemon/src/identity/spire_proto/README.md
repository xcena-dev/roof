# SPIRE Delegated Identity API schema

Vendored `.proto` files for the SPIFFE Delegated Identity API.
The daemon is not the workload it attests, so it asks a local spire-agent for another process's SVID over this API rather than over the Workload API.
`protoc` and `grpc_cpp_plugin` turn these into the request, response and stub types `spire_grpc.cpp` uses, and they are generated into the build tree rather than committed.

## Provenance

| | |
| --- | --- |
| Upstream | https://github.com/spiffe/spire-api-sdk |
| Version | `v1.15.3`, commit `adb81b3b0fb87a881693d1d42eaf8b2fa1223e42` |
| Verified | 2026-09-11 |

Five files, because `protoc` needs the whole import closure.
`delegatedidentity.proto` imports `selector`, `x509svid` and `jwtsvid`, and the last two import `spiffeid`.

## The one local change

Import paths are flattened.
Upstream keeps the files under `proto/spire/api/types/` and `proto/spire/api/agent/delegatedidentity/v1/`, and imports them by those paths.
This directory is flat and `protoc` is given one include path, so each `import "spire/api/types/X.proto"` reads `import "X.proto"` here.
Nothing else differs, including the `option go_package` lines, which the C++ build ignores.

## Re-vendoring

Copy the five files from the same upstream paths, flatten the import lines, and update the table above.
Verify with a diff that nothing else moved:

```sh
sed 's|^import "spire/api/types/|import "|' upstream/<name>.proto | diff - <name>.proto
```

Field numbers are the wire contract and names are not transmitted, so a hand-edited schema that drifts from upstream fails silently against a live agent rather than at build time.
