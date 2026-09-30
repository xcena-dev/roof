#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Per-span microseconds across one probe run, from the module-wide upcall counters.

    span_delta.py <mount> <rounds> <tag> [build-dir]

The sysfs file carries sums rather than means, so a delta across the run divided by the delta in
count is what one round trip of that kind spent in that span.
"""

import subprocess
import sys
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]


def fs_name() -> str:
    """The one line at the tree's root that names the filesystem, and so its sysfs directory."""
    for line in (TREE / "fsname").read_text().splitlines():
        if line.startswith("FS_NAME="):
            return line.partition("=")[2].strip()
    raise SystemExit("fsname has no FS_NAME= line")


SYSFS = Path("/sys/fs") / fs_name()
COUNTERS = SYSFS / "test" / "upcall_latency"


def stages_path(mount: str) -> Path:
    """The stage counters are per mount, so the file to read comes from the mount's node id."""
    for line in Path("/proc/self/mounts").read_text().splitlines():
        fields = line.split()
        if len(fields) < 4 or fields[1] != mount:
            continue
        for option in fields[3].split(","):
            if option.startswith("node_id="):
                node = option.removeprefix("node_id=")
                return SYSFS / f"node{node}" / "test" / "meta_stages"
    return Path("/nonexistent")


def find_probe(build_dir: str) -> Path:
    """The suite builds the probe, so this looks for the artifact rather than compiling its own."""
    built = TREE / build_dir / "tests" / "measurement" / "probe_ops"
    if not built.is_file():
        raise SystemExit(f"{built} is not there: cmake --build {build_dir} --target probe_ops")
    return built


def read_counters(source: Path = COUNTERS) -> dict[tuple[str, str], tuple[int, int, int]]:
    if not source.is_file():
        return {}
    taken = {}
    for line in source.read_text().splitlines():
        fields = line.split()
        if len(fields) != 5:
            continue
        kind, span, count, total_ns, worst_ns = fields
        taken[(kind, span)] = (int(count), int(total_ns), int(worst_ns))
    return taken


def main() -> int:
    if len(sys.argv) not in (4, 5):
        print(__doc__.strip())
        return 2
    mount, rounds, tag = sys.argv[1], sys.argv[2], sys.argv[3]
    probe_path = find_probe(sys.argv[4] if len(sys.argv) == 5 else "build")

    stages = stages_path(mount)
    was_spans, was_stages = read_counters(), read_counters(stages)
    probe = subprocess.run([str(probe_path), mount, rounds, tag], capture_output=True, text=True)
    sys.stdout.write(probe.stdout)
    sys.stderr.write(probe.stderr)

    report("upcall", was_spans, read_counters())
    report("stage", was_stages, read_counters(stages))
    return probe.returncode


def report(what: str, before: dict, after: dict) -> None:
    """The worst column is a running maximum the module never resets, so it belongs to every run
    since the load rather than to this one. It is printed as such and never differenced."""
    if not after:
        return
    print()
    print(f"{'kind':8} {what:18} {'n':>6} {'us/op':>9} {'worst since load':>17}")
    for (kind, span), (count, total_ns, worst_ns) in after.items():
        was_count, was_total, _was_worst = before.get((kind, span), (0, 0, 0))
        rounds_here = count - was_count
        if rounds_here <= 0:
            continue
        took_us = (total_ns - was_total) / rounds_here / 1000.0
        print(f"{kind:8} {span:18} {rounds_here:6d} {took_us:9.2f} {worst_ns / 1000.0:17.2f}")


if __name__ == "__main__":
    sys.exit(main())
