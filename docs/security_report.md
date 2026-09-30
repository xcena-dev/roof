# ROOF Security Report

## Overview

This document answers the following questions.
- Which attackers are assumed
- Which security requirements are set
- Which attacks are possible
- Which mechanism stops each one.

The verdicts mean the following.

| Verdict | Meaning |
|---|---|
| 🟢 Blocked | The target action is refused within the stated assumptions. |
| 🟡 Partial | Only some paths are closed. |
| 🔴 Open | No check enforces that goal.<br>Cases the design intends are marked separately. |

The terms below are used throughout.

| Term | What it is |
|---|---|
| node | One host's mount of the shared device, identified by the id the kernel claims in `mount(2)` |
| region | One file on the mount, and one entry in the allocation table on the device |
| default permission | The bits a region grants a caller that holds no row, read after ownership and before the rows.<br>Changing it takes ADMIN. |
| row | One permission record on the device, naming either a process or an account, read only on the node that wrote it |
| lock region | The reserved region the daemons serialise metadata writes through |
| channel | One character device per mount, which the kernel asks its questions on and the daemon answers |
| daemon | The unprivileged process that decides identity and access where the rows do not |
| mount helper | The privileged program `mount(8)` execs, which brings one mount up and takes it down |

## Attacker Model

The attacker is an unprivileged process on a node that has this filesystem mounted.

What the attacker can do.

| Capability | Detail |
|---|---|
| Call the kernel | syscall, ioctl, fork, exec, and the namespace manipulation an ordinary account is permitted |
| Choose what it passes in | the path it opens, the ioctl arguments it fills, and the binary it execs |
| Retry until a pid suits it | fork until the kernel hands back the number it wants |
| Run on any node | claim the same uid and pid on a second node of the same device |
| Keep an identity across exec | tgid, start time and exe inode survive a re-exec of the same binary |
| Speak for the daemon, with its account | Open the channel and answer the kernel's questions |

What the attacker cannot do.

| Limit | Why the attacker is stopped |
|---|---|
| Gain CAP_SYS_MODULE or CAP_SYS_ADMIN | An ordinary account cannot acquire either, so it cannot load a module, mount, or name its own daemon |
| Write the device directly | The DAX device is not mappable by an ordinary account |
| Change which hosts are bound | The Fabric Manager is outside any node's reach |

What the system trusts.

| Trusted | What it is trusted to do |
|---|---|
| Every kernel that maps the device | Read the rows and enforce the decision they carry |
| The loader's `CAP_SYS_MODULE` | Load the module |
| The mount helper's `CAP_SYS_ADMIN` | Call `mount(2)` and claim this node's bootstrap slot |
| root on each node | Name the daemon's account by mounting, and own the channel device |

CXL offers two mechanisms that change this set.<br>
Integrity and Data Encryption (IDE) protects the traffic on the link, and the TEE Security Protocol (TSP) lets a device serve a trusted VM.<br>
Together they put the TEE security manager and an attested device in the place the host holds here.<br>
The trusted set then narrows to a trusted VM's own kernel and the daemon inside it, and a host kernel outside that VM is no longer trusted.

What is out of scope.

| Out of scope | Why | What covers it |
|---|---|---|
| A host with raw write access to the device | It can alter any byte of shared metadata, and no check here reads that host's writes | The Fabric Manager, by not binding it |
| Which hosts map the device | An operator configures it, and no check here sees a host that was not configured | The Fabric Manager |
| A trusted party that misbehaves | The kernel, the daemon and root are the decision path itself | Nothing in this design |

The tables above are drawn below.

