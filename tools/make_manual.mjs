// What tools/make_manual.sh runs: the screens of the manual, then the PDF. Node 22+, no packages.
//   node tools/make_manual.mjs URL [--no-shots]      URL: `python3 web/serve.py PORT`, serving web/www
import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import zlib from "node:zlib";

const root = path.join(path.dirname(new URL(import.meta.url).pathname), "..");
const out = path.join(root, "web/www/manual"), pdf = path.join(root, "web/www/downloads/nemesis-manual.pdf");
const base = process.argv[2].replace(/\/?$/, "/"), noShots = process.argv.includes("--no-shots");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
fs.mkdirSync(out, { recursive: true });
fs.mkdirSync(path.dirname(pdf), { recursive: true });

// ---- the screens the firmware's own code draws: a state of tests/app/render_screens.cpp -> the file
const DRAWN = { step1: "step-editor", step2: "step-chord", drum2: "step-sample", tracker: "tracker", tracker_pop: "tracker-popover",
  tracker_sampler: "tracker-sampler", tracker_stop: "tracker-limit", flt1: "filter1", amp: "amp", "osc-bank": "osc-wavetable", "osc-bank-missing": "osc-missing", smp: "sample", lock: "lock",
  mix1: "mix-levels", mix2: "mix-voices", mix4: "mix-cpu", rec: "rec", browse: "browse", project: "project-key", "fx-delay": "fx-delay", "fx-reverb": "fx-reverb", "fx-chorus": "fx-chorus", "fx-comp": "fx-comp", "fx-amp": "fx-amp",
  "fx-sends": "fx-sends", "fx-lock": "fx-lock", "project-midi": "project-midi", "save-new": "save-preset", song: "song", slice: "slices", "picker-folder": "picker-folder", "picker-play": "picker-sample", play_sel: "home-marks" };

// A small PNG writer (RGB, each row with the filter that suits a screen: the left neighbour)
function png(w, h, rgb) {
  const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc = (b) => { let c = 0xffffffff; for (const x of b) c = crcTable[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
  const chunk = (t, d) => { const b = Buffer.concat([Buffer.from(t), d]), l = Buffer.alloc(4), c = Buffer.alloc(4); l.writeUInt32BE(d.length); c.writeUInt32BE(crc(b)); return Buffer.concat([l, b, c]); };
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0, o = 0; y < h; ++y) { raw[o++] = 1; for (let x = 0; x < w * 3; ++x) raw[o++] = (rgb[y * w * 3 + x] - (x >= 3 ? rgb[y * w * 3 + x - 3] : 0)) & 255; }
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", zlib.deflateSync(raw, { level: 9 })), chunk("IEND", Buffer.alloc(0))]);
}
function drawnScreens() {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "nemesis-screens-"));
  const r = spawnSync(path.join(root, "tests/build/render_screens_lean"), [tmp, ...Object.keys(DRAWN)], { stdio: ["ignore", "ignore", "inherit"] });
  if (r.status !== 0) throw new Error("render_screens_lean failed");
  for (const [state, name] of Object.entries(DRAWN)) {
    const b = fs.readFileSync(path.join(tmp, state + ".ppm"));
    const head = /^P6\s+(\d+)\s+(\d+)\s+255\s/.exec(b.subarray(0, 32).toString("latin1"));
    const w = +head[1], h = +head[2];
    fs.writeFileSync(path.join(out, name + ".png"), png(w, h, b.subarray(head[0].length)));
  }
  fs.rmSync(tmp, { recursive: true, force: true });
}

