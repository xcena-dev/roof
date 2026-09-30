#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# leftovers.sh -- what this tree still has on the host, and the command that removes each one.
#
#   tools/deploy/leftovers.sh            list what is there
#   sudo tools/deploy/leftovers.sh --rm  remove it
#   FS_NAME=oldname ... --rm             clean up a host installed under a previous name
#
# Reads nothing but the host. install.sh writes a state file and uninstall.sh reads it, but a host
# that lost that file still has to be cleanable, so every place is named here rather than recorded.
#
# Needs no privilege to list. Removing does.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
# The tree's name unless one is handed in: a host installed before a rename still has to be
# cleanable, and the file at the root no longer says what it was called then.
if [ -z "${FS_NAME:-}" ]; then
	. "${ROOT}/fsname"
fi
. "${ROOT}/shell/paths.sh"
. "${ROOT}/shell/deploy_state.sh"

REMOVE=false

for arg in "$@"; do
	case "$arg" in
	--rm) REMOVE=true ;;
	-h | --help)
		sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "unknown flag: $arg" >&2
		exit 2
		;;
	esac
done

RED=$'\033[0;31m'
GREEN=$'\033[0;32m'
NC=$'\033[0m'

FOUND=0

# @what is what a reader sees, @probe decides, @undo is the command that takes it away.
report()
{
	local what="$1" probe="$2" undo="$3"
	if ! eval "$probe" > /dev/null 2>&1; then
		printf '%s  gone%s %s\n' "$GREEN" "$NC" "$what"
		return
	fi
	FOUND=$((FOUND + 1))
	printf '%s  HERE%s %s\n' "$RED" "$NC" "$what"
	printf '         %s\n' "$undo"
	if [ "$REMOVE" = true ]; then
		eval "$undo" && printf '         removed\n' || printf '         that did not work\n'
	fi
}

echo "== processes and the module =="
report "the ${DAEMON_NAME} instances" \
	"systemctl is-active --quiet '${DAEMON_NAME}@*.service'" \
	"systemctl disable --now '${DAEMON_NAME}@*.service'"
for area in $(systemctl list-units --no-legend "cmed-${FS_NAME}@*" 2>/dev/null | awk '{print $1}'); do
	report "the daemon ${area}" "systemctl is-active --quiet ${area}" "systemctl disable --now ${area}"
done
while IFS= read -r mount; do
	[ -n "$mount" ] || continue
	report "the mount at ${mount}" "mountpoint -q '${mount}'" "umount '${mount}'"
done < <(awk -v type="$FS_NAME" '$3 == type {print $2}' /proc/mounts)
report "the ${FS_NAME} module, resident" "[ -d /sys/module/${FS_NAME} ]" "modprobe -r ${FS_NAME}"

echo
echo "== units =="
report "the mount units" \
	"ls ${UNITDIR}/mnt-${FS_NAME}*.mount" \
	"rm -f ${UNITDIR}/mnt-${FS_NAME}*.mount && systemctl daemon-reload"
report "the daemon unit template" \
	"[ -e ${UNITDIR}/cmed-${FS_NAME}@.service ]" \
	"rm -f ${UNITDIR}/cmed-${FS_NAME}@.service && systemctl daemon-reload"
report "the daemon unit" \
	"[ -e /etc/systemd/system/${DAEMON_NAME}@.service ]" \
	"rm -f /etc/systemd/system/${DAEMON_NAME}@.service && systemctl daemon-reload"
report "the daemon unit's mount drop-in" \
	"[ -e /etc/systemd/system/${DAEMON_NAME}@.service.d/mounts.conf ]" \
	"rm -rf /etc/systemd/system/${DAEMON_NAME}@.service.d && systemctl daemon-reload"
report "the autoload mount unit" \
	"[ -e /etc/systemd/system/${FS_NAME}-mount.service ]" \
	"rm -f /etc/systemd/system/${FS_NAME}-mount.service && systemctl daemon-reload"

echo
echo "== what a build installed =="
report "the module file" "[ -e ${MODULE_DIR}/${FS_NAME}.ko ]" \
	"rm -f ${MODULE_DIR}/${FS_NAME}.ko && depmod -a"
report "the mount helpers" \
	"ls ${HELPER_DIR}/mount.${FS_NAME} ${HELPER_DIR}/umount.${FS_NAME}" \
	"rm -f ${HELPER_DIR}/mount.${FS_NAME} ${HELPER_DIR}/umount.${FS_NAME}"
