#!/usr/bin/env python3
"""build_listen.py - encode the renders to small mono mp3 and write ONE self-contained listening page, content/out/listen.html.

Input (from build_content.py): <scratch>/wav/NNN.wav (Drumboy), <scratch>/hy/NNN.wav (original upstream engine, dry), their stats .jsonl,
content/data/presets.json, content/out/convert_report.json, content/out/selection.json.
mp3: mono, 32 kHz, 40 kbit/s CBR, 25 Hz high-pass (some patches carry DC), loudness already matched at -20 dBFS RMS by the renderer.
"""
import base64, concurrent.futures as cf, json, math, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
S = os.environ.get("DBOY_CONTENT_SCRATCH", "/tmp/dboy-content")
OUT = os.path.join(REPO, "content", "out")
KBPS = int(os.environ.get("DBOY_MP3_KBPS", "40"))


def enc(args):
    src, dst = args
    subprocess.check_call(["ffmpeg", "-loglevel", "error", "-y", "-i", src, "-af", "highpass=f=25", "-ac", "1", "-ar", "32000", "-c:a", "libmp3lame",
                           "-b:a", "%dk" % KBPS, dst])
    return dst


def uri(path):
    return "data:audio/mpeg;base64," + base64.b64encode(open(path, "rb").read()).decode()


def label(it):
    f = it["feat"]
    if f.startswith("mod:"):
        a, b = f[4:].split(">", 1)
        return (a + ">" + b).lower()
    if f.startswith("fx:"):
        return "fx " + f[3:]
    return f


def main():
    pres = json.load(open(os.path.join(REPO, "content", "data", "presets.json")))["presets"]
    rep = json.load(open(os.path.join(OUT, "convert_report.json")))
    st = {json.loads(l)["i"]: json.loads(l) for l in open(os.path.join(S, "stats.jsonl"))}
    hy = {}
    for l in open(os.path.join(S, "up_stats.jsonl")):
        r = json.loads(l)
        hy[r["i"]] = r
    mp = os.path.join(S, "mp3")
    os.makedirs(mp, exist_ok=True)
    jobs = []
    for i in range(len(pres)):
        jobs.append((os.path.join(S, "wav", "%03d.wav" % i), os.path.join(mp, "d%03d.mp3" % i)))
        if os.path.exists(os.path.join(S, "hy", "%03d.wav" % i)):
            jobs.append((os.path.join(S, "hy", "%03d.wav" % i), os.path.join(mp, "h%03d.mp3" % i)))
    with cf.ThreadPoolExecutor(8) as ex:
        list(ex.map(enc, jobs))
    items, audio, total = [], [], 0
    for i, p in enumerate(pres):
        r = rep[i]
        worst = sorted([x for x in r["items"] if x["loss"] >= 1.0], key=lambda x: -x["loss"])[:5]
        d = os.path.join(mp, "d%03d.mp3" % i)
        h = os.path.join(mp, "h%03d.mp3" % i)
        total += os.path.getsize(d) + (os.path.getsize(h) if os.path.exists(h) else 0)
        oct_ = None
        if i in hy and "centroid" in hy[i]:
            oct_ = round(math.log2(max(st[i]["centroid"], 20) / max(hy[i]["centroid"], 20)), 1)
        items.append(dict(n=p["name"], c=p["category"], s=int(round(p["score"])), cost=p["cost"], v=p["voices"], b=p["bank"], ph=st[i]["phrase"],
                          t=len(p["tables"]), w=[[label(x), x["status"][0]] for x in worst], oct=oct_, lvl=round(st[i]["rmsActiveDb"], 1),
                          hy=1 if os.path.exists(h) else 0))
        audio.append([uri(d), uri(h) if os.path.exists(h) else ""])
    cats = []
    for it in items:
        if it["c"] not in cats:
            cats.append(it["c"])
    order = ["BASS", "LEAD", "POLY", "PAD", "PLUCK", "STAB", "KEYS", "BRASS", "FX", "SEQ", "TEXTURE", "BELL"]
    cats = [c for c in order if c in cats] + [c for c in cats if c not in order]
    html = TEMPLATE.replace("/*ITEMS*/", json.dumps(items, separators=(",", ":"))).replace("/*CATS*/", json.dumps(cats)).replace("/*AUDIO*/", json.dumps(audio, separators=(",", ":")))
    html = html.replace("__KBPS__", str(KBPS)).replace("__N__", str(len(items)))
    path = os.path.join(OUT, "listen.html")
    open(path, "w").write(html)
    print("%d presets, mp3 total %.1f MB, listen.html %.1f MB" % (len(items), total / 1e6, os.path.getsize(path) / 1e6))


