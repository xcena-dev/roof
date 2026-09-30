#!/usr/bin/env python3
"""Pull the live excalidraw canvas into a versioned .excalidraw file.

The canvas server holds the scene in memory only, so the file here is the
source that survives a server restart.
"""

import argparse
import json
import re
import sys
import urllib.error
import urllib.request

import canvas_config

DEFAULT_SERVER = canvas_config.server_url()

# Excalidraw's built-in drawing fonts. The browser bundle carries these only.
FONT_FAMILY = {
    "virgil": 1,
    "helvetica": 2,
    "cascadia": 3,
    "excalifont": 5,
    "nunito": 6,
    "lilita": 7,
    "comic-shanns": 8,
}


def request_json(url, method="GET", payload=None):
    body = None if payload is None else json.dumps(payload).encode()
    headers = {"Content-Type": "application/json"} if body else {}
    request = urllib.request.Request(url, data=body, headers=headers, method=method)
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.loads(response.read().decode())


def fetch_elements(server):
    payload = request_json(f"{server}/api/elements")
    if isinstance(payload, dict):
        return payload.get("elements", payload.get("data", []))
    return payload


def apply_font(server, family):
    changed = 0
    for element in fetch_elements(server):
        if element.get("type") != "text":
            continue
        request_json(
            f"{server}/api/elements/{element['id']}",
            method="PUT",
            payload={"fontFamily": family},
        )
        changed += 1
    return changed


# Fields a person changes by hand. Seeds, nonces and sync timestamps churn on
# every frontend push, so comparing them would bury the real edits.
COMPARED = ("type", "x", "y", "width", "height", "text",
            "strokeColor", "backgroundColor", "points")


def compare(server, path):
    with open(path, encoding="utf-8") as handle:
        stored = {item["id"]: item for item in json.load(handle)["elements"]}
    live = {item["id"]: item for item in fetch_elements(server)}

    for element_id in live.keys() - stored.keys():
        print(f"added   {element_id} {live[element_id].get('type')}")
    for element_id in stored.keys() - live.keys():
        print(f"removed {element_id} {stored[element_id].get('type')}")
    for element_id in live.keys() & stored.keys():
        for field in COMPARED:
            before, after = stored[element_id].get(field), live[element_id].get(field)
            if before != after:
                print(f"changed {element_id} {field}: {before} -> {after}")


def reach_outline(point, ahead, box):
    """Walk @point along the @ahead direction until it meets @box's outline.

    A bound arrow stops a gap short of the shape and leaves room for its head, so
    the coordinates the editor hands back sit well clear of the box. The picture
    the person drew has them touching.
    """
    step_x, step_y = ahead
    length = (step_x * step_x + step_y * step_y) ** 0.5
    if length < 1e-6:
        return point
    step_x, step_y = step_x / length, step_y / length

    left, top = box["x"], box["y"]
    right, bottom = left + box["width"], top + box["height"]
    inside = left <= point[0] <= right and top <= point[1] <= bottom
    # Inside the box the endpoint backs out to the outline; outside it walks in.
    if inside:
        step_x, step_y = -step_x, -step_y

    at_x, at_y = point
    for _ in range(int(length) + 120):
        here = left <= at_x <= right and top <= at_y <= bottom
        if here != inside:
            return (at_x, at_y) if inside else (at_x - step_x, at_y - step_y)
        at_x += step_x
        at_y += step_y
    return point


# How far an endpoint may sit from a shape and still be taken to mean it.
WITHIN_REACH = 60.0


def nearest_box(point, boxes):
    """The shape an endpoint is closest to, when it is close to one at all."""
    at_x, at_y = point
    best, found = WITHIN_REACH, None
    for box in boxes:
        away_x = max(box["x"] - at_x, 0, at_x - (box["x"] + box["width"]))
        away_y = max(box["y"] - at_y, 0, at_y - (box["y"] + box["height"]))
        gap = (away_x * away_x + away_y * away_y) ** 0.5
        if gap < best:
            best, found = gap, box
    return found


