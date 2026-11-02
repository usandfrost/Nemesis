#!/usr/bin/env python3
"""Preset files for the Drumboy Pro (firmware/src/app/PresetFile.h): one .PRS file per preset in /CATHODE/PRESETS on the card.

  prs.py show FILE...            print presets
  prs.py export FILE             a preset as JSON on stdout (edit it, then `new`)
  prs.py new SPEC.json [-d DIR] [--id N]  make a preset file from a JSON description (the id: the next free one from 1000, or N; an \"id\" in the JSON is ignored)
  prs.py check DIR [--renamed]   validate a folder of presets (duplicate ids and names, damaged files); --renamed also fails on a file that still has a name from content/data/renames.json

Copy the files into /CATHODE/PRESETS on the card. The firmware notices the change at the next boot and rebuilds its index (INDEX.PIX) itself.

A JSON description:
  {"name": "My Bass", "category": "BASS", "voices": 2, "banks": [null, 12],
   "pages": {"OSC1": [8 values], "OSC2": [...], "SUB": [...], "FLT1": [...], "FLT2": [...], "GRIT": [...], "AMP": [...], "MOD": [...], "GLOB": [...]},
   "fx": {"sends": [delay, reverb, chorus], "amp": [8 values] or null, "delay": [8] or null, "reverb": [8] or null, "chorus": [8] or null}}
Values are the knob numbers the panel shows (docs/PRESETS_AND_WAVETABLES.md lists the pages). A "banks" entry is a wavetable catalogue number, or null
for the oscillator's basic wave (OSC1/OSC2 WAVE 0-3 in the pages). Values outside a knob's range are brought into it by the firmware."""
import json, os, re, struct, sys, zlib

CATS = ["ALL", "BASS", "LEAD", "PAD", "PLUCK", "STAB", "POLY", "KEYS", "FX", "SEQ", "TEXTURE", "BELL"]   # = kPresetCats
PAGES = ["OSC1", "OSC2", "SUB", "FLT1", "FLT2", "GRIT", "AMP", "MOD", "GLOB"]
BODY = 148
ID_MIN, ID_MAX = 10, 32000
NAME_MAX = 20
BANKS = 200
VOICES_MAX = 5
FIRST_USER_ID = 1000


def similarity_cuts():
    """{id: name} of the presets voted out on the similarity page (content/data/similarity_cuts.json); the builders leave them out."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "content/data/similarity_cuts.json")
    if not os.path.exists(path):
        return {}
    try:
        return {int(c["id"]): str(c["name"]) for c in json.load(open(path))["cut"]}
    except (ValueError, KeyError, TypeError) as e:
        raise PresetError(f"content/data/similarity_cuts.json: {type(e).__name__}: {e} (it holds {{\"cut\": [{{\"id\", \"name\"}}, ...]}})")


def golden_skips():
    """{id: name} of the library presets skipped on the Golden Presets page (content/data/golden_votes.json); the builders leave them out. The factory presets are not in it."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "content/data/golden_votes.json")
    if not os.path.exists(path):
        return {}
    try:
        r = {int(c["id"]): str(c["name"]) for c in json.load(open(path))["skip"]}
    except (ValueError, KeyError, TypeError) as e:
        raise PresetError(f"content/data/golden_votes.json: {type(e).__name__}: {e} (it holds {{\"skip\": [{{\"id\", \"name\"}}, ...]}})")
    if any(i < 10 for i in r):
        raise PresetError("content/data/golden_votes.json: the factory presets (ids 0-9) are in the firmware and cannot be left out")
    return r


def library_cuts():
    """{id: name} of every library preset the builders leave out: voted out as too similar (similarity_cuts.json) or skipped on the Golden Presets page (golden_votes.json)."""
    out = similarity_cuts()
    for i, n in golden_skips().items():
        out.setdefault(i, n)
    return out


def cut_names():
    """The names of library_cuts() as the batches and ratings know them too (a renamed preset is cut under its old name as well)."""
    names = set(library_cuts().values())
    return names | {old for old, new in renames().items() if new in names}


