# Figures

Each figure here is a `.excalidraw` scene and the `.png` the documents embed.
The scene is the source and the PNG is built from it, so an edit starts from the scene and never from the image.

A scene is edited on a running [Excalidraw](https://github.com/excalidraw/excalidraw) canvas rather than by hand.
The canvas server keeps the scene in memory only, which is why the file in this directory exists.

## The three scripts

```sh
python3 canvas_push.py   access_decision.excalidraw   # file onto the canvas
python3 canvas_pull.py   access_decision.excalidraw   # canvas back into the file
python3 canvas_render.py access_decision.png          # canvas to a PNG
```

`canvas_render.py` asks the server for the rendering, so the browser tab holding the scene has to be open.

The server address differs per clone and is read from `canvas.yaml`, which is not tracked.
Copy `canvas.example.yaml` to `canvas.yaml` and fill it in.

## Drafting a new figure

`figlib.py` builds a first draft in code, for a figure that is easier to lay out by numbers than by hand.
A draft script imports it,
places elements with `box`, `container`, `note`, `line` and `arrow`,
and ends with `writeScene`.

```python
from figlib import BLUE, BLUE_BG, arrow, box, writeScene

box("client", 0, 0, 300, 100, BLUE, BLUE_BG, "client\nopens the file")
box("server", 500, 0, 300, 100, BLUE, BLUE_BG, "server")
arrow("open", "client", (300, 50), "server", (500, 50), BLUE)
writeScene("example.excalidraw", "example.py")
```

A box label's first line is the component's name, and the lines after it are its description in smaller text.
`layoutSteps` lays out a sequence figure:
one numbered note above each arrow,
each note wrapped to its arrow's width.
`make_example.py` is a whole draft of a small sequence figure, with lanes, lifelines and a boxed step between arrows.
`python3 make_example.py example.excalidraw` writes it, and `canvas_push.py` puts it on the canvas.
Every element takes the colours at the top of `figlib.py` and the Comic Shanns font, so a draft matches the figures already here.

The draft then takes the path every scene takes:
`canvas_push.py` puts it on the canvas,
the canvas is where it is finished by hand,
and `canvas_pull.py` writes the finished scene back.
From then on the scene is the source.
The draft script is not run again, because its output would discard the hand edits.

## Two things that bite

**Run `canvas_push.py --check` before a push.**
Deleting a label in the editor leaves its id behind in the arrow's `boundElements`, and the check is what finds it.

**The server exports one canvas unit as one pixel.**
`canvas_render.py --scale` and the browser's zoom are both ignored, so a figure that needs to be larger is drawn with larger coordinates and larger text.

## Fonts

`canvas_pull.py --font` sets the scene's font, and the default these figures use is Comic Shanns.
A scene stores the font as a number and carries none of its data, and a PNG carries only the rendered pixels.
Exporting SVG instead would embed the font, which is a licensing question this directory does not have today.
