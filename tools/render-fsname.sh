#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Renders one header template, or checks that a rendered one still matches it.
#
# A template carries two kinds of placeholder. @FS_PREFIX@ and @FS_PREFIX_UPPER@ open every name it
# declares, and one substitution decides how those names read. @FS_NAME@ and @FS_UPPER@ are the
# filesystem's own name, which a value keeps in either rendering.
#
#   render-fsname.sh <template> <output>            fs_ / FS_ names, which this tree compiles on
#   render-fsname.sh --release <template> <output>  the module's name in front of each of them
#   render-fsname.sh --ensure <template> <output>   renders a missing one, refuses a drifted one
#   render-fsname.sh --check <template> <output>    exits non-zero when <output> has drifted
#   render-fsname.sh --namespace <in> <output>      rewrites the C++ namespace for a release build
#
# --namespace is not a template pass. It takes a source file as it stands and swaps the neutral
# namespace token for the library's own, header and .cpp alike, because a namespace is part of a
# mangled symbol: publishing one name over an archive built under another would not link. It also
# moves the include directory to that name, so an application writes #include <lib>/session.hpp with
# the library's own word in front of the slash.
#
# A build calls --ensure rather than rendering. Somebody wrote code against the header as it stands,
# so a build that quietly rewrote it would compile something other than what they read.
#
# Both spellings define the same value directly, so a reader of either header sees the number rather
# than another name to follow.
#
# The neutral prefix is fs and not nothing: an empty one turns NAME_MAX into the one linux/limits.h
# already defines, and struct perm_req into a name a kernel tree has several of.
#
# Both build systems call this rather than each substituting on its own, so a kbuild header and a
# CMake header of one template come out as the same bytes.

set -eu

mode=build
case "${1:-}" in
    --release) mode=release; shift ;;
    --ensure)  mode=ensure;  shift ;;
    --namespace) mode=namespace; shift ;;
    --check)   mode=check;   shift ;;
esac

template="$1"
output="$2"
root="$(cd "$(dirname "$0")/.." && pwd)"

. "$root/fsname"
upper="$(printf '%s' "$FS_NAME" | tr '[:lower:]' '[:upper:]')"
daemon_upper="$(printf '%s' "$DAEMON_NAME" | tr '[:lower:]' '[:upper:]')"

if [ "$mode" = release ]; then
    prefix="$FS_NAME"
    prefix_upper="$upper"
else
    prefix=fs
    prefix_upper=FS
fi

render()
{
    if [ "$mode" = namespace ]; then
        # Only these two openings, never a bare fs/: "/sys/fs/" is a real path in here.
        sed -e "s/\\bfsuser\\b/$LIB_NAME/g" \
            -e "s|#include \"fs/|#include \"$LIB_NAME/|g" \
            -e "s|// fs/|// $LIB_NAME/|g" \
            -e "s/<FS>/$upper/g" -e "s/<fs>/$FS_NAME/g" "$template"
    else
        sed -e "s/@FS_PREFIX_UPPER@/$prefix_upper/g" -e "s/@FS_PREFIX@/$prefix/g" \
            -e "s/@FS_UPPER@/$upper/g" -e "s/@FS_NAME@/$FS_NAME/g" \
            -e "s/@DAEMON_UPPER@/$daemon_upper/g" -e "s/@DAEMON_NAME@/$DAEMON_NAME/g" "$template"
    fi
}

if [ "$mode" = check ] || [ "$mode" = ensure ]; then
    if [ ! -f "$output" ]; then
        if [ "$mode" = check ]; then
            echo "$output: never rendered from $(basename "$template")" >&2
            exit 1
        fi
    else
        render | diff -u "$output" - > "$output.diff" && { rm -f "$output.diff"; exit 0; }
        cat "$output.diff" >&2
        rm -f "$output.diff"
        echo "$output and $(basename "$template") disagree." >&2
        echo "Whoever wrote against that header read what is on the left. Settle it by hand, then:" >&2
        echo "  $0 $template $output" >&2
        exit 1
    fi
fi

mkdir -p "$(dirname "$output")"
render > "$output.tmp"
mv -f "$output.tmp" "$output"
