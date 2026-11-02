#!/usr/bin/env python3
"""Writes web/www/install.html from docs/INSTALL.txt, so the page and the instructions in the download cannot say different things. Usage: make_install_page.py"""
import html, os, re

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
VERSION, ZIP = "Beta2", "downloads/nemesis-beta-2-install.zip"
text = open(os.path.join(ROOT, "docs/INSTALL.txt"), encoding="utf-8").read().replace("@VERSION@", VERSION).replace("@SHORT@", VERSION.replace("Beta", "B"))
blocks = [b.strip("\n") for b in re.split(r"\n\s*\n", text) if b.strip()]
title = blocks.pop(0).title().replace("Beta 2 On A Drumboy Pro", "Beta 2")

out, opened = [], False


def close():
    global opened
    if opened:
        out.append("  </section>")
    opened = False


for b in blocks:
    lines = b.split("\n")
    if len(lines) == 1 and lines[0].replace("macOS", "MACOS") == lines[0].replace("macOS", "MACOS").upper() and re.search("[A-Z]", lines[0]):
        close()
        warn = lines[0].startswith("PLEASE READ")
        out.append('  <section class="box%s"><h2 class="box-head">%s</h2>' % (" warn" if warn else "", html.escape(lines[0].capitalize().replace("macos", "macOS").replace("windows", "Windows"))))
        opened = True
    elif re.match(r"\d+\. ", lines[0]):
        out.append('    <ol class="steps">' + "".join("<li>%s</li>" % html.escape(re.sub(r"^\d+\. ", "", l)) for l in lines) + "</ol>")
    elif lines[0].startswith("- "):
        out.append('    <ul class="steps">' + "".join("<li>%s</li>" % html.escape(re.sub(r"\s{2,}", ": ", l[2:])) for l in lines) + "</ul>")
    else:
        if not opened:
            out.append('  <section class="box">')
            opened = True
        out.append("    <p>%s</p>" % html.escape(" ".join(lines)))
close()

page = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="icon" type="image/png" href="img/icon.png">
<title>Install Nemesis Beta 2</title>
<link rel="stylesheet" href="site.css">
</head>
<body class="site">
<a class="skip" href="#top">Skip to the page</a>
<canvas id="sky" aria-hidden="true"></canvas>
<header id="bar" class="menubar"></header>
<div class="copper"></div>
<main id="top" class="wrap">
  <section class="heading"><h1 class="chrome">Install Beta 2</h1></section>
  <section class="box"><h2 class="box-head">Download</h2>
    <p>The firmware, every preset and wavetable, and these instructions. Your samples are already on your Drumboy's card and are not changed. Read the steps below first.</p>
    <p class="btns"><a class="btn go" id="download" href="%s" download>Download Nemesis Beta 2</a></p>
  </section>
%s
</main>
<script src="site.js"></script>
</body>
</html>
""" % (ZIP, "\n".join(out))
open(os.path.join(ROOT, "web/www/install.html"), "w", encoding="utf-8").write(page)
print("web/www/install.html")
