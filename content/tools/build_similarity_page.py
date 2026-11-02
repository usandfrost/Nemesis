#!/usr/bin/env python3
"""build_similarity_page.py - the voting page for the similarity check.

    build_similarity_page.py <sim.json> <mp3 dir> <out.html>

sim.json comes from similarity.py; the mp3 dir holds fx_<id>.mp3 for the presets (content/tools/run_batch.sh makes them in its scratch folder, next to the renders).
Every preset that landed in a cluster gets its audio embedded (with effects, as heard on the card). The page keeps the votes in the artifact's `db` (collection
`votes`, one document per preset: {id, name, vote: keep|cut}) so Claude can read them, and shows an export of the same JSON as a fallback."""
import base64, json, os, sys

TEMPLATE = r'''<title>Library Similarity Check</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;500;600&display=swap">
<style>
:root {
  --bg: #f4f1ea; --panel: #fffdf8; --panel2: #ece8dd; --line: #d8d2c3; --fg: #23211c; --muted: #6d675a;
  --accent: #b35c00; --accent-fg: #fffdf8; --keep: #1f7a4d; --keep-bg: #dcefe3; --cut: #b3261e; --cut-bg: #f6dcd9; --warn: #8a6a00;
  --font: "IBM Plex Sans", system-ui, -apple-system, "Segoe UI", sans-serif; --mono: "IBM Plex Mono", ui-monospace, Menlo, monospace;
}
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --bg: #17150f; --panel: #211e16; --panel2: #2b271d; --line: #3a3528; --fg: #ece7d8; --muted: #a39b86;
  --accent: #f0a050; --accent-fg: #17150f; --keep: #6fd39b; --keep-bg: #1d3a2b; --cut: #ff8d84; --cut-bg: #44201d; --warn: #e3c35a; color-scheme: dark; } }
:root[data-theme="dark"] {
  --bg: #17150f; --panel: #211e16; --panel2: #2b271d; --line: #3a3528; --fg: #ece7d8; --muted: #a39b86;
  --accent: #f0a050; --accent-fg: #17150f; --keep: #6fd39b; --keep-bg: #1d3a2b; --cut: #ff8d84; --cut-bg: #44201d; --warn: #e3c35a; color-scheme: dark; }
* { box-sizing: border-box; }
html { scroll-padding-bottom: 90px; }
body { background: var(--bg); color: var(--fg); font-family: var(--font); line-height: 1.45; padding-inline: max(16px, env(safe-area-inset-left, 0px)) max(16px, env(safe-area-inset-right, 0px)); padding-block: 20px 120px; }
main { max-width: 980px; margin: 0 auto; display: flex; flex-direction: column; gap: 18px; }
h1 { font-size: 1.6rem; margin: 0; letter-spacing: -0.01em; text-wrap: balance; }
.lede { color: var(--muted); max-width: 68ch; margin: 4px 0 0; }
.mono { font-family: var(--mono); }
details.how { background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 10px 14px; }
details.how summary { cursor: pointer; font-weight: 500; }
details.how p { margin: 8px 0 0; color: var(--muted); max-width: 72ch; }
.bar { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
.chip { font: 500 0.8rem var(--font); padding: 8px 12px; min-height: 36px; border-radius: 999px; border: 1px solid var(--line); background: var(--panel); color: var(--fg); cursor: pointer; }
.chip:disabled { opacity: 0.45; cursor: default; }
.chip[aria-pressed="true"] { background: var(--accent); color: var(--accent-fg); border-color: var(--accent); }
.chip:focus-visible, button:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
.cluster { background: var(--panel); border: 1px solid var(--line); border-radius: 12px; overflow: hidden; }
.chead { display: flex; flex-wrap: wrap; gap: 8px 14px; align-items: center; padding: 10px 14px; background: var(--panel2); border-bottom: 1px solid var(--line); }
.chead .cat { font: 500 0.75rem var(--mono); letter-spacing: 0.08em; text-transform: uppercase; color: var(--accent); }
.chead .tight { font-size: 0.8rem; color: var(--muted); }
.chead .spacer { flex: 1 1 auto; }
.act { font: 500 0.78rem var(--font); padding: 8px 12px; min-height: 36px; border-radius: 7px; border: 1px solid var(--line); background: var(--panel); color: var(--fg); cursor: pointer; }
.act:hover { border-color: var(--accent); }
.member { display: grid; grid-template-columns: auto minmax(0, 1fr) auto; gap: 6px 12px; align-items: center; padding: 10px 14px; border-top: 1px solid var(--line); }
.chead + .member { border-top: 0; }
.member.keep { background: var(--keep-bg); } .member.cut { background: var(--cut-bg); }
.play { width: 38px; height: 38px; border-radius: 50%; border: 1px solid var(--line); background: var(--panel); color: var(--fg); cursor: pointer; font-size: 0.9rem; display: grid; place-items: center; }
.play[aria-pressed="true"] { background: var(--accent); color: var(--accent-fg); border-color: var(--accent); }
.mname { font-weight: 600; } .mname small { font: 400 0.75rem var(--mono); color: var(--muted); margin-left: 6px; }
.facts { grid-column: 2 / 3; font: 400 0.76rem var(--mono); color: var(--muted); display: flex; flex-wrap: wrap; gap: 2px 12px; }
.badges { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; margin-top: 2px; }
.badge { font: 500 0.7rem var(--mono); padding: 1px 7px; border-radius: 6px; border: 1px solid var(--line); color: var(--muted); }
.badge.r5, .badge.r4 { color: var(--keep); border-color: var(--keep); }
.dist { display: inline-block; width: 70px; height: 6px; background: var(--line); border-radius: 3px; vertical-align: middle; overflow: hidden; }
.dist i { display: block; height: 100%; background: var(--accent); }
.votes { display: flex; gap: 6px; }
.v { font: 600 0.8rem var(--font); min-width: 64px; padding: 8px 12px; border-radius: 8px; border: 1px solid var(--line); background: var(--panel); color: var(--fg); cursor: pointer; }
.v.k[aria-pressed="true"] { background: var(--keep); color: var(--bg); border-color: var(--keep); }
.v.c[aria-pressed="true"] { background: var(--cut); color: var(--bg); border-color: var(--cut); }
.foot { position: fixed; left: 0; right: 0; bottom: 0; background: var(--panel); border-top: 1px solid var(--line); padding: 10px max(16px, env(safe-area-inset-right, 0px)) calc(10px + env(safe-area-inset-bottom, 0px)) max(16px, env(safe-area-inset-left, 0px)); z-index: 5; }
.foot .in { max-width: 980px; margin: 0 auto; display: flex; flex-wrap: wrap; gap: 8px 16px; align-items: center; }
.count b { font-family: var(--mono); }
.count .k b { color: var(--keep); } .count .c b { color: var(--cut); }
.spacer { flex: 1 1 auto; }
.primary { font: 600 0.85rem var(--font); padding: 9px 14px; border-radius: 8px; border: 0; background: var(--accent); color: var(--accent-fg); cursor: pointer; }
#out { width: 100%; height: 160px; font: 400 0.75rem var(--mono); background: var(--panel); color: var(--fg); border: 1px solid var(--line); border-radius: 8px; padding: 8px; }
.note { color: var(--muted); font-size: 0.85rem; }
@media (max-width: 560px) { .member { grid-template-columns: auto minmax(0, 1fr); } .votes { grid-column: 1 / -1; } .v { flex: 1; } .facts { grid-column: 1 / -1; } }
</style>
<main>
  <header>
    <h1>Library Similarity Check</h1>
    <p class="lede" id="lede"></p>
  </header>
  <details class="how"><summary>How the sounds were compared</summary>
    <p>Every preset was rendered with the listening phrase for its category and compared only with others of the same category. Two things are measured: what it sounds like (from the dry render: the spectrum in 32 bands with the level removed, how noisy it is, the loudness over time and the brightness over time, 70%; the audio on the cards has the effects on) and how it is built (oscillators, filter, envelope, LFO, sends, 30%). Sounds that are close to each other are grouped; the bar next to a name shows its distance from the group's most typical member (longer is further away). A group is a suggestion, not a verdict: open each one and listen.</p>
    <p>Voting <b>Keep</b> or <b>Cut</b> saves at once (the votes are kept in the page's database, so they stay if you close it). Presets you leave undecided are kept. Undo last takes back your latest vote or group action. The ratings shown are your earlier ones.</p>
  </details>
  <div class="bar" id="filters" role="group" aria-label="Filters"></div>
  <div id="list"></div>
  <section>
    <h2 style="font-size:1.05rem;margin:8px 0">Export</h2>
    <p class="note">Claude reads the votes straight from the page's database. If you want a copy, press Show export below and copy the text.</p>
    <div class="bar"><button class="act" id="show">Show export</button><button class="act" id="copy">Copy</button></div>
    <textarea id="out" readonly hidden aria-label="Votes as JSON"></textarea>
  </section>
</main>
<div class="foot"><div class="in">
  <span class="count" id="count"></span><span class="spacer"></span>
  <span class="note" id="status" role="status" aria-live="polite"></span>
</div></div>
<script>
const CL = /*CLUSTERS*/;
const AUDIO = /*AUDIO*/;
const META = /*META*/;
const ALL = {};                          // preset id -> member, for every preset on this page
CL.forEach((c) => c.members.forEach((m) => { ALL[m.id] = m; }));
const votes = {};                        // preset id -> "keep" | "cut"
const remoteSeen = {};                   // ids the database has told us about
let dbVotes = null, synced = false, playing = null, catFilter = "ALL", undecidedOnly = false;
let batch = null;
const undoStack = [];
const audio = new Audio();
const $ = (s) => document.querySelector(s);
const esc = (t) => String(t).replace(/[&<>"]/g, (c) => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"}[c]));
const lsKey = "dboy-similarity-votes";
try { const o = JSON.parse(localStorage.getItem(lsKey) || "{}"); Object.keys(o).forEach((id) => { if (ALL[id] && (o[id] === "keep" || o[id] === "cut")) votes[id] = o[id]; }); } catch (e) {}
function saveLocal() { try { localStorage.setItem(lsKey, JSON.stringify(votes)); } catch (e) {} }
function status(t) { $("#status").textContent = t; }

$("#lede").textContent = META.presets + " presets compared, " + CL.length + " groups of similar sounds (" + META.inClusters + " presets). The other " + META.alone + " have no close neighbour and are not shown.";

let queue = Promise.resolve();
function persist(m, v) {
  queue = queue.then(async () => {
    if (!dbVotes) return;
    try {
      const ref = dbVotes.doc("p" + m.id);
      if (v) await ref.set({id: m.id, name: m.name, cat: m.cat, set: m.set, vote: v, at: new Date().toISOString()});
      else await ref.delete();
      status("saved");
    } catch (e) { status("not saved to the database (" + (e && e.code || "error") + "); it is kept in this browser and tried again at the next visit"); }
  });
}
function setVote(m, v, record) {
  if (record !== false && batch) batch.push([m, votes[m.id] || ""]);
  if (v) votes[m.id] = v; else delete votes[m.id];
  saveLocal(); persist(m, v);
}
function act(fn) {
  batch = [];
  fn();
  if (batch.length) { undoStack.push(batch); if (undoStack.length > 50) undoStack.shift(); }
  batch = null;
  render();
}
function undo() {
  const rec = undoStack.pop();
  if (!rec) return;
  rec.slice().reverse().forEach(([m, prev]) => setVote(m, prev, false));
  render();
}
function best(c) {
  return c.members.slice().sort((a, b) => ((b.rating || 0) - (a.rating || 0)) || (a.dist - b.dist))[0];
}
function render() {
  const keep = document.activeElement && document.activeElement.dataset ? document.activeElement.dataset.key : null;
  const cats = ["ALL", ...Array.from(new Set(CL.map((c) => c.cat)))];
  $("#filters").innerHTML = cats.map((c) => '<button class="chip" data-key="cat' + c + '" aria-pressed="' + (c === catFilter) + '" data-cat="' + c + '">' + c + '</button>').join("") +
    '<button class="chip" data-key="und" aria-pressed="' + undecidedOnly + '" id="und">Undecided only</button>' +
    '<button class="chip" data-key="undo" id="undo"' + (undoStack.length ? "" : " disabled") + '>Undo last</button>';
  $("#filters").querySelectorAll("[data-cat]").forEach((b) => b.onclick = () => { catFilter = b.dataset.cat; render(); });
  $("#und").onclick = () => { undecidedOnly = !undecidedOnly; render(); };
  $("#undo").onclick = undo;
  const list = $("#list");
  list.innerHTML = "";
  const wrap = document.createElement("div"); wrap.style.cssText = "display:flex;flex-direction:column;gap:14px";
  CL.forEach((c, ci) => {
    if (catFilter !== "ALL" && c.cat !== catFilter) return;
    if (undecidedOnly && c.members.every((m) => votes[m.id])) return;
    const el = document.createElement("section"); el.className = "cluster";
    const label = c.spread < 0.18 ? "very close" : c.spread < 0.22 ? "close" : "similar";
    el.innerHTML = '<div class="chead"><span class="cat">' + c.cat + '</span><span class="tight">' + c.size + " presets, " + label + '</span><span class="spacer"></span>' +
      '<button class="act" data-key="a' + ci + 'b" data-a="best">Keep best, cut the rest</button><button class="act" data-key="a' + ci + 'a" data-a="all">Keep all</button><button class="act" data-key="a' + ci + 'c" data-a="clear">Clear</button></div>';
    c.members.forEach((m) => {
      const v = votes[m.id] || "";
      const f = m.facts;
      const row = document.createElement("div"); row.className = "member " + v;
      row.innerHTML = '<button class="play" data-key="p' + m.id + '" aria-label="' + (playing === m.id ? "Stop " : "Play ") + esc(m.name) + '" aria-pressed="' + (playing === m.id) + '">' + (playing === m.id ? "&#9632;" : "&#9654;") + '</button>' +
        '<div><div class="mname">' + esc(m.name) + '<small>#' + m.id + ' &middot; ' + esc(m.set) + '</small></div>' +
        '<div class="badges">' + (m.rating ? '<span class="badge r' + m.rating + '">rated ' + m.rating + '</span>' : '<span class="badge">not rated</span>') +
        '<span class="badge"><span class="dist"><i style="width:' + Math.min(100, Math.round(m.dist / 0.5 * 100)) + '%"></i></span> ' + (m.dist === 0 ? "typical" : "distance " + m.dist.toFixed(2)) + '</span></div></div>' +
        '<div class="votes"><button class="v k" data-key="k' + m.id + '" aria-label="Keep ' + esc(m.name) + '" aria-pressed="' + (v === "keep") + '">Keep</button><button class="v c" data-key="c' + m.id + '" aria-label="Cut ' + esc(m.name) + '" aria-pressed="' + (v === "cut") + '">Cut</button></div>' +
        '<div class="facts"><span>' + esc(f.osc || "-") + '</span><span>' + esc(f.flt) + '</span><span>' + esc(f.env) + '</span><span>' + f.voices + ' voice' + (f.voices > 1 ? "s" : "") +
        '</span>' + (f.fx ? '<span>' + esc(f.fx) + '</span>' : "") + (f.lfo ? '<span>LFO ' + esc(f.lfo) + '</span>' : "") + '</div>';
      row.querySelector(".play").onclick = () => play(m);
      row.querySelector(".k").onclick = () => act(() => setVote(m, v === "keep" ? "" : "keep"));
      row.querySelector(".c").onclick = () => act(() => setVote(m, v === "cut" ? "" : "cut"));
      el.appendChild(row);
    });
    el.querySelector('[data-a="best"]').onclick = () => act(() => { const b = best(c); c.members.forEach((m) => setVote(m, m === b ? "keep" : "cut")); });
    el.querySelector('[data-a="all"]').onclick = () => act(() => c.members.forEach((m) => setVote(m, "keep")));
    el.querySelector('[data-a="clear"]').onclick = () => act(() => c.members.forEach((m) => setVote(m, "")));
    wrap.appendChild(el);
  });
  if (!wrap.children.length) wrap.innerHTML = '<p class="note">Nothing to show with these filters.</p>';
  list.appendChild(wrap);
  counts();
  if (keep) { const el = document.querySelector('[data-key="' + keep + '"]'); if (el && !el.disabled) el.focus(); }
}
function counts() {
  const ids = Object.keys(votes).filter((id) => ALL[id]);
  const k = ids.filter((id) => votes[id] === "keep").length, c = ids.filter((id) => votes[id] === "cut").length;
  $("#count").innerHTML = '<span class="k"><b>' + k + "</b> keep</span> &nbsp; <span class=\"c\"><b>" + c + "</b> cut</span> &nbsp; <b>" + (META.inClusters - k - c) + "</b> undecided";
}
function play(m) {
  if (playing === m.id) { audio.pause(); playing = null; render(); return; }
  const id = m.id;
  audio.src = AUDIO[id]; playing = id;
  audio.play().catch(() => { if (playing === id) { playing = null; render(); } });
  render();
}
audio.onended = () => { playing = null; render(); };
audio.onerror = () => { playing = null; status("that sound could not be played"); render(); };
function exportText() {
  const rows = Object.keys(votes).filter((id) => ALL[id]).map((id) => ({id: +id, name: ALL[id].name, cat: ALL[id].cat, set: ALL[id].set, vote: votes[id]})).sort((a, b) => a.id - b.id);
  return JSON.stringify({page: "dboy-similarity", exported: new Date().toISOString(), keep: rows.filter((r) => r.vote === "keep").length, cut: rows.filter((r) => r.vote === "cut").length, votes: rows}, null, 1);
}
$("#show").onclick = () => { const o = $("#out"); o.hidden = false; o.value = exportText(); o.select(); };
$("#copy").onclick = async () => {
  const o = $("#out"); o.hidden = false; o.value = exportText(); o.select();
  try { await navigator.clipboard.writeText(o.value); status("copied"); } catch (e) { status("press Ctrl/Cmd+C to copy the selected text"); }
};
render();
status("connecting to the database...");
// the shared database: merge the saved votes with this visit's, then keep them in step
(async () => {
  try {
    if (!window.claude || !claude.use) { status("votes are kept in this browser only (no database here)"); return; }
    const db = await claude.use("db");
    if (!db) { status("votes are kept in this browser only (no database here)"); return; }
    dbVotes = db.collection("votes");
    dbVotes.onSnapshot((snap) => {
      const seen = {};
      snap.docs.forEach((d) => { const x = d.data(); if (x && x.vote && ALL[x.id]) seen[x.id] = x.vote; });
      let changed = false;
      Object.keys(seen).forEach((id) => { remoteSeen[id] = 1; if (votes[id] !== seen[id]) { votes[id] = seen[id]; changed = true; } });
      if (!snap.metadata.hasPendingWrites) {
        Object.keys(remoteSeen).forEach((id) => { if (!seen[id]) { delete remoteSeen[id]; if (votes[id]) { delete votes[id]; changed = true; } } });
      }
      if (!snap.metadata.fromCache && !synced) {          // the first definitive answer: send the votes only this browser has
        synced = true;
        Object.keys(votes).forEach((id) => { if (!seen[id]) persist(ALL[id], votes[id]); });
        status("connected");
      }
      if (changed) { saveLocal(); render(); }
    }, () => { status("the database stopped answering; votes are kept in this browser"); });
  } catch (e) { status("votes are kept in this browser only"); }
})();
</script>
'''


def main(sim, mp3dir, out):
    d = json.load(open(sim))
    audio = {}
    for c in d["clusters"]:
        for m in c["members"]:
            p = os.path.join(mp3dir, "fx_%03d.mp3" % m["id"])
            if not os.path.isfile(p):
                sys.exit("build_similarity_page.py: no %s (run content/tools/run_batch.sh on the same presets first)" % p)
            audio[m["id"]] = "data:audio/mpeg;base64," + base64.b64encode(open(p, "rb").read()).decode()
    n = len(d["singles"]) + sum(c["size"] for c in d["clusters"])
    meta = dict(presets=n, inClusters=sum(c["size"] for c in d["clusters"]), alone=len(d["singles"]))
    html = (TEMPLATE.replace("/*CLUSTERS*/", json.dumps(d["clusters"], separators=(",", ":"))).replace("/*AUDIO*/", json.dumps(audio, separators=(",", ":")))
            .replace("/*META*/", json.dumps(meta)))
    open(out, "w").write(html)
    print("%s: %d groups, %d presets with audio, %.1f MB" % (out, len(d["clusters"]), len(audio), len(html) / 1e6))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
