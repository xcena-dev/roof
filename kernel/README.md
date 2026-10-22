# The kernel module

The kernel half of the filesystem: 
a Linux module that lays regions out in CXL shared memory 
and decides, on every data access, who may reach them.

`make` builds it into `build/`, against the running kernel's headers.
Putting it on a host is [`tools/deploy/`](../tools/deploy/)'s job, 
and the suite is [`tests/`](../tests/).
What follows is what only this module can say: 
how it is mounted, what it answers to, and why it refuses.

Everything below is what [`libroof/`](../libroof/) wraps: 
the options a mount is given, 
the rules a region is held to, the ioctls, and the errors they answer with.
An application reaches all of it through the library 
and spells none of these names itself.
They are listed here 
because they are the contract the library rests on, 
and the header that declares them is `include/uapi.h`, 
rendered from `uapi.h.in` beside it.
Its names are spelled neutrally, `FS_IOC_PERM_GRANT` and `struct fs_perm_req`, 
and only their values carry the filesystem's name.

## Mount Options

A mount is not `mount(2)` alone.
The helper that `tools/deploy/` installs lays out the lock region 
and starts this node's daemon after it, 
and the module refuses with `-EAGAIN` while that daemon is down: 
every create, every metadata write, 
and every mapping the rows do not already cover.
So a mount is brought up through the unit the installer writes, 
and these are the options the module reads off it.

| Option | Description |
|--------|-------------|
| `daxdev=/dev/daxX.Y` | The DEV_DAX device. Required. |
| `format` | Lay the filesystem out on the device. First mount only. |
| `node_id=N` | Claim slot N as this mount's identity. Without it the kernel takes the first free slot. |
| `granule_shift=`, `uc_mib=` | The medium's split, written by `format` and read back by every later mount. Refused without `format`. |
| `daemon_config=PATH` | The daemon's config for this mount. The helper keeps it and strips it before `mount(2)`. |

## What a Region Is

How an application uses one is [`libroof/`](../libroof/)'s to explain.
What the module holds every caller to, library or not, is this.

- A region comes into being in two steps.
  `open(O_CREAT)` reserves a metadata slot and nothing else, 
  and the first `ftruncate()` allocates the bytes, rounded up to 2 MiB.
- The size is fixed from then on.
  A second `ftruncate()` on a region with a size is refused with `-EACCES`, 
  and `ftruncate(fd, 0)` does nothing.
- `mmap` is the data path.
  `write()` is always refused with `-EACCES`.
- Every descriptor needs `O_CLOEXEC`.
  A data path reached through one without it is refused with `-EACCES`.
- A filesystem holds up to 256 regions, and a name is at most 63 bytes.
- `unlink()` after the last `close()` deletes a region.
  One left behind is reclaimed by GC once its owner process has exited.

## Permission System

The filesystem uses its own delegation records instead of POSIX file permissions.
A check runs at the data access, `mmap`, `read` and `ioctl`, and not at `open()`.

**Permission bits:**

| Constant | Value | Meaning |
|----------|-------|---------|
| `FS_PERM_READ` | 0x0001 | `read()`, `mmap(PROT_READ)` |
| `FS_PERM_WRITE` | 0x0002 | `mmap(PROT_WRITE)` |
| `FS_PERM_DELETE` | 0x0004 | `unlink()` |
| `FS_PERM_ADMIN` | 0x0008 | Set the default, take another account's row off, hand on `ADMIN` or `GRANT` |
| `FS_PERM_IOCTL` | 0x0010 | The cache policy ioctls |
| `FS_PERM_GRANT` | 0x0020 | Hand on what the caller holds, `ADMIN` and `GRANT` excepted |

**What the rows say:** 
owner, then the default, then the delegation rows.
The owner is the process that created the region, on the node it created it from.
A row written by `PERM_GRANT` names an account or a role.
An account row is read only on the node that wrote it.
A role row names a workload by the group and role its identity carries, 
on every node or on the one the row names.
Only a row naming a process keeps a region past its owner.

