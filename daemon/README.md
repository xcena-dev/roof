# daemon

The daemon the kernel module asks when it cannot decide alone: 
who a process is, and which permissions it gets on a region.
One instance runs per mount 
and answers the upcalls on `/dev/roofd-<node>`, the upcall channel.

| Upcall | When | Question |
|---|---|---|
| `ATTEST_REQUEST` | region create | what identity does this process have? |
| `ACCESS_REQUEST` | a `read` or `mmap` no row covers | consumer X asks for a region owned by Y: which permissions? |
| `LOCK_REQUEST` / `UNLOCK_REQUEST` | a metadata write | take and give back this node's turn on the shared lock region |

Why the split exists and why each side trusts the other is in [`docs/design.md`](../docs/design.md).
This file is what someone building, configuring or running the daemon needs.

## How it is put together

![The helper daemon, one process per mount, between the kernel module on the left and what it reaches on the right. The kernel's ATTEST, ACCESS and LOCK or UNLOCK upcalls arrive on the daemon's main loop, which hands a request that would block to a worker pool. The identity provider has a SPIRE and a local backend, the policy engine an OPA and a local backend, and the turn service reaches the lock region through libcme. Outside the daemon, the SPIRE backend reaches the SPIRE agent through the Delegated Identity API, the OPA backend reaches the shared OPA server over HTTP, and the turn service meets the other nodes' turn services through shared memory with no network.](../docs/figs/daemon.png)

The arrows into the daemon interface are the kernel's upcalls, 
each with what its answer carries.
The arrows from the daemon interface to a module are 
where the daemon calls that module for the upcall.
The arrows from a module outward name what it reaches and how.

- **Main loop.**
  One thread waits on the upcall channel 
  and answers every request that completes without blocking.
- **Worker pool.**
  Takes a request whose answer has to wait on something outside the daemon, 
  so the main loop stays free for the rest.
- **Identity provider.**
  Answers who a process is, 
  for the owner at `ATTEST` and for the reader at `ACCESS`.
  The local backend matches the selector rules in `identity-rules.yaml`.
  The SPIRE backend calls the SPIRE Agent's Delegated Identity API.
- **Policy engine.**
  Decides at `ACCESS` which permissions the reader gets on the region, 
  from the reader's identity and the owner's.
  The local backend evaluates `policy.rego` in-process through regorus.
  The OPA backend sends the same query to an OPA server over HTTP.
- **Turn service.**
  A CME `SharedSession` over the lock region, a file inside the mount, 
  which serialises metadata writes across nodes.
  The other nodes' turn services meet it there through shared memory, 
  not over a network.
  Present only when the build links CME.
  Without it the daemon does not declare `LOCK`, 
  and every metadata write on the mount is refused.
  A build that links CME but cannot open the region does not start, 
  so a mount never looks healthy while its writes are refused.

## What each upcall carries

Every request names a task by what the kernel read off it: 
pid, uid, gid, start time, and the executable's inode and path.
The kernel fills in every field from the task itself, 
so a process cannot claim to be someone else.

| Upcall | Kernel sends | Daemon answers | What the kernel does with the answer |
|---|---|---|---|
| `HELLO` | | `capabilities`: `ATTEST_OWNER`, `ACCESS_REQUEST`, `OPA_RESPONSE`, `REVOKE`, `LOCK` | queues a request only to a daemon that declared its kind;<br>no `LOCK` means every metadata write is refused |
| `ATTEST_REQUEST` | the creating task | `status`, `group`, `role` | stamps `group` and `role` into the region as `owner_group` and `owner_role`;<br>non-zero `status` refuses the create |
| `ACCESS_REQUEST` | the calling task, plus the region's `owner_group` and `owner_role` | `status`, `granted_perms`, `expiry_secs` | masks to `READ`, `WRITE`, `GRANT`, `ADMIN`, admits if every needed bit is there, and writes a delegation row so the next access asks nobody;<br>`expiry_secs` is not yet read |
| `LOCK_REQUEST` / `UNLOCK_REQUEST` | `domain`, and on lock `timeout_ms` | `status` | 0 is the turn, taken after the mount's own mutex;<br>anything else refuses the write with `-EAGAIN`, a silent daemon with `-ETIMEDOUT` |


## Why SPIRE and OPA

Both questions already have an answer in the wider ecosystem, 
so the backends build on those rather than on a scheme of this project's own.

**SPIRE** answers *who is this process*.
It attests a workload from facts the platform vouches for, 
and issues a signed, short-lived SPIFFE id, `spiffe://<trust-domain>/<path>`.
A pid or a uid means something only on the host that issued it.
A SPIFFE id is the same name on every node of the trust domain, 
so an owner stamped on node 1 and a consumer asking from node 2 are compared under one scheme.

**OPA** answers *which permissions does it get*.
Policy is Rego evaluated as data, 
so the rules live in a file an operator changes and audits, not in daemon code.
The same file is served by an OPA process shared across a fleet, 
or evaluated inside the daemon by regorus with no network hop.

**The local backends fit those two.**
The local identity backend hands out SPIFFE ids of the same form, 
so a policy written against `input.consumer.spiffe_id` runs unchanged when a host moves to SPIRE.
The local policy backend evaluates the very same `policy.rego`.
A host starts with the two local backends and no other service, 
and switches either one by changing a name in `daemon.yaml`.

## Configuration

Three files under `/etc/rooffs/`, planted from the examples in [`deploy/`](deploy/) by the installer. <!-- fsname -->
Each is owned by the daemon's own account at mode `0400`, with root as its group,
so root and that account are the only ones that read them.
The daemon reads them where they are.
Edit one with `sudo`, then send `SIGHUP` to reload all three.
An inline `policy:` key is refused, 
so the policy file stays the single source of truth.

### `daemon.yaml`: which backends, and how to reach them

```yaml
backends:
  identity: local        # or spire
  policy:   local        # or opa

# identity: local
identity_rules_path: identity-rules.yaml   # relative, so it resolves next to this file
selectors: [uid, path]   # the match kinds the identity rules may use

# policy: local
policy_path: policy.rego                   # relative, same reasoning

# identity: spire
spire_workload_api_socket: /run/spire-agent/admin/api.sock

# policy: opa
policy_url: http://127.0.0.1:8181/v1/data/rooffs/authz
```
<!-- fsname -->

- `backends` selects by registered name, 
  and a name nothing registered under is refused at startup.
  `spire` needs the SPIRE Agent's admin socket, 
  and `opa` an OPA server at `policy_url`.
- `selectors` is the allow-list of match kinds an identity rule may use.
  A rule using any other kind is dropped when the rules load.

### `identity-rules.yaml`: who a process is

The kernel sends the task's uid, gid and executable.
A rule matches on those and hands back a SPIFFE id, a group and a role.

```yaml
rules:
  - match:
      uid: 1005
    identity:
      spiffe_id: spiffe://rooffs.local/group/prod/role/llm-worker
      group:     prod
      role:      llm-worker

default: deny
```
<!-- fsname -->

- The selectors inside one `match` AND together, 
  and the first rule that matches wins.
- `default: deny` is what a task matching no rule gets, 
  and a create by such a task is refused.

### `policy.rego`: which permissions an identity gets

The policy takes who asks and who owns the region, 
and returns which permissions the asker gets.
regorus and OPA evaluate the same file.

```rego
package rooffs.authz

import rego.v1

default allow := false
default granted_perms := []
default rule := ""

same_group_llm_worker if {
    input.consumer.group == "prod"
    input.consumer.role in ["llm-worker", "llm-prefill", "llm-decode"]
    input.owner.group == input.consumer.group
}

allow if same_group_llm_worker
granted_perms := ["READ", "WRITE"] if same_group_llm_worker
rule := "same-group-llm-worker" if same_group_llm_worker
```
<!-- fsname -->

- The input is exactly this, and nothing else crosses to the policy:

  ```json
  {
    "consumer": {"spiffe_id": "...", "group": "...", "role": "..."},
    "owner":    {"group": "...", "role": "..."}
  }
  ```

- The daemon reads back `allow`, `granted_perms` and `rule`.
  `granted_perms` lists `READ`, `WRITE`, `DELETE`, `ADMIN`, `IOCTL`, `GRANT` or `ALL`, 
  and an unknown name denies.
  `rule` names the rule that matched, for the audit line.
- A rule such as `granted_perms` may be defined more than once only with mutually exclusive bodies.
  Two that hold at once fail the evaluation, which denies.

## License

Apache-2.0.
See [`LICENSE`](../LICENSE) at the root of the repository.