def renames():
    """{old name: new name} from content/data/renames.json: presets whose name pointed at an instrument or a voice they do not sound like; the builders write the new name."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "content/data/renames.json")
    if not os.path.exists(path):
        return {}
    try:
        r = {str(k): str(v) for k, v in json.load(open(path))["renames"].items()}
    except (ValueError, KeyError, TypeError, AttributeError) as e:
        raise PresetError(f"content/data/renames.json: {type(e).__name__}: {e} (it holds {{\"renames\": {{\"old name\": \"new name\"}}}})")
    for new in r.values():
        check_name(new)
    return r


def renamed(name):
    return renames().get(name, name)


def trimmed(name, rec):
    """rec with AMP LEVEL lowered by the dB in content/data/level_trims.json for this (converter) name, if it has an entry."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "content/data/level_trims.json")
    db = json.load(open(path)).get("trims_db", {}).get(name) if os.path.exists(path) else None
    if db is None:
        return rec
    v = list(rec["v"])
    i = 6 * 8 + 5   # AMP page, LEVEL
    v[i] = max(1, min(127, int(round(v[i] * 10 ** (db / 20.0)))))
    return dict(rec, v=v)


def fx_fixed(name, rec):
    """rec with sends and the delay settings replaced by the ones in content/data/fx_fixes.json for this (converter) name, if it has an entry (raw knob numbers).
    Optional keys: delay_send, delay (the DELAY page's 8 values), reverb_send."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "content/data/fx_fixes.json")
    fix = json.load(open(path)).get("fixes", {}).get(name) if os.path.exists(path) else None
    if fix is None:
        return rec
    out = dict(rec)
    sends = list(rec["sends"])
    for key, i in (("delay_send", 0), ("reverb_send", 1)):
        if key in fix:
            sends[i] = int(fix[key])
    assert all(0 <= x <= 127 for x in sends), name
    out["sends"] = sends
    if "delay" in fix:
        row = [int(x) for x in fix["delay"]]
        assert len(row) == 8 and all(0 <= x <= 127 for x in row), name
        out["delay"] = row
        out["mask"] = rec["mask"] | 1
    return out


class PresetError(ValueError):
    pass


def _bank(b):
    if b is None or b == 255:
        return 255
    if not (0 <= b < BANKS):
        raise PresetError(f"bank {b} is not a catalogue number (0-{BANKS - 1})")
    return b


FILE_OK = re.compile(r"^[0-9A-Za-z _().-]+$")


def check_file_name(f):
    if not f.upper().endswith(".PRS") or len(f) < 5 or len(f) > 31:
        raise PresetError("the file name must be 5-31 characters ending in .PRS")
    if f[0] == "." or not FILE_OK.match(f[:-4]):
        raise PresetError("the file name may only hold letters, digits, space and _ - . ( ), and must not start with a dot")


def check_name(name):
    if not (1 <= len(name) <= NAME_MAX) or any(not (0x20 <= ord(c) <= 0x7E) for c in name) or name[0] == " ":
        raise PresetError(f"name {name!r}: 1-{NAME_MAX} printable ASCII characters, not starting with a space")


def validate(p):
    """The rules the firmware applies to a file it reads (PresetFile.h), so `check` says what the Drumboy will do."""
    if not (ID_MIN <= p["id"] <= ID_MAX):
        raise PresetError(f"id {p['id']} is outside {ID_MIN}..{ID_MAX}")
    if not (1 <= p["cat"] < len(CATS)):
        raise PresetError(f"category {p['cat']} is not one of BASS..BELL (1-{len(CATS) - 1})")
    if not (1 <= p["voices"] <= VOICES_MAX):
        raise PresetError(f"voices {p['voices']} is not 1-{VOICES_MAX}")
    for b in p["banks"]:
        if b != 255 and not (0 <= b < BANKS):
            raise PresetError(f"bank {b} is not a catalogue number (0-{BANKS - 1}) or 255")
    if p["mask"] & ~15:
        raise PresetError("unknown effect flags")


def encode(p):
    """p: dict(id, name, cat (index), voices, banks [a, b], v (72 ints), sends [3], mask, amp, delay, reverb, chorus (8 ints each)) -> 148 bytes"""
    check_name(p["name"])
    if not (ID_MIN <= p["id"] <= ID_MAX):
        raise PresetError(f"id {p['id']} is outside {ID_MIN}..{ID_MAX}")
    if not (1 <= p["cat"] < len(CATS)):
        raise PresetError("category must be one of " + " ".join(CATS[1:]))
    if not (1 <= p["voices"] <= VOICES_MAX):
        raise PresetError(f"voices must be 1-{VOICES_MAX}")
    if len(p["v"]) != 72 or any(not (-128 <= v <= 127) for v in p["v"]):
        raise PresetError("pages need 9 x 8 values, each -128..127")
    for k in ("sends",):
        if len(p[k]) != 3 or any(not (0 <= v <= 127) for v in p[k]):
            raise PresetError("sends need 3 values 0..127")
    for k in ("amp", "delay", "reverb", "chorus"):
        if len(p[k]) != 8 or any(not (0 <= v <= 127) for v in p[k]):
            raise PresetError(f"{k} needs 8 values 0..127")
    b = bytearray(BODY)
    b[0:4] = b"DBPR"
    b[4] = 1
    struct.pack_into("<H", b, 6, p["id"])
    b[8], b[9] = p["cat"], p["voices"]
    b[10], b[11] = _bank(p["banks"][0]), _bank(p["banks"][1])
    b[12:12 + len(p["name"])] = p["name"].encode("ascii")
    for i, v in enumerate(p["v"]):
        b[36 + i] = v & 0xFF
    b[108:111] = bytes(p["sends"])
    b[111] = p["mask"]
    b[112:120] = bytes(p["amp"])
    b[120:128] = bytes(p["delay"])
    b[128:136] = bytes(p["reverb"])
    b[136:144] = bytes(p["chorus"])
    struct.pack_into("<I", b, BODY - 4, zlib.crc32(bytes(b[:BODY - 4])) & 0xFFFFFFFF)
    return bytes(b)


def decode(b):
    if len(b) != BODY:
        raise PresetError(f"{len(b)} bytes (a preset file is {BODY})")
    if b[0:4] != b"DBPR" or b[4] != 1 or b[5] != 0:
        raise PresetError("not a version 1 preset file")
    if zlib.crc32(b[:BODY - 4]) & 0xFFFFFFFF != struct.unpack_from("<I", b, BODY - 4)[0]:
        raise PresetError("the checksum does not match")
    name = bytes(b[12:36]).rstrip(b"\0").decode("ascii", "replace")
    p = dict(id=struct.unpack_from("<H", b, 6)[0], name=name, cat=b[8], voices=b[9], banks=[b[10], b[11]],
             v=[x - 256 if x > 127 else x for x in b[36:108]], sends=list(b[108:111]), mask=b[111],
             amp=list(b[112:120]), delay=list(b[120:128]), reverb=list(b[128:136]), chorus=list(b[136:144]))
    check_name(name)
    if bytes(b[12 + len(name):36]).strip(b"\0"):
        raise PresetError("the name field has stray bytes")
    validate(p)
    return p


def to_spec(p):
    """The JSON description of a decoded preset."""
    fx = {"sends": p["sends"], "amp": p["amp"] if p["mask"] & 8 else None}
    for k, bit in (("delay", 1), ("reverb", 2), ("chorus", 4)):
        fx[k] = p[k] if p["mask"] & bit else None
    return {"id": p["id"], "name": p["name"], "category": CATS[p["cat"]] if p["cat"] < len(CATS) else p["cat"], "voices": p["voices"],
            "banks": [None if x == 255 else x for x in p["banks"]],
            "pages": {n: p["v"][i * 8:(i + 1) * 8] for i, n in enumerate(PAGES)}, "fx": fx}


def _ints(v, n, what, lo=None, hi=None):
    if not isinstance(v, (list, tuple)) or len(v) != n or any(isinstance(x, bool) or not isinstance(x, int) for x in v):
        raise PresetError(f"{what}: {n} whole numbers are needed")
    if lo is not None and any(not (lo <= x <= hi) for x in v):
        raise PresetError(f"{what}: values must be {lo}..{hi}")
    return list(v)


def from_spec(s, pid=None):
    if not isinstance(s, dict):
        raise PresetError("the description must be a JSON object")
    for k in ("name", "category", "pages"):
        if k not in s:
            raise PresetError(f"the description has no '{k}'")
    if not isinstance(s["name"], str):
        raise PresetError("name must be text")
    cat = s.get("category")
    if cat not in CATS[1:]:
        raise PresetError("category must be one of " + " ".join(CATS[1:]))
    pages = s["pages"]
    if not isinstance(pages, dict) or set(pages) != set(PAGES):
        raise PresetError("pages needs " + ", ".join(PAGES) + ", each with 8 values")
    v = [x for n in PAGES for x in _ints(pages[n], 8, f"page {n}", -128, 127)]
    fx = s.get("fx") or {}
    if not isinstance(fx, dict):
        raise PresetError("fx must be an object")
    z = [0] * 8
    parts = {k: (_ints(fx[k], 8, f"fx {k}", 0, 127) if fx.get(k) else None) for k in ("amp", "delay", "reverb", "chorus")}
    mask = (1 if parts["delay"] else 0) | (2 if parts["reverb"] else 0) | (4 if parts["chorus"] else 0) | (8 if parts["amp"] else 0)
    banks = s.get("banks") or [None, None]
    if not isinstance(banks, (list, tuple)) or len(banks) != 2 or any(b is not None and (isinstance(b, bool) or not isinstance(b, int)) for b in banks):
        raise PresetError("banks must be two entries, each a catalogue number or null")
    voices = s.get("voices", 1)
    if isinstance(voices, bool) or not isinstance(voices, int):
        raise PresetError("voices must be a whole number")
    pid = pid if pid is not None else s.get("id")
    if isinstance(pid, bool) or not isinstance(pid, int):
        raise PresetError("id must be a whole number")
    return dict(id=pid, name=s["name"], cat=CATS.index(cat), voices=voices, banks=list(banks), v=v,
                sends=_ints(fx.get("sends") or [0, 0, 0], 3, "fx sends", 0, 127), mask=mask,
                amp=parts["amp"] or z, delay=parts["delay"] or z, reverb=parts["reverb"] or z, chorus=parts["chorus"] or z)


def file_name(p):
    safe = re.sub(r"[^0-9A-Za-z _().-]", "_", p["name"])
    return f"{p['id']:04d} {safe}.PRS"


def load_dir(d, strict=True):
    """[(file name, preset)] of the folder's valid presets; a bad file raises with its name (strict) or is skipped."""
    out = []
    for f in sorted(os.listdir(d)):
        if f.upper().endswith(".PRS") and not f.startswith("."):
            try:
                out.append((f, decode(open(os.path.join(d, f), "rb").read())))
            except PresetError as e:
                if strict:
                    raise PresetError(f"{f}: {e}")
    return out


