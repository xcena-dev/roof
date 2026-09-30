#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# rat_place_race -- two nodes place regions at once, and no two extents overlap.
#
# A placement picks its gap from a list the mount built while the helper was still taking the
# turn, and trusts that list when the RAT's placement count has not moved since. This is the
# steady-state check of that trust: workers on both mounts create and place files against each
# other, the RAT is read while every extent stands, and the extents are checked pairwise.
#
# Runs against the host's two mounts and their daemons, as the account that owns the files. It
# shares the lifecycle label because it needs the host's mounts to itself while it runs:
#
#   ctest --test-dir <build> -R rat_place_race

set -u -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
# shellcheck source=/dev/null
. "${ROOT}/fsname"

SYSFS="/sys/fs/${FS_NAME}"
MOUNT_A="${MOUNT_A:-/mnt/${FS_NAME}}"
MOUNT_B="${MOUNT_B:-/mnt/${FS_NAME}2}"
# Per node. Kept under the RAT's slots with room for the lock region and stragglers.
WORKERS="${WORKERS:-4}"
FILES_PER_WORKER="${FILES_PER_WORKER:-12}"
ROUNDS="${ROUNDS:-3}"
SIZE_BYTES="${SIZE_BYTES:-$((4 << 20))}"

FAILED=0
pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; FAILED=$((FAILED + 1)); }
skip() { printf 'SKIP %s\n' "$*"; exit 77; }

for point in "$MOUNT_A" "$MOUNT_B"; do
    mountpoint -q "$point" || skip "no mount at ${point}"
done
[ -r "${SYSFS}/rat/0" ] || skip "no ${SYSFS}/rat/<slot> files to read the extents from"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# One process per worker for its whole life: a region's owner is the process that created it, so
# the unlink at the end has to come from the same one. It places its files, says so, and waits for
# the go marker before it takes them down.
worker()
{
    local mount="$1" tag="$2" round="$3" go="$4"
    python3 - "$mount" "$tag" "$round" "$FILES_PER_WORKER" "$SIZE_BYTES" "$WORK" "$go" <<'EOF'
import os, sys, time
mount, tag, round_, count, size, work, go = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), sys.argv[6], sys.argv[7]
names = [f"{mount}/race-{round_}-{tag}-{idx}" for idx in range(count)]
fds = []
for name in names:
    fd = os.open(name, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_CLOEXEC, 0o644)
    os.ftruncate(fd, size)
    fds.append(fd)
open(f"{work}/done-{round_}-{tag}", "w").close()
while not os.path.exists(f"{work}/{go}"):
    time.sleep(0.05)
for fd, name in zip(fds, names):
    os.close(fd)
    os.unlink(name)
EOF
}

# Every slot's row, one file each, so no page limit cuts the table.
ratRows()
{
    local slot
    for slot in "${SYSFS}"/rat/*; do
        cat "$slot"
    done
}

# Every standing extent, sorted by offset, and any pair that overlaps.
overlaps()
{
    ratRows | awk -F'\t' 'NF == 6 && $5 != "0x0" && $4 > 0 { printf "%d %d %s\n", strtonum($5), strtonum($5) + $4, $6 }' |
        sort -n |
        awk 'NR > 1 && $1 < prev_end { printf "%s [%d,%d) overlaps %s [%d,%d)\n", prev_name, prev_start, prev_end, $3, $1, $2 }
             { prev_start = $1; prev_end = $2; prev_name = $3 }'
}

# Until @2 done markers of round @1 are there, or 120s have passed.
waitForDone()
{
    local round="$1" expected="$2"
    local deadline=$((SECONDS + 120))
    while [ "$(find "$WORK" -name "done-${round}-*" | wc -l)" -lt "$expected" ]; do
        if [ "$SECONDS" -ge "$deadline" ]; then
            fail "round ${round}: only $(find "$WORK" -name "done-${round}-*" | wc -l) of ${expected} workers placed their files"
            return 1
        fi
        sleep 0.2
    done
}

# The table as it stands must hold no overlapping pair, and @2 regions of this run.
checkTable()
{
    local round="$1" expected_placed="$2"
    local placed clashes
    placed="$(ratRows | awk -F'\t' 'NF == 6 && $6 ~ /^race-/ { count++ } END { print count + 0 }')"
    clashes="$(overlaps)"
    if [ -z "$clashes" ] && [ "$placed" -ge "$expected_placed" ]; then
        pass "round ${round}: ${placed} regions standing from two nodes, none overlapping"
    else
        fail "round ${round}: ${placed} regions standing, overlaps: ${clashes:-none}"
    fi
}

# Both nodes place at once, then both unlink at once.
for round in $(seq 1 "$ROUNDS"); do
    pids=()
    for idx in $(seq 1 "$WORKERS"); do
        worker "$MOUNT_A" "a${idx}" "$round" "go-${round}" &
        pids+=($!)
        worker "$MOUNT_B" "b${idx}" "$round" "go-${round}" &
        pids+=($!)
    done
    waitForDone "$round" $((WORKERS * 2))
    checkTable "$round" $((WORKERS * 2 * FILES_PER_WORKER))
    touch "${WORK}/go-${round}"
    for pid in "${pids[@]}"; do
        wait "$pid" || fail "round ${round}: a worker failed"
    done
done

# One node unlinks while the other places, which is where the placement count goes odd under a
# placer that has to notice. Node B's workers are released the moment node A's start.
for round in $(seq $((ROUNDS + 1)) $((ROUNDS * 2))); do
    pids_b=()
    for idx in $(seq 1 "$WORKERS"); do
        worker "$MOUNT_B" "b${idx}" "$round" "go-${round}-b" &
        pids_b+=($!)
    done
    waitForDone "$round" "$WORKERS"

    # Node B's unlinks and node A's placements start together; node A holds until its own marker.
    pids_a=()
    touch "${WORK}/go-${round}-b"
    for idx in $(seq 1 "$WORKERS"); do
        worker "$MOUNT_A" "a${idx}" "$round" "go-${round}-a" &
        pids_a+=($!)
    done
    for pid in "${pids_b[@]}"; do
        wait "$pid" || fail "round ${round}: a node B worker failed"
    done
    waitForDone "$round" $((WORKERS * 2))
    checkTable "$round" $((WORKERS * FILES_PER_WORKER))
    touch "${WORK}/go-${round}-a"
    for pid in "${pids_a[@]}"; do
        wait "$pid" || fail "round ${round}: a node A worker failed"
    done
done

if [ "$FAILED" -eq 0 ]; then
    printf 'every check passed\n'
    exit 0
fi
printf '%d checks failed\n' "$FAILED"
exit 1
