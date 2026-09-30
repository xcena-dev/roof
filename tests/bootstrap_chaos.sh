#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# bootstrap_chaos -- the bootstrap paths no case over the library can reach.
#
# Every other case in this directory attaches to a mount somebody else put up. These are about
# putting one up: which node formats, what happens to the one that starts and does not finish,
# what a slot is worth after a graceful umount, and what the mount past the last slot gets. So
# this owns the module and the mount for its whole run, needs root, and takes the host's mounts
# down while it works.
#
# It is not in the suite gate for that reason. Its label keeps it out, and a deliberate run is
#
#   sudo ctest --test-dir <build> -L lifecycle
#
# T1 runs last because it spends most of itself with slot[0] held by a token nobody will release,
# and every other case needs a free table. Its own last step gives slot[0] back, so a run that
# finishes leaves nothing behind. One killed between node A and node B does not: a held slot now
# stays held until a sweep cleans the node it belonged to.

set -u -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
ROOT="$(cd "${HERE}/.." && pwd)"
. "${ROOT}/fsname"
. "${ROOT}/shell/units.sh"

MODULE="${FS_NAME}"
MODULE_KO="${MODULE_KO:-${HERE}/../kernel/build/${FS_NAME}.ko}"
DAX_DEVICE="${DAX_DEVICE:-/dev/dax0.0}"
MNT_BASE="${MNT_BASE:-/mnt/${FS_NAME}-chaos}"
# Built by tests/CMakeLists.txt. T7 is the one case that needs a process, and it says so rather than
# passing without one.
MAPPER="${MAPPER:-${HERE}/../build/tests/chaos_mapper}"
PARAM_DIR="/sys/module/${MODULE}/parameters"

# How many mounts race in T2 and how many times. Both are small on purpose: the delay below makes the
# collision certain rather than likely, so more rounds buy less than the run costs.
NODES_T2="${NODES_T2:-4}"
ROUNDS_T2="${ROUNDS_T2:-3}"

# Held between the free-slot scan and the claim write, so every racer picks the slot the others
# picked. Without it that window is microseconds and the lost-claim retry may never run.
CLAIM_DELAY_US="${CLAIM_DELAY_US:-5000}"

# T4 fills the table, so this is the table's size and not a load knob. FS_MAX_NODE_ID slots exist
# and the mount after the last one has nothing left to claim.
NODES_T4="${NODES_T4:-8}"

# Under BOOTSTRAP_FORMAT_TIMEOUT_MS, so T1's joiner reaches its timeout inside a test run.
FORMAT_TIMEOUT_MS="${FORMAT_TIMEOUT_MS:-500}"

# Past BOOTSTRAP_HEARTBEAT_TIMEOUT_MS with room to spare, so a sweep reads T5's frozen node as gone.
# The timeout is compiled in rather than a parameter, so this waits it out.
HEARTBEAT_WAIT_S="${HEARTBEAT_WAIT_S:-10}"

# Long enough for two GC_INTERVAL_MS sweeps and the work in them, since staking a dead node and
# clearing up after it are deliberately different sweeps.
RECOVERY_WAIT_S="${RECOVERY_WAIT_S:-45}"

# T8 recovers one node per two sweeps and has NODES_T8 - 1 of them to get through, so its budget is
# that many times the one above rather than a number of its own.
NODES_T8="${NODES_T8:-5}"
SWEEP_ALL_WAIT_S="${SWEEP_ALL_WAIT_S:-180}"

PASSED=0
FAILED=0
CASE_NAME=""

# Names every pass/fail/skip/note from here on, so a case spells its own name once rather than at
# each of its checks.
setCase() { CASE_NAME="$1"; }

# @1 bare before any case has set a name, "<case>: @1" once one has.
labelled() { [ -n "$CASE_NAME" ] && printf '%s: %s' "$CASE_NAME" "$1" || printf '%s' "$1"; }

note() { printf '[chaos] %s\n' "$(labelled "$*")"; }
pass() { printf '  ok   %s\n' "$(labelled "$1")"; PASSED=$((PASSED + 1)); }
fail() { printf '  FAIL %s\n' "$(labelled "$1")" >&2; FAILED=$((FAILED + 1)); }
skip() { printf '  --   %s\n' "$(labelled "$1")"; exit 77; }

[ "$(id -u)" -eq 0 ] || skip "bootstrap chaos owns the module and the mounts, so it needs root"
[ -c "$DAX_DEVICE" ] || skip "no DAX device at ${DAX_DEVICE}"
[ -f "$MODULE_KO" ] || skip "no module at ${MODULE_KO}; build kernel/ first"

# The module maps the metadata area uncached, which device_dax's whole-device reservation refuses to
# grant. A bound device makes every mount here fail for a reason that is not what is under test.
if [ -e "/sys/bus/dax/devices/$(basename "$DAX_DEVICE")/driver" ]; then
    skip "${DAX_DEVICE} is bound to device_dax; unbind it before this runs"
fi

# Its own mounts are the only ones it may take down. Another mount of this filesystem holds the
# module, so rmmod would fail and every case after the first would read a device somebody else owns.
if mount | grep -q " type ${MODULE} " &&
    ! mount | grep " type ${MODULE} " | grep -qv " ${MNT_BASE}"; then
    :
elif mount | grep -q " type ${MODULE} "; then
    skip "another ${MODULE} mount is up; take it down first (tools/deploy/reload.sh --no-start)"
fi

WORK="$(mktemp -d "/tmp/${FS_NAME}-chaos-XXXXXX")"

moduleLoaded() { grep -q "^${MODULE} " /proc/modules; }

umountAll()
{
    local point
    for point in "${MNT_BASE}"*; do
        [ -d "$point" ] || continue
        mountpoint -q "$point" && { umount "$point" || umount -l "$point"; } 2>/dev/null
        rmdir "$point" 2>/dev/null
    done
    return 0
}

daemonWasActive=false
daemon_active && daemonWasActive=true

# The layout these cases format is their own, and the suite needs one its daemons agree with. false
# keeps what a run left, for a reader who wants the state rather than the next suite run.
RESTORE=true

# Which mounts come back. Empty leaves reload.sh to its own default, and a host provisioned for
# more needs its points named here, because a run starts with none up and cannot read what was.
RESTORE_MOUNTS=""

usage()
{
    cat <<EOF
bootstrap_chaos.sh -- the bootstrap paths no case over the library can reach. Needs root.

  --no-restore          leave the layout these cases formatted, rather than putting one back
  --restore-mount PATH  a mount point to bring back; repeat for more (default: reload.sh's own)
  --help                this
EOF
}

# Flags and not environment variables, because sudoers carries env_reset and this only ever runs
# under sudo. An argument survives that; an exported name does not.
while [ $# -gt 0 ]; do
    case "$1" in
    --no-restore) RESTORE=false; shift ;;
    --restore-mount) RESTORE_MOUNTS="${RESTORE_MOUNTS} $2"; shift 2 ;;
    --help) usage; exit 0 ;;
    *) printf 'unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

unloadModule()
{
    umountAll
    moduleLoaded || return 0
    # The helper holds the daemon char device, and that is a module reference rmmod cannot take back.
    for unit in $(daemon_loaded_units); do systemctl stop "$unit" 2>/dev/null; done
    # The GC thread holds a module reference until it joins, so a first rmmod can arrive too early.
    local try
    for try in 1 2 3 4 5 6 7 8 9 10; do
        moduleLoaded || return 0
        rmmod "$MODULE" 2>/dev/null && return 0
        sleep 0.5
    done
    note "warning: ${MODULE} still loaded"
    return 1
}

