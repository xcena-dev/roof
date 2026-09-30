#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Every gate this tree has, in one run: headers, docs, build, format, tidy, daemon, suite.
#
# Each gate reports on its own and the script keeps going, so one run says everything that is wrong
# rather than the first thing. The exit code is the number of gates that failed.
#
#   ./check.sh                  check only
#   ./check.sh --fix            let clang-format and doc-fsname.py rewrite what they would have complained about
#   ./check.sh --build <dir>    somewhere other than ./build
#   ./check.sh --gate tidy      one gate by name: headers, docs, build, format, tidy, daemon, suite

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$ROOT/fsname"
. "$ROOT/shell/units.sh"
FS_UPPER="${FS_NAME^^}"

# The versioned binaries. A bare clang-format is a different release here and disagrees with this
# one over code that is fine.
FORMAT_BIN="${FS_CLANG_FORMAT:-clang-format-18}"
TIDY_BIN="${FS_CLANG_TIDY:-clang-tidy-18}"

BUILD_DIR="${FS_BUILD:-$ROOT/build}"
JOBS="${FS_JOBS:-8}"

# What the repository root's config governs.
SOURCE_DIRS=(tests libroof tools daemon)

# The module's own, which kernel/.clang-format governs. The formatter resolves its config from each
# file's path, so one binary holds both trees to their own rules. kernel/include/ is left out: the
# headers there are rendered from templates, and formatting one would drift it from its .in file.
KERNEL_DIRS=(kernel/src kernel/test kernel/debug)

# What a file with no database entry is compiled with. -DNDEBUG is the build's own rather than
# decoration: without it the analyzer diagnoses code that nothing compiles that way.
# A helper's own name is a per-target definition, and a header is compiled here under no target, so
# the pass carries a stand-in for it. Only the message strings read it.
TIDY_FLAGS=(-std=c++17 -O2 -DNDEBUG
            -DFS_PROGRAM_NAME=\"header-pass\"
            -I"$ROOT/tests"
            -I"$ROOT/libroof/include"
            -I"$ROOT/kernel/include"
            -I/usr/local/include)

# The daemon's C++ names its own headers from its src/ root, takes the rendered uapi headers from a
# build tree, and reaches cme's and the rego FFI's public headers, so its header pass carries those
# paths on top. Miss one and include-cleaner cannot parse the header behind it, then reports the
# includes that header's dependants do use as unused.
DAEMON_CME_DIR="${DAEMON_CME_DIR:-$HOME/cme}"
DAEMON_TIDY_FLAGS=(-std=c++17 -O2 -DNDEBUG
                  -I"$ROOT/daemon/src"
                  -I"$ROOT/daemon/regorus_ffi/include"
                  -I"$BUILD_DIR/daemon/gen"
                  -I"$DAEMON_CME_DIR/include"
                  -I/usr/local/include)

APPLY_FORMAT=false
ONLY_GATE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --fix)   APPLY_FORMAT=true; shift ;;
        --build) BUILD_DIR="$2"; shift 2 ;;
        --gate)  ONLY_GATE="$2"; shift 2 ;;
        --help|-h)
            sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

RED=$'\033[0;31m'
GREEN=$'\033[0;32m'
YELLOW=$'\033[1;33m'
NC=$'\033[0m'

FAILED=0

wanted()
{
    [ -z "$ONLY_GATE" ] || [ "$ONLY_GATE" = "$1" ]
}

announce()
{
    printf '\n%s=== %s ===%s\n' "$YELLOW" "$1" "$NC"
}

# @outcome is the gate's own exit code, so a gate that cannot run counts the same as one that failed.
settle()
{
    local named="$1" outcome="$2"
    if [ "$outcome" -eq 0 ]; then
        printf '%s  PASS%s %s\n' "$GREEN" "$NC" "$named"
    else
        printf '%s  FAIL%s %s\n' "$RED" "$NC" "$named"
        FAILED=$((FAILED + 1))
    fi
}

# Every .cpp and .hpp the root config governs, NUL separated so a path with a space survives. A
# build tree sits inside one of these directories and holds generated sources, so it is pruned.
list_sources()
{
    ( cd "$ROOT" && find "${SOURCE_DIRS[@]}" -name 'build' -prune -o -name 'build-*' -prune -o \
        \( -name '*.cpp' -o -name '*.hpp' \) -print0 )
}

# The module's .c and .h. Format only: tidy needs a compile database, and Kbuild builds the module
# rather than the cmake that writes one.
list_kernel_sources()
{
    ( cd "$ROOT" && find "${KERNEL_DIRS[@]}" \( -name '*.c' -o -name '*.h' \) -print0 )
}

