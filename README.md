# ROOF (Region Ownership Over Fabric)

## A multi-node filesystem for fabric-attached memory,<br>with cross-node identity and access control.

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Kernel](https://img.shields.io/badge/kernel%20module-GPL--2.0-blue.svg)](kernel/LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Status](https://img.shields.io/badge/status-experimental-orange.svg)

CXL does not answer *"who owns these bytes, and who may read them?"*

Fabric-attached memory (FAM) is byte-addressable from every host.<br>
The fabric controls which host may reach that memory,<br>
and the host is the finest unit it distinguishes.<br>
A filesystem has always divided bytes into named objects, and given each one an owner and a lifetime.<br>
ROOF answers the question as a filesystem, where the object is a region.<br>
The owner identity and the region's access control live in its own metadata, in FAM itself.

Keeping the metadata in FAM is what makes an identity mean the same thing on every node.<br>
The owner is attested once, when the region is created, and the result is written into the region.<br>
Every node that maps the region afterwards reads the owner from there instead of judging it again with its own accounts.<br>
Every node reads the same metadata,<br>
so every node sees the same objects and enforces the same access control.

## Who it is for

ROOF is for systems that share data between hosts through CXL memory and must decide who may read each piece of it.<br>
It gives that memory what a filesystem gives a disk.

- Named regions that every host sees alike, each with an owner and a lifetime.
- An identity per process that means the same thing on every host.
- Access control per process and per operation, carried by the region itself, so read can be allowed without write.
- Direct `mmap()` access once a mapping is admitted, with no copy and no kernel code on the data path.
- Cleanup of what a dead process or a dead host left behind, with no coordinator between the hosts.

What the design assumes of the platform is in [Assumptions](docs/design.md#4-assumptions).

> [!WARNING]
> Experimental.<br>
> The layout in FAM, the upcall protocol and the permission model are all subject to change.<br>
> Not for production use.

## Quick start

One privileged script brings a host to where the filesystem is mounted and serving.

```sh
sudo tools/deploy/install.sh --account "$USER" --device /dev/dax0.0
```

It builds the module, the mount helpers and the library,<br>
then writes the account, the group and one mount unit per mount point,<br>
runs the daemon's own installer, and mounts.<br>
The default is one mount point under `/mnt`, named after `fsname`.<br>
The suite's multinode cases need a second node on the same device,<br>
which a test host adds with a second `--mount`.

One binary comes from outside this tree:<br>
`cme-format`, from the [CME](https://github.com/xcena-dev/cme) project,<br>
which the mount helper runs to lay out the lock region.<br>
The daemon links `libcme` directly and is itself the CME peer.

Once the script returns, that mount point is a filesystem.<br>
`mmap()` is the data path and `write()` is refused.<br>
A region is sized with `ftruncate()` on an open descriptor, and `truncate()` on a path is refused.<br>
An unmount is refused while a file on that node is still open, and the mount and its daemon stay up.<br>
A caller reaches a region in one of three ways.

- It created the region.<br>
The daemon attested it then,<br>
and the owner check reads what it recorded.
- It has no grant, and the daemon decides.<br>
A mapping that no row covers asks the mount's daemon,<br>
and an allow is written into the region as a row.
- The owner granted it.<br>
`FS_IOC_PERM_GRANT` writes a row naming an account,<br>
and the default grant covers everyone with no row.

ROOF performs its own access control instead of honouring the mode bits on the mount.

## Architecture

![Architecture overview: an application, the helper daemon and the filesystem kernel module on one node, over one CXL FAM device that every node maps. The colour of a box says who owns its bytes: the kernel module holds the metadata area, the daemon's CME peer holds the lock region, and the application's mappings hold the region extents.](docs/figs/architecture.png)

The kernel module, the helper daemon and the application share one region of FAM.

The kernel module owns the filesystem's metadata in FAM, and the fault path an `mmap()` runs through.<br>
The helper daemon is one process per mount, and it answers four upcalls.

- `ATTEST_REQUEST` records an owner identity when a region is created.
- `ACCESS_REQUEST` tells the kernel what a reader may do with a region.
- `LOCK_REQUEST` takes this node's turn on the shared metadata.
- `UNLOCK_REQUEST` returns it.

[CME](https://github.com/xcena-dev/cme) is what makes that turn mean something between hosts,<br>
because it arbitrates exclusive ownership in the shared memory itself, with no coordinator and no atomics.

## Components

| | |
|---|---|
| [`kernel/`](kernel/) | The module: VFS, the region metadata, the `mmap()` fault path, the upcall device. Built by kbuild against the running kernel, not by this project's CMake. |
| [`daemon/`](daemon/) | The helper daemon, one per mount. The identity and policy backends, and the node's CME turn. |
| [`libroof/`](libroof/) | The library over the syscalls an application makes on a mount. Three operations read shared metadata and then write it, which is what it exists to get right. |
| [`tests/`](tests/) | The suite. One binary per case over `libroof`, because that is the surface an application has. |
| [`tools/`](tools/) | The mount and umount helpers, plus [`deploy/`](tools/deploy/) with the installer, its undo, and the mount unit template. |
| `shell/` | `units.sh` is how every script reads the host's mounts and daemon instances, so the installer, `check.sh` and the chaos runner all see the same ones. `paths.sh` is where the deploy scripts agree on locations. |
| [`docs/`](docs/) | The design, the security report, the figures and their sources, and article drafts. |

`fsname` is one line at the root naming the filesystem,<br>
and the module name, the mount default, the library, the helper names and the pkg-config file are all read off it.<br>
A source file spells a neutral macro such as `FS_IOC_PERM_GRANT`,<br>
and only the value renders as `rooffs`. <!-- fsname -->

## Build and check

The userspace half is three CMake projects,<br>
and the kernel module builds with kbuild rather than from any of them.<br>
The daemon reads part of [CME](https://github.com/xcena-dev/cme) at build time,<br>
from a checkout at `~/cme` unless `-DDAEMON_CME_DIR` names another.

```sh
cmake -S . -B build && cmake --build build -j                     # the library and the tests
cmake -S tools -B build/tools && cmake --build build/tools -j     # the mount helpers
cmake -S daemon -B build/daemon && cmake --build build/daemon -j  # the daemon
make -C kernel                                                    # the kernel module, against the running kernel
```

`check.sh` runs every gate this tree has in one pass:<br>
headers, docs, build, format, tidy, daemon, suite.<br>
Each gate reports on its own and the run keeps going,<br>
so the exit code is the number of gates that failed rather than the first one.<br>
There is no hosted CI, so this script is the gate.

```sh
./check.sh              # every gate, exit code = number of gates that failed
./check.sh --gate tidy  # one gate by name
```

The suite needs two mounts of one device,<br>
which `tools/deploy/install.sh --mount /mnt/rooffs --mount /mnt/rooffs2` provides. <!-- fsname --><br>
The suite gate runs the cases labelled `root` in a pass of their own under `sudo`.

## Documentation

| | |
|---|---|
| [`docs/design.md`](docs/design.md) | The design: the records on the medium, the region lifecycle, the daemon, the turn, GC and recovery |
| [`docs/security_report.md`](docs/security_report.md) | What the design must prevent, and the attack scenarios checked against it |
| [`docs/articles/`](docs/articles/) | A two-part article: [why a shared CXL pool needs permissions](docs/articles/medium_draft_part1.md) and [how ROOF provides them](docs/articles/medium_draft_part2.md) |
| [`daemon/README.md`](daemon/README.md) | The helper daemon: its upcalls, its identity and policy backends, and how to run it |
| [`libroof/README.md`](libroof/README.md) | The library an application calls, with an example in [`libroof/examples/overlap.cpp`](libroof/examples/overlap.cpp) |
| [`tests/README.md`](tests/README.md) | The suite, and [`tests/measurement/`](tests/measurement/README.md) for what each operation costs |
| [`tools/deploy/README.md`](tools/deploy/README.md) | The installer, its undo, and the mount unit |

## License

Apache-2.0, except `kernel/`, which is GPL-2.0-only because the module links the kernel.<br>
See [`LICENSE`](LICENSE) and [`kernel/LICENSE`](kernel/LICENSE).
