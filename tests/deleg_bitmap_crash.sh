#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# deleg_bitmap_crash -- a node dies between a delegation row and its taken bit, and the survivor's
# scans and next grant cope with the row left behind: a set bit over an EMPTY row.
#
# Needs root and a module built with CONFIG_FS_TEST_KNOBS, on the host's two mounts:
#
#   sudo ctest --test-dir <build> -R deleg_bitmap_crash

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
MAPPER="${MAPPER:-${ROOT}/build/tests/chaos_mapper}"

# Past the survivor's heartbeat verdict on the dead node and the sweeps that act on it.
RECOVERY_WAIT_S="${RECOVERY_WAIT_S:-90}"
SWEEPS_TO_WAIT=2
SWEEP_WAIT_S=60
MAPPER_LIMIT_S=30

# How often a wait polls, how often a refused umount is retried, and how long a revived node settles.
POLL_S=1
OWNER_POLL_S=0.2
UMOUNT_RETRIES=10
SETTLE_S=2

CRASH_GRANT=9
CRASH_REVOKE=10

# Permission bits as the library names them. The default omits READ, so a stranger's read map has
# no standing and asks the helper, and carries ADMIN so a node that did not place the region can
# grant and revoke on it.
PERM_READ=0x1
PERM_WRITE=0x2
PERM_DEFAULT=0x2e

# The lock region's rat entry, whose taken bits stay set for every node slot.
LOCK_RID=0
LOCK_ROWS_MASK=255

ROLE_GROUP=crashgrp
ROLE_NAME=crashrole

# Well clear of any account this host has, so a row here names nobody who could then use it. Each
# case takes its uids from a block of its own, and the grants that reuse a stale row from one more.
FIRST_UID=62000
GRANT_UIDS=$((FIRST_UID))
REVOKE_UIDS=$((FIRST_UID + 10))
CALLER_UIDS=$((FIRST_UID + 20))
ROLE_UIDS=$((FIRST_UID + 30))
REUSE_UIDS=$((FIRST_UID + 90))
UID_NOBODY=62999

FAILED=0
pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; FAILED=$((FAILED + 1)); }
note() { printf '     %s\n' "$*"; }
skip() { printf 'SKIP %s\n' "$*"; exit 77; }

[ "$(id -u)" -eq 0 ] || skip "needs root for the module parameter and the units"
[ -e "${PARAM_DIR}/region_inject_crash_point" ] || skip "the module has no crash points (CONFIG_FS_TEST_KNOBS)"
[ -e "${SYSFS}/deleg_info" ] || skip "the module has no deleg_info"
[ -x "$MAPPER" ] || skip "no chaos_mapper at ${MAPPER}"
for point in "$DEAD_MOUNT" "$LIVE_MOUNT"; do
    mountpoint -q "$point" || skip "no mount at ${point}"
done

DEAD_NODE="$(nodeOfMount "$DEAD_MOUNT")"
LIVE_NODE="$(nodeOfMount "$LIVE_MOUNT")"
[ -n "$DEAD_NODE" ] && [ -n "$LIVE_NODE" ] || skip "a mount reports no node id"
[ -e "${SYSFS}/node${LIVE_NODE}/test/deleg_reads" ] || skip "the module has no deleg_reads"
[ -e "${SYSFS}/gc_status" ] || skip "the module has no gc_status"

# The kernel attests the real uid, and root has no identity rule.
asUser()
{
    if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
        runuser -u "$SUDO_USER" -- "$@"
    else
        "$@"
    fi
}

USER_ID="$(asUser id -u)"

# One call of the mapper on the mount @1 against the region @2, in the mode @3... Bounded, since a
# mapping that was not refused would be held for minutes.
mapperOn() { asUser timeout "$MAPPER_LIMIT_S" "$MAPPER" "$1" "$2" "${@:3}" >/dev/null 2>&1; }

regionId() { awk -v want="$1" '$7 == want { print $1 }' "${SYSFS}/region_info"; }

