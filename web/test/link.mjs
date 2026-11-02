// The website's real client (web/www/sysex.js) against the real firmware side (firmware/src/midi/PresetLink.cpp, tests/midi/link_bridge.cpp), with a folder for a card.
//   node web/test/link.mjs [path to link_bridge]      exit status 1 when a check fails
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import readline from "node:readline";
const root = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "..");
await import(path.join(root, "web/www/sysex.js"));
const P = globalThis.PresetSysex;
const bridge = process.argv[2] || path.join(root, "tests/build/link_bridge");
let checks = 0, fails = 0;
const ok = (c, m) => { ++checks; if (!c) { ++fails; console.log("FAIL", m); } };
const hex = (b) => Buffer.from(b).toString("hex");

const dir = fs.mkdtempSync(path.join(os.tmpdir(), "link-"));
const src = path.join(root, "content/presets");
const names = fs.readdirSync(src).filter((n) => n.endsWith(".PRS")).sort().slice(0, 12);
for (const n of names) fs.copyFileSync(path.join(src, n), path.join(dir, n));
const device = spawn(bridge, [dir], { stdio: ["pipe", "pipe", "inherit"] });
const send = (b) => device.stdin.write(hex(b) + "\n");
const tick = (ms) => device.stdin.write(`tick ${ms}\n`);
const c = new P.Client(send);
readline.createInterface({ input: device.stdout }).on("line", (l) => c.receive(Uint8Array.from(Buffer.from(l, "hex"))));
const rejects = async (p, code, m) => { try { await p; ok(false, m + ": no error"); } catch (e) { ok(e.code === code, m + ": " + e.message); } };
const raw = (n) => Uint8Array.from(fs.readFileSync(path.join(dir, n)));

// who is there, and what it has
const who = await c.identify();
ok(who.protocol === 1 && who.name === "NEMESIS" && !who.simulated, "identify: a real device");
const song = await c.songMidi(2);
ok(song.length === 1000 && song.every((b, i) => b === ((i * 3 + 2) & 255)), "a song as MIDI: every byte, over chunks, checked by its CRC");
await rejects(c.songMidi(9), P.ERR.NO_SONG, "a song that is not there");
ok(who.count === names.length && who.capacity === P.LIBRARY_MAX, "identify: the files on the card: " + who.count);
const list = await c.list();
ok(list.length === names.length + 3, "list: the card's and the built-in three: " + list.length);
ok(list.filter((e) => e.builtIn).length === 3, "list: three built in");
const first = list.find((e) => !e.builtIn);

// fetching
for (const e of list.filter((x) => !x.builtIn)) {
  const got = await c.download(e.id);
  const want = raw(names.find((n) => P.parsePreset(raw(n)).id === e.id));
  ok(Buffer.compare(Buffer.from(got), Buffer.from(want)) === 0, "download: byte for byte, preset " + e.id);
}
await rejects(c.download(1), P.ERR.BUILT_IN, "download a built-in");
await rejects(c.download(9999), P.ERR.NOT_FOUND, "download what is not there");

// a new one, under a free number: a copy of the first with another id
const copy = (from, id, name) => {
  const d = Uint8Array.from(from);
  d[6] = id & 255; d[7] = id >> 8;
  if (name) { d.fill(0, 12, 34); for (let i = 0; i < name.length; ++i) d[12 + i] = name.charCodeAt(i); }
  const crc = P.crc32(d.subarray(0, P.PRS_SIZE - 4));
  d.set(P.u32(crc), P.PRS_SIZE - 4);
  return d;
};
const fresh = copy(await c.download(first.id), 5000, "From The Web");
let r = await c.upload(fresh, false);
ok(r.id === 5000 && r.count === names.length + 1, "upload: a new preset, " + JSON.stringify(r));
ok(fs.existsSync(path.join(dir, "5000 From The Web.PRS")), "upload: the file is on the card under the firmware's name");
ok(Buffer.compare(Buffer.from(await c.download(5000)), Buffer.from(fresh)) === 0, "upload: and comes back the same");
await rejects(c.upload(fresh, false), P.ERR.ID_IN_USE, "upload over a preset without saying so");
ok(fs.readdirSync(dir).filter((n) => n.endsWith(".PRS")).length === names.length + 1, "and nothing was added");

// an update: same number, new name, new bytes
const updated = copy(fresh, 5000, "Better Name");
r = await c.upload(updated, true);
ok(r.count === names.length + 1, "replace: the count stands");
ok(Buffer.compare(Buffer.from(await c.download(5000)), Buffer.from(updated)) === 0, "replace: the new bytes come back");
ok((await c.list()).find((e) => e.id === 5000).name === "Better Name", "replace: the list follows");

