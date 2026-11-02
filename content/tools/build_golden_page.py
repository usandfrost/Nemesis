#!/usr/bin/env python3
"""build_golden_page.py - one listening page for the WHOLE library (the factory ten and every .PRS file): hear a preset, Keep or Skip, until only the golden ones are left.

    build_golden_page.py <wav dir> <stats.jsonl> <out dir>

The wav dir and stats come from render_listen run on the whole library with the factory ten (DBOY_RENDER_FACTORY=1 DBOY_PRESET_SRC=<all the .PRS files>
content/tools/run_batch.sh does it in its scratch folder; it stops at its own count check because of the ten factory presets, the renders are complete by then).
<out dir> gets index.html and audio/NNN.mp3 (one file per preset, WITH effects as BROWSE plays it; the page fetches them as it goes, so nothing has to be
embedded and the page stays small). Publish the folder with the Artifact tool's `files`. The votes are kept in the artifact's `db` (collection `votes`,
one document per preset: {id, name, cat, set, vote: keep|skip}) so Claude can read them, with an export as the fallback."""
import concurrent.futures as cf, importlib.util, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import prs                                                                       # noqa: E402
spec = importlib.util.spec_from_file_location("bl2", os.path.join(HERE, "build_listen2.py"))
bl2 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bl2)
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
KBPS = int(os.environ.get("DBOY_KBPS_GOLD", "56"))
SETS = [(0, 9, "factory"), (10, 140, "round 1"), (141, 299, "synthwave"), (300, 349, "techno"), (350, 399, "synth pop"), (400, 449, "trance"), (450, 499, "EBM"),
        (500, 599, "round 2"), (600, 649, "Röyksopp"), (650, 699, "Delerium"), (700, 749, "voice pads")]


def set_of(i):
    for a, b, n in SETS:
        if a <= i <= b:
            return n
    return "saved on the panel" if i >= 1000 else "other"


def earlier_ratings():
    """{name: rating}: what the listening pages said about each preset (by the name it has now; ratings are kept under the old names)."""
    out = {}
    def put(name, r):
        if isinstance(r, int) and 1 <= r <= 5:
            out[name] = out[prs.renamed(name)] = r
    for dirpath, _, files in os.walk(os.path.join(ROOT, "content")):
        if "ratings.json" not in files:
            continue
        d = json.load(open(os.path.join(dirpath, "ratings.json")))
        rs = d["ratings"]
        if isinstance(rs, dict):
            for n, v in rs.items():
                put(n, v.get("rating"))
        else:
            for v in rs:
                put(v["name"], v.get("rating"))
    return out


def main(wavdir, stats_path, out):
    stats = sorted((json.loads(l) for l in open(stats_path)), key=lambda r: r["i"])
    os.makedirs(os.path.join(out, "audio"), exist_ok=True)
    jobs = []
    for r in stats:
        jobs.append(("%s/fx_%03d.wav" % (wavdir, r["i"]), "%s/audio/%03d.mp3" % (out, r["i"]), 2, KBPS))
    with cf.ThreadPoolExecutor(8) as ex:
        info = {dst: dur for dst, dur, _ in ex.map(bl2.enc, jobs)}
    hist = earlier_ratings()
    items = []
    for r in stats:
        i = r["i"]
        items.append(dict(id=i, n=r["name"], c=r["cat"], s=set_of(i), v=r["voices"], b=[x for x in r["bankNames"] if x], h=hist.get(r["name"], 0),
                          d=round(info["%s/audio/%03d.mp3" % (out, i)], 1), ph=r["phrase"]))
    cats = [c for c in bl2.CATS if any(x["c"] == c for x in items)] + sorted({x["c"] for x in items} - set(bl2.CATS))
    sets = [n for _, _, n in SETS if any(x["s"] == n for x in items)]
    html = TEMPLATE.replace("/*ITEMS*/", json.dumps(items, separators=(",", ":"), ensure_ascii=False)).replace("/*CATS*/", json.dumps(cats)).replace("/*SETS*/", json.dumps(sets, ensure_ascii=False))
    open(os.path.join(out, "index.html"), "w").write(html)
    mb = sum(os.path.getsize("%s/audio/%03d.mp3" % (out, r["i"])) for r in stats) / 1e6
    print("%d presets, audio %.1f MB in %s/audio, page %.0f KB" % (len(items), mb, out, len(html) / 1e3))


