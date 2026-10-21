# SPDX-License-Identifier: Apache-2.0
# kernel_log.sh -- what the kernel logged after a point a test chose.
#
# A test writes a line of its own into the kernel log and later reads only what follows it. A line
# count cannot stand in for that line: once the ring buffer is full, the count stays put while old
# lines drop off the front, and every new line falls past it.

# Writes a line no other run writes into the kernel log and prints it, for loggedSince. Needs root.
logMark() {
	local mark
	mark="$(basename "$0") mark $(date +%s%N)"
	printf '%s\n' "$mark" >/dev/kmsg
	printf '%s\n' "$mark"
}

# True when a kernel line after the mark @1 holds @2. Both are matched as plain text.
loggedSince() {
	dmesg | awk -v mark="$1" -v want="$2" '
		seen && index($0, want) { found = 1; exit }
		index($0, mark) { seen = 1 }
		END { exit !found }'
}
