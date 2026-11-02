'use strict';
// The panel's pages: Online Nemesis, the tutorial, and the preset bench with the panel on a tab. The sound is made in engine-worker.js; this file is the UI, the note patterns and the audio start.
const CAP = 16384;                       // frames in the ring shared with the worklet
const $ = (s, r = document) => r.querySelector(s);
const BENCH = !!$("#list");              // the bench has the preset list; the other pages are the panel alone
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const REG = { BASS: 36, LEAD: 60, PAD: 48, PLUCK: 55, STAB: 52, POLY: 52, KEYS: 52, FX: 48, SEQ: 43, TEXTURE: 48, BELL: 60, ALL: 48 };
const CATS = ["ALL", "BASS", "LEAD", "PAD", "PLUCK", "STAB", "POLY", "KEYS", "FX", "SEQ", "TEXTURE", "BELL"];
const GAP_TAGS = new Set(["PD", "RING", "SYNC", "FILTER2", "GRIT", "WIDTH"]);   // what the lean voice does not have yet

const S = { list: [], info: {}, filtered: [], sel: -1, cat: "ALL", engine: "full", mode: "chord", reg: 0, vel: 100, seq: 0, timers: [], held: new Map(), ctx: null, worker: null, ring: null, started: false, ready: false, audioNode: null, verdicts: {}, fxHalf: true };

function toast(msg, ms = 2600) { const t = $("#toast"); t.textContent = msg; t.style.display = "block"; clearTimeout(toast.t); toast.t = setTimeout(() => (t.style.display = "none"), ms); }

// ---- the engine worker
function startWorker() {
  if (!crossOriginIsolated) {   // no shared memory: nothing can run, and the visitor should hear why, not meet a dead panel
    if (!$("#noEngine")) {
      const m = document.createElement("div");
      m.id = "noEngine";
      m.className = "wide-note";
      m.style.display = "block";
      m.textContent = isSecureContext ? "Nemesis cannot run on this server: it does not send the two headers the sound engine needs. The start page and the preset librarian still work."
        : "Nemesis needs an https address to run. The start page and the preset librarian still work.";
      document.querySelector("main").before(m);
    }
    return;
  }
  if (!S.ring) S.ring = new SharedArrayBuffer(16 + CAP * 8);
  frameBusy = false;   // a worker that was replaced never answers the frame it was asked for
  S.worker = new Worker("engine-worker.js" + (S.fxHalf ? "?hr=1" : ""));
  S.worker.onmessage = (e) => onWorker(e.data);
  S.worker.postMessage({ cmd: "init", sab: S.ring, cap: CAP });
}
// What EXPORT made goes to the computer: one file as it is, several as a zip (the browser would stop a run of downloads)
function saveExport(files) {
  S.exports = (S.exports || []).concat(files.map((f) => ({ name: f.name, size: f.bytes.length })));
  const single = files.length === 1, bytes = single ? files[0].bytes : PresetSysex.zip(files);
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
  a.download = single ? files[0].name : "Nemesis stems.zip";
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 60000);
}

function onWorker(m) {
  switch (m.ev) {
    case "export": saveExport(m.files); break;
    case "ready":
      S.list = m.list; S.ready = true;
      if (BENCH) buildList();
      if (S.onReady) {   // a lesson's fresh Drumboy, as the hardware boots: the lean voice and no preset forced onto part A. The Presets tab puts its own back (refreshPresets)
        const go = S.onReady; S.onReady = null;
        if (S.sel < 0) S.sel = 0;
        send({ cmd: "master", v: +$("#master").value }); send({ cmd: "engine", lean: 1 });
        go();
        break;
      }
      if (!BENCH) {   // a Drumboy as it boots, as on the lessons' start: the lean voice and no preset forced onto part A
        send({ cmd: "master", v: +$("#master").value }); send({ cmd: "engine", lean: 1 });
        if (window.Learn) Learn.open();
        break;
      }
      if (S.sel < 0) select(0, false); else { select(S.sel, false); send({ cmd: "master", v: +$("#master").value }); } toast(S.swap ? "effects: " + (S.fxHalf ? "half rate" : "full rate") : m.list.length + " presets loaded"); S.swap = false; break;
    case "loaded": if (m.seq === S.seq) { $("#dLoad").textContent = m.ok ? (m.banks.length ? "wavetable bank " + m.banks.join(", ") + " ready" : "") : "could not load"; S.loadedId = m.id; if (S.autoPending) { S.autoPending = false; const sq = m.seq; setTimeout(() => { if (sq === S.seq) play(); }, Math.max(0, S.playAt - performance.now())); } } break;
    case "stats": drawMeter(m); plateBars(m.peakL, m.peakR); break;
    case "bootImage": $("#screen").getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(m.px), 960, 540), 0, 0); break;
    case "frame": drawFrame(m); break;
    case "error": console.error(m.msg); toast("engine error: " + m.msg.split("\n")[0], 6000); break;
  }
}
const send = (m) => S.worker?.postMessage(m);

