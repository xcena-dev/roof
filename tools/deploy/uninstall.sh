#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# uninstall.sh -- take back what install.sh put on this host.
#
#   sudo tools/deploy/uninstall.sh            # units and the state file
#   sudo tools/deploy/uninstall.sh --purge    # configs, groups and the socket directory too
#
# It reads /var/lib/${FS_NAME}/deploy.state, which install.sh wrote before it changed anything, so the
# device goes back to the owner and mode it actually had rather than to a guess.
#
# Configs survive without --purge. An operator edits them, and a reinstall that found its own file
# would keep it, so removing them by default would throw away the one thing here worth keeping.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
. "${ROOT}/fsname"
. "${ROOT}/shell/units.sh"
. "${ROOT}/shell/paths.sh"
. "${ROOT}/shell/deploy_state.sh"

KEEP_STATE=false

usage() {
	cat <<EOF
uninstall.sh -- take back everything install.sh put on this host.

  --keep-state   leave /var/lib/${FS_NAME}/deploy.state behind, so a later run can still restore
  --help         this

The mounts come down, then the daemons, then the units, then daemon, then the module. The device
goes back under the driver install.sh took it from, and the module file, the mount helpers, the
library, its headers and its pc file are removed with the configs, the groups and the account. The
mount points it created go too, each once it is empty.

Not removed: cme-format. A different project installs it.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--keep-state) KEEP_STATE=true; shift ;;
	--help) usage; exit 0 ;;
	*) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	echo "uninstall.sh: needs root to remove units and groups" >&2
	exit 1
fi

# ── what install.sh recorded ────────────────────────────────────────────
devices=
deviceGroups=
daxDrivers=
account=
mounts=
createdMounts=unrecorded
prefix=
moduleDir=

if [ -r "$STATEFILE" ]; then
	# shellcheck disable=SC1090
	. "$STATEFILE"
	# Where install.sh actually put them, which a --prefix run makes different from the default.
	PREFIX="${prefix:-$PREFIX}"
	MODULE_DIR="${moduleDir:-$MODULE_DIR}"
else
	echo "uninstall.sh: no ${STATEFILE}; the devices will be left as they are"
fi
ACCOUNT="$account"
# One word per device in each list, in the same order.
read -r -a DEVICES <<<"$devices"
read -r -a DEVICE_GROUPS <<<"$deviceGroups"
read -r -a DAX_DRIVERS <<<"$daxDrivers"

# Without the state file the mount points are unknown, so fall back to whatever units are here.
if [ -n "$mounts" ]; then
	read -r -a MOUNTS <<<"$mounts"
