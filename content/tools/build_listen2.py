#!/usr/bin/env python3
"""build_listen2.py - the second listening page: the 131 library presets through the firmware's real code, WITH EFFECTS and DRY (and the original upstream
engine, dry), encoded to mp3 and written as ONE self-contained page, content/out/listen2.html, plus the objective checks, content/out/listen2_checks.json.

Input (scratch dir, default .../scratchpad/listen2):
    wav/fx_NNN.wav (stereo) wav/dry_NNN.wav (mono)   from render_listen (cmake: content/tools/listen.cmake); NNN = the preset's number in the firmware's list
    stats.jsonl                                       render_listen's JSON line per preset
    hy/NNN.wav, up_stats.jsonl                        upstream_render with the CORRECTED category's phrase (upstream_render < hy_lines.txt)
and content/data/{cut,ratings}.json. Encoding: 32 kHz mp3, 12 Hz high-pass (some patches carry DC); WITH EFFECTS joint stereo, DRY and ORIGINAL mono; the tail after the last
sound above -80 dBFS is cut (+150 ms, 50 ms fade).
"""
import array, base64, concurrent.futures as cf, datetime, glob, json, os, subprocess, wave

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
S = os.environ.get("DBOY_LISTEN2_SCRATCH", "/tmp/dboy-content")
OUT = os.path.join(REPO, "content", "out")
KB = {"fx": int(os.environ.get("DBOY_KBPS_FX", "48")), "dry": int(os.environ.get("DBOY_KBPS_DRY", "40")), "hy": int(os.environ.get("DBOY_KBPS_HY", "40"))}
HP = os.environ.get("DBOY_HP", "12")   # Hz; 25 Hz took up to 10 dB off a few sub-bass presets
CATS = ["BASS", "POLY", "PAD", "LEAD", "KEYS", "BELL", "STAB", "FX", "PLUCK", "SEQ", "TEXTURE"]   # the order of the user's counts


def active_len(path, thr=1e-4):
    w = wave.open(path)
    ch, n = w.getnchannels(), w.getnframes()
    a = array.array("h")
    a.frombytes(w.readframes(n))
    lim = int(thr * 32768)
    for i in range(len(a) - 1, -1, -1):
        if abs(a[i]) > lim:
            return i // ch / 44100.0, n / 44100.0
    return 0.0, n / 44100.0


def enc(job):
    src, dst, ch, kb = job
    last, total = active_len(src)
    dur = min(total, last + 0.15)
    subprocess.check_call(["ffmpeg", "-loglevel", "error", "-y", "-i", src, "-t", "%.3f" % dur, "-af", "highpass=f=%s,afade=t=out:st=%.3f:d=0.05" % (HP, dur - 0.05),
                           "-ac", str(ch), "-ar", "32000", "-c:a", "libmp3lame", "-b:a", "%dk" % kb] + (["-joint_stereo", "1"] if ch == 2 else []) + [dst])
    return dst, dur, total


def uri(path):
    return "data:audio/mpeg;base64," + base64.b64encode(open(path, "rb").read()).decode()