// ---- headless Chrome
const port = 9700 + Math.floor(Math.random() * 90);
const prof = fs.mkdtempSync(path.join(os.tmpdir(), "nemesis-manual-"));
const chrome = spawn("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", ["--headless=new", "--disable-gpu", "--no-sandbox", "--mute-audio", `--remote-debugging-port=${port}`, `--user-data-dir=${prof}`, "--window-size=1500,950", "--autoplay-policy=no-user-gesture-required", "about:blank"], { stdio: "ignore" });
const closeChrome = async () => { chrome.kill(); await sleep(400); fs.rmSync(prof, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }); };
let ws, id = 0;
const pending = new Map();
const call = (method, params = {}) => new Promise((res, rej) => { const i = ++id; pending.set(i, { res, rej }); ws.send(JSON.stringify({ id: i, method, params })); });
async function ev(e) {
  const r = await call("Runtime.evaluate", { expression: e, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) throw new Error("in the page: " + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
  return r.result.value;
}
async function goto(page, ready) {
  await call("Page.navigate", { url: new URL(page, base).href });
  for (let k = 0; k < 400 && !(await ev(`document.readyState === "complete" && location.pathname.endsWith(${JSON.stringify(page)}) && (${ready})`).catch(() => false)); ++k) await sleep(50);
}

// ---- the screens that come from Online Nemesis: real keys and a real mouse wheel on the page's own panel (panel.js KEYMAP)
const CODE = { PLAY: "Space", STOP: "Escape", OK: "Enter", UP: "ArrowUp", DOWN: "ArrowDown", LEFT: "ArrowLeft", RIGHT: "ArrowRight", CLEAR: "Backspace", ADD: "Period", REC: "KeyR", MUTE: "KeyM", FILL: "KeyF", STEP: "KeyS", COPY: "KeyC", PASTE: "KeyP",
  LANE: "KeyL", PROJECT: "KeyI", SONG: "KeyO", FX: "KeyX", FILE: "KeyB", PART: "KeyN", MIXER: "KeyE", OSC1: "KeyQ", OSC2: "KeyW", SUB: "KeyT", FLT1: "KeyA", FLT2: "KeyD", GRIT: "KeyG", AMP: "KeyH", MOD: "KeyJ", GLOB: "KeyK" };
for (let i = 1; i <= 10; ++i) CODE["K" + i] = "Digit" + (i % 10);
const keyEvent = (type, code, mod = 0) => call("Input.dispatchKeyEvent", { type, code, key: code.startsWith("Key") ? code.slice(3).toLowerCase() : code.startsWith("Digit") ? code.slice(5) : code === "Space" ? " " : code === "Period" ? "." : code === "Backspace" ? "Backspace" : code === "Escape" ? "Escape" : code, windowsVirtualKeyCode: 0, modifiers: mod });
async function press(name, ms = 40) { await keyEvent("rawKeyDown", CODE[name]); await sleep(ms); await keyEvent("keyUp", CODE[name]); await sleep(60); }
async function shift(on) { await call("Input.dispatchKeyEvent", { type: on ? "rawKeyDown" : "keyUp", key: "Shift", code: "ShiftLeft", modifiers: on ? 8 : 0 }); await sleep(60); }
async function turn(knob, n) {
  const [x, y] = await ev(`(() => { const b = document.querySelector('[title="${knob}"]').getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2]; })()`);
  await call("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
  for (let i = 0; i < Math.abs(n); ++i) { await call("Input.dispatchMouseEvent", { type: "mouseWheel", x, y, deltaX: 0, deltaY: n > 0 ? -120 : 120 }); await sleep(12); }
  await sleep(120);
}
async function capture(name, settle = 1400) {
  await sleep(settle);   // a toast has gone, and a new frame has come
  const d = await ev(`document.querySelector("#screen").toDataURL("image/png")`);
  fs.writeFileSync(path.join(out, name + ".png"), Buffer.from(d.split(",")[1], "base64"));
}
const fresh = async () => { await goto("online.html", "typeof S !== 'undefined' && S.ready && !!S.worker"); await sleep(1800); await press("STOP"); };
const home = async () => { await shift(true); await press("LANE"); await shift(false); };

async function panelScreens() {
  await fresh();
  // the factory rows, then a slot saved and about to be deleted
  await press("PROJECT"); for (let i = 0; i < 3; ++i) await press("UP");
  await press("OK");
  await capture("project-list");
  await home(); await press("PLAY"); await press("K1");
  await sleep(1300); await capture("home-demo", 300);
  await press("STOP");
    await press("K8"); await press("STEP"); await capture("step-kick"); await home();
  await press("PROJECT"); for (let i = 0; i < 3; ++i) await press("DOWN");
  await shift(true); await press("OK"); await shift(false); await sleep(1300);
  await press("CLEAR"); await capture("project-delete", 300);
  await press("STOP"); await home();
  await press("PROJECT"); await press("PROJECT"); await press("PROJECT"); await capture("export"); await home();
  // the pages the renderer has no state for, on part A
  await fresh(); await press("K1");
  await press("OSC2"); await turn("B3", 40); await turn("T4", 1); await capture("osc2");
  await press("SUB"); await turn("T1", 60); await turn("T3", 30); await turn("B1", 2); await capture("sub");
  await press("FLT2"); await turn("B2", 1); await turn("T1", 0); await capture("filter2");
  await press("GRIT"); await turn("T1", -10); await turn("T3", -60); await capture("grit");
  await press("MOD"); await turn("T2", 60); await capture("mod");
  await press("GLOB"); await capture("global"); await home();
  // a sample loop, as the tutorial's lesson makes one
  await fresh(); await press("K10"); await press("OSC1"); await shift(true); await turn("T1", 1); await shift(false); await turn("T1", 3);
  await turn("T2", 13); await turn("T3", -63); await turn("T4", 2);
  await press("SUB"); await turn("T2", 30); await turn("T1", 1); await capture("loop-auto", 900); await home();
  // slices cut on a grid, one slice at a time
  await fresh(); await press("K10"); await press("OSC1"); await shift(true); await turn("T1", 3); await shift(false); await turn("T1", 1); await press("OSC1");
  await press("OSC2"); await turn("T1", 1); await turn("B4", 2); await sleep(900); await press("OSC2"); await turn("T1", 3); await turn("T3", 5); await capture("slice-edit");
}

// ---- the PDF
const printed = () => call("Page.printToPDF", { printBackground: true, preferCSSPageSize: true, displayHeaderFooter: false, generateTaggedPDF: true, generateDocumentOutline: true }).then((r) => Buffer.from(r.data, "base64"));
function destinations(file) {
  const t = spawnSync("pdfinfo", ["-dests", file], { encoding: "utf8", maxBuffer: 1 << 26 }).stdout;
  const pages = {};
  for (const m of t.matchAll(/^\s*(\d+)\s+\[.*\]\s+"(.+)"\s*$/gm)) pages[m[2]] = +m[1];
  return pages;
}
async function makePdf() {
  await goto("manual.html", "true");
  await ev(`document.fonts.ready.then(() => Promise.all([...document.images].map((i) => { i.loading = "eager"; return i.decode().catch(() => {}); })))`);
  await sleep(500);
  const tmp = pdf + ".tmp";
  let pages = {};
  for (let pass = 0; pass < 3; ++pass) {   // (a number that gets wider could move a page: the last pass has to change nothing)
    fs.writeFileSync(tmp, await printed());
    const now = destinations(tmp);
    if (JSON.stringify(now) === JSON.stringify(pages)) break;
    pages = now;
    await ev(`(${JSON.stringify(pages)}, (p) => document.querySelectorAll("[data-page-of]").forEach((e) => (e.textContent = p[e.dataset.pageOf] ?? "?")))(${JSON.stringify(pages)})`);
  }
  fs.renameSync(tmp, pdf);
  const missing = await ev(`[...document.querySelectorAll("[data-page-of]")].filter((e) => e.textContent === "?").map((e) => e.dataset.pageOf)`);
  if (missing.length) console.log("no page found for:", missing.join(" "));
  console.log(Object.keys(pages).length, "places found;", fs.statSync(pdf).size, "bytes");
}

(async () => {
  try {
    if (!noShots) drawnScreens();
    let targets;
    for (let k = 0; k < 50; ++k) { try { targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json(); if (targets.find((t) => t.type === "page")) break; } catch {} await sleep(200); }
    ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
    await new Promise((r) => (ws.onopen = r));
    ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pending.has(d.id)) { const p = pending.get(d.id); pending.delete(d.id); d.error ? p.rej(new Error(JSON.stringify(d.error))) : p.res(d.result); } };
    await call("Runtime.enable"); await call("Page.enable");
    if (!noShots) await panelScreens();
    await makePdf();
    await closeChrome();
    process.exit(0);
  } catch (e) { console.error(e); await closeChrome(); process.exit(1); }
})();