# The Deleg_taken column of perm_info for region @1, as a number.
takenMask()
{
    local text
    text="$(awk -F'\t' -v want="$1" '$1 == want { print $3 }' "${SYSFS}/perm_info")"
    printf '%d' "${text:-0}"
}

bitCount()
{
    local val=$(($1)) total=0
    while [ "$val" -ne 0 ]; do
        total=$((total + (val & 1)))
        val=$((val >> 1))
    done
    printf '%d' "$total"
}

# deleg_info lists only the rows that read ACTIVE, so a row missing from it reads EMPTY.
delegInfo()
{
    printf '%s' "$1" >"${SYSFS}/deleg_info"
    cat "${SYSFS}/deleg_info"
}

activeRows() { delegInfo "$1" | sed -n 's/^ *deleg\[\([0-9]*\)\]:.*/\1/p'; }

isRowActive() { activeRows "$1" | grep -qx "$2"; }

# Reads the counter @2 of node @1: check, role, slot, revoke or sweep.
readCount() { sed -n "s/.*\\b$2=\\([0-9]*\\).*/\\1/p" "${SYSFS}/node$1/test/deleg_reads"; }

turnsTaken() { sed -n 's/.*taken=\([0-9]*\).*/\1/p' "${SYSFS}/node$1/test/meta_lock"; }

# gc_status is one line of "node<id>:<state> epoch=<n>" fields.
gcEpoch() { sed -n "1s/.*node$1:[^ ]* epoch=\\([0-9]*\\).*/\\1/p" "${SYSFS}/gc_status"; }

# No ACTIVE row may stand under a clear bit, in any region that stands.
checkNoRowUnderClearBit()
{
    local label="$1" rid mask row bad=0
    for rid in $(awk -F'\t' 'NR > 1 { print $1 }' "${SYSFS}/perm_info"); do
        mask="$(takenMask "$rid")"
        for row in $(activeRows "$rid"); do
            if [ $(((mask >> row) & 1)) -eq 0 ]; then
                note "region ${rid} row ${row} is ACTIVE under a clear bit (mask ${mask})"
                bad=1
            fi
        done
    done
    if [ "$bad" -eq 0 ]; then
        pass "${label}: no ACTIVE row stands under a clear bit"
    else
        fail "${label}: an ACTIVE row stands under a clear bit"
    fi
}

# The lock region marks every node's slot at format and never clears one, whatever recovery empties.
checkLockRegionMarks()
{
    local label="$1" mask
    mask="$(takenMask "$LOCK_RID")"
    if [ "$mask" -eq "$LOCK_ROWS_MASK" ]; then
        pass "${label}: the lock region still marks every node slot"
    else
        fail "${label}: the lock region mask reads ${mask}, not ${LOCK_ROWS_MASK}"
    fi
}

killDeadDaemon()
{
    local unit="${DAEMON_NAME}@${DEAD_NODE}.service"
    systemctl kill -s KILL "$unit"
    systemctl stop "$unit"
}

# The dead mount comes down and up again and its heartbeat freeze is lifted, so the next point has a
# live node to kill.
reviveDeadNode()
{
    local unit tries=0
    unit="$(unitNameFor "$DEAD_MOUNT")"
    # A process stopped just before can hold its descriptor a moment longer, which umount refuses.
    until timeout 60 systemctl stop "$unit"; do
        tries=$((tries + 1))
        [ "$tries" -lt "$UMOUNT_RETRIES" ] || { fail "the dead mount did not come down"; return 1; }
        sleep "$POLL_S"
    done
    systemctl reset-failed "${DAEMON_NAME}@${DEAD_NODE}.service" 2>/dev/null
    systemctl start "$unit" || { fail "the dead mount did not come back"; return 1; }
    # The survivor holds the old id until no row names it, so the mount can come back under another.
    DEAD_NODE="$(nodeOfMount "$DEAD_MOUNT")"
    [ -n "$DEAD_NODE" ] || { fail "the dead mount came back with no node id"; return 1; }
    systemctl start "${DAEMON_NAME}@${DEAD_NODE}.service" || { fail "the dead node's daemon did not start"; return 1; }
    sleep "$SETTLE_S"
    mountpoint -q "$DEAD_MOUNT"
}

