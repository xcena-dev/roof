# Tools

The userspace programs that ship with the filesystem, built apart from the kernel module.

| Program | What it is |
|---|---|
| `mount.<fs>` | The helper `mount(8)` execs for this type. It finishes what `mount(2)` alone cannot. |
| `umount.<fs>` | The helper `umount(8)` execs. It stops the mount's daemon before unmounting. |
| `render-fsname.sh` | Renders a `.in` template with the name in `fsname`, and refuses to overwrite one that drifted. |
| `doc-fsname.py` | Holds the Markdown files to that name through an HTML comment reading `fsname` on each line that spells it. |

Putting them on a host is [`deploy/`](deploy/)'s job.

## The mount helper

Three steps, and a failure after the first unmounts and exits 32.

1. `mount(2)`, with `daemon_config=` stripped off the options 
and `daemon_uid=`/`daemon_gid=` added from the daemon's own account, 
picking the node id and binding the lock region to that account inside the call.
2. `cme-format` on the lock region, with the arguments `<daemon> --print-region-format <dir>` prints, 
run as the daemon account because step 1 is what closed the region to it.
3. `systemctl start` on the daemon's unit for that node id, 
and a wait of up to five seconds for `hello_done=1`.

```
mount -t rooffs none /mnt/rooffs -o daxdev=/dev/dax0.0,daemon_config=/etc/rooffs/daemon.yaml
```
<!-- fsname -->

`daemon_config=` defaults to `/etc/<fs>/daemon.yaml`, and `node_id=` names the slot to claim.
`daemon_uid=` and `daemon_gid=` are the ones the helper fills in, 
and a caller reaching `mount(2)` directly can set them itself, 
with either id but not both left at the any-account sentinel.
The commands the helpers run are overridable through the environment.

| Variable | Default |
|---|---|
| `<FS>_DAEMON` | `<daemon>` |
| `<FS>_CME_FORMAT` | `cme-format` |
| `<FS>_DAEMON_START` | `systemctl start <daemon>@<node>` |
| `<FS>_DAEMON_STOP` | `systemctl stop <daemon>@<node>` |

## The umount helper

The daemon's mapping of the lock region holds the mount, so `umount(2)` alone answers `EBUSY`.
The helper stops the node's daemon, unmounts, and on `EBUSY` names the processes still mapping the region.

## Build and install

```
cmake -S tools -B build/tools
cmake --build build/tools
sudo cmake --install build/tools
```

The helpers land in `/sbin`, where `mount(8)` looks, whatever the prefix says.
They link nothing of cme: `cme-format` and the daemon run as commands.
