#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# unmount_held_turn -- an unmount never waits forever on a metadata turn that another writer holds.
#
# live: a create holds its turn past the clean step's wait, which must give up with EAGAIN.
# dead: a node dies mid-placement holding its turn and is unmounted before a peer stakes its slot.
# Needs root, the host's two mounts and a module built with CONFIG_FS_TEST_KNOBS:
#   sudo ctest --test-dir <build> -R unmount_held_turn

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

# Twice the clean step's UNMOUNT_CREATE_WAIT_MS, so the create still holds the turn when it gives up.
HOLD_MS="${HOLD_MS:-20000}"
# Just under UNMOUNT_CREATE_WAIT_MS: an answer sooner is a clean step that never waited.
CLEAN_WAIT_MIN_S=8
# Covers the clean step's wait, the helper's retries and the rest of the umount.
UMOUNT_BOUND_S="${UMOUNT_BOUND_S:-30}"
# Past the heartbeat timeout and two sweeps, and the dead daemon's turn coming back.
RECOVERY_WAIT_S="${RECOVERY_WAIT_S:-90}"

FAILED=0
pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; FAILED=$((FAILED + 1)); }
note() { printf '     %s\n' "$*"; }
skip() { printf 'SKIP %s\n' "$*"; exit 77; }

[ "$(id -u)" -eq 0 ] || skip "needs root for the module parameters and the units"
for knob in region_inject_crash_point meta_inject_create_hold_ms; do
    [ -e "${PARAM_DIR}/${knob}" ] || skip "the module has no ${knob} (CONFIG_FS_TEST_KNOBS)"
done
for point in "$DEAD_MOUNT" "$LIVE_MOUNT"; do
    mountpoint -q "$point" || skip "no mount at ${point}"
done

DEAD_NODE="$(nodeOfMount "$DEAD_MOUNT")"
LIVE_NODE="$(nodeOfMount "$LIVE_MOUNT")"
[ -n "$DEAD_NODE" ] && [ -n "$LIVE_NODE" ] || skip "a mount reports no node id"

WORK="$(mktemp -d)"
chmod 0777 "$WORK"
cleanUp()
{
    printf '0' >"${PARAM_DIR}/meta_inject_create_hold_ms"
    printf '0' >"${PARAM_DIR}/region_inject_crash_point"
    rm -rf "$WORK"
}
trap cleanUp EXIT

# The file operations run as whoever called sudo: the kernel attests the real uid, and root has no
# identity rule, so a create by root is refused before it reaches the turn.
asUser()
{
    if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
        runuser -u "$SUDO_USER" -- "$@"
    else
        "$@"
    fi
}

# Creates @1 and places a region under it from one process. @2 is how many seconds the create may
# keep answering EAGAIN, which is the turn not yet back.
place()
{
    asUser python3 - "$1" "${2:-0}" <<'EOF'
import os, sys, time
path, patience = sys.argv[1], float(sys.argv[2])
deadline = time.monotonic() + patience
while True:
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_CLOEXEC, 0o644)
        break
    except BlockingIOError:
        if time.monotonic() >= deadline:
            raise
        time.sleep(0.2)
os.ftruncate(fd, 4 << 20)
os.close(fd)
EOF
}

# Every task in uninterruptible sleep, which is the hang this test rules out.
stuckTasks() { ps -eo pid,stat,args | awk '$2 ~ /^D/'; }

checkNoStuckTask()
{
    local stuck
    stuck="$(stuckTasks)"
    if [ -z "$stuck" ]; then
        pass "$1: no task is left in uninterruptible sleep"
    else
        fail "$1: tasks in uninterruptible sleep"
        note "$stuck"
    fi
}

# Until the slot of node @1 reads FREE in the survivor's view, which is the last step of a recovery.
waitForSlotFree()
{
    local deadline=$((SECONDS + RECOVERY_WAIT_S))
    while [ "$SECONDS" -lt "$deadline" ]; do
        grep -q "^slot\[$(($1 - 1))\] .* state=0 " "${SYSFS}/node${LIVE_NODE}/test/bootstrap_dump" && return 0
        sleep 2
    done
    return 1
}

# The dead mount comes down and up again with its heartbeat freeze lifted. It can come back under a
# new id, since the survivor holds the old one until no row names it.
reviveDeadNode()
{
    local unit
    unit="$(unitNameFor "$DEAD_MOUNT")"
    if mountpoint -q "$DEAD_MOUNT"; then
        timeout 60 systemctl stop "$unit" || { fail "the dead mount did not come down"; return 1; }
    fi
    systemctl reset-failed "${DAEMON_NAME}@${DEAD_NODE}.service" 2>/dev/null
    systemctl start "$unit" || { fail "the dead mount did not come back"; return 1; }
    DEAD_NODE="$(nodeOfMount "$DEAD_MOUNT")"
    [ -n "$DEAD_NODE" ] || { fail "the dead mount came back with no node id"; return 1; }
    systemctl start "${DAEMON_NAME}@${DEAD_NODE}.service" || { fail "the dead node's daemon did not start"; return 1; }
    sleep 2
    mountpoint -q "$DEAD_MOUNT"
}