TEMPLATE = r'''<title>Golden Presets</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;500;600;700&display=swap">
<style>
:root {
  --bg: #f4f1ea; --panel: #fffdf8; --panel2: #ece8dd; --line: #d8d2c3; --fg: #23211c; --muted: #6d675a;
  --accent: #b35c00; --accent-fg: #fffdf8; --keep: #1f7a4d; --keep-bg: #dcefe3; --skip: #8a4b3c; --skip-bg: #f1e1db; --gold: #a87400;
  --font: "IBM Plex Sans", system-ui, -apple-system, "Segoe UI", sans-serif; --mono: "IBM Plex Mono", ui-monospace, Menlo, monospace;
}
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --bg: #17150f; --panel: #211e16; --panel2: #2b271d; --line: #3a3528; --fg: #ece7d8; --muted: #a39b86;
  --accent: #f0a050; --accent-fg: #17150f; --keep: #6fd39b; --keep-bg: #1d3a2b; --skip: #d9a08f; --skip-bg: #3a2620; --gold: #e8c25a; color-scheme: dark; } }
:root[data-theme="dark"] {
  --bg: #17150f; --panel: #211e16; --panel2: #2b271d; --line: #3a3528; --fg: #ece7d8; --muted: #a39b86;
  --accent: #f0a050; --accent-fg: #17150f; --keep: #6fd39b; --keep-bg: #1d3a2b; --skip: #d9a08f; --skip-bg: #3a2620; --gold: #e8c25a; color-scheme: dark; }
* { box-sizing: border-box; }
body { background: var(--bg); color: var(--fg); font-family: var(--font); line-height: 1.45; padding-inline: max(16px, env(safe-area-inset-left, 0px)) max(16px, env(safe-area-inset-right, 0px)); padding-block: 18px 40px; }
main { max-width: 940px; margin: 0 auto; display: flex; flex-direction: column; gap: 16px; }
h1 { font-size: 1.5rem; margin: 0; letter-spacing: -0.01em; text-wrap: balance; }
.lede { color: var(--muted); max-width: 70ch; margin: 4px 0 0; }
.mono { font-family: var(--mono); }
button { font-family: inherit; color: inherit; }
button:focus-visible, input:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
.top { display: flex; flex-wrap: wrap; gap: 8px 18px; align-items: baseline; justify-content: space-between; }
.tally { font: 500 0.85rem var(--mono); display: flex; flex-wrap: wrap; gap: 4px 14px; }
.tally .k b { color: var(--keep); } .tally .s b { color: var(--skip); } .tally .g b { color: var(--gold); }
.prog { height: 6px; border-radius: 3px; background: var(--line); overflow: hidden; display: flex; }
.prog i { display: block; height: 100%; } .prog .pk { background: var(--keep); } .prog .ps { background: var(--skip); opacity: 0.7; }
details.how { background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 10px 14px; }
details.how summary { cursor: pointer; font-weight: 500; }
details.how p, details.how li { color: var(--muted); max-width: 74ch; margin: 8px 0 0; }
details.how ul { margin: 6px 0 0; padding-left: 1.2em; }
kbd { font: 500 0.75rem var(--mono); background: var(--panel2); border: 1px solid var(--line); border-radius: 5px; padding: 1px 6px; }
.stage { background: var(--panel); border: 1px solid var(--line); border-radius: 14px; padding: 18px 18px 16px; display: flex; flex-direction: column; gap: 14px; }
.stage.keep { border-color: var(--keep); } .stage.skip { border-color: var(--skip); }
.sname { font-size: 1.7rem; font-weight: 700; letter-spacing: -0.01em; line-height: 1.15; text-wrap: balance; overflow-wrap: anywhere; }
.smeta { display: flex; flex-wrap: wrap; gap: 6px; }
.badge { font: 500 0.72rem var(--mono); padding: 2px 8px; border-radius: 6px; border: 1px solid var(--line); color: var(--muted); }
.badge.cat { color: var(--accent); border-color: var(--accent); letter-spacing: 0.06em; }
.badge.r5, .badge.r4 { color: var(--keep); border-color: var(--keep); }
.badge.st-keep { color: var(--keep); border-color: var(--keep); } .badge.st-skip { color: var(--skip); border-color: var(--skip); }
.player { display: grid; grid-template-columns: auto minmax(0, 1fr) auto; gap: 12px; align-items: center; }
.pbtn { width: 52px; height: 52px; border-radius: 50%; border: 1px solid var(--accent); background: var(--accent); color: var(--accent-fg); font-size: 1.1rem; cursor: pointer; display: grid; place-items: center; }
.seek { height: 8px; border-radius: 4px; background: var(--line); overflow: hidden; cursor: pointer; }
.seek i { display: block; height: 100%; width: 0; background: var(--accent); }
.time { font: 400 0.78rem var(--mono); color: var(--muted); min-width: 5.5ch; text-align: right; }
.choice { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
.big { font: 700 1.15rem var(--font); min-height: 64px; border-radius: 12px; border: 2px solid var(--line); background: var(--panel2); cursor: pointer; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 2px; }
.big small { font: 400 0.7rem var(--mono); color: var(--muted); }
.big.k { border-color: var(--keep); color: var(--keep); } .big.s { border-color: var(--skip); color: var(--skip); }
.big.k:hover, .big.k.on { background: var(--keep); color: var(--bg); } .big.s:hover, .big.s.on { background: var(--skip); color: var(--bg); }
.big.on small, .big:hover small { color: inherit; }
.nav { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
.act, .chip { font: 500 0.8rem var(--font); padding: 8px 12px; min-height: 36px; border-radius: 8px; border: 1px solid var(--line); background: var(--panel); cursor: pointer; }
.chip { border-radius: 999px; }
.act:hover { border-color: var(--accent); } .act:disabled { opacity: 0.45; cursor: default; }
.chip[aria-pressed="true"] { background: var(--accent); color: var(--accent-fg); border-color: var(--accent); }
.spacer { flex: 1 1 auto; }
.filters { display: flex; flex-direction: column; gap: 8px; }
.row { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; }
.row .lab { font: 500 0.72rem var(--mono); letter-spacing: 0.08em; text-transform: uppercase; color: var(--muted); min-width: 5.5em; }
.goal { display: flex; flex-wrap: wrap; gap: 8px 12px; align-items: center; background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 8px 12px; font-size: 0.88rem; }
.goal input { width: 5.5em; font: 500 0.9rem var(--mono); padding: 5px 8px; border-radius: 6px; border: 1px solid var(--line); background: var(--bg); color: var(--fg); }
.list { background: var(--panel); border: 1px solid var(--line); border-radius: 12px; overflow: hidden; }
.item { display: grid; grid-template-columns: 1.1em minmax(0, 1fr) auto; gap: 10px; align-items: center; width: 100%; text-align: left; padding: 9px 14px; border: 0; border-top: 1px solid var(--line); background: transparent; cursor: pointer; font-size: 0.92rem; }
.item:first-child { border-top: 0; }
.item:hover { background: var(--panel2); }
.item.cur { background: var(--panel2); box-shadow: inset 3px 0 0 var(--accent); }
.dot { width: 10px; height: 10px; border-radius: 50%; border: 1.5px solid var(--muted); }
.item.keep .dot { background: var(--keep); border-color: var(--keep); } .item.skip .dot { background: var(--skip); border-color: var(--skip); opacity: 0.7; }
.item .nm { overflow-wrap: anywhere; } .item.skip .nm { color: var(--muted); text-decoration: line-through; text-decoration-color: var(--line); }
.item .tag { font: 400 0.72rem var(--mono); color: var(--muted); white-space: nowrap; }
#out { width: 100%; height: 150px; font: 400 0.75rem var(--mono); background: var(--panel); color: var(--fg); border: 1px solid var(--line); border-radius: 8px; padding: 8px; }
.note { color: var(--muted); font-size: 0.85rem; }
@media (max-width: 560px) { .sname { font-size: 1.35rem; } .item { grid-template-columns: 1.1em minmax(0, 1fr); } .item .tag { grid-column: 2; } .row .lab { min-width: 100%; } }
</style>
<main>
  <header>
    <div class="top"><h1>Golden Presets</h1><div class="tally" id="tally" aria-live="polite"></div></div>
    <p class="lede" id="lede"></p>
  </header>
  <div class="prog" id="prog" aria-hidden="true"><i class="pk"></i><i class="ps"></i></div>
  <details class="how"><summary>How this works</summary>
    <ul>
      <li>Every preset in the library, the ten factory ones too, rendered with the firmware's own code and played with its effects, the way BROWSE plays it. One phrase for its category. Nothing here has run on the board yet.</li>
      <li><b>Keep</b> or <b>Skip</b> each one. It plays when you land on it, and the next one that you have not heard comes up. Your choices are saved at once (in the page's database, so they stay if you close it).</li>
      <li>The idea is two or more rounds: first Keep anything you would want, then switch the filter to <b>Kept</b> and go through those again, only keeping the ones that earn a place. Keep going until the count is the size of library you want. The goal box shows how far you are.</li>
      <li>Keys: <kbd>K</kbd> or <kbd>&rarr;</kbd> keep, <kbd>S</kbd> or <kbd>&larr;</kbd> skip, <kbd>Space</kbd> play and stop, <kbd>R</kbd> from the start, <kbd>&uarr;</kbd><kbd>&darr;</kbd> previous and next, <kbd>Z</kbd> undo.</li>
      <li>The badge "earlier" is your rating from the earlier listening pages. The list is grouped by category so the similar sounds are next to each other.</li>
    </ul>
  </details>
  <section class="stage" id="stage" aria-label="Current preset"></section>
  <div class="filters">
    <div class="row"><span class="lab">Show</span><span id="fstatus"></span></div>
    <div class="row"><span class="lab">Category</span><span id="fcat"></span></div>
    <div class="row"><span class="lab">Set</span><span id="fset"></span></div>
  </div>
  <div class="goal"><label for="goal">Goal</label><input id="goal" type="number" min="1" max="999" value="100"> <span>presets to keep</span><span class="spacer"></span><span id="goalnote" class="mono"></span></div>
  <div class="list" id="list" role="list"></div>
  <section>
    <h2 style="font-size:1.05rem;margin:8px 0">Export</h2>
    <p class="note" id="exnote">Claude reads the choices straight from the page's database. For a copy, press Show export and copy the text.</p>
    <div class="nav"><button class="act" id="show">Show export</button><button class="act" id="copy">Copy</button><span class="note" id="status" role="status" aria-live="polite"></span></div>
    <textarea id="out" readonly hidden aria-label="Choices as JSON"></textarea>
  </section>
</main>
<script>
const ITEMS = /*ITEMS*/;
const CATS = /*CATS*/;
const SETS = /*SETS*/;
const ORDER = ITEMS.slice().sort((a, b) => (CATS.indexOf(a.c) - CATS.indexOf(b.c)) || (a.id - b.id));
const BY = {}; ORDER.forEach((m, k) => { m.k = k; BY[m.id] = m; });
const votes = {};                         // preset id -> "keep" | "skip"
const remoteSeen = {};
let dbVotes = null, synced = false;
let fStatus = "unheard", fCat = "ALL", fSet = "ALL", cur = ORDER[0].id, goal = 100;
const undoStack = [];
const audio = new Audio(); audio.preload = "auto";
const cache = new Map();                  // id -> object URL (kept: the last 40)
const $ = (s) => document.querySelector(s);
const esc = (t) => String(t).replace(/[&<>"]/g, (c) => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"}[c]));
const pad3 = (n) => String(n).padStart(3, "0");
const lsKey = "dboy-golden-votes", lsGoal = "dboy-golden-goal";
try { const o = JSON.parse(localStorage.getItem(lsKey) || "{}"); Object.keys(o).forEach((id) => { if (BY[id] && (o[id] === "keep" || o[id] === "skip")) votes[id] = o[id]; }); } catch (e) {}
try { const g = +localStorage.getItem(lsGoal); if (g >= 1 && g <= 999) goal = g; } catch (e) {}
$("#goal").value = goal;
function saveLocal() { try { localStorage.setItem(lsKey, JSON.stringify(votes)); } catch (e) {} }
function status(t) { $("#status").textContent = t; }

function view() {
  return ORDER.filter((m) => (fCat === "ALL" || m.c === fCat) && (fSet === "ALL" || m.s === fSet) &&
    (fStatus === "all" || (fStatus === "unheard" ? !votes[m.id] : votes[m.id] === fStatus)));
}
function counts() {
  let k = 0, s = 0;
  ORDER.forEach((m) => { if (votes[m.id] === "keep") k++; else if (votes[m.id] === "skip") s++; });
  return {k, s, u: ORDER.length - k - s};
}

let queue = Promise.resolve();
function persist(m, v) {
  queue = queue.then(async () => {
    if (!dbVotes) return;
    try {
      const ref = dbVotes.doc("p" + m.id);
      if (v) await ref.set({id: m.id, name: m.n, cat: m.c, set: m.s, vote: v, at: new Date().toISOString()});
      else await ref.delete();
      status("saved");
    } catch (e) { status("not saved to the database (" + (e && e.code || "error") + "); kept in this browser and tried again next visit"); }
  });
}
function setVote(m, v, record) {
  if (record !== false) { undoStack.push([m.id, votes[m.id] || ""]); if (undoStack.length > 100) undoStack.shift(); }
  if (v) votes[m.id] = v; else delete votes[m.id];
  saveLocal(); persist(m, v);
}
function vote(v) {
  const m = BY[cur];
  setVote(m, v);
  // move on: the next one after this in the list order that the filter still shows (the ones not heard yet, when the filter is "all"); at the end of the list, stay
  const after = view();
  const next = after.find((x) => x.k > m.k && (fStatus !== "all" || !votes[x.id])) || after.find((x) => x.k > m.k) || after.find((x) => !votes[x.id]) ||
    (after.some((x) => x.id === m.id) ? m : after[after.length - 1]);
  if (next) select(next.id, next.id !== m.id); else render();
}
function undo() {
  const r = undoStack.pop();
  if (!r) return;
  setVote(BY[r[0]], r[1], false);
  select(r[0], true);
}
function step(d) {
  const v = view(); if (!v.length) return;
  const at = v.findIndex((x) => x.id === cur);
  let to = at < 0 ? (d > 0 ? v.findIndex((x) => x.k > BY[cur].k) : v.length - 1) : at + d;
  if (to < 0) to = 0; if (to >= v.length) to = v.length - 1;
  select(v[to].id, true);
}

// audio: fetched from the page's own folder as needed, a few ahead
async function urlFor(id) {
  if (cache.has(id)) { const u = cache.get(id); cache.delete(id); cache.set(id, u); return u; }
  const r = await fetch("audio/" + pad3(id) + ".mp3");
  if (!r.ok) throw new Error("http " + r.status);
  const u = URL.createObjectURL(await r.blob());
  cache.set(id, u);
  while (cache.size > 40) { const old = cache.keys().next().value; if (old !== cur) URL.revokeObjectURL(cache.get(old)); cache.delete(old); }
  return u;
}
function prefetch() {
  const v = view(), at = v.findIndex((x) => x.id === cur);
  v.slice(Math.max(at, 0) + 1, Math.max(at, 0) + 4).forEach((x) => { urlFor(x.id).catch(() => {}); });
}
let loadToken = 0;
async function load(id, andPlay) {
  const tok = ++loadToken;
  audio.pause();
  try {
    let u; try { u = await urlFor(id); } catch (e) { u = "audio/" + pad3(id) + ".mp3"; }
    if (tok !== loadToken) return;
    audio.src = u; audio.currentTime = 0;
    if (andPlay) await audio.play();
  } catch (e) { if (tok === loadToken) status(andPlay ? "press Play to hear it" : "that sound could not be played"); }
  paintPlayer();
  prefetch();
}
function select(id, andPlay) {
  cur = id;
  render();
  load(id, andPlay);
  const el = document.querySelector('.item[data-id="' + id + '"]');
  if (el && el.scrollIntoView) el.scrollIntoView({block: "nearest"});
}
function toggle() { if (audio.paused) { if (!audio.src) load(cur, true); else audio.play().catch(() => {}); } else audio.pause(); }
function restart() { audio.currentTime = 0; audio.play().catch(() => load(cur, true)); }
audio.addEventListener("play", paintPlayer); audio.addEventListener("pause", paintPlayer); audio.addEventListener("ended", paintPlayer);
audio.addEventListener("timeupdate", paintPlayer);
function fmt(t) { t = Math.max(0, t || 0); return Math.floor(t / 60) + ":" + String(Math.floor(t % 60)).padStart(2, "0"); }
function paintPlayer() {
  const b = $("#pbtn"); if (!b) return;
  b.innerHTML = audio.paused ? "&#9654;" : "&#10074;&#10074;"; b.setAttribute("aria-label", audio.paused ? "Play" : "Pause");
  const d = audio.duration || BY[cur].d;
  $("#seekbar").style.width = (d ? Math.min(100, audio.currentTime / d * 100) : 0) + "%";
  $("#time").textContent = fmt(audio.currentTime) + " / " + fmt(d);
}

function renderStage() {
  const m = BY[cur], v = votes[m.id] || "", pos = view().findIndex((x) => x.id === cur);
  const stage = $("#stage"); stage.className = "stage " + v;
  stage.innerHTML =
    '<div class="smeta"><span class="badge cat">' + esc(m.c) + '</span><span class="badge">#' + m.id + '</span><span class="badge">' + esc(m.s) + '</span>' +
    '<span class="badge">' + m.v + ' voice' + (m.v > 1 ? "s" : "") + '</span>' + m.b.map((b) => '<span class="badge">wavetable ' + esc(b) + '</span>').join("") +
    (m.h ? '<span class="badge r' + m.h + '">earlier ' + m.h + '/5</span>' : "") + (v ? '<span class="badge st-' + v + '">' + (v === "keep" ? "KEPT" : "SKIPPED") + '</span>' : "") + '</div>' +
    '<div class="sname">' + esc(m.n) + '</div>' +
    '<div class="player"><button class="pbtn" id="pbtn"></button><div class="seek" id="seek"><i id="seekbar"></i></div><span class="time" id="time"></span></div>' +
    '<div class="choice"><button class="big s' + (v === "skip" ? " on" : "") + '" id="bskip">Skip<small>S &middot; &larr;</small></button><button class="big k' + (v === "keep" ? " on" : "") + '" id="bkeep">Keep<small>K &middot; &rarr;</small></button></div>' +
    '<div class="nav"><button class="act" id="bprev">&larr; Previous</button><button class="act" id="bnext">Next &rarr;</button><button class="act" id="bre">From the start</button><button class="act" id="bundo"' + (undoStack.length ? "" : " disabled") + '>Undo</button>' +
    '<span class="spacer"></span><span class="note">' + (pos >= 0 ? (pos + 1) + " of " + view().length + " in this list" : "not in this list") + '</span></div>';
  $("#pbtn").onclick = () => toggle();
  $("#seek").onclick = (e) => { const r = e.currentTarget.getBoundingClientRect(), d = audio.duration || m.d; audio.currentTime = d * (e.clientX - r.left) / r.width; };
  $("#bkeep").onclick = (e) => { e.currentTarget.blur(); vote("keep"); };
  $("#bskip").onclick = (e) => { e.currentTarget.blur(); vote("skip"); };
  $("#bprev").onclick = (e) => { e.currentTarget.blur(); step(-1); };
  $("#bnext").onclick = (e) => { e.currentTarget.blur(); step(1); };
  $("#bre").onclick = (e) => { e.currentTarget.blur(); restart(); };
  $("#bundo").onclick = (e) => { e.currentTarget.blur(); undo(); };
  paintPlayer();
}
function chips(el, opts, cur, fn) {
  el.innerHTML = opts.map(([key, label]) => '<button class="chip" data-v="' + esc(key) + '" aria-pressed="' + (key === cur) + '">' + esc(label) + '</button>').join(" ");
  el.querySelectorAll("button").forEach((b) => b.onclick = () => { b.blur(); fn(b.dataset.v); });
}
function render() {
  const c = counts();
  $("#tally").innerHTML = '<span class="k"><b>' + c.k + '</b> kept</span><span class="s"><b>' + c.s + '</b> skipped</span><span><b>' + c.u + '</b> not heard</span>';
  $("#prog").children[0].style.width = c.k / ORDER.length * 100 + "%"; $("#prog").children[1].style.width = c.s / ORDER.length * 100 + "%";
  $("#lede").textContent = ORDER.length + " presets in the library. Listen, then Keep or Skip. What is left is the golden set.";
  const setChanged = (fn) => (v) => { fn(v); const vw = view(); if (!vw.some((x) => x.id === cur) && vw.length) { select(vw[0].id, false); } else render(); };
  chips($("#fstatus"), [["unheard", "Not heard (" + c.u + ")"], ["keep", "Kept (" + c.k + ")"], ["skip", "Skipped (" + c.s + ")"], ["all", "All (" + ORDER.length + ")"]], fStatus, setChanged((v) => { fStatus = v; }));
  chips($("#fcat"), [["ALL", "All"], ...CATS.map((x) => [x, x + " (" + ORDER.filter((m) => m.c === x).length + ")"])], fCat, setChanged((v) => { fCat = v; }));
  chips($("#fset"), [["ALL", "All"], ...SETS.map((x) => [x, x])], fSet, setChanged((v) => { fSet = v; }));
  const diff = c.k - goal;
  $("#goalnote").textContent = c.k + " kept" + (c.k === 0 ? "" : diff > 0 ? ", " + diff + " over the goal" : diff < 0 ? ", " + (-diff) + " to go" : ", on the goal");
  renderStage();
  const vw = view();
  $("#list").innerHTML = vw.length ? vw.map((m) => '<button class="item ' + (votes[m.id] || "") + (m.id === cur ? " cur" : "") + '" role="listitem" data-id="' + m.id + '"><span class="dot"></span><span class="nm">' + esc(m.n) +
    '</span><span class="tag">' + esc(m.c) + ' &middot; #' + m.id + (m.h ? ' &middot; earlier ' + m.h : "") + '</span></button>').join("") : '<p class="note" style="padding:12px 14px;margin:0">Nothing in this list' + (fStatus === "unheard" ? " - you have heard them all." : ".") + '</p>';
  $("#list").querySelectorAll(".item").forEach((b) => b.onclick = () => { b.blur(); select(+b.dataset.id, true); });
}
$("#goal").onchange = (e) => { const g = Math.round(+e.target.value); if (g >= 1 && g <= 999) { goal = g; try { localStorage.setItem(lsGoal, String(g)); } catch (x) {} } e.target.value = goal; render(); };
document.addEventListener("keydown", (e) => {
  if (e.target && (e.target.tagName === "INPUT" || e.target.tagName === "TEXTAREA") || e.ctrlKey || e.metaKey || e.altKey) return;
  const k = e.key; let used = true;
  if (k === "k" || k === "K" || k === "ArrowRight") vote("keep");
  else if (k === "s" || k === "S" || k === "ArrowLeft") vote("skip");
  else if (k === " ") toggle();
  else if (k === "r" || k === "R") restart();
  else if (k === "ArrowDown") step(1);
  else if (k === "ArrowUp") step(-1);
  else if (k === "z" || k === "Z") undo();
  else used = false;
  if (used) e.preventDefault();
});
function exportText() {
  const rows = ORDER.filter((m) => votes[m.id]).map((m) => ({id: m.id, name: m.n, cat: m.c, set: m.s, vote: votes[m.id]})).sort((a, b) => a.id - b.id);
  const c = counts();
  return JSON.stringify({page: "dboy-golden", exported: new Date().toISOString(), keep: c.k, skip: c.s, notHeard: c.u, votes: rows}, null, 1);
}
$("#show").onclick = () => { const o = $("#out"); o.hidden = false; o.value = exportText(); o.select(); };
$("#copy").onclick = async () => {
  const o = $("#out"); o.hidden = false; o.value = exportText(); o.select();
  try { await navigator.clipboard.writeText(o.value); status("copied"); } catch (e) { status("press Ctrl/Cmd+C to copy the selected text"); }
};
// start on the first preset not heard yet (the saved choices of an earlier visit are in localStorage already)
{ const f = view()[0]; if (f) cur = f.id; }
render();
load(cur, false);
status("connecting to the database...");
(async () => {
  try {
    if (!window.claude || !claude.use) { status("choices are kept in this browser only (no database here)"); return; }
    const db = await claude.use("db");
    if (!db) { status("choices are kept in this browser only (no database here)"); return; }
    dbVotes = db.collection("votes");
    dbVotes.onSnapshot((snap) => {
      const seen = {};
      snap.docs.forEach((d) => { const x = d.data(); if (x && (x.vote === "keep" || x.vote === "skip") && BY[x.id]) seen[x.id] = x.vote; });
      let changed = false;
      Object.keys(seen).forEach((id) => { remoteSeen[id] = 1; if (votes[id] !== seen[id]) { votes[id] = seen[id]; changed = true; } });
      if (!snap.metadata.hasPendingWrites) {
        Object.keys(remoteSeen).forEach((id) => { if (!seen[id]) { delete remoteSeen[id]; if (votes[id]) { delete votes[id]; changed = true; } } });
      }
      if (!snap.metadata.fromCache && !synced) {          // the first definitive answer: send the choices only this browser has
        synced = true;
        Object.keys(votes).forEach((id) => { if (!seen[id]) persist(BY[id], votes[id]); });
        status("connected");
      }
      if (changed) { saveLocal(); render(); }
    }, () => { status("the database stopped answering; choices are kept in this browser"); });
  } catch (e) { status("choices are kept in this browser only"); }
})();
</script>
'''

if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
