// Every lesson of the tutorial page, on the real firmware in headless Chrome: each step through Show me, from a fresh Drumboy. Needs `python3 web/serve.py PORT` running.
//   node web/test/learn.mjs [URL]      exit status 1 when a step did not finish
import { spawnSync } from "node:child_process";
import path from "node:path";
const url = process.argv[2] || "http://localhost:8765/";
const r = spawnSync("node", [path.join(path.dirname(new URL(import.meta.url).pathname), "cdp.mjs"), new URL("tutorial.html", url).href, "--wait", "4000",
  "--eval", "Learn.runAll().then(JSON.stringify).catch((e) => JSON.stringify(['exception: ' + e.stack]))"], { encoding: "utf8", timeout: 600000 });
const line = r.stdout.split("\n").find((l) => l.startsWith("[eval] Learn.runAll()")) || "";
const failed = JSON.parse(JSON.parse(line.slice(line.lastIndexOf(" => ") + 4).trim() || '"[\\"the page did not answer\\"]"'));
failed.forEach((f) => console.log("FAILED", f));
console.log(failed.length ? failed.length + " lessons stopped" : "every lesson finished through Show me");
process.exit(failed.length ? 1 : 0);
