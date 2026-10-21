#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# uninstall_removes_library -- an uninstall leaves no library file under the prefix, without root.
#
# Installs libroof into a scratch prefix the way install.sh does, runs the removal uninstall.sh
# shares, and lists whatever file or link is still there.
#
#   ctest --test-dir <build> -R uninstall_removes_library

set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
. "${ROOT}/fsname"
. "${ROOT}/shell/paths.sh"

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT
LIB_BUILD="${SCRATCH}/build"
PREFIX="${SCRATCH}/prefix"

cmake -S "$ROOT" -B "$LIB_BUILD" -DFS_PUBLISH_NAMESPACE=ON > /dev/null || exit 1
cmake --build "$LIB_BUILD" > /dev/null || exit 1
cmake --install "$LIB_BUILD" --prefix "$PREFIX" > /dev/null || exit 1

removeLibraryFiles "$PREFIX" "$LIB_NAME"

leftovers="$(find "$PREFIX" \( -type f -o -type l \) | sort)"
if [[ -n "$leftovers" ]]; then
  printf 'FAIL uninstall left these under %s:\n%s\n' "$PREFIX" "${leftovers//${PREFIX}/<prefix>}"
  exit 1
fi
printf 'PASS uninstall left nothing under the prefix\n'
