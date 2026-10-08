"""Element builders for drafting a figure as an .excalidraw scene.

A draft script places elements with box, container, note, line and arrow, then
calls writeScene. canvas_push.py puts the draft on the canvas for hand editing,
canvas_pull.py brings the edited scene back as the figure's source, and
canvas_render.py renders it to a PNG.
"""
import json
import pathlib
import random
import time

random.seed(20260923)
NOW = int(time.time() * 1000)
FONT = 8  # Comic Shanns

GREY, GREY_BG = "#3b5266", "#f4f6f8"
GREEN, GREEN_BG = "#2e7d32", "#e8f5e9"
AMBER, AMBER_BG = "#7a5a00", "#fff8e1"
BLUE, BLUE_BG = "#1f4e79", "#e3f0fb"
PURPLE, PURPLE_BG = "#6a1b9a", "#f3e5f5"
RED, RED_BG = "#b71c1c", "#fdeaea"
INK = "#1e1e1e"
WHITE = "#ffffff"

elements = []
byId = {}


def nextIndex():
    # The editor rejects a fractional index whose fraction ends in 0, and then drops the whole scene.
    return f"a{len(elements):04d}V"


def base(kind, ident, xPos, yPos, width, height, stroke, background="transparent", strokeWidth=2):
    element = {
        "id": ident, "type": kind, "x": xPos, "y": yPos, "width": width, "height": height,
        "angle": 0, "strokeColor": stroke, "backgroundColor": background, "fillStyle": "solid",
        "strokeWidth": strokeWidth, "strokeStyle": "solid", "roughness": 1, "opacity": 100,
        "groupIds": [], "frameId": None, "index": nextIndex(), "roundness": None,
        "seed": random.randint(1, 2**31 - 1), "version": 1,
        "versionNonce": random.randint(1, 2**31 - 1), "isDeleted": False,
        "boundElements": None, "updated": NOW, "link": None, "locked": False,
    }
    elements.append(element)
    byId[ident] = element
    return element


def textSize(text, fontSize):
    lines = text.split("\n")
    width = max(len(line) for line in lines) * fontSize * 0.55
    height = len(lines) * fontSize * 1.25
    return width, height


def bindLabel(rect, labelId, text, fontSize, vertical):
    textWidth, textHeight = textSize(text, fontSize)
    label = base("text", labelId, rect["x"] + 20, rect["y"] + 10, textWidth, textHeight, rect["strokeColor"])
    label.update({
        "text": text, "fontSize": fontSize, "fontFamily": FONT, "textAlign": "center",
        "verticalAlign": vertical, "containerId": rect["id"], "originalText": text,
        "autoResize": True, "lineHeight": 1.25,
    })
    rect["boundElements"] = [{"id": labelId, "type": "text"}]


NAME_SIZE = 28


def box(ident, xPos, yPos, width, height, stroke, background, label=None, fontSize=None):
    """A rectangle. A label's first line is the component name, set larger at the top;
    the remaining lines are centered in the rest of the box through an invisible inner rectangle."""
    rect = base("rectangle", ident, xPos, yPos, width, height, stroke, background)
    rect["roundness"] = {"type": 3}
    if label is None:
        return rect
    name, description = label.partition("\n")[::2]
    if not description:
        bindLabel(rect, f"{ident}-label", name, fontSize or NAME_SIZE, "middle")
        return rect
    fontSize = fontSize or 20
    # The editor centers a bound label whatever alignment it carries, so the name and the
    # description each get an invisible band the height of their text, stacked at the box's center.
    nameHeight = NAME_SIZE * 1.25
    descHeight = description.count("\n") * fontSize * 1.25 + fontSize * 1.25
    gap = 6
    top = yPos + (height - (nameHeight + gap + descHeight)) / 2
    nameBand = base("rectangle", f"{ident}-name", xPos, top, width, nameHeight, stroke, "transparent")
    nameBand["opacity"] = 0
    bindLabel(nameBand, f"{ident}-label", name, NAME_SIZE, "middle")
    descBand = base("rectangle", f"{ident}-desc", xPos, top + nameHeight + gap, width, descHeight, stroke, "transparent")
    descBand["opacity"] = 0
    bindLabel(descBand, f"{ident}-desc-label", description, fontSize, "middle")
    return rect


def note(ident, xPos, yPos, text, fontSize=24, stroke=INK):
    textWidth, textHeight = textSize(text, fontSize)
    element = base("text", ident, xPos, yPos, textWidth, textHeight, stroke)
    element.update({
        "text": text, "fontSize": fontSize, "fontFamily": FONT, "textAlign": "left",
        "verticalAlign": "top", "containerId": None, "originalText": text,
        "autoResize": True, "lineHeight": 1.25,
    })
    return element