# Until the survivor's epoch is @1 sweeps past @2.
waitForSweeps()
{
    local deadline=$((SECONDS + SWEEP_WAIT_S))
    while [ "$SECONDS" -lt "$deadline" ]; do
        [ "$(gcEpoch "$LIVE_NODE")" -ge $(($2 + $1)) ] && return 0
        sleep "$POLL_S"
    done
    return 1
}

# Until the dead node's row in the lock region reads EMPTY, which is the survivor's recovery of it.
waitForLockRowDropped()
{
    local deadline=$((SECONDS + RECOVERY_WAIT_S))
    while [ "$SECONDS" -lt "$deadline" ]; do
        isRowActive "$LOCK_RID" $((DEAD_NODE - 1)) || return 0
        sleep "$POLL_S"
    done
    return 1
}

# The region's owner, which lives for the whole case: a sweep reclaims a region whose creating
# process is gone, and with it the rows the case reads.
OWNER_PID=""
OWNER_LOG="$(mktemp)"

# A reader the dead node holds across its death, for the case that needs a process row there.
HOLDER_PID=""

trap '[ -z "$OWNER_PID" ] || stopProcess "$OWNER_PID"; [ -z "$HOLDER_PID" ] || stopProcess "$HOLDER_PID"; rm -f "$OWNER_LOG"' EXIT

# A process started through runuser is its child, so both are told to go.
stopProcess()
{
    pkill -TERM -P "$1" 2>/dev/null
    kill "$1" 2>/dev/null
    wait "$1" 2>/dev/null
}

# The owner goes first, since its mapping and descriptor would keep the extent past the unlink.
dropRegion()
{
    if [ -n "$OWNER_PID" ]; then
        stopProcess "$OWNER_PID"
        OWNER_PID=""
    fi
    asUser rm -f "${LIVE_MOUNT}/$1" 2>/dev/null
}

# A region the live node places and opens to every node, then keeps mapped. Later calls from any
# node stand on the default alone.
makeRegion()
{
    dropRegion "$1"
    : >"$OWNER_LOG"
    asUser "$MAPPER" "$LIVE_MOUNT" "$1" --own "$PERM_DEFAULT" >"$OWNER_LOG" 2>&1 &
    OWNER_PID=$!
    local deadline=$((SECONDS + MAPPER_LIMIT_S))
    until grep -qx mapped "$OWNER_LOG"; do
        [ "$SECONDS" -lt "$deadline" ] && kill -0 "$OWNER_PID" 2>/dev/null || return 1
        sleep "$OWNER_POLL_S"
    done
}

# One process for the create, the placement and the unlink, as a region's owner is the process that
# created it. @2 is the longest the create may keep answering EAGAIN.
metaOps()
{
    asUser python3 - "$1" "$2" <<'EOF'
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
os.unlink(path)
EOF
}

# Arms crash point @1 and runs the mapper call @4... on the dead node's mount against region @3.
# Succeeds when the node died there.
crashOnDeadNode()
{
    local point="$1" label="$2" name="$3" mark
    mark=$(($(dmesg | wc -l) + 1))
    printf '%s' "$point" >"${PARAM_DIR}/region_inject_crash_point"
    mapperOn "$DEAD_MOUNT" "$name" "${@:4}" && fail "${label}: the call that died reported success"
    if dmesg | tail -n +"$mark" | grep -q "died at crash point ${point}"; then
        pass "${label}: node ${DEAD_NODE} died at crash point ${point}"
        return 0
    fi
    fail "${label}: the crash point did not fire"
    printf '0' >"${PARAM_DIR}/region_inject_crash_point"
    return 1
}