def close_gaps(elements):
    """Push each endpoint out to the shape it points at.

    A binding names that shape. An arrow drawn without one, or frozen by an
    earlier pull, is read by what it nearly touches.
    """
    boxes = {element["id"]: element for element in elements
             if element.get("type") in ("rectangle", "ellipse", "diamond")}
    # A white patch behind a caption is not something an arrow points at, and a zone
    # drawn around other boxes, white ones included, is reached first while the box inside it is meant.
    plain = [box for box in boxes.values()
             if box.get("backgroundColor") not in ("#ffffff", "transparent")]
    shown = [box for box in boxes.values() if box.get("opacity", 100) > 0]

    def wraps_another(box):
        return any(other is not box
                   and box["x"] <= other["x"]
                   and box["y"] <= other["y"]
                   and box["x"] + box["width"] >= other["x"] + other["width"]
                   and box["y"] + box["height"] >= other["y"] + other["height"]
                   for other in shown)

    candidates = [box for box in plain if not wraps_another(box)]
    closed = 0
    for element in elements:
        if element.get("type") != "arrow":
            continue
        points = element.get("points") or []
        # A bent arrow's end segment does not point at the shape the way a straight
        # one does, so walking along it pushes the endpoint sideways into a box that
        # was never the target. Those keep the coordinates the editor gave them.
        if len(points) != 2:
            continue
        moved = list(points)
        for index, side in ((0, "startBinding"), (-1, "endBinding")):
            binding = element.get(side)
            here = moved[index]
            world = (element["x"] + here[0], element["y"] + here[1])
            box = boxes.get(binding["elementId"]) if binding else nearest_box(world, candidates)
            if not box:
                continue
            neighbour = moved[1] if index == 0 else moved[-2]
            ahead = (here[0] - neighbour[0], here[1] - neighbour[1])
            reached = reach_outline(world, ahead, box)
            moved[index] = [reached[0] - element["x"], reached[1] - element["y"]]
            closed += 1
        # The box is squared with the points every time, since an endpoint that
        # already touched its shape leaves the server's stale width behind.
        element["points"] = moved
        element["width"] = max(p[0] for p in moved) - min(p[0] for p in moved)
        element["height"] = max(p[1] for p in moved) - min(p[1] for p in moved)
    return closed


def place_labels(elements):
    """Write each arrow label's real place into the file.

    The editor derives a bound label's position when it draws, and leaves whatever
    it last computed in the stored fields, which can be far off the canvas. The
    label sits at the middle of the arrow it rides, so that is what gets written.
    """
    arrows = {element["id"]: element for element in elements
              if element.get("type") in ("arrow", "line")}
    placed = 0
    for element in elements:
        arrow = arrows.get(element.get("containerId") or "")
        if element.get("type") != "text" or not arrow:
            continue
        points = arrow.get("points") or [[0, 0]]
        middle_x = arrow["x"] + sum(point[0] for point in points) / len(points)
        middle_y = arrow["y"] + sum(point[1] for point in points) / len(points)
        element["x"] = middle_x - element.get("width", 0) / 2
        element["y"] = middle_y - element.get("height", 0) / 2
        placed += 1
    return placed


def freeze_bindings(elements):
    """Turn every bound arrow into plain coordinates.

    While a binding is on an arrow the editor owns its geometry and recomputes it,
    so a later write from here starts that recompute from a state the editor did
    not produce and the endpoints run off the canvas. The points read here are
    already where the arrow is drawn, so dropping the binding keeps the picture
    and hands the coordinates back. A label riding on an arrow is left alone.
    """
    linear = {element["id"] for element in elements
              if element.get("type") in ("arrow", "line")}
    frozen = 0
    for element in elements:
        if element["id"] in linear and (element.get("startBinding") or element.get("endBinding")):
            element["startBinding"] = None
            element["endBinding"] = None
            frozen += 1
        if element["id"] in linear:
            continue
        bound = element.get("boundElements")
        if bound:
            element["boundElements"] = [ref for ref in bound if ref["id"] not in linear] or None
    return frozen


# The editor names anything drawn by hand with a long random string.
RANDOM_ID = re.compile(r"^[A-Za-z0-9_-]{20,}$")


def slug(text):
    return "-".join(re.findall(r"[a-z0-9]+", (text or "").lower())[:3])


