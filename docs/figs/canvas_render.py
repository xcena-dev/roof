#!/usr/bin/env python3
"""Render what is on the canvas to a PNG beside the scene file.

The rendering is the browser's, so this asks the canvas server for it and the
tab that holds the scene has to be open. The server writes only under the
directory it was started with, so the image is fetched and written here instead.
"""

import argparse
import base64
import json
import pathlib
import sys
import urllib.error
import urllib.request

import canvas_config

DEFAULT_SERVER = canvas_config.server_url()


def request_json(url, payload, timeout):
    body = json.dumps(payload).encode()
    headers = {"Content-Type": "application/json"}
    request = urllib.request.Request(url, data=body, headers=headers, method="POST")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="the .png to write")
    parser.add_argument("--server", default=DEFAULT_SERVER)
    parser.add_argument("--scale", type=int, default=2,
                        help="pixels per canvas unit (default 2)")
    parser.add_argument("--timeout", type=int, default=60,
                        help="seconds to wait for the tab to render")
    options = parser.parse_args()

    try:
        answer = request_json(f"{options.server}/api/export/image",
                              {"format": "png", "scale": options.scale,
                               "exportBackground": True},
                              options.timeout)
    except urllib.error.HTTPError as error:
        print(f"{error.code}: {error.read().decode()[:300]}", file=sys.stderr)
        return 1
    except urllib.error.URLError as error:
        print(f"canvas server unreachable at {options.server}: {error}", file=sys.stderr)
        return 1

    encoded = answer.get("data") or answer.get("image") or ""
    if "," in encoded[:64]:
        encoded = encoded.split(",", 1)[1]
    if not encoded:
        print("no image came back; is the canvas open in a browser tab?", file=sys.stderr)
        return 1

    path = pathlib.Path(options.output)
    path.write_bytes(base64.b64decode(encoded))
    print(f"{path}: {path.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
