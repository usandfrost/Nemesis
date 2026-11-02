#!/usr/bin/env python3
"""Lays out what goes on the SD card, in a folder you copy onto it:
    Drumboy_Pro_Fw_CATHODE.bin      the firmware image the stock bootloader installs from the card's root (make_sd_firmware.py)
    CATHODE/PRESETS/*.PRS           the preset library, one file each (content/presets, made by gen_presets.py; content/presets2, made by cut_more.py; content/synthwave, content/techno, content/synthpop, content/trance, content/ebm, content/royksopp, content/delerium and content/voice, made by genre_presets.py;
                                    add your own with prs.py)
    CATHODE/WT/wtNNN.wtp            the wavetable banks the presets use (content/data/wtp, made by wt_pyramid)
The samples (/Sample) and the projects (/CATHODE/PROJn.DBP) are the card's own. Usage: make_sd_content.py <out dir> [--all]
(--all: every converted bank; by default only the banks the shipped presets use)"""
import json, os, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
out = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else sys.exit(__doc__)
everything = "--all" in sys.argv
os.makedirs(f"{out}/CATHODE/WT", exist_ok=True)
os.makedirs(f"{out}/CATHODE/PRESETS", exist_ok=True)
nprs = 0
banks_in_files = set()
sys.path.insert(0, HERE)
import prs
for folder in ("presets", "presets2", "synthwave", "techno", "synthpop", "trance", "ebm", "royksopp", "delerium", "voice"):
    d = f"{ROOT}/content/{folder}"
    for f in sorted(os.listdir(d) if os.path.isdir(d) else []):
        if f.upper().endswith(".PRS") and not f.startswith("."):
            shutil.copy(f"{d}/{f}", f"{out}/CATHODE/PRESETS/{f}")
            banks_in_files |= {b for b in prs.decode(open(f"{d}/{f}", "rb").read())["banks"] if b != 255}
            nprs += 1
fw = f"{ROOT}/firmware/build/Drumboy_Pro_Fw_CATHODE.bin"
if os.path.exists(fw):
    shutil.copy(fw, f"{out}/Drumboy_Pro_Fw_CATHODE.bin")
else:
    print("no firmware image yet (build the firmware, then tools/make_sd_firmware.py)")
used = None
if not everything:
    cut = json.load(open(f"{ROOT}/content/data/cut.json"))["presets"]
    pre = json.load(open(f"{ROOT}/content/data/presets.json"))
    slot2cat = {t["slot"]: t["catalogue"] for t in pre["tables"]}
    used = {slot2cat[t] for p in cut for t in p["table"] if t >= 0} | banks_in_files
n = size = 0
import re
for f in sorted(os.listdir(f"{ROOT}/content/data/wtp")):
    if not re.fullmatch(r"wt\d{3}\.wtp", f):
        continue   # (a stray .DS_Store)
    b = int(f[2:5])
    if used is None or b in used:
        shutil.copy(f"{ROOT}/content/data/wtp/{f}", f"{out}/CATHODE/WT/{f}")
        n += 1
        size += os.path.getsize(f"{ROOT}/content/data/wtp/{f}")
print(f"{out}: the firmware image, {nprs} presets in CATHODE/PRESETS and {n} wavetable banks ({size / 1e6:.1f} MB) in CATHODE/WT")
