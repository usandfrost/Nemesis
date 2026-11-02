// A tiny Chrome DevTools driver for testing the page headlessly (Node 22+, no packages):
//   node web/test/cdp.mjs URL [--wait MS] [--eval "JS"]... [--shot out.png]
// prints the page's console output and the value of each --eval (run in order, awaiting promises).
import { spawn } from "node:child_process";
import fs from "node:fs";
const args = process.argv.slice(2);
const url = args[0];
const evals = [], opt = (n, d) => (args.includes(n) ? args[args.indexOf(n) + 1] : d);
args.forEach((a, i) => { if (a === "--eval") evals.push(args[i + 1]); });
const wait = +opt("--wait", 3000), shot = opt("--shot", null), port = 9300 + Math.floor(Math.random() * 500);
const chrome = spawn("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", ["--headless=new", "--disable-gpu", "--no-sandbox", `--remote-debugging-port=${port}`, `--user-data-dir=/tmp/cdp-prof-${port}`, "--autoplay-policy=no-user-gesture-required", "--mute-audio", "--window-size=1500,950", "about:blank"], { stdio: "ignore" });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let ws, id = 0; const pending = new Map();
const call = (method, params = {}) => new Promise((res, rej) => { const i = ++id; pending.set(i, { res, rej }); ws.send(JSON.stringify({ id: i, method, params })); });
(async () => {
  let targets;
  for (let k = 0; k < 50; k++) { try { targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json(); if (targets.find((t) => t.type === "page")) break; } catch {} await sleep(200); }
  ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && pending.has(d.id)) { const p = pending.get(d.id); pending.delete(d.id); d.error ? p.rej(new Error(JSON.stringify(d.error))) : p.res(d.result); }
    else if (d.method === "Runtime.consoleAPICalled") console.log("[console." + d.params.type + "]", d.params.args.map((a) => a.value ?? a.description ?? "").join(" "));
    else if (d.method === "Runtime.exceptionThrown") console.log("[exception]", d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text);
    else if (d.method === "Log.entryAdded") console.log("[log." + d.params.entry.level + "]", d.params.entry.text, d.params.entry.url || "");
  };
  await call("Runtime.enable"); await call("Log.enable"); await call("Page.enable");
  await call("Page.navigate", { url });
  await sleep(wait);
  for (const e of evals) {
    const r = await call("Runtime.evaluate", { expression: e, awaitPromise: true, returnByValue: true });
    console.log("[eval]", e.length > 60 ? e.slice(0, 60) + "…" : e, "=>", JSON.stringify(r.result.value ?? r.exceptionDetails?.exception?.description ?? r.result.description));
  }
  if (shot) { const r = await call("Page.captureScreenshot", { format: "png" }); fs.writeFileSync(shot, Buffer.from(r.data, "base64")); console.log("screenshot", shot); }
  chrome.kill(); process.exit(0);
})().catch((e) => { console.error("driver error", e); chrome.kill(); process.exit(1); });