# The live node grants a fresh uid on region @1 and says what its two scans read. Neither scan stops
# early: no row the caller matches reaches every permission bit, and no row names the new uid, so
# each scan reads every taken row and the count is exactly the bits set.
grantAndCountReads()
{
    local name="$1" uid="$2" before_check before_slot
    before_check="$(readCount "$LIVE_NODE" check)"
    before_slot="$(readCount "$LIVE_NODE" slot)"
    mapperOn "$LIVE_MOUNT" "$name" --grant "$PERM_READ" "$uid" || return 1
    READ_CHECK=$(($(readCount "$LIVE_NODE" check) - before_check))
    READ_SLOT=$(($(readCount "$LIVE_NODE" slot) - before_slot))
}

# What the death left on region @2: mask @3 with the rows in @4 EMPTY under their bits.
checkDeathLeft()
{
    local label="$1" rid="$2" want_mask="$3" stale_rows="$4" stale
    if [ "$(takenMask "$rid")" -eq "$want_mask" ]; then
        pass "${label}: the mask reads ${want_mask} as the death left it"
    else
        fail "${label}: the mask reads $(takenMask "$rid"), not ${want_mask}"
    fi
    for stale in $stale_rows; do
        if isRowActive "$rid" "$stale"; then
            fail "${label}: row ${stale} reads ACTIVE"
        else
            pass "${label}: row ${stale} reads EMPTY under its set bit"
        fi
    done
    checkNoRowUnderClearBit "${label} at the death"
}

# The survivor recovers the dead node, and neither the recovery nor its sweeps touch the stale rows.
checkRecoveryKeepsStaleRows()
{
    local label="$1" rid="$2" want_mask="$3" stale_rows="$4" epoch stale unchanged=1
    epoch="$(gcEpoch "$LIVE_NODE")"
    killDeadDaemon
    if waitForSweeps "$SWEEPS_TO_WAIT" "$epoch"; then
        pass "${label}: node ${LIVE_NODE} swept ${SWEEPS_TO_WAIT} times"
    else
        fail "${label}: node ${LIVE_NODE} did not sweep in ${SWEEP_WAIT_S}s"
    fi
    if waitForLockRowDropped; then
        pass "${label}: node ${LIVE_NODE} dropped node ${DEAD_NODE}'s row in the lock region"
    else
        fail "${label}: node ${DEAD_NODE}'s lock region row stood for ${RECOVERY_WAIT_S}s"
    fi
    checkLockRegionMarks "${label} after the recovery"

    [ "$(takenMask "$rid")" -eq "$want_mask" ] || unchanged=0
    for stale in $stale_rows; do
        ! isRowActive "$rid" "$stale" || unchanged=0
    done
    if [ "$unchanged" -eq 1 ]; then
        pass "${label}: the sweeps left the stale rows and their bits alone"
    else
        fail "${label}: a sweep changed a stale row or the mask"
    fi
}

# A revoke naming nobody finds nothing and reads every taken row of mask @3 once, stale ones included.
checkEmptyRevokeReads()
{
    local label="$1" name="$2" bits before read_revoke
    bits="$(bitCount "$3")"
    before="$(readCount "$LIVE_NODE" revoke)"
    mapperOn "$LIVE_MOUNT" "$name" --revoke "$UID_NOBODY" && fail "${label}: a revoke of no row succeeded"
    read_revoke=$(($(readCount "$LIVE_NODE" revoke) - before))
    if [ "$read_revoke" -eq "$bits" ]; then
        pass "${label}: the revoke scan read ${bits} rows, each taken row once"
    else
        fail "${label}: the revoke scan read ${read_revoke} rows, not ${bits}"
    fi
}