# Every translation unit in a compile database, checked against it. The one binary the rest of this
# script already names drives it, so no separately packaged run-clang-tidy wrapper has to be present.
tidy_database()
{
    local build="$1" log="$2"
    if [ ! -f "$build/compile_commands.json" ]; then
        printf '  %sno compile database at %s; run the build gate first%s\n' "$RED" "$build" "$NC"
        return 1
    fi
    python3 -c 'import json,sys
for entry in json.load(open(sys.argv[1])):
    print(entry["file"])' "$build/compile_commands.json" |
        xargs -r -P "$JOBS" -n 1 "$TIDY_BIN" -p "$build" --quiet > "$log" 2>&1
    ! grep -q -E 'warning:|error:' "$log"
}

# The .hpp files under @dirs, each handed to clang-tidy as the file under diagnosis. A header has no
# database entry, so the flags a caller passes before -- are what it is compiled with.
tidy_headers()
{
    local build="$1" log="$2"
    shift 2
    local flags=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do
        flags+=("$1")
        shift
    done
    shift
    mapfile -t -d '' headers < <( cd "$ROOT" && find "$@" -name 'build' -prune -o \
        -name 'build-*' -prune -o -name '*.hpp' -print0 )
    printf '  checking %d headers under %s\n' "${#headers[@]}" "$*"
    ( cd "$ROOT" && "$TIDY_BIN" -p "$build" --quiet "${headers[@]}" -- "${flags[@]}" ) > "$log" 2>&1
    ! grep -q -E 'warning:|error:' "$log"
}

