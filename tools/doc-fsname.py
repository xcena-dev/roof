#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Hold the Markdown files to the filesystem name in fsname.

    doc-fsname.py [--fix] [--mark] [--from NAME] [file.md ...]

Prose has to carry the real name where a reader sees it, so it cannot be rendered from a template
the way the headers are. Instead every line that names the filesystem ends in the marker
`<!-- fsname -->`, and a fenced block that names it is followed by a line holding only the marker.
The marker renders as nothing.

Without options every marked line has to hold the current name and every unmarked line has to hold
none, and each miss is one problem. When fsname differs from the committed one, --fix rewrites the
old name to the new inside the marked scopes; --from names the old value when git cannot. --mark
adds the marker wherever the name stands unmarked, which is how a new document is brought in.
With no files given, every tracked .md is read.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

MARKER = "<!-- fsname -->"
TREE = Path(__file__).resolve().parents[1]
FENCE = re.compile(r"^\s*(```|~~~)")


def nameIn(text: str) -> str:
    """The FS_NAME= value of one fsname file's content."""
    for line in text.splitlines():
        if line.startswith("FS_NAME="):
            return line.partition("=")[2].strip()
    raise SystemExit("fsname has no FS_NAME= line")


def committedName() -> str | None:
    shown = subprocess.run(["git", "-C", str(TREE), "show", "HEAD:fsname"], capture_output=True, text=True)
    return nameIn(shown.stdout) if shown.returncode == 0 else None


def trackedMarkdown() -> list[Path]:
    listed = subprocess.run(["git", "-C", str(TREE), "ls-files", "*.md"], capture_output=True, text=True, check=True)
    return [TREE / line for line in listed.stdout.splitlines() if line]


def variants(name: str) -> tuple[str, ...]:
    return (name, name.upper())


def holds(text: str, name: str) -> bool:
    return any(form in text for form in variants(name))


def swap(text: str, old: str, new: str) -> str:
    for oldForm, newForm in zip(variants(old), variants(new)):
        text = text.replace(oldForm, newForm)
    return text


class Scope:
    """One unit the marker governs: a prose line, or the body of a fenced block."""

    def __init__(self, first: int, last: int, marked: bool, block: bool):
        self.first = first
        self.last = last
        self.marked = marked
        self.block = block


def scopesOf(lines: list[str]) -> list[Scope]:
    found: list[Scope] = []
    index = 0
    while index < len(lines):
        if FENCE.match(lines[index]):
            opened = index
            index += 1
            while index < len(lines) and not FENCE.match(lines[index]):
                index += 1
            closing = index
            marked = closing + 1 < len(lines) and lines[closing + 1].strip() == MARKER
            found.append(Scope(opened + 1, closing, marked, block=True))
            index = closing + (2 if marked else 1)
            continue
        found.append(Scope(index, index + 1, MARKER in lines[index], block=False))
        index += 1
    return found


def examine(path: Path, new: str, old: str, fix: bool, mark: bool) -> list[str]:
    lines = path.read_text().splitlines(keepends=True)
    problems: list[str] = []
    rewritten = list(lines)
    inserted = 0

    for scope in scopesOf(lines):
        body = "".join(lines[scope.first : scope.last])
        hasNew, hasOld = holds(body, new), old != new and holds(body, old)
        where = f"{path.relative_to(TREE)}:{scope.first + 1}"

        if scope.marked:
            if hasOld:
                if fix:
                    for row in range(scope.first, scope.last):
                        rewritten[row + inserted] = swap(lines[row], old, new)
                else:
                    problems.append(f"{where}: still names {old}; --fix rewrites it to {new}")
            elif not hasNew:
                problems.append(f"{where}: marked, but names no filesystem")
            continue

        if not (hasNew or hasOld):
            continue
        if mark:
            if scope.block:
                rewritten.insert(scope.last + 1 + inserted, MARKER + "\n")
                inserted += 1
            else:
                line = rewritten[scope.first + inserted]
                rewritten[scope.first + inserted] = line.rstrip("\n") + " " + MARKER + "\n"
        else:
            problems.append(f"{where}: names the filesystem without {MARKER}")

    if rewritten != lines:
        path.write_text("".join(rewritten))
        print(f"{path.relative_to(TREE)}: rewritten")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fix", action="store_true", help="rewrite the committed name to the current one in marked scopes")
    parser.add_argument("--mark", action="store_true", help="add the marker where the name stands unmarked")
    parser.add_argument("--from", dest="previous", help="the name to rewrite from, when git cannot say")
    parser.add_argument("files", nargs="*", type=Path)
    args = parser.parse_args()

    new = nameIn((TREE / "fsname").read_text())
    old = args.previous or committedName() or new
    files = [path.resolve() for path in args.files] or trackedMarkdown()

    problems: list[str] = []
    for path in files:
        problems.extend(examine(path, new, old, args.fix, args.mark))

    for problem in problems:
        print(problem)
    if problems:
        print(f"{len(problems)} problem(s) against fsname={new}")
    return len(problems)


if __name__ == "__main__":
    sys.exit(main())