# Each next grant reads every taken row in both scans and fills the lowest stale slot, mask unchanged.
checkGrantsReuseStaleRows()
{
    local label="$1" rid="$2" want_mask="$3" stale_rows="$4" name="$5" bits step=0 stale uid
    bits="$(bitCount "$want_mask")"
    for stale in $stale_rows; do
        uid=$((REUSE_UIDS + step))
        if ! grantAndCountReads "$name" "$uid"; then
            fail "${label}: the live node's grant ${step} was refused"
            break
        fi
        if [ "$READ_CHECK" -eq "$bits" ]; then
            pass "${label}: grant ${step}: the permission check read ${bits} rows"
        else
            fail "${label}: grant ${step}: the permission check read ${READ_CHECK} rows, not ${bits}"
        fi
        if [ "$READ_SLOT" -eq "$bits" ]; then
            pass "${label}: grant ${step}: the slot search read ${bits} rows"
        else
            fail "${label}: grant ${step}: the slot search read ${READ_SLOT} rows, not ${bits}"
        fi
        if [ "$(takenMask "$rid")" -eq "$want_mask" ]; then
            pass "${label}: grant ${step} took no new bit"
        else
            fail "${label}: grant ${step}: the mask reads $(takenMask "$rid"), not ${want_mask}"
        fi
        if isRowActive "$rid" "$stale" && delegInfo "$rid" | grep -q "deleg\\[${stale}\\].*uid=${uid}"; then
            pass "${label}: grant ${step} reused row ${stale}"
        else
            fail "${label}: grant ${step} did not land in row ${stale}"
        fi
        step=$((step + 1))
    done
    checkNoRowUnderClearBit "${label} after the reuse"
}

# After the remount the lock region still marks every slot, the node's own row is back, and its
# daemon can take a turn.
checkRemountedNode()
{
    local label="$1" before after
    checkLockRegionMarks "${label} after the remount"
    if isRowActive "$LOCK_RID" $((DEAD_NODE - 1)); then
        pass "${label}: node ${DEAD_NODE} wrote its lock region row again"
    else
        fail "${label}: node ${DEAD_NODE}'s lock region row is not back"
    fi
    before="$(turnsTaken "$DEAD_NODE")"
    if metaOps "${DEAD_MOUNT}/deleg-crash-turn-${label}" "$RECOVERY_WAIT_S"; then
        after="$(turnsTaken "$DEAD_NODE")"
        if [ "$after" -gt "$before" ]; then
            pass "${label}: node ${DEAD_NODE} took a turn for a create and a placement"
        else
            fail "${label}: node ${DEAD_NODE} wrote metadata without taking a turn"
        fi
    else
        fail "${label}: node ${DEAD_NODE} could not create after the remount"
    fi
}

# The live node writes two account rows, and the dead node dies granting a third.
setupGrant()
{
    mapperOn "$LIVE_MOUNT" "$1" --grant "$PERM_READ" "$((GRANT_UIDS + 1))" &&
        mapperOn "$LIVE_MOUNT" "$1" --grant "$PERM_READ" "$((GRANT_UIDS + 2))"
}

# The live node writes one row. A process on the dead node then maps the region with no row of its
# own, so the kernel asks the helper and dies marking the process row the answer earns.
setupCaller() { mapperOn "$LIVE_MOUNT" "$1" --grant "$PERM_READ" "$((CALLER_UIDS + 1))"; }

# The dead node writes an account row of its own beside the live node's, then dies revoking it.
setupRevoke()
{
    mapperOn "$LIVE_MOUNT" "$1" --grant "$PERM_READ" "$((REVOKE_UIDS + 1))" &&
        mapperOn "$DEAD_MOUNT" "$1" --grant "$PERM_WRITE" "$USER_ID"
}

# The dead node writes a role row, which its ADMIN default lets it, then dies taking it back.
setupRole()
{
    mapperOn "$LIVE_MOUNT" "$1" --grant "$PERM_READ" "$((ROLE_UIDS + 1))" &&
        mapperOn "$DEAD_MOUNT" "$1" --grant-role "$PERM_WRITE" "$ROLE_GROUP" "$ROLE_NAME"
}

