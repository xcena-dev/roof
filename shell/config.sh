# SPDX-License-Identifier: Apache-2.0
# config.sh -- the host config, read through the daemon's own parser.
#
# Sourced after fsname. Every value a deploy script uses comes from this one file, and the daemon
# answers for each, so no script matches the file's text itself.
#
#   . "${ROOT}/fsname"
#   . "${ROOT}/shell/config.sh"

if [ -z "${FS_NAME:-}" ]; then
	echo "config.sh: source fsname first" >&2
	return 1 2>/dev/null || exit 1
fi

CONFIG_FILE="/etc/${FS_NAME}/config.yaml"
DAEMON_BIN="${DAEMON_BIN:-/usr/local/bin/${DAEMON_NAME}}"

# The lines --print-config answers for @key. Read as the file's owner, since the daemon takes its
# config from that account or from root and from nobody else.
readSetting() {
	local key="$1" owner
	owner="$(stat -c '%U' "$CONFIG_FILE")" || return 1
	if [ "$owner" = root ]; then
		"$DAEMON_BIN" --config "$CONFIG_FILE" --print-config "$key"
	else
		runuser -u "$owner" -- "$DAEMON_BIN" --config "$CONFIG_FILE" --print-config "$key"
	fi
}

# @template on stdout for the daemon account @user: the fsname placeholders and @DAEMON_USER@,
# @DAEMON_GROUP@, which a unit and a udev rule have to spell literally.
renderForAccount() {
	local template="$1" user="$2" group
	group="$(id -gn "$user" 2>/dev/null || echo "$user")"
	sed -e "s|@DAEMON_USER@|${user}|g" \
		-e "s|@DAEMON_GROUP@|${group}|g" \
		-e "s|@FS_NAME@|${FS_NAME}|g" \
		-e "s|@DAEMON_NAME@|${DAEMON_NAME}|g" \
		"$template"
}

# The command an operator runs to try the config by hand, as the account it has to run as.
describeCheck() {
	printf 'sudo -u %s %s --config %s --check-config' "$(stat -c '%U' "$CONFIG_FILE" 2>/dev/null || echo root)" \
		"$DAEMON_BIN" "$CONFIG_FILE"
}

# --check-config, read the same way. Its stderr says what each backend loaded.
checkSettings() {
	local owner
	owner="$(stat -c '%U' "$CONFIG_FILE")" || return 1
	if [ "$owner" = root ]; then
		"$DAEMON_BIN" --config "$CONFIG_FILE" --check-config
	else
		runuser -u "$owner" -- "$DAEMON_BIN" --config "$CONFIG_FILE" --check-config
	fi
}
