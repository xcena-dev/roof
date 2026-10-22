#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# device_order_remount -- a mount of several devices keeps each device's index whatever order the
# daxdev= options come in, and a mount that leaves a device out is refused.
#
# Takes the host's first mount down, mounts it again with its devices in reverse order and compares
# the device lines of pool_info, then mounts it with its last device left out, then brings it back
# as its unit has it. A mount of one device has nothing to reorder, so the case skips. Needs root:
#
#   sudo ctest --test-dir <build> -R device_order_remount

set -u -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
# shellcheck source=/dev/null
. "${ROOT}/fsname"
# shellcheck source=/dev/null
. "${ROOT}/shell/units.sh"
# shellcheck source=/dev/null
. "${ROOT}/shell/kernel_log.sh"

SYSFS="/sys/fs/${FS_NAME}"
MOUNT="${MOUNT:-/mnt/${FS_NAME}}"
MOUNT_WAIT_S="${MOUNT_WAIT_S:-60}"

FAILED=0
pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; FAILED=$((FAILED + 1)); }
note() { printf '     %s\n' "$*"; }
skip() { printf 'SKIP %s\n' "$*"; exit 77; }

[ "$(id -u)" -eq 0 ] || skip "needs root to take the mount down and up"
mountpoint -q "$MOUNT" || skip "no mount at ${MOUNT}"

UNIT="$(unitNameFor "$MOUNT")"
UNIT_OPTIONS="$(systemctl show --property=Options --value "$UNIT")"
[ -n "$UNIT_OPTIONS" ] || skip "no mount unit ${UNIT} to bring the mount back with"

# The daxdev= values of the mount, in the order the kernel shows them.
mountedDevices()
{
    awk -v target="$MOUNT" -v kind="$FS_NAME" '
        $2 == target && $3 == kind {
            count = split($4, options, ",")
            for (idx = 1; idx <= count; idx++) {
                if (options[idx] ~ /^daxdev=/) {
                    sub(/^daxdev=/, "", options[idx])
                    print options[idx]
                }
            }
            exit
        }' /proc/self/mounts
}

mapfile -t DEVICES < <(mountedDevices)
[ "${#DEVICES[@]}" -ge 2 ] || skip "the mount has one device, so there is no order to change"

# Each device's index, start and size in the write-back pool. The free column moves with every
# placement, so it is left out.
deviceLines()
{
    awk '$1 == "device" { print $1, $2, $3, $4 }' "${SYSFS}/pool_info"
}

# The unit's options with its daxdev= entries replaced by the ones in $@, in that order.
optionsWithDevices()
{
    local kept="" entry
    local -a parts
    IFS=',' read -r -a parts <<<"$UNIT_OPTIONS"
    for entry in "${parts[@]}"; do
        case "$entry" in
        daxdev=*) ;;
        *) kept+=",${entry}" ;;
        esac
    done
    local devices="" device
    for device in "$@"; do
        devices+="daxdev=${device},"
    done
    printf '%s' "${devices}${kept#,}"
}

# Through the mount helper, which starts the node's daemon, as the unit does.
mountWith()
{
    timeout "$MOUNT_WAIT_S" mount -t "$FS_NAME" -o "$(optionsWithDevices "$@")" none "$MOUNT"
}

takeDown()
{
    timeout "$MOUNT_WAIT_S" systemctl stop "$UNIT"
    ! mountpoint -q "$MOUNT"
}

bringBack()
{
    systemctl reset-failed "${DAEMON_NAME}@*.service" 2>/dev/null
    timeout "$MOUNT_WAIT_S" systemctl start "$UNIT" && mountpoint -q "$MOUNT"
}

restoreMount()
{
    mountpoint -q "$MOUNT" || bringBack
}

BEFORE="$(deviceLines)"
[ -n "$BEFORE" ] || skip "pool_info lists no device lines"
note "devices before: $(tr '\n' ';' <<<"$BEFORE")"

REVERSED=()
for ((idx = ${#DEVICES[@]} - 1; idx >= 0; idx--)); do
    REVERSED+=("${DEVICES[idx]}")
done

takeDown || { fail "the mount did not come down"; exit 1; }
trap restoreMount EXIT

if mountWith "${REVERSED[@]}"; then
    pass "the mount came up with its devices in reverse order"
    if [ "$(mountedDevices)" = "$(printf '%s\n' "${DEVICES[@]}")" ]; then
        pass "the mount table lists the devices in index order"
    else
        fail "the mount table lists the devices out of index order"
    fi
    AFTER="$(deviceLines)"
    if [ "$AFTER" = "$BEFORE" ]; then
        pass "every device kept its index"
    else
        fail "the device lines changed: $(tr '\n' ';' <<<"$AFTER")"
    fi
    takeDown || fail "the reversed mount did not come down"
else
    fail "the mount did not come up with its devices in reverse order"
fi

MARK="$(logMark)"
if mountWith "${DEVICES[@]:0:${#DEVICES[@]}-1}"; then
    fail "a mount with a device left out came up"
    takeDown
else
    pass "a mount with a device left out is refused"
    if loggedSince "$MARK" "the superblock lists"; then
        pass "the kernel said the device list did not match"
    else
        fail "the kernel did not log the device count mismatch"
    fi
fi
mountpoint -q "$MOUNT" && takeDown

if bringBack; then
    pass "the mount is back as its unit has it"
    if [ "$(deviceLines)" = "$BEFORE" ]; then
        pass "and its devices read as they did at the start"
    else
        fail "the devices differ from the start: $(deviceLines | tr '\n' ';')"
    fi
else
    fail "the mount did not come back through ${UNIT}"
fi

if [ "$FAILED" -eq 0 ]; then
    printf 'every check passed\n'
    exit 0
fi
printf '%d checks failed\n' "$FAILED"
exit 1
