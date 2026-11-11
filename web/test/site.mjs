// The pages in headless Chrome with real pointer events (Node 22+, no packages), and every step of every lesson played by hand, by wheel and by drag. Needs
// `python3 web/serve.py PORT` running, with the web build in web/www.
//   node web/test/site.mjs [URL] [--shots DIR] [--only NAME]      exit status 1 when a check fails
// The librarian is tested against its simulated device and against fake Web MIDI ports. None of this says anything about hardware: that is docs/PRESET_SYSEX.md's status line.
import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import zlib from "node:zlib";
import os from "node:os";
import path from "node:path";
const args = process.argv.slice(2);
const base = (args[0] && !args[0].startsWith("--") ? args[0] : "http://localhost:8765/").replace(/\/?$/, "/");
const shots = args.includes("--shots") ? args[args.indexOf("--shots") + 1] : null;
const only = args.includes("--only") ? args[args.indexOf("--only") + 1] : null;
const root = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "..");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let checks = 0, fails = 0;
const ok = (c, m) => { ++checks; if (!c) { ++fails; console.log("FAIL", m); } };

const port = 9800 + Math.floor(Math.random() * 150);
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "nemesis-site-"));
const chrome = spawn("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", ["--headless=new", "--disable-gpu", "--no-sandbox", `--remote-debugging-port=${port}`, `--user-data-dir=${tmp}/prof`, "--autoplay-policy=no-user-gesture-required", "--mute-audio", "--window-size=1400,950", "about:blank"], { stdio: "ignore" });
let ws, id = 0;
const pending = new Map(), problems = [], dialogs = [];
let leavePage = true;   // what a page's "leave this page?" question is answered
const call = (method, params = {}) => new Promise((res, rej) => { const i = ++id; pending.set(i, { res, rej }); ws.send(JSON.stringify({ id: i, method, params })); });
async function ev(e) {
  const r = await call("Runtime.evaluate", { expression: e, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) throw new Error("in the page: " + (r.exceptionDetails.exception?.description || r.exceptionDetails.text) + "\n  " + e);
  return r.result.value;
}
const until = async (e, ms = 8000, what = e) => { const t0 = Date.now(); for (;;) { if (await ev(e)) return true; if (Date.now() - t0 > ms) { ok(false, "timed out waiting for: " + what); return false; } await sleep(40); } };
async function goto(page) { problems.length = 0; await ev("window.__leaving = 1"); await call("Page.navigate", { url: new URL(page, base).href }); await until("!window.__leaving && document.readyState === 'complete'"); }   // (the mark goes with the old page: its readyState says nothing about the new one)
async function viewport(w, h, mobile = false) { await call("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: mobile ? 2 : 1, mobile }); }
async function shot(name) { if (!shots) return; await sleep(200); const r = await call("Page.captureScreenshot", { format: "png" }); fs.mkdirSync(shots, { recursive: true }); fs.writeFileSync(path.join(shots, name + ".png"), Buffer.from(r.data, "base64")); }
async function centre(sel) {
  const r = await ev(`(() => { const e = typeof ${JSON.stringify(sel)} === "string" ? document.querySelector(${JSON.stringify(sel)}) : null; if (!e) return null; e.scrollIntoView({ block: "center", inline: "center", behavior: "instant" }); const b = e.getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2, e.disabled === true]; })()`);
  if (!r) throw new Error("no element " + sel);
  return r;
}
// a press and a release of the left button at an element's middle, as a person would: it goes through hit testing
async function click(sel, holdMs = 30) {
  let at = await centre(sel);
  for (let k = 0; k < 100 && at[2]; ++k) { await sleep(30); at = await centre(sel); }
  const [x, y, off] = at;
  if (off) ok(false, "clicking a disabled button: " + sel);
  await call("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
  await call("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", buttons: 1, clickCount: 1, pointerType: "mouse" });
  await sleep(holdMs);
  await call("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", clickCount: 1, pointerType: "mouse" });
  await sleep(30);
}
async function wheel(sel, dy, times = 1) {
  const [x, y] = await centre(sel);
  await call("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
  for (let i = 0; i < times; ++i) { await call("Input.dispatchMouseEvent", { type: "mouseWheel", x, y, deltaX: 0, deltaY: dy }); await sleep(15); }
}
const text = (sel) => ev(`document.querySelector(${JSON.stringify(sel)}).textContent`);
async function drag(sel, dy) {
  const [x, y] = await centre(sel);
  await call("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
  await call("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", buttons: 1, clickCount: 1, pointerType: "mouse" });
  for (let i = 1; i <= 8; ++i) { await call("Input.dispatchMouseEvent", { type: "mouseMoved", x, y: y + dy * i / 8, buttons: 1, button: "left" }); await sleep(20); }
  await call("Input.dispatchMouseEvent", { type: "mouseReleased", x, y: y + dy, button: "left", clickCount: 1, pointerType: "mouse" });
  await sleep(250);
}
const knobAngle = (name) => ev(`parseFloat(document.querySelector('[title="${name}"] .cap').style.transform.slice(7))`);
async function knobStops() {
  const bpmNow = () => ev(`window.__st[${BPM}]`);
  await drag('[title="T1"]', -320);
  ok(await bpmNow() === 240, "a knob dragged far past its top is at the firmware's top: " + await bpmNow());
  ok(Math.abs(await knobAngle("T1") - 135) < 0.01, "and its pointer at the top stop: " + await knobAngle("T1"));
  await wheel('[title="T1"]', -40, 6);
  await sleep(200);
  ok(Math.abs(await knobAngle("T1") - 135) < 0.01, "the wheel does not take it past the stop");
  await drag('[title="T1"]', 320);
  ok(await bpmNow() === 40 && Math.abs(await knobAngle("T1") + 135) < 0.01, "dragged down, the bottom stop: " + await bpmNow());
  await drag('[title="T1"]', -160);
  ok(await bpmNow() === 240, "one short drag covers all of it, 200 steps: " + await bpmNow());
  await drag('[title="T1"]', 110);
  // the arrow keys, on a knob that was clicked, go one step
  const before = await bpmNow();
  await click('[title="T1"]');
  await call("Input.dispatchKeyEvent", { type: "keyDown", key: "ArrowUp", code: "ArrowUp", windowsVirtualKeyCode: 38 });
  await call("Input.dispatchKeyEvent", { type: "keyUp", key: "ArrowUp", code: "ArrowUp", windowsVirtualKeyCode: 38 });
  await until(`window.__st[${BPM}] !== ${before}`, 3000, "an arrow key to move a clicked knob");
  ok(await bpmNow() === before + 1, "an arrow key moves a clicked knob one step: " + before + " to " + await bpmNow());
  await wheel('[title="T1"]', -120);
  await until(`window.__st[${BPM}] !== ${before + 1}`, 3000, "a notch of a mouse's wheel to move a knob");
  ok(await bpmNow() === before + 2, "one notch of a mouse's wheel is one step: " + (before + 1) + " to " + await bpmNow());
  // a key takes the arrow keys back: they are the panel's UP and DOWN again
  await click('[aria-label="STOP"]');
  const lane = await ev(`window.__st[2]`);
  await call("Input.dispatchKeyEvent", { type: "keyDown", key: "ArrowDown", code: "ArrowDown", windowsVirtualKeyCode: 40 });
  await call("Input.dispatchKeyEvent", { type: "keyUp", key: "ArrowDown", code: "ArrowDown", windowsVirtualKeyCode: 40 });
  await until(`window.__st[2] !== ${lane}`, 3000, "the arrow keys to be the panel's again after a key was pressed");
}
// a list of a few values: a drag goes to the nearest of them, and does not flip between two as the pointer moves
async function knobLists() {
  const mode = () => ev(`window.__st[48]`);
  await click('[aria-label="layer 10"]');
  await click('[aria-label="OSC 2"]');
  await until(`window.__st[3] === 12`, 3000, "the SLICE page");
  await drag('[title="T1"]', -20);
  ok(await mode() === 0, "a short drag on a list of three stays: " + await mode());
  await drag('[title="T1"]', -60);
  ok(await mode() === 1, "60 pixels of 160 is nearest the middle of three: " + await mode());
  await drag('[title="T1"]', -60);
  ok(await mode() === 2, "and on to the top: " + await mode());
  await drag('[title="T1"]', -60);
  ok(await mode() === 2, "and it stays at the stop: " + await mode());
  await drag('[title="T1"]', 160);
  ok(await mode() === 0, "a whole range back: " + await mode());
  // on the way, pixel by pixel: it only ever rises, and exactly half way between two values it holds still
  const [x, y] = await centre('[title="T1"]');
  await call("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
  await call("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", buttons: 1, clickCount: 1, pointerType: "mouse" });
  const seen = [];
  for (let d = 1; d <= 40; ++d) { await call("Input.dispatchMouseEvent", { type: "mouseMoved", x, y: y - d, buttons: 1, button: "left" }); await sleep(30); seen.push(await mode()); }
  ok(seen.every((v) => v === 0), "up to half way the value holds: " + seen.join(""));
  const atHalf = [];
  for (let d = 1; d <= 6; ++d) { await call("Input.dispatchMouseEvent", { type: "mouseMoved", x: x + d, y: y - 40, buttons: 1, button: "left" }); await sleep(40); atHalf.push(await mode()); }
  ok(new Set(atHalf).size === 1, "and exactly half way it does not flip: " + atHalf.join(""));
  await call("Input.dispatchMouseEvent", { type: "mouseReleased", x, y: y - 40, button: "left", clickCount: 1, pointerType: "mouse" });
  await sleep(150);
  await drag('[title="T1"]', 160);
  await click('[aria-label="LANE"]');
}
// what every page must be: no errors, nothing from outside, the faces loaded, nothing wider than the screen
async function common(name) {
  ok(problems.length === 0, name + ": the console is clean: " + problems.join(" | "));
  const outside = await ev(`performance.getEntriesByType("resource").map((r) => r.name).concat([...document.querySelectorAll("[src],[href]")].map((e) => e.src || (e.href?.baseVal ? new URL(e.href.baseVal, location.href).href : e.href))).filter((u) => u && !u.startsWith(location.origin) && !u.startsWith("blob:") && !u.startsWith("data:"))`);
  ok(outside.length === 0, name + ": nothing is loaded from outside: " + outside.join(" "));
  ok(await ev(`document.fonts.ready.then(() => document.fonts.check('18.667px "Jersey 10"'))`), name + ": the font is loaded");
  if (name !== "install" && name !== "manual") ok(!/cathode/i.test(await ev("document.body.innerText + document.title")), name + ": says Nemesis, not the old name");   // (the install page and the manual have to name the folder)
  ok(await ev(`parseFloat(getComputedStyle(document.body).fontSize) >= 18 && parseFloat(getComputedStyle(document.body).lineHeight) >= 26`), name + ": the text is at its readable size");
  ok(await ev("[...document.querySelectorAll('#bar nav a')].length") === 6, name + ": the menu has the six pages");
}
const wide = (name) => ev("document.documentElement.scrollWidth - innerWidth").then((d) => ok(d <= 0, name + ": fits the screen, " + d + " px too wide"));

// the title screen: it runs while it is seen, stops when it is not, holds still when asked to, and draws something
const frameCount = () => ev(`+(document.querySelector("#demo").dataset.frames || 0)`);
async function demoChecks() {
  await until(`+(document.querySelector("#demo").dataset.frames || 0) > 3`, 5000, "the title screen to run");
  ok(await ev(`(() => { const d = document.querySelector("#demo").getContext("2d").getImageData(0, 0, 480, 200).data, seen = new Set(); for (let i = 0; i < d.length; i += 400) seen.add(d[i] + "," + d[i + 1]); return seen.size > 5; })()`), "start: the title screen is drawn");
  const a = await frameCount(); await sleep(1000); const b = await frameCount();
  ok(b - a >= 5 && b - a <= 35, "start: the title screen runs at 30 frames a second at most: " + (b - a));
  await ev(`document.querySelector("#scores").scrollIntoView({ behavior: "instant" }); 1`);
  await sleep(400);
  const c = await frameCount(); await sleep(600); const d = await frameCount();
  ok(c === d, "start: the title screen stops when it is off-screen: " + (d - c));
  await ev(`scrollTo({ top: 0, behavior: "instant" }); 1`);
  await sleep(400);
  ok(await frameCount() > d, "start: and it starts again when it is back");
  await call("Emulation.setEmulatedMedia", { features: [{ name: "prefers-reduced-motion", value: "reduce" }] });
  await goto("index.html");
  await until(`+(document.querySelector("#demo").dataset.frames || 0) > 0`, 5000, "the still frame");
  const e = await frameCount(); await sleep(600); const f = await frameCount();
  ok(e === f && e <= 3, "start: reduced motion holds one still frame: " + e + " then " + f);
  ok(await ev(`getComputedStyle(document.querySelector(".scroll-text")).position === "static" && getComputedStyle(document.querySelector(".press")).animationName === "none"`), "start: reduced motion shows the scroller's words as plain text, and PRESS START does not blink");
  await call("Emulation.setEmulatedMedia", { features: [] });
  await goto("index.html");
}
async function startPage() {
  await viewport(1400, 900);
  await goto("index.html");
  await common("start");
  await demoChecks();
  ok(await ev(`document.querySelectorAll(".items .item").length`) >= 15, "start: the power-ups are there");
  ok(await ev(`document.querySelectorAll(".map a").length`) === 10, "start: the stage select has its ten stages");
  ok(await ev(`[...document.querySelectorAll(".map a")].every((a) => !!document.querySelector(a.getAttribute("href")))`), "start: every stage on the map exists");
  ok(await ev(`document.querySelectorAll(".scores tbody tr").length`) === 10, "start: the hi-score table is there");
  const body = await ev("document.body.innerText");
  for (const w of ["Seven parts", "slices", "Effects", "200 presets", "COMING", "Beta 2", "New in Beta 2", "Tracker", "stems", "manual"]) ok(body.includes(w), "start: mentions " + w);
  ok(body.toLowerCase().includes("learn it in sixteen levels") && LESSON_TITLES.length === 16, "start: says how many lessons there are");
  ok(await ev(`![...document.querySelectorAll(".soon")].some((s) => /Recording|Loop/.test(s.parentElement.textContent)) && [...document.querySelectorAll(".soon")].some((s) => /librarian/i.test(s.parentElement.textContent))`), "start: only the librarian is labelled coming");
  ok(!/(cannot|can't|never|won't) (be )?(corrupt|damage|lose)/i.test(body), "start: makes no promise about cards");
  await shot("start_laptop");
  // over the whole bounce the logo stays on the canvas: the rows just inside the top edge hold nothing but stars and copper
  ok(await ev(`new Promise((done) => { const c = document.querySelector("#demo"), g = c.getContext("2d"); let clear = true, n = 0; const t = setInterval(() => { const d = g.getImageData(0, 0, c.width, 2).data; const cols = new Set(); for (let i = 0; i < d.length; i += 4) cols.add(d[i] * 65536 + d[i + 1] * 256 + d[i + 2]); clear = clear && !cols.has(0x2c4688) && !cols.has(0xdfeafa); if (++n > 40) { clearInterval(t); done(clear); } }, 40); })`), "start: the logo's spikes stay on the canvas through the bounce");
  ok(await ev(`Promise.all(["logo-440", "logo-340", "logo-260"].map((n) => new Promise((res) => { const i = new Image(); i.onload = () => { const c = document.createElement("canvas"); c.width = i.width; c.height = i.height; const g = c.getContext("2d"); g.drawImage(i, 0, 0); const d = g.getImageData(0, 0, i.width, i.height).data; let hard = true; for (let k = 3; k < d.length; k += 4) hard = hard && (d[k] === 0 || d[k] === 255); res(hard); }; i.src = "img/" + n + ".png"; }))).then((a) => a.every(Boolean))`), "start: the logo's alpha is hard: 0 or 255, no edge to fringe");
  ok(await ev(`(() => { const c = document.querySelector("#demo"); return c.getBoundingClientRect().width % c.width === 0 && c.getBoundingClientRect().height % c.height === 0; })()`), "start: the title screen is scaled by a whole number");
  // the tour: real screens at their real size, every label on its frame, one note for each label, and a note lights its label under the pointer
  await ev(`document.querySelectorAll(".shot img").forEach((i) => (i.loading = "eager")); Promise.all([...document.querySelectorAll(".shot img")].map((i) => i.decode().catch(() => null))).then(() => 1)`);
  ok(await ev(`[...document.querySelectorAll(".shot img")].every((i) => i.naturalWidth === 960 && i.naturalHeight === 540)`), "start: the screens are the firmware's 960 x 540 frames");
  ok(await count(".tour") === 10, "start: ten screens are shown");
  ok(await ev(`[...document.querySelectorAll(".tour")].every((t) => t.querySelectorAll(".mark").length === t.querySelectorAll(".notes li").length && t.querySelectorAll(".mark").length >= 3)`), "start: each screen has its labels, one note for each");
  ok(await ev(`[...document.querySelectorAll(".shot")].every((s) => { const i = s.querySelector("img").getBoundingClientRect(); return [...s.querySelectorAll(".mark")].every((m) => { const b = m.getBoundingClientRect(); return b.left >= i.left - 3 && b.right <= i.right + 3 && b.top >= i.top - 3 && b.bottom <= i.bottom + 3 && b.width > 8 && b.height > 8; }); })`), "start: every label is on its frame");
  await ev(`document.querySelector(".tour .notes li:nth-child(2)").dataset.t = "note"; 1`);
  await centre('[data-t="note"]').then(([x, y]) => call("Input.dispatchMouseEvent", { type: "mouseMoved", x, y }));
  await until(`document.querySelector(".tour .mark:nth-of-type(2)").classList.contains("on")`, 2000, "a note to light its label");
  await shot("start_tour");
  for (const [sel, name] of [["#stage-3", "start_stage3"], ["#stage-5", "start_stage5"], ["#scores", "start_scores"], ["#powerups", "start_powerups"], ["#status", "start_status"]]) {
    await ev(`document.querySelector(${JSON.stringify(sel)}).scrollIntoView({ block: "start", behavior: "instant" }); 1`);
    await shot(name);
  }
  await click('#bar nav a[href="librarian.html"]');
  await until(`location.pathname.endsWith("librarian.html")`);
  await click('#bar nav a[href="tutorial.html"]');
  await until(`location.pathname.endsWith("tutorial.html")`);
  await click('#bar nav a[href="online.html"]');
  await until(`location.pathname.endsWith("online.html")`);
  await click('#bar .logo');
  await until(`document.querySelector(".title-screen") !== null`);
  await click(".press");
  await until(`document.querySelector("#stages").getBoundingClientRect().top < innerHeight / 2`, 3000, "PRESS START to go to the stages");
  await viewport(1366, 768); await goto("index.html"); await wide("start, laptop");
  await viewport(390, 844, true); await goto("index.html"); await wide("start, phone"); await shot("start_phone");
  ok(await ev(`getComputedStyle(document.querySelector(".stage-body")).gridTemplateColumns.split(" ").length`) === 1, "start, phone: one column");
  await viewport(1400, 900);
}

// the site's styles must leave the plate as style.css draws it, and the panel must fit its column
async function plateIntact(name) {
  ok(await ev(`getComputedStyle(document.querySelector("#plate .bar.l")).position === "absolute" && getComputedStyle(document.querySelector("#plate .bar.l")).backgroundColor === "rgba(0, 0, 0, 0)"`), name + ": the plate's level bars are as style.css draws them");
  ok(await ev(`(() => { const w = document.querySelector("#panelWrap"); return w.scrollWidth <= w.clientWidth; })()`), name + ": the panel fits, with nothing to scroll sideways");
}
async function frames() { await until("typeof S !== 'undefined' && S.ready && !!S.worker", 20000, "the engine"); await ev(`(window.__st = null, S.worker.addEventListener("message", (e) => { if (e.data.ev === "frame") window.__st = e.data.state; }), 1)`); await until("window.__st !== null", 8000, "the first frame"); }
const PLAYING = 4, BPM = 30;
const LESSON_TITLES = ["First sound", "Lanes and parts", "Your first beat", "Choosing sounds", "Shaping a sound", "Melody", "P-locks", "Tracker", "Slices", "Sample loops", "Recording", "Effects", "Mixing and CPU", "Patterns and songs", "Projects", "Exporting"];
async function onlinePage() {
  await viewport(1400, 950);
  await goto("online.html");
  await common("online");
  ok(await count("#demo, #sky") === 0 && await ev(`getComputedStyle(document.querySelector("body")).animationName`) === "none", "online: nothing moves behind the panel");
  await frames();
  ok(await ev(`[window.__st[20], window.__st[21], window.__st[22]].every((v) => v === -1)`), "online: it boots as the hardware does, with no preset forced onto a part");
  await shot("online_laptop");
  await click("#start");
  await until("S.started", 6000, "the audio to start");
  await until(`/LEAN/.test(document.querySelector("#engNow").textContent)`, 3000, "the lean voice, as on the board");
  ok(await ev(`[...document.querySelectorAll("#plate .key")].length`) === 42, "online: the panel has its keys");
  await plateIntact("online");
  const off = await ev(`window.__st[${PLAYING}]`);
  ok(off === 0, "online: it boots stopped");
  await click('[aria-label="PLAY"]');
  await until(`window.__st[${PLAYING}] === 1`, 3000, "PLAY to reach the firmware");
  await click('[aria-label="STOP"]');
  await until(`window.__st[${PLAYING}] === 0`, 3000, "STOP to reach the firmware");
  const bpm = await ev(`window.__st[${BPM}]`);
  await wheel('[title="T1"]', -40, 4);
  await until(`window.__st[${BPM}] > ${bpm}`, 3000, "a knob on the panel to turn the tempo");
  // a knob has stops: dragged far past the end its pointer stays there, the wheel goes no further, and a short drag covers the whole range
  await knobStops();
  await knobLists();
  await trackerKeys();
  ok(await ev(`(() => { const c = document.querySelector("#screen").getContext("2d").getImageData(0, 0, 960, 540).data; const seen = new Set(); for (let i = 0; i < c.length; i += 4000) seen.add(c[i]); return seen.size > 3; })()`), "online: the firmware's screen is drawn");
  ok(problems.length === 0, "online: still clean after playing: " + problems.join(" | "));
  await viewport(390, 844, true); await goto("online.html"); await wide("online, phone");
  ok(await ev(`getComputedStyle(document.querySelector(".wide-note")).display`) === "block", "online, phone: it says it needs a wide screen");
}

// The tracker from a computer keyboard, with real key events: OK held with the arrows, which one pointer cannot do, and the piano
async function trackerKeys() {
  const key = (type, k, code, vk) => call("Input.dispatchKeyEvent", { type, key: k, code, windowsVirtualKeyCode: vk });
  const tap = async (k, code, vk) => { await key("keyDown", k, code, vk); await key("keyUp", k, code, vk); await sleep(60); };
  const letter = (c) => tap(c, "Key" + c.toUpperCase(), c.toUpperCase().charCodeAt(0));
  const st = (i) => ev(`window.__st[${i}]`);
  const ROW = 65, POPOVER = 66, NOTE = 39, VIEW = 43, CURSOR = 10, STEPS = 13, LANE = 2;
  await click('[aria-label="LANE"]');
  await click('[aria-label="layer 3"]');
  await click('[aria-label="STEP"]');
  for (let i = 0; i < 3 && await st(VIEW) !== 2; ++i) { await letter("s"); await sleep(200); }
  await until(`window.__st[${VIEW}] === 2`, 3000, "S to reach the tracker");
  await key("keyDown", "ArrowDown", "ArrowDown", 40); await key("keyUp", "ArrowDown", "ArrowDown", 40);
  await until(`window.__st[${ROW}] === 1`, 3000, "the down arrow to move the row");
  ok(await st(LANE) === 2, "tracker: the arrow moved a row, not a lane");
  await key("keyDown", "ArrowRight", "ArrowRight", 39); await key("keyUp", "ArrowRight", "ArrowRight", 39);
  await until(`window.__st[${LANE}] === 3`, 3000, "the right arrow to move a lane");
  await key("keyDown", "Enter", "Enter", 13);
  await sleep(120);
  ok(!(await st(POPOVER)), "tracker: OK does nothing until it comes up");
  await key("keyDown", "ArrowUp", "ArrowUp", 38); await key("keyUp", "ArrowUp", "ArrowUp", 38);
  await key("keyDown", "ArrowRight", "ArrowRight", 39); await key("keyUp", "ArrowRight", "ArrowRight", 39);
  await key("keyUp", "Enter", "Enter", 13);
  await until(`window.__st[${NOTE}] === 13`, 3000, "Enter held with the arrows to move the note an octave and a semitone: " + await st(NOTE));
  ok(await st(POPOVER) === 1, "tracker: the popover is open, and Enter coming up has not closed it");
  ok(await st(ROW) === 1 && await st(LANE) === 3, "tracker: and the cursor has stayed");
  // the piano: a key is a note and a row down. Lane 4's root is C, so A is its root, and F five semitones on
  await letter("a");
  await until(`window.__st[${ROW}] === 2`, 3000, "a piano key to go down a row");
  await sleep(120);
  await key("keyDown", "ArrowUp", "ArrowUp", 38); await key("keyUp", "ArrowUp", "ArrowUp", 38);
  await until(`window.__st[${ROW}] === 1 && window.__st[${NOTE}] === 0`, 3000, "A to write the root note");
  await letter("f");
  await until(`window.__st[${ROW}] === 2`, 3000, "and go down again");
  await key("keyDown", "ArrowUp", "ArrowUp", 38); await key("keyUp", "ArrowUp", "ArrowUp", 38);
  await until(`window.__st[${ROW}] === 1 && window.__st[${NOTE}] === 5`, 3000, "F to write the note five semitones up");
  await letter("x");
  await letter("g");
  await key("keyDown", "ArrowUp", "ArrowUp", 38); await key("keyUp", "ArrowUp", "ArrowUp", 38);
  await until(`window.__st[${ROW}] === 1 && window.__st[${NOTE}] === 19`, 3000, "X an octave up, G a fifth over it");
  await letter("x"); await letter("x");
  await letter("k");
  await sleep(150);
  ok(/two octaves/.test(await text("#panelHint")) && await st(ROW) === 1, "tracker: a note out of a step's range says so and stays: " + await text("#panelHint"));
  await letter("z"); await letter("z");
  await letter("z"); await letter("z");
  // a piano key held down repeats: one note, and it must not fall through to the panel key under the letter
  const rowNow = await st(ROW);
  await call("Input.dispatchKeyEvent", { type: "keyDown", key: "s", code: "KeyS", windowsVirtualKeyCode: 83 });
  for (let i = 0; i < 3; ++i) await call("Input.dispatchKeyEvent", { type: "keyDown", key: "s", code: "KeyS", windowsVirtualKeyCode: 83, autoRepeat: true });
  await call("Input.dispatchKeyEvent", { type: "keyUp", key: "s", code: "KeyS", windowsVirtualKeyCode: 83 });
  await until(`window.__st[${ROW}] === ${rowNow + 1}`, 3000, "one row for one held key");
  await sleep(200);
  ok(await st(VIEW) === 2 && await st(ROW) === rowNow + 1, "tracker: a held S is one note, and stays in the view (it is STEP elsewhere)");
  await key("keyDown", "ArrowUp", "ArrowUp", 38); await key("keyUp", "ArrowUp", "ArrowUp", 38);
  await until(`window.__st[${ROW}] === ${rowNow}`, 3000, "the row back");
  // an empty step takes a typed note, and the letters are the panel's again outside the view
  const steps = await st(STEPS), cursor = await st(CURSOR);
  ok(!!((steps >>> cursor) & 1), "tracker: the step typed on is on");
  await key("keyDown", "l", "KeyL", 76); await key("keyUp", "l", "KeyL", 76);
  await until(`window.__st[0] === 0`, 3000, "L to leave");
  ok(problems.length === 0, "tracker keys: the console is clean: " + problems.join(" | "));
}

// EXPORT in the browser: the panel's own keys, and what comes out of them
async function exportPanel() {
  await viewport(1400, 950);
  await goto("online.html");
  await frames();
  await click("#start");
  await until("S.started", 6000, "the audio to start");
  for (let i = 0; i < 3; ++i) { await click(plateSel("PROJECT")); await sleep(150); }
  await until(`window.__st[80] === 2`, 3000, "PROJECT's third view");
  const made = async (name, n, press = true) => {
    const was = await ev(`(S.exports || []).length`);
    if (press) await click(plateSel("OK"));
    await until(`(S.exports || []).length >= ${was + n}`, 60000, name);
    return (await ev(`JSON.parse(JSON.stringify(S.exports))`)).slice(was);
  };
  const wav = await made("a song to a WAV", 1);
  ok(wav[0].name === "Song 1.wav" && wav[0].size > 500000, "export: a WAV comes to the browser: " + JSON.stringify(wav));
  await wheel(plateSel("B2"), -30); await sleep(200);
  ok(await ev(`window.__st[82]`) === 1, "export: B2 goes to STEMS");
  const stems = await made("stems", 3);
  ok(stems.length >= 3 && stems.every((f) => /^Song 1 lane \d\d /.test(f.name) && f.name.endsWith(".wav") && f.size > 44), "export: a stem for each lane: " + stems.map((f) => f.name).join(", "));
  await wheel(plateSel("B2"), -30); await sleep(200);
  const midi = await made("a MIDI file", 1);
  ok(midi[0].name === "Song 1.mid" && midi[0].size > 100, "export: and a MIDI file: " + JSON.stringify(midi));
  ok(problems.length === 0, "export: the console is clean: " + problems.join(" | "));
}

async function tutorialPage() {
  await viewport(1400, 950);
  await goto("tutorial.html");
  await common("tutorial");
  await until(`document.querySelectorAll("#lesson ol button").length === ${LESSON_TITLES.length}`, 20000, "every lesson on the level select");
  ok(JSON.stringify(await ev(`[...document.querySelectorAll("#lesson ol button span:first-child")].map((s) => s.textContent)`)) === JSON.stringify(LESSON_TITLES.map((t, i) => `${i + 1}. ${t}`)), "tutorial: the lessons are numbered in order");
  await panelWiring();
  await lessonText();
  await plateIntact("tutorial");
  ok(await count("#demo, #sky") === 0, "tutorial: nothing moves behind the panel");
  await shot("tutorial_laptop");
  // a list of finished lessons from when there were ten follows the lessons, not their numbers
  await ev(`localStorage.setItem("drumboy.learn", "[0, 7]"); 1`);
  await goto("tutorial.html");
  await until(`document.querySelectorAll("#lesson ol button").length === ${LESSON_TITLES.length}`, 20000, "the level select again");
  const shown = await ev(`[...document.querySelectorAll("#lesson ol button")].map((b) => /done/.test(b.textContent))`);
  ok(shown[0] && shown[LESSON_TITLES.indexOf("Effects")] && shown.filter(Boolean).length === 2, "tutorial: lessons finished before the new ones were added stay finished: " + shown.join(" "));
  await ev(`localStorage.removeItem("drumboy.learn"); 1`);
  await goto("tutorial.html");
  await until(`document.querySelectorAll("#lesson ol button").length === ${LESSON_TITLES.length}`, 20000, "the level select once more");
  await slips();
  // every step of every lesson by hand, by the flashing control (Show me is web/test/learn.mjs): knobs by the wheel, then all of it again with knobs dragged
  for (const how of ["wheel", "drag"]) for (let i = 0; i < LESSON_TITLES.length; ++i) await lessonByHand(i, how);
  ok(await ev(`[...document.querySelectorAll("#lesson ol button")].every((b) => /done/.test(b.textContent))`), "tutorial: every lesson is ticked on the level select");
  ok(problems.length === 0, "tutorial: clean after the lessons: " + problems.join(" | "));
}

// What each key prints, as a lesson has to say it: the letter of a letter key (panel.js KEYS), and a word for a key with a picture. The transport's two are obvious after lesson 1.
const PRINTED = { "OSC 1": "sine", "OSC 2": "fork", SUB: "two dots", "FILTER 1": "curve", AMP: "target", MOD: "target", OK: "bars", UP: "+", DOWN: "minus", LEFT: "triangle", RIGHT: "triangle", ADD: "the dot", CLEAR: "small ring", SHIFT: "nine dots", FX: "ring with a line" };
// The keys as they sit on the plate, row by row, the left block and then the right, are the bits on the port expanders in order (Input.cpp); the left block's last bit is the FX key, drawn apart. Every other test goes by key name, so a print on the wrong key passes them. Both sides here are code: where a key is on the real plate comes from the board.
async function panelWiring() {
  const read = (f) => fs.readFileSync(new URL("../../firmware/src/" + f, import.meta.url), "utf8");
  const names = read("app/Ui.h").match(/enum class Key[^{]*\{([^}]*)\}/)[1].replace(/\/\/.*/g, "").split(",").map((k) => k.trim()).filter(Boolean);
  const table = (n) => read("board/Input.cpp").match(new RegExp(n + "\\[16\\] = \\{([^}]*)\\}"))[1].split(",").map((k) => k.trim().replace("Key::", ""));
  const wired = [...table("kLeftKeys").slice(0, 15), ...table("kRightKeys")];
  const plate = await ev(`[...KEYS].sort((a, b) => (a[1] > 1000) - (b[1] > 1000) || a[2] - b[2] || a[1] - b[1]).map((k) => ({ key: k[0], at: k[1] + "," + k[2], face: k[3], name: k[4] }))`);
  ok(wired.length === 31 && plate.length === 31, "panel: all 31 keys are compared");
  ok(new Set(plate.map((k) => k.at)).size === 31, "panel: no two keys in one place");
  wired.forEach((k, i) => ok(names[plate[i].key] === k, `panel: ${plate[i].name} is where the board has ${k}: the panel has ${names[plate[i].key]}`));
  const faces = await ev(`Object.fromEntries(["rec", "plus", "ringS", "minus"].map((g) => [g, glyph(g)]))`);
  for (const [face, key, name] of [["rec", "Add", "ADD"], ["plus", "Up", "UP"], ["ringS", "Clear", "CLEAR"], ["minus", "Down", "DOWN"]]) {
    const k = plate.find((p) => names[p.key] === key);
    ok(k.name === name && k.face === faces[face], `panel: ${key} prints ${face} and is called ${name}: it prints ${Object.keys(faces).find((f) => faces[f] === k.face)} and is called ${k.name}`);
  }
  const keyboard = await ev(`["+", "-", ".", "Backspace"].map((k) => KEYMAP[k]).join()`);
  ok(keyboard === "UP,DOWN,ADD,CLEAR", "panel: the computer keyboard's + and - are the printed ones: " + keyboard);
}

async function lessonText() {
  const lessons = await ev(`JSON.parse(JSON.stringify(LESSONS))`);
  const letters = Object.fromEntries((await ev(`KEYS.map((k) => [k[4], /class="ch/.test(k[3]) ? k[3].replace(/<[^>]+>/g, "") : ""])`)).filter((k) => k[1]));
  const panel = await ev(`Object.keys(window.plateKeys)`);
  ok(JSON.stringify(letters).includes('"PART":"D"') && letters.FILE === "F" && letters.MIXER === "R", "tutorial: the top right keys print F D R: " + JSON.stringify(letters));
  lessons.forEach((l, i) => {
    const seen = new Set(), name = `lesson ${i + 1}: `, words = [l.intro, l.outro, ...l.steps.flatMap((s) => [s.say, s.why])].join(" ");
    ok(!/cathode/i.test(words), name + "says Nemesis");
    ok(!/scroll|endless|for ever|forever|keep turning/i.test(words), name + "assumes no endless knob: " + words.match(/.{30}(scroll|endless|for ever|forever|keep turning).{30}/i)?.[0]);
    l.steps.forEach((s) => {
      for (const t of [].concat(s.target || [])) {
        ok(panel.includes(t), `${name}"${s.say}": ${t} is on the panel`);
        if (seen.has(t)) continue;
        seen.add(t);
        if (letters[t]) ok(s.say.includes(`the ${letters[t]} key`), `${name}"${s.say}" names ${t} by its letter, ${letters[t]}`);
        else if (PRINTED[t]) ok((s.say + s.why).includes(PRINTED[t]), `${name}"${s.say}" says what ${t} shows: ${PRINTED[t]}`);
      }
      for (const m of s.say.matchAll(/the ([A-Z]) key/g)) ok(letters[[].concat(s.target)[0]] === m[1], `${name}"${s.say}": the key that flashes prints ${m[1]}`);
    });
  });
}
// A person's slips in the two lessons on one lane: each must either leave the step waiting, or come back through Show me, and none may leave it stuck.
const dot = () => ev(`[...document.querySelectorAll("#lesson .dots i")].findIndex((d) => d.classList.contains("now"))`);
const stepState = () => ev(`Learn.state()`);
async function openAt(i, k) {
  await goto("tutorial.html");
  await until(`document.querySelectorAll("#lesson ol button").length === ${LESSON_TITLES.length}`, 20000, "the level select");
  await click(`#lesson ol li:nth-child(${i + 1}) button`);
  await until(`!!document.querySelector("#go")`, 15000, "the lesson to open");
  await click("#go");
  await until(`!!document.querySelector("#showMe")`, 5000, "the first step");
  for (let n = 0; n < k; ++n) { await click("#showMe"); await until(`[...document.querySelectorAll("#lesson .dots i")].findIndex((d) => d.classList.contains("now")) === ${n + 1}`, 8000, `Show me to reach step ${n + 2}`); await sleep(120); }
}
async function comesBack(k, what) {
  for (let n = 0; n < 8 && await dot() === k; ++n) {
    await click("#showMe");
    await sleep(1100);
    await until(`!document.querySelector("#showMe")?.disabled`, 20000, what + ": Show me to finish");   // (a knob at the far end is a long way back)
  }
  ok(await dot() > k, what + ": Show me comes back and finishes the step: " + JSON.stringify(await stepState()));
}
async function slips() {
  const SLICES = LESSON_TITLES.indexOf("Slices"), LOOPS = LESSON_TITLES.indexOf("Sample loops");
  // a synth lane has no such pages
  await openAt(LOOPS, 4);
  await click('[aria-label="layer 2"]');
  await until(`!!document.querySelector("#lesson .hint")`, 3000, "a hint after a lane key on a sampler page");
  await comesBack(4, "lesson 9, a lane key on the SAMPLE page");
  await openAt(SLICES, 13);
  await click('[aria-label="PART"]');
  await sleep(300);
  await comesBack(13, "lesson 8, PART on the SLICE page");
  // the wrong lane at the start is not carried into the lesson
  await openAt(SLICES, 0);
  await click('[aria-label="layer 1"]');
  await sleep(200);
  await click('[aria-label="PLAY"]');
  await sleep(500);
  ok(await dot() === 0, "lesson 8: PLAY on the wrong lane does not pass the step");
  await comesBack(0, "lesson 8, the wrong lane at the start");
  // the editor's lane key wraps round
  await openAt(SLICES, 6);
  await click('[aria-label="DOWN"]');
  await sleep(300);
  await comesBack(6, "lesson 8, DOWN from lane 10 in the editor");
  ok((await stepState()).lane === 9, "and it is back on lane 10");
  // a pattern changed on the home screen
  await openAt(SLICES, 1);
  await wheel('[title="T4"]', -30);
  await sleep(300);
  await comesBack(1, "lesson 8, the PATTERN knob on the home screen");
  ok((await stepState()).pattern === 0, "and it is back on the lesson's pattern");
  // the cursor, in the two steps that work on step 6
  await openAt(SLICES, 10);
  await click('[aria-label="LEFT"]');
  await sleep(300);
  await comesBack(10, "lesson 8, the cursor moved off step 6");
  // a knob past the value: Show me turns back
  await openAt(SLICES, 2);
  await drag('[title="T1"]', -160);
  await comesBack(2, "lesson 8, MODE dragged past STEP");
  await openAt(LOOPS, 2);
  await drag('[title="T2"]', -160);
  await comesBack(2, "lesson 9, START dragged to the top");
  // leaving the page does not pass a step that reads its knobs
  await openAt(SLICES, 14);
  await click('[aria-label="STEP"]');
  await sleep(500);
  ok(await dot() === 14, "lesson 8: leaving the SLICE page does not pass \"all the way down\"");
  await comesBack(14, "lesson 8, away from the SLICE page");
  await openAt(LOOPS, 7);
  await click('[aria-label="LANE"]');
  await sleep(500);
  ok(await dot() === 7, "lesson 9: leaving the LOOP page does not pass \"all the way down\"");
  await comesBack(7, "lesson 9, away from the LOOP page");
  // a key the lesson asks for is not a wrong press, by a short press too
  await openAt(SLICES, 16);
  await click('[aria-label="LANE"]');
  await sleep(300);
  ok(!await ev(`!!document.querySelector("#lesson .hint")`), "lesson 8: a short press of the L key raises no warning");
  await comesBack(16, "lesson 8, a short press where a hold was asked for");
  // SHIFT clicked: Show me must not turn into undo
  await openAt(SLICES, 5);
  await click('[aria-label="SHIFT"]');
  await sleep(100);
  await comesBack(5, "lesson 8, SHIFT left down");
  ok((await stepState()).sliceCount === 8 && !(await stepState()).shift, "SHIFT left down: Show me did not undo the cut");
  // and it does not outlive the firmware it was pressed on
  await click('[aria-label="SHIFT"]');
  await sleep(100);
  await click("#again");
  await until(`!!document.querySelector("#go")`, 15000, "Start over");
  ok(!await ev(`document.querySelector('[aria-label="SHIFT"]').classList.contains("down")`), "Start over lets go of a clicked SHIFT");
  // what no press can mend says so: the lane shortened on the home screen, so step 6 is gone. Show me gets as far as it can, then says Start over
  await openAt(SLICES, 1);
  await wheel('[title="B3"]', 30, 3);
  await sleep(300);
  for (let n = 0; n < 12 && await dot() < 8; ++n) { await click("#showMe"); await sleep(1100); }
  ok(await dot() === 8, "a lane shortened by mistake stops at the step it cannot do: " + await dot());
  await click("#showMe");
  await sleep(1300);
  ok(/Start over/.test(await text("#lesson .hint")), "and the lesson says what to do");
  ok(problems.length === 0, "tutorial: clean after the slips: " + problems.join(" | "));
  await goto("tutorial.html");
  await until(`document.querySelectorAll("#lesson ol button").length === ${LESSON_TITLES.length}`, 20000, "the level select");
}
const plateSel = (n) => (n.startsWith("LAYER ") ? `[aria-label="layer ${n.slice(6)}"]` : /^[TB][1-4]$/.test(n) ? `[title="${n}"]` : `[aria-label="${n}"]`);
const whereInLesson = () => ev(`(document.querySelector("#lesson .kicker")?.textContent || "") + "/" + [...document.querySelectorAll("#lesson .dots i")].findIndex((d) => d.classList.contains("now"))`);
// a person's drag: down on the knob, along until the value is right, wait, let go. The lesson must wait for the release.
async function dragKnob(sel, i, k, dir) {
  const [x, y] = await centre(sel);
  await ev(`window.__at = Learn.state(); 1`);
  const before = await whereInLesson();
  await call("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
  await call("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", buttons: 1, clickCount: 1, pointerType: "mouse" });
  let py = 0, there = false;
  for (let n = 0; n < 120 && !there; ++n) {
    py += 4 * dir;
    await call("Input.dispatchMouseEvent", { type: "mouseMoved", x, y: y + py, buttons: 1, button: "left" });
    await sleep(50);
    there = await ev(`!!LESSONS[${i}].steps[${k}].done(Learn.state(), window.__at)`);
  }
  await sleep(250);
  ok(before === await whereInLesson(), `lesson ${i + 1} step ${k + 1}: waits for the knob to be let go`);
  await call("Input.dispatchMouseEvent", { type: "mouseReleased", x, y: y + py, button: "left", clickCount: 1, pointerType: "mouse" });
  await sleep(30);
}
async function lessonByHand(i, how) {
  const name = `lesson ${i + 1} by ${how}`, title = LESSON_TITLES[i];
  await click(`#lesson ol li:nth-child(${i + 1}) button`);
  await until(`!!document.querySelector("#go")`, 15000, name + " to open");
  const project = await ev(`JSON.parse(JSON.stringify(LESSONS[${i}].project || null))`);
  if (title === "Slices") ok(await ev(`Learn.state().sample === 11 && Learn.state().lane === 9 && Learn.state().length === 8 && Learn.state().hits === 1 && Learn.state().mode === 0 && Learn.state().part === 2`), name + ": its project: lane 10, FX Sweep, eight steps, one hit: " + JSON.stringify(await ev(`Learn.state()`)));
  if (title === "Sample loops") ok(await ev(`Learn.state().sample === 9 && Learn.state().lane === 9 && Learn.state().hits === 4 && Learn.state().mode === 0`), name + ": its project: lane 10, Tom Low, on every beat: " + JSON.stringify(await ev(`Learn.state()`)));
  if (title === "Recording") ok(await ev(`Learn.state().lane === 9 && Learn.state().hits === 1 && Learn.state().laneSampler === 1 && Learn.state().mode === 0`), name + ": its project: lane 10, on the sampler, one hit a bar: " + JSON.stringify(await ev(`Learn.state()`)));
  ok(!!project === (title === "Slices" || title === "Sample loops" || title === "Recording"), name + ": a starting project where it needs one");
  if (title === "Tracker") await until(`!!Learn.state()`, 5000, "the first state of the fresh Drumboy");
  if (title === "Tracker") ok(await ev(`Learn.state().lane === 0 && !((Learn.state().steps >>> 1) & 1)`), name + ": lane 1's step 2 is empty, as the lesson says");
  await click("#go");
  for (let k = 0; ; ++k) {
    await until(`!!document.querySelector("#lesson .dots") || /done/.test(document.querySelector("#lesson .kicker")?.textContent || "")`, 8000, `${name}: step ${k + 1}`);
    if (/done/.test(await text("#lesson .kicker"))) break;
    const s = await ev(`JSON.parse(JSON.stringify(LESSONS[${i}].steps[${k}]))`);
    const where = await whereInLesson();
    const moved = async () => (await whereInLesson()) !== where;
    const targets = [].concat(s.target || []).map(plateSel);
    ok(await whereInLesson() === `Lesson ${i + 1}: ${title}/${k}`, `${name}: step ${k + 1} is where it should be: ${where}`);
    for (const t of targets) ok(await ev(`document.querySelector(${JSON.stringify(t)}).classList.contains("coach")`), `${name}: step ${k + 1}: ${t} flashes`);
    if (i === 0 && k === 1 && how === "wheel") await shot("tutorial_step");
    if (s.pause) await sleep(s.pause);
    if (s.next) await click("#nextStep");
    else if (s.wait) ok(targets.length === 0, `${name}: step ${k + 1} asks for nothing to be pressed`);
    else if (s.chord) { await click(targets[0]); await click(targets[1]); }
    else if (s.twice) { await click(targets[0]); await click(targets[0]); }
    else if (s.repeat) for (let n = 0; n < 8 && !await moved(); ++n) {
      await click(targets[0]); await sleep(300);
      if (!await moved()) ok(!await ev(`!!document.querySelector("#lesson .hint")`), `${name}: step ${k + 1}: the key it asks for raises no warning`);
    }
    else if (s.turn && how === "drag") await dragKnob(targets[0], i, k, s.turn > 0 ? -1 : 1);
    else if (s.turn) for (let n = 0; n < Math.abs(s.turn) && !await moved(); ++n) { await wheel(targets[0], s.turn > 0 ? -30 : 30); await sleep(50); }
    else await click(targets[0], s.hold || 30);
    const t0 = Date.now();
    while (!await moved() && Date.now() - t0 < 8000) await sleep(30);   // (a pattern change waits for the end of the pattern)
    if (!await moved()) { ok(false, `${name}: step ${k + 1} did not finish: "${s.say}": ${JSON.stringify(await ev(`Learn.state()`))}`); await click("#leave"); await until(`!!document.querySelector("#lesson ol")`, 3000, "the level select"); return; }
  }
  ok(new RegExp(`Lesson ${i + 1} done`).test(await text("#lesson .kicker")), name + " finishes with real pointer events");
  await click("#leave");
  await until(`document.querySelectorAll("#lesson ol button").length === ${LESSON_TITLES.length}`, 3000, "the level select after " + name);
}

// the old page, moved: the preset list and the panel on its tab still work on the same app.js
async function benchPage() {
  await viewport(1400, 950);
  await goto("bench.html");
  await until("typeof S !== 'undefined' && S.ready && document.querySelectorAll('.row-p').length >= 197", 20000, "the bench's preset list");
  await click(".row-p:nth-child(3)");
  await until(`S.sel === 2`, 3000, "the preset that was clicked to be selected");
  await click('.tab[data-tab="panel"]');
  await frames();
  await click('[aria-label="PLAY"]');
  await until(`window.__st[${PLAYING}] === 1`, 3000, "PLAY on the bench's panel");
  ok(problems.length === 0, "bench: the console is clean: " + problems.join(" | "));
}

// ---- the install page
async function installPage() {
  await goto("install.html");
  const t = await ev("document.body.innerText");
  for (const w of ["Beta 2", "no warranty", "Windows", "macOS", "backup", "Going back to the original firmware"]) ok(t.toLowerCase().includes(w.toLowerCase()), "install: mentions " + w);
  ok(t.indexOf("no warranty") < t.toLowerCase().indexOf("installing on windows"), "install: the disclaimer comes before the steps");
  ok(await ev(`fetch(document.querySelector("#download").href, { method: "HEAD" }).then((r) => r.status === 200)`), "install: the download is there");
  await common("install");
}

// the manual: the menu marks it, every link inside it lands somewhere, every image and the PDF are there
async function manualPage() {
  await goto("manual.html");
  ok(await ev(`document.querySelector('#bar nav a[aria-current="page"]')?.getAttribute("href")`) === "manual.html", "manual: the menu marks it as the current page");
  ok(await text("h1") === "Manual", "manual: it has its title");
  const lost = await ev(`[...document.querySelectorAll('a[href^="#"]')].filter((a) => !document.getElementById(a.getAttribute("href").slice(1))).map((a) => a.getAttribute("href"))`);
  ok(await ev(`document.querySelectorAll('a[href^="#"]').length`) > 40, "manual: the contents and the cross references are links");
  ok(lost.length === 0, "manual: every link in the page has a target: " + lost.join(" "));
  const ids = await ev(`[...document.querySelectorAll("[id]")].map((e) => e.id)`);
  ok(new Set(ids).size === ids.length, "manual: no id is used twice");
  const broken = await ev(`Promise.all([...document.querySelectorAll("img")].map((i) => { i.loading = "eager"; return i.decode().then(() => null, () => i.getAttribute("src")); })).then((r) => r.filter(Boolean).concat([...document.querySelectorAll("img")].filter((i) => !i.naturalWidth).map((i) => i.getAttribute("src"))))`);
  ok(await ev(`document.images.length`) > 30, "manual: it has its screens");
  ok(broken.length === 0, "manual: every image loads: " + broken.join(" "));
  ok(await ev(`[...document.querySelectorAll("svg image")].length`) >= 1 && await ev(`fetch(document.querySelector("svg image").href.baseVal, { method: "HEAD" }).then((r) => r.ok)`), "manual: the panel drawing has its screen");
  ok(await ev(`fetch(document.querySelector("#pdf").href, { method: "HEAD" }).then((r) => r.status === 200)`), "manual: the PDF is there");
  const t = await ev("document.body.innerText");
  for (const w of ["no warranty", "Windows", "macOS", "F1 DEMO", "not yet been proven"]) ok(t.toLowerCase().includes(w.toLowerCase()), "manual: mentions " + w);
  ok(t.indexOf("no warranty") < t.toLowerCase().indexOf("installing on windows"), "manual: the disclaimer comes before the steps");
  await wide("manual, wide");
  await shot("manual_top");
  await ev(`document.documentElement.style.scrollBehavior = "auto"; document.querySelector("#tracker").scrollIntoView(); 1`);
  await sleep(300);
  await shot("manual_chapter");
  await viewport(390, 844, true);
  await sleep(200);
  await wide("manual, phone");
  await shot("manual_phone");
  await viewport(1400, 950);
  await common("manual");
}

// ---- the librarian
const status = () => text("#status");
const log = () => text("#log");
const count = (sel) => ev(`document.querySelectorAll(${JSON.stringify(sel)}).length`);
async function tick(list, i) { await click(`#${list} tr:nth-child(${i}) input`); }
async function ticked(list) { return ev(`[...document.querySelectorAll("#${list} input:checked")].length`); }
async function setFiles(paths) {
  const { root: doc } = await call("DOM.getDocument");
  const { nodeId } = await call("DOM.querySelector", { nodeId: doc.nodeId, selector: "#files" });
  await call("DOM.setFileInputFiles", { nodeId, files: paths });
  await sleep(200);
}
async function librarianPage() {
  await viewport(1400, 950);
  const honest = async () => ok(/not yet proven/.test(await text("#honest")), "librarian: says sending and deleting are not yet proven");
  // no Web MIDI, refused, no ports, a port nothing answers on, and fake ports with the simulated device behind them
  const midi = async (script, name, expect) => {
    const { identifier } = await call("Page.addScriptToEvaluateOnNewDocument", { source: script });
    await goto("librarian.html");
    await click("#useMidi");
    await until(`document.querySelector("#status").textContent !== "Not connected."`);
    const s = await status();
    ok(expect.test(s), `librarian: ${name}: ${s}`);
    await call("Page.removeScriptToEvaluateOnNewDocument", { identifier });
    return s;
  };
  await midi("Object.defineProperty(Navigator.prototype, 'requestMIDIAccess', { value: undefined, configurable: true });", "a browser with no Web MIDI", /has no Web MIDI/);
  await shot("librarian_nomidi");
  await midi("Navigator.prototype.requestMIDIAccess = () => Promise.reject(new DOMException('no', 'SecurityError'));", "permission refused", /not allowed to use MIDI/);
  await midi("Navigator.prototype.requestMIDIAccess = async () => ({ inputs: new Map(), outputs: new Map() });", "no device", /No MIDI device found/);
  await midi("Navigator.prototype.requestMIDIAccess = async () => { throw new Error('x'); };", "a refusal of another kind", /not allowed to use MIDI/);
  const fake = (wired) => `Navigator.prototype.requestMIDIAccess = async () => {
    const input = { id: "i", name: "Fake in", state: "connected", onmidimessage: null }, output = { id: "o", name: "Fake out", state: "connected", sent: [], send(b) { this.sent.push(b); window.__midiOut(Uint8Array.from(b)); } };
    window.__midiOut = () => {};
    ${wired ? `const dev = new SimDevice([], 20); window.__dev = dev; dev.onsend = (b) => !window.__deaf && input.onmidimessage && input.onmidimessage({ data: b }); window.__midiOut = (b) => { if (window.__dropCmd === b[5]) window.__deaf = true; else if (!(window.__dropList && b[5] === 2)) dev.receive(b); };` : ""}
    return (window.__access = { inputs: new Map([["i", input]]), outputs: new Map([["o", output]]) });
  };`;
  const unplug = `(window.__access.inputs.clear(), window.__access.outputs.clear(), window.__access.onstatechange({ port: { id: "o", state: "disconnected" } }), 1)`;
  await midi(fake(false), "a silent port: asks for the ports first", /Choose the ports/);
  await click("#tryPorts");
  await until(`/Nothing answered/.test(document.querySelector("#status").textContent)`, 3000, "the no-answer message");
  ok(/Check the cable/.test(await status()), "librarian: a silent device: says what to check");
  ok(await ev(`document.querySelector("#refresh").disabled`), "librarian: a silent device stays not connected");
  await midi(fake(true), "ports with the simulation behind them", /Choose the ports/);
  await click("#tryPorts");
  await until(`/Connected to/.test(document.querySelector("#status").textContent)`, 3000, "identify through the Web MIDI path");
  await until(`document.querySelectorAll("#devList tr").length === 3`, 3000, "the list through the Web MIDI path");
  // upload and delete through the same ports, then the interface goes away while a question is open
  const someFile = path.join(root, "content/synthwave", fs.readdirSync(path.join(root, "content/synthwave")).find((f) => f.endsWith(".PRS")));
  await setFiles([someFile]);
  await tick("pcList", 1);
  await click("#uploadSel");
  await until(`document.querySelector("#ask").open`, 2000, "the question over the MIDI path");
  ok(/the device/.test(await text("#askTitle")) && !/simulated/.test(await text("#askTitle")), "librarian: over MIDI, the question says the device");
  await click("#askGo");
  await until(`document.querySelectorAll("#devList tr").length === 4`, 4000, "an upload over the MIDI path");
  await tick("devList", 4);
  await click("#deleteSel");
  await until(`document.querySelector("#ask").open`, 2000, "the delete question over the MIDI path");
  await click("#askGo");
  await until(`document.querySelectorAll("#devList tr").length === 3`, 4000, "a delete over the MIDI path");
  await click("#uploadSel");
  await until(`document.querySelector("#ask").open`, 2000, "a question to unplug under");
  await ev(unplug);
  await sleep(200);
  ok(!await ev(`document.querySelector("#ask").open`), "librarian: unplugged with a question open: the question closes");
  ok(/unplugged/.test(await status()) && /Nothing was sent/.test(await log()), "librarian: and it says so, and sends nothing: " + await status());
  ok(await ev(`document.querySelector("#refresh").disabled && document.querySelector("#uploadSel").disabled`), "librarian: unplugged: nothing can be sent");
  ok(!problems.some((p) => /Cannot/.test(p)), "librarian: unplugged: no error in the page: " + problems.join(" | "));
  // unplugged in the middle of a job: the message stays
  await midi(fake(true), "ports again", /Choose the ports/);
  await click("#tryPorts");
  await until(`document.querySelectorAll("#devList tr").length === 3`, 3000, "the list again");
  await ev(`window.__dropList = true; 1`);
  await click("#refresh");
  await sleep(100);
  await ev(unplug);
  await until(`/unplugged/.test(document.querySelector("#status").textContent)`, 2000, "the unplugged message");
  await sleep(1300);
  ok(/unplugged/.test(await status()), "librarian: unplugged during a job: the message stays: " + await status());
  // the editor over the same ports: a send whose last message gets no answer, and one that loses the device there. Either way it may have landed, and the edit stays
  await midi(fake(true), "ports for the editor", /Choose the ports/);
  await click("#tryPorts");
  await until(`document.querySelectorAll("#devList tr").length === 3`, 3000, "the list for the editor");
  await click("#edNew");
  await calm();
  await typeName("Unanswered");
  await ev(`window.__dropCmd = 5; 1`);   // (from UPLOAD_COMMIT on the cable is silent)
  await click("#edSend");
  await until(`document.querySelector("#ask").open`, 3000, "the question over the MIDI path");
  await click("#askGo");
  await until(`/Sending stopped/.test(document.querySelector("#status").textContent)`, 35000, "the unanswered send");   // (an answer that has to reach the card may take the whole-card check: 30 s)
  ok(/no answer/.test(await status()) && /may have gone through/.test(await status()) && /still here, so you can try again/.test(await status()), "editor: a send with no last answer says it may have landed: " + await status());
  await until(`!document.querySelector("#refresh").disabled`, 3000, "the page to be usable");
  await ev(`window.__deaf = false; 1`);
  await click("#edSend");
  await until(`document.querySelector("#ask").open`, 3000, "the question again");
  await click("#askGo");
  await sleep(400);
  await ev(unplug);
  await until(`/Sending stopped/.test(document.querySelector("#status").textContent)`, 3000, "the send that lost the device");
  ok(/unplugged\. It may have gone through/.test(await status()) && !/\.\./.test(await status()) && !/try again/.test(await status()), "editor: a send that loses the device says so: " + await status());
  ok(await ev(`document.querySelector("#edName").value`) === "Unanswered" && await shown("#edDirty"), "editor: and the edit stays");
  await ev(`document.querySelector("#edUndo").click(); 1`);
  await until(`document.querySelector("#ask").open`, 2000, "the question before undo");
  await click("#askGo");
  await sleep(100);

  // the ports vanish before anyone asks: the row goes, in words
  await midi(fake(false), "ports that will go", /Choose the ports/);
  await ev(unplug);
  await sleep(100);
  ok(/No MIDI device found/.test(await status()) && await ev(`document.querySelector("#ports").hidden`), "librarian: ports gone before asking: " + await status());
  // the device answers who it is, then not the list: not left half connected
  await midi(fake(true), "ports for a list that fails", /Choose the ports/);
  await ev(`window.__dropList = true; 1`);
  await click("#tryPorts");
  await until(`/stopped/.test(document.querySelector("#status").textContent)`, 3000, "a list failure after identify");
  ok(await ev(`document.querySelector("#refresh").disabled`), "librarian: a list failure after identify leaves it not connected");

  // the simulated device, from the top
  await goto("librarian.html");
  await common("librarian");
  await honest();
  ok(await ev(`document.querySelector("#deleteSel").disabled && document.querySelector("#uploadSel").disabled`), "librarian: nothing can be sent before connecting");
  ok(await ev(`getComputedStyle(document.querySelector("#ports")).display`) === "none", "librarian: the port row is hidden until there are ports");
  await click("#useSim");
  await until(`/Connected to the simulated device/.test(document.querySelector("#status").textContent)`, 3000, "the simulated device");
  await until(`document.querySelectorAll("#devList tr").length === 27`, 4000, "the simulated device's list");
  // pressing Connect in a browser with no Web MIDI must not throw away the simulation
  await ev(`Object.defineProperty(navigator, "requestMIDIAccess", { value: undefined, configurable: true }); 1`);
  await click("#useMidi");
  await until(`/has no Web MIDI/.test(document.querySelector("#status").textContent)`, 2000, "the no Web MIDI message");
  ok(await ev(`document.querySelectorAll("#devList tr").length`) === 27, "librarian: no Web MIDI leaves the simulated device connected");
  ok(/simulated/i.test(await text("#devTitle")), "librarian: the device window says simulated");
  ok(await ev(`[...document.querySelectorAll("#devList tr:nth-child(1) input")][0].disabled`), "librarian: a built-in preset cannot be selected");
  await shot("librarian_laptop");

  // backup: one preset as a .PRS, two as a zip, and the bytes are the device's
  const dl = path.join(tmp, "dl");
  fs.mkdirSync(dl);
  await call("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: dl });
  await tick("devList", 4); await tick("devList", 5);
  ok(await ticked("devList") === 2, "librarian: two presets selected");
  await click("#backupSel");
  await until(`/Handed 2 presets/.test(document.querySelector("#status").textContent)`, 4000, "the backup");
  await sleep(400);
  const zips = fs.readdirSync(dl).filter((f) => f.endsWith(".zip"));
  ok(zips.length === 1, "librarian: a backup of two is one zip: " + fs.readdirSync(dl).join(" "));
  await tick("devList", 5);
  await click("#backupSel");
  await until(`/Handed 1 preset to/.test(document.querySelector("#status").textContent)`, 4000, "a backup of one");
  await sleep(400);
  const prs = fs.readdirSync(dl).filter((f) => f.endsWith(".PRS"));
  ok(prs.length === 1 && fs.statSync(path.join(dl, prs[0])).size === 148, "librarian: a backup of one is its 148-byte file: " + prs.join(" "));
  await tick("devList", 4);

  // load from the computer: a good file, a bad one, a zip of this page's
  const good = path.join(root, "content/synthwave", fs.readdirSync(path.join(root, "content/synthwave")).find((f) => f.endsWith(".PRS")));
  const damaged = path.join(tmp, "damaged.PRS");
  const b = fs.readFileSync(good); b[50] ^= 1; fs.writeFileSync(damaged, b);
  const hostile = path.join(tmp, "hostile.zip");
  const hz = Buffer.alloc(64); hz.writeUInt32LE(0x04034B50, 0); hz.writeUInt32LE(0xFFFFFFFF, 18); hz.writeUInt16LE(4, 26);
  fs.writeFileSync(hostile, hz);
  await setFiles([good, damaged, path.join(dl, zips[0]), hostile]);
  await until(`document.querySelectorAll("#pcList tr").length === 5`, 3000, "the loaded files");
  ok(/hostile\.zip: this zip is cut off/.test(await text("#pcList")), "librarian: a zip that lies about its size is refused, in words");
  ok(/damaged/.test(await text("#pcList")), "librarian: a damaged file is named, with the reason");
  ok(await count("#pcList input") === 3, "librarian: a damaged file cannot be selected");

  // upload needs a yes: Cancel sends nothing, and it is the one with the keyboard
  const devCount = () => ev(`document.querySelectorAll("#devList tr").length`);
  await tick("pcList", 1);
  await click("#uploadSel");
  await until(`document.querySelector("#ask").open`, 2000, "the question before an upload");
  ok(await ev(`document.activeElement.id`) === "askNo", "librarian: Cancel has the keyboard");
  ok(/simulated device/.test(await text("#askTitle")) && /simulated device/.test(await text("#askText")), "librarian: the question says it is the simulated device");
  await shot("librarian_ask");
  await click("#askNo");
  await sleep(300);
  ok(await devCount() === 27 && /Nothing was sent/.test(await log()), "librarian: Cancel sends nothing");
  await click("#uploadSel");
  await until(`document.querySelector("#ask").open`, 2000, "the question again, for Esc");
  await call("Input.dispatchKeyEvent", { type: "keyDown", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await call("Input.dispatchKeyEvent", { type: "keyUp", key: "Escape", code: "Escape", windowsVirtualKeyCode: 27 });
  await until(`!document.querySelector("#ask").open`, 1000, "Esc to close the question");
  await sleep(200);
  ok(await devCount() === 27, "librarian: Esc sends nothing");
  await click("#uploadSel");
  await until(`document.querySelector("#ask").open`, 2000, "the question again");
  await click("#askGo");
  await until(`/Sent 1 preset to the simulated device/.test(document.querySelector("#status").textContent)`, 4000, "the upload");
  await until(`document.querySelectorAll("#devList tr").length === 28`, 4000, "the new preset in the list");

  // a failed upload: the bytes change on the way, the device refuses, nothing changes, and the next try works
  await ev(`(() => { const s = document.querySelector("#fault"); s.value = "corrupt-chunk"; s.dispatchEvent(new Event("change")); })()`);
  await click("#uploadSel");
  await until(`document.querySelector("#ask").open`, 2000, "the question");
  ok(/replaces/.test(await text("#askList")), "librarian: the question says which preset is replaced");
  await click("#askGo");
  await until(`/Sending stopped/.test(document.querySelector("#status").textContent)`, 4000, "the failed upload to be reported");
  ok(/checksum/.test(await status()) && /0 of 1/.test(await status()), "librarian: a failed upload says what happened: " + await status());
  await until(`!document.querySelector("#refresh").disabled`, 3000, "the page to be usable again");
  ok(await devCount() === 28, "librarian: a failed upload changes nothing");
  // ... by the bytes: the preset it tried to replace comes back as it was
  const id = b.readUInt16LE(6);
  await click(`#devList input[value="${id}"]`);
  await click("#backupSel");
  await until(`/Handed 1 preset to/.test(document.querySelector("#status").textContent) && !document.querySelector("#backupSel").disabled`, 4000, "a backup of the preset");
  await sleep(400);
  const back = fs.readdirSync(dl).find((f) => f.startsWith(String(id).padStart(4, "0")) && f.endsWith(".PRS"));
  ok(back && fs.readFileSync(path.join(dl, back)).equals(fs.readFileSync(good)), "librarian: a failed replace leaves the old preset byte for byte");
  await click(`#devList input[value="${id}"]`);
  await shot("librarian_failed");

  // failed backups: nothing is saved
  const saved = () => fs.readdirSync(dl).filter((f) => /\.(zip|PRS)$/.test(f)).sort().join(" | ");   // (not the folder's count: Chrome's own temporary files come and go in it)
  const before = saved();
  await tick("devList", 4);
  for (const [fault, want] of [["corrupt-chunk", /checksum/], ["bad-sum", /message check/], ["no-reply", /no answer/]]) {
    await ev(`(() => { const s = document.querySelector("#fault"); s.value = ${JSON.stringify(fault)}; s.dispatchEvent(new Event("change")); })()`);
    await click("#backupSel");
    await until(`/backup stopped/.test(document.querySelector("#status").textContent)`, 4000, "a failed backup to be reported");
    ok(want.test(await status()), `librarian: backup with ${fault}: ${await status()}`);
    await ev(`document.querySelector("#status").textContent = ""`);
    await until(`!document.querySelector("#backupSel").disabled`, 3000, "the page to be usable again");
    await sleep(100);
  }
  await sleep(500);
  ok(saved() === before, `librarian: a failed backup saves nothing: was [${before}], now [${saved()}], folder [${fs.readdirSync(dl).join(" | ")}]`);

  // delete needs a yes too
  await click("#deleteSel");
  await until(`document.querySelector("#ask").open`, 2000, "the question before a delete");
  ok(await ev(`document.activeElement.id`) === "askNo", "librarian: Cancel has the keyboard for a delete too");
  await click("#askNo");
  await sleep(300);
  ok(await devCount() === 28 && /Nothing was deleted/.test(await log()), "librarian: Cancel deletes nothing");
  await click("#deleteSel");
  await until(`document.querySelector("#ask").open`, 2000, "the question");
  await click("#askGo");
  await until(`document.querySelectorAll("#devList tr").length === 27`, 4000, "the delete");
  ok(/Deleted 1 preset from the simulated device/.test(await log()), "librarian: the delete is reported");
  ok(problems.length === 0, "librarian: the console is clean at the end: " + problems.join(" | "));

  await viewport(1366, 768); await goto("librarian.html"); await wide("librarian, laptop");
  await viewport(390, 844, true); await goto("librarian.html"); await click("#useSim");
  await until(`document.querySelectorAll("#devList tr").length === 27`, 4000, "the list on a phone");
  await wide("librarian, phone"); await shot("librarian_phone");
  ok(await ev(`["#devList tr:nth-child(4) td:nth-child(2)", "#devList tr:nth-child(4) td:nth-child(4)"].every((s) => document.querySelector(s).getClientRects()[0].height < 36)`), "librarian, phone: the number and the type stay on one line");
}

// ---- the preset editor, with the pointer and the keyboard. The bytes are web/test/preset-edit.mjs; this is the page: what can be reached, what it says, and what comes out of it.
const edStatus = () => text("#edStatus");
const shown = (sel) => ev(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); return !!e && e.getClientRects().length > 0; })()`);
const pick = async (sel, value) => { await centre(sel); await ev(`(() => { const s = document.querySelector(${JSON.stringify(sel)}); s.value = ${JSON.stringify(String(value))}; s.dispatchEvent(new Event("change", { bubbles: true })); })()`); };   // (a select's list is the system's own, out of reach of the page's pointer)
const fault = (name) => ev(`(() => { const s = document.querySelector("#fault"); s.value = ${JSON.stringify(name)}; s.dispatchEvent(new Event("change")); })()`);
// a press on a slider's track, at this fraction of its width, then a drag to another: the browser's own slider does the rest
async function slide(sel, from, to) {
  const r = await ev(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); e.scrollIntoView({ block: "center", behavior: "instant" }); const b = e.getBoundingClientRect(); return [b.x, b.y + b.height / 2, b.width]; })()`);
  const [x0, y, w] = r, at = (f) => x0 + 8 + (w - 16) * f;
  await call("Input.dispatchMouseEvent", { type: "mouseMoved", x: at(from), y });
  await call("Input.dispatchMouseEvent", { type: "mousePressed", x: at(from), y, button: "left", buttons: 1, clickCount: 1, pointerType: "mouse" });
  for (let i = 1; i <= 6; ++i) { await call("Input.dispatchMouseEvent", { type: "mouseMoved", x: at(from + (to - from) * i / 6), y, buttons: 1, button: "left" }); await sleep(10); }
  await call("Input.dispatchMouseEvent", { type: "mouseReleased", x: at(to), y, button: "left", clickCount: 1, pointerType: "mouse" });
  await sleep(40);
}
async function key(name, code, vk) {
  await call("Input.dispatchKeyEvent", { type: "rawKeyDown", key: name, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk });
  await call("Input.dispatchKeyEvent", { type: "keyUp", key: name, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk });
  await sleep(40);
}
async function typeName(t) {
  await click("#edName");
  await ev(`document.querySelector("#edName").select(); 1`);
  await call("Input.insertText", { text: t });
  await sleep(40);
}
// the page glides to a preset that opens: nothing to press until it stops
const calm = () => ev(`new Promise((done) => { let y = -1, still = 0; const t = setInterval(() => { still = scrollY === y ? still + 1 : 0; y = scrollY; if (still > 4) { clearInterval(t); done(1); } }, 30); })`);
async function editorPage() {
  await viewport(1400, 950);
  await goto("librarian.html");
  const dl = path.join(tmp, "dl-editor");
  fs.mkdirSync(dl);
  await call("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: dl });
  const saved = () => fs.readdirSync(dl).filter((f) => f.endsWith(".PRS"));
  const file = async (name) => { for (let i = 0; i < 40 && !fs.existsSync(path.join(dl, name)); ++i) await sleep(50); await sleep(100); return fs.readFileSync(path.join(dl, name)); };
  const devCount = () => count("#devList tr");
  const accepted = (f) => spawnSync(path.join(root, "tests/build/preset_accept"), [f], { encoding: "utf8" }).status === 0 && spawnSync("python3", [path.join(root, "tools/prs.py"), "show", f], { encoding: "utf8" }).status === 0;
  ok(!await shown("#edBody"), "editor: nothing to edit until something is opened");
  ok(/Sending and deleting are not yet proven/i.test(await text("#honest")), "editor: the banner is left as it was");

  // a new one, with no device
  await click("#edNew");
  await until(`!document.querySelector("#edBody").hidden`, 1000, "a new preset to open");
  ok(await ev(`document.activeElement.id`) === "edHead", "editor: the keyboard lands on the preset, and the phone's stays down");
  await key("Tab", "Tab", 9);
  ok(await ev(`document.activeElement.id`) === "edName", "editor: Tab goes on to the name");
  ok(await ev(`document.querySelector("#edSend").disabled && document.querySelector("#edSendNew").disabled`) && /connect to a device/.test(await text("#edPlan")), "editor: with no device it cannot send, and says what to do");
  ok(await ev(`[...document.querySelectorAll(".ed-tab")].map((t) => t.textContent).join()`) === await ev(`[...PresetSchema.pages, ...PresetSchema.fx].map((p) => p.title).join()`), "editor: the tabs are the firmware's page names");
  ok(await ev(`document.querySelectorAll("#edPanels .knob").length`) === await ev(`PresetModel.SLOTS.filter((s) => !s.knob.unused).length`), "editor: every value has a control");
  ok(await ev(`PresetModel.SLOTS.filter((s) => !s.knob.unused).every((s) => { const c = document.querySelector('[data-k="' + s.id + '"]'); const i = c.querySelector("input,select"); return !!i && c.querySelector(".knob-name").textContent === s.knob.name; })`), "editor: each control has the panel's knob name");
  ok(await ev(`PresetModel.SLOTS.filter((s) => !s.knob.unused && !s.knob.wave).every((s) => { const i = document.querySelector('[data-k="' + s.id + '"] input,[data-k="' + s.id + '"] select'); if (i.tagName === "SELECT") return i.options.length === s.knob.max - s.knob.min + 1 && !!s.knob.list; return +i.min === s.knob.min && +i.max === s.knob.max && !s.knob.list; })`), "editor: lists for list values, sliders with the firmware's range for the rest");
  ok(await ev(`[...document.querySelector('[data-k="OSC1:0"] select').options].length`) === 92, "editor: the wave list is the four waves and the banks that exist");
  ok(await ev(`[...document.querySelector("#edCat").options].map((o) => o.textContent).join()`) === "BASS,LEAD,PAD,PLUCK,STAB,POLY,KEYS,FX,SEQ,TEXTURE,BELL", "editor: the types are the firmware's, without ALL");
  ok(await ev(`document.querySelector("#edVoices").options.length`) === 5, "editor: one to five voices");
  await common("editor");
  await shot("editor_new");

  // the name: what the firmware would refuse cannot leave the page
  await typeName("Bl\u00e5 bas");
  ok(/plain keyboard/.test(await text("#edNameNote")), "editor: a name with a character the firmware refuses says why: " + await text("#edNameNote"));
  ok(await ev(`document.querySelector("#edSave").disabled`), "editor: and it cannot be saved");
  await typeName(" Space first");
  ok(/start with a space/.test(await text("#edNameNote")) && await ev(`document.querySelector("#edSave").disabled`), "editor: nor a name that starts with a space");
  await typeName("My First Bass");
  ok(await shown("#edDirty"), "editor: a change shows as not saved");
  ok(!await ev(`document.querySelector("#edSave").disabled`), "editor: a good name can be saved");

  // the pointer: tab, slider
  await click("#edTab-FLT1");
  ok(await shown('[data-k="FLT1:1"]') && !await shown('[data-k="OSC1:1"]'), "editor: a tab shows its page");
  await slide('[data-k="FLT1:1"] input', 0.3, 0.75);
  const v = await ev(`+document.querySelector('[data-k="FLT1:1"] input').value`);
  ok(v > 85 && v < 105, "editor: a dragged slider lands where it was dragged to: " + v);
  ok(/Hz/.test(await text('[data-k="FLT1:1"] .knob-val')), "editor: the value is in the panel's own words: " + await text('[data-k="FLT1:1"] .knob-val'));
  ok(await ev(`document.querySelector("#edTab-FLT1").classList.contains("changed") && document.querySelector('[data-k="FLT1:1"]').classList.contains("changed")`), "editor: a moved knob and its page are marked");
  await shot("editor_filter");

  // the keyboard: the slider has the focus, an arrow is one step, and the tabs move with arrows
  await key("ArrowRight", "ArrowRight", 39);
  ok(await ev(`+document.querySelector('[data-k="FLT1:1"] input').value`) === v + 1, "editor: an arrow moves a slider one step");
  ok(await ev(`document.querySelector('[data-k="FLT1:1"] input').getAttribute("aria-valuetext")`) === await text('[data-k="FLT1:1"] .knob-val'), "editor: a screen reader gets the same words");
  await ev(`document.querySelector("#edTab-FLT1").focus(); 1`);
  await key("ArrowRight", "ArrowRight", 39);
  ok(await ev(`document.activeElement.id`) === "edTab-FLT2" && await shown('[data-k="FLT2:1"]'), "editor: the arrows move along the tabs");

  // the panel's rules: what does nothing right now says so
  ok(await ev(`document.querySelector('[data-k="FLT2:1"]').classList.contains("idle")`) && /ACTIVE/.test(await text('[data-k="FLT2:1"] .knob-hint')), "editor: filter 2 off: its knobs say so");
  await pick('[data-k="FLT2:5"] select', 1);
  ok(!await ev(`document.querySelector('[data-k="FLT2:1"]').classList.contains("idle")`), "editor: and wake when it is on");
  await click("#edTab-OSC2");
  await pick('[data-k="OSC2:0"] select', "b5");
  ok(await text('[data-k="OSC2:1"] .knob-name') === "POSITION" && await ev(`document.querySelector('[data-k="OSC2:2"] input').disabled`), "editor: on a wavetable PD SHAPE is POSITION and PD AMT is off, as on the panel");
  await click("#edTab-AMPSIM");
  ok(await ev(`document.querySelector('[data-k="AMPSIM:1"] input').disabled`), "editor: an effect the preset does not carry cannot be moved");
  await click("#edCarry-AMPSIM");
  ok(!await ev(`document.querySelector('[data-k="AMPSIM:1"] input').disabled`) && await text('[data-k="AMPSIM:1"] .knob-val') === "40%", "editor: ticked, it starts at the panel's default: " + await text('[data-k="AMPSIM:1"] .knob-val'));
  await shot("editor_effect");
  await click("#edCarry-AMPSIM");
  ok(await ev(`[...document.querySelectorAll("#edPanel-AMPSIM .knob")].every((k) => !k.classList.contains("changed"))`) && !await ev(`document.querySelector("#edTab-AMPSIM").classList.contains("changed")`), "editor: unticked again, nothing on that page counts as changed");
  await click("#edCarry-AMPSIM");

  // saved to the computer
  await click("#edSave");
  await until(`/Handed 1 preset to the browser/.test(document.querySelector("#edStatus").textContent)`, 2000, "the save");
  const nf = await file("1000 My First Bass.PRS");
  ok(nf.length === 148 && accepted(path.join(dl, "1000 My First Bass.PRS")), "editor: a new preset saved: 148 bytes the firmware and prs.py take");
  ok(!await shown("#edDirty"), "editor: saved, nothing is unsaved");

  // connect; a preset from the device; the failed read
  await click("#useSim");
  await until(`document.querySelectorAll("#devList tr").length === 27`, 4000, "the simulated device");
  ok(await ev(`document.querySelectorAll("#devList tr:nth-child(1) .edit-open").length`) === 0, "editor: a built-in preset has no Edit");
  await fault("corrupt-chunk");
  await click("#devList tr:nth-child(4) .edit-open");
  await until(`/could not be read/.test(document.querySelector("#status").textContent)`, 3000, "the failed read to be reported");
  ok(/checksum/.test(await status()) && /My First Bass/.test(await text("#edShown")), "editor: a read that fails leaves the editor as it was: " + await status());
  await until(`!document.querySelector("#refresh").disabled`, 2000, "the page to be usable");
  await click("#devList tr:nth-child(4) .edit-open");
  await until(`/Opened \\d+, /.test(document.querySelector("#status").textContent)`, 3000, "a preset from the device");
  await sleep(900);   // (the page scrolls smoothly)
  ok(await ev(`document.querySelector("#edWin").getBoundingClientRect().top < innerHeight`), "editor: the opened preset is on screen: " + await ev(`document.querySelector("#edWin").getBoundingClientRect().top + " of " + innerHeight + ", scrolled " + scrollY`));
  const firstId = await ev(`+document.querySelector("#edShown").textContent.slice(0, 4)`), firstName = await ev(`document.querySelector("#edName").value`);
  ok(/Send to device replaces/.test(await text("#edPlan")) && (await text("#edPlan")).includes(firstName), "editor: it says before the press what Send will replace: " + await text("#edPlan"));
  await click("#edSave");
  await until(`/Handed 1 preset/.test(document.querySelector("#edStatus").textContent)`, 2000, "the untouched file");
  const padded = String(firstId).padStart(4, "0"), original = await file(`${padded} ${firstName}.PRS`);
  fs.renameSync(path.join(dl, `${padded} ${firstName}.PRS`), path.join(dl, "original.bin"));

  // one value changed: one byte and the checksum move
  await click("#edTab-AMP");
  await slide('[data-k="AMP:0"] input', 0.5, 0.25);
  const atk = await ev(`+document.querySelector('[data-k="AMP:0"] input').value`);
  await click("#edSave");
  await sleep(100);
  const edited = await file(`${padded} ${firstName}.PRS`);
  const moved = [...edited.keys()].filter((i) => edited[i] !== original[i] && i < 144);
  ok(moved.length === 1 && moved[0] === 36 + 6 * 8 && edited[moved[0]] === atk, "editor: one value moved one byte: " + moved.join());
  ok(accepted(path.join(dl, `${padded} ${firstName}.PRS`)), "editor: and the firmware and prs.py take the file");
  fs.rmSync(path.join(dl, `${padded} ${firstName}.PRS`));

  // unsaved changes: another preset asks first, and so does leaving the page
  await slide('[data-k="AMP:0"] input', 0.25, 0.4);
  await click("#devList tr:nth-child(5) .edit-open");
  await until(`document.querySelector("#ask").open`, 2000, "the question before another preset");
  ok(/Drop your changes/.test(await text("#askTitle")) && await ev(`document.activeElement.id`) === "askNo", "editor: another preset asks first, and Cancel has the keyboard");
  await shot("editor_unsaved");
  await click("#askNo");
  await sleep(200);
  ok(await ev(`document.querySelector("#edName").value`) === firstName && await shown("#edDirty") && /Nothing was opened/.test(await log()), "editor: Cancel keeps the edit");
  await click("#edNew");
  await until(`document.querySelector("#ask").open`, 2000, "the question before a new preset");
  await click("#askNo");
  await sleep(200);
  ok(await ev(`document.querySelector("#edName").value`) === firstName, "editor: so does New");
  leavePage = false; dialogs.length = 0;
  await click('#bar nav a[href="index.html"]');
  await sleep(400);
  ok(dialogs.some((d) => d.type === "beforeunload"), "editor: leaving the page warns first");
  ok(await ev(`location.pathname.endsWith("librarian.html")`) && await shown("#edDirty"), "editor: and staying keeps the edit");
  leavePage = true;

  // send: replacing says what it overwrites; Cancel sends nothing; a failed send keeps the edit and changes nothing
  await typeName("Edited Twice");
  const count0 = await devCount();
  await click("#edSend");
  await until(`document.querySelector("#ask").open`, 3000, "the question before a replace");
  ok(/Replace a preset on the simulated device/.test(await text("#askTitle")) && (await text("#askText")).includes(`preset ${firstId}, ${firstName}`) && (await text("#askList")).includes(`replaces ${firstName}`), "editor: replacing says which one it overwrites: " + await text("#askText"));
  ok(await ev(`document.activeElement.id`) === "askNo", "editor: Cancel has the keyboard");
  await shot("editor_replace");
  await click("#askNo");
  await sleep(200);
  ok(/Nothing was sent/.test(await log()) && await shown("#edDirty"), "editor: Cancel sends nothing");
  await fault("write-fail");
  await click("#edSend");
  await until(`document.querySelector("#ask").open`, 3000, "the question again");
  await click("#askGo");
  await until(`/Sending stopped/.test(document.querySelector("#status").textContent)`, 5000, "the failed send to be reported");
  ok(/could not write to its card/.test(await status()) && /still here/.test(await status()), "editor: a failed send says what happened: " + await status());
  ok(await edStatus() === await status(), "editor: and it says so beside the editor too");
  ok(await shown("#edDirty") && await ev(`document.querySelector("#edName").value`) === "Edited Twice", "editor: a failed send keeps the edit");
  await until(`!document.querySelector("#refresh").disabled`, 3000, "the page to be usable again");
  await shot("editor_failed");
  ok(await ev(`[...document.querySelectorAll("#devList tr")].some((r) => r.textContent.includes(${JSON.stringify(firstName)}))`) && await devCount() === count0, "editor: and the device has what it had");
  const listRow = `#devList input[value="${firstId}"]`;
  await click(listRow);
  await click("#backupSel");
  await until(`/Handed 1 preset to/.test(document.querySelector("#status").textContent) && !document.querySelector("#backupSel").disabled`, 3000, "a backup of the preset that was not replaced");
  ok((await file(`${padded} ${firstName}.PRS`)).equals(original), "editor: by the bytes: the preset on the device is the one it was");
  fs.rmSync(path.join(dl, `${padded} ${firstName}.PRS`));
  await click(listRow);
  // a list that cannot be read: nothing is sent
  await fault("no-reply");
  await click("#edSend");
  await until(`/nothing was sent/.test(document.querySelector("#status").textContent)`, 4000, "the list failure before a send");
  ok(await shown("#edDirty"), "editor: a list that cannot be read sends nothing and keeps the edit: " + await status());
  await until(`!document.querySelector("#refresh").disabled`, 3000, "the page to be usable again");
  // ... and now it goes through
  await click("#edSend");
  await until(`document.querySelector("#ask").open`, 3000, "the question for the send that works");
  await click("#askGo");
  await until(`/Sent 1 preset to the simulated device/.test(document.querySelector("#status").textContent)`, 4000, "the send");
  await until(`[...document.querySelectorAll("#devList tr")].some((r) => r.textContent.includes("Edited Twice"))`, 4000, "the new name in the list");
  ok(await devCount() === count0 && !await shown("#edDirty"), "editor: replacing keeps the count, and nothing is unsaved");
  await click(listRow);
  await click("#backupSel");
  await until(`/Handed 1 preset to/.test(document.querySelector("#status").textContent) && !document.querySelector("#backupSel").disabled`, 3000, "a backup of the sent preset");
  ok(accepted(path.join(dl, `${padded} Edited Twice.PRS`)) || (await file(`${padded} Edited Twice.PRS`), accepted(path.join(dl, `${padded} Edited Twice.PRS`))), "editor: what the device holds now is a file the firmware takes");
  await click(listRow);

  // send as new: a free number, and the first stays
  await slide('[data-k="AMP:0"] input', 0.4, 0.6);
  await click("#edSendNew");
  await until(`document.querySelector("#ask").open`, 3000, "the question for send as new");
  ok(/number 1000/.test(await text("#askText")) && !/replaces/.test(await text("#askList")), "editor: a copy gets the first free number from 1000: " + await text("#askText"));
  await click("#askGo");
  await until(`document.querySelectorAll("#devList tr").length === ${count0 + 1}`, 4000, "the copy in the list");
  ok(await ev(`[...document.querySelectorAll("#devList tr")].filter((r) => r.textContent.includes("Edited Twice")).length`) === 2, "editor: both are on the device");
  ok(/^1000 /.test(await text("#edShown")), "editor: and the editor is on the copy: " + await text("#edShown"));

  // a new one on the device, then a file from the computer that is already there
  await click("#edNew"); await calm();
  await until(`/a new preset/.test(document.querySelector("#edFrom").textContent)`, 2000, "a new preset");
  await click("#edSend");
  await until(`document.querySelector("#ask").open`, 3000, "the question for a new preset");
  ok(/number 1001/.test(await text("#askText")), "editor: a new preset gets the next free number: " + await text("#askText"));
  await click("#askGo");
  await until(`document.querySelectorAll("#devList tr").length === ${count0 + 2}`, 4000, "the new preset in the list");
  await setFiles([path.join(dl, "1000 My First Bass.PRS")]);
  await until(`document.querySelectorAll("#pcList tr").length === 1`, 2000, "the file");
  await click("#pcList .edit-open");
  await until(`/Opened the file/.test(document.querySelector("#status").textContent)`, 2000, "the file in the editor");
  ok(await ev(`document.querySelector("#edName").value`) === "My First Bass" && await ev(`document.querySelector("#edCat").value`) === "1", "editor: a file from the computer opens");
  await click("#edTab-OSC2");
  ok(await ev(`document.querySelector('[data-k="OSC2:0"] select').selectedOptions[0].textContent`) === "SOFT HARD", "editor: with everything that was saved in it");
  await click("#edSend");
  await until(`document.querySelector("#ask").open`, 3000, "the question for a file whose number is taken");
  ok(/Edited Twice/.test(await text("#askText")), "editor: a file whose number is taken says what it would replace: " + await text("#askText"));
  await click("#askNo");
  await sleep(150);

  // undo
  await typeName("Throw away");
  await click("#edUndo");
  await until(`document.querySelector("#ask").open`, 2000, "the question before undo");
  await click("#askGo");
  await until(`document.querySelector("#edName").value === "My First Bass"`, 2000, "undo");
  ok(!await shown("#edDirty"), "editor: undo goes back to what was opened");

  // hearing it: the real engine, the real bytes. A level that goes to nothing with the AMP LEVEL, and back
  if (await ev(`crossOriginIsolated`) && await ev(`fetch("drumboy_hr.wasm", { method: "HEAD" }).then((r) => r.ok)`)) {
    const level = () => ev(`parseFloat(document.querySelector("#edLevel").style.width) || 0`);
    await click("#edNew"); await calm();
    await click("#edHear");
    ok(await text("#edHear") === "Stop", "editor: Hear it turns into Stop");
    await until(`parseFloat(document.querySelector("#edLevel").style.width) > 5`, 12000, "sound from the engine");
    ok(!await ev(`/did not start|could not be/.test(document.querySelector("#edStatus").textContent)`), "editor: the engine started: " + await edStatus());
    await click("#edTab-AMP");
    await slide('[data-k="AMP:5"] input', 0.8, 0);
    ok(await ev(`+document.querySelector('[data-k="AMP:5"] input').value`) === 0, "editor: AMP LEVEL at zero");
    await sleep(2400);
    let peak = 0;
    for (let i = 0; i < 20; ++i) { peak = Math.max(peak, await level()); await sleep(100); }
    ok(peak === 0, "editor: the edited bytes reach the engine: AMP LEVEL 0 is silence: " + peak);
    await slide('[data-k="AMP:5"] input', 0, 0.8);
    await until(`parseFloat(document.querySelector("#edLevel").style.width) > 5`, 5000, "the sound to come back with the level");
    await click("#edHear");
    ok(await text("#edHear") === "Hear it", "editor: Stop stops it");
    await sleep(300);
    ok(await level() === 0, "editor: and the meter falls");
    await click("#edUndo");
    await until(`document.querySelector("#ask").open`, 2000, "the question");
    await click("#askGo");
    await sleep(100);
  } else ok(false, "editor: the web build is needed to test hearing a preset");

  // a bank that does not exist: as on the card, the nearest basic wave plays
  {
    const b = Buffer.from(fs.readFileSync(path.join(dl, "1000 My First Bass.PRS")));
    b[10] = 6; b.writeUInt32LE(zlib.crc32(b.subarray(0, 144)), 144);
    const f = path.join(tmp, "no such bank.PRS");
    fs.writeFileSync(f, b);
    await setFiles([f]);
    await until(`document.querySelectorAll("#pcList tr").length === 2`, 2000, "the file with a missing bank");
    await click("#pcList tr:nth-child(2) .edit-open");
    await calm();
    await click("#edTab-OSC1");
    ok(/ \?$/.test(await ev(`document.querySelector('[data-k="OSC1:0"] select').selectedOptions[0].textContent`)) && /does not exist/.test(await text('[data-k="OSC1:0"] .knob-hint')), "editor: a bank that does not exist stays chosen, and says what the ? means");
    await click("#edHear");
    await until(`parseFloat(document.querySelector("#edLevel").style.width) > 5`, 6000, "sound with a bank that is missing");
    ok(/nearest/.test(await edStatus()) && await text("#edHear") === "Stop", "editor: it plays, and says what it plays: " + await edStatus());
    // Stop works whatever the name says
    await typeName("Bl\u00e5");
    ok(!await ev(`document.querySelector("#edHear").disabled`), "editor: Stop is there with a name that cannot be saved");
    await click("#edHear");
    await sleep(200);
    ok(await ev(`parseFloat(document.querySelector("#edLevel").style.width) || 0`) === 0 && await ev(`document.querySelector("#edHear").disabled`), "editor: stopped, and the meter is down");
    await typeName("My First Bass");
    problems.length = 0;   // (the missing bank's 404 is the point of this part)
  }

  // the panel's rule for a screen: wide, a laptop, a phone
  ok(problems.length === 0, "editor: the console is clean: " + problems.join(" | "));
  await click("#edTab-FLT1");
  await shot("editor_laptop");
  await viewport(1366, 768); await wide("editor, laptop");
  await viewport(390, 844, true);
  await sleep(200);
  await wide("editor, phone");
  ok(await ev(`getComputedStyle(document.querySelector(".ed-panel:not([hidden]) .knobs")).gridTemplateColumns.split(" ").length`) === 1, "editor, phone: one column of knobs");
  ok(await ev(`[...document.querySelectorAll("#edWin button, #edWin select, #edWin input")].filter((e) => e.getClientRects().length).every((e) => { const b = e.getBoundingClientRect(); return b.left >= 0 && b.right <= innerWidth; })`), "editor, phone: every control is on the screen");
  await ev(`document.querySelector("#edWin").scrollIntoView({ block: "start", behavior: "instant" }); 1`);
  await shot("editor_phone");
  await viewport(1400, 950);
}

(async () => {
  let targets;
  for (let k = 0; k < 50; k++) { try { targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json(); if (targets.find((t) => t.type === "page")) break; } catch {} await sleep(200); }
  ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && pending.has(d.id)) { const p = pending.get(d.id); pending.delete(d.id); d.error ? p.rej(new Error(JSON.stringify(d.error))) : p.res(d.result); }
    else if (d.method === "Page.javascriptDialogOpening") { dialogs.push(d.params); call("Page.handleJavaScriptDialog", { accept: leavePage }); }
    else if (d.method === "Runtime.exceptionThrown") problems.push(d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text);
    else if (d.method === "Runtime.consoleAPICalled" && d.params.type === "error") problems.push(d.params.args.map((a) => a.value ?? a.description ?? "").join(" "));
    else if (d.method === "Log.entryAdded" && d.params.entry.level === "error") problems.push(d.params.entry.text + " " + (d.params.entry.url || ""));
  };
  for (const dom of ["Runtime", "Log", "Page", "DOM"]) await call(dom + ".enable");
  for (const [name, fn] of [["start", startPage], ["online", onlinePage], ["export", exportPanel], ["tutorial", tutorialPage], ["bench", benchPage], ["install", installPage], ["manual", manualPage], ["librarian", librarianPage], ["editor", editorPage]]) {
    if (only && only !== name) continue;
    try { await fn(); } catch (e) { ok(false, name + " page stopped: " + e.message); }
  }
  console.log(checks + " checks, " + fails + " failed");
  chrome.kill();
  await sleep(300);
  fs.rmSync(tmp, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.error("driver error", e); chrome.kill(); process.exit(1); });