![One node the Fabric Manager has bound, holding the attacker beside the root account, the two capabilities a mount needs, the kernel filesystem and the daemon it decides with, a node the Fabric Manager has not bound, and the fabric-attached memory the bound node's kernel reaches. Blue is what the design relies on, red is the attacker and the paths it takes, and grey is what the design does not check.](figs/attacker_model.png)

## Security Requirements

A requirement states what the design must prevent.<br>
The `Violation` column states the event that breaks it, so every entry there must not occur.<br>
The mechanisms that carry the requirements are in [Attack Scenarios and Defenses](#attack-scenarios-and-defenses),<br>
and the last column names the scenarios that exercise each one.

| ID | Requirement | Violation | Scenarios |
|---|---|---|---|
| SR1 | Explicit authority | A caller reaches a region without ownership, the default permission, or a row naming it. | A1 |
| SR2 | Cross-node identity | A uid, gid or pid that one node assigned authorises a caller on another node. | B1, B2, B4 |
| SR3 | Owner continuity | Ownership outlives the process that earned it, so a later process reusing the pid or execing another image inherits it. | A3, A4, A6, A22, H1 |
| SR4 | Authorisation freshness | An authorisation earned by one image still names the process after the image changes or the policy behind it changes. | A20, A21, B3, H3, H4 |
| SR5 | Descriptor confinement | An authorisation travels on a descriptor to a process that was never authorised. | A5, A7, D2, H5, H7 |
| SR6 | Privilege non-escalation | A caller obtains a bit outside the set it holds, whether from a grant or from the daemon's answer. | A10, A11, A12, A13, H2 |
| SR7 | Channel authenticity | An account other than the one the mount named answers the kernel, or an answer attaches to a request it does not belong to. | D1, D3, D7, D8 |
| SR8 | Fail-closed availability | A missing, slow or killed daemon opens access instead of refusing it, or a caller waits without bound. | A17, D5, D6, H6 |
| SR9 | Metadata serialisation | An account other than the daemon's writes the lock region, or an ioctl reaches it as an ordinary region. | F1, F2, F3, F4 |
| SR10 | Reclamation | A dead process's or a dead node's records hold a region, reclaim hands authority back to a caller, or reclaim takes a region from a caller that is still live. | A19, A22, G1, G2 |
| SR11 | No mode-bit bypass | A permission question is answered by the file mode instead of the rows and the daemon. | A2 |
| SR12 | No superuser exemption | `CAP_SYS_ADMIN` or uid 0 reaches a region's content by privilege alone. | none |

## Attack Scenarios and Defenses

The mechanisms below carry the requirements of [Security Requirements](#security-requirements).

- These are the only ways to reach a region.
  The caller is the owner, the default permits it, or a row names the caller. (SR1, SR12)
- The owner is the process whose node, tgid, start time, and exe inode all match.
  pid reuse and exec into a different binary both lose ownership. (SR2, SR3)
- Delegation rows come in two shapes that never meet.
  A process row, written by the daemon's allow, names a process.
  An account row, written by ioctl, names a uid and gid. (SR1)
- A row is read only on the node that wrote it. (SR2)
- A process row also carries the caller's execve generation.
  An exec raises that count, so the row stops naming the image now running. (SR4)
- Every data path requires `O_CLOEXEC` on the descriptor.
  A descriptor carried across exec is unusable on its own. (SR5)
- GRANT passes on only the bits the caller holds, and takes none back.
  Passing ADMIN or GRANT, or any bit outside the caller's own set, requires ADMIN.
  Rewriting an account's row with fewer bits than it holds requires ADMIN too, since that rewrite is a revocation. (SR6)
- The daemon's channel is bound to its first opener, and each response is bound to its request by sequence number. (SR7)
- The kernel narrows the daemon's answer.
  Unknown bits outside the requested set never land in a row. (SR6)
- Every wait on the daemon is finite.
  A missing or late daemon returns an error, and there is no switch that runs a mount without a daemon.
  The wait for this node's own metadata mutex has no timeout.
  Only the holder's release ends it. (SR8)
- create is refused until identity is settled. (SR8)
- The lock region has a reserved name, and mount names the account it opens to.
  A write from any other account is refused without asking the daemon. (SR9)
- POSIX mode bits are not the answer to a permission question.
  The rows and the daemon decide. (SR11)
- Rows of dead processes and rows of dead nodes are reclaimed. (SR10)

Most scenarios below stop at one branch of the decision flow.
The branch where a scenario stops is that row's "blocking mechanism".

![The order a check runs in when the kernel decides an access: FD_CLOEXEC on the descriptor, ownership by node, tgid, start time and exe inode, the default permission, a row of the caller's own node, and last the daemon's answer. Blue is a check the kernel makes, red is a refusal and the errno it returns, and green is the access going through.](figs/access_decision.png)

### Unprivileged process

| ID | Attack | Verdict | Blocking mechanism | Requirement | Test |
|---|---|---|---|---|---|
| A1 | read or mmap another's region without a row | 🟢 Blocked | When the rows refuse, the kernel asks the daemon.<br>The daemon's deny returns EACCES.<br>The library asks the same question with `FS_IOC_PERM_ASK` before it maps, and a deny there is the same EACCES. | SR1 | `test_postexec_attack`<br>`test_pid_reuse` |
| A2 | create under another's mount point | 🔴 Open (design) | POSIX mode bits are not checked.<br>Attest is what gates create. | SR11 | none |
| A3 | Reacquire a dead process's pid and use its row | 🟢 Blocked | The row also compares start time. | SR3 | `test_pid_reuse` |
| A4 | exec into another binary, reopen the region, use the row | 🟢 Blocked | The row also compares exe inode. | SR3 | `test_postexec_attack` |
| A5 | Call a data path with a descriptor carried across exec | 🟢 Blocked | Every read, mmap, and ioctl requires `FD_CLOEXEC`. | SR5 | `test_cloexec_required` |
| A6 | fork child uses the parent's descriptor as owner | 🟢 Blocked | Ownership compares tgid.<br>The child is therefore a different principal. | SR3 | `test_fork_inherit` |
| A7 | fork child reads the parent's mapping as is | 🟢 Blocked | The mapping is not copied at fork.<br>A parent can undo that with `MADV_DOFORK` before it forks, and a parent that does so hands its child bytes it could equally copy out, so the child gains nothing the parent did not already hold. | SR5 | `test_vm_protect` |
| A8 | Widen a READ mapping to WRITE with mprotect | 🟢 Blocked | Each mprotect re-checks the rows as they stand at that moment. | SR6 | `test_vm_protect`<br>`test_vm_protect_peer` |
| A9 | Grow a mapping with mremap | 🟢 Blocked | The mapping does not grow. | SR6 | `test_vm_protect` |
| A10 | Pass on ADMIN or GRANT with GRANT alone | 🟢 Blocked | Neither bit can be passed on without ADMIN. | SR6 | `test_perm_peer` |
| A11 | Widen one's own row while holding only a READ row | 🟢 Blocked | Without GRANT or ADMIN the grant itself is refused. | SR6 | `test_perm_peer` |
| A12 | revoke another's account row | 🟢 Blocked | Deleting a row that does not name the caller requires ADMIN. | SR6 | `test_perm_revoke` |
| A13 | A member of a group row revokes that row | 🟢 Blocked | Only a uid narrows a row to the caller.<br>Taking off a row that names a group therefore needs ADMIN. | SR6 | `test_perm_revoke`<br>`test_security_group_revoke` |
| A15 | Enumerate delegation rows via sysfs | 🟢 Blocked | Only root reads that file. | SR1 | `test_sysfs` |
| A16 | Read the daemon config to pick targets | 🟢 Blocked | The config, the identity rules and the policy open to root and the daemon's own account alone.<br>Each file is owned by that account at mode 0400, with root as its group.<br>No group bit, no world bit and no ACL entry opens it further. | SR1 | `test_config_mode` |
| A17 | Delay the daemon's response to timeout to open a deny window | 🟢 Blocked | A timeout latches the channel only when the daemon answered nobody else meanwhile.<br>Delaying one caller while serving the rest therefore costs that caller its wait and latches nothing.<br>Every request for one wait after it is refused before it is sent.<br>Claiming the mark is the admission.<br>The wait behind it therefore lets one caller through and no more. | SR8 | `test_security_probe_gate`<br>`test_security_helper_latch` |
| A18 | Make a daemon with the sha256 selector re-evaluate a large binary repeatedly | 🟢 Blocked | A digest is kept against the exe's device, inode, mtime, ctime and length.<br>It is kept only once the clock has left the second the ctime names, since a change inside that second could stamp the same ctime again.<br>An exe past the size limit is refused rather than read. | SR8 | `daemon-selector-probe` |
| A19 | unlink a region whose owner died, without DELETE | 🟡 Partial (design) | Allowed on the owner's node and refused on every other node.<br>Allowed only while no delegation row stands on the region.<br>A region with a row left on it answers EACCES here as anywhere. | SR10 | `test_dead_owner_unlink` |
| A20 | Use the row earned before a re-exec of the same binary | 🟢 Blocked | The row carries the execve generation.<br>The row therefore no longer names the running image.<br>The daemon is asked again. | SR4 | `test_exec_generation` |
| A21 | Keep the bits an earlier image was granted by having the new one re-authorized | 🟢 Blocked | A grant of a different generation replaces the row instead of widening it. | SR4 | `test_exec_generation` |
| A22 | Create a region from a thread other than the group leader | 🟢 Blocked | An owner record stores the thread group leader's start time beside its tgid.<br>Both the owner check and the sweep read the leader's start time.<br>A region a worker thread created is therefore judged against the task its pid names. | SR10, SR3 | `test_gc_thread_owner` |
| A23 | Rewrite another account's row with a narrower mask while holding GRANT | 🟢 Blocked | A grant for an account that already holds a row rewrites that row.<br>A rewrite that drops a bit the row holds needs ADMIN, since GRANT hands on and takes nothing back. | SR6 | `test_perm_peer` |
| A24 | Change the default or revoke a row after the ADMIN it rests on was revoked, by having the request wait for the turn | 🟢 Blocked | The ADMIN check runs before the turn and again under it.<br>A row revoked while the caller waited fails the second check. | SR6 | none |

### Identity written on a different node

Every row and every owner record carries the node id of the node that wrote it, <br>
and a lookup compares that id with the caller's node.<br>
uid and pid spaces are per node, so the same numbers name different principals on two nodes.

| ID | Attack | Verdict | Blocking mechanism | Requirement | Test |
|---|---|---|---|---|---|
| B1 | Use a row written on a different node that carries the caller's uid or pid | 🟢 Blocked | A row is read only on the node that wrote it. | SR2 | `test_perm_peer` |
| B2 | revoke a row written on a different node while holding ADMIN | 🟢 Blocked | A revoke also matches only rows carrying the caller's node id. | SR2 | `test_perm_revoke` |
| B3 | Keep accessing through an existing mapping after default narrowing or revoke | 🔴 Open (design) | A permission change does not tear down a standing mapping. | SR4 | `test_perm_revoke` (indirect) |
| B4 | Change a thread's own ids and let the rest of the identity describe the thread group | 🟢 Blocked | Every field an upcall carries about its caller describes the thread group.<br>A thread that changed only its own ids is therefore judged as the process it belongs to. | SR2 | `test_thread_account` |

### Host with raw write access to the device

| ID | Attack | Verdict | Note |
|---|---|---|---|
| C1 | Alter default permission | 🔴 Open (design) | Shared metadata carries no signature. |
| C2 | Forge a row naming the victim node | 🔴 Open (design) | Same. |
| C3 | Fence the victim node by altering the bootstrap token | 🔴 Open (design) | Same. |
| C4 | Point a region offset into the metadata area | 🔴 Open (design) | Same. |

Every host that maps the device is trusted.<br>
Which hosts map it is the fabric's boundary,<br>
and this filesystem does not guard that boundary.

### The daemon's channel

| ID | Attack | Verdict | Blocking mechanism | Requirement | Test |
|---|---|---|---|---|---|
| D1 | Open the channel from another account and answer in the daemon's place | 🟢 Blocked | The channel is 0600 and owned by the daemon account.<br>The channel admits a single reader. | SR7 | `test_channel_device` |
| D2 | Obtain the daemon's channel descriptor through fork or exec | 🟢 Blocked | The channel is bound to the first opener's tgid, start time, exec id, and euid.<br>The descriptor is `O_CLOEXEC`. | SR5 | `daemon-channel-probe` |
| D3 | Replay a response or alter a frame | 🟢 Blocked | A response attaches only to a pending request whose seq and type match.<br>Accepting one takes the slot off the lists under the queue lock.<br>A second answer therefore finds nothing to attach to. | SR7 | `test_security_double_answer` |
| D4 | The daemon itself is compromised and answers falsely | 🔴 Open (outside trust) | The daemon's judgment is not authenticated.<br>The kernel strips only bits outside the request. | SR7 | none |
| D5 | Kill the daemon to induce fail-open | 🟢 Blocked | Without a daemon, create, metadata writes, and an access no row covers all return EAGAIN.<br>An access a row already covers goes through, since that decision was made while the daemon served.<br>A build with test knobs carries a root-only sysfs stub that stands in for the daemon on a named node, answering attest with a fixed identity and granting the turn, for the lifecycle cases; a production build compiles it out. | SR8 | `test_security_daemon_down`<br>`test_security_helper_latch` (indirect) |
| D6 | Use the mount first during the bring-up window | 🟢 Blocked | Every mount writes the daemon's account row into the lock region inside mount(2).<br>The mount narrows that region's default in the same call.<br>A mount whose own format creates the region does that first.<br>It writes the row and narrows the default in the same call.<br>A refused row fails the mount. | SR8 | `test_lock_region_guard` |
| D7 | Hold the daemon's account and open the channel before the daemon does | 🔴 Open | The kernel binds the channel to whoever opens it first.<br>The kernel reads no more of that process than its account.<br>Appraising the opener's executable is what closes it. | SR7 | none |
| D8 | Connect a helper that declares few capabilities and let every other kind of request time out on it | 🟢 Blocked | Admission reads the kind against what HELLO declared.<br>An undeclared kind is refused at once.<br>Nothing of that kind is queued. | SR7 | `test_security_probe_gate` |

The binding between the channel's requests and responses is shown below.<br>
D1 is stopped at the first arrow, D2 at the second, and D3 stops or leaks at the fourth.

![The channel between the kernel and the daemon: the daemon's open binds the channel to that opener, a response attaches only to the pending request whose sequence number and type match, and the impersonator's open and its resent response are both refused. Yellow is what the kernel enforces on the normal path, and red is the impersonator's attempt together with the rule that refuses it.](figs/channel_binding.png)

### The daemon's identity decision

| ID | Attack | Verdict | Blocking mechanism | Requirement | Test |
|---|---|---|---|---|---|
| E1 | Overlay another binary on a trusted path via mount namespace and exec | 🟢 Blocked | The kernel renders the exe path in the caller's own namespace.<br>So the name a request carries is the caller's to choose.<br>The selector resolves that name in the daemon's namespace instead.<br>A name whose inode is not the one the kernel read derives no identity. | SR4 | `daemon-security-identity-probe` |
| E2 | Attach another process's supplementary groups to one's own identity | 🟢 Blocked | The group read compares two start times for that pid.<br>One comes from the upcall and the other from `/proc`.<br>The read refuses when the two differ.<br>The kernel reads that start time from the thread group leader.<br>The leader is the task the records name. | SR2 | `daemon-security-identity-probe`<br>`test_thread_identity` |
| E3 | Write a key twice in the SPIFFE id to pick the admin value | 🟢 Blocked | A duplicate key empties the identity.<br>An empty label is refused before the response is built, so the kernel never sees it. | SR6 | `daemon-security-identity-probe`<br>`daemon-serve-probe` |
| E4 | Put a newline in an audit field to forge an ALLOW line | 🟢 Blocked | Control characters in external fields are replaced with spaces. | SR7 | `daemon-security-audit-probe` |
| E5 | A fake server grabs the OPA port on loopback first | 🟢 Blocked | The decision travels over a unix socket under /run.<br>That directory is root's.<br>No account but root can put an endpoint at that name.<br>The socket admits the daemon's group alone. | SR7 | `daemon-opa-transport-probe`<br>`test_opa_unix_socket` |
| E6 | The development SPIRE socket is reachable by every account | 🟢 Blocked | The socket goes to the one account the smoke runs as, at 0600. | SR7 | `test_smoke_spire_socket` |
| E7 | Dependency downloads are not verified | 🟢 Blocked | Each download is checked against the digest its project publishes beside it.<br>A mismatch or a missing digest ends the install. | SR7 | `test_install_deps_sha256` |
| E8 | Resolve an identity label longer than the wire field, so the kernel stores another tenant's prefix as the owner | 🟢 Blocked | A label that would not fit the field is refused whole rather than cut. | SR2 | `daemon-serve-probe` |
| E9 | Hand the decision reader a result with a duplicated key, a broken number or a malformed expiry | 🟢 Blocked | A duplicated key refuses the document.<br>A number is read whole or not at all.<br>An expiry that is not a whole non-negative number an int64 holds is a denial. | SR7 | `daemon-policy-probe` |

### Lock region

| ID | Attack | Verdict | Blocking mechanism | Requirement | Test |
|---|---|---|---|---|---|
| F1 | create, unlink, or rename under the reserved name | 🟢 Blocked | A lookup always resolves that name to the lock inode.<br>Both unlink and rename return EPERM. | SR9 | `test_lock_region_guard` |
| F2 | PERM_GRANT or PERM_REVOKE on the lock region | 🟢 Blocked | The lock region's slot is fixed per node.<br>Both ioctls therefore return EPERM. | SR9 | `test_lock_region_guard` |
| F3 | Rewrite the lock region's owner row | 🟢 Blocked | The row is written from mount options inside mount(2).<br>No ioctl reaches the row. | SR9 | `test_lock_region_guard` |
| F4 | Write-map the lock region from a non-daemon account | 🟢 Blocked | When the rows refuse, the kernel returns EACCES without asking the daemon.<br>The same holds for mprotect. | SR9 | `test_lock_region_guard` |
| F5 | Shell metacharacters in the mount helper's command string | 🟢 Blocked | The helper forks and execs an argv.<br>A character in a path is therefore a character.<br>No shell reads the path. | SR9 | `test-mount-helper-argv` |

### Node death

| ID | Attack | Verdict | Blocking mechanism | Requirement | Test |
|---|---|---|---|---|---|
| G1 | Leave a dead node's rows behind to obstruct region reclaim | 🟢 Blocked | The admin node deletes the dead node's rows and reclaims the slot. | SR10 | `bootstrap_chaos.sh` T9 |
| G2 | Reclaim a region whose owner died, or extend its life with a new row | 🟢 Blocked | While live rows remain, the region stays OWNER_DEAD.<br>New rows are refused. | SR10 | `bootstrap_chaos.sh` T9, T10 |
| G3 | A node whose tick stopped keeps accessing or returns with its old identity | 🟡 Partial | The returning node sees the token mismatch.<br>The node fences itself and tears down its mappings.<br>Access during the stall is not stopped by hardware. | SR10 | `bootstrap_chaos.sh` T5, T7 |

The states a region passes through before reclaim, after its owner or node dies, are shown below.<br>
G1 and G2 are attempts to block one step of this transition.

![The three states a region moves between, with the event that moves it drawn as a box of its own: create takes Freed to Live, the owner's tgid and start time disappearing takes Live to OwnerDead, and unlink or the last live row leaving takes either back to Freed. The admin sweep deletes a dead node's rows without leaving OwnerDead. Blue is a state, grey is the event that moves it, and red is the return to Live that refusing new rows closes.](figs/region_lifecycle.png)

### Row lifetime and resource limits

| ID | Attack | Verdict | Blocking mechanism | Requirement | Test |
|---|---|---|---|---|---|
| H1 | Re-exec the same binary with different argv or a script | 🔴 Open (design) | The principal is identified down to the exe inode and no further. | SR3 | none |
| H2 | Grant oneself WRITE while holding only GRANT and READ | 🟢 Blocked | A grant refuses bits outside the caller's set with EPERM. | SR6 | `test_perm_peer` |
| H3 | Reuse a daemon-written process row after the owner's revoke | 🟢 Blocked | A process row carries the account it was written for.<br>That account can therefore name the row.<br>One revoke takes that account's own row and its processes' rows together. | SR4 | `test_security_deleg_revoke` |
| H4 | Access through a row after expiry or a policy change | 🔴 Open (design) | Rows have no expiry.<br>A policy change does not touch existing rows. | SR4 | none |
| H5 | Call through the bit-less one of two descriptors on the same file | 🟢 Blocked | A file operation is handed no descriptor number.<br>The check therefore asks about every descriptor the caller holds on the file.<br>The check refuses when any of them lacks the bit.<br>The answer no longer depends on which descriptor sits at the lower slot. | SR5 | `test_security_cloexec_alias`<br>`test_cloexec_required` |
| H6 | Exhaust the delegation table and the request queue | 🟢 Blocked | Both are bounded.<br>The table answers ENOSPC past its last row.<br>The queue answers EAGAIN past its last slot.<br>Neither opens access and neither leaves a caller waiting. | SR8 | `test_perm_grants`<br>`test_rat_exhaustion` |
| H7 | Hold a process row through a group the revoke names only as a supplementary one | 🟢 Blocked | A row holds the one gid its holder ran under.<br>A revoke naming a gid reads the groups that holder carries now.<br>It takes the rows those groups cover.<br>A holder that has left the group since is reached by naming its uid.<br>A holder that has exited and not yet been swept is reached the same way. | SR5 | `test_security_gid_revoke` |
| H8 | Read a dead owner's bytes through a region placed over its freed extent | 🟢 Blocked | The sweep zeroes the extent before it publishes the slot free, when the owner is gone.<br>A live owner clears its own bytes before it unlinks, which the design leaves to it. | SR1 | `test_gc_zero_extent` |

## What Stands Open

A scenario that is not blocked stays open for one of these reasons, and each has its own table below.

- **Outside the boundary.** The answer lies below this software.
- **Chosen, and the cost is stated.** The design took this trade on purpose.
- **Work.** The work is not done.

A reader looking for what to do next reads Work.<br>
It is the only one of them that names work.

### Outside the boundary

The design trusts every host kernel that maps the device and the privileged administration on it.<br>
A scenario that needs one of those to be hostile is not answered here,<br>
and cannot be answered by anything running above the fabric.<br>
A scenario that needs one of them to have stopped running is the same,<br>
because the mechanism that would stop it runs on the node it is aimed at.

| ID | Scenario |
|---|---|
| C1 | Alter a region's default permission by writing the shared metadata |
| C2 | Forge a delegation row naming the victim node |
| C3 | Fence the victim node by altering the bootstrap token |
| C4 | Point a region's offset into the metadata area |
| D4 | The daemon itself is compromised and answers falsely |
| G3 | A node whose tick stopped keeps reading through the mappings it already holds |

Closing any of these means a boundary below this software:<br>
the Fabric Manager deciding which host maps the device,<br>
and an attestation the kernel can check on what the daemon says.<br>
G3 is there because a fence is raised by the node it fences.<br>
No node can clear another node's page tables, so no peer can reach a stalled one.

### Chosen, and the cost is stated

These are answers the design gives on purpose.<br>
Each is a place where the alternative costs more than the exposure,<br>
and a reader who disagrees with the trade is reading the right row.

| ID | What is open | Why it is the answer |
|---|---|---|
| A2 | A caller creates a region under a mount point whose POSIX mode bits say it may not | The rows and the daemon decide, and reading the mode bits as well would give two answers to one question |
| A19 | The owner's node unlinks a region whose owner died and carries no row, without holding DELETE | The node that watched the owner die is the one that knows, and requiring a grant there would leave the region for nobody |
| E1 | A path rule is only as strong as the path it names, and nothing checks who may write that path | The daemon holds the name to the inode the kernel read, and which paths an account may write is the host's to set, not this filesystem's |
| B3 | A consumer keeps reading through a mapping that stood before the permission changed | The permission question is asked once when the mapping is built, and asking it per fault would put the daemon in the data path |
| H1 | A principal re-execs the same binary with different arguments | The principal is the exe inode, and arguments are not something the kernel can hold a record against |
| H4 | A row outlives the policy that produced it | The daemon sends an expiry and the kernel does not read it yet, so a row ends with its process rather than with a clock |
| H7 | A revoke naming a group reads the holder's membership at revoke time, so a process that left the group keeps its row | Recording the membership a grant was made under would put a list where one cacheline holds one row, and naming the uid is exact |

### Work

What is left is neither a boundary nor a decision.<br>
Each row below is a mechanism that does less than its scenario asks,<br>
and each could be closed without changing what the design is.

| ID | What falls short | What closing it needs |
|---|---|---|
| D7 | The kernel binds the channel to whoever opens it first and reads no more of that process than its account | Something the kernel can check about the opener beyond its uid |