def main():
    stats = [json.loads(l) for l in open(os.path.join(S, "stats.jsonl"))]
    stats.sort(key=lambda r: r["i"])
    hy = {json.loads(l)["i"]: json.loads(l) for l in open(os.path.join(S, "up_stats.jsonl"))}
    cut = {p["name"]: p for p in json.load(open(os.path.join(REPO, "content", "data", "cut.json")))["presets"]}
    rat = json.load(open(os.path.join(REPO, "content", "data", "ratings.json")))["ratings"]
    alias = {"Iron Squelch 2": "Iron Squelch"}   # renamed in the firmware's list (the factory has an Iron Squelch)
    mp = os.path.join(S, "mp3")
    os.makedirs(mp, exist_ok=True)
    jobs = []
    for r in stats:
        i = r["i"]
        jobs.append(("%s/wav/fx_%03d.wav" % (S, i), "%s/fx_%03d.mp3" % (mp, i), 2, KB["fx"]))
        jobs.append(("%s/wav/dry_%03d.wav" % (S, i), "%s/dry_%03d.mp3" % (mp, i), 1, KB["dry"]))
        if i in hy:
            jobs.append(("%s/hy/%03d.wav" % (S, i), "%s/hy_%03d.mp3" % (mp, i), 1, KB["hy"]))
    with cf.ThreadPoolExecutor(8) as ex:
        info = {dst: (dur, tot) for dst, dur, tot in ex.map(enc, jobs)}
    items, audio, raw = [], [], 0
    for r in stats:
        i = r["i"]
        key = alias.get(r["name"], r["name"])
        c = cut[key]
        pr = rat.get(key, {})
        fxp, dp, hp = ("%s/%s_%03d.mp3" % (mp, k, i) for k in ("fx", "dry", "hy"))
        has_hy = os.path.exists(hp)
        raw += os.path.getsize(fxp) + os.path.getsize(dp) + (os.path.getsize(hp) if has_hy else 0)
        sends = r["sends"]
        amp = r["ampName"] if r["ampModel"] > 0 else ""
        diff = r["diffDb"]
        items.append(dict(n=r["name"], c=r["cat"], i=i, v=r["voices"], va=r["voicesAsked"], b=[x for x in r["bankNames"] if x], sd=sends, amp=amp, dr=r["ampDrive"] if amp else 0,
                          pr=pr.get("rating", 0), was=c.get("listenCategory", ""), ph=r["phrase"], hy=1 if has_hy else 0,
                          fxd=None if diff < -100 else round(diff), tail=r["fx"]["tail60S"], tailDry=r["dry"]["tail60S"], miss=r["notesSilent"], nn=r["notes"],
                          dur=[round(info[fxp][0], 2), round(info[dp][0], 2)]))
        audio.append([uri(fxp), uri(dp), uri(hp) if has_hy else ""])
    # objective summary text for the page
    withfx = [r for r in stats if r["diffDb"] > -100]
    summary = dict(
        renders=len(stats) * 2, finite=all(r["finiteDry"] and r["finiteFx"] for r in stats),
        clipped=sum(r["dry"]["clipped"] + r["fx"]["clipped"] for r in stats), silent=sum(r["dry"]["silent"] + r["fx"]["silent"] for r in stats),
        withEffects=len(withfx), unchanged=len(stats) - len(withfx),
        layerOver=dict((str(t), sum(1 for r in stats if r["diffDb"] > t)) for t in (-40, -26, -12, -6, 0)),
        voicesTrimmed=sum(1 for r in stats if r["voices"] < r["voicesAsked"]), droppedNotes=[r["name"] for r in stats if r["notesSilent"]],
        gainLimitedByFx=sum(1 for r in stats if r["gainLimitedByFx"]))
    order = [c for c in CATS if any(x["c"] == c for x in items)]
    order += sorted({x["c"] for x in items} - set(order))
    html = (TEMPLATE.replace("/*ITEMS*/", json.dumps(items, separators=(",", ":"))).replace("/*CATS*/", json.dumps(order))
            .replace("/*AUDIO*/", json.dumps(audio, separators=(",", ":"))).replace("/*SUMMARY*/", json.dumps(summary))
            .replace("__KBPS__", "%d / %d / %d" % (KB["fx"], KB["dry"], KB["hy"])).replace("__N__", str(len(items))))
    path = os.path.join(OUT, "listen2.html")
    open(path, "w").write(html)
    checks = dict(page="drumboy-listen-v2", generated=datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"), summary=summary,
                  method=dict(
                      renderer="content/tools/render_listen.cpp: firmware app/*.cpp, audio/*.cpp, fx/*.cpp and the fork DSP built in double on the host, wavetable banks through audio::WaveStore from content/data/wtp",
                      load="as BROWSE: applyPresetFx, applyPresetIndex, voices = presetVoices then fitVoicesToBudget; project defaults (master 100); the dry twin has the same voices, every send 0 and the amp OFF",
                      level="one gain per preset, set from the DRY twin: its active-region RMS (mono sum, frames above -30 dB of the loudest) to -20 dBFS, peak at most 0.89 over BOTH renders; the WITH EFFECTS file uses the same gain",
                      figures="addedDb: whole-render energy with effects against dry (dB); diffDb: energy of (with effects - dry) against dry (dB; the effect layer); tailFx/DryDb: RMS dBFS from last note-off + 0.5 s to the end at the shared gain; tail60S: seconds after the last note-off until the 50 ms RMS stays under -60 dBFS",
                      notesSilent="notes of the phrase whose first 60 ms are silent in the dry render (a dropped note)"),
                  presets=stats)
    json.dump(checks, open(os.path.join(OUT, "listen2_checks.json"), "w"), indent=1)
    print("%d presets, mp3 total %.2f MB, listen2.html %.2f MB" % (len(items), raw / 1e6, os.path.getsize(path) / 1e6))
    print(json.dumps(summary))


