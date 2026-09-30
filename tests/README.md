# The suite

Every case is a binary over `libroof`, takes its mount points on the command line, and returns the number of claims that failed.
`check.sh` runs them through `ctest`, and a label says what a case needs.

| Label | Needs |
|---|---|
| `onenode` | one mount |
| `multinode` | two mounts of one device |
| `manynode` | every mount the host has |
| `gpu` | a GPU the CUDA runtime can see |
| `pathrule` | identity rules naming one binary, so it runs apart |
| `lifecycle` | the module and every mount, for its whole run |
| `security` | a property the tree does not hold yet, so the gate leaves it out until the label comes off |

## Names and the index

| Case | Label | Claim |
|---|---|---|
| `test_names` | onenode | Create, find, remove and reuse a name, each checked by a lookup. |
| `test_names_peer` | multinode | Two mounts never disagree about what exists, what `stat` says, or who may remove it. |
| `test_names_concurrent` | multinode | Two nodes filling and emptying the namespace at once lose no entries. |
| `test_dupname` | multinode | Two nodes creating one name with `O_EXCL` at once: exactly one wins. |
| `test_rat_exhaustion` | onenode | The table runs out with `ENOSPC`, and removing names frees the slots. |

## Placement and mapping

| Case | Label | Claim |
|---|---|---|
| `test_mmap_unplaced` | onenode | A created but unplaced file answers `ENODATA`, not `EACCES`. |
| `test_mmap_bytes` | onenode | What a mapping stores is read back through every path. |
| `test_mmap_peer` | multinode | Bytes one node stores are the bytes the other reads. |
| `test_mmap_cuda` | gpu | `cudaHostRegister` accepts an address `File::map` handed out. |
| `test_cache_policy` | onenode | A file lands in the pool asked for, and the vma flags show that pool's mmap path. |
| `test_overlap` | multinode | No two regions share physical bytes. The overlap checker under `libroof/`, not a case. |

## Permissions and delegation

| Case | Label | Claim |
|---|---|---|
| `test_perm_grants` | onenode | Which grants the kernel writes down on a node's own file. |
| `test_perm_peer` | multinode | What a node that owns nothing may do, one rung at a time. |
| `test_perm_revoke` | multinode | Who may take a row off: the caller it names, or ADMIN, and only the node that wrote it. |
| `test_cross_process` | multinode | One file's life watched from another process through the other mount. |
| `test_fork_inherit` | onenode | A descriptor survives `fork` and its permission does not. |
| `test_pid_reuse` | multinode | A reused pid inherits nothing from the row that named it. Skips without `cap_sys_admin`. |
| `test_postexec_attack` | multinode, pathrule | `execve` keeps pid and start time, and the executable is what separates the granted process from the new image. `postexec_other` is that image. |
| `test_exec_generation` | multinode | A process that re-execs its own binary keeps its delegation slot but not the row from before the exec. |
| `test_cloexec_required` | onenode | Every data path refuses a descriptor without `FD_CLOEXEC`, and admits the same one once `fcntl` sets the bit. |
| `test_channel_device` | onenode | The daemon's upcall channel under `/dev` is not openable by an unprivileged process. |
| `test_lock_region_guard` | onenode | The lock region is the daemon's alone: no create, unlink, rename, perm ioctl, or write mapping. |

## The mapping after mmap returns

| Case | Label | Claim |
|---|---|---|
| `test_vm_protect` | onenode | What `mprotect`, `fork`, `mremap` and a partial `mprotect` may do to the mapping. The child's segfault is the pass. |
| `test_vm_protect_peer` | multinode | Widening a peer's mapping goes back through the delegation. |
| `test_contract` | onenode | The two refusals a correct caller cannot provoke. |

## Reclaiming