// ---- audio
function startAudio() { return (S.startP = S.startP || startAudioNow()); }
async function startAudioNow() {
  if (!S.ring) { S.startP = null; return; }   // no engine (see startWorker): nothing to start, and nothing to say it started
  if (S.started) return;
  const ctx = new AudioContext({ sampleRate: 44100, latencyHint: "interactive" });
  await ctx.audioWorklet.addModule("ring-worklet.js");
  const node = new AudioWorkletNode(ctx, "ring-player", { outputChannelCount: [2], processorOptions: { sab: S.ring, cap: CAP } });
  node.connect(ctx.destination);
  await ctx.resume();
  S.ctx = ctx; S.audioNode = node; S.started = true;
  $("#audioState").textContent = "audio on · " + ctx.sampleRate + " Hz";
  document.querySelector(".dot").classList.add("live");
  $("#start").style.display = "none";
  setInterval(() => { if (S.started) send({ cmd: "stats" }); }, 100);
  setInterval(() => { const u = new Int32Array(S.ring, 0, 4)[2] - (S.underrunsSeen || 0); if (u > 0) $("#audioState").textContent = "audio on · underruns " + u; }, 1000);
  send({ cmd: "master", v: +$("#master").value });
}

// ---- the preset list
function grade(id) {
  const g = (S.info[id] || {}).grade;
  if (!g) return "N";
  const order = "ABC";
  return Object.values(g).reduce((w, x) => (order.indexOf(x) > order.indexOf(w) ? x : w), "A");
}
function tagsOf(id) { return (S.info[id] || {}).tags || []; }
function buildCats() {
  const el = $("#cats");
  el.innerHTML = "";
  for (const c of CATS) { const b = document.createElement("span"); b.className = "chip" + (c === S.cat ? " on" : ""); b.textContent = c; b.onclick = () => { S.cat = c; buildCats(); buildList(); }; el.appendChild(b); }
}
function buildList() {
  const q = $("#search").value.trim().toLowerCase(), onlyA = $("#onlyA").checked;
  S.filtered = S.list.filter((p) => {
    if (S.cat !== "ALL" && p.cat !== S.cat) return false;
    if (onlyA && grade(p.id) !== "A") return false;
    const vf = $("#vfilter").value, vd = (S.verdicts[p.id] || {}).verdict || "none";
    if (vf && vd !== vf) return false;
    if (!q) return true;
    return (p.name + " " + p.id + " " + p.cat + " " + tagsOf(p.id).join(" ")).toLowerCase().includes(q);
  });
  const el = $("#list");
  el.innerHTML = "";
  const frag = document.createDocumentFragment();
  S.filtered.forEach((p, i) => {
    const d = document.createElement("div");
    d.className = "row-p" + (p.id === S.selId ? " sel" : "");
    d.dataset.id = p.id;
    const g = grade(p.id);
    d.innerHTML = `<span class="id">${String(p.id).padStart(4, "0")}</span><span>${p.name}</span><span class="cat">${p.cat}</span><span class="muted">${p.voices || ""}</span><span class="g ${g}" title="lean voice match: ${g}">${g === "N" ? "·" : g}</span>${vMark(p.id)}`;
    d.onclick = () => selectId(p.id, true);
    frag.appendChild(d);
  });
  el.appendChild(frag);
  $("#count").textContent = S.filtered.length + " of " + S.list.length;
}
function vMark(id) {
  const v = S.verdicts[id];
  if (!v) return '<span class="vm"></span>';
  if (v.verdict === "rejected") return `<span class="vm rejected" title="rejected: ${esc(v.comment || "")}">✗</span>`;
  if (v.verdict === "approved") return `<span class="vm approved" title="approved">✓</span>`;
  return `<span class="vm note" title="${esc(v.comment || "")}">✎</span>`;
}
const esc = (t) => String(t).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

