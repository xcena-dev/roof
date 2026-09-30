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

## Two things that bite

**Run `canvas_push.py --check` before a push.**
Deleting a label in the editor leaves its id behind in the arrow's `boundElements`, and the check is what finds it.

**The server exports one canvas unit as one pixel.**
`canvas_render.py --scale` and the browser's zoom are both ignored, so a figure that needs to be larger is drawn with larger coordinates and larger text.

## Fonts

`canvas_pull.py --font` sets the scene's font, and the default these figures use is Comic Shanns.
A scene stores the font as a number and carries none of its data, and a PNG carries only the rendered pixels.
Exporting SVG instead would embed the font, which is a licensing question this directory does not have today.
