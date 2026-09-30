# SPDX-License-Identifier: Apache-2.0
# units.sh -- what this filesystem has on the host: its units, and the mounts behind them.
#
# Sourced after fsname, which is where FS_NAME comes from. Every deploy script and every test runner
# needs the same answers, and a host provisions as many mounts as it has node ids, so a script that
# names one instance works on a developer's host and stops working on a provisioned one.
#
#   . "${ROOT}/fsname"
#   . "${ROOT}/shell/units.sh"

if [ -z "${FS_NAME:-}" ]; then
	echo "units.sh: source fsname first" >&2
	return 1 2>/dev/null || exit 1
fi

# One helper instance per mounted node, named after the channel its mount registered. Read from
# /dev and not from systemd, so the list is the channels that exist rather than the units that once
# did.
daemon_units() {
	local dev
	for dev in /dev/${DAEMON_NAME}-*; do
		[ -e "$dev" ] || continue
		echo "${DAEMON_NAME}@${dev##*/${DAEMON_NAME}-}.service"
	done
}

# The instances systemd knows, for stopping: a channel that already went leaves its unit behind.
daemon_loaded_units() {
	systemctl list-units --plain --no-legend --all "${DAEMON_NAME}@*.service" 2>/dev/null \
		| awk '{print $1}'
}

# True when at least one instance is serving. Asked of systemd rather than of the channels: a
# helper can be up while the channel list has moved under it.
daemon_active() {
	systemctl list-units --plain --no-legend --state=active "${DAEMON_NAME}@*.service" 2>/dev/null \
		| grep -q .
}

# The instance name a per-mount unit and its config are keyed on: the mount point with its
# separators turned into underscores, which two mount points cannot both produce.
areaNameFor() {
	local trimmed="${1#/}"
	echo "${trimmed//\//_}"
}

# The mount unit that carries @1, which systemd names after the path.
unitNameFor() {
	systemd-escape --path --suffix=mount "$1"
}

# The node id the mount at @1 claimed, read off the kernel's own mount table, or empty when nothing
# is mounted there. That id names the daemon instance attending it.
nodeOfMount() {
	awk -v target="$1" -v kind="${FS_NAME}" \
		'$2 == target && $3 == kind {
			split($4, options, ",")
			for (index_ in options) {
				if (options[index_] ~ /^node_id=/) {
					sub(/^node_id=/, "", options[index_])
					print options[index_]
					exit
				}
			}
		}' /proc/self/mounts
}

# Every mount of this filesystem that is up, read from the kernel. One left behind holds the module,
# so a script that takes mounts down asks this rather than assuming which ones it made.
mounted_points() {
	awk -v want="$FS_NAME" '$3 == want { print $2 }' /proc/self/mounts
}

# modprobe refuses to load a module whose conf names a parameter it does not have, and this one
# takes none. Called before a load, by a caller that is already root.
dropModuleOptions() {
	local conf="/etc/modprobe.d/${FS_NAME}.conf"
	grep -qs "^options ${FS_NAME}[[:space:]]" "$conf" || return 0
	sed -i "/^options ${FS_NAME}[[:space:]]/d" "$conf"
	# Nothing else is written there, so comments alone leave the file with no reader.
	grep -qv '^[[:space:]]*\(#.*\)\?$' "$conf" || rm -f "$conf"
}
