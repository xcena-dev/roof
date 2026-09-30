#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# rat_summary_crash -- a node dies between two lines of a metadata write, and its peer repairs the
# RAT's summary line.
#
# The module's crash points make one write stop part-way and leave that node dead: it takes no
# lock, releases none and stops its heartbeat. This kills its daemon too, the way losing the node
# would, and then watches the surviving node's sweep bring the summary line back in step with the
# entries and go on creating, placing and unlinking. The dead node is then mounted again for the
# next point, since a dead mount stays dead.
#
# Runs against the host's two mounts and their daemons, so it is in the lifecycle label and not the
# suite gate. Needs root and a module built with CONFIG_FS_TEST_KNOBS:
#
#   sudo ctest --test-dir <build> -R rat_summary_crash

set -u -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
# shellcheck source=/dev/null
. "${ROOT}/fsname"
# shellcheck source=/dev/null
. "${ROOT}/shell/units.sh"

MODULE="${FS_NAME}"
PARAM_DIR="/sys/module/${MODULE}/parameters"
SYSFS="/sys/fs/${MODULE}"
DEAD_MOUNT="${DEAD_MOUNT:-/mnt/${FS_NAME}}"
LIVE_MOUNT="${LIVE_MOUNT:-/mnt/${FS_NAME}2}"

# Past the heartbeat timeout and two GC_INTERVAL_MS sweeps: the survivor has to read the dead node
# as gone, become the admin, and then repair on a sweep of its own.
RECOVERY_WAIT_S="${RECOVERY_WAIT_S:-90}"

FAILED=0
pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; FAILED=$((FAILED + 1)); }
note() { printf '     %s\n' "$*"; }
skip() { printf 'SKIP %s\n' "$*"; exit 77; }

[ "$(id -u)" -eq 0 ] || skip "needs root for the module parameter and the units"
[ -e "${PARAM_DIR}/region_inject_crash_point" ] || skip "the module has no crash points (CONFIG_FS_TEST_KNOBS)"
for point in "$DEAD_MOUNT" "$LIVE_MOUNT"; do
    mountpoint -q "$point" || skip "no mount at ${point}"
done

DEAD_NODE="$(nodeOfMount "$DEAD_MOUNT")"
LIVE_NODE="$(nodeOfMount "$LIVE_MOUNT")"
[ -n "$DEAD_NODE" ] && [ -n "$LIVE_NODE" ] || skip "a mount reports no node id"

# World-writable: the file operations run as the account that called sudo and leave their markers
# here.
WORK="$(mktemp -d)"
chmod 0777 "$WORK"
trap 'rm -rf "$WORK"' EXIT

mapOf() { cat "${SYSFS}/node$1/test/rat_map"; }

# The file operations run as whoever called sudo: the kernel attests the real uid, and root has no
# identity rule, so a create by root is refused before it reaches any crash point.
asUser()
{
    if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
        runuser -u "$SUDO_USER" -- "$@"
    else
        "$@"
    fi
}

# True when the summary line reads as the entries do and no placement is in flight.
mapInStep()
{
    local text
    text="$(mapOf "$1")" || return 1
    local placements
    placements="$(sed -n 's/^placements=\([0-9]*\).*/\1/p' <<<"$text")"
    [ $((placements % 2)) -eq 0 ] || return 1
    ! grep -E '^word[0-9]+ line=([0-9a-f]+) entries=' <<<"$text" |
        awk '{ split($2, a, "="); split($3, b, "="); if (a[2] != b[2]) found = 1 } END { exit !found }'
}

waitForRepair()
{
    local deadline=$((SECONDS + RECOVERY_WAIT_S))
    while [ "$SECONDS" -lt "$deadline" ]; do
        mapInStep "$LIVE_NODE" && return 0
        sleep 2
    done
    return 1
}

# Until the survivor's readdir no longer lists @1, which is the entry having gone back to FREE.
waitForReclaim()
{
    local deadline=$((SECONDS + RECOVERY_WAIT_S))
    while [ "$SECONDS" -lt "$deadline" ]; do
        asUser ls "$LIVE_MOUNT" | grep -q "$1\$" || return 0
        sleep 2
    done
    return 1
}

# How many entries read taken, off the entries themselves and not the line.
takenCount()
{
    mapOf "$1" | sed -n 's/^word[0-9]* line=[0-9a-f]* entries=\([0-9a-f]*\)$/\1/p' |
        python3 -c 'import sys; print(sum(bin(int(w, 16)).count("1") for w in sys.stdin.read().split()))'
}