| Case | Label | Claim |
|---|---|---|
| `test_gc_deleg` | multinode | A row outlives its process, and a sweep returns its slot. |
| `test_gc_owner` | multinode | A file outlives its creator, and a sweep returns the region. |
| `test_dead_owner_unlink` | multinode | A dead owner's region can be unlinked without DELETE, and only from the node that owns it. |
| `test_unlink_live_refs` | multinode | Unlink takes the name at once, and the extent only after the last descriptor or mapping on any node has gone. |
| `test_unlink_holders` | multinode | With no holder the extent goes with the name. With holders on both nodes it goes when the last one leaves, and meanwhile no open, no placement and no other bytes reach it. |
| `test_gc_owner_readers` | multinode | A dead owner's region stays while a peer reads it, row or no row, and goes once the reader leaves. A live process may unlink the name meanwhile. |
| `test_gc_zero_extent` | onenode | A region the sweep takes back from a dead owner is zeroed first, so the next file placed over its extent reads zero. |
| `test_unlink_race` | multinode | Two nodes creating, mapping and unlinking at once never read bytes that are not their own file's, and leave nothing standing. |

## The cross-node lock

| Case | Label | Claim |
|---|---|---|
| `test_meta_lock` | multinode | Paths that write shared metadata take a turn, paths that only read take none. Skips when no turn was taken at all. |

## What the filesystem publishes

| Case | Label | Claim |
|---|---|---|
| `test_sysfs` | multinode | Every file under `/sys/fs/<fs>/` reads and follows what the library did, and `deleg_info` is root's alone. |

## Contention

| Case | Label | Claim |
|---|---|---|
| `mount_stress` | manynode | Every mount on the metadata domain at once, then every node stamping one cacheline of a shared region, and nothing left behind. |

## Findings

Red on purpose while the tree does not hold the property, a regression check once it does.

| Case | Label | Claim |
|---|---|---|
| `test_security_deleg_revoke` | security | An owner can take back the row an upcall wrote for a process. |
| `test_security_group_revoke` | security | Removing a group's row asks for the standing that writing it did. |
| `test_security_cloexec_cost` | | A read costs the same wherever the descriptor sits in the table. |
| `test_security_helper_latch` | | One late answer does not refuse the mount for good. Runs only with `SECURITY_HELPER_PID` set, under sudo. |

## Mount lifetime and election

`bootstrap_chaos.sh` owns the module for its whole run and reformats the device on the way out.
Its mounts come up without a daemon, so the script turns on the test knob that stands in for the daemon on each of them.
`chaos_mapper` holds regions with a live process on the far side of a revoke.
`rat_summary_crash.sh` makes one node die between two lines of a create, a placement or an unlink, kills its daemon, and checks that the survivor's sweep puts the RAT summary line back in step with the entries and keeps writing.
It also mounts a node onto a zeroed summary line and checks that the node searches the entries until a sweep repairs the line.
`rat_place_race.sh` places regions from both nodes at once and checks that no two extents overlap.

| Case | Claim |
|---|---|
| T2 | Four racing mounts elect exactly one formatter and no two share a node_id. |
| T3 | A graceful umount hands the slot back, so the remount elects no formatter. |
| T4 | Eight mounts fill the slot table, and the ninth is refused with `EBUSY`. |
| T5 | A stalled node keeps its slot until the admin's sweep, then fences itself, revokes its mappings, and still unmounts. |
| T6 | A recovery its admin abandons is finished by the next admin. |
| T7 | A mapping of a recovered node's region takes a SIGBUS when that node comes back. |
| T8 | Every mount but one stops at once, and the survivor recovers all of them. |
| T9 | A recovery empties the dead node's rows everywhere and marks a region a live node still holds a row on. |
| T10 | A marked region takes no new row, stays out of the allocator, and goes when its last row does. |
| T11 | A caller drops the row naming it without asking, and a row naming somebody else moves only for the owner. |
| T12 | A dead owner's region that a reader holds through the default alone is marked, not freed, and goes when the reader does. |
| T13 | An unlinked extent held only by a node that then dies goes back once that node is recovered. |
| T14 | A clean unmount clears every row naming the node before its slot goes, so a region a peer holds is marked and the id's next holder owns none of it. |
| T1 | A formatter that never publishes is stolen from. Runs last, since it ends with a slot held. |

## Two hosts

Every case runs on one host, where a peer is a second mount of the same device.
A run across two machines follows `multi/README.md`.