# A read map the dead node holds writes a process row, and the dead node's account row joins it, so
# one revoke of that account empties both. 77 when the helper there does not admit the map.
setupTwo()
{
    local name="$1" rid="$2" deadline=$((SECONDS + MAPPER_LIMIT_S))
    asUser "$MAPPER" "$DEAD_MOUNT" "$name" --hold-read >/dev/null 2>&1 &
    HOLDER_PID=$!
    until delegInfo "$rid" | grep -q "process.*node=${DEAD_NODE}"; do
        [ "$SECONDS" -lt "$deadline" ] || return 77
        sleep "$POLL_S"
    done
    mapperOn "$DEAD_MOUNT" "$name" --grant "$PERM_WRITE" "$USER_ID"
}

stopHolder()
{
    [ -z "$HOLDER_PID" ] || stopProcess "$HOLDER_PID"
    HOLDER_PID=""
}

# One case: @setup writes the rows (mask @3), the dead node dies at point @6 in the mapper call the
# rest makes, and the death leaves mask @4 with the rows in @5 EMPTY under their bits.
runCrashCase()
{
    local label="$1" setup="$2" mask_before="$3" mask_after="$4" stale_rows="$5" point="$6"
    shift 6
    local name="deleg-crash-${label}" rid status crashed=0
    if ! makeRegion "$name" || ! rid="$(regionId "$name")" || [ -z "$rid" ]; then
        fail "${label}: the region could not be made"
        dropRegion "$name"
        return
    fi

    "$setup" "$name" "$rid"
    status=$?
    if [ "$status" -eq 77 ]; then
        note "${label}: skipped, the helper on node ${DEAD_NODE} does not admit a stranger's read map"
    elif [ "$status" -ne 0 ]; then
        fail "${label}: the rows could not be written"
    else
        if [ "$(takenMask "$rid")" -eq "$mask_before" ]; then
            pass "${label}: the mask reads ${mask_before} before the death"
        else
            fail "${label}: the mask reads $(takenMask "$rid") before the death, not ${mask_before}"
        fi
        crashed=1
        if crashOnDeadNode "$point" "$label" "$name" "$@"; then
            checkDeathLeft "$label" "$rid" "$mask_after" "$stale_rows"
            checkRecoveryKeepsStaleRows "$label" "$rid" "$mask_after" "$stale_rows"
            checkEmptyRevokeReads "$label" "$name" "$mask_after"
            checkGrantsReuseStaleRows "$label" "$rid" "$mask_after" "$stale_rows" "$name"
        fi
    fi

    stopHolder
    if [ "$crashed" -eq 1 ] && reviveDeadNode; then
        pass "${label}: node ${DEAD_NODE} is back"
        checkRemountedNode "$label"
    fi
    dropRegion "$name"
}

# ONLY="grant revoke" runs those cases alone; empty runs them all.
ONLY="${ONLY:-}"
wants() { [ -z "$ONLY" ] || [[ " ${ONLY} " == *" $1 "* ]]; }

wants grant && runCrashCase grant setupGrant 3 7 "2" "$CRASH_GRANT" --grant "$PERM_WRITE" "$((GRANT_UIDS + 3))"
wants caller && runCrashCase caller setupCaller 1 3 "1" "$CRASH_GRANT" --hold-read
wants revoke && runCrashCase revoke setupRevoke 3 3 "1" "$CRASH_REVOKE" --revoke "$USER_ID"
wants role && runCrashCase role setupRole 3 3 "1" "$CRASH_REVOKE" --revoke-role "$ROLE_GROUP" "$ROLE_NAME"
wants two && runCrashCase two setupTwo 3 3 "0 1" "$CRASH_REVOKE" --revoke "$USER_ID"

if [ "$FAILED" -eq 0 ]; then
    printf 'every check passed\n'
    exit 0
fi
printf '%d checks failed\n' "$FAILED"
exit 1