waitForTakenCount()
{
    local deadline=$((SECONDS + RECOVERY_WAIT_S))
    while [ "$SECONDS" -lt "$deadline" ]; do
        [ "$(takenCount "$LIVE_NODE")" -le "$1" ] && return 0
        sleep 2
    done
    return 1
}

# The table filled to its last slot, then one slot freed by a node that dies before it clears the
# bit. Every bit now reads taken, and the create that follows has to find the free entry by reading
# the entries themselves, which is the allocator's last resort.
runFullTable()
{
    local mark
    mark=$(($(dmesg | wc -l) + 1))
    local free_slots=$((256 - $(takenCount "$LIVE_NODE")))

    # One process owns every filler, so the unlink that dies comes from the process that created it.
    asUser python3 - "$DEAD_MOUNT" "$free_slots" "${PARAM_DIR}/region_inject_crash_point" "$WORK" <<'EOF' &
import os, sys, time
mount, count, knob, work = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
names = [f"{mount}/summary-crash-fill-{idx}" for idx in range(count)]
for name in names:
    os.close(os.open(name, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_CLOEXEC, 0o644))
open(f"{work}/filled", "w").close()
while not os.path.exists(f"{work}/armed"):
    time.sleep(0.05)
try:
    os.unlink(names[-1])
except OSError:
    pass
EOF
    local filler=$!
    local deadline=$((SECONDS + 60))
    while [ ! -e "${WORK}/filled" ]; do
        [ "$SECONDS" -lt "$deadline" ] || { fail "full: the table did not fill in 60s"; return; }
        sleep 0.2
    done
    if [ "$(takenCount "$LIVE_NODE")" -eq 256 ]; then
        pass "full: every slot reads taken"
    else
        fail "full: $(takenCount "$LIVE_NODE") slots read taken, not 256"
    fi

    printf '4' >"${PARAM_DIR}/region_inject_crash_point"
    touch "${WORK}/armed"
    wait "$filler"
    if dmesg | tail -n +"$mark" | grep -q "died at crash point 4"; then
        pass "full: node ${DEAD_NODE} died with the last slot freed and its bit still set"
    else
        fail "full: the crash point did not fire"
        printf '0' >"${PARAM_DIR}/region_inject_crash_point"
        return
    fi

    # The dead node's daemon still holds the turn its unlink died with, so it goes before the create,
    # and the survivor's sweep is paused so no repair reaches the bit while the turn comes back.
    pauseGc "$LIVE_NODE" 1
    killDeadDaemon
    if mapInStep "$LIVE_NODE"; then
        fail "full: the bit read repaired before the create"
    elif metaOps "${LIVE_MOUNT}/summary-crash-full" "unlink" "$RECOVERY_WAIT_S"; then
        pass "full: node ${LIVE_NODE} created into the one free slot behind a taken bit"
    else
        fail "full: node ${LIVE_NODE} could not create with every bit set"
    fi
    pauseGc "$LIVE_NODE" 0

    if waitForTakenCount 1; then
        pass "full: node ${LIVE_NODE} reclaimed the dead node's fillers"
    else
        fail "full: $(takenCount "$LIVE_NODE") slots still read taken after ${RECOVERY_WAIT_S}s"
    fi
    reviveDeadNode && pass "full: node ${DEAD_NODE} is back"
}

# Both mounts trust the summary again, which is what a sweep's repair leaves behind.
waitForTrust()
{
    local deadline=$((SECONDS + RECOVERY_WAIT_S))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if mapOf "$DEAD_NODE" | grep -q 'trusted=1' && mapOf "$LIVE_NODE" | grep -q 'trusted=1' &&
            mapInStep "$LIVE_NODE"; then
            return 0
        fi
        sleep 2
    done
    return 1
}