# A create holds its turn for HOLD_MS while the clean step runs on the same node. The clean step
# must give up after its own wait and reopen the mount, and the create must still finish.
runLive()
{
    local held="${DEAD_MOUNT}/held-turn-live-$$"
    printf '%s' "$HOLD_MS" >"${PARAM_DIR}/meta_inject_create_hold_ms"
    place "$held" &
    local holder=$!

    # The knob reads 0 once the create has taken it, which is the create inside its hold.
    local deadline=$((SECONDS + 10))
    until [ "$(cat "${PARAM_DIR}/meta_inject_create_hold_ms")" = 0 ]; do
        [ "$SECONDS" -lt "$deadline" ] || { fail "live: no create took the hold"; wait "$holder"; return; }
        sleep 0.1
    done

    local began=$SECONDS refused=0
    printf '1' 2>"${WORK}/prepare.err" >"${SYSFS}/node${DEAD_NODE}/unmount_prepare" || refused=1
    local spent=$((SECONDS - began))
    local hold_s=$((HOLD_MS / 1000))
    if [ "$refused" -eq 0 ]; then
        fail "live: the clean step ran while a create held the turn"
        wait "$holder"
        reviveDeadNode
        return
    fi
    if grep -q 'Resource temporarily unavailable' "${WORK}/prepare.err" &&
        [ "$spent" -ge "$CLEAN_WAIT_MIN_S" ] && [ "$spent" -lt "$hold_s" ]; then
        pass "live: the clean step gave up with EAGAIN after ${spent}s, before the ${hold_s}s hold ended"
    else
        fail "live: the clean step answered '$(cat "${WORK}/prepare.err")' after ${spent}s"
    fi
    checkNoStuckTask live

    if wait "$holder"; then
        pass "live: the held create finished and placed its region"
    else
        fail "live: the held create failed"
    fi
    if place "${DEAD_MOUNT}/held-turn-after-$$"; then
        pass "live: node ${DEAD_NODE} takes a create again after the clean step gave up"
    else
        fail "live: node ${DEAD_NODE} stays closed after the clean step gave up"
    fi
}

# A placement dies holding its turn, and the dead mount is taken down at once, before the survivor
# reads its heartbeat as stopped. Its daemon is left running, as it was when this hung.
runDead()
{
    local mark
    mark=$(($(dmesg | wc -l) + 1))
    printf '2' >"${PARAM_DIR}/region_inject_crash_point"
    place "${DEAD_MOUNT}/held-turn-dead-$$" 2>/dev/null && fail "dead: the placement that died reported success"
    if dmesg | tail -n +"$mark" | grep -q "died at crash point 2"; then
        pass "dead: node ${DEAD_NODE} died holding the turn of a placement"
    else
        fail "dead: the crash point did not fire"
        printf '0' >"${PARAM_DIR}/region_inject_crash_point"
        return
    fi

    local unit began=$SECONDS
    unit="$(unitNameFor "$DEAD_MOUNT")"
    if timeout "$UMOUNT_BOUND_S" systemctl stop "$unit" && ! mountpoint -q "$DEAD_MOUNT"; then
        pass "dead: node ${DEAD_NODE}'s mount came down in $((SECONDS - began))s"
    else
        fail "dead: node ${DEAD_NODE}'s mount did not come down in ${UMOUNT_BOUND_S}s"
    fi
    checkNoStuckTask dead

    if place "${LIVE_MOUNT}/held-turn-survivor-$$" "$RECOVERY_WAIT_S"; then
        pass "dead: node ${LIVE_NODE} creates and places once the dead turn is back"
    else
        fail "dead: node ${LIVE_NODE} cannot write metadata after the death"
    fi
    # Until the survivor frees the slot, so the host is left with no recovery in flight.
    if waitForSlotFree "$DEAD_NODE"; then
        pass "dead: node ${LIVE_NODE} finished recovering node ${DEAD_NODE}"
    else
        fail "dead: node ${DEAD_NODE}'s slot was still held after ${RECOVERY_WAIT_S}s"
    fi
    reviveDeadNode && pass "dead: node ${DEAD_NODE} is back"
}

# ONLY="dead" runs that case alone; empty runs them all.
ONLY="${ONLY:-}"
wants() { [ -z "$ONLY" ] || [[ " ${ONLY} " == *" $1 "* ]]; }

wants live && runLive
wants dead && runDead

if [ "$FAILED" -eq 0 ]; then
    printf 'every check passed\n'
    exit 0
fi
printf '%d checks failed\n' "$FAILED"
exit 1