loadModule()
{
    unloadModule
    insmod "$MODULE_KO" || return 1
    [ -d "$PARAM_DIR" ] || return 1
    return 0
}

setParam() { printf '%s' "$2" >"${PARAM_DIR}/$1"; }

# Every case here needs a mount to reach the formatter election, and only an unformatted device gets
# there. Nothing in user space can make one: the module maps the device itself, and device_dax, which
# owns the /dev node, has to stay unbound for that mapping to be uncached. So the module is asked
# instead, and it clears what it formats over.
blankDevice() { setParam bootstrap_inject_blank_device "$1"; }

# A sysfs attribute and not a module parameter, because it is keyed on the node and one host can
# hold several. Takes "<node_id> <0|1>".
freezeTick() { printf '%s %s' "$1" "$2" >"/sys/fs/${MODULE}/test/freeze_heartbeat"; }

# Stops @1's sweeps while its tick keeps running, so a frozen tick with a paused GC is a node that
# has stopped altogether. Takes "<node_id> <0|1>".
pauseGc() { printf '%s:%s' "$1" "$2" >"/sys/fs/${MODULE}/debug/gc_pause"; }

# Adds @2 seconds to what @1's tick would otherwise stamp, so the value keeps moving with the real
# clock but by an amount a peer's wall-clock check refuses. 0 clears the fault.
stampFault() { printf '%s %s' "$1" "$(($2 * 1000000000))" >"/sys/fs/${MODULE}/test/stamp_offset"; }

# -i so the mount helper stays out of it: that helper knows the daemon's options, and what is under
# test here is the kernel's own path.
mountNode()
{
    local suffix="$1"
    local extra="${2:-}"
    local point="${MNT_BASE}-${suffix}"
    local options="daxdev=${DAX_DEVICE}"
    [ -n "$extra" ] && options="${options},${extra}"
    mkdir -p "$point"
    mount -i -t "$MODULE" -o "$options" "none-${suffix}" "$point" 2>"${WORK}/mount-${suffix}.err" || return 1
    stubDaemon "$point"
}

# A mount with no daemon refuses every create and every turn, and these cases place regions. The
# knob stands in for that node's daemon, keyed on the node id the mount came up as.
stubDaemon()
{
    local nodeId
    nodeId="$(findmnt -no OPTIONS "$1" | grep -oE 'node_id=[0-9]+' | cut -d= -f2)"
    [ -n "$nodeId" ] || return 0
    printf '%s 1' "$nodeId" >"/sys/fs/${MODULE}/test/daemon_stub"
}

umountNode()
{
    local point="${MNT_BASE}-$1"
    mountpoint -q "$point" && umount "$point"
    rmdir "$point" 2>/dev/null
    return 0
}

# Since a marker stamp, so a check reads only what this run produced. A stamp and not a line count:
# the ring buffer drops its oldest lines once full, and a count taken earlier then names a later
# line than the one it was taken at.
dmesgSince()
{
    # The stamp is padded on the left below 100000 s, so it is cut out of the line and not taken as
    # the first field.
    dmesg | awk -v mark="$1" '{ line = $0; sub(/^\[ */, "", line); split(line, parts, "]"); if (parts[1] + 0 > mark + 0) print }'
}

# How many lines matching @pattern have landed in dmesg since @mark, which is what a count of
# something that should happen exactly once reads as.
countSince() { dmesgSince "$1" | grep -c "$2"; }

# Loads the module fresh, blanks the device, and sets the format timeout, under the name @1 that
# pass/fail/skip/note prepend from here on.
beginCase()
{
    setCase "$1"
    loadModule || { fail "the module did not load"; exit 1; }
    # A slot a stale holder left is not claimable until an admin recovers it, and a case counting
    # its mounts must not inherit one. A scratch mount zeroes the table and formats on its way.
    setParam bootstrap_inject_blank_slots 1
    blankDevice 1
    mountNode scratch && umountNode scratch
    blankDevice 1
    setParam bootstrap_format_timeout_ms "${2:-30000}"
}

# True when the mapper binary is there to run. Reports why not, with @1 filling in what could not
# happen without it, when it is missing.
hasMapper()
{
    [ -x "$MAPPER" ] && return 0
    fail "no mapper at ${MAPPER}, so $1"
    return 1
}

# Mounts node 1 as the formatter, then every node id passed after it once the device stops
# looking blank. Each mount that fails is reported under the current case, with its own stderr.
mountFormatterThenPeers()
{
    local stem="$1"
    shift

    if mountNode "${stem}1" node_id=1; then
        blankDevice 0
        local peer
        for peer in "$@"; do
            mountNode "${stem}${peer}" "node_id=${peer}" ||
                fail "node ${peer} did not mount: $(tr '\n' ' ' <"${WORK}/mount-${stem}${peer}.err")"
        done
    else
        blankDevice 0
        fail "node 1 did not mount: $(tr '\n' ' ' <"${WORK}/mount-${stem}1.err")"
    fi
}

# The end of a case: everything named comes down in the order given, and then the module unloads
# with it.
endCase()
{
    local node
    for node in "$@"; do
        umountNode "$node"
    done
    unloadModule
}

# Recovery advances one step per GC sweep, so how long a step takes is a cadence and not a constant
# this script knows. Waits for the line that says the step landed, up to RECOVERY_WAIT_S.
waitForDmesg()
{
    local mark="$1"
    local pattern="$2"
    local budget="${3:-$RECOVERY_WAIT_S}"
    local waited=0

    while [ "$waited" -lt "$budget" ]; do
        if dmesgSince "$mark" | grep -q "$pattern"; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}
# The stamp of the newest line, or 0 on an empty log.
dmesgMark() { dmesg | tail -n 1 | sed -n 's/^\[ *\([0-9.]*\)\].*/\1/p' | grep . || echo 0; }

# Places a region and holds it mapped until killed, printing the holder's pid.
#
# Held, and not placed and let go, because a region whose creator has exited and that carries no
# delegation is an orphan its own node reclaims on the next sweep. That sweep would then be what
# removed it, and a case about recovery would be reading somebody else's work.
#
# The caller takes the holder down before it umounts: a vma keeps a reference on the file and that
# reference pins the vfsmount.
startHolder()
{
    local out="${WORK}/holder-$2.out"
    : >"$out"
    "$MAPPER" "${MNT_BASE}-$1" "$2" >"$out" 2>"${WORK}/holder-$2.err" &
    local pid=$!
    local waited=0

    while [ "$waited" -lt 10 ] && ! grep -q mapped "$out"; do
        sleep 1
        waited=$((waited + 1))
    done

    grep -q mapped "$out" || return 1
    printf '%s' "$pid"
}

holderErr() { tr '\n' ' ' <"${WORK}/holder-$1.err"; }

# Places a region and lets it go. A case that only needs the entry to exist wants this; one that
# needs the entry to survive an ordinary orphan sweep wants startHolder instead.
makeRegion()
{
    "$MAPPER" "${MNT_BASE}-$1" "$2" --place >/dev/null 2>"${WORK}/mkregion.err"
}

mkRegionErr() { tr '\n' ' ' <"${WORK}/mkregion.err"; }

# Delegates permissions on a region to this account, from the mount named. The row records the node
# the call was made from, which is what a recovery matches on.
grantOn()
{
    "$MAPPER" "${MNT_BASE}-$1" "$2" --grant "$3" ${4:+"$4"} >/dev/null 2>"${WORK}/grant.err"
}