# A mount that finds the summary line zeroed, as a device formatted before the line existed shows
# it: it must say so, search the entries meanwhile, and trust the line again once a sweep repairs it.
runBlankSummary()
{
    local mark
    mark=$(($(dmesg | wc -l) + 1))
    local unit
    unit="$(unitNameFor "$DEAD_MOUNT")"

    printf '1' >"${PARAM_DIR}/region_inject_blank_summary"
    systemctl stop "${DAEMON_NAME}@${DEAD_NODE}.service"
    timeout 60 systemctl stop "$unit" || { fail "blank: the mount did not come down"; return; }
    systemctl reset-failed "${DAEMON_NAME}@${DEAD_NODE}.service" 2>/dev/null
    systemctl start "$unit" || { fail "blank: the mount did not come back"; return; }
    systemctl start "${DAEMON_NAME}@${DEAD_NODE}.service"
    sleep 2

    if dmesg | tail -n +"$mark" | grep -q "RAT summary disagrees"; then
        pass "blank: the mount noticed the zeroed summary"
    else
        fail "blank: the mount did not notice the zeroed summary"
    fi
    if mapOf "$DEAD_NODE" | grep -q 'trusted=0'; then
        pass "blank: node ${DEAD_NODE} searches the entries meanwhile"
    else
        fail "blank: node ${DEAD_NODE} trusts a zeroed summary"
    fi
    if metaOps "${DEAD_MOUNT}/summary-crash-blank" "place unlink"; then
        pass "blank: node ${DEAD_NODE} creates, places and unlinks off the entries"
    else
        fail "blank: node ${DEAD_NODE} cannot write metadata off the entries"
    fi
    if waitForTrust; then
        pass "blank: a sweep repaired the line and both nodes trust it again"
    else
        fail "blank: the line was not repaired within ${RECOVERY_WAIT_S}s"
    fi
    note "$(mapOf "$DEAD_NODE" | tr '\n' ' ')"
}

# The dead node's mount comes down and up again, and its heartbeat freeze is lifted, so the next
# point has a live node to kill.
reviveDeadNode()
{
    local unit
    unit="$(unitNameFor "$DEAD_MOUNT")"
    timeout 60 systemctl stop "$unit" || { fail "the dead mount did not come down"; return 1; }
    # A start-limit hit is the one failed state a start does not get past, and the mount helper's
    # own start of the unit would then fail on it.
    systemctl reset-failed "${DAEMON_NAME}@${DEAD_NODE}.service" 2>/dev/null
    systemctl start "$unit" || { fail "the dead mount did not come back"; return 1; }
    printf '%s 0' "$DEAD_NODE" >"${SYSFS}/test/freeze_heartbeat"
    systemctl start "${DAEMON_NAME}@${DEAD_NODE}.service" || { fail "the dead node's daemon did not start"; return 1; }
    sleep 2
    mountpoint -q "$DEAD_MOUNT"
}

# One process for every step on a name: a region's owner is the process that created it, so a
# placement or an unlink from another process is refused. @2 names which steps follow the create.
# @3 is how many seconds the create may keep answering EAGAIN, which is the turn not yet back.
metaOps()
{
    asUser python3 - "$1" "$2" "${3:-0}" <<'EOF'
import os, sys, time
path, steps, patience = sys.argv[1], sys.argv[2], float(sys.argv[3])
deadline = time.monotonic() + patience
while True:
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_CLOEXEC, 0o644)
        break
    except BlockingIOError:
        if time.monotonic() >= deadline:
            raise
        time.sleep(0.2)
if "place" in steps:
    os.ftruncate(fd, 4 << 20)
os.close(fd)
if "unlink" in steps:
    os.unlink(path)
EOF
}

liveNodeWorks()
{
    metaOps "${LIVE_MOUNT}/summary-crash-live-$1" "place unlink"
}

# Killed, then stopped before RestartSec brings it back: a node that lost power runs no daemon.
# Node @1's sweep, held (@2 = 1) or released (@2 = 0). Its ticks go on, so it still sees a dead peer.
pauseGc()
{
    printf '%s:%s' "$1" "$2" >"${SYSFS}/debug/gc_pause"
}

killDeadDaemon()
{
    local unit="${DAEMON_NAME}@${DEAD_NODE}.service"
    systemctl kill -s KILL "$unit"
    systemctl stop "$unit"
}

