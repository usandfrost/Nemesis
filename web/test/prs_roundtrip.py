#!/usr/bin/env python3
"""The preset editor's output through tools/prs.py: FILE holds 148-byte records, one after the other. Each must decode, and encode back to the same bytes. web/test/preset-edit.mjs calls this."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import prs

data = open(sys.argv[1], "rb").read()
n = len(data) // prs.BODY
bad = 0
for i in range(n):
    rec = data[i * prs.BODY:(i + 1) * prs.BODY]
    try:
        if prs.encode(prs.decode(rec)) != rec:
            raise prs.PresetError("came back changed")
    except prs.PresetError as e:
        bad += 1
        if bad <= 10:
            print(f"record {i}: {e}")
print(f"{n} records, {n - bad} read and written back unchanged")
sys.exit(1 if bad or not n else 0)
