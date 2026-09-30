# SPDX-License-Identifier: Apache-2.0
# paths.sh -- where a deploy puts things on the host.
#
# Sourced after fsname, which is where FS_NAME comes from. install.sh writes to these and
# uninstall.sh and leftovers.sh read them back, so one spelling per location is what keeps a
# teardown reaching what the install left.
#
#   . "${ROOT}/fsname"
#   . "${ROOT}/shell/paths.sh"
#
# A caller that parses options sources this first: the defaults below stand only where nothing
# else was named, and a flag assigning afterwards wins.

if [ -z "${FS_NAME:-}" ]; then
	echo "paths.sh: source fsname first" >&2
	return 1 2>/dev/null || exit 1
fi

# Where the library, its headers and its pc file land, and where the module and the mount helpers
# go. Overridable, because a host that keeps them elsewhere says so once.
PREFIX="${PREFIX:-/usr/local}"
MODULE_DIR="${MODULE_DIR:-/lib/modules/$(uname -r)/extra}"
HELPER_DIR="${HELPER_DIR:-/sbin}"

# Not overridable: the units live here by systemd's rule, not by this tree's choice.
UNITDIR=/etc/systemd/system

# What uninstall.sh reads to undo an install exactly rather than by convention.
STATEDIR=/var/lib/${FS_NAME}
STATEFILE="${STATEDIR}/deploy.state"

# Scratch trees, kept out of the source tree so a root build leaves nothing its owner cannot write.
TOOLS_BUILD="${TOOLS_BUILD:-/tmp/${FS_NAME}-tools-build}"
LIB_BUILD="${LIB_BUILD:-/tmp/${FS_NAME}-lib-build}"
