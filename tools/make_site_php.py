#!/usr/bin/env python3
"""Turns a copy of the website into its PHP version, for a host that runs PHP but cannot be told which headers to send. Used by make_site_zip.sh --php.
Each page becomes a .php that sends the two headers and then is the same page. The engine worker gets one too: a dedicated worker's script needs its own
Cross-Origin-Embedder-Policy, or the browser refuses to start it. Usage: make_site_php.py <folder>"""
import os, re, sys

folder = sys.argv[1]
PAGES = ["index", "online", "tutorial", "librarian", "install"]
HEADERS = "header('Cross-Origin-Opener-Policy: same-origin'); header('Cross-Origin-Embedder-Policy: require-corp');"

def links(text):
    return re.sub(r"\b(%s)\.html\b" % "|".join(PAGES), r"\1.php", text)

for page in PAGES:
    path = os.path.join(folder, page + ".html")
    text = open(path, encoding="utf-8").read()
    assert "<?" not in text, page + " holds something PHP would read as code"
    with open(os.path.join(folder, page + ".php"), "w", encoding="utf-8") as f:
        f.write("<?php " + HEADERS + " ?>\n" + links(text))
    os.remove(path)

path = os.path.join(folder, "site.js")
text = open(path, encoding="utf-8").read()
open(path, "w", encoding="utf-8").write(links(text))

for name in ("app.js", "preset-hear.js"):
    path = os.path.join(folder, name)
    text = open(path, encoding="utf-8").read()
    assert 'new Worker("engine-worker.js"' in text
    open(path, "w", encoding="utf-8").write(text.replace('new Worker("engine-worker.js"', 'new Worker("engine-worker.php"'))

with open(os.path.join(folder, "engine-worker.php"), "w", encoding="utf-8") as f:
    f.write("<?php " + HEADERS + " header('Content-Type: text/javascript'); readfile(__DIR__ . '/engine-worker.js');\n")

with open(os.path.join(folder, "HOSTING.txt"), "w", encoding="utf-8") as f:
    f.write("""Nemesis website, PHP version. Upload every file in this folder, keeping the folders as they are.
There is nothing to set up: no database, no settings, and no server program of your own.

The host must run PHP (any version from 5.3) and serve the site over https.

The .php pages send the two headers Online Nemesis, the Tutorial and the preset editor's Hear it button need:
    Cross-Origin-Opener-Policy: same-origin
    Cross-Origin-Embedder-Policy: require-corp
If they say they cannot run on this server, something on the host removed the headers. Ask the host about that.

If the host already has an index.html in this folder, delete it, or it may be shown instead of index.php.
""")
os.remove(os.path.join(folder, "_headers"))
