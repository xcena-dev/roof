# Design

| Section | What it covers |
|---|---|
| [1. Overview](#1-overview) | The programs on a node, what each owns on the device, and the paths between them |
| [2. Environment](#2-environment) | The workloads that share a CXL pool and why a region must carry its own permission |
| [3. Requirements](#3-requirements) | DR1 to DR8: identity, authority, enforcement and lifetime the design must provide |
| [4. Assumptions](#4-assumptions) | The platform properties the checks rest on, and the trust boundary |
| [5. Principles](#5-principles) | DP1 to DP6: the rules every mechanism below follows |
| [6. Records on the Medium](#6-records-on-the-medium) | The superblock, bootstrap slots, name index, region table and extents, who writes each, and how each area is cached |
| [7. Mount and Unmount](#7-mount-and-unmount) | Claiming a node id from a bootstrap slot, the lock-region row, and handing both back |
| [8. Region Lifecycle](#8-region-lifecycle) | Create, place, open, unlink, grant and access, and freeing |
| [9. The Daemon](#9-the-daemon) | The per-mount helper: the upcalls it answers, and how kernel and daemon trust each other |
| [10. The Turn](#10-the-turn) | Why multi-node writes need a turn, where it comes from, and which paths take it |
| [11. GC and Recovery](#11-gc-and-recovery) | The sweep, the heartbeat, node death and fencing, and what each event stops |
| [12. Limits and Future Work](#12-limits-and-future-work) | What the design leaves alone on purpose, and what a later release has to add |

## 1. Overview

![Architecture overview: an application, the helper daemon and the filesystem kernel module on one node, over one CXL FAM device that every node maps. The colour of a box says who owns its bytes: the kernel module holds the metadata area, the daemon's CME peer holds the lock region, and the application's mappings hold the region extents.](figs/architecture.png)

Every node that mounts the device runs the same programs, and the device holds everything they share.

| Program | Runs as | Owns on the device | Detail |
|---|---|---|:---:|
| application | one process, a producer or a consumer | the region extents it maps | [8. Region Lifecycle](#8-region-lifecycle) |
| filesystem kernel module | one per node | the metadata area: bootstrap slots, name index, region table, delegation rows | [6. Records on the Medium](#6-records-on-the-medium) |
| helper daemon | one per mount, beside the module | the lock region, as the node's `libcme` peer | [9. The Daemon](#9-the-daemon),<br>[The Turn](#10-the-turn) |

The application reaches the module through the mount's syscalls,<br>
and reaches its bytes through a mapping the fault fills one page at a time.<br>
The module reads and writes the metadata area itself,<br>
and asks the daemon through one upcall channel for an identity, a permission, or the node's turn.<br>
The daemon takes that turn in the lock region, and no coordinator stands between the nodes.

## 2. Environment

A workload on one host writes data into a CXL pool.<br>
Workloads on other hosts read that data by mapping the same memory, without copying it over a network.<br>
Some of the data must reach only some of those workloads.<br>
The fabric cannot enforce that, because it grants or denies a whole host access to the pool,<br>
and the workloads that must be kept apart often run on the same host.<br>
So each region of the pool has to carry its own permission,<br>
and that permission cannot be a property of the pool, the host or the user account.

An inference deployment is one case of this.<br>
One worker computes the KV cache for a prompt prefix and writes it into the pool.<br>
Other workers read that cache and continue the generation from it.<br>
Some of that cache is shared by every worker, some belongs to one project, and some to one user's conversation.<br>
One person can also run several workloads at once.<br>
A planner workload reads the confidential business purpose.<br>
The flight and hotel workloads read only the destination, the dates and the budget.<br>
All three run for the same person, on the same host, under the same user account.

![A producer on one host writes three regions into the CXL pool: a prefix cache for every worker, a cache for one project, and a cache for one conversation. Three processes on another host run under one user account, and each region admits only some of them. Brown is the producer writing, green is a mapping the region admits, and dashed red is one it refuses.](figs/environment.png)

## 3. Requirements

These are what that environment demands.<br>
They state what the design must provide.<br>
[`security_report.md`](security_report.md) states what the design must prevent.

| ID | About | Requirement | What it rules out |
|---|:---:|---|---|
| ***DR1*** | Identity | A consumer's identity means the same thing on every host | A local account number, or a name the requester supplies for itself, decides access |
| ***DR2*** | Identity | An identity names one process | Two threads of one process are told apart, or a whole account is treated as one consumer |
| ***DR3*** | Authority | A region names an owner, and only the owner changes who may reach it | A consumer that can read a region can also grant it to others |
| ***DR4*** | Authority | Permissions are per operation, so read can be granted without write | A consumer that may read a region can also overwrite it |
| ***DR5*** | Enforcement | The enforcement point is where the mapping is made | A process maps the device under the region label and reads past its range and rights |
| ***DR6*** | Lifetime | A permission ends when the process that earned it ends | A finished task or an exited worker leaves its authorisation standing for the next holder of its pid |
| ***DR7*** | Lifetime | Revocation says which access it stops | "Revoked" is claimed without stating whether new mappings are refused,<br>existing reads are stopped, or neither |
| ***DR8*** | Lifetime | An extent a dead owner left is cleared before it is placed again<br>(a live owner clears its own bytes before it unlinks) | A reused allocation hands a dead owner's contents to the next one |

***DR2*** rules out both neighbouring units.<br>
An account is too coarse, since the three workloads above share one.<br>
A thread is too fine, since threads of one process read each other's memory directly.

***DR7*** is met by stating that no path revokes a permission yet.<br>
[Grant and Access](#83-grant-and-access) lists the events that end a row,<br>
and [What Each Event Stops](#114-what-each-event-stops) states what each of them stops.

***DR8*** stops at the dead owner on purpose.<br>
A live owner knows what its region holds and clears it before it unlinks,<br>
so the design zeroes only what nobody is left to clear.<br>
[Freeing a Region](#84-freeing-a-region) works through how an extent is kept while it is held and cleared when it is not.

## 4. Assumptions

The checks in this design rest on properties of the platform under it.

| Assumption | What rests on it | Without it |
|---|---|---|
| A store that writes one cacheline as one transaction | Every metadata record is published as one cacheline.<br>`MOVDIR64B` is defined that way,<br>and a 512-bit vector store was measured to behave so ([measured](experiments/store_split_turin.md)) | A reader sees a torn record: part of the new line beside part of the old one |
| x86-64 | `MOVDIR64B`, AVX-512 and the cache-type calls are x86 | Another architecture needs its own line store, line load and<br>cache-type control before the module builds there |
| Each node's clock advances | A live node's heartbeat stamp changes on every tick,<br>and a stamp too far from the reader's clock is judged by that change | A node whose clock stands still is taken for dead while it is healthy |
| A dax device | The mount maps the device's physical range itself<br>and picks each area's memory type | An fsdax or block device needs its range read from elsewhere,<br>and its driver's write-back mapping refuses the uncached one |
| Every host kernel that maps the device is trusted | Nothing reads another host's writes to shared metadata to check them | A kernel or a privileged process with raw write access can forge any row,<br>and only the Fabric Manager keeps such a host off the device |

The last row is the trust boundary.<br>
Every host kernel that maps the device is trusted, and so is privileged administration on it.<br>
A host with raw write access to the pool can forge any record this design relies on.<br>
Keeping such a host off the device belongs to the fabric, below this software.<br>
What that trust covers and what it leaves open is in [`security_report.md`](security_report.md).

## 5. Principles

The rules every section below follows.

| ID | Principle | Why | Detail |
|---|---|---|:---:|
| ***DP1*** | Every decision reads the medium | Records and data share one span that every node maps,<br>so no node decides from a private copy (***DR1***) | [6. Records on the Medium](#6-records-on-the-medium) |
| ***DP2*** | A record taken whole is one cacheline | One store publishes it, so no reader sees half of it.<br>A RAT entry is judged line by line | [6. Records on the Medium](#6-records-on-the-medium) |
| ***DP3*** | One line has one writer at a time | A node's own lines are written by that node,<br>a shared line by the node holding the turn,<br>so no two stores to one line race | [10. The Turn](#10-the-turn) |
| ***DP4*** | A write that spans lines takes the turn,<br>a read takes nothing | The lock region serialises multi-line changes across nodes,<br>and a reader judges each line on its own | [10. The Turn](#10-the-turn) |
| ***DP5*** | The kernel enforces at the mapping | The daemon decides, and the kernel of the mapping host carries it out (***DR5***) | [9. The Daemon](#9-the-daemon) |
| ***DP6*** | Nobody commands another node | Liveness is a heartbeat each node stamps and reclaim a sweep each node runs.<br>A dead node is recovered by the admin and fences itself when it returns | [11. GC and Recovery](#11-gc-and-recovery) |

## 6. Records on the Medium

Every requirement above is carried by a record, so where those records live is the first decision.

![Two rows of boxes. The upper row is the span from the superblock through the bootstrap slots, the shard table, the bucket lines, the index line pool, the RAT, the reference bits and the pad to the lock region and the region area, with the uncached mapping bracketed under everything before the region area and the write-back mapping under the region area, split at a boundary the superblock states. Each area is coloured by its purpose. The lower row is one RAT entry: the hot, name and acl lines darker, then one delegation row per line lighter, in the RAT's colour.](figs/medium_layout.png)

| Purpose | Record | Holds | Writer | Read by |
|:---:|---|---|---|---|
| layout of<br>the medium | Superblock | magic, version, where each area lies,<br>granule and pool split, checksum | the formatting node, once | the mapper before either mapping exists, then every mount |
| node state | Bootstrap | state, token, heartbeat, recoverer | the slot's own node, or the admin recovering it | every mounting node, and every admin sweep |
| names | Shard header | magic, bucket and pool geometry | the formatting node, once | every mount, then cached in DRAM |
|  | Index link | name tag, region id, next link | the node holding the turn (`create`, `unlink`) | every name lookup |
| region state | RAT header | magic, version, where the regions start | the formatting node, once | every mount |
|  | RAT summary | taken bitmap, placement count | the node holding the turn (placement, free) | the allocator, and the extent survey |
|  | RAT entry hot | state, type, extent offset and size,<br>timestamps, mode | the node holding the turn (`create`, placement, free) | `open`, `read` and `stat` |
|  | RAT entry name | the name | the node holding the turn (`create`, `unlink`, free) | a lookup whose tag matched |
|  | RAT entry acl | owner identity, default permission | the node holding the turn (create, free,<br>the owner's default-permission change) | every permission check |
|  | Delegation row | grantee identity, permissions,<br>when granted | the node holding the turn (grant) | every permission check, and GC |
|  | Reference bits | one bit per region | the owning node alone (`open`, last `close`) | `unlink`, the last `close`, and the sweep that frees a region |
| turn-taking | Lock region | the daemons' turn queues,<br>one region with a RAT entry of its own | the daemons, through their own mappings | the daemons |
| application | Region extent | the application's bytes | the application, through its own mapping | the application |

### 6.1 Cache Policy

The metadata area and the front of the region area are mapped uncached, the rest of the region area write-back.<br>
An application picks a region's pool with `FS_IOC_CACHE_SET` before the region is placed, and the choice is fixed once it is.<br>
The mount checks the cache type it received and refuses a cached metadata area,<br>
because `memtype_reserve` hands back write-back rather than failing when another owner already holds the range.

| Mapping | Gains | Gives up | Fits |
|---|---|---|---|
| uncached<br>(metadata area, front of the region area) | A load reaches the device, so a peer's store is visible with nothing to flush.<br>A store leaves as one whole-line write | Cache locality: a repeated read pays fabric latency again.<br>`struct page` backing: nothing here can be pinned for DMA | Records, and a region peers exchange with single stores: a flag, a queue, a lock |
| write-back<br>(rest of the region area) | This node's cache on a repeated read.<br>ZONE_DEVICE pages a GPU DMA registration can pin | Cross-host visibility inside the filesystem: on CXL 2.0 the producer writes its lines back and the consumer drops its copies, as with any shared memory | Bulk bytes read many times: a KV cache, a model shard |

## 7. Mount and Unmount

A mount claims a node id and a place in the lock region before any region procedure runs, and an unmount hands both back.<br>
The clean step an unmount runs on its own rows is the recovery step of [GC and Recovery](#11-gc-and-recovery).

![Mount as a sequence. The mount helper calls mount(2). The kernel reads the superblock, maps the metadata uncached and the regions write-back, and stores a token and a heartbeat in a FREE bootstrap slot. After the settle window it rereads the slot, and when its token kept, the slot index is the node id. It stores the helper's row in the lock region, starts the GC thread and returns the node id. The helper formats the lock region if nobody has and starts the daemon. The daemon joins the lock region and sends HELLO on the channel, and the helper returns. A failure after mount(2) unmounts, so the caller sees a finished mount or none.](figs/mount.png)

![Unmount as a sequence. The umount helper writes unmount_prepare, and the kernel refuses new opens and answers -EBUSY while a file is open. The kernel then takes this node's turn through the daemon one entry at a time, empties this node's rows, zeroes its reference bits, frees what nobody holds, gives the turn back, and keeps the mount closed. The helper stops the daemon and calls umount(2). kill_sb cleans again, stops the GC thread, drops the channel and releases the slot as FREE, except that an entry still locked keeps the slot for the admin. A fenced or staked mount skips the clean step, which runs while the daemon is up because it needs the turn.](figs/unmount.png)

The node cleans up after itself because nothing else will.<br>
The admin recovers only a slot whose heartbeat stopped, and a released slot is skipped,<br>
so a row left behind would pass to whichever host claims the id next.<br>
The clean step treats the node as dead, so it runs only once no `open` or `open(O_CREAT)` on this node can reach a region.<br>
The mount stays closed after the clean step because what it owned is already released.

## 8. Region Lifecycle

A region is created, placed, opened, shared and freed,<br>and each step is a procedure on the records above.

### 8.1 Creation

![Create and place as a sequence. open(O_CREAT) sends ATTEST for the group and role with no lock held, then takes the mutex and the turn. The kernel picks a free index from the taken bitmap, moves the hot line from FREE to ALLOCATING, fills the name and acl lines, and publishes ALLOCATED with no extent yet. It writes the bucket slot, sets the taken bit, gives the turn and the mutex back, and returns the descriptor. ftruncate(size) takes the mutex and the turn again, finds a gap in the pool the cache policy names, writes offset and size into the hot line, bumps the placement count and returns. A reader on any node sees an entry before or after each write, never between.](figs/create.png)

The acl line carries the creator's tgid, start time, executable inode, group and role before the entry publishes `ALLOCATED`, so the owner record stands from that store on.

### 8.2 Open, Close and Unlink

![Open, close and unlink as a sequence. A holder's open sets its node's reference bit. The owner's unlink empties the bucket slot under the turn and reads every node's reference bit. When none is set it frees the entry at once. When one is set it leaves the entry nameless. The holder's last close on its node clears the bit, and when that was the last bit, its kernel frees the entry under a turn.](figs/unlink.png)

Each node keeps its own open count per region and publishes it as one reference bit in a line only that node writes, so no lock orders the nodes.<br>
`unlink` clears the name before it reads the reference bits, and `open` sets its reference bit before it reads the name.<br>
Whichever came second sees the other, so an `open` racing an `unlink` either holds the region or is refused, and never holds an entry freed under it.

### 8.3 Grant and Access

![Grant and access as a sequence. FS_IOC_PERM_GRANT takes the mutex and the turn and reads the acl line and rows, where GRANT hands on what the caller has and ADMIN hands on anything. It writes one account row with a uid or gid and permissions. FS_IOC_PERM_ASK finds nothing covering the caller and sends ACCESS with both identities before mmap, so the wait holds no mmap_lock. When the answer allows it, the kernel writes one process row with the tgid, start time, executable and permissions under the turn. The mmap or read that follows finds that row and proceeds with no upcall and no turn. Each row changes with one whole-line store, so a check on any node sees it before or after each event.](figs/grant.png)

| Row | Written by | What it names | Ends when |
|---|---|---|---|
| process row | an `ACCESS_REQUEST` the daemon allowed | one process on one node, by its tgid and start time<br>and by its executable inode and exec generation | the writing node's GC finds that process dead,<br>its node unmounts or is recovered as dead,<br>or the region is freed |
| account row | `FS_IOC_PERM_GRANT` from a caller holding `GRANT` or `ADMIN` | one account on one node, by uid and gid | its node unmounts or is recovered as dead,<br>or the region is freed |
| lock-region row | a mount, at the table slot its own node id names | that node's daemon account | its node unmounts or is recovered as dead |

### 8.4 Freeing a Region

A region is freed once nothing uses it any more.<br>
No node may hold it open or mapped, and once its owner is gone, no delegation row may stand on it either.<br>
`unlink` takes the name away, and the entry and its extent are freed on the first of these events.

| Event | Who frees the region | Only when |
|---|---|---|
| `unlink` | the unlinking node | no node holds the region open or mapped |
| the last `close` after an `unlink` | the node whose last holder let go | no other node holds it |
| the owner process exits | the GC on the owner's node | no delegation row and no open reference stands |
| the owner's node unmounts | that node's clean step | no peer's row or open reference stands |
| the owner's node dies | the admin's recovery | no row or open reference stands |

A row or an open reference that still stands when the owner's node goes turns the entry `OWNER_DEAD` instead.<br>
The owner identity is zeroed, nothing new starts on the entry, and the ordinary sweep frees it once the last row and reference go.<br>
Nobody takes ownership over, because ownership carries the policy context of the process that created the region.

![Freeing a region as a sequence, once the last reference goes or the owner goes with nothing holding the region. The kernel takes the mutex and the turn and drops the name's bucket slot. When the owner is gone it zeroes the whole extent, since nobody is left to clear it. A placed region steps the placement count odd. The hot line goes to DELETING, which no lookup or create accepts. Every delegation row is emptied, the acl line zeroed and the name line emptied. The hot line is published FREE with every field zero, the taken bit is cleared, the placement count goes even, and the turn and the mutex are given back. A live owner's unlink leaves the bytes in the extent, and a create clears the delegation table again before writing.](figs/free.png)

The entry goes to `DELETING` before anything is cleared and to `FREE` only after, so no reader sees a half-cleared entry and no create takes the slot early.<br>
A node's reference-bit line is zeroed by the recovery that drops its rows, and by the node itself when it claims its id.

## 9. The Daemon

![The helper daemon, one process per mount, between the kernel module on the left and what it reaches on the right. The kernel's ATTEST, ACCESS and LOCK or UNLOCK upcalls arrive on the daemon's main loop, which hands a request that would block to a worker pool. The identity provider has a SPIRE and a local backend, the policy engine an OPA and a local backend, and the turn service reaches the lock region through libcme. Outside the daemon, the SPIRE backend reaches the SPIRE agent through the Delegated Identity API, the OPA backend reaches the shared OPA server over HTTP, and the turn service meets the other nodes' turn services through shared memory with no network.](figs/daemon.png)

### 9.1 Why a Daemon

Every answer the kernel asks for comes from a system that lives in user space.<br>
The node's turn comes from [CME](https://github.com/xcena-dev/cme), which arbitrates between processes through `libcme`.<br>
*Who is this process?* (***DR1***) comes from an identity system such as SPIRE.<br>
*May it have what it asks for?* (***DR3***) comes from a policy engine such as OPA.<br>
A kernel module can link none of them, so one daemon per mount links them and answers for the kernel.<br>
The enforcement point stays in the kernel module (***DR5***), and the daemon only supplies the answers.<br>
A host therefore plugs in the identity and policy systems it already runs, and changes them without a module rebuild.

### 9.2 Upcalls

| Upcall | When the kernel sends it | The question |
|---|---|---|
| `ATTEST_REQUEST` | a region is created | what identity does this process have? |
| `ACCESS_REQUEST` | a `read` or `mmap` that no row covers | consumer X asks for a region owned by Y: which permissions? |
| `LOCK_REQUEST` | a metadata write begins | take this node's turn on the shared lock region |
| `UNLOCK_REQUEST` | the write is done | give the turn back (no answer: the kernel moves on once it is queued) |

`ATTEST_REQUEST` and `ACCESS_REQUEST` carry only who asks and who owns, never the region, the operation or the node.<br>
The daemon decides on those identities, and the kernel applies the answer to the access in hand.<br>
While the daemon is down, every create, every metadata write and every access no row covers fails with `-EAGAIN`.<br>
An access a row already covers goes through, because the row is a decision the daemon already made.

![One upcall as a sequence. The kernel queues a request with a sequence number on the channel and sleeps with a deadline. The daemon reads the request and asks its identity or policy backend, or for LOCK takes this node's turn in the lock region, then writes the answer. The channel matches the sequence number and wakes the caller, in any order. ATTEST writes the group and role into the acl line, an allowed ACCESS writes the delegation row, and LOCK lets the write proceed under the turn. UNLOCK is queued with no answer. A passed deadline returns -ETIMEDOUT, and a daemon that answered nobody meanwhile is marked dead. The channel writes nothing to the medium.](figs/upcall.png)

### 9.3 Mutual Trust

The kernel trusts the daemon because of who may hold the channel.<br>
The channel file is mode 0600 and owned by the daemon's account,<br>
and the module binds the channel to its first opener,<br>
which is the task's tgid, exec id, start time and effective uid.<br>
A write from any other task is refused for as long as that channel is held.

The daemon trusts the identity in a request because the kernel fills it in from the calling task, not from anything the task passes.

## 10. The Turn

Every node that mounts the device writes the same region table.<br>
A whole-line store keeps a line from tearing, but two nodes writing it at once would still lose one node's write.<br>
Every metadata write is decided on what the writer just read, so none may interleave with a peer's.<br>
CXL gives the nodes no atomic operation across hosts, so the exclusion comes from CME,<br>
which orders the nodes through ordered stores in the lock region.<br>
The daemon is this node's peer in CME, and takes the lock for the kernel when a LOCK upcall asks.<br>
Threads on one node take the mount's mutex first, so only one of them asks the daemon for the turn at a time.

A path that writes shared metadata takes the turn, and a path that only reads takes none.

| Takes the turn | Takes none |
|---|---|
| creating a region entry | lookup, readdir and stat |
| unlinking a region | a permission check the rows already answer |
| placing a region, which publishes its offset and its size together | `read` and `mmap` while the rows already answer |
| writing a delegation row | the fault on a live mapping |
| setting a region's default permissions | |
| freeing a region, and the GC sweep | |

A writer that cannot get the turn is refused with `-EAGAIN` rather than left waiting.

## 11. GC and Recovery

What a dead process, a leaving node or a dead node left on the medium has to be found and freed.<br>
This section is the sweep that does it and the heartbeat that says when a node is dead.

### 11.1 Reclaim

![One GC cycle as a sequence. Every tick inside the wait between sweeps stamps this node's heartbeat in its bootstrap slot. Every sweep, on every node, reads who the admin is, the lowest ticking node id. For each standing entry it empties, under the turn, a row this node wrote whose process died. An entry this node owns whose owner is dead or whose name is gone, with no row left and no reference bit set on any node, is freed under the turn, with its extent zeroed when the owner is dead. The admin alone then does the same for OWNER_DEAD entries, notes in memory an entry left mid-create or mid-delete with no owner node, and names itself owner of one that stays unchanged past the grace period. It repairs taken bits that disagree with the walk under the turn, and runs one recovery step: stake, clean or evict. A judgement read without the turn is made again under it, and an entry whose turn does not come waits for the next sweep. A tick that finds the slot is not this node's fences the mount and ends the cycle.](figs/gc.png)

A node cannot clear another node's rows during an ordinary sweep,<br>
because another node's process liveness is not a local process table's to judge.<br>
The one path that crosses that line is dead-node recovery,<br>
and it runs only after the admin has staked the dead node's bootstrap slot.

Each node decides whether it is the admin once per sweep, so two nodes can both act as admin for one cycle after a lower id mounts.<br>
That costs a repeated step and not a wrong one,<br>
because each admin rereads an entry under the turn and skips one the other already freed.

An entry a create or a delete abandoned names no owner node, so no node's own sweep reaches it.<br>
Once the admin names itself owner, its ordinary sweep frees the entry on the next cycle.

Two sysfs attributes report this.<br>
`gc_status` prints each mount's thread state and its sweep counter.<br>
`deleg_info` prints one region's rows,<br>
and it is mode 0600 because those rows name other accounts' uids, gids and pids.

### 11.2 Node Death

Each node's bootstrap slot carries a heartbeat stamp.<br>
The node's own GC tick writes its wall clock into that stamp.<br>
The tick runs on a much shorter period inside the sweep's wait,<br>
so a paused sweep does not let peers conclude this node died.<br>
The sweep runs on the same thread, so the stamp stands still while a sweep runs.<br>
A sweep longer than one timeout, from zeroing a large extent or waiting on turns, can make peers judge this node dead.<br>
A reader compares the stamp against the reader's own wall clock,<br>
and it never sleeps to probe.

| The stamp reads | Verdict |
|---|---|
| within one timeout of the reader's clock, either side | alive |
| anything else | alive once the value changes, dead once it stays unchanged for one timeout |

A stamp further off means either that the holder stopped or that the two clocks disagree,<br>
and one reading cannot tell which.<br>
For such a stamp the reader watches whether the value moves and times the wait on its own clock.<br>
It judges the holder alive once the value changes, and dead once the value stays unchanged for one timeout.<br>
Until then it stakes nobody and names no admin.

![Node death as a sequence. The dead node stamps its heartbeat and stops. After one timeout without a new stamp, the admin stakes the slot as RECOVERING. On the next sweep the admin empties the dead node's rows, zeroes its reference bits, marks its held regions OWNER_DEAD, and zeroes the rest and frees them. On the sweep after that it evicts the slot as FREE. If the dead node comes back, its next tick finds the slot is not its own and the mount is fenced. Every file operation returns -EIO, every mapping comes down, and only unmount clears the fence. Rows a live node wrote stay, and the id stays off the market until the slot is evicted.](figs/node_death.png)

Recovery runs one step per sweep, and only on the admin.<br>
The token stays through the stake, which keeps the node id off the market while rows still name it.<br>
A region the dead node owned goes only when no row and no other node's open reference is left on it,<br>
because either is what a consumer stakes to keep a region past its owner.<br>
Rows a live node wrote are left alone, since a grant a live node holds is not this sweep's to withdraw.<br>
Eviction goes last, because a scan skips a free slot and a premature free would leave rows no scan reaches.<br>
The sweep between the stake and the clean gives the staked node a whole interval to read its own slot before the admin deletes what it owns.

### 11.3 Fencing

The fence is latched by the staked node's own tick, and the following hold from then on.

- Every file operation on that mount fails with `-EIO`, because the fence check sits in the context each operation opens.
- The GC thread leaves its sweep loop and takes down every mapping of every inode on the mount, including the private copies a `MAP_PRIVATE` reader made.
- A fault on anything left returns `SIGBUS`, since the fault path opens the same context.

Once a mapping stands, no kernel code sits between a load and the fabric (***DR5***), so clearing its page table entries alone takes nothing down.<br>
The next fault reads the fence state, the placement and the bounds, and no row, so it would put the page back.<br>
The fault does read the fence, which is why the fence is what takes a mapping down, and why its reach is the whole mount.<br>
No path clears one consumer's entries and leaves the rest of the mount running, since that would need a fault that consults the rows.

Fencing is therefore self-inflicted and local.<br>
No node can fence another one, and no node can take another node's mappings down.<br>
What stops a fenced node from writing as a node id that has passed to a peer is that node's own kernel, running this module and reaching its next tick.<br>
A mount that cannot claim a bootstrap slot fails to mount,<br>
and a mount whose GC thread cannot start fails as well,<br>
so no mount runs without the tick that would fence it.

<!-- OPEN: nothing bounds the window between the admin's stake and the staked node's fence beyond that node's own tick period. A node whose kernel is wedged, whose clock is broken, or whose GC thread is stopped outside the mount path keeps its mappings and keeps writing. The fabric is where that would be stopped, and this design does not stop it. -->

### 11.4 What Each Event Stops

| What happened | Stops at once | Stops later | Does not stop |
|---|---|---|---|
| the delegated process exits | the row matches no caller,<br>since a later holder of that pid fails on the start time | the row's slot,<br>emptied by the next sweep of the node that wrote it | the region, its contents, and any bytes that process copied out |
| the owning process exits | nothing | the region entry,<br>once no row and no open reference stands<br>and its own node's sweep reaches it,<br>its extent zeroed first | mappings other processes hold,<br>which keep the entry until they go |
| a node stops answering | nothing on any other node | every row naming it,<br>then the regions it owned with no row left,<br>over the admin's next sweeps | that node's own mappings and writes,<br>until its own tick fences it |
| a node unmounts | every row naming it,<br>and the regions it owned that no peer holds,<br>before its slot goes | the regions it owned that a peer still holds,<br>once that peer lets go, by the admin's sweep | that peer's mappings,<br>which read their own bytes |
| that node's tick fences it | every file operation on the mount,<br>and every standing mapping including private copies | nothing | anything that node already copied out |
| `unlink` | the name, and every new open of it | the extent,<br>once no node holds the region open or mapped,<br>by the node whose last holder let go | descriptors and mappings already standing,<br>which keep the extent and read their own bytes |

Read down the last column.<br>
Every event in this design refuses future decisions and reclaims records.<br>
Only fencing takes a standing mapping down,<br>
and nothing at all reaches a byte a consumer has already copied.

## 12. Limits and Future Work

The sections above state what the design enforces.<br>
This one states what it leaves alone, and what a later release has to add.

### 12.1 Left Alone on Purpose

| Item | What happens |
|---|---|
| POSIX mode bits | They decide nothing.<br>A directory that reads `drwxr-xr-x root` still lets any user create a region in it,<br>because the rows and the daemon answer the permission check, not the bits. |
| What the policy sees | Only the consumer's and the owner's group and role.<br>It does not see the region's name, the operation or the node,<br>so a rule cannot single out one region. |
| Grant expiry | The daemon sends one with every grant, and nothing reads it.<br>A grant lasts until an event in [Grant and Access](#83-grant-and-access) ends it. |

### 12.2 Future Work

| Item | Today | What a future release has to provide |
|---|---|---|
| Revocation | No path revokes a permission.<br>A row ends only on the events in [Grant and Access](#83-grant-and-access). | Say what it stops: the rows, a standing mapping, or both.<br>Reach one process as well as a whole account, and a row another node wrote. |
| Region lifetime | A region nobody holds is freed when its owner's process or mount goes,<br>even if a later reader was meant to find it. | Let an owner keep a region past its own exit and unmount.<br>Say who may end such a region and who reclaims it. |
| Region size | A region is sized once with `ftruncate()`.<br>Its extent is one contiguous range of the pool. | Grow a region by adding extents that later mappings see.<br>Shrink only a tail that no node holds mapped. |
| Root and the kernel | Trusting the host kernel means trusting root as well. | Keep root from replacing the kernel or this module once the host has booted.<br>Have the daemon confirm that it talks to this module and not to a stand-in.<br>(for example, a signed module under Secure Boot lockdown) |
| Daemon responses | A response is trusted for the descriptor it arrived on. | Let the kernel check that a response came from the daemon it trusts,<br>not only that it arrived on the right descriptor.<br>(for example, a response signature with a key sealed in a TPM) |
| Daemon isolation | The daemon runs as an ordinary host process. | Keep the daemon's identity and decisions out of reach of other host processes, root included.<br>Judge what a process runs and how its memory changes, not only who it was when attested.<br>(for example, a daemon in a TEE, with executables appraised at `execve`) |