// ---- verdicts: approve / reject a preset with a comment; the server keeps them in web/feedback/verdicts.json for Claude to pick up
const REASONS = ["too dark", "too bright / harsh", "thin", "too quiet", "too loud", "level differs from full", "clicks / glitches", "wrong pitch", "tail / release", "missing movement", "reverb / fx", "other"];
let vTimer = null;
function loadVerdicts() {
  let local = {};
  try { local = JSON.parse(localStorage.getItem("verdicts") || "{}"); } catch (e) {}
  S.verdicts = local;
  fetch("/api/verdicts").then((r) => r.json()).then((j) => { S.verdicts = { ...local, ...j }; buildList(); showVerdict(); }).catch(() => {});
}
function showVerdict() {
  const p = S.list[S.sel];
  if (!p) return;
  const v = S.verdicts[p.id] || {};
  $("#verdict").className = "card " + (v.verdict === "none" ? "" : v.verdict || "");
  $("#vOk").classList.toggle("on", v.verdict === "approved");
  $("#vNo").classList.toggle("on", v.verdict === "rejected");
  const have = new Set(v.reasons || []);
  $("#vReasons").innerHTML = "";
  for (const r of REASONS) { const c = document.createElement("span"); c.className = "chip" + (have.has(r) ? " on" : ""); c.textContent = r; c.onclick = () => { const cur = new Set((S.verdicts[p.id] || {}).reasons || []); cur.has(r) ? cur.delete(r) : cur.add(r); setVerdict(p, { reasons: [...cur], verdict: (S.verdicts[p.id] || {}).verdict && S.verdicts[p.id].verdict !== "none" ? S.verdicts[p.id].verdict : "rejected" }); }; $("#vReasons").appendChild(c); }
  if (document.activeElement !== $("#vText")) $("#vText").value = v.comment || "";
  $("#vState").textContent = v.time ? "· saved " + v.time.slice(11, 16) : "";
}
function setVerdict(p, patch) {
  const cur = S.verdicts[p.id] || { id: p.id, name: p.name, verdict: "none", reasons: [], comment: "" };
  S.verdicts[p.id] = { ...cur, ...patch };
  showVerdict(); updateMark(p.id);
  clearTimeout(vTimer); vTimer = setTimeout(flushVerdict, 500); S.vPending = p.id;
}
function updateMark(id) { const row = $(`.row-p[data-id="${id}"]`); if (row) { const m = row.querySelector(".vm"); if (m) m.outerHTML = vMark(id); } }
function flushVerdict() {
  clearTimeout(vTimer);
  const id = S.vPending; if (id === undefined) return; S.vPending = undefined;
  const v = S.verdicts[id]; if (!v) return;
  const body = { id, name: v.name, verdict: v.verdict, reasons: v.reasons, comment: v.comment, engine: S.engine, mode: S.mode, register: S.reg, grade: grade(id) };
  try { localStorage.setItem("verdicts", JSON.stringify(S.verdicts)); } catch (e) {}
  fetch("/api/verdict", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) })
    .then((r) => r.json()).then((j) => { if (!j.ok) throw new Error(j.error); if (v.verdict === "none" && !v.comment.trim()) delete S.verdicts[id]; else { v.time = j.time; v.engine = S.engine; } if (S.selId === id) $("#vState").textContent = j.time ? "· saved " + j.time.slice(11, 16) : "· cleared"; })
    .catch(() => { if (S.selId === id) $("#vState").textContent = "· NOT saved to disk (server?)"; });
}
function wireVerdict() {
  const cur = () => S.list[S.sel];
  $("#vOk").onclick = () => {
    const p = cur(); if (!p) return;
    const was = (S.verdicts[p.id] || {}).verdict === "approved";
    setVerdict(p, { verdict: was ? "none" : "approved" });
    if (was) return;
    const i = S.filtered.findIndex((x) => x.id === p.id), nx = S.filtered[i + 1];   // approving moves on to the next preset in the list
    if (nx) selectId(nx.id, true); else toast("last preset in the list");
  };
  $("#vNo").onclick = () => { const p = cur(); if (!p) return; const rej = (S.verdicts[p.id] || {}).verdict === "rejected"; setVerdict(p, { verdict: rej ? "none" : "rejected" }); if (!rej) $("#vText").focus(); };
  $("#vText").oninput = (e) => { const p = cur(); if (p) setVerdict(p, { comment: e.target.value, verdict: (S.verdicts[p.id] || {}).verdict || "none" }); };
  $("#vText").onkeydown = (e) => { if (e.key === "Escape") e.target.blur(); e.stopPropagation(); };
  $("#vfilter").onchange = buildList;
  window.addEventListener("beforeunload", flushVerdict);
}