def container(ident, xPos, yPos, width, height, stroke, background, title):
    box(ident, xPos, yPos, width, height, stroke, background)
    note(f"{ident}-title", xPos + 20, yPos + 12, title, 30, stroke)


def line(ident, xPos, yPos, deltaX, deltaY, stroke, strokeWidth=2, dashed=False):
    element = base("line", ident, xPos, yPos, abs(deltaX), abs(deltaY), stroke, "transparent", strokeWidth)
    if dashed:
        element["strokeStyle"] = "dashed"
    element.update({
        "points": [[0, 0], [deltaX, deltaY]], "lastCommittedPoint": None,
        "startBinding": None, "endBinding": None, "startArrowhead": None,
        "endArrowhead": None, "elbowed": False,
    })
    return element


def arrow(ident, fromId, fromPoint, toId, toPoint, stroke, strokeWidth=2):
    """Bound at both ends. fromPoint/toPoint are absolute edge coordinates."""
    (startX, startY), (endX, endY) = fromPoint, toPoint
    deltaX, deltaY = endX - startX, endY - startY
    element = base("arrow", ident, startX, startY, abs(deltaX), abs(deltaY), stroke, "transparent", strokeWidth)
    element["roundness"] = {"type": 2}
    element.update({
        "points": [[0, 0], [deltaX, deltaY]], "lastCommittedPoint": None,
        "startBinding": {"elementId": fromId, "focus": 0, "gap": 1},
        "endBinding": {"elementId": toId, "focus": 0, "gap": 1},
        "startArrowhead": None, "endArrowhead": "arrow", "elbowed": False,
    })
    for holder in (fromId, toId):
        bound = byId[holder]["boundElements"] or []
        bound.append({"id": ident, "type": "arrow"})
        byId[holder]["boundElements"] = bound
    return element


def writeScene(path, source, files=None):
    """Write every element built so far to @path as a scene, and return how many there are."""
    scene = {"type": "excalidraw", "version": 2, "source": source, "elements": elements,
             "appState": {"viewBackgroundColor": "#ffffff", "gridSize": 20}, "files": files or {}}
    pathlib.Path(path).write_text(json.dumps(scene, indent=1), encoding="utf-8")
    return len(elements)


# ── sequence figures ─────────────────────────────────────────────────────
STEP_FONT = 22
STEP_LINE = STEP_FONT * 1.25


def wrapStep(text, span):
    """Re-wrap a step note so no line is wider than the arrow it sits on.
    The first line keeps its number; continuation lines are indented under the words."""
    # The text starts 30 px in and a rendered glyph measures 0.54 of the font size,
    # so this leaves 10 px before the arrow's end.
    maxChars = max(12, int((span - 40) / (STEP_FONT * 0.54)))
    # Continuation lines start under the first word after the number, whatever its width.
    number = text.split()[0] if text.split() and text.split()[0].isdigit() else ""
    hang = " " * (len(number) + 1) if number else ""
    out = []
    for raw in text.split("\n"):
        current = hang if out else ""
        for word in raw.strip().split():
            candidate = current + word if current.strip() == "" else current + " " + word
            if len(candidate) > maxChars and current.strip():
                out.append(current)
                current = hang + word
            else:
                current = candidate
        out.append(current)
    return "\n".join(out)


def anchor(ident, xPos, yPos):
    """An invisible box on a lifeline, so an arrow has something to bind to."""
    element = box(ident, xPos - 6, yPos - 6, 12, 12, INK, "transparent")
    element["opacity"] = 0
    return element


def layoutSteps(steps, startY, gap=44):
    """steps: list of (ident, fromX, toX, text, stroke) or ("box", ident, x, w, h, stroke, bg, label).
    Places each note above its arrow and returns the y below the last arrow."""
    cursor = startY
    for item in steps:
        if item[0] == "box":
            ident, xPos, width, height, stroke, background, label = item[1:]
            box(ident, xPos, cursor, width, height, stroke, background, label, 20)
            cursor += height + gap
            continue
        ident, fromX, toX, text, stroke = item
        wrapped = wrapStep(text, abs(toX - fromX))
        lines = wrapped.count("\n") + 1
        noteX = min(fromX, toX) + 30
        noteY = cursor
        arrowY = noteY + lines * STEP_LINE + 12
        width, height = textSize(wrapped, STEP_FONT)
        box(f"{ident}-card", noteX - 8, noteY - 4, width + 16, height + 8, "transparent", WHITE)
        note(f"{ident}-note", noteX, noteY, wrapped, STEP_FONT, stroke)
        anchor(f"{ident}-from", fromX, arrowY)
        anchor(f"{ident}-to", toX, arrowY)
        arrow(ident, f"{ident}-from", (fromX, arrowY), f"{ident}-to", (toX, arrowY), stroke, 2)
        cursor = arrowY + gap
    return cursor
