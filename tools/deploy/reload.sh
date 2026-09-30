#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# reload.sh -- put this working tree's module and helpers onto a running host.
#
#   sudo tools/deploy/reload.sh
#
# The edit-test loop after install.sh has run once. It takes the mounts down, swaps the module and
# the helpers, and brings them back, then says what the mounts came up as.
#
# Only three steps need root: the unit control, the module install, and the /sbin install. The two
# builds run as the invoking account, which is also why this fixes a root-owned kernel/build on the
# way past: a tree built once under sudo cannot be rebuilt without it afterwards.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
. "${ROOT}/fsname"
. "${ROOT}/shell/units.sh"
. "${ROOT}/shell/paths.sh"

BUILD=true
START=true
DAEMON_UNIT_TEMPLATE=${DAEMON_NAME}@.service

# What a cold host mounts, for a run that finds nothing up yet.
MOUNTS=()
while read -r point; do MOUNTS+=("$point"); done < <(mounted_points)
if [ ${#MOUNTS[@]} -eq 0 ]; then
	MOUNTS=(/mnt/${FS_NAME})
fi

MODULE_SYSFS=/sys/module/${FS_NAME}

usage() {
	cat <<EOF
reload.sh -- rebuild and reload the module and helpers on a running host.

  --mount PATH     a mount point to cycle; repeat for more (default: every mount that is up)
  --skip-build     reload what is already built
  --no-start       leave the mounts down
  --help           this

Run install.sh once before this. It creates the accounts, the units and the configs; this only
swaps what a build produces.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--mount)
		if [ "${MOUNTS_SET:-false}" = false ]; then
			MOUNTS=()
			MOUNTS_SET=true
		fi
		MOUNTS+=("$2")
		shift 2
		;;
	--skip-build) BUILD=false; shift ;;
	--no-start) START=false; shift ;;
	--help) usage; exit 0 ;;
	*) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	echo "reload.sh: needs root to control units and install the module" >&2
	exit 1
fi

# Whoever owns the tree, not whoever invoked sudo: a root shell reports SUDO_USER=root, and the
# builds would then leave a kernel/build the tree's owner cannot write.
ACCOUNT="$(stat -c '%U' "$ROOT")"
if [ "$ACCOUNT" = root ]; then
	echo "reload.sh: ${ROOT} is root's, so the builds cannot be handed to anyone" >&2
	exit 1
fi

asAccount() {
	sudo -u "$ACCOUNT" "$@"
}

# ── down ────────────────────────────────────────────────────────────────
# Stopping the mount unit runs umount(8), which reaches the mount helper. The helper cleans this
# node's rows while the daemon can still take the turn, then stops that daemon and unmounts.
echo "== down =="
# Read before the unmounts, since the names come from the channels they drop.
UNITS="$(daemon_loaded_units)"

for mount in "${MOUNTS[@]}"; do
	unit="$(unitNameFor "$mount")"
	systemctl stop "$unit" >/dev/null 2>&1 || true
	if mountpoint -q "$mount"; then
		echo "reload.sh: ${mount} did not unmount; something still holds it"
		exit 1
	fi
done

# A daemon the helper left running holds its channel open, and the channel holds the module.
for unit in $UNITS; do systemctl stop "$unit"; done

# The directory and not `lsmod | grep -q`: grep leaves on its first match, the SIGPIPE that kills
# lsmod makes the pipeline fail under pipefail, and a resident module then reads as absent.
if [ -d "$MODULE_SYSFS" ]; then
	rmmod ${FS_NAME}
fi
if [ -d "$MODULE_SYSFS" ]; then
	echo "reload.sh: the module is still loaded, so the unit below would keep using it" >&2
	exit 1
fi

# ── build ───────────────────────────────────────────────────────────────
if [ "$BUILD" = true ]; then
	echo "== build =="

	# A build directory this account cannot write is what one earlier root build leaves behind.
	# The module Makefile then copies into it without checking, so the failure reads as a stale
	# module rather than as a permission one.
	for dir in "${ROOT}/kernel/build" "$TOOLS_BUILD"; do
		if [ -d "$dir" ] && [ "$(stat -c '%U' "$dir")" != "$ACCOUNT" ]; then
			echo "reload.sh: ${dir} was $(stat -c '%U' "$dir")'s; giving it to ${ACCOUNT}"
			chown -R "${ACCOUNT}" "$dir"
		fi
	done

	asAccount make -C "${ROOT}/kernel" >/dev/null
	asAccount cmake -S "${ROOT}/tools" -B "$TOOLS_BUILD" >/dev/null
	asAccount cmake --build "$TOOLS_BUILD" >/dev/null
fi