function selectId(id, play) { const p = S.list.find((x) => x.id === id); if (p) select(S.list.indexOf(p), play); }
function select(idx, doPlay) {
  const p = S.list[idx];
  if (!p) return;
  flushVerdict();
  if (document.activeElement === $("#vText")) $("#vText").blur();
  S.sel = idx; S.selId = p.id;
  for (const r of document.querySelectorAll(".row-p")) r.classList.toggle("sel", +r.dataset.id === p.id);
  const sel = $(".row-p.sel"); if (sel) sel.scrollIntoView({ block: "nearest" });
  $("#dName").textContent = p.name;
  $("#dSub").textContent = `#${p.id} · ${p.cat} · ${p.voices || 4} voice${(p.voices || 4) > 1 ? "s" : ""}`;
  const tags = tagsOf(p.id);
  $("#dTags").innerHTML = tags.map((t) => `<span class="tag${GAP_TAGS.has(t) ? " gap" : ""}" title="${GAP_TAGS.has(t) ? "the lean voice does not have this yet" : ""}">${t}</span>`).join("");
  const gi = (S.info[p.id] || {}).grade, sp = (S.info[p.id] || {}).spec;
  $("#dGrades").innerHTML = gi ? Object.keys(gi).map((n) => `<span><span class="g ${gi[n]}">${gi[n]}</span> note ${n}: level ${sp[n][0] >= 0 ? "+" : ""}${sp[n][0].toFixed(1)} dB, spectrum ${sp[n][1].toFixed(1)} dB mean / ${sp[n][2].toFixed(1)} dB p95</span>`).join("") + `<span>(lean vs full engine, host renders)</span>` : "";
  showVerdict();
  $("#dLoad").textContent = "loading…";
  S.seq++;
  S.playSeq = (S.playSeq || 0) + 1;   // an A/B pattern still running must not carry on into the new preset
  stopAll(true);                      // release every note of the old pattern (its note-offs are cancelled with the timers)
  send({ cmd: "load", id: p.id, seq: S.seq });
  S.autoPending = !!(doPlay && $("#autoplay").checked); S.playAt = performance.now() + 120;   // play once the preset has loaded (the delay lets arrow-key repeats pass)
}