def cmd_show(files):
    for f in files:
        p = decode(open(f, "rb").read())
        print(f"{p['id']:5d}  {CATS[p['cat']]:8s} voices {p['voices']}  banks {p['banks']}  {p['name']}   sends {p['sends']} fx mask {p['mask']:04b}")


def cmd_check(d, renamed=False):
    bad = 0
    ids = {}
    names = {}
    old = renames() if renamed else {}
    for f in sorted(os.listdir(d)):
        if not f.upper().endswith(".PRS"):
            continue
        if f.startswith("."):
            print(f"{f}: a hidden file; the Drumboy does not look at it")
            continue
        try:
            check_file_name(f)
            p = decode(open(os.path.join(d, f), "rb").read())
            if p["id"] in ids:
                raise PresetError(f"the id {p['id']} is already used by {ids[p['id']]}")
            ids[p["id"]] = f
            if p["name"].lower() in names:
                raise PresetError(f"the name {p['name']!r} is already used by {names[p['name'].lower()]}")
            names[p["name"].lower()] = f
            if p["name"] in old:
                raise PresetError(f"still has the name {p['name']!r}, renamed to {old[p['name']]!r} in content/data/renames.json")
        except PresetError as e:
            print(f"{f}: {e}")
            bad += 1
    print(f"{len(ids)} presets ok, {bad} not")
    return 1 if bad else 0


