#!/usr/bin/env python3
"""Where the canvas server lives, read from a file rather than written in code.

The address differs per clone, so the filled-in copy stays out of the repository
and only the template beside it is tracked.

Order: the CANVAS_SERVER environment variable, then canvas.yaml, then
canvas.example.yaml. The file is flat "key: value" lines, which the kv reader the
daemon already uses accepts too.
"""

import os
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
MINE = HERE / "canvas.yaml"
TEMPLATE = HERE / "canvas.example.yaml"


def read_pairs(path):
    pairs = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line or ":" not in line:
            continue
        key, value = line.split(":", 1)
        pairs[key.strip()] = value.strip().strip('"').strip("'")
    return pairs


def server_url():
    """The base URL of the canvas server, such as http://10.0.0.5:3000."""
    named = os.environ.get("CANVAS_SERVER")
    if named:
        return named.rstrip("/")

    for path in (MINE, TEMPLATE):
        if not path.exists():
            continue
        pairs = read_pairs(path)
        host, port = pairs.get("host"), pairs.get("port")
        if host and port:
            if path is TEMPLATE:
                print(f"no {MINE.name}, using the template's address", file=sys.stderr)
            return f"http://{host}:{port}"

    raise SystemExit(
        f"no canvas address: set CANVAS_SERVER, or copy {TEMPLATE.name} to {MINE.name}"
    )