else
	MOUNTS=()
	for unit in "${UNITDIR}"/*.mount; do
		[ -e "$unit" ] || continue
		grep -q "^Type=${FS_NAME}\$" "$unit" || continue
		MOUNTS+=("$(sed -n 's/^Where=//p' "$unit")")
	done
fi

# ── the mounts ──────────────────────────────────────────────────────────
# Stopping the mount is the whole teardown, the mirror of install.sh starting it. The umount helper
# owns the order: it clears this node's rows while the daemon can still take the turn, stops the
# daemon, and only then unmounts. So nothing here stops a daemon ahead of that helper.
for mount in "${MOUNTS[@]}"; do
	unit="$(unitNameFor "$mount")"

	if systemctl is-active --quiet "$unit"; then
		systemctl disable --now "$unit" >/dev/null 2>&1 || true
		echo "uninstall.sh: ${mount} down, and its daemon with it"
	else
		systemctl disable "$unit" >/dev/null 2>&1 || true
	fi

	# A mount made by hand, which no unit knows about. umount(8) picks the helper from the
	# filesystem type, so that helper still runs here and stops the daemon in its own order.
	if mountpoint -q "$mount"; then
		echo "uninstall.sh: ${mount} was mounted outside its unit; unmounting"
		umount "$mount" || echo "uninstall.sh: umount ${mount} failed" >&2
	fi
done

# A mount still up needs its daemon, its helpers and the module for a later unmount, so nothing
# below may go while one stands.
remaining="$(mounted_points)"
if [ -n "$remaining" ]; then
	echo "uninstall.sh: still mounted, so nothing else was removed:" >&2
	echo "$remaining" | sed 's/^/  /' >&2
	echo "uninstall.sh: close what holds them, unmount them, and run this again" >&2
	exit 1
fi

# ── the units ───────────────────────────────────────────────────────────
for mount in "${MOUNTS[@]}"; do
	rm -f "${UNITDIR}/$(unitNameFor "$mount")"
done
rm -rf "${UNITDIR}/${DAEMON_NAME}@.service.d"
systemctl daemon-reload

# ── the mount points ────────────────────────────────────────────────────
# The ones install.sh created, each once it is unmounted and empty. A state file without that list
# cannot tell them from an operator's, so every one stays and leftovers.sh lists it.
if [ "$createdMounts" = unrecorded ]; then
	createdMounts=""
	echo "uninstall.sh: no record of which mount points install.sh made, so none is removed" >&2
fi
read -r -a CREATED_MOUNTS <<<"$createdMounts"
removeCreatedMounts uninstall.sh "${CREATED_MOUNTS[@]}"

# ── the daemon ────────────────────────────────────────────────────
# Before the module: the daemon's open of the daemon device holds a reference on it, so the unload
# below would fail while it serves.
#
# Handed to its own uninstaller, which owns the binary, the udev rule and the account, the same way
# install.sh hands it the install. --purge carries through to the configs and the account.
if [ -x "${ROOT}/daemon/deploy/uninstall.sh" ]; then
	bash "${ROOT}/daemon/deploy/uninstall.sh" --purge ||
		echo "uninstall.sh: the daemon uninstaller reported a problem" >&2
elif systemctl is-active --quiet "${DAEMON_NAME}@*.service"; then
	systemctl stop "${DAEMON_NAME}@*.service"
	echo "uninstall.sh: every ${DAEMON_NAME} instance stopped"
fi

# ── the module ──────────────────────────────────────────────────────────
# Before the rebind, because putting device_dax back while the module still maps the device would hand
# the same range to two owners. /sys and not `lsmod | grep -q`: grep leaves on its first match and
# the SIGPIPE that kills lsmod makes the pipeline fail under pipefail.
if [ -d /sys/module/${FS_NAME} ]; then
	# rmmod and not modprobe -r: modprobe resolves the name under /lib/modules, and the module file
	# there is this script's to remove, so a run that removed it first could no longer unload.
	if rmmod ${FS_NAME} 2>/dev/null; then
		echo "uninstall.sh: ${FS_NAME} module unloaded"
	else
		echo "uninstall.sh: ${FS_NAME} is still in use; leaving it loaded and the device unbound"
	fi
fi

# ── the devices ─────────────────────────────────────────────────────────
for index in "${!DEVICES[@]}"; do
	device="${DEVICES[$index]}"
	driver="${DAX_DRIVERS[$index]:--}"
	group="${DEVICE_GROUPS[$index]:-}"

	# Rebind first: an unbound dax device has no /dev node, so the chgrp below has nothing to act on
	# until device_dax recreates it.
	if [ "$driver" != - ] && [ -d /sys/module/${FS_NAME} ]; then
		echo "uninstall.sh: ${FS_NAME} still loaded; ${device} left unbound"
	elif [ "$driver" != - ]; then
		name="$(basename "$device")"
		if [ -e "/sys/bus/dax/devices/${name}" ] && [ ! -L "/sys/bus/dax/devices/${name}/driver" ]; then
			echo "$name" >"/sys/bus/dax/drivers/${driver}/bind"
			echo "uninstall.sh: ${device} rebound to ${driver}"
		fi
	fi

	# The group is all install.sh changed, so the group is all that goes back. Its mode is left
	# where it stands, which may be somewhere an operator moved it.
	if [ -e "$device" ] && [ -n "$group" ]; then
		chgrp "$(chooseRestoredGroup "$group")" "$device"
		echo "uninstall.sh: ${device} group restored to $(stat -c '%G' "$device")"
	fi
done

# ── what a build put on the host ────────────────────────────────────────
# After the module is unloaded: removing the file under /lib/modules while it is resident leaves a
# loaded module with nothing on disk behind it, and modinfo then has nothing to answer with.
rm -f "${MODULE_DIR}/${FS_NAME}.ko"
depmod -a
rmdir --ignore-fail-on-non-empty "$MODULE_DIR" 2>/dev/null || true

# mount(8) finds a type helper in /sbin alone, which is why install.sh puts them there.
rm -f "${HELPER_DIR}/mount.${FS_NAME}" "${HELPER_DIR}/umount.${FS_NAME}"

# The library an application links, and what it needs to compile against.
rm -f "${PREFIX}/lib/lib${LIB_NAME}.a" "${PREFIX}/lib/pkgconfig/lib${LIB_NAME}.pc"
rm -rf "${PREFIX}/include/${LIB_NAME}"
rmdir --ignore-fail-on-non-empty "${PREFIX}/lib/pkgconfig" 2>/dev/null || true
echo "uninstall.sh: the module file, the helpers and the library are gone"

# ── the group ───────────────────────────────────────────────────────────
if getent group "${FS_NAME}" >/dev/null; then
	[ -n "$ACCOUNT" ] && gpasswd -d "$ACCOUNT" "${FS_NAME}" >/dev/null 2>&1 || true
	groupdel "${FS_NAME}" 2>/dev/null || echo "uninstall.sh: group ${FS_NAME} still has members"
fi

if [ "$KEEP_STATE" = false ]; then
	rm -f "$STATEFILE"
	rmdir --ignore-fail-on-non-empty "$STATEDIR" 2>/dev/null || true
fi

cat <<EOF

uninstall.sh: done. Nothing this tree installed is left.

Left in place on purpose: cme-format, which a different project installs.

leftovers.sh beside this script checks every place independently, for a host whose state file was
lost or which an older revision of this script left something on:

  tools/deploy/leftovers.sh            list
  sudo tools/deploy/leftovers.sh --rm  remove
EOF