**When the rows say nothing:** 
a `read` or `mmap` the rows refuse asks the mount's daemon.
The daemon answers with what its policy grants and with the caller's group and role.
The kernel adds what the region's role rows grant those labels.
If the sum covers the access, the kernel writes a row naming the calling process, 
and the access goes through on that row.
Otherwise it is `-EACCES`.

## ioctl Reference

| Command | Direction | Struct | Required |
|---------|-----------|--------|----------|
| `FS_IOC_PERM_GRANT` | W | `fs_perm_req` | `ADMIN` or `GRANT`. Only `ADMIN` hands on `ADMIN` or `GRANT`. `kind` picks an account row or a role row. |
| `FS_IOC_PERM_SET_DEFAULT` | W | `fs_perm_req` | `ADMIN` |
| `FS_IOC_PERM_REVOKE` | W | `fs_perm_req` | An account row: nothing for a row naming the caller, `ADMIN` for any other. A role row: `ADMIN`. |
| `FS_IOC_REGION_ATTR_SET` | W | `fs_region_attr_req` | `IOCTL`. Refused with `-EBUSY` once the region is placed, and with `-EINVAL` for a paged region in the uncached pool, a `device` above the mount's device count, or an uncached region on a device other than 1. |
| `FS_IOC_REGION_ATTR_GET` | R | `fs_region_attr_req` | Nothing. It names only the pool, the placement, the piece count and the device. |

## What the Module Reports

Under `/sys/fs/rooffs/`, readable by anyone unless noted. <!-- fsname -->

| File | What it says |
|------|--------------|
| `node<N>/daemon_state` | Whether node N's daemon holds the channel and has greeted the kernel: `reader_open`, `hello_done`, `helper_dead`, `queue_depth`. `hello_done=1` is a mount that answers. |
| `region_info` | One line per region: its slot, the node and process that own it, its state, size, offset and name. A show holds one page, so a full table is cut; `rat/<slot>` is the whole of it. |
| `rat/<slot>` | One file per RAT slot with region_info's row for it, or `free`. A reader that wants every region loops over the slots. |
| `extents/<slot>` | One line per piece of the slot's region, in file order: its device offset, its length and its device index. `rat/<slot>` shows only the first piece's offset and the total, which is all a contiguous region has. A slot with no region shows nothing. |
| `perm_info` | One line per region: the default grant and how far its delegation rows extend. |
| `pool_info` | The two pools the device is split into, uncached and write-back, each with its start, its size and what is free in it. `statfs` reports the two summed, so this is where a full uncached pool beside an empty write-back one shows. A `device <index> <start> <total> <free>` line per device follows, for that device's write-back pool. |
| `deleg_info` | The delegation rows of one region. Root writes the region's slot number to it, then reads. |
| `gc_status` | Per node, whether its collector thread is alive and the epoch it has reached. |

## Error Codes

| Error | Context | Meaning |
|-------|---------|---------|
| `EACCES` | mmap, read, ioctl, unlink, ftruncate | Refused by the rows and the daemon, a descriptor without `O_CLOEXEC`, or a second `ftruncate` |
| `EAGAIN` | create, unlink, mmap, read, PERM_* | The mount's daemon is down, or the metadata turn was refused. Transient, retry. |
| `ETIMEDOUT` | create, mmap, read | The daemon stopped answering. |
| `EIO` | any | The region's metadata could not be reached, or the mount is fenced. Not transient. |
| `EEXIST` | open(O_CREAT) | Another node took that name first. |
| `EPERM` | PERM_GRANT, create, unlink | Handing on `ADMIN` or `GRANT` without `ADMIN`, or touching the lock region's name |
| `ENOENT` | PERM_REVOKE | No such row on this node |
| `ENODATA` | mmap | The region has a slot and no bytes yet, before its first `ftruncate` |
| `ENOSPC` | open(O_CREAT), PERM_GRANT | The 256 regions or a region's 28 rows are all taken |
| `ENAMETOOLONG` | open(O_CREAT) | Name longer than 63 bytes |
| `EBUSY` | REGION_ATTR_SET | The region is already placed |
| `EINVAL` | ioctls | Bad argument |