// ---- notes
function root() { const p = S.list[S.sel]; return (REG[p ? p.cat : "ALL"] || 48) + S.reg; }
function clearTimers() { for (const t of S.timers) clearTimeout(t); S.timers = []; }
function stopAll(tell = true) { clearTimers(); S.held.clear(); document.querySelectorAll(".k.down").forEach((k) => k.classList.remove("down")); if (tell) send({ cmd: "allOff" }); }
function at(ms, fn) { S.timers.push(setTimeout(fn, ms)); }
function noteOn(n, v) { send({ cmd: "noteOn", note: n, vel: v || S.vel }); }
function noteOff(n) { send({ cmd: "noteOff", note: n }); }
function schedule(mode) {   // returns the pattern's length in ms
  const R = root(), v = S.vel;
  const hit = (t, n, len) => { at(t, () => noteOn(n, v)); at(t + len, () => noteOff(n)); };
  if (mode === "note") { hit(0, R, 1300); return 1700; }
  if (mode === "chord") { [0, 3, 7, 10].forEach((i, k) => hit(k * 6, R + i, 1800)); return 2300; }
  if (mode === "arp") { [0, 3, 7, 12, 7, 3, 0, 10].forEach((i, k) => hit(k * 150, R + i, 130)); return 1600; }
  if (mode === "line") { [[0, 0], [1, 0], [2, 7], [3, 5], [4, 3], [5, 0], [6, 10], [7, 7]].forEach(([k, i]) => hit(k * 200, R + i, 170)); return 1900; }
  return 0;
}
async function playWith(lean, seq) {
  send({ cmd: "engine", lean });
  await sleep(80);
  if (seq !== undefined && S.playSeq !== seq) return 0;
  if (S.mode === "device") { send({ cmd: "preview", id: S.selId }); return 1500; }
  return schedule(S.mode);
}
async function play() {
  if (!S.started) { await startAudio(); }
  stopAll();
  const seq = ++S.playSeq || (S.playSeq = 1);
  S.playSeq = seq;
  const eng = S.engine;
  if (eng === "ab") {
    toast("A: full engine", 900);
    const len = await playWith(0);
    await sleep(len + 500);
    if (S.playSeq !== seq) return;
    stopAll(); await sleep(100);
    if (S.playSeq !== seq) return;
    toast("B: lean voice", 900);
    await playWith(1, seq);
  } else await playWith(eng === "lean" ? 1 : 0);
}

// ---- the piano
const PIANO = { a: 0, w: 1, s: 2, e: 3, d: 4, f: 5, t: 6, g: 7, y: 8, h: 9, u: 10, j: 11, k: 12, o: 13, l: 14 };
function buildKeyboard() {
  const el = $("#kbd"); el.innerHTML = "";
  const whites = [0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23, 24], blacks = { 1: 0, 3: 1, 6: 3, 8: 4, 10: 5, 13: 7, 15: 8, 18: 10, 20: 11, 22: 12 };
  const w = 100 / whites.length;
  whites.forEach((n, i) => { const k = document.createElement("div"); k.className = "k"; k.dataset.n = n; k.style.left = i * w + "%"; k.style.width = w + "%"; el.appendChild(k); });
  for (const [n, pos] of Object.entries(blacks)) { const k = document.createElement("div"); k.className = "k black"; k.dataset.n = n; k.style.left = (pos + 0.68) * w + "%"; k.style.width = w * 0.62 + "%"; el.appendChild(k); }
  const down = (e) => { const k = e.target.closest(".k"); if (!k) return; e.preventDefault(); pressNote(+k.dataset.n); k.setPointerCapture?.(e.pointerId); };
  const up = (e) => { const k = e.target.closest(".k"); if (k) releaseNote(+k.dataset.n); };
  el.addEventListener("pointerdown", down); el.addEventListener("pointerup", up); el.addEventListener("pointercancel", up);
}
function pressNote(n) {
  if (!S.started) { startAudio().then(() => pressNote(n)); return; }
  if (S.held.has(n)) return;
  const note = root() + n - 12 + 12;
  S.held.set(n, note); noteOn(note, S.vel);
  const k = $(`.k[data-n="${n}"]`); if (k) k.classList.add("down");
}
function releaseNote(n) { const note = S.held.get(n); if (note === undefined) return; S.held.delete(n); noteOff(note); const k = $(`.k[data-n="${n}"]`); if (k) k.classList.remove("down"); }

// ---- the meter
const meter = $("#meter"), mctx = meter.getContext("2d");
let peakHold = 0;
function drawMeter(m) {
  const v = Math.max(m.peakL, m.peakR);
  peakHold = Math.max(v, peakHold * 0.92);
  mctx.clearRect(0, 0, 90, 14);
  const css = getComputedStyle(document.documentElement);
  const w = Math.min(90, Math.sqrt(peakHold) * 90);
  mctx.fillStyle = peakHold > 0.9 ? css.getPropertyValue("--c") : css.getPropertyValue("--a");
  mctx.fillRect(0, 2, w, 10);
  mctx.strokeStyle = css.getPropertyValue("--line"); mctx.strokeRect(0.5, 1.5, 89, 11);
  $("#engNow").textContent = "playing: " + (m.engine ? "LEAN" : "FULL"); $("#engNow").style.color = m.engine ? "var(--blue)" : "var(--accent)";
  $("#audioState").title = "voices " + m.voices + " · engine " + (m.engine ? "lean" : "full");
}

