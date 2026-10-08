#!/usr/bin/env python3
"""An example draft: a three-lane sequence figure built with figlib.

    python3 make_example.py example.excalidraw
    python3 canvas_push.py example.excalidraw
"""
import sys

from figlib import (BLUE, BLUE_BG, GREEN, GREEN_BG, GREY, GREY_BG, INK, WHITE,
                    box, layoutSteps, line, note, writeScene)

CLIENT, SERVER, STORE = 240, 700, 1160

boundary = box("boundary", 40, 40, 1360, 100, INK, "transparent")
note("title", 80, 70, "Example: a read through a server", 36, INK)

lanes = [("client", CLIENT, GREY, GREY_BG), ("server", SERVER, BLUE, BLUE_BG), ("store", STORE, GREEN, GREEN_BG)]
lifelines = []
for name, xPos, stroke, background in lanes:
    box(f"lane-{name}", xPos - 150, 130, 300, 90, stroke, background, name, 26)
    # Drawn short here and stretched to the last step once the steps are placed.
    lifelines.append(line(f"life-{name}", xPos, 220, 0, 100, stroke, 2, dashed=True))

bottom = layoutSteps([
    ("s1", CLIENT, SERVER, "1  ask for the record by name", GREY),
    ("s2", SERVER, STORE, "2  read the record and the version it carries", BLUE),
    ("box", "check", SERVER - 180, 360, 50, BLUE, WHITE, "check the caller may read it"),
    ("s3", STORE, SERVER, "3  the record", GREEN),
    ("s4", SERVER, CLIENT, "4  the record, or a refusal that names the reason", BLUE),
], startY=260)

for element in lifelines:
    element["height"] = bottom - element["y"]
    element["points"] = [[0, 0], [0, bottom - element["y"]]]

note("footnote", CLIENT - 150, bottom + 30, "each step's note wraps to the width of its arrow", 20, GREY)
boundary["height"] = bottom + 90 - boundary["y"]

target = sys.argv[1] if len(sys.argv) > 1 else "example.excalidraw"
count = writeScene(target, "make_example.py")
print(count, "elements ->", target)
