#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# install.sh -- bring a host to where the filesystem is mounted and serving, with one privileged unit.
#
#   sudo tools/deploy/install.sh --account app --device /dev/dax0.0
#   sudo tools/deploy/install.sh --account app --no-start   # stop after the units are written
#
# It installs no binary of the cme project: cme-format comes from there, and the mount helper runs
# it. What this does is the account, the group, the modes and one mount unit per mount point; the
# daemon has an installer of its own, called from here.
#
# Every mode is set by install(1) rather than by a copy: a copy creates the destination through the
# umask, and a permissive one would widen what the daemon refuses to read.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
. "${ROOT}/fsname"
. "${ROOT}/shell/units.sh"
. "${ROOT}/shell/paths.sh"
. "${ROOT}/shell/deploy_state.sh"

ACCOUNT="${SUDO_USER:-}"
DEVICE=/dev/dax0.0
START=true
RESET_CFG=false
BUILD=true
MOUNTS=(/mnt/${FS_NAME})

usage() {
	cat <<EOF
install.sh -- groups, modes and units for running ${FS_NAME} unprivileged.

  --account NAME   the account every non-mount process runs as (default: the sudo caller)
  --device PATH    the DAX device the mounts sit on (default: /dev/dax0.0)
  --mount PATH     a mount point; repeat for more (default: /mnt/${FS_NAME})
  --no-start       write everything but leave the mounts down
  --reset-cfg      replace /etc/<fs>/*.yaml and policy.rego with this tree's examples
  --skip-build     install what is already built rather than building it first
  --prefix PATH    where the library and its headers go (default: /usr/local)
  --help           this

The device is unbound from device_dax, because ${FS_NAME} maps its two areas itself: the metadata area
uncached and the file data write-back. That driver reserves the whole device write-back while it is
bound, and an uncached mapping over that reservation comes back cached with no error, so the mount
refuses itself instead. Nothing else may hold the device afterwards.

uninstall.sh beside this script takes it all away again.

The account is one decision made in two places: it is the User= of each daemon and the account
every application runs as. An empty admit in the daemon config admits that uid and nobody else,
which is what lets an application connect with no admit list and no socket group.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--account) ACCOUNT="$2"; shift 2 ;;
	--device) DEVICE="$2"; shift 2 ;;
	--no-start) START=false; shift ;;
	--reset-cfg) RESET_CFG=true; shift ;;
	--skip-build) BUILD=false; shift ;;
	--prefix) PREFIX="$2"; shift 2 ;;
	--mount)
		if [ "${MOUNTS_SET:-false}" = false ]; then
			MOUNTS=()
			MOUNTS_SET=true
		fi
		MOUNTS+=("$2")
		shift 2
		;;
	--help) usage; exit 0 ;;
	*) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	echo "install.sh: needs root to create groups and install units" >&2
	exit 1
fi
if [ -z "$ACCOUNT" ]; then
	echo "install.sh: no account; pass --account NAME" >&2
	exit 1
fi
if ! id "$ACCOUNT" >/dev/null 2>&1; then
	echo "install.sh: no such account: $ACCOUNT" >&2
	exit 1
fi

# The area name picks the socket and the single-daemon lock, so two mount points must not fold to
# one name. The path with separators turned into underscores is unique for that reason.
# The driver a dax device is bound to, empty when it is bound to none. Read from sysfs and not from
# /dev, because unbinding takes the /dev node away and leaves the sysfs device behind.
daxDriverOf() {
	local link="/sys/bus/dax/devices/$(basename "$1")/driver"
	if [ -L "$link" ]; then
		basename "$(readlink -f "$link")"
	fi
}

# ── what the device was ─────────────────────────────────────────────────
# Read before the group below exists, so a gid that group takes over is not recorded as the original.
deviceGid="$(readOriginalGroup "$DEVICE")"

# ── accounts ────────────────────────────────────────────────────────────
# Supplementary, so the account's primary group stays its own. The mount points below are set to
# this group, which is what lets the account create there without owning the directory.
getent group "${FS_NAME}" >/dev/null || groupadd --system "${FS_NAME}"
# case and not a pipe into grep -q: grep leaves on its first match, and the SIGPIPE that kills the
# writer makes the pipeline fail under pipefail, so a member reads as not one.
case " $(id -nG "$ACCOUNT") " in
*" ${FS_NAME} "*) ;;
*) usermod -aG "${FS_NAME}" "$ACCOUNT" ;;
esac

# ── the device and the socket directory ─────────────────────────────────
# What the device looked like first, so uninstall.sh restores it instead of guessing. Written once:
# a second install must not record what the first one already changed.
install -d -o root -g root -m 0755 "$STATEDIR"
if [ ! -e "$STATEFILE" ]; then
	{
		echo "# Written by tools/deploy/install.sh. uninstall.sh reads it to put the device back."
		echo "device=${DEVICE}"
		echo "deviceGroup=${deviceGid}"
		# The driver as it was, so uninstall.sh can put the device back under it. It rebinds
		# only a device it finds unbound, so a device already free stays free.
		echo "daxDriver=$(daxDriverOf "$DEVICE")"
		echo "account=${ACCOUNT}"
		echo "prefix=${PREFIX}"
		echo "moduleDir=${MODULE_DIR}"
		# Quoted, because uninstall.sh sources this file and the list holds spaces. Bare, the
		# second path is read as a command to run.
		printf 'mounts="%s"\n' "${MOUNTS[*]}"
		# Only these go at uninstall, so a mount point an operator made stays in place.
		createdMounts=()
		for mount in "${MOUNTS[@]}"; do
			[ -e "$mount" ] || createdMounts+=("$mount")
		done
		printf 'createdMounts="%s"\n' "${createdMounts[*]}"
	} >"$STATEFILE"
	chmod 0644 "$STATEFILE"
fi

# Group only, never the mode. Nothing here opens the device: mount(2) is gated on CAP_SYS_ADMIN
# rather than on the node, and a file: region never touches it. Narrowing the mode would take
# access away from whatever else uses this device and give this deployment nothing.
chgrp ${FS_NAME} "$DEVICE"

# ── what a host runs ────────────────────────────────────────────────────
# The module, the two mount helpers, and the library with its headers, so one run of this script is
# the whole bring-up and uninstall.sh has one list to undo.
#
# Built as the account: a root build leaves a directory this account cannot write into afterwards.
asAccount() {
	if [ -n "$ACCOUNT" ] && [ "$ACCOUNT" != root ]; then
		sudo -u "$ACCOUNT" "$@"
	else
		"$@"
	fi
}

if [ "$BUILD" = true ]; then
	echo "install.sh: building the module, the helpers and the library"
	for dir in "${ROOT}/kernel/build" "$TOOLS_BUILD" "$LIB_BUILD"; do
		if [ -d "$dir" ] && [ "$(stat -c '%U' "$dir")" != "${ACCOUNT:-root}" ]; then
			chown -R "${ACCOUNT:-root}" "$dir"
		fi
	done
	asAccount make -C "${ROOT}/kernel" > /dev/null
	asAccount cmake -S "${ROOT}/tools" -B "$TOOLS_BUILD" > /dev/null
	asAccount cmake --build "$TOOLS_BUILD" > /dev/null
	asAccount cmake -S "$ROOT" -B "$LIB_BUILD" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DFS_PUBLISH_NAMESPACE=ON > /dev/null
	asAccount cmake --build "$LIB_BUILD" > /dev/null
fi

install -d -m 0755 "$MODULE_DIR"
install -m 0644 "${ROOT}/kernel/build/${FS_NAME}.ko" "${MODULE_DIR}/${FS_NAME}.ko"
depmod -a
echo "install.sh: ${MODULE_DIR}/${FS_NAME}.ko installed"

cmake --install "$TOOLS_BUILD" > /dev/null
cmake --install "$LIB_BUILD" --prefix "$PREFIX" > /dev/null
echo "install.sh: mount.${FS_NAME} and lib${LIB_NAME} installed under ${PREFIX}"

# ── the device and the module ───────────────────────────────────────────
# the module maps the device's two areas itself, so the device has to leave device_dax's hands. The
# mount refuses itself otherwise: it checks the PTE it got and fails rather than running cached.
driver="$(daxDriverOf "$DEVICE")"

# fuser and not a retry: unbinding a device somebody has mapped takes the ZONE_DEVICE memmap out
# from under that mapping, and the host locks up hard rather than returning an error.
if [ -n "$driver" ] && [ -e "$DEVICE" ] && fuser -s "$DEVICE" 2>/dev/null; then
	echo "install.sh: ${DEVICE} is in use; unbinding it would lock this host up" >&2
	fuser -v "$DEVICE" >&2 || true
	exit 1
fi

# A bare unbind from kmem does not hand the range back: the hot-remove fails while the memory is
# online, and the kernel keeps that range System RAM until the next reboot. daxctl offlines it first.
if [ "$driver" = "kmem" ]; then
	if ! command -v daxctl >/dev/null 2>&1; then
		echo "install.sh: ${DEVICE} is under kmem and daxctl is missing; install ndctl first" >&2
		exit 1
	fi
	daxctl reconfigure-device --mode=devdax --force "$(basename "$DEVICE")" >/dev/null
	echo "install.sh: ${DEVICE} taken back from kmem"
	driver="$(daxDriverOf "$DEVICE")"
fi

if [ -n "$driver" ]; then
	echo "$(basename "$DEVICE")" >"/sys/bus/dax/drivers/${driver}/unbind"
	echo "install.sh: ${DEVICE} unbound from ${driver}"
else
	echo "install.sh: ${DEVICE} is already bound to no driver"
fi

# The unbind comes first, so the module never sees the device while device_dax still holds it.
dropModuleOptions
if modinfo -n ${FS_NAME} >/dev/null 2>&1; then
	modprobe ${FS_NAME}
	echo "install.sh: ${FS_NAME} module loaded"
else
	echo "install.sh: no ${FS_NAME} module under /lib/modules; a run without --skip-build builds and installs it"
fi

# ── the mount points ────────────────────────────────────────────────────
for mount in "${MOUNTS[@]}"; do
	# Only when nothing is mounted there. On a live mount this would chown the filesystem's own
	# root inode instead of the directory under it, and the module rebuilds that inode at 0755 root
	# on every mount, so the change would look applied and be gone after a remount.
	if mountpoint -q "$mount"; then
		echo "install.sh: ${mount} is mounted; leaving its mount point alone"
	else
		install -d -o "$ACCOUNT" -g ${FS_NAME} -m 2775 "$mount"
	fi
done

# ── units ───────────────────────────────────────────────────────────────
rendered="$(mktemp -d)"
trap 'rm -rf "${rendered}"' EXIT
"${ROOT}/tools/render-fsname.sh" "${HERE}/mount.example.mount.in" "${rendered}/mount.mount"

for mount in "${MOUNTS[@]}"; do
	area="$(areaNameFor "$mount")"
	unit="$(unitNameFor "$mount")"

	sed -e "s|/mnt/${FS_NAME}\b|${mount}|g" \
	    -e "s|mnt_${FS_NAME}\b|${area}|g" \
	    -e "s|^Options=daxdev=[^,]*|Options=daxdev=${DEVICE}|" \
	    "${rendered}/mount.mount" >"${UNITDIR}/${unit}"
	chmod 0644 "${UNITDIR}/${unit}"
	echo "install.sh: wrote ${UNITDIR}/${unit}"
done

# The daemon's unit is a template on the node id, and the kernel picks that id inside mount(2), so
# the unit itself cannot name the mount it serves. Every mount point of this host is opened instead.
# Written before the daemon installer below, whose restart of an already-attended channel would
# otherwise hit EROFS against a unit with no writable mount yet.
daemonDropIn="${UNITDIR}/${DAEMON_NAME}@.service.d"
install -d -m 0755 "$daemonDropIn"
{
	echo "# SPDX-License-Identifier: Apache-2.0"
	echo "# Written by install.sh: the mounts an instance may take a turn in."
	echo "[Service]"
	for mount in "${MOUNTS[@]}"; do
		echo "ReadWritePaths=-${mount}"
	done
} >"${daemonDropIn}/mounts.conf"
chmod 0644 "${daemonDropIn}/mounts.conf"
echo "install.sh: wrote ${daemonDropIn}/mounts.conf"

systemctl daemon-reload

# ── the daemon ────────────────────────────────────────────────────
# After the module and the mount drop-in above: a mount with nobody attending it answers -EAGAIN to
# every create, and the daemon's own installer here restarts an already-running instance in place.
#
# No account is passed: letting the applications' account run the daemon would put the policy
# decision point and the workloads it judges under one uid.
daemonFlags=()
[ "$START" = false ] && daemonFlags+=(--no-start)
[ "$RESET_CFG" = true ] && daemonFlags+=(--reset-cfg)
if APP_USER="$ACCOUNT" bash "${ROOT}/daemon/deploy/install.sh" "${daemonFlags[@]+"${daemonFlags[@]}"}"; then
	echo "install.sh: ${DAEMON_NAME} installed"
else
	echo "install.sh: ${DAEMON_NAME} did not install; the mounts below stay down" >&2
	exit 1
fi

# systemd ignores a key it does not know and keeps only the first word of an unquoted Environment=,
# both silently. Verifying here turns either into a line the operator reads now rather than into a
# mount that fails later for a reason two journal hops away.
for mount in "${MOUNTS[@]}"; do
	systemd-analyze verify "${UNITDIR}/$(unitNameFor "$mount")" 2>&1 |
		grep -E "$(unitNameFor "$mount")" || true
done

# ── what the units name but this script does not install ────────────────
# Reported here rather than left to `systemctl start`, where the same gap surfaces as a unit that
# failed for a reason two journal hops away.
missing=()
for path in /usr/local/bin/cme-format /sbin/mount.${FS_NAME}; do
	[ -x "$path" ] || missing+=("$path")
done
[ -d /sys/module/${FS_NAME} ] || missing+=("the ${FS_NAME} module under /lib/modules")

if [ ${#missing[@]} -ne 0 ]; then
	echo
	echo "install.sh: not starting; these are still missing:"
	printf '  %s\n' "${missing[@]}"
	echo
	echo "  cmake --install <cme-build> --prefix /usr/local   # cme-format"
	echo "  tools/deploy/install.sh without --skip-build       # mount.${FS_NAME} and the module"
	START=false
fi

# ── the mounts ──────────────────────────────────────────────────────────
# Starting the mount is the whole bring-up. the mount helper owns the order that follows mount(2) --
# format the lock region, bind it to this account, start the daemon, create the metadata domain --
# and each step needs what the one before it left, so nothing here may run any of them separately.
if [ "$START" = true ]; then
	for mount in "${MOUNTS[@]}"; do
		unit="$(unitNameFor "$mount")"

		if systemctl start "$unit"; then
			echo "install.sh: ${mount} up, attended by ${DAEMON_NAME}@$(nodeOfMount "$mount")"
		else
			echo "install.sh: ${unit} did not start; journalctl -u ${unit}" >&2
		fi
	done
fi

cat <<EOF

install.sh: done.

${ACCOUNT} has to log in again before the new group membership is in its credentials.
EOF

if [ "$START" = false ]; then
	printf '\n  systemctl start %s\n' "$(unitNameFor "${MOUNTS[0]}")"
fi