// ---- the Drumboy tab
const KEY_NAMES = ["STOP", "PLAY", "REC", "UP", "ADD", "DOWN", "LEFT", "OK", "RIGHT", "COPY", "CLEAR", "PASTE", "MUTE", "FILL", "STEP", "FILE", "PART", "MIXER", "OSC1", "OSC2", "SUB", "FLT1", "FLT2", "GRIT", "AMP", "MOD", "GLOB", "LANE", "INFO", "SONG", "SHIFT", "SPARE"];
let frameBusy = false, frameImg = null;
function drawFrame(m) {
  frameBusy = false;
  if (performance.now() >= S.bootImageUntil) $("#screen").getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(m.px), 960, 540), 0, 0);   // (the state below is read all the same: a lesson runs behind the picture)
  plateLeds(m.leds);
  plateKnobs(m.knobs);
  plateState(m.state);
  if (window.Learn) Learn.onState(m.state);
}
function buildPanelKeys() { buildPlate({ send: (m) => { S.bootImageUntil = 0; send(m); }, startAudio, master: () => $("#master") }); }   // (as on the hardware, the first key ends the picture)
const BOOT_IMAGE_MS = 1500;   // the first time the panel is seen, the screen starts as the hardware's does
function tickPanel() {
  if (!$("#tab-panel").classList.contains("on") || !S.ready) return;
  if (S.bootImageUntil === undefined) { S.bootImageUntil = performance.now() + BOOT_IMAGE_MS; send({ cmd: "bootImage" }); }
  if (!frameBusy) { frameBusy = true; send({ cmd: "frame" }); }
}

