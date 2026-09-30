# tests/measurement/ — what each operation costs

Nothing here is a case.
A case asserts and returns the number of claims that failed, whereas a probe measures, 
so a slow run is a result rather than a failure.
None of it is registered with `ctest`, and all of it builds with the suite, 
so a change that breaks a probe breaks the build gate.
Every probe prints CSV rows with min, median, p95, max and mean, 
and leaves the mount as it found it.

| Probe | What it prices |
|---|---|
| `probe_ops` | The syscalls that take a turn, one at a time: `open(O_CREAT)`, `ftruncate`, `open` on a name that exists, and `unlink`, each over its own set of names so no sample waits on the sample before it. The mount's `meta_lock` counter is read around each phase, so the turns a phase took are measured. |
| `probe_access` | One access upcall, from a consumer that is not the owner. The owner mount creates and places a region per round with the default at owner-only, and the consumer mount maps it for the first time, which is the one map that reaches the helper. Needs two mounts of one device. |
| `probe_perm` | The three permission ioctls, priced apart because they do different work under the same turn: a grant fills a row, a default touches the shared line, a revoke scans the table. A prefill lays rows down first, which separates a scan over a full table from one over an empty one. |
| `probe_data` | The data path once the name and the extent exist, none of which takes a turn: `read()` at two sizes, a store into a standing mapping, and a second `mmap` of a name already delegated. The line pass sweeps the region at cacheline stride, since one store lands under the clock's own overhead. A padding count opens that many descriptors first, which is where the close-on-exec check's cost would show. |
| `probe_devdax` | The floor: `open` and `mmap` on the device_dax chardev with no filesystem in the way. Only the mapping half has a counterpart, since a chardev holds no names. Needs the chardev free, so not while the module holds the device. |
| `span_delta.py` | Runs `probe_ops` and turns the kernel's `upcall_latency` sums into microseconds per span: `helper_wake` and `read_copy` are the kernel handing the request out, `helper_turnaround` is everything the daemon spends, `response_match` and `waiter_wake` are the kernel handing the answer back, and `whole` is the round trip. An `unlock` has no answer, so its `whole` ends at `read_copy`. Needs a module built with `CONFIG_FS_TEST_KNOBS`. |

A probe prices the daemon installed under `/usr/local/bin` and the module loaded, not the tree.
An edit reaches a measurement only after `daemon/deploy/install.sh` and `tools/deploy/reload.sh` have put it in place.