TEMPLATE = r"""<title>Drumboy Library Listening</title>
<meta name="claude:viewport-fit-sides" content="cover">
<style>
/* Layout: a sticky bar (category tabs, A/B, stop, export) over one list of rows. A row is the preset's name and facts, three play buttons (WITH FX is the
   primary), the effects it uses as small send bars, then rating keys and a note. Accents are the panel's own: yellow for the Drumboy with its effects
   (the AMP page), cyan for the original upstream engine (the GLOBAL page); DRY is neutral. */
:root {
  --bg: #101318; --panel: #181c23; --panel2: #1f242d; --line: #2a303b; --fg: #e8ebf1; --mute: #8f98a9;
  --drum: #ffd23d; --drum-ink: #1b1604; --orig: #19d3e6; --orig-ink: #04181b;
  --good: #5ff08a; --warn: #ffd23d; --bad: #ff5d5d; --focus: #ff3da6; --track: #2d3441;
  --mono: ui-monospace, "SF Mono", Menlo, Consolas, monospace; --sans: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  color-scheme: dark;
}
@media (prefers-color-scheme: light) { :root:not([data-theme="dark"]) {
  --bg: #f3f4f7; --panel: #ffffff; --panel2: #eceef3; --line: #d5d9e1; --fg: #15181e; --mute: #5d6678; --track: #dde1e9;
  --drum: #b78600; --drum-ink: #ffffff; --orig: #0a8796; --orig-ink: #ffffff;
  --good: #1a8a46; --warn: #9a6f00; --bad: #c42b2b; --focus: #c4146f; color-scheme: light; } }
:root[data-theme="light"] {
  --bg: #f3f4f7; --panel: #ffffff; --panel2: #eceef3; --line: #d5d9e1; --fg: #15181e; --mute: #5d6678; --track: #dde1e9;
  --drum: #b78600; --drum-ink: #ffffff; --orig: #0a8796; --orig-ink: #ffffff;
  --good: #1a8a46; --warn: #9a6f00; --bad: #c42b2b; --focus: #c4146f; color-scheme: light; }
* { box-sizing: border-box; }
body { background: var(--bg); color: var(--fg); font: 15px/1.45 var(--sans); margin: 0; padding-inline: max(16px, env(safe-area-inset-left, 0px)) max(16px, env(safe-area-inset-right, 0px)); padding-block: 0 48px; }
body::before { content: ""; position: fixed; inset: 0 0 auto; height: env(safe-area-inset-top, 0px); z-index: 30; background: var(--bg); }
header.top { padding-block: 20px 8px; max-width: 1000px; margin-inline: auto; }
h1 { font-size: 22px; line-height: 1.2; margin: 0 0 6px; letter-spacing: .01em; text-wrap: balance; }
.lede { color: var(--mute); margin: 0 0 8px; max-width: 72ch; font-size: 14px; }
.lede b { color: var(--fg); font-weight: 600; }
details { color: var(--mute); font-size: 13px; max-width: 72ch; }
details summary { cursor: pointer; color: var(--fg); }
details p { margin: 6px 0; }
.bar { position: sticky; top: env(safe-area-inset-top, 0px); z-index: 20; background: var(--bg); border-bottom: 1px solid var(--line); margin-inline: calc(-1 * max(16px, env(safe-area-inset-left, 0px))); padding: 10px max(16px, env(safe-area-inset-right, 0px)) 10px max(16px, env(safe-area-inset-left, 0px)); }
.bar-in { max-width: 1000px; margin-inline: auto; display: flex; flex-wrap: wrap; gap: 8px 10px; align-items: center; }
.tabs { display: flex; flex-wrap: wrap; gap: 6px; min-width: 0; flex: 1 1 360px; }
button { font: inherit; color: var(--fg); background: var(--panel2); border: 1px solid var(--line); border-radius: 6px; padding: 6px 10px; cursor: pointer; min-height: 34px; }
button:hover { border-color: var(--mute); }
:focus-visible { outline: 2px solid var(--focus); outline-offset: 2px; }
.tab { font-family: var(--mono); font-size: 12px; letter-spacing: .06em; padding: 6px 9px; }
.tab[aria-selected="true"] { background: var(--fg); color: var(--bg); border-color: var(--fg); }
.tab small { opacity: .65; margin-left: 4px; }
.tools { display: flex; gap: 8px; flex-wrap: wrap; }
#stop { border-color: var(--bad); color: var(--bad); }
#ab { font-family: var(--mono); font-size: 12px; letter-spacing: .08em; font-weight: 600; }
#ab[aria-pressed="true"] { background: var(--drum); color: var(--drum-ink); border-color: var(--drum); }
main { max-width: 1000px; margin-inline: auto; padding-top: 10px; }
.sub { color: var(--mute); font-size: 13px; margin: 6px 0 10px; display: flex; flex-wrap: wrap; gap: 4px 16px; }
ol { list-style: none; margin: 0; padding: 0; display: grid; gap: 8px; }
li.row { background: var(--panel); border: 1px solid var(--line); border-radius: 8px; padding: 10px 12px; display: grid; gap: 8px 14px; grid-template-columns: minmax(0, 1fr) auto; }
li.row.sel { border-color: var(--focus); box-shadow: 0 0 0 1px var(--focus); }
.id { min-width: 0; }
.nm { font-weight: 600; font-size: 16px; overflow-wrap: anywhere; display: flex; flex-wrap: wrap; gap: 2px 10px; align-items: baseline; }
.prev { font-family: var(--mono); font-size: 11px; font-weight: 500; color: var(--mute); border: 1px solid var(--line); border-radius: 4px; padding: 0 6px; white-space: nowrap; }
.prev b { color: var(--fg); }
.meta { font-family: var(--mono); font-size: 12px; color: var(--mute); display: flex; flex-wrap: wrap; gap: 2px 12px; margin-top: 3px; }
.meta .warn { color: var(--warn); } .meta .bad { color: var(--bad); }
.fxline { grid-column: 1 / -1; display: flex; flex-wrap: wrap; gap: 5px 6px; align-items: center; }
.fxc { font-family: var(--mono); font-size: 11px; letter-spacing: .04em; padding: 2px 8px; border-radius: 10px; border: 1px solid var(--line); color: var(--fg); display: inline-flex; align-items: center; gap: 6px; }
.fxc i { display: inline-block; width: 34px; height: 5px; border-radius: 3px; background: var(--track); position: relative; overflow: hidden; }
.fxc i::after { content: ""; position: absolute; inset: 0 auto 0 0; width: var(--v); background: var(--drum); border-radius: 3px; }
.fxc.none { color: var(--mute); }
.fxc.amp i::after { background: var(--bad); }
.play { display: flex; gap: 8px; align-items: flex-start; flex-wrap: wrap; justify-content: flex-end; }
.pb { font-family: var(--mono); font-size: 12px; letter-spacing: .03em; white-space: nowrap; min-width: 96px; font-weight: 600; position: relative; overflow: hidden; }
.pb.fx { background: var(--drum); color: var(--drum-ink); border-color: var(--drum); min-width: 116px; }
.pb.dry { background: var(--panel2); color: var(--fg); border-color: var(--mute); }
.pb.orig { background: var(--orig); color: var(--orig-ink); border-color: var(--orig); }
.pb[disabled] { opacity: .35; cursor: default; }
.pb .prog { position: absolute; left: 0; bottom: 0; height: 3px; width: 0; background: currentColor; opacity: .55; }
.pb.on::after { content: ""; position: absolute; inset: 0; box-shadow: inset 0 0 0 2px var(--fg); border-radius: 5px; }
.rate { grid-column: 1 / -1; display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
.stars { display: flex; gap: 4px; }
.stars button { padding: 0; width: 34px; font-family: var(--mono); font-weight: 600; }
.stars button[aria-pressed="true"] { background: var(--fg); color: var(--bg); border-color: var(--fg); }
.note { flex: 1 1 220px; min-width: 0; font: inherit; color: var(--fg); background: var(--bg); border: 1px solid var(--line); border-radius: 6px; padding: 6px 9px; min-height: 34px; }
.keys { color: var(--mute); font-size: 12px; margin-top: 18px; max-width: 80ch; }
kbd { font-family: var(--mono); border: 1px solid var(--line); border-bottom-width: 2px; border-radius: 4px; padding: 0 5px; background: var(--panel2); }
#out { width: 100%; height: 160px; margin-top: 10px; font-family: var(--mono); font-size: 12px; color: var(--fg); background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 8px; }
#toast { position: fixed; left: 50%; bottom: calc(18px + env(safe-area-inset-bottom, 0px)); transform: translateX(-50%); background: var(--fg); color: var(--bg); padding: 8px 14px; border-radius: 6px; font-size: 13px; z-index: 40; max-width: calc(100% - 32px); }
@media (max-width: 560px) { li.row { grid-template-columns: 1fr; } .play { justify-content: stretch; } .pb { flex: 1; min-width: 0; padding-inline: 6px; } }
@media (prefers-reduced-motion: no-preference) { .fxc i::after { transition: width .2s; } }
</style>
<header class="top">
  <h1>Drumboy Library Listening</h1>
  <p class="lede">The <b>__N__ library presets</b>, rendered through the firmware's own code: the preset mapper, the wavetable banks and the effects section (delay, reverb, chorus, amp), loaded the way BROWSE loads a preset. <b>WITH FX</b> is what you hear after loading it. <b>DRY</b> is the same sound with every send at 0 and the amp off, at the same gain, so the difference you hear is the effects alone. <b>ORIGINAL</b> is the upstream engine without its effects, normalised on its own. Categories are the ones you set in your notes on the first page; the grey badge shows your first rating. A preset that uses no effects sounds the same in WITH FX and DRY.</p>
  <details><summary>Objective checks</summary><div id="sum"></div></details>
</header>
<div class="bar"><div class="bar-in">
  <div class="tabs" role="tablist" id="tabs"></div>
  <div class="tools"><button id="ab" aria-pressed="false" title="A/B: Space or Tab flips between WITH FX and DRY at the same position (A)">A/B</button><button id="stop" title="Stop all (S)">Stop all</button><button id="export" title="Copy ratings as JSON">Export ratings</button></div>
</div></div>
<main>
  <div class="sub" id="sub"></div>
  <ol id="list"></ol>
  <textarea id="out" hidden readonly aria-label="Ratings as JSON"></textarea>
  <p class="keys"><kbd>&uarr;</kbd> <kbd>&darr;</kbd> move &middot; <kbd>Space</kbd> WITH FX &middot; <kbd>D</kbd> DRY &middot; <kbd>O</kbd> ORIGINAL &middot; <kbd>A</kbd> A/B on or off (then <kbd>Space</kbd> or <kbd>Tab</kbd> flips WITH FX and DRY in place) &middot; <kbd>1</kbd>-<kbd>5</kbd> rate &middot; <kbd>&larr;</kbd> <kbd>&rarr;</kbd> category &middot; <kbd>S</kbd> stop.
  A note means "this belongs in category ...": type the category (BASS, POLY, PAD, LEAD, KEYS, BELL, STAB, FX, PLUCK, SEQ, TEXTURE) to move a sound, anything else is a comment. Ratings stay in this browser until you press Export. Audio: mp3 at 32 kHz, __KBPS__ kbit/s (WITH FX stereo, DRY and ORIGINAL mono), so the highs are dulled; judge character and effect amount, not the top end. Rendered on a computer in double precision with no CPU limit, so the device may sound slightly different.</p>
</main>
<div id="toast" hidden></div>
<script id="audio" type="application/json">/*AUDIO*/</script>
<script>
"use strict";
const ITEMS = /*ITEMS*/;
const CATS = /*CATS*/;
const SUMMARY = /*SUMMARY*/;
const KEY = "dboy-listen-v2";
const FXN = ["DELAY", "REVERB", "CHORUS"];
let AUDIO = null, store = {}, cat = CATS[0], sel = 0, ab = false, player = new Audio(), playing = null, raf = 0;
try { store = JSON.parse(localStorage.getItem(KEY) || "{}") || {}; } catch (e) { store = {}; }
function save() { try { localStorage.setItem(KEY, JSON.stringify(store)); } catch (e) { /* private window: ratings last until the tab closes */ } }
function sid(it) { return it.c + "/" + it.n; }
function rec(it) { return store[sid(it)] || { r: 0, note: "" }; }   // a read never creates an entry
function edit(it) { return store[sid(it)] || (store[sid(it)] = { r: 0, note: "" }); }
function toast(t) { const el = document.getElementById("toast"); el.textContent = t; el.hidden = false; clearTimeout(toast.t); toast.t = setTimeout(() => { el.hidden = true; }, 2400); }
const $ = (s, r) => (r || document).querySelector(s);
const esc = s => String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/"/g, "&quot;");
const rows = () => ITEMS.map((it, i) => ({ it, i })).filter(x => x.it.c === cat);
const WNAME = ["WITH FX", "DRY", "ORIGINAL"];

function renderSummary() {
  const s = SUMMARY, el = $("#sum"), l = s.layerOver;
  const lines = [
    s.renders + " renders (" + ITEMS.length + " presets, with effects and dry): all finite " + (s.finite ? "yes" : "NO") + ", clipped samples " + s.clipped + ", silent renders " + s.silent + ".",
    s.withEffects + " presets use effects; " + s.unchanged + " have none, so their two versions are identical. The effect layer (with effects minus dry) is within 40 dB of the dry sound in " + l["-40"] + " presets, within 26 dB in " + l["-26"] + ", within 12 dB in " + l["-12"] + ", within 6 dB in " + l["-6"] + ", and louder than the dry in " + l["0"] + ".",
    s.voicesTrimmed + " presets run on fewer voices than they ask for because their effects need CPU (the same trim BROWSE makes). " + s.gainLimitedByFx + " were turned down a little so the effects version stays under full scale; the dry twin then sits slightly under -20 dBFS.",
    s.droppedNotes.length ? "The firmware drops some notes on a one-voice part when a new note starts while the last is still releasing: " + s.droppedNotes.join(", ") + " (marked in their rows). That is a firmware matter, not a preset setting." : ""];
  el.textContent = "";
  for (const t of lines) if (t) { const p = document.createElement("p"); p.textContent = t; el.appendChild(p); }
}
function renderTabs() {
  const t = $("#tabs");
  t.textContent = "";
  for (const c of CATS) {
    const b = document.createElement("button");
    const n = ITEMS.filter(x => x.c === c).length, rated = ITEMS.filter(x => x.c === c && rec(x).r).length;
    b.className = "tab"; b.setAttribute("role", "tab"); b.setAttribute("aria-selected", String(c === cat));
    b.innerHTML = c + "<small>" + (rated ? rated + "/" : "") + n + "</small>";
    b.onclick = () => { cat = c; sel = 0; render(); };
    t.appendChild(b);
  }
}
function fxChips(it) {
  const out = [];
  it.sd.forEach((v, k) => { if (v > 0) out.push('<span class="fxc" title="' + FXN[k] + ' send ' + v + ' of 127">' + FXN[k] + ' <i style="--v:' + Math.round(100 * v / 127) + '%"></i></span>'); });
  if (it.amp) out.push('<span class="fxc amp" title="amp model ' + esc(it.amp) + ', drive ' + it.dr + ' of 127">AMP ' + esc(it.amp) + ' <i style="--v:' + Math.round(100 * it.dr / 127) + '%"></i></span>');
  return out.length ? out.join("") : '<span class="fxc none">NO EFFECTS</span>';
}
function render() {
  renderTabs();
  const list = $("#list"), rs = rows();
  list.textContent = "";
  $("#sub").textContent = "";
  const withFx = rs.filter(x => x.it.fxd !== null).length;
  for (const t of [rs.length + " presets", withFx + " use effects", "phrase: " + (rs[0] ? rs[0].it.ph : "")]) { const s = document.createElement("span"); s.textContent = t; $("#sub").appendChild(s); }
  rs.forEach((x, k) => {
    const it = x.it, r = rec(it);
    const li = document.createElement("li");
    li.className = "row" + (k === sel ? " sel" : ""); li.dataset.k = k; li.dataset.i = x.i;
    const voices = it.v + (it.v === 1 ? " voice" : " voices") + (it.v < it.va ? ' <span class="warn" title="The effects need CPU, so BROWSE gives this preset fewer voices than it asks for">(asks ' + it.va + ', CPU)</span>' : "");
    const banks = it.b.length ? it.b.map(esc).join(" + ") : "basic waves";
    const layer = it.fxd === null ? "no effects" : "effect layer " + (it.fxd > 0 ? "+" : "") + it.fxd + " dB vs dry";
    const tail = it.fxd !== null && it.tail > it.tailDry + 0.2 ? "tail " + it.tail.toFixed(1) + " s" : "";
    const prev = '<span class="prev" title="Your rating on the first page">1st <b>' + (it.pr || "-") + "</b>" + (it.was && it.was !== it.c ? " &middot; was " + esc(it.was) : "") + "</span>";
    li.innerHTML =
      '<div class="id"><div class="nm"><span class="n"></span>' + prev + '</div><div class="meta"><span>' + voices + "</span><span>" + banks + "</span><span>" + layer + "</span>" + (tail ? "<span>" + tail + "</span>" : "") +
      (it.miss ? '<span class="bad" title="In the render these notes of the phrase are silent: the firmware drops a note on a one-voice part that starts while the last is releasing">drops ' + it.miss + " of " + it.nn + " notes</span>" : "") + "</div></div>" +
      '<div class="play"><button class="pb fx" data-w="0">&#9654; WITH FX<span class="prog"></span></button><button class="pb dry" data-w="1">&#9654; DRY<span class="prog"></span></button>' +
      '<button class="pb orig" data-w="2"' + (it.hy ? "" : " disabled") + '>&#9654; ORIGINAL<span class="prog"></span></button></div>' +
      '<div class="fxline">' + fxChips(it) + "</div>" +
      '<div class="rate"><div class="stars" role="group" aria-label="Rating">' + [1, 2, 3, 4, 5].map(n => '<button data-r="' + n + '" aria-pressed="' + (r.r === n) + '" aria-label="' + n + ' of 5">' + n + "</button>").join("") +
      '</div><input class="note" type="text" maxlength="300" placeholder="Note, or a category it belongs in" aria-label="Note for ' + esc(it.n) + '"></div>';
    $(".n", li).textContent = it.n;
    $(".note", li).value = r.note;
    li.addEventListener("click", e => {
      sel = k; markSel();
      const pb = e.target.closest(".pb"), st = e.target.closest("[data-r]");
      if (pb && !pb.disabled) { const w = +pb.dataset.w, same = playing && playing.i === x.i && playing.w === w; play(x.i, w, ab && w < 2 && !same); }
      if (st) rate(x.i, +st.dataset.r);
    });
    $(".note", li).addEventListener("input", e => { edit(it).note = e.target.value; save(); });
    $(".note", li).addEventListener("focus", () => { sel = k; markSel(); });
    list.appendChild(li);
  });
  paintPlaying();
}
function markSel() { document.querySelectorAll("li.row").forEach(li => li.classList.toggle("sel", +li.dataset.k === sel)); }
function rate(i, n) {
  const r = edit(ITEMS[i]); r.r = r.r === n ? 0 : n; save();
  const li = document.querySelector('li.row[data-i="' + i + '"]');
  if (li) li.querySelectorAll("[data-r]").forEach(b => b.setAttribute("aria-pressed", String(+b.dataset.r === r.r)));
  const y = window.scrollY; renderTabs(); window.scrollTo(0, y);
}
function loadAudio() { if (!AUDIO) AUDIO = JSON.parse($("#audio").textContent); return AUDIO; }
function stop() { player.pause(); playing = null; cancelAnimationFrame(raf); paintPlaying(); }
// Play version w of preset i. keep: continue at the same position (the A/B flip between versions of one row).
function play(i, w, keep) {
  if (playing && playing.i === i && playing.w === w) { if (!keep) stop(); return; }
  const src = loadAudio()[i][w];
  if (!src) return;
  const pos = keep && playing && playing.i === i ? player.currentTime : 0;
  player.pause(); player.src = src;
  playing = { i, w };
  const go = () => { try { player.currentTime = pos; } catch (e) { /* not seekable yet */ } player.play().then(tick, () => { toast("Playback was blocked; press a play button."); playing = null; paintPlaying(); }); };
  if (pos > 0) player.addEventListener("loadedmetadata", go, { once: true }); else go();
  paintPlaying();
}
function flip() {   // A/B: WITH FX <-> DRY on the selected row, in place
  const i = cur();
  if (i < 0) return;
  if (playing && playing.i === i && playing.w < 2) play(i, 1 - playing.w, true); else play(i, 0);
}
player.addEventListener("ended", stop);
function tick() { paintPlaying(); if (playing) raf = requestAnimationFrame(tick); }
function paintPlaying() {
  document.querySelectorAll(".pb").forEach(b => { b.classList.remove("on"); $(".prog", b).style.width = "0"; });
  if (!playing) return;
  const li = document.querySelector('li.row[data-i="' + playing.i + '"]');
  if (!li) return;
  const b = $('.pb[data-w="' + playing.w + '"]', li);
  b.classList.add("on");
  $(".prog", b).style.width = (player.duration ? 100 * player.currentTime / player.duration : 0) + "%";
}
function exportRatings() {
  const out = ITEMS.filter(it => { const r = store[sid(it)]; return r && (r.r || r.note); }).map(it => ({ name: it.n, category: it.c, rating: rec(it).r, note: rec(it).note }));
  const txt = JSON.stringify({ page: "drumboy-listen-v2", exported: new Date().toISOString(), rated: out.length, of: ITEMS.length, ratings: out }, null, 1);
  const ta = $("#out"); ta.value = txt; ta.hidden = false;
  const done = () => toast(out.length + " ratings copied");
  const fallback = () => { ta.focus(); ta.select(); toast("Select and copy the text below"); };
  try { navigator.clipboard.writeText(txt).then(done, fallback); } catch (e) { fallback(); }
}
function move(d) {
  const n = rows().length; sel = Math.max(0, Math.min(n - 1, sel + d)); markSel();
  const li = document.querySelector('li.row[data-k="' + sel + '"]');
  if (li) li.scrollIntoView({ block: "center", behavior: "auto" });
}
function cur() { const r = rows()[sel]; return r ? r.i : -1; }
function setAB(on) { ab = on; $("#ab").setAttribute("aria-pressed", String(ab)); toast(ab ? "A/B on: Space or Tab flips WITH FX and DRY" : "A/B off"); }
document.addEventListener("keydown", e => {
  if (e.target.matches("input, textarea") || e.metaKey || e.ctrlKey || e.altKey) { if (e.key === "Escape") e.target.blur(); return; }
  const k = e.key;
  if (k === "ArrowDown") { move(1); e.preventDefault(); }
  else if (k === "ArrowUp") { move(-1); e.preventDefault(); }
  else if (k === "ArrowRight" || k === "ArrowLeft") { const j = CATS.indexOf(cat) + (k === "ArrowRight" ? 1 : -1); if (j >= 0 && j < CATS.length) { cat = CATS[j]; sel = 0; render(); } e.preventDefault(); }
  else if (k === " " && e.target.tagName !== "BUTTON") { if (ab) flip(); else if (cur() >= 0) play(cur(), 0); e.preventDefault(); }
  else if (k === "Tab" && ab && !e.shiftKey) { flip(); e.preventDefault(); }
  else if (k === "d" || k === "D") { if (cur() >= 0) play(cur(), 1, ab); }
  else if ((k === "o" || k === "O") && cur() >= 0 && ITEMS[cur()].hy) play(cur(), 2);
  else if (k === "a" || k === "A") setAB(!ab);
  else if (k === "s" || k === "S") stop();
  else if (k >= "1" && k <= "5" && cur() >= 0) rate(cur(), +k);
});
$("#stop").onclick = stop;
$("#export").onclick = exportRatings;
$("#ab").onclick = () => setAB(!ab);
renderSummary();
render();
</script>
"""

if __name__ == "__main__":
    main()