runPoint()
{
    local point="$1" what="$2"
    local mark
    mark=$(($(dmesg | wc -l) + 1))
    local victim="${DEAD_MOUNT}/summary-crash-${point}"
    local taken_before
    taken_before="$(takenCount "$LIVE_NODE")"

    printf '%s' "$point" >"${PARAM_DIR}/region_inject_crash_point"
    case "$point" in
    1 | 5 | 7)
        metaOps "$victim" "" 2>/dev/null && fail "P${point}: the create that died reported success"
        ;;
    2 | 3)
        metaOps "$victim" "place" 2>/dev/null && fail "P${point}: the placement that died reported success"
        ;;
    4 | 6 | 8)
        metaOps "$victim" "unlink" 2>/dev/null
        ;;
    esac

    if dmesg | tail -n +"$mark" | grep -q "died at crash point ${point}"; then
        pass "P${point}: node ${DEAD_NODE} died ${what}"
    else
        fail "P${point}: the crash point did not fire"
        printf '0' >"${PARAM_DIR}/region_inject_crash_point"
        return
    fi

    # Points 5 and 6 leave the summary in step: what they leave is a taken entry with no name in
    # the index, which readdir still lists off the entry's own name line until a sweep reclaims it.
    # Points 7 and 8 leave a transient entry, which readdir never lists, so it shows only as one
    # more taken entry than before.
    if [ "$point" -eq 5 ] || [ "$point" -eq 6 ]; then
        if asUser ls "$LIVE_MOUNT" | grep -q "summary-crash-${point}\$"; then
            pass "P${point}: the entry stands without its name in the index"
        else
            fail "P${point}: the entry is not there to reclaim"
        fi
    elif [ "$point" -eq 7 ] || [ "$point" -eq 8 ]; then
        if [ "$(takenCount "$LIVE_NODE")" -eq $((taken_before + 1)) ]; then
            pass "P${point}: the transient entry stands"
        else
            fail "P${point}: $(takenCount "$LIVE_NODE") taken entries, expected $((taken_before + 1))"
        fi
    elif mapInStep "$DEAD_NODE"; then
        fail "P${point}: the summary still reads in step with the entries"
    else
        pass "P${point}: the summary is out of step, as the death left it"
    fi
    note "$(mapOf "$DEAD_NODE" | tr '\n' ' ')"

    killDeadDaemon

    if [ "$point" -eq 5 ] || [ "$point" -eq 6 ]; then
        if waitForReclaim "summary-crash-${point}"; then
            pass "P${point}: node ${LIVE_NODE} reclaimed the nameless entry"
        else
            fail "P${point}: the nameless entry stood for ${RECOVERY_WAIT_S}s"
        fi
        if mapInStep "$LIVE_NODE"; then
            pass "P${point}: the summary reads in step after the reclaim"
        else
            fail "P${point}: the reclaim left the summary out of step"
        fi
    elif [ "$point" -eq 7 ] || [ "$point" -eq 8 ]; then
        # No name to watch for, so the reclaim shows as the taken count falling back to what it
        # was before the operation began.
        if waitForTakenCount "$taken_before"; then
            pass "P${point}: node ${LIVE_NODE} reclaimed the transient entry"
        else
            fail "P${point}: the transient entry stood for ${RECOVERY_WAIT_S}s"
        fi
        if mapInStep "$LIVE_NODE"; then
            pass "P${point}: the summary reads in step after the reclaim"
        else
            fail "P${point}: the reclaim left the summary out of step"
        fi
    else
        if waitForRepair; then
            pass "P${point}: node ${LIVE_NODE} brought the summary back in step"
        else
            fail "P${point}: the summary stayed out of step for ${RECOVERY_WAIT_S}s"
        fi
        if dmesg | tail -n +"$mark" | grep -q "RAT summary repaired"; then
            pass "P${point}: the sweep logged its repair"
        else
            note "P${point}: no repair line; the allocator or a free may have closed the gap first"
        fi
    fi
    note "$(mapOf "$LIVE_NODE" | tr '\n' ' ')"

    if liveNodeWorks "$point"; then
        pass "P${point}: node ${LIVE_NODE} creates, places and unlinks"
    else
        fail "P${point}: node ${LIVE_NODE} cannot write metadata"
    fi

    reviveDeadNode && pass "P${point}: node ${DEAD_NODE} is back"
}

# ONLY="7 8 full" runs those cases alone; empty runs them all.
ONLY="${ONLY:-}"
wants() { [ -z "$ONLY" ] || [[ " ${ONLY} " == *" $1 "* ]]; }

wants 1 && runPoint 1 "with a slot taken and its bit clear"
wants 2 && runPoint 2 "with the count odd and no extent written"
wants 3 && runPoint 3 "with an extent written and the count odd"
wants 4 && runPoint 4 "with a slot freed and its bit still set"
wants 5 && runPoint 5 "with a slot taken and marked but no name in the index"
wants 6 && runPoint 6 "with the name out of the index and the slot still taken"
wants 7 && runPoint 7 "with the entry ALLOCATING and nothing else written"
wants 8 && runPoint 8 "with the entry DELETING and its lines intact"
wants blank && runBlankSummary
wants full && runFullTable

if [ "$FAILED" -eq 0 ]; then
    printf 'every check passed\n'
    exit 0
fi
printf '%d checks failed\n' "$FAILED"
exit 1