# Opens a region to every node. A row is only read on the node that wrote it, so this is what lets a
# second node reach the region at all and write a row of its own.
defaultOn()
{
    "$MAPPER" "${MNT_BASE}-$1" "$2" --default "$3" >/dev/null 2>"${WORK}/grant.err"
}

grantErr() { tr '\n' ' ' <"${WORK}/grant.err"; }

# region_info is one line per placed region: RAT_Entry Node PID State Size Offset Name.
regionOwner() { awk -v want="$1" '$7 == want { print $2 }' "/sys/fs/${MODULE}/region_info"; }
regionId() { awk -v want="$1" '$7 == want { print $1 }' "/sys/fs/${MODULE}/region_info"; }
regionOwnerPid() { awk -v want="$1" '$7 == want { print $3 }' "/sys/fs/${MODULE}/region_info"; }
regionState() { awk -v want="$1" '$7 == want { print $4 }' "/sys/fs/${MODULE}/region_info"; }
regionOffset() { awk -v want="$1" '$7 == want { print $6 }' "/sys/fs/${MODULE}/region_info"; }

# Takes the row naming an account back off. Own row or not is the module's judgement.
revokeOn()
{
    "$MAPPER" "${MNT_BASE}-$1" "$2" --revoke "$3" >/dev/null 2>"${WORK}/grant.err"
}

# deleg_info answers for whichever region was last written to it, so the write and the read are one
# operation as far as a caller is concerned.
delegNodes()
{
    local rid
    rid="$(regionId "$1")"
    [ -n "$rid" ] || return 1
    printf '%s' "$rid" >"/sys/fs/${MODULE}/deleg_info"
    grep -o 'node=[0-9]*' "/sys/fs/${MODULE}/deleg_info" | sort -u | tr '\n' ' '
}

stopHolder()
{
    [ -n "$1" ] || return 0
    kill "$1" 2>/dev/null
    wait "$1" 2>/dev/null
    return 0
}

# Placed and not merely named: an empty name would answer the same and prove nothing.
hasRegion() { [ -s "${MNT_BASE}-$1/$2" ]; }

# Every standing mount's reading of the slot table. One file per mount, each naming its own slot
# with "<mine>", so the count of those lines is the count of mounts holding one.
dumpMine()
{
    grep -h "<mine>" "/sys/fs/${MODULE}/node"*/test/bootstrap_dump 2>/dev/null
    return 0
}

# The state field of @1's slot, read off node 1's table since node 1 stays live for every case that
# uses this: 0 free, 1 held, 2 recovering.
bootstrapState()
{
    awk -v want="node_id=$1" '$2 == want { for (i = 1; i <= NF; i++) if ($i ~ /^state=/) { sub(/^state=/, "", $i); print $i } }' \
        "/sys/fs/${MODULE}/node1/test/bootstrap_dump"
}

# The module goes back on before the posture, because that parameter belongs to a module this has
# just unloaded. One format with nobody racing for it is what the daemons then join.
restoreHost()
{
    local reload="${HERE}/../tools/deploy/reload.sh"
    [ -x "$reload" ] || return 1

    local named=()
    local point
    for point in $RESTORE_MOUNTS; do
        named+=(--mount "$point")
    done

    bash "$reload" --skip-build --no-start "${named[@]}" >/dev/null 2>&1 || return 1
    mount -t "$MODULE" -o "daxdev=${DAX_DEVICE},node_id=1,format" none "/mnt/${FS_NAME}" || return 1
    umount "/mnt/${FS_NAME}" || return 1
    bash "$reload" --skip-build "${named[@]}" >/dev/null 2>&1 || return 1

    return 0
}