TEMPLATE = r"""<title>Drumboy Preset Listening</title>
<meta name="claude:viewport-fit-sides" content="cover">
<style>
/* Layout: a sticky bar (tabs, stop, export) over one list of rows; each row is name + fit + two play buttons + rating + note.
   The accents are the panel's own colours: yellow for the Drumboy render (the AMP page), cyan for the original (the GLOBAL page). */
:root {
  --bg: #101318; --panel: #181c23; --panel2: #1f242d; --line: #2a303b; --fg: #e8ebf1; --mute: #8f98a9;
  --drum: #ffd23d; --drum-ink: #1b1604; --orig: #19d3e6; --orig-ink: #04181b;
  --good: #5ff08a; --warn: #ffd23d; --bad: #ff5d5d; --focus: #ff3da6;
  --mono: ui-monospace, "SF Mono", Menlo, Consolas, monospace; --sans: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  color-scheme: dark;
}
@media (prefers-color-scheme: light) { :root:not([data-theme="dark"]) {
  --bg: #f3f4f7; --panel: #ffffff; --panel2: #eceef3; --line: #d5d9e1; --fg: #15181e; --mute: #5d6678;
  --drum: #b78600; --drum-ink: #ffffff; --orig: #0a8796; --orig-ink: #ffffff;
  --good: #1a8a46; --warn: #9a6f00; --bad: #c42b2b; --focus: #c4146f; color-scheme: light; } }
:root[data-theme="light"] {
  --bg: #f3f4f7; --panel: #ffffff; --panel2: #eceef3; --line: #d5d9e1; --fg: #15181e; --mute: #5d6678;
  --drum: #b78600; --drum-ink: #ffffff; --orig: #0a8796; --orig-ink: #ffffff;
  --good: #1a8a46; --warn: #9a6f00; --bad: #c42b2b; --focus: #c4146f; color-scheme: light; }
* { box-sizing: border-box; }
body { background: var(--bg); color: var(--fg); font: 15px/1.45 var(--sans); padding-inline: max(16px, env(safe-area-inset-left, 0px)) max(16px, env(safe-area-inset-right, 0px)); padding-block: 0 48px; }
body::before { content: ""; position: fixed; inset: 0 0 auto; height: env(safe-area-inset-top, 0px); z-index: 30; background: var(--bg); }
header.top { padding-block: 20px 8px; max-width: 980px; margin-inline: auto; }
h1 { font-size: 22px; line-height: 1.2; margin: 0 0 6px; letter-spacing: .01em; text-wrap: balance; }
.lede { color: var(--mute); margin: 0; max-width: 70ch; font-size: 14px; }
.lede b { color: var(--fg); font-weight: 600; }
.bar { position: sticky; top: env(safe-area-inset-top, 0px); z-index: 20; background: var(--bg); border-bottom: 1px solid var(--line); margin-inline: calc(-1 * max(16px, env(safe-area-inset-left, 0px))); padding: 10px max(16px, env(safe-area-inset-right, 0px)) 10px max(16px, env(safe-area-inset-left, 0px)); }
.bar-in { max-width: 980px; margin-inline: auto; display: flex; flex-wrap: wrap; gap: 8px 10px; align-items: center; }
.tabs { display: flex; flex-wrap: wrap; gap: 6px; min-width: 0; flex: 1 1 320px; }
button { font: inherit; color: var(--fg); background: var(--panel2); border: 1px solid var(--line); border-radius: 6px; padding: 6px 10px; cursor: pointer; min-height: 34px; }
button:hover { border-color: var(--mute); }
:focus-visible { outline: 2px solid var(--focus); outline-offset: 2px; }
.tab { font-family: var(--mono); font-size: 12px; letter-spacing: .06em; padding: 6px 9px; }
.tab[aria-selected="true"] { background: var(--fg); color: var(--bg); border-color: var(--fg); }
.tab small { opacity: .65; margin-left: 4px; }
.tools { display: flex; gap: 8px; flex-wrap: wrap; }
#stop { border-color: var(--bad); color: var(--bad); }
main { max-width: 980px; margin-inline: auto; padding-top: 10px; }
.sub { color: var(--mute); font-size: 13px; margin: 6px 0 10px; display: flex; flex-wrap: wrap; gap: 4px 16px; }
ol { list-style: none; margin: 0; padding: 0; display: grid; gap: 8px; }
li.row { background: var(--panel); border: 1px solid var(--line); border-radius: 8px; padding: 10px 12px; display: grid; gap: 8px 14px; grid-template-columns: minmax(0, 1fr) auto; }
li.row.sel { border-color: var(--focus); box-shadow: 0 0 0 1px var(--focus); }
.id { min-width: 0; }
.nm { font-weight: 600; font-size: 16px; overflow-wrap: anywhere; }
.meta { font-family: var(--mono); font-size: 12px; color: var(--mute); display: flex; flex-wrap: wrap; gap: 2px 12px; margin-top: 2px; }
.fit { display: inline-flex; align-items: center; gap: 6px; }
.fitbar { width: 54px; height: 6px; border-radius: 3px; background: var(--panel2); overflow: hidden; display: inline-block; }
.fitbar i { display: block; height: 100%; background: var(--good); }
.fitbar.mid i { background: var(--warn); } .fitbar.low i { background: var(--bad); }
.lost { grid-column: 1 / -1; display: flex; flex-wrap: wrap; gap: 4px; }
.chip { font-family: var(--mono); font-size: 11px; padding: 1px 7px; border-radius: 10px; border: 1px solid var(--line); color: var(--mute); }
.chip.d { border-color: color-mix(in srgb, var(--bad) 55%, var(--line)); }
.chip.a { border-color: color-mix(in srgb, var(--warn) 55%, var(--line)); }
.play { display: flex; gap: 8px; align-items: flex-start; }
.pb { font-family: var(--mono); font-size: 12px; letter-spacing: .05em; min-width: 104px; font-weight: 600; position: relative; overflow: hidden; }
.pb.drum { background: var(--drum); color: var(--drum-ink); border-color: var(--drum); }
.pb.orig { background: var(--orig); color: var(--orig-ink); border-color: var(--orig); }
.pb[disabled] { opacity: .35; cursor: default; }
.pb .prog { position: absolute; left: 0; bottom: 0; height: 3px; width: 0; background: currentColor; opacity: .55; }
.pb.on::after { content: ""; position: absolute; inset: 0; box-shadow: inset 0 0 0 2px var(--fg); border-radius: 5px; }
.rate { grid-column: 1 / -1; display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
.stars { display: flex; gap: 4px; }
.stars button { padding: 0; width: 34px; font-family: var(--mono); font-weight: 600; }
.stars button[aria-pressed="true"] { background: var(--fg); color: var(--bg); border-color: var(--fg); }
.note { flex: 1 1 200px; min-width: 0; font: inherit; color: var(--fg); background: var(--bg); border: 1px solid var(--line); border-radius: 6px; padding: 6px 9px; min-height: 34px; }
.keys { color: var(--mute); font-size: 12px; margin-top: 18px; }
kbd { font-family: var(--mono); border: 1px solid var(--line); border-bottom-width: 2px; border-radius: 4px; padding: 0 5px; background: var(--panel2); }
#out { width: 100%; height: 160px; margin-top: 10px; font-family: var(--mono); font-size: 12px; color: var(--fg); background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 8px; }
#toast { position: fixed; left: 50%; bottom: calc(18px + env(safe-area-inset-bottom, 0px)); transform: translateX(-50%); background: var(--fg); color: var(--bg); padding: 8px 14px; border-radius: 6px; font-size: 13px; z-index: 40; }
@media (max-width: 520px) { li.row { grid-template-columns: 1fr; } .play { width: 100%; } .pb { flex: 1; } }
@media (prefers-reduced-motion: no-preference) { .fitbar i { transition: width .2s; } }
</style>
<header class="top">
  <h1>Drumboy Preset Listening</h1>
  <p class="lede"><b>__N__ CATHODE presets</b> converted to the Drumboy's nine sound pages and played through the firmware's own CATHODE mapping (with the wavetable prototype). <b>DRUMBOY</b> is that render; <b>ORIGINAL</b> is the same preset on the upstream engine without its effects chain, same notes, both set to the same loudness. Fit is how much of the patch the panel can express (100 = nothing lost). Rate what you hear; ratings stay in this browser until you export them.</p>
</header>
<div class="bar"><div class="bar-in">
  <div class="tabs" role="tablist" id="tabs"></div>
  <div class="tools"><button id="stop" title="Stop all (S)">Stop all</button><button id="export" title="Copy ratings as JSON">Export ratings</button></div>
</div></div>
<main>
  <div class="sub" id="sub"></div>
  <ol id="list"></ol>
  <textarea id="out" hidden readonly aria-label="Ratings as JSON"></textarea>
  <p class="keys"><kbd>↑</kbd> <kbd>↓</kbd> move &middot; <kbd>Space</kbd> play DRUMBOY &middot; <kbd>O</kbd> play ORIGINAL &middot; <kbd>1</kbd>-<kbd>5</kbd> rate &middot; <kbd>←</kbd> <kbd>→</kbd> category &middot; <kbd>S</kbd> stop. Audio: mono mp3, 32 kHz, __KBPS__ kbit/s, so highs are dulled; judge character, not top-end.</p>
</main>
<div id="toast" hidden></div>
<script id="audio" type="application/json">/*AUDIO*/</script>
<script>
"use strict";
const ITEMS = /*ITEMS*/;
const CATS = /*CATS*/;
const KEY = "dboy-listen-v1";
let AUDIO = null, store = {}, cat = "BASS", sel = 0, player = new Audio(), playing = null, raf = 0;
try { store = JSON.parse(localStorage.getItem(KEY) || "{}") || {}; } catch (e) { store = {}; }
function save() { try { localStorage.setItem(KEY, JSON.stringify(store)); } catch (e) { /* private window: ratings last until the tab closes */ } }
function sid(it) { return it.c + "/" + it.n; }
function rec(it) { return store[sid(it)] || { r: 0, note: "" }; }   // a read never creates an entry
function edit(it) { return store[sid(it)] || (store[sid(it)] = { r: 0, note: "" }); }
function toast(t) { const el = document.getElementById("toast"); el.textContent = t; el.hidden = false; clearTimeout(toast.t); toast.t = setTimeout(() => { el.hidden = true; }, 2200); }
const $ = (s, r) => (r || document).querySelector(s);
const rows = () => ITEMS.map((it, i) => ({ it, i })).filter(x => x.it.c === cat);

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
function fitClass(s) { return s >= 85 ? "" : s >= 70 ? "mid" : "low"; }
function render() {
  renderTabs();
  const list = $("#list"), rs = rows();
  list.textContent = "";
  const avg = rs.reduce((a, x) => a + x.it.s, 0) / (rs.length || 1);
  $("#sub").textContent = "";
  for (const t of [rs.length + " presets", "mean fit " + avg.toFixed(0), "phrase: " + (rs[0] ? rs[0].it.ph : "")]) { const s = document.createElement("span"); s.textContent = t; $("#sub").appendChild(s); }
  rs.forEach((x, k) => {
    const it = x.it, r = rec(it);
    const li = document.createElement("li");
    li.className = "row" + (k === sel ? " sel" : ""); li.dataset.k = k; li.dataset.i = x.i;
    const chips = it.w.map(w => '<span class="chip ' + (w[1] === "d" ? "d" : "a") + '" title="' + (w[1] === "d" ? "dropped" : "approximated") + '">' + w[0].replace(/&/g, "&amp;").replace(/</g, "&lt;") + "</span>").join("");
    li.innerHTML =
      '<div class="id"><div class="nm"></div><div class="meta"><span class="fit">fit ' + it.s + ' <span class="fitbar ' + fitClass(it.s) + '"><i style="width:' + it.s + '%"></i></span></span>' +
      "<span>" + it.v + (it.v === 1 ? " voice" : " voices") + " &middot; cost " + it.cost.toFixed(2) + "</span>" + (it.t ? "<span>" + it.t + " wavetable" + (it.t > 1 ? "s" : "") + "</span>" : "") +
      (it.oct !== null ? "<span title=\"spectral centroid, DRUMBOY versus ORIGINAL\">brightness " + (it.oct > 0 ? "+" : "") + it.oct + " oct</span>" : "") + "<span>" + it.b + "</span></div></div>" +
      '<div class="play"><button class="pb drum" data-w="0">&#9654; DRUMBOY<span class="prog"></span></button><button class="pb orig" data-w="1"' + (it.hy ? "" : " disabled") + '>&#9654; ORIGINAL<span class="prog"></span></button></div>' +
      '<div class="lost">' + (chips || '<span class="chip">nothing notable lost</span>') + "</div>" +
      '<div class="rate"><div class="stars" role="group" aria-label="Rating">' + [1, 2, 3, 4, 5].map(n => '<button data-r="' + n + '" aria-pressed="' + (r.r === n) + '" aria-label="' + n + ' of 5">' + n + "</button>").join("") +
      '</div><input class="note" type="text" maxlength="300" placeholder="Note" aria-label="Note for ' + it.n.replace(/"/g, "") + '"></div>';
    $(".nm", li).textContent = it.n;
    $(".note", li).value = r.note;
    li.addEventListener("click", e => {
      sel = k; markSel();
      const pb = e.target.closest(".pb"), st = e.target.closest("[data-r]");
      if (pb && !pb.disabled) play(x.i, +pb.dataset.w);
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
  renderTabsKeep();
}
function renderTabsKeep() { const y = window.scrollY; renderTabs(); window.scrollTo(0, y); }
function loadAudio() { if (!AUDIO) AUDIO = JSON.parse($("#audio").textContent); return AUDIO; }
function stop() { player.pause(); playing = null; cancelAnimationFrame(raf); paintPlaying(); }
function play(i, w) {
  if (playing && playing.i === i && playing.w === w) { stop(); return; }
  const src = loadAudio()[i][w];
  if (!src) return;
  player.pause(); player.src = src; player.currentTime = 0;
  playing = { i, w };
  player.play().then(tick, () => { toast("Playback was blocked; press a play button."); playing = null; paintPlaying(); });
  paintPlaying();
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
  const out = ITEMS.filter(it => { const r = store[sid(it)]; return r && (r.r || r.note); }).map(it => ({ name: it.n, category: it.c, fit: it.s, rating: rec(it).r, note: rec(it).note }));
  const txt = JSON.stringify({ page: "drumboy-listen-v1", exported: new Date().toISOString(), rated: out.length, of: ITEMS.length, ratings: out }, null, 1);
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
document.addEventListener("keydown", e => {
  if (e.target.matches("input, textarea") || e.metaKey || e.ctrlKey || e.altKey) { if (e.key === "Escape") e.target.blur(); return; }
  const k = e.key;
  if (k === "ArrowDown") { move(1); e.preventDefault(); }
  else if (k === "ArrowUp") { move(-1); e.preventDefault(); }
  else if (k === "ArrowRight" || k === "ArrowLeft") { const j = CATS.indexOf(cat) + (k === "ArrowRight" ? 1 : -1); if (j >= 0 && j < CATS.length) { cat = CATS[j]; sel = 0; render(); } e.preventDefault(); }
  else if (k === " " && e.target.tagName !== "BUTTON") { if (cur() >= 0) play(cur(), 0); e.preventDefault(); }
  else if ((k === "o" || k === "O") && cur() >= 0 && ITEMS[cur()].hy) play(cur(), 1);
  else if (k === "s" || k === "S") stop();
  else if (k >= "1" && k <= "5" && cur() >= 0) rate(cur(), +k);
});
$("#stop").onclick = stop;
$("#export").onclick = exportRatings;
render();
</script>
"""

if __name__ == "__main__":
    main()
