# A ROOF over the CXL Pool

*Part 2 of 2: how ROOF combines region ownership, workload grants, and reclamation across hosts.*

[Part 1](medium_draft_part1.md) started and ended with this scene:

> A workload on Host A has already processed a prefix.<br>
> It has stored the resulting KV cache in the CXL pool.<br>
> A workload on Host B is ready to reuse it.
>
> May this workload use it?

As Part 1 showed, the answer depends on the context and the workload.<br>
In Part 1's travel assistant, the Hotel Agent may reuse the booking context.<br>
It may not read the Planner's private context.<br>
Each context needs its own region.<br>
Each workload needs its own permissions.<br>
And the answer must be the same on every host.

A filesystem was the natural choice for those regions,<br>
because filesystems already provide what a region needs:

- **Namespace.** A workload finds a region by name, not by device offset.
- **Space allocation.** The filesystem gives each region its range of the pool.
- **Access control.** The kernel checks access to each region, not just to the whole device.
- **Region lifecycle.** A region lives from its creation to its deletion.<br>
  The region's space stays while any process still has it open or mapped.

There was already a filesystem for the CXL pool:
[famfs](https://github.com/cxl-micron-reskit/famfs).

**So why build another?**

## Famfs: Built for Sharing

*What does famfs already solve?*

Famfs makes the CXL pool accessible through files.<br>
An application maps a file and uses its data in place.<br>
Workloads on several hosts map the same copy of the data.

![On famfs only the master creates files and logs them, and a client replays the log before it maps the same files](figs/part2_famfs_sharing.png)

**Famfs already brings several of those basics to the CXL pool.**

- **Namespace.** Each file has a name,
  so a workload finds data by name.
- **Space allocation.** Famfs gives each file its range of the pool.
- **Access control.** Files carry ordinary POSIX permissions.
- **Region lifecycle, in part.** Files can be created and mapped,
  but not deleted.

We needed those basics.<br>
But we needed answers to more questions.<br>
Famfs answers them with its design:

- **Who creates a region, and who owns it?**<br>
  Only the master host creates files.<br>
  Each file belongs to a uid and gid.
- **When do other hosts see it?** Only after they replay the master's metadata log.
- **Who may use it, on every host?**<br>
  Each host checks access by uid and gid.
- **How long do the region and each permission last?**<br>
  Individual files cannot be deleted to reclaim their space.<br>
  A permission does not end when a process exits.

## Famfs: Not Built for Workload Control

*What do our workloads need that famfs does not give?*

Replay the scene from Part 1 on famfs, with the travel assistant.<br>
The Planner Agent runs on Host A, and the Hotel Agent runs on Host B.<br>
Both run under the same employee's account.

1. **The Planner Agent on Host A stores its KV caches.**<br>
   In famfs, only the master host creates files.<br>
   The Planner Agent must ask the master to create them on its behalf.
2. **The Hotel Agent on Host B reads the booking context in place.**<br>
   Host B sees the new file only after it replays the master's metadata log.
3. **The Hotel Agent tries to read the Planner's private context.**<br>
   Famfs checks only the account both agents share.<br>
   So the Hotel Agent gets in.
4. **The trip is booked, and no workload needs the caches.**<br>
   Famfs cannot delete a single file.<br>
   So the regions' space never returns to the pool.

![On famfs the Planner Agent on Host A must ask the master, Host B waits for the log, the account check admits the Hotel Agent, and the unused caches keep their space](figs/part2_famfs.png)

**So we needed owners, workload grants, and lifetimes that every host shares.**

## ROOF: Built for Workload Control

*Why build ROOF, a new filesystem?*

We could have added these to famfs.<br>
But doing so changes famfs's answers to all four questions above.<br>
That would leave little of the original design.

We chose to build all of them together in ROOF (Region Ownership Over Fabric).

![On every host the ROOF kernel module checks each mapping against owners and grants kept in the CXL pool](figs/part2_architecture_full.png)

ROOF keeps one copy of its metadata in the CXL pool.<br>
Our earlier [CME work](https://medium.com/xcena-blog/cxl-gave-us-shared-memory-it-did-not-give-us-a-mutex-514b9cb025b3) decides whose turn it is to change it.<br>
Each of the four questions has its answer in this design:

- **Who creates a region, and who owns it?**<br>
  A workload on any host can create a region.<br>
  After its host takes a CME turn, the workload creates the region and owns it.
- **When do other hosts see it?** Once the creating host publishes the region's metadata in the pool.<br>
  Every host reads it in place, never from a stale cache line.
- **Who may use it, on every host?**<br>
  The owner grants each region to the workloads it chooses, not to accounts.<br>
  Each host's kernel module checks every mapping against the region's grants.<br>
  So every host reads the same answer from the pool.
- **How long do the region and each permission last?**<br>
  Each grant ends with its process.<br>
  ROOF reclaims the region after its last user, including the owner, is gone.

**In ROOF, workload control is part of the filesystem.**

## ROOF: Sharing With Control

*What does ROOF cover?*

Replay the same scene on ROOF.

1. **The Planner Agent on Host A stores its KV caches.**<br>
   The Planner Agent creates the regions itself and owns them.<br>
   It grants the booking context to the Hotel Agent and the private context to no one.
2. **The Hotel Agent on Host B reads the booking context in place.**<br>
   Host B sees the region as soon as Host A writes its metadata.<br>
   ROOF checks the Hotel Agent's grant before the kernel maps the region.
3. **The Hotel Agent tries to read the Planner's private context.**<br>
   Both agents run under one account.<br>
   But no grant names the Hotel Agent for this context.<br>
   So ROOF refuses it.
4. **The trip is booked, and no workload needs the caches.**<br>
   When the Planner Agent exits, ROOF reclaims its private context.<br>
   When the Hotel Agent exits, ROOF reclaims the booking context too.

![On ROOF the Planner Agent creates both contexts from Host A, Host B reads the metadata in place, Host B's ROOF refuses the Hotel Agent the private context, and both contexts are freed after their last user](figs/part2_roof.png)

**Owners, workload grants, and lifetimes now hold on every host.**

## ROOF: Control With Limits

*What does ROOF not cover yet?*

ROOF is experimental.<br>
These limits remain:

- **Trust.** ROOF trusts the kernel and the administrators on every host.<br>
  Workloads must not open the raw DAX device themselves.
- **Identity granularity.** ROOF tells workloads apart by process.<br>
  Two agents inside one process share its permissions.
- **Region lifetime.** ROOF reclaims a region after its last user, including the owner, is gone.<br>
  A later reader cannot find it.
- **Region size.** A region is sized once and cannot grow or shrink.
- **Revocation.** ROOF cannot take back a grant yet.<br>
  We still have to define what it stops.<br>
  It could stop new mappings, a mapping already in place, or both.

## Back to the First Question

Part 2 started with this scene:

> A workload on Host A has already processed a prefix.<br>
> It has stored the resulting KV cache in the CXL pool.<br>
> A workload on Host B is ready to reuse it.
>
> May this workload use it?

On ROOF, the answer depends on the workload itself.<br>
ROOF on Host B checks the workload against the grants the cache's owner wrote into the pool.<br>
Every host reads those grants and reaches the same answer.

The memory stays shared.<br>
The permission to use each region stays specific to each workload.

**That is why we built ROOF.**

ROOF is open source on [GitHub](https://github.com/xcena-dev/roof).<br>
The [design document](https://github.com/xcena-dev/roof/blob/main/docs/design.md) explains how it works.<br>
The [security report](https://github.com/xcena-dev/roof/blob/main/docs/security_report.md) lists the attackers it assumes and what it defends against.

---

Icons: [Lucide](https://lucide.dev) under the [ISC License](https://lucide.dev/license)

### Suggested tags

`CXL` · `KV Cache` · `Filesystem` · `Access Control` · `Shared Memory`