report "the library archive" "[ -e ${PREFIX}/lib/lib${LIB_NAME}.a ]" \
	"rm -f ${PREFIX}/lib/lib${LIB_NAME}.a"
report "its pc file" "[ -e ${PREFIX}/lib/pkgconfig/lib${LIB_NAME}.pc ]" \
	"rm -f ${PREFIX}/lib/pkgconfig/lib${LIB_NAME}.pc"
report "its headers" "[ -d ${PREFIX}/include/${LIB_NAME} ]" \
	"rm -rf ${PREFIX}/include/${LIB_NAME}"
report "the daemon binary" "[ -e /usr/local/bin/${DAEMON_NAME} ]" \
	"rm -f /usr/local/bin/${DAEMON_NAME}"

echo
echo "== configs and rules =="
# The next two are an older layout's, which ran a cme daemon beside each mount. Nothing installs
# them now, and a host that had that layout still has them.
report "an older layout's cmed configs" "ls /etc/cme/cmed-mnt_${FS_NAME}*.yaml" \
	"rm -f /etc/cme/cmed-mnt_${FS_NAME}*.yaml"
report "the daemon's config directory" "[ -d /etc/${FS_NAME} ]" "rm -rf /etc/${FS_NAME}"
report "the udev rule" "[ -e /etc/udev/rules.d/99-${DAEMON_NAME}.rules ]" \
	"rm -f /etc/udev/rules.d/99-${DAEMON_NAME}.rules && udevadm control --reload"
report "the modprobe pin" "[ -e /etc/modprobe.d/${FS_NAME}.conf ]" \
	"rm -f /etc/modprobe.d/${FS_NAME}.conf"
report "the autoload configs" \
	"ls /etc/modules-load.d/${FS_NAME}.conf" \
	"rm -f /etc/modules-load.d/${FS_NAME}.conf"
report "an fstab line" "grep -q ' ${FS_NAME} ' /etc/fstab" \
	"sed -i '/[[:space:]]${FS_NAME}[[:space:]]/d' /etc/fstab"

echo
echo "== state, logs and accounts =="
report "the deploy state" "[ -d /var/lib/${FS_NAME} ]" "rm -rf /var/lib/${FS_NAME}"
report "the audit log" "[ -d /var/log/${FS_NAME} ]" "rm -rf /var/log/${FS_NAME}"
report "the runtime directory" "[ -d /run/${FS_NAME} ]" "rm -rf /run/${FS_NAME}"
report "an older layout's socket directory" "[ -d /run/cmed ]" "rm -rf /run/cmed"
report "the mount points" "ls -d /mnt/${FS_NAME}*" "rmdir /mnt/${FS_NAME}*"
report "the helper account" "id ${FS_NAME}" "userdel ${FS_NAME}"
# Only this filesystem's own group. An older layout also made a group named cme, which it shared
# with the cme project's own daemon, so that one is left to whoever still uses it.
#
# The members go first: groupdel refuses a group somebody is still in, and install.sh is what put
# the operator's account in it.
members="$(getent group "${FS_NAME}" | cut -d: -f4)"
undo="groupdel ${FS_NAME}"
if [ -n "$members" ]; then
	undo="members='${members}'; for member in \${members//,/ }; do gpasswd -d \$member ${FS_NAME}; done; ${undo}"
fi
report "the group ${FS_NAME}${members:+ (members: ${members})}" \
	"getent group ${FS_NAME}" "$undo"

echo
echo "== devices =="
# install.sh gives the dax device this tree's group, and deleting that group leaves its gid behind.
for device in /dev/dax*; do
	[ -e "$device" ] || continue
	group="$(stat -c '%G' "$device")"
	probe=false
	isLeftoverGroup "$group" && probe=true
	report "the group on ${device} (${group})" "$probe" "chgrp root ${device}"
done

printf '\n'
if [ "$FOUND" -eq 0 ]; then
	printf '%snothing of this tree is left on the host%s\n' "$GREEN" "$NC"
	exit 0
fi
if [ "$REMOVE" = true ]; then
	printf 'ran the removal for %d of them; rerun to see what is left\n' "$FOUND"
	exit 0
fi
printf '%s%d left. Rerun with --rm as root, or run the commands above one at a time.%s\n' \
	"$RED" "$FOUND" "$NC"
exit 1
