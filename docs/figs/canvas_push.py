#!/usr/bin/env python3
"""Restore a .excalidraw file onto the canvas server.

Together with canvas_pull.py this makes the file the figure's source: elements
the editor authored go back exactly as they were, so a scene survives a server
restart and can be regenerated without redrawing it.
"""

import argparse
import json
import sys
import urllib.error
import urllib.request

import canvas_config

DEFAULT_SERVER = canvas_config.server_url()

# Written by the server on receipt, so sending them back is pointless.
SERVER_OWNED = ("syncedAt", "syncTimestamp", "source", "createdAt", "updatedAt")

# How far an arrow's stored box may sit from its own points before it counts as broken.
SPAN_SLACK = 2.0

# A bound label is placed far from its container, but a coordinate past this came out
# of a recompute that had nothing to work with, and the element is gone from the page.
CANVAS_LIMIT = 100000


def request_json(url, method="GET", payload=None):
    body = None if payload is None else json.dumps(payload).encode()
    headers = {"Content-Type": "application/json"} if body else {}
    request = urllib.request.Request(url, data=body, headers=headers, method=method)
    with urllib.request.urlopen(request, timeout=30) as response:
        raw = response.read()
    return json.loads(raw) if raw else {}


def load_elements(path):
    """Read the scene in the order the editor stacks it.

    `index` is what decides which element is drawn on top, so bringing one to the
    front only survives a push when the elements go back in that order.
    """
    with open(path, encoding="utf-8") as handle:
        scene = json.load(handle)
    elements = scene["elements"] if isinstance(scene, dict) else scene
    kept = [{key: value for key, value in element.items() if key not in SERVER_OWNED}
            for element in elements if not element.get("isDeleted")]
    kept.sort(key=lambda element: (element.get("index") is None, element.get("index") or ""))
    return kept


def load_files(path):
    """The image data the scene's image elements show, keyed by file id."""
    with open(path, encoding="utf-8") as handle:
        scene = json.load(handle)
    return (scene.get("files") or {}) if isinstance(scene, dict) else {}


def check(elements, files):
    """Report the shapes that make the editor recompute an element wrongly."""
    known = {element["id"] for element in elements}
    problems = []
    for element in elements:
        if element.get("type") == "image" and element.get("fileId") not in files:
            problems.append(f"{element['id']} shows file {element.get('fileId')}, which the scene does not carry")
    for element in elements:
        for axis in ("x", "y"):
            if abs(element.get(axis) or 0) > CANVAS_LIMIT:
                problems.append(f"{element['id']} {axis} {element[axis]:.0f} is off the canvas")
    for element in elements:
        for reference in element.get("boundElements") or []:
            if reference["id"] not in known:
                problems.append(f"{element['id']} points at missing {reference['id']}")
        if element.get("type") != "arrow":
            continue
        for side in ("startBinding", "endBinding"):
            binding = element.get(side)
            if binding and binding["elementId"] not in known:
                problems.append(f"{element['id']} {side} points at missing {binding['elementId']}")
        points = element.get("points") or []
        if len(points) >= 2:
            spread_x = max(point[0] for point in points) - min(point[0] for point in points)
            spread_y = max(point[1] for point in points) - min(point[1] for point in points)
            # The editor leaves a pixel or two between an arrow's box and its own points,
            # and a break that matters has always been orders of magnitude wider.
            if abs(spread_x - abs(element.get("width", 0))) > SPAN_SLACK:
                problems.append(f"{element['id']} width {element.get('width')} but points span {spread_x:.1f}")
            if abs(spread_y - abs(element.get("height", 0))) > SPAN_SLACK:
                problems.append(f"{element['id']} height {element.get('height')} but points span {spread_y:.1f}")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", help="the .excalidraw file to restore")
    parser.add_argument("--server", default=DEFAULT_SERVER)
    parser.add_argument("--check", action="store_true",
                        help="report inconsistencies and push nothing")
    options = parser.parse_args()

    elements = load_elements(options.scene)
    files = load_files(options.scene)
    problems = check(elements, files)
    for problem in problems:
        print(f"warning: {problem}", file=sys.stderr)
    if options.check:
        print(f"{len(elements)} elements, {len(problems)} problems")
        return 1 if problems else 0

    try:
        request_json(f"{options.server}/api/elements/clear", method="DELETE")
        if files:
            request_json(f"{options.server}/api/files",
                         method="POST", payload={"files": list(files.values())})
        request_json(f"{options.server}/api/elements/batch",
                     method="POST", payload={"elements": elements})
    except urllib.error.HTTPError as error:
        print(f"{error.code}: {error.read().decode()[:400]}", file=sys.stderr)
        return 1
    except urllib.error.URLError as error:
        print(f"canvas server unreachable at {options.server}: {error}", file=sys.stderr)
        return 1
    print(f"{len(elements)} elements restored to {options.server}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
