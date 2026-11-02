#!/usr/bin/env python3
"""The real screens on the start page: web/www/img/screens/*.png, committed, so nothing here runs in a normal build. tests/app/render_screens.cpp draws them with the firmware's own code,
built as the board builds it (the lean voice's cost table), and this turns its PPM files into PNG. Nothing is retouched: what the labels on the page point at is in index.html.
    cmake --build tests/build --target render_screens_lean
    python3 -m venv /tmp/v && /tmp/v/bin/pip install pillow && /tmp/v/bin/python web/tools/make_screens.py"""
import os, subprocess, tempfile
from PIL import Image
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = f"{ROOT}/web/www/img/screens"
# the state in render_screens.cpp -> the file
SHOTS = {"play": "sequencer", "lock": "lock", "mix2": "mixer", "cpu": "cpu", "fx-reverb": "reverb", "slice": "slices", "browse": "browser"}
os.makedirs(OUT, exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
    subprocess.run([f"{ROOT}/tests/build/render_screens_lean", tmp, *SHOTS], check=True, stdout=subprocess.DEVNULL)
    for state, name in SHOTS.items():
        path = f"{OUT}/{name}.png"
        Image.open(f"{tmp}/{state}.ppm").convert("RGB").save(path, optimize=True)
        print(name, os.path.getsize(path), "bytes")
