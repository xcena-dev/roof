# SPDX-License-Identifier: Apache-2.0
# deploy_state.sh -- the decisions install.sh records and uninstall.sh and leftovers.sh read back.
#
# Sourced after fsname. Kept out of the scripts so a test can drive each decision on scratch files,
# without root and without a device.
#
#   . "${ROOT}/fsname"
#   . "${ROOT}/shell/deploy_state.sh"

if [ -z "${FS_NAME:-}" ]; then
	echo "deploy_state.sh: source fsname first" >&2
	return 1 2>/dev/null || exit 1
fi

# The group @device has before this tree makes its own, by number. A new group can be handed the gid
# a deleted one left on the device, so a gid no group names is a leftover and root goes back instead.
readOriginalGroup() {
	local device="$1" gid
	gid="$(stat -c '%g' "$device" 2>/dev/null)" || gid=0
	getent group "$gid" >/dev/null || gid=0
	printf '%s\n' "$gid"
}

# The group to put back on the device from what a state file recorded. This tree's own group,
# UNKNOWN, or a gid nothing names is no original, so root is.
chooseRestoredGroup() {
	local recorded="$1"
	case "$recorded" in
	"${FS_NAME}" | UNKNOWN) recorded=0 ;;
	esac
	getent group "$recorded" >/dev/null || recorded=0
	printf '%s\n' "$recorded"
}

# Whether a device group named @group is one this tree left: its own, or a gid whose group is gone,
# which stat names UNKNOWN.
isLeftoverGroup() {
	case "$1" in
	"${FS_NAME}" | UNKNOWN) return 0 ;;
	esac
	return 1
}

# Removes each mount point after @caller that is unmounted and empty, and says under @caller what
# stayed and why.
removeCreatedMounts() {
	local caller="$1" mount
	shift
	for mount in "$@"; do
		if mountpoint -q "$mount"; then
			echo "${caller}: ${mount} is still mounted; its mount point stays"
		elif rmdir "$mount" 2>/dev/null; then
			echo "${caller}: ${mount} removed"
		elif [ -e "$mount" ]; then
			echo "${caller}: ${mount} is not empty; it stays"
		fi
	done
}