# ── headers ─────────────────────────────────────────────────────────────
# The rendered uapi headers are not tracked, so nothing else notices a hand edit to one. A missing
# one is rendered here, and the gate runs first because a build of a drifted header proves nothing.
if wanted headers; then
    announce "headers"
    headers_failed=0
    for template in "$ROOT"/kernel/include/*.h.in; do
        rendered="${template%.in}"
        "$ROOT/tools/render-fsname.sh" --ensure "$template" "$rendered" || headers_failed=1
    done
    settle "headers" "$headers_failed"
fi

# ── docs ────────────────────────────────────────────────────────────────
# Prose carries the real name where a reader sees it, so it cannot render from a template. A marker
# on each such line is what lets the name be held to fsname and rewritten when fsname moves.
if wanted docs; then
    announce "docs"
    if [ "$APPLY_FORMAT" = true ]; then
        python3 "$ROOT/tools/doc-fsname.py" --fix
        settle "docs (applied)" $?
    else
        python3 "$ROOT/tools/doc-fsname.py"
        settle "docs" $?
    fi
fi

# ── build ───────────────────────────────────────────────────────────────
# First, and not only for itself: tidy reads the compile database this leaves behind.
if wanted build; then
    announce "build"
    cmake -S "$ROOT" -B "$BUILD_DIR" > /dev/null &&
        cmake --build "$BUILD_DIR" -j"$JOBS"
    settle "build" $?
fi

# ── format ──────────────────────────────────────────────────────────────
if wanted format; then
    announce "format"
    # Both trees, and one verdict: a gate that stopped at the first list would hide the other.
    if [ "$APPLY_FORMAT" = true ]; then
        list_sources | xargs -0 "$FORMAT_BIN" -i
        format_failed=$?
        list_kernel_sources | xargs -0 "$FORMAT_BIN" -i || format_failed=1
        settle "format (applied)" "$format_failed"
    else
        # --Werror turns a difference into a non-zero exit, which is what a gate needs.
        list_sources | xargs -0 "$FORMAT_BIN" --dry-run --Werror
        format_failed=$?
        list_kernel_sources | xargs -0 "$FORMAT_BIN" --dry-run --Werror || format_failed=1
        settle "format" "$format_failed"
    fi
fi

# ── tidy ─────────────────────────────────────────────────────────────────────
# Two steps, because a whole-tree sweep is not the same as every file. The compile database holds
# translation units, so a header is checked only where it is included, and include-cleaner attributes
# a header's own missing include to the .cpp that included it. HeaderFilterRegex does not change
# that, so passing each header directly is the only way one is the file under diagnosis.
if wanted tidy; then
    announce "tidy"
    tidy_failed=0

    # A log with no warning in it is also what a missing binary leaves behind, so the binary is
    # checked for before the log is trusted.
    if ! command -v "$TIDY_BIN" > /dev/null; then
        printf '  %s%s is not installed, so nothing was checked%s\n' "$RED" "$TIDY_BIN" "$NC"
        tidy_failed=1
    fi

    tidy_database "$BUILD_DIR" "$BUILD_DIR/tidy.log" || tidy_failed=1

    # tools/ configures on its own, and each helper is handed the name it prints itself under. That
    # is a per-target definition, so its database is the only place those flags are complete.
    cmake -S "$ROOT/tools" -B "$BUILD_DIR/tools" > /dev/null
    tidy_database "$BUILD_DIR/tools" "$BUILD_DIR/tools.log" || tidy_failed=1

    # The daemon renders uapi headers into its own build tree, so its database is the only place
    # those include paths are complete.
    cmake -S "$ROOT/daemon" -B "$BUILD_DIR/daemon" > /dev/null
    tidy_database "$BUILD_DIR/daemon" "$BUILD_DIR/daemon.log" || tidy_failed=1

    # Same reason for the flags here: a header has no database entry to take them from. The daemon's
    # headers take the C++20 flags and its own include paths, and the rest take the tree's.
    tidy_headers "$BUILD_DIR" "$BUILD_DIR/headers.log" "${TIDY_FLAGS[@]}" -- tests libroof tools ||
        tidy_failed=1
    tidy_headers "$BUILD_DIR/daemon" "$BUILD_DIR/daemon-headers.log" "${DAEMON_TIDY_FLAGS[@]}" \
        -- daemon || tidy_failed=1

    settle "tidy" "$tidy_failed"
fi

# ── daemon ───────────────────────────────────────────────────────────────
# The daemon's own probes, which need no mount and no module: each drives its layer over a
# socketpair or a temporary file. Its own build tree, because the daemon renders the uapi headers it
# reads into one and the repository build does not carry them.
if wanted daemon; then
    announce "daemon"
    daemon_build="$BUILD_DIR/daemon"
    daemon_args=(-S "$ROOT/daemon" -B "$daemon_build")
    # The turn backend is half the lock path, and without cme its two probes do not build at all.
    [ -d "$DAEMON_CME_DIR" ] && daemon_args+=(-DDAEMON_ENABLE_TURN=ON -DDAEMON_CME_DIR="$DAEMON_CME_DIR")
    cmake "${daemon_args[@]}" > /dev/null &&
        cmake --build "$daemon_build" -j"$JOBS" > /dev/null &&
        ctest --test-dir "$daemon_build" -j"$JOBS" -R ".*" -LE 'security' --output-on-failure
    settle "daemon" $?
fi

# ── suite ───────────────────────────────────────────────────────────────
# The mounts and their daemons are a precondition rather than something this sets up, since mounting
# needs root and the suite does not.
#
# Both are checked, and a missing daemon is why: every case attaches before it does anything, so one
# that is down turns the whole run red for a reason that is not the code.

# The area name a mount folds to, which is what names its socket and its daemon instance.
area_of()
{
    local trimmed="${1#/}"
    echo "${trimmed//\//_}"
}

# Prints what is missing, one per line, and nothing when the suite can run.
missing_for_suite()
{
    local mount
    for mount in "/mnt/$FS_NAME" "/mnt/${FS_NAME}2"; do
        mountpoint -q "$mount" 2> /dev/null || echo "no mount at $mount"
    done
}

# The identity rules the helper reads, and the daemon restarted onto them. Both are root's, so a
# run that has to swap them asks for a password here.
RULES_FILE="/etc/${FS_NAME}/identity-rules.yaml"

# The account the daemon runs as, from the first unit that is loaded. Empty when none is.
helper_account()
{
    local unit
    unit="$(daemon_units | head -n1)"
    [ -n "$unit" ] && systemctl show -p User --value "$unit"
}

# Planted the way the installer plants them: owned by the daemon's account at 0400, root's group.
plant_rules()
{
    [ -n "$(daemon_units)" ] || return 1
    sudo install -m 0400 -o "$(helper_account)" -g root "$1" "$RULES_FILE" || return 1
    for unit in $(daemon_units); do
        sudo systemctl restart "$unit" || return 1
    done
}

# The daemon confirms a path rule against the inode at that path, so its account has to reach the
# binary the rule names. A home is 0750 on most hosts, so each ancestor that hides the path gets a
# traverse ACL for that account, printed one per line so the caller can take them back.
grant_traverse()
{
    local account="$1" dir
    dir="$(dirname "$2")"
    while [ "$dir" != "/" ]; do
        if ! sudo -u "$account" test -x "$dir"; then
            sudo setfacl -m "u:${account}:x" "$dir" && printf '%s\n' "$dir"
        fi
        dir="$(dirname "$dir")"
    done
}

revoke_traverse()
{
    local account="$1"
    shift
    for dir in "$@"; do
        sudo setfacl -x "u:${account}" "$dir"
    done
}

# The suite, less the cases that cannot share a run with it.
run_suite()
{
    if ! daemon_active; then
        printf '  %sSKIP%s the suite needs %s serving, or every create answers -EAGAIN\n' \
            "$YELLOW" "$NC" "${DAEMON_NAME}@<node>"
        return
    fi
    # Each exclusion needs a host the others cannot share: lifecycle the module itself, which it
    # takes down, pathrule the rules the run below plants, and root the privilege the pass after it
    # takes. A security case reproduces an open defect and turns green only when the fix lands.
    ctest --test-dir "$BUILD_DIR" -R '.*' -LE 'pathrule|lifecycle|security|root'
    settle "suite" $?

    run_root_cases
    run_pathrule_cases
}

# The cases that need an effective uid of 0 for one step. Each drops its real ids back to SUDO_UID,
# so the daemon still attests the invoking account the rules name.
run_root_cases()
{
    sudo ctest --test-dir "$BUILD_DIR" -R '.*' -L root -LE 'pathrule|lifecycle|security'
    local answered=$?
    # ctest wrote its logs as root, and the invoker's next run has to overwrite them.
    sudo chown -R "$(id -u):$(id -g)" "$BUILD_DIR/Testing"
    settle "suite (root)" "$answered"
}

# The pathrule cases, under identity rules that name one binary.
#
# Separated because such a rule refuses every other binary on the host, so it cannot be the one the
# rest of the suite runs under. What those cases claim is that a process which execs into another
# binary loses what the first one earned, and only a rule naming a binary can tell the two apart.
#
# The rules go back to what they were on the way out, whatever the run left behind.
run_pathrule_cases()
{
    local rendered="$BUILD_DIR/daemon-rules-under-test.yaml"
    local kept="$BUILD_DIR/daemon-rules-as-found.yaml"
    local case_path="$BUILD_DIR/tests/test_postexec_attack"

    if ! daemon_active; then
        printf '  %sSKIP%s pathrule cases need %s serving to decide anything\n' \
            "$YELLOW" "$NC" "${DAEMON_NAME}@<node>"
        return
    fi
    # sudo, because the file is root's alone: an ordinary -r on it answers no on every host.
    if ! sudo test -r "$RULES_FILE"; then
        printf '  %sSKIP%s pathrule cases need %s readable\n' "$YELLOW" "$NC" "$RULES_FILE"
        return
    fi
    if ! command -v setfacl > /dev/null; then
        printf '  %sSKIP%s pathrule cases need setfacl, from the acl package, to let the daemon reach the case binary\n' \
            "$YELLOW" "$NC"
        return
    fi
    # The redirect belongs to this shell, so the copy stays the invoker's while the read is root's.
    # A copy that did not complete is not planted back: an empty rules file names nobody.
    if ! sudo cat "$RULES_FILE" > "$kept"; then
        printf '  %sSKIP%s pathrule cases: %s could not be copied aside\n' "$YELLOW" "$NC" "$RULES_FILE"
        return
    fi

    "$ROOT/tools/render-fsname.sh" "$ROOT/tests/postexec-rules.yaml.in" "$rendered"
    sed -i "s|@APP_UID@|$(id -u)|g; s|@CASE_PATH@|${case_path}|g" "$rendered"

    local account
    account="$(helper_account)"
    mapfile -t opened < <( grant_traverse "$account" "$case_path" )

    if ! plant_rules "$rendered"; then
        printf '  %sSKIP%s pathrule cases: %s could not be planted\n' \
            "$YELLOW" "$NC" "$RULES_FILE"
        # A restart failure after the file write above still leaves the one-binary rule in place,
        # so every other application's identity request needs the original restored here too.
        plant_rules "$kept" || printf '  %sthe rules were left naming one binary%s\n' "$YELLOW" "$NC"
        revoke_traverse "$account" "${opened[@]}"
        return
    fi
    ctest --test-dir "$BUILD_DIR" -R '.*' -L pathrule
    local answered=$?

    plant_rules "$kept" || printf '  %sthe rules were left naming one binary%s\n' "$YELLOW" "$NC"
    revoke_traverse "$account" "${opened[@]}"
    settle "suite (pathrule)" "$answered"
}

if wanted suite; then
    announce "suite"
    mapfile -t absent < <( missing_for_suite )
    if [ "${#absent[@]}" -gt 0 ]; then
        printf '  %sSKIP%s the suite needs its mounts:\n' "$YELLOW" "$NC"
        printf '    %s\n' "${absent[@]}"
        printf '    install.sh --mount /mnt/%s --mount /mnt/%s2 provisions both, and reload.sh cycles them\n' "$FS_NAME" "$FS_NAME"
    else
        run_suite
    fi
fi

printf '\n'
if [ "$FAILED" -eq 0 ]; then
    printf '%severy gate passed%s\n' "$GREEN" "$NC"
else
    printf '%s%d gate(s) failed%s\n' "$RED" "$FAILED" "$NC"
fi
exit "$FAILED"