// damaged, stale, or out of turn
const badCrc = Uint8Array.from(updated); badCrc[40] ^= 1;
await rejects(c.remove(5000, new Uint8Array(P.NAME_FIELD)), P.ERR.NAME_MISMATCH, "delete with a stale name");
ok((await c.list()).some((e) => e.id === 5000), "and it is still there");
await rejects(c.remove(1, new Uint8Array(P.NAME_FIELD)), P.ERR.BUILT_IN, "delete a built-in");
await rejects(c.remove(9999, new Uint8Array(P.NAME_FIELD)), P.ERR.NOT_FOUND, "delete what is not there");
{
  const tag = c.start();
  const m = P.encode(P.CMD.LIST, tag); m[m.length - 2] ^= 1;
  send(m);
  await rejects(c.reply(1000, [P.CMD.DATA_BEGIN]), P.ERR.BAD_MESSAGE, "a damaged message");
}
{   // a bad whole-file check: only the commit can tell, and nothing reaches the card
  const tag = c.start();
  send(P.encode(P.CMD.UPLOAD_BEGIN, tag, [...P.u16(6000), ...P.u16(P.PRS_SIZE), ...P.u32(P.crc32(fresh) ^ 1), 0]));
  await c.reply(1000, [P.CMD.ACK]);
  send(P.encode(P.CMD.DATA_CHUNK, tag, [...P.u16(0), ...copy(fresh, 6000, "Bad")]));
  await c.reply(1000, [P.CMD.ACK]);
  send(P.encode(P.CMD.UPLOAD_COMMIT, tag));
  await rejects(c.reply(1000, [P.CMD.DONE]), P.ERR.CRC_MISMATCH, "a transfer that does not match");
  ok(!fs.readdirSync(dir).some((n) => n.startsWith("6000")), "and no file");
}
{   // a good CRC of a file that does not pass: the id inside is not the one announced
  const other = copy(fresh, 6001, "Wrong Id");
  const tag = c.start();
  send(P.encode(P.CMD.UPLOAD_BEGIN, tag, [...P.u16(6002), ...P.u16(P.PRS_SIZE), ...P.u32(P.crc32(other)), 0]));
  await c.reply(1000, [P.CMD.ACK]);
  send(P.encode(P.CMD.DATA_CHUNK, tag, [...P.u16(0), ...other]));
  await c.reply(1000, [P.CMD.ACK]);
  send(P.encode(P.CMD.UPLOAD_COMMIT, tag));
  await rejects(c.reply(1000, [P.CMD.DONE]), P.ERR.BAD_PRESET, "a file with another id inside");
}
{   // a chunk out of order ends the upload
  const tag = c.start();
  send(P.encode(P.CMD.UPLOAD_BEGIN, tag, [...P.u16(6003), ...P.u16(P.PRS_SIZE), ...P.u32(1), 0]));
  await c.reply(1000, [P.CMD.ACK]);
  send(P.encode(P.CMD.DATA_CHUNK, tag, [...P.u16(5), 1, 2, 3]));
  await rejects(c.reply(1000, [P.CMD.ACK]), P.ERR.SEQUENCE, "a chunk out of order");
}
{   // while an upload is open only it, IDENTIFY and ABORT get through; a silent one is dropped
  const tag = c.start();
  send(P.encode(P.CMD.UPLOAD_BEGIN, tag, [...P.u16(6004), ...P.u16(P.PRS_SIZE), ...P.u32(1), 0]));
  await c.reply(1000, [P.CMD.ACK]);
  send(P.encode(P.CMD.LIST, tag));
  await rejects(c.reply(1000, [P.CMD.DATA_BEGIN]), P.ERR.BUSY, "busy while an upload is open");
  tick(2500);
  await rejects(c.reply(1000, [P.CMD.ACK]), P.ERR.TIMEOUT, "an upload gone silent");
  ok((await c.list()).length === names.length + 4, "and the device answers again");
}

// and gone
const e = (await c.list()).find((x) => x.id === 5000);
r = await c.remove(5000, e.nameField);
ok(r.count === names.length, "delete: the count: " + JSON.stringify(r));
ok(!fs.readdirSync(dir).some((n) => n.startsWith("5000")), "delete: the file is gone from the card");
ok(!(await c.list()).some((x) => x.id === 5000), "delete: and from the list");

device.stdin.end();
fs.rmSync(dir, { recursive: true, force: true });
fs.rmSync(dir + ".pix", { force: true });
console.log(`${checks} checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
