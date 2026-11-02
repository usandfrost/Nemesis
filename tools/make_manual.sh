#!/usr/bin/env bash
# The Nemesis manual as a PDF: web/www/downloads/nemesis-manual.pdf, made from web/www/manual.html.
#   tools/make_manual.sh [--no-shots]     --no-shots keeps the screens already in web/www/manual
# It first gathers the screens into web/www/manual: tests/build/render_screens_lean draws most of them with the firmware's own code, and the rest come from Online Nemesis, driven with
# real key events. Then headless Chrome prints the page twice: once to find what page each chapter falls on, and again with those numbers in the contents.
# Needs node, python3, Chrome, poppler's pdfinfo, tests/build/render_screens_lean and the web build in web/www.
# Headless Chrome only, always with --mute-audio, one at a time, and always closed afterwards: the owner must never hear it.
set -eu
cd "$(dirname "$0")/.."
for tool in node python3 pdfinfo; do command -v "$tool" >/dev/null || { echo "$tool is needed"; exit 1; }; done
[ -x tests/build/render_screens_lean ] || { echo "no tests/build/render_screens_lean: cmake --build tests/build --target render_screens_lean"; exit 1; }
[ -e web/www/drumboy.wasm ] || { echo "no web/www/drumboy.wasm: build the web version first"; exit 1; }

# one headless Chrome at a time
while pgrep -f 'Google Chrome --[h]eadless' >/dev/null; do echo "another headless Chrome is running, waiting"; sleep 5; done

port=$((8700 + RANDOM % 90))
python3 web/serve.py "$port" >/dev/null 2>&1 &
server=$!
trap 'kill "$server" 2>/dev/null || true; pkill -f "user-data-dir=.*nemesis-manual-" 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do curl -s -o /dev/null "http://localhost:$port/manual.html" && break; sleep 0.1; done

node tools/make_manual.mjs "http://localhost:$port/" "$@"
ls -l web/www/downloads/nemesis-manual.pdf
