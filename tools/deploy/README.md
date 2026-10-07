# Deploy

Bringing a mount up on a host with one privileged unit, and taking it down again.

Two operations need a capability: 
loading the module takes `CAP_SYS_MODULE` and `mount(2)` takes `CAP_SYS_ADMIN`. 
Both happen inside the mount helper under the mount unit, 
so the mount is the only privileged step, 
and the account the lock region opens to rides in as a mount option.
The daemon runs as its own unprivileged account, 
and the applications run as the account `install.sh` is given.

| Script | What it does |
|---|---|
| `install.sh` | The whole bring-up: group, device, builds, module, daemon, units, mounts. Run once per host. |
| `reload.sh` | The edit loop after it: swaps the module and the helpers under the mounts. |
| `uninstall.sh` | Takes back everything `install.sh` put on the host, device included. |
| `leftovers.sh` | Lists what is still on the host and, with `--rm`, removes it. Works without the state file. |
| `mount.example.mount.in` | The mount unit `install.sh` renders once per mount point. |

## Install

```
sudo tools/deploy/install.sh --account app --device /dev/dax0.0
```

One run is the whole bring-up: 
the group, the state file, the builds, the module, 
the daemon through its own installer, the mount units, and the mounts started.

| Option | Meaning |
|---|---|
| `--account NAME` | the account every application runs as (default: the sudo caller) |
| `--device PATH` | the DAX device a mount without one of its own sits on |
| `--mount PATH[=DEV]` | a mount point, on `DEV` or on `--device`; repeat for more (default: one under `/mnt`, named after the filesystem) |
| `--no-start` | write everything but leave the mounts down |
| `--reset-cfg` | replace the config and the policy with this tree's examples |
| `--skip-build` | install what is already built |
| `--prefix PATH` | where the library and its headers go (default: `/usr/local`) |

`--account`, `--device` and `--mount` shape the config a first run plants.
Once the config is there, 
it is the one source: 
the accounts and the mounts come from it, 
and an edit to it is what changes them.

Each device is unbound from `device_dax` first, 
since the module maps it itself, 
and never while something has it open, 
since that locks the host up.
The daemon's unit is a template on a node id the kernel picks, 
so a drop-in beside it names every mount point of the host as writable.

## Reload after an edit

```
sudo tools/deploy/reload.sh
```

Stops the daemons and the mounts, 
rebuilds and reinstalls the module and the helpers, 
and brings the mounts back.

## Uninstall

```
sudo tools/deploy/uninstall.sh                # everything install.sh put here
sudo tools/deploy/uninstall.sh --keep-state   # leave deploy.state for a later restore
```

The mounts are unmounted first, 
then the units removed, 
then the daemon, 
then the module unloaded, 
and last the device goes back under the driver it had.
Each holds the next: 
the daemon's mapping holds the mount, 
the mount holds the module, 
and the module holds the device.
A mount that does not come down stops the uninstall there, 
and nothing after it is removed.
`deploy.state` under `/var/lib/<fs>/` is where `install.sh` recorded that driver, 
and without it the device is left alone.
`cme-format` is not removed: a different project installs it.

## Leftovers

```
tools/deploy/leftovers.sh                     # what is still here
sudo tools/deploy/leftovers.sh --rm           # remove it
FS_NAME=oldname sudo tools/deploy/leftovers.sh --rm
```

Reads nothing but the host, 
so it works where `deploy.state` is gone.
The third form cleans a host that was installed 
under a different name than `fsname` spells now.

## Two things to keep true

Every node runs NTP.
A slot's heartbeat, an entry's allocation time and a grant's time are each one node's clock read on another, 
and a node running fast reclaims what a peer just made.

The daemon's unit is never ordered after the mount.
The mount helper waits for the daemon from inside `mount(2)`, 
so an `After=<mount>` on the daemon deadlocks.
The teardown runs the other way through the umount helper, 
since a `[Mount]` unit has no `ExecStop=`.