def name_elements(elements):
    """Give the hand drawn elements ids that say what they are.

    A shape takes the words on it, an arrow takes the two shapes it runs between,
    and a label takes its container's name. Runs before the bindings are frozen,
    since an arrow names its ends from them.
    """
    boxes = [element for element in elements
             if element.get("type") in ("rectangle", "ellipse", "diamond")]
    labels = {item["containerId"]: item.get("text", "") for item in elements
              if item.get("type") == "text" and item.get("containerId")}

    names = {}
    for box in boxes:
        wanted = slug(labels.get(box["id"], ""))
        if RANDOM_ID.match(box["id"]) and wanted:
            names[box["id"]] = wanted

    for element in elements:
        if element.get("type") not in ("arrow", "line") or not RANDOM_ID.match(element["id"]):
            continue
        ends = []
        for index, side in ((0, "startBinding"), (-1, "endBinding")):
            binding = element.get(side)
            target = binding["elementId"] if binding else None
            if target is None:
                points = element.get("points") or [[0, 0]]
                at = points[index]
                box = nearest_box((element["x"] + at[0], element["y"] + at[1]), boxes)
                target = box["id"] if box else None
            ends.append(names.get(target, target) or "loose")
        names[element["id"]] = f"{ends[0]}-to-{ends[1]}"

    for element in elements:
        holder = element.get("containerId")
        if element.get("type") == "text" and holder and RANDOM_ID.match(element["id"]):
            names[element["id"]] = f"{names.get(holder, holder)}-label"

    seen = {}
    for element_id, wanted in list(names.items()):
        count = seen.get(wanted, 0)
        seen[wanted] = count + 1
        if count:
            names[element_id] = f"{wanted}-{count + 1}"

    for element in elements:
        element["id"] = names.get(element["id"], element["id"])
        holder = element.get("containerId")
        if holder:
            element["containerId"] = names.get(holder, holder)
        bound = element.get("boundElements")
        if bound:
            element["boundElements"] = [{**ref, "id": names.get(ref["id"], ref["id"])}
                                        for ref in bound]
        for side in ("startBinding", "endBinding"):
            binding = element.get(side)
            if binding:
                binding["elementId"] = names.get(binding["elementId"], binding["elementId"])
    return len(names)


def write_scene(server, path, grid=None, keep_bindings=False):
    """Write the scene. @grid is the grid step in pixels, or None for no grid.

    A file written without a step keeps whatever step it already carried, so a
    plain pull does not turn the grid off behind the person drawing.
    """
    if grid is None:
        try:
            with open(path, encoding="utf-8") as handle:
                grid = json.load(handle).get("appState", {}).get("gridSize")
        except (OSError, ValueError):
            grid = None

    elements = fetch_elements(server)
    # The server keeps image data apart from the elements that show it.
    shown = {element["fileId"] for element in elements
             if element.get("type") == "image" and element.get("fileId")}
    stored = request_json(f"{server}/api/files").get("files", {}) if shown else {}
    if not keep_bindings:
        named = name_elements(elements)
        closed = close_gaps(elements)
        frozen = freeze_bindings(elements)
        placed = place_labels(elements)
        if named or frozen or placed:
            print(f"named {named}, froze {frozen} arrows, took {closed} ends "
                  f"to their box, placed {placed} labels")

    scene = {
        "type": "excalidraw",
        "version": 2,
        # Not the server's address: the file is tracked and the address is not.
        "source": "canvas_pull.py",
        "elements": elements,
        "appState": {"viewBackgroundColor": "#ffffff", "gridSize": grid},
        "files": {file_id: stored[file_id] for file_id in sorted(shown) if file_id in stored},
    }
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(scene, handle, indent=2, ensure_ascii=False)
        handle.write("\n")
    return len(scene["elements"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="path of the .excalidraw file to write")
    parser.add_argument("--server", default=DEFAULT_SERVER)
    parser.add_argument(
        "--font",
        choices=sorted(FONT_FAMILY),
        help="set this font on every text element before writing",
    )
    parser.add_argument(
        "--compare",
        action="store_true",
        help="report how the live canvas differs from the file, and write nothing",
    )
    parser.add_argument(
        "--grid",
        type=int,
        metavar="STEP",
        help="turn the background grid on at this step in pixels, 0 to turn it off",
    )
    parser.add_argument(
        "--keep-bindings",
        action="store_true",
        help="write an arrow's bindings as they are instead of freezing them to coordinates",
    )
    options = parser.parse_args()

    try:
        if options.compare:
            compare(options.server, options.output)
            return 0
        if options.font:
            count = apply_font(options.server, FONT_FAMILY[options.font])
            print(f"font {options.font}: {count} text elements")
        grid = None if options.grid is None else (options.grid or None)
        total = write_scene(options.server, options.output, grid, options.keep_bindings)
    except urllib.error.URLError as error:
        print(f"canvas server unreachable at {options.server}: {error}", file=sys.stderr)
        return 1
    print(f"{options.output}: {total} elements")
    return 0


if __name__ == "__main__":
    sys.exit(main())