// ---- wiring
function wire() {
  buildPanelKeys();
  $("#start").onclick = startAudio;
  $("#master").oninput = (e) => send({ cmd: "master", v: +e.target.value });
  setInterval(tickPanel, 80);
  if (!BENCH) return;
  buildCats(); buildKeyboard(); wireVerdict(); loadVerdicts();
  $("#search").oninput = buildList; $("#onlyA").onchange = buildList;
  $("#play").onclick = play; $("#stop").onclick = () => { S.playSeq = (S.playSeq || 0) + 1; stopAll(); };
  $("#vel").oninput = (e) => (S.vel = +e.target.value);
  $("#reg").oninput = (e) => { S.reg = +e.target.value; $("#regV").textContent = S.reg ? (S.reg > 0 ? "+" : "") + S.reg : "auto"; };
  document.querySelectorAll("#engineSeg button").forEach((b) => (b.onclick = () => setEngine(b.dataset.e)));
  document.querySelectorAll("#fxSeg button").forEach((b) => (b.onclick = () => setFxHalf(b.dataset.f === "half")));
  document.querySelectorAll("#modeSeg button").forEach((b) => (b.onclick = () => { S.mode = b.dataset.m; document.querySelectorAll("#modeSeg button").forEach((x) => x.classList.toggle("on", x === b)); }));
  document.querySelectorAll(".tab").forEach((t) => (t.onclick = () => { document.querySelectorAll(".tab").forEach((x) => x.classList.toggle("on", x === t)); document.querySelectorAll(".pane").forEach((p) => p.classList.toggle("on", p.id === "tab-" + t.dataset.tab));
    if (t.dataset.tab === "presets") refreshPresets();
    t.blur();   // the keyboard is the panel's again: a focused tab would swallow Space and Enter
  }));
  window.addEventListener("keydown", onKey); window.addEventListener("keyup", onKeyUp);
  window.addEventListener("blur", () => { for (const n of [...S.held.keys()]) releaseNote(n); });
  document.addEventListener("visibilitychange", () => { if (document.hidden) for (const n of [...S.held.keys()]) releaseNote(n); });
  fetch("data/info.json").then((r) => r.json()).then((j) => { S.info = j; if (S.ready) buildList(); }).catch(() => {});
}
// the reverb and delay at half the sample rate (the CPU the board saves) or as they are: the engine worker is replaced by one that loads the other module
// a Drumboy as it boots, for the start of a lesson: a new engine worker, so nothing done before can change what the lesson says
function freshDrumboy() {
  return new Promise((done) => {
    stopAll(false); S.playSeq = (S.playSeq || 0) + 1; S.ready = false; S.onReady = done; S.lessonDrumboy = true;
    S.worker.terminate();
    startWorker();
    S.underrunsSeen = new Int32Array(S.ring, 0, 4)[2] + 64;   // (the swap itself: nothing is wrong)
    $("#audioState").textContent = "audio on";
  });
}
// the Presets tab after a lesson: its engine and preset again, as the page shows them
function refreshPresets() {
  if (!S.lessonDrumboy || !S.ready) return;
  S.lessonDrumboy = false;
  send({ cmd: "key", k: 0, down: true }); send({ cmd: "key", k: 0, down: false });   // (STOP: a pattern a lesson left running has no stop button here)
  send({ cmd: "engine", lean: S.engine === "lean" ? 1 : 0 });
  if (S.sel >= 0) select(S.sel, false);
}
function setFxHalf(on) {
  if (!!on === !!S.fxHalf) return;
  S.fxHalf = !!on; S.swap = true;
  document.querySelectorAll("#fxSeg button").forEach((x) => x.classList.toggle("on", (x.dataset.f === "half") === S.fxHalf));
  stopAll(false); S.ready = false;
  S.worker.terminate();
  startWorker();
}
function setEngine(e) {
  S.engine = e;
  document.querySelectorAll("#engineSeg button").forEach((x) => x.classList.toggle("on", x.dataset.e === e));
  $("#engineHelp").textContent = { full: "The full engine: the reference the lean voice is compared with.", lean: "The lean voice: the same knobs, a fraction of the CPU. Grades in the list say how close it is to the full engine.", ab: "Plays the pattern twice: A = full engine, then B = lean voice." }[e];
}
function onKey(e) {
  if (e.target.matches("textarea, select")) return;
  if (e.target.matches("input[type=search]")) { if (e.key === "Escape" || e.key === "ArrowDown") e.target.blur(); else return; }
  if (e.metaKey || e.ctrlKey || e.altKey) return;
  if ($("#tab-panel").classList.contains("on")) return;
  if (e.key === "ArrowDown" || e.key === "ArrowUp") {
    e.preventDefault();
    const i = S.filtered.findIndex((p) => p.id === S.selId), j = Math.max(0, Math.min(S.filtered.length - 1, i + (e.key === "ArrowDown" ? 1 : -1)));
    if (S.filtered[j]) selectId(S.filtered[j].id, true);
  } else if (e.key === " ") { e.preventDefault(); if (!e.repeat) play(); }
  else if (e.key === "Escape") { S.playSeq = (S.playSeq || 0) + 1; stopAll(); }
  else if (e.key === "1" || e.key === "2" || e.key === "3") { e.preventDefault(); if (!e.repeat) { setEngine({ 1: "full", 2: "lean", 3: "ab" }[e.key]); play(); } }   // pick the engine and play it at once
  else if (e.key.toLowerCase() === "x") { if (!e.repeat) setFxHalf(!S.fxHalf); }
  else if (e.key === "Tab") { e.preventDefault(); setEngine({ full: "lean", lean: "ab", ab: "full" }[S.engine]); }
  else if (e.key === "[") { $("#reg").value = S.reg = Math.max(-24, S.reg - 12); $("#regV").textContent = S.reg || "auto"; }
  else if (e.key === "]") { $("#reg").value = S.reg = Math.min(24, S.reg + 12); $("#regV").textContent = S.reg || "auto"; }
  else if (e.key.toLowerCase() === "v") $("#vOk").click();
  else if (e.key.toLowerCase() === "r") { e.preventDefault(); if (!e.repeat) { const p = S.list[S.sel]; if (p && (S.verdicts[p.id] || {}).verdict !== "rejected") $("#vNo").click(); else $("#vText").focus(); } }
  else if (e.key === "/") { e.preventDefault(); $("#search").focus(); }
  else if (!e.repeat && PIANO[e.key.toLowerCase()] !== undefined) pressNote(PIANO[e.key.toLowerCase()]);
}
function onKeyUp(e) { const n = PIANO[e.key.toLowerCase()]; if (n !== undefined) releaseNote(n); }

wire();
startWorker();