# ── install ─────────────────────────────────────────────────────────────
echo "== install =="
install -d -m 0755 "$MODULE_DIR"
install -m 0644 "${ROOT}/kernel/build/${FS_NAME}.ko" "${MODULE_DIR}/${FS_NAME}.ko"
depmod -a
cmake --install "$TOOLS_BUILD" >/dev/null

dropModuleOptions

# Load it here rather than leaving it to the unit's modprobe: that one is quiet about a module
# already resident, so a failed unload upstream would show as the new build never taking effect.
#
# On srcversion and not on lsmod: an unload still settling lets modprobe re-insert the image it was
# unloading, and by name alone that reads as the new build having landed.
WANTED_SRC="$(modinfo -F srcversion "${MODULE_DIR}/${FS_NAME}.ko")"
loadedSrcVersion() {
	cat "${MODULE_SYSFS}/srcversion" 2>/dev/null || true
}

for attempt in 1 2 3 4 5; do
	if [ "$(loadedSrcVersion)" = "$WANTED_SRC" ]; then
		break
	fi
	if [ -d "$MODULE_SYSFS" ]; then
		rmmod ${FS_NAME} || true
	fi
	sleep 1
	modprobe ${FS_NAME} || true
done
if [ "$(loadedSrcVersion)" != "$WANTED_SRC" ]; then
	echo "reload.sh: the resident module is not this build (want ${WANTED_SRC}, have $(loadedSrcVersion))" >&2
	exit 1
fi

# The load registers the daemon miscdevice, and udev is what turns that into a node under /dev. A
# reload that skipped this leaves the class entry without the node, and the daemon's unit then fails
# its namespace on a DeviceAllow= naming a path that is not there.
udevadm control --reload >/dev/null
udevadm trigger --subsystem-match=misc >/dev/null 2>&1 || true
udevadm settle --timeout=5 >/dev/null 2>&1 || true
# No channel yet: the load registers one per mount, and the mounts come up further down.

# ── the helper unit ─────────────────────────────────────────────────────
# Its unit is not a build product, but starting the one already installed would run a copy this
# tree no longer describes. Rendering it here is the same swap the module above gets.
"${ROOT}/tools/render-fsname.sh" "${ROOT}/daemon/deploy/daemon/daemon@.service.in" \
	"/etc/systemd/system/${DAEMON_UNIT_TEMPLATE}"
chmod 0644 "/etc/systemd/system/${DAEMON_UNIT_TEMPLATE}"
systemctl daemon-reload

# ── up ──────────────────────────────────────────────────────────────────
if [ "$START" = false ]; then
	echo "reload.sh: left down, as asked"
	exit 0
fi

echo "== up =="
# A start-limit hit is the one failed state a start does not get past, and the mount helper's own
# start of the unit would then fail on it and unmount. Cleared first.
systemctl reset-failed "${DAEMON_NAME}@*.service" 2>/dev/null || true
for mount in "${MOUNTS[@]}"; do
	systemctl start "$(unitNameFor "$mount")"
done

# After the mounts and not before: a channel is what a mount registers, so this is the first point
# at which there is one instance per node to start. The mount helper starts each one on its way up,
# so this catches a node whose helper died rather than being the usual path.
udevadm settle --timeout=5 >/dev/null 2>&1 || true
for unit in $(daemon_units); do systemctl start "$unit" || true; done

# ── what came up ────────────────────────────────────────────────────────
# node_id in the option string is the live value, not what the mount was given: show_options reads
# sbi->node_id. A zero there means the claim inside mount(2) did not land.
echo
grep ${FS_NAME} /proc/mounts || echo "reload.sh: nothing mounted"

# One helper per mounted node, started by the mount helper rather than by this script. A mount whose
# helper is dead reads as a working mount and is not one: every metadata write then has no turn to
# take. hello_done is the helper answering, which is what the module waits for.
echo
echo "helpers:"
for unit in $(daemon_units); do
	node="${unit##*@}"
	node="${node%.service}"
	state="$(systemctl show --value -p ActiveState "$unit")"
	said="$(cat /sys/fs/${FS_NAME}/node${node}/daemon_state 2>/dev/null || echo unreadable)"
	printf '  %-34s %-10s %s\n' "$unit" "$state" "$said"
done

echo
if [ -r /sys/fs/${FS_NAME}/perm_info ]; then
	echo "RAT permissions:"
	cat /sys/fs/${FS_NAME}/perm_info
fi

# The lock region's own rows, which is what admits each node's helper. deleg_info reads whichever
# region its store half was last given, and 0 is the lock region.
if [ -w /sys/fs/${FS_NAME}/deleg_info ]; then
	echo
	echo 0 >/sys/fs/${FS_NAME}/deleg_info
	cat /sys/fs/${FS_NAME}/deleg_info
fi