def cmd_new(spec_file, d, want_id=None):
    try:
        s = json.load(open(spec_file))
    except (OSError, json.JSONDecodeError) as e:
        raise PresetError(f"{spec_file}: {e}")
    used = {p["id"] for _, p in load_dir(d, strict=False)} if os.path.isdir(d) else set()
    pid = want_id   # the "id" an export carries is only information: a new preset takes the next free id unless --id says otherwise
    if pid is None:
        pid = FIRST_USER_ID
        while pid in used:
            pid += 1
    elif pid in used:
        raise PresetError(f"the id {pid} is already used in {d}")
    p = from_spec(s, pid)
    data = encode(p)   # (checked before anything is written)
    path = os.path.join(d, file_name(p))
    check_file_name(os.path.basename(path))
    os.makedirs(d, exist_ok=True)
    open(path, "wb").write(data)
    print(path, f"(id {pid})")


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    try:
        if argv[1] == "show":
            cmd_show(argv[2:])
        elif argv[1] == "export":
            json.dump(to_spec(decode(open(argv[2], "rb").read())), sys.stdout, indent=1)
            print()
        elif argv[1] == "new":
            d = argv[argv.index("-d") + 1] if "-d" in argv else "."
            cmd_new(argv[2], d, int(argv[argv.index("--id") + 1]) if "--id" in argv else None)
        elif argv[1] == "check":
            return cmd_check(argv[2], "--renamed" in argv)
        else:
            print(__doc__)
            return 2
    except PresetError as e:
        print("error:", e)
        return 1
    except (KeyError, TypeError, ValueError, IndexError, OSError) as e:
        print(f"error: {type(e).__name__}: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