cleanup()
{
    unloadModule
    rm -rf "$WORK"

    if [ "$RESTORE" = true ] && restoreHost; then
        # The count, because reload.sh brings up its own default set and a host provisioned for more
        # comes back with fewer. A case that wants every mount reads this and knows.
        printf '\n[chaos] the host is back: one format and %s mount(s) up.\n' \
            "$(mount | grep -c "type ${MODULE} ")"
        return 0
    fi

    # A step that could not run leaves the rest here rather than to be guessed at, which is what
    # stops the next suite run from reporting a red case for a reason nobody can see.
    if [ "$daemonWasActive" = true ]; then
        for unit in $(daemon_units); do systemctl start "$unit" 2>/dev/null; done
    fi
    printf '\n[chaos] the device was reformatted by these cases.\n'
    printf '[chaos] the suite needs a layout its daemons agree with, which this sequence restores:\n'
    printf '[chaos]   sudo tools/deploy/reload.sh --skip-build --no-start\n'
    printf '[chaos]   sudo mount -t %s -o daxdev=%s,node_id=1,format none /mnt/%s\n' \
        "$MODULE" "$DAX_DEVICE" "$FS_NAME"
    printf '[chaos]   sudo umount /mnt/%s\n' "$FS_NAME"
    printf '[chaos]   sudo tools/deploy/reload.sh --skip-build\n'
    return 0
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# T2: N mounts at once elect one formatter
# ---------------------------------------------------------------------------
setCase T2
note "${NODES_T2} mounts at once, ${ROUNDS_T2} rounds"

round=1
while [ "$round" -le "$ROUNDS_T2" ]; do
    beginCase "T2 round ${round}" 30000
    setParam bootstrap_debug_pre_write_delay_us "$CLAIM_DELAY_US"

    mark="$(dmesgMark)"
    gate="${WORK}/gate-${round}"
    rm -f "$gate"

    # Each mount waits on the gate file, so all N reach mount(2) within one scheduling window rather
    # than in the order they were started.
    index=1
    while [ "$index" -le "$NODES_T2" ]; do
        (
            while [ ! -e "$gate" ]; do :; done
            mountNode "r${round}n${index}"
            printf '%s' "$?" >"${WORK}/rc-${round}-${index}"
        ) &
        index=$((index + 1))
    done

    : >"$gate"
    wait

    ok=0
    index=1
    while [ "$index" -le "$NODES_T2" ]; do
        [ "$(cat "${WORK}/rc-${round}-${index}" 2>/dev/null)" = "0" ] && ok=$((ok + 1))
        index=$((index + 1))
    done

    formatters="$(countSince "$mark" "formatter elected")"
    if [ "$formatters" -eq 1 ]; then
        pass "exactly one formatter among ${ok} mounts"
    else
        fail "${formatters} formatters elected"
    fi

    # The invariants below hold whether or not anyone lost a claim, so a round with no collision
    # would pass while testing nothing. This is what says the retry ran.
    lost="$(countSince "$mark" "claim race lost, retrying")"
    if [ "$lost" -ge 1 ]; then
        pass "${lost} lost claims went around again"
    else
        fail "no claim collided, so the retry path never ran"
    fi

    if [ "$ok" -ge 1 ]; then
        pass "${ok} of ${NODES_T2} mounts came up"
    else
        fail "no mount came up"
    fi

    # A node_id is a slot index plus one, so two mounts naming the same one is two mounts on one slot.
    owned="$(dumpMine | grep -oE 'node_id=[0-9]+' | sort)"
    if [ -n "$owned" ]; then
        if [ "$(printf '%s\n' "$owned" | wc -l)" = "$(printf '%s\n' "$owned" | sort -u | wc -l)" ]; then
            pass "no two mounts name the same node_id"
        else
            fail "a node_id is held twice"
        fi
    fi

    unloadModule
    round=$((round + 1))
done

# ---------------------------------------------------------------------------
# T3: a slot comes back and the device does not
# ---------------------------------------------------------------------------
beginCase T3
note "a slot comes back and the device does not"

mark="$(dmesgMark)"
if mountNode t3; then
    pass "the first mount of a blank device comes up"
else
    fail "the first mount failed"
fi

if [ "$(countSince "$mark" "formatter elected")" -eq 1 ]; then
    pass "and elected a formatter, since the device carried no layout"
else
    fail "no formatter elected on a blank device"
fi

umountNode t3

# The second half asks what an already formatted device does, so the device speaks for itself again.
blankDevice 0

mark="$(dmesgMark)"
if mountNode t3; then
    pass "it mounts again after a graceful umount"
else
    fail "the remount failed, so the slot did not come back"
fi

# A release writes a zero token, so the scan finds that slot free and claims it without formatting.
if [ "$(countSince "$mark" "formatter elected")" -eq 0 ]; then
    pass "and elects no formatter, so the layout survived the umount"
else
    fail "the remount formatted a device that was already formatted"
fi

endCase t3

# ---------------------------------------------------------------------------
# T4: every slot taken, and the mount after that
# ---------------------------------------------------------------------------
beginCase T4
note "${NODES_T4} mounts fill the table, and the next one is refused"

mark="$(dmesgMark)"

# Only the first mount may find a blank device. Left on, every mount after it would clear the
# superblock the first one wrote, time out waiting for a format and steal slot[0] instead of joining.
up=0
mountNode t4n1 && up=$((up + 1))
blankDevice 0

index=2
while [ "$index" -le "$NODES_T4" ]; do
    mountNode "t4n${index}" && up=$((up + 1))
    index=$((index + 1))
done

if [ "$up" -eq "$NODES_T4" ]; then
    pass "all ${NODES_T4} slots came up"
else
    fail "${up} of ${NODES_T4} mounts came up"
fi

# A node_id is a slot index plus one, so N mounts standing at once must name N different ones. Read
# from what each mount logged and not from bootstrap_dump: that attribute is one sysfs page, and a
# table of N mounts by N slots stops fitting well before N reaches the table's size.
distinct="$(dmesgSince "$mark" | grep -oE 'node_id=[0-9]+ \(slot [0-9]+\)' | sort -u | wc -l)"
if [ "$distinct" -eq "$up" ]; then
    pass "${distinct} distinct node_ids for ${up} mounts"
else
    fail "${up} mounts hold ${distinct} distinct node_ids"
fi

# Nothing is left for the claim scan to hand out, and a slot handed out twice is the failure this
# is here to catch.
if mountNode t4over; then
    fail "the mount past the last slot came up"
    umountNode t4over
elif grep -qi "busy" "${WORK}/mount-t4over.err"; then
    pass "the mount past the last slot is refused with EBUSY"
else
    fail "refused, but not with EBUSY: $(tr '\n' ' ' <"${WORK}/mount-t4over.err")"
fi

index=1
while [ "$index" -le "$NODES_T4" ]; do
    umountNode "t4n${index}"
    index=$((index + 1))
done
unloadModule

# ---------------------------------------------------------------------------
# T5: a stalled node is recovered by the admin, and fences itself when it comes back
# ---------------------------------------------------------------------------
# Nothing pokes the slot here. Node 1 is the admin, and its sweep is what stakes node 2, clears the
# rows and frees the slot, one step per sweep, which is why this waits on dmesg rather than sleeping.
#
# The tick is where a node reads its own slot, so the freeze is lifted rather than left on: coming
# back is what it fences on. Nothing here maps anything, so the revoke below finds no page to unmap.
beginCase T5
note "a stalled node is recovered by the admin, and fences itself when it comes back"

mountFormatterThenPeers t5n 2

# Regions on both sides. Without node 2's the sweep would be judged on an empty table, and without
# node 1's a sweep that cleared everything would read the same as one that cleared the right rows.
keptHolder="$(startHolder t5n1 t5-kept)" || fail "node 1 could not hold a region: $(holderErr t5-kept)"
goneHolderA="$(startHolder t5n2 t5-gone-a)" || fail "node 2 could not hold a region: $(holderErr t5-gone-a)"
goneHolderB="$(startHolder t5n2 t5-gone-b)" || fail "node 2 could not hold its second: $(holderErr t5-gone-b)"

if hasRegion t5n1 t5-gone-a && hasRegion t5n1 t5-gone-b; then
    pass "node 1 sees both of node 2's regions"
else
    fail "node 2's regions never reached the shared table"
fi

mark="$(dmesgMark)"

# Node 2 stalls. Its slot stops being restamped, so a peer reads the owner as dead.
freezeTick 2 1
sleep "$HEARTBEAT_WAIT_S"

# A stalled stamp frees nothing on its own, so the id is refused until the sweep has finished.
if mountNode t5n2b node_id=2; then
    fail "node_id=2 was claimable while its owner still held the slot"
    umountNode t5n2b
else
    pass "a stalled owner's slot is not claimable"
fi

if waitForDmesg "$mark" "took node 2 for dead"; then
    pass "the admin staked the stalled node's slot"
else
    fail "no stake after ${RECOVERY_WAIT_S}s"
fi

if waitForDmesg "$mark" "node 2 recovered"; then
    pass "and freed the slot once its rows were clear"
else
    fail "the slot was never freed"
fi

# The point of clearing the rows first: what the dead node owned is gone, and nothing else is. Both
# sides are still held by a live process, so no ordinary orphan sweep can account for either answer.
if hasRegion t5n1 t5-gone-a || hasRegion t5n1 t5-gone-b; then
    fail "the dead node's regions outlived its slot"
else
    pass "the dead node's regions went with it"
fi

if hasRegion t5n1 t5-kept; then
    pass "and the live node's region did not"
else
    fail "the recovery took a region belonging to a live node"
fi

# The mappings go before the umounts below, and node 2's before it comes back: what the fence does
# to a mapping is T7's claim, and leaving them here would make this case depend on it.
stopHolder "$goneHolderA"
stopHolder "$goneHolderB"

# Node 2 comes back. Its next tick reads a token that is not the one it wrote.
freezeTick 2 0
sleep 3

if dmesgSince "$mark" | grep -q "fencing this mount"; then
    pass "the returning node fenced itself"
else
    fail "no fence in dmesg"
fi

if dmesgSince "$mark" | grep -q "inode mapping(s) revoked"; then
    pass "the revoke ran on the way out"
else
    fail "no revoke in dmesg"
fi

# The mount has to still be there for a refusal to mean anything: an absent one refuses everything,
# which is how this read as green on a run where nothing mounted at all.
if ! mountpoint -q "${MNT_BASE}-t5n2"; then
    fail "node 2 is not mounted, so nothing below is being tested"
fi

# readdir and not stat on the mount point: the root inode carries dir_iops, which has no .getattr,
# so a stat there is answered from DRAM and reaches no shared state to refuse. The errno is checked
# too, since ENOENT from a missing directory would otherwise read the same as the refusal.
lserr="$(ls "${MNT_BASE}-t5n2" 2>&1 >/dev/null)"
if [ -z "$lserr" ]; then
    fail "a fenced mount still lists its directory"
elif printf '%s' "$lserr" | grep -qi "input/output error"; then
    pass "a fenced mount refuses to list its directory with EIO"
else
    fail "refused, but not with EIO: $(printf '%s' "$lserr" | tr '\n' ' ')"
fi

# The way out has to stay open, or a fenced mount would need a reboot. This holds only while nobody
# maps it: a vma keeps a reference on the file, that reference pins the vfsmount, and umount then
# answers EBUSY. Revoking the pages does not take the vma away, so a mapper still has to let go.
umountNode t5n2
if mountpoint -q "${MNT_BASE}-t5n2"; then
    fail "a fenced mount with no mapper will not unmount"
else
    pass "a fenced mount with no mapper still unmounts"
fi

# After the fenced mount is down: it still registered the channel device named after its id, and
# one host registers that name once.
if mountNode t5n2b node_id=2; then
    pass "the recovered node_id is claimable again"
else
    fail "node_id=2 was refused after the recovery: $(tr '\n' ' ' <"${WORK}/mount-t5n2b.err")"
fi

stopHolder "$keptHolder"
endCase t5n2b t5n1

# ---------------------------------------------------------------------------
# T6: a recovery whose admin dies is finished by the next admin
# ---------------------------------------------------------------------------
# What makes this possible is that no step is remembered anywhere but the slot. Node 1 stakes node 2
# and then stops answering itself, so node 3 becomes the admin, reads a stake whose owner is gone,
# and finishes what it did not start.
beginCase T6
note "a recovery whose admin dies is finished by the next admin"

mountFormatterThenPeers t6n 2 3

mark="$(dmesgMark)"

freezeTick 2 1
sleep "$HEARTBEAT_WAIT_S"

if waitForDmesg "$mark" "node 1 took node 2 for dead"; then
    pass "the admin staked node 2"
else
    fail "node 1 never staked node 2"
fi

# The admin goes down between staking and clearing, which is the window R3 is about. A frozen tick
# alone leaves its sweeps running, and they would finish the recovery before a peer reads it as dead.
pauseGc 1 1
freezeTick 1 1
sleep "$HEARTBEAT_WAIT_S"

if waitForDmesg "$mark" "node 3 took node 2 for dead"; then
    pass "the next admin picked the stranded recovery up"
else
    fail "node 3 never took over"
fi

if waitForDmesg "$mark" "node 3 freed slot 1, node 2 recovered"; then
    pass "and carried it through to the slot being freed"
else
    fail "node 3 took over but never finished"
fi

freezeTick 1 0
pauseGc 1 0
freezeTick 2 0
endCase t6n3 t6n2 t6n1

# ---------------------------------------------------------------------------
# T7: a mapping held across a recovery stops answering
# ---------------------------------------------------------------------------
# Refusing an operation reaches a caller that asks, and a process holding a mapping asks nobody, so
# this is the only case where the revoke is worth anything. The mapper maps a region on node 2 and
# reads it in a loop; the fence node 2 raises when it comes back is what takes the pages away, and
# the read after that is a SIGBUS the mapper reports.
#
# What this does not claim: node 2 keeps reading those pages for the whole freeze, including after
# the admin has freed the region they sit in. The tick is what notices, and a frozen tick notices
# nothing. A node that never comes back never revokes.
setCase T7
note "a mapping held across a recovery stops answering"

if hasMapper "the revoke is untested"; then
    beginCase T7

    mountFormatterThenPeers t7n 2

    mark="$(dmesgMark)"
    mapperOut="${WORK}/mapper.out"
    : >"$mapperOut"
    "$MAPPER" "${MNT_BASE}-t7n2" t7-mapped >"$mapperOut" 2>"${WORK}/mapper.err" &
    mapperPid=$!

    waited=0
    while [ "$waited" -lt 10 ] && ! grep -q "mapped" "$mapperOut"; do
        sleep 1
        waited=$((waited + 1))
    done

    if grep -q "mapped" "$mapperOut"; then
        pass "the mapper holds a mapping of node 2's region"
    else
        fail "the mapper never mapped: $(tr '\n' ' ' <"${WORK}/mapper.err")"
    fi

    freezeTick 2 1
    sleep "$HEARTBEAT_WAIT_S"

    if waitForDmesg "$mark" "node 2 recovered"; then
        pass "the admin recovered node 2 while the mapping stood"
    else
        fail "node 2 was never recovered"
    fi

    # Node 2 comes back, fences, and its GC thread is the context that clears the page tables.
    freezeTick 2 0

    if waitForDmesg "$mark" "inode mapping(s) revoked"; then
        pass "the returning node revoked what it had mapped"
    else
        fail "no revoke after the fence"
    fi

    waited=0
    while [ "$waited" -lt 15 ] && ! grep -q "revoked" "$mapperOut"; do
        sleep 1
        waited=$((waited + 1))
    done

    if grep -q "revoked" "$mapperOut"; then
        pass "and the process reading it took a SIGBUS"
    else
        fail "the mapper is still reading pages the admin gave away"
    fi

    wait "$mapperPid" 2>/dev/null
    endCase t7n2 t7n1
fi

# ---------------------------------------------------------------------------
# T8: one survivor recovers every node that stopped
# ---------------------------------------------------------------------------
# The cases above have one casualty and a healthy table around it. This is the other end: everything
# stops but one mount, and that mount is the admin by default because it is the only node still
# ticking. It has to get through all of them, one per two sweeps, without anyone to hand over to.
#
# Every node holds a region of its own, so the sweep has rows to clear for each and the survivor's
# own region is there to say it cleared the right ones.
setCase T8
note "one survivor recovers every node that stopped"

if hasMapper "there are no regions to recover"; then
    beginCase T8

    survivor="$NODES_T8"
    t8Holders=""
    t8Up=0

    index=1
    while [ "$index" -le "$NODES_T8" ]; do
        if mountNode "t8n${index}" "node_id=${index}"; then
            t8Up=$((t8Up + 1))
        fi
        blankDevice 0
        index=$((index + 1))
    done

    if [ "$t8Up" -eq "$NODES_T8" ]; then
        pass "all ${NODES_T8} nodes came up"
    else
        fail "only ${t8Up} of ${NODES_T8} nodes came up"
    fi

    placed=0
    index=1
    while [ "$index" -le "$NODES_T8" ]; do
        holder="$(startHolder "t8n${index}" "t8-region-${index}")" && placed=$((placed + 1))
        t8Holders="${t8Holders} ${holder}"
        index=$((index + 1))
    done

    if [ "$placed" -eq "$NODES_T8" ]; then
        pass "each node holds a region of its own"
    else
        fail "only ${placed} of ${NODES_T8} regions were held: $(holderErr "t8-region-1")"
    fi

    mark="$(dmesgMark)"

    # Everything but the survivor stops at once, which is the case: no staggered failures to make
    # the order easy, and nobody for the survivor to hand a half-finished recovery to.
    index=1
    while [ "$index" -lt "$survivor" ]; do
        freezeTick "$index" 1
        index=$((index + 1))
    done
    sleep "$HEARTBEAT_WAIT_S"

    recovered=0
    index=1
    while [ "$index" -lt "$survivor" ]; do
        if waitForDmesg "$mark" "node ${survivor} freed slot $((index - 1)), node ${index} recovered" \
                        "$SWEEP_ALL_WAIT_S"; then
            recovered=$((recovered + 1))
        fi
        index=$((index + 1))
    done

    if [ "$recovered" -eq $((survivor - 1)) ]; then
        pass "the survivor recovered all ${recovered} of them"
    else
        fail "only ${recovered} of $((survivor - 1)) were recovered"
    fi

    if hasRegion "t8n${survivor}" "t8-region-${survivor}"; then
        pass "and left its own region alone"
    else
        fail "the survivor swept away its own region"
    fi

    gone=0
    index=1
    while [ "$index" -lt "$survivor" ]; do
        hasRegion "t8n${survivor}" "t8-region-${index}" || gone=$((gone + 1))
        index=$((index + 1))
    done

    if [ "$gone" -eq $((survivor - 1)) ]; then
        pass "every stopped node's region went with it"
    else
        fail "$((survivor - 1 - gone)) of the stopped nodes' regions are still there"
    fi

    index=1
    while [ "$index" -lt "$survivor" ]; do
        freezeTick "$index" 0
        index=$((index + 1))
    done

    fenced=0
    index=1
    while [ "$index" -lt "$survivor" ]; do
        if waitForDmesg "$mark" "node ${index} lost slot $((index - 1)), fencing this mount"; then
            fenced=$((fenced + 1))
        fi
        index=$((index + 1))
    done

    if [ "$fenced" -eq $((survivor - 1)) ]; then
        pass "and each of them fenced itself on the way back"
    else
        fail "only ${fenced} of $((survivor - 1)) fenced"
    fi

    # The fenced mounts come down before their ids are claimed again on this host: each still holds
    # the channel device named after its id, and one host registers that name once.
    for holder in $t8Holders; do
        stopHolder "$holder"
    done
    index=1
    while [ "$index" -lt "$survivor" ]; do
        umountNode "t8n${index}"
        index=$((index + 1))
    done

    # A table that came back is one that can be claimed again, and each freed id is asked
    # separately: recovering four and leaving one of them unclaimable would read the same above.
    reclaimed=0
    served=0
    index=1
    while [ "$index" -lt "$survivor" ]; do
        if mountNode "t8spare${index}" "node_id=${index}"; then
            reclaimed=$((reclaimed + 1))
            # Mounting is not using. A region placed here says the layout under the reclaimed slot
            # is one this node can actually write.
            makeRegion "t8spare${index}" "t8-after-${index}" && served=$((served + 1))
            umountNode "t8spare${index}"
        fi
        index=$((index + 1))
    done

    if [ "$reclaimed" -eq $((survivor - 1)) ]; then
        pass "all ${reclaimed} freed node_ids are claimable again"
    else
        fail "only ${reclaimed} of $((survivor - 1)) freed node_ids could be claimed"
    fi

    if [ "$served" -eq $((survivor - 1)) ]; then
        pass "and each reclaimed mount places a region of its own"
    else
        fail "only ${served} of ${reclaimed} reclaimed mounts could place a region: $(mkRegionErr)"
    fi

    umountNode "t8n${survivor}"
    unloadModule
fi

# ---------------------------------------------------------------------------
# T9: a recovery clears the dead node's rows and marks what it cannot free
# ---------------------------------------------------------------------------
# Two regions end differently, and the rows decide which. A row carrying the dead node goes wherever
# it sits. Node 1's region therefore loses one row and keeps the other, and keeps its owner.
#
# The region the dead node owned still has a live node's row on it, so freeing it would take a region
# from a caller holding a grant. It is marked instead, and the whole owner identity goes with the
# mark: a node id nobody holds is what stops a later holder of that id reading itself as the owner.
setCase T9
note "a recovery clears the dead node's rows and marks what it cannot free"

if hasMapper "no grant can be issued"; then
    beginCase T9

    mountFormatterThenPeers t9n 2 3

    # The owner opens each region with READ|WRITE|GRANT as a default, because a row written on one
    # node is invisible on another and a peer would otherwise have no way in. Each node then writes
    # its own row for 0x3: a caller holding GRANT rather than ADMIN may not pass GRANT on.
    defaultOn t9n1 t9-kept 0x23 || fail "node 1 could not open its own region: $(grantErr)"
    grantOn t9n1 t9-kept 0x3 || fail "node 1 could not grant on its own region: $(grantErr)"
    grantOn t9n2 t9-kept 0x3 || fail "node 2 could not grant on node 1's region: $(grantErr)"

    defaultOn t9n2 t9-marked 0x23 || fail "node 2 could not open its own region: $(grantErr)"
    grantOn t9n2 t9-marked 0x3 || fail "node 2 could not grant on its own region: $(grantErr)"
    grantOn t9n3 t9-marked 0x3 || fail "node 3 could not grant on node 2's region: $(grantErr)"

    if [ "$(delegNodes t9-kept)" = "node=1 node=2 " ] && [ "$(delegNodes t9-marked)" = "node=2 node=3 " ]; then
        pass "each region carries a row from two nodes"
    else
        fail "rows are [$(delegNodes t9-kept)] and [$(delegNodes t9-marked)]"
    fi

    markedOffset="$(regionOffset t9-marked)"
    mark="$(dmesgMark)"

    freezeTick 2 1
    sleep "$HEARTBEAT_WAIT_S"

    if waitForDmesg "$mark" "node 2 recovered"; then
        pass "node 2 was recovered"
    else
        fail "node 2 was never recovered"
    fi

    # A row carrying the dead node goes; the row beside it does not.
    if [ "$(delegNodes t9-kept)" = "node=1 " ]; then
        pass "its row on the live node's region was cleared, and only that one"
    else
        fail "t9-kept rows are [$(delegNodes t9-kept)] after the sweep"
    fi

    if [ "$(regionState t9-kept)" = "ALLOCATED" ] && [ "$(regionOwner t9-kept)" = "1" ]; then
        pass "and that region is untouched otherwise"
    else
        fail "t9-kept is $(regionState t9-kept) owned by node $(regionOwner t9-kept)"
    fi

    # Its own region keeps the live node's row, so it is marked rather than freed.
    if [ "$(regionState t9-marked)" = "OWNER_DEAD" ]; then
        pass "the region it owned is marked, not freed"
    else
        fail "t9-marked is $(regionState t9-marked)"
    fi

    if [ "$(delegNodes t9-marked)" = "node=3 " ]; then
        pass "with its own row gone and the peer's left standing"
    else
        fail "t9-marked rows are [$(delegNodes t9-marked)] after the sweep"
    fi

    # The whole identity goes, not just the node: half of it left behind would still name a process
    # on the host that died, and a later holder of that node id would read itself as the owner.
    if [ "$(regionOwner t9-marked)" = "0" ] && [ "$(regionOwnerPid t9-marked)" = "0" ]; then
        pass "and nobody named as its owner"
    else
        fail "t9-marked names node $(regionOwner t9-marked) pid $(regionOwnerPid t9-marked)"
    fi

    freezeTick 2 0
    if waitForDmesg "$mark" "node 2 lost slot 1, fencing this mount"; then
        pass "the recovered node fences itself on the way back"
    else
        fail "node 2 came back and kept serving"
    fi

    umountNode t9n2
fi

# ---------------------------------------------------------------------------
# T10: what a marked region refuses, and what it still answers
# ---------------------------------------------------------------------------
# Runs on what T9 left standing, because the state is what T9 produced and rebuilding it here would
# test the setup rather than the rule.
#
# The mark is not a fence. Its readers are being let finish, so a default still admits one and the
# extent stays occupied. What ends is anything new: no row may be added, and the region goes as soon
# as the last row does.
setCase T10
note "what a marked region refuses, and what it still answers"

if [ "$(regionState t9-marked)" != "OWNER_DEAD" ]; then
    skip "T9 left no marked region to work on"
fi

# A grant would hand out a permission over bytes that are going away.
if grantOn t9n3 t9-marked 0x1 12345; then
    fail "a marked region took a new row"
else
    pass "a marked region takes no new row"
fi

# The extent is still somebody's, so a fresh region must not be placed on top of it.
makeRegion t9n1 t10-fresh || fail "node 1 could not place a region: $(mkRegionErr)"
if [ "$(regionOffset t10-fresh)" = "$markedOffset" ]; then
    fail "a new region landed on the marked one's extent"
else
    pass "and its extent is not handed to a new region"
fi

# The default the dead owner left still admits a reader, which is the point of letting them finish.
t10Holder="$(startHolder t9n3 t9-marked)" && pass "a default still admits a reader" ||
    fail "a marked region refused a reader its default admits: $(holderErr t9-marked)"
stopHolder "$t10Holder"

# The last row goes, and with it the region.
mark="$(dmesgMark)"
if revokeOn t9n3 t9-marked 0; then
    pass "the last row can be given up"
else
    fail "the last row could not be given up: $(grantErr)"
fi

if waitForDmesg "$mark" "gc reclaiming RAT entry"; then
    pass "and the region goes with it"
else
    fail "the marked region outlived its last row"
fi

endCase t9n3 t9n1

# ---------------------------------------------------------------------------
# T11: who may take a row back
# ---------------------------------------------------------------------------
# Giving up a row that names you takes nothing from anybody, so it asks for nothing. Taking one off
# somebody else is the owner deciding who reaches the region, which is what ADMIN means.
setCase T11
note "who may take a row back"

if hasMapper "no row can be written"; then
    beginCase T11

    mountFormatterThenPeers t11n 2

    # An owner is only an owner inside the process that placed the region, and that process exits.
    # Placing it with a grant leaves ADMIN as a row, which every later call on node 1 reads back.
    grantOn t11n1 t11-region 0x8 || fail "node 1 could not keep ADMIN as a row: $(grantErr)"
    defaultOn t11n1 t11-region 0x23 || fail "node 1 could not open its region: $(grantErr)"
    grantOn t11n2 t11-region 0x3 || fail "node 2 could not grant on it: $(grantErr)"
    # A row naming somebody this run is not, so the self case cannot answer for it.
    grantOn t11n1 t11-region 0x1 12345 || fail "node 1 could not grant to another account: $(grantErr)"

    if revokeOn t11n2 t11-region 0; then
        pass "a caller gives up the row that names it"
    else
        fail "giving up an own row was refused: $(grantErr)"
    fi

    if [ "$(delegNodes t11-region)" = "node=1 " ]; then
        pass "and only that row went"
    else
        fail "rows are [$(delegNodes t11-region)] after the release"
    fi

    if revokeOn t11n2 t11-region 0; then
        fail "taking the same row back twice answered success"
    else
        pass "a row that is already gone answers ENOENT"
    fi

    # Node 2 is not the owner and holds no ADMIN, so a row naming somebody else is not its to take.
    if revokeOn t11n2 t11-region 12345; then
        fail "a caller without ADMIN took somebody else's row"
    else
        pass "a caller without ADMIN cannot take somebody else's row"
    fi

    if revokeOn t11n1 t11-region 12345; then
        pass "and a caller holding ADMIN can"
    else
        fail "a caller holding ADMIN could not take that row: $(grantErr)"
    fi

    endCase t11n2 t11n1
fi

# The state field of one slot's row under rat/. A free slot's whole row is the word "free".
slotState() { awk -F'\t' 'NF >= 3 { print $3; next } { print $1 }' "/sys/fs/${MODULE}/rat/$1"; }

# ---------------------------------------------------------------------------
# T12: a dead owner's region waits for a reader that holds no row
# ---------------------------------------------------------------------------
# Only the reader's reference bit says it is there, since it came in through the default and wrote
# no row. The recovery marks the region, and the admin's sweep takes it once the reader is gone.
setCase T12
note "a dead owner's region waits for a reader that holds no row"

if hasMapper "no reader can hold a region"; then
    beginCase T12

    mountFormatterThenPeers t12n 2

    defaultOn t12n2 t12-read 0x3 || fail "node 2 could not open its region to every node: $(grantErr)"
    t12Holder="$(startHolder t12n1 t12-read)" || fail "node 1 could not map node 2's region: $(holderErr t12-read)"
    readOffset="$(regionOffset t12-read)"

    mark="$(dmesgMark)"
    freezeTick 2 1
    sleep "$HEARTBEAT_WAIT_S"

    if waitForDmesg "$mark" "node 2 recovered"; then
        pass "node 2 was recovered"
    else
        fail "node 2 was never recovered"
    fi

    if [ "$(regionState t12-read)" = "OWNER_DEAD" ]; then
        pass "the region a live reader holds is marked, not freed"
    else
        fail "t12-read is '$(regionState t12-read)' after the recovery"
    fi

    makeRegion t12n1 t12-fresh || fail "node 1 could not place a region: $(mkRegionErr)"
    if [ "$(regionOffset t12-fresh)" = "$readOffset" ]; then
        fail "a new region landed on the held extent"
    else
        pass "and its extent is not handed to a new region"
    fi

    mark="$(dmesgMark)"
    stopHolder "$t12Holder"
    if waitForDmesg "$mark" "gc reclaiming RAT entry"; then
        pass "the region goes once the reader does"
    else
        fail "the marked region outlived its last reader"
    fi

    freezeTick 2 0
    waitForDmesg "$mark" "fencing this mount" || fail "node 2 came back and kept serving"
    umountNode t12n2
    endCase t12n1
fi

# ---------------------------------------------------------------------------
# T13: a holder's node dies, and the extent it held goes back
# ---------------------------------------------------------------------------
# The bit a dead node left set would otherwise keep an unlinked extent forever. The recovery zeroes
# that node's line, and the owner's next sweep frees what only that bit was holding.
setCase T13
note "a holder's node dies, and the extent it held goes back"

if hasMapper "no reader can hold a region"; then
    beginCase T13

    mountFormatterThenPeers t13n 2

    defaultOn t13n1 t13-held 0x3 || fail "node 1 could not open its region to every node: $(grantErr)"
    t13Holder="$(startHolder t13n2 t13-held)" || fail "node 2 could not map node 1's region: $(holderErr t13-held)"
    heldSlot="$(regionId t13-held)"

    if rm "${MNT_BASE}-t13n1/t13-held" 2>"${WORK}/rm.err"; then
        pass "the name goes while node 2 holds the region"
    else
        fail "unlink refused: $(tr '\n' ' ' <"${WORK}/rm.err")"
    fi

    if [ "$(slotState "$heldSlot")" = "ALLOCATED" ]; then
        pass "and the slot stays taken for the holder"
    else
        fail "slot ${heldSlot} reads '$(slotState "$heldSlot")' after the unlink"
    fi

    mark="$(dmesgMark)"
    freezeTick 2 1
    sleep "$HEARTBEAT_WAIT_S"

    if waitForDmesg "$mark" "node 2 recovered"; then
        pass "node 2 was recovered"
    else
        fail "node 2 was never recovered"
    fi

    if waitForDmesg "$mark" "gc reclaiming RAT entry ${heldSlot} ("; then
        pass "the slot only its bit was holding goes back"
    else
        fail "slot ${heldSlot} outlived the node that held it"
    fi

    # The mapper on the recovered node has to let go before that mount can come down.
    stopHolder "$t13Holder"
    freezeTick 2 0
    waitForDmesg "$mark" "fencing this mount" || fail "node 2 came back and kept serving"
    umountNode t13n2
    endCase t13n1
fi

# ---------------------------------------------------------------------------
# T14: an owner node's clean unmount leaves nothing that names it
# ---------------------------------------------------------------------------
# No admin recovers a slot that was released, so the departing node runs the clean step on itself
# before the slot goes. A region a peer still holds is marked, one nobody holds goes, and the id is
# claimable again at once. The mount that takes the id next then owns none of the old regions.
setCase T14
note "an owner node's clean unmount leaves nothing that names it"

if hasMapper "no reader can hold a region"; then
    beginCase T14

    mountFormatterThenPeers t14n 2

    defaultOn t14n2 t14-held 0x3 || fail "node 2 could not open its region to every node: $(grantErr)"
    makeRegion t14n2 t14-loose || fail "node 2 could not place a second region: $(mkRegionErr)"
    t14Holder="$(startHolder t14n1 t14-held)" || fail "node 1 could not map node 2's region: $(holderErr t14-held)"
    heldSlot="$(regionId t14-held)"
    looseSlot="$(regionId t14-loose)"

    mark="$(dmesgMark)"
    umountNode t14n2

    if waitForDmesg "$mark" "node 2 slot 1 released" 5; then
        pass "node 2 released its slot"
    else
        fail "node 2 kept its slot after a clean unmount"
    fi

    if [ "$(regionState t14-held)" = "OWNER_DEAD" ]; then
        pass "the region a peer holds is marked when its owner unmounts"
    else
        fail "t14-held is '$(regionState t14-held)' after the unmount"
    fi

    if [ "$(slotState "$looseSlot")" = "free" ]; then
        pass "and the region nobody holds goes with the mount"
    else
        fail "slot ${looseSlot} reads '$(slotState "$looseSlot")' after the unmount"
    fi

    if mountNode t14n2b node_id=2; then
        pass "the id is claimable again at once"
    else
        fail "node_id=2 was refused after a clean unmount: $(tr '\n' ' ' <"${WORK}/mount-t14n2b.err")"
    fi

    if [ "$(regionState t14-held)" = "OWNER_DEAD" ]; then
        pass "and the next holder of the id does not read the marked region as its own"
    else
        fail "t14-held is '$(regionState t14-held)' once node_id=2 is back"
    fi

    mark="$(dmesgMark)"
    stopHolder "$t14Holder"
    if waitForDmesg "$mark" "gc reclaiming RAT entry ${heldSlot} ("; then
        pass "the marked region goes once the peer does"
    else
        fail "slot ${heldSlot} outlived its last reader"
    fi

    endCase t14n2b t14n1
fi

# ---------------------------------------------------------------------------
# T15: an out-of-range but moving stamp is not staked; frozen, it still is
# ---------------------------------------------------------------------------
# A stamp outside the wall-clock window falls back to whether it moves, so a live node an hour slow
# or a minute fast is not staked while it keeps changing, and is once the same fault stops moving.
setCase T15
note "an out-of-range but moving stamp is not staked; frozen, it still is"

if hasMapper "no reader can hold a region"; then
    beginCase T15

    mountFormatterThenPeers t15n 2

    keptHolder="$(startHolder t15n1 t15-kept)" || fail "node 1 could not hold a region: $(holderErr t15-kept)"
    goneHolder="$(startHolder t15n2 t15-gone)" || fail "node 2 could not hold a region: $(holderErr t15-gone)"

    mark="$(dmesgMark)"

    # An hour slow. A wall-clock check would have refused this on the first sweep; only movement
    # calls it alive now.
    stampFault 2 -3600
    sleep "$RECOVERY_WAIT_S"

    if [ "$(bootstrapState 2)" = "1" ]; then
        pass "an hour-slow but moving stamp keeps the slot held"
    else
        fail "slot 2 reads state $(bootstrapState 2) while its stamp was still moving"
    fi

    if dmesgSince "$mark" | grep -q "took node 2 for dead"; then
        fail "the admin staked a node whose stamp was still moving"
    else
        pass "no stake while the stamp moved, an hour slow"
    fi

    mark="$(dmesgMark)"

    # A minute fast: the same fallback, the other side of now.
    stampFault 2 60
    sleep "$RECOVERY_WAIT_S"

    if [ "$(bootstrapState 2)" = "1" ]; then
        pass "a minute-fast but moving stamp keeps the slot held"
    else
        fail "slot 2 reads state $(bootstrapState 2) while its stamp was still moving"
    fi

    if dmesgSince "$mark" | grep -q "took node 2 for dead"; then
        fail "the admin staked a node whose stamp was still moving"
    else
        pass "no stake while the stamp moved, a minute fast"
    fi

    if mountNode t15n2b node_id=2; then
        fail "node_id=2 was claimable while its moving-but-out-of-range slot was still held"
        umountNode t15n2b
    else
        pass "a slot alive by movement is not claimable"
    fi

    # The same out-of-range stamp now stops moving too, which is what an ordinary dead node also
    # leaves behind: the rest of this case is T5's recovery path.
    mark="$(dmesgMark)"
    freezeTick 2 1
    sleep "$HEARTBEAT_WAIT_S"

    if waitForDmesg "$mark" "took node 2 for dead"; then
        pass "the stamp going still on top of the same fault is what gets it staked"
    else
        fail "no stake after ${RECOVERY_WAIT_S}s"
    fi

    if waitForDmesg "$mark" "node 2 recovered"; then
        pass "and freed the slot once its rows were clear"
    else
        fail "the slot was never freed"
    fi

    if hasRegion t15n1 t15-gone; then
        fail "the dead node's region outlived its slot"
    else
        pass "the dead node's region went with it"
    fi

    if hasRegion t15n1 t15-kept; then
        pass "and the live node's region did not"
    else
        fail "the recovery took a region belonging to a live node"
    fi

    stopHolder "$goneHolder"

    stampFault 2 0
    freezeTick 2 0
    sleep 3

    if dmesgSince "$mark" | grep -q "fencing this mount"; then
        pass "the returning node fenced itself"
    else
        fail "no fence in dmesg"
    fi

    stopHolder "$keptHolder"
    umountNode t15n2
    endCase t15n1
fi

# ---------------------------------------------------------------------------
# T1: a formatter that never publishes is stolen from
# ---------------------------------------------------------------------------
beginCase T1 "$FORMAT_TIMEOUT_MS"
note "a formatter that never publishes is stolen from"
setParam bootstrap_inject_stuck_formatter 1

mark="$(dmesgMark)"

# Node A wins slot[0] and returns -EOWNERDEAD without releasing it, so the mount fails by design and
# the slot stays held with no GSB magic behind it.
if mountNode a; then
    fail "node A mounted, so the injection did not take"
    umountNode a
else
    pass "node A refuses its own mount under the injection"
fi

if dmesgSince "$mark" | grep -q "stuck-formatter injection active"; then
    pass "and says so, which is the slot being left held"
else
    fail "no injection line in dmesg, so the path was not taken"
fi

setParam bootstrap_inject_stuck_formatter 0
mark="$(dmesgMark)"

# Node B claims a different slot, waits out wait_for_format, finds no magic, and takes slot[0].
if mountNode b; then
    pass "node B mounts after waiting the formatter out"
else
    fail "node B never mounted, so the steal path did not finish"
fi

if dmesgSince "$mark" | grep -q "no format after"; then
    pass "node B waited for a format that never came"
else
    fail "node B did not report a format timeout"
fi

if dmesgSince "$mark" | grep -q "stole slot\[0\] for recovery"; then
    pass "and stole slot[0]"
else
    fail "no steal in dmesg"
fi

if dumpMine | grep -q "<mine>"; then
    pass "the mount that recovered holds a slot of its own"
else
    fail "bootstrap_dump names no slot for the recovered mount"
fi

endCase b

printf '\nbootstrap_chaos: %d of %d passed\n' "$PASSED" "$((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
