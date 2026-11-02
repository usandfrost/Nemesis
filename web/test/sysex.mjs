// The librarian's protocol without a browser: the 7-bit coding, the message check, the preset file checks on every preset in content/, and the client against the simulated device,
// a failed transfer of every kind included. Against the simulated device: link.mjs runs the same client against the firmware's own code.
//   node web/test/sysex.mjs      exit status 1 when a check fails
import fs from "node:fs";
import path from "node:path";
const root = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "..");
await import(path.join(root, "web/www/sysex.js"));
await import(path.join(root, "web/www/sim-device.js"));
const P = globalThis.PresetSysex, SimDevice = globalThis.SimDevice;
let checks = 0, fails = 0;
const ok = (c, m) => { ++checks; if (!c) { ++fails; console.log("FAIL", m); } };
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

// ---- the bytes
for (let n = 0; n < 60; ++n) {
  const d = Uint8Array.from({ length: n }, () => Math.floor(Math.random() * 256));
  const p = P.pack7(d);
  ok(p.every((v) => v < 128), "packed bytes are 7 bits, length " + n);
  ok(same(P.unpack7(p), d), "pack7 round trip, length " + n);
}
ok(same(P.pack7([0x80, 0x01]), [0x01, 0x00, 0x01]), "the top bit of the group's first byte is bit 0 of the leading byte");
const msg = P.encode(P.CMD.DOWNLOAD, 5, [0xFF, 0x00, 0x80]);
ok(msg[0] === 0xF0 && msg[msg.length - 1] === 0xF7 && msg.slice(1, -1).every((v) => v < 128), "a message is one SysEx with 7-bit bytes inside");
const m = P.decode(msg);
ok(m.ours && m.cmd === P.CMD.DOWNLOAD && m.tag === 5 && same(m.payload, [0xFF, 0x00, 0x80]), "a message decodes to what went in");
for (let i = 4; i < msg.length - 1; ++i) { const b = Uint8Array.from(msg); b[i] ^= 1; ok(P.decode(b).bad, "one flipped bit at byte " + i + " is seen"); }
ok(!P.decode(Uint8Array.from([0xF0, 0x41, 0x10, 0x42, 0xF7])).ours, "someone else's SysEx is left alone");
ok(P.decode(msg.slice(0, 6)).bad, "a message cut off is seen");
const v2 = Uint8Array.from(msg); v2[4] = 2; v2[v2.length - 2] = Array.from(v2.slice(4, -2)).reduce((s, v) => s + v, 0) & 0x7F;
ok(P.decode(v2).version === 2, "another version of the protocol is seen");
ok(P.crc32(new TextEncoder().encode("123456789")) === 0xCBF43926, "CRC-32 is zlib's");

// ---- the files: every preset the project ships, and damaged copies
const files = [];
for (const dir of ["presets", "presets2", "synthwave", "techno", "synthpop", "trance", "ebm", "royksopp", "delerium", "voice"])
  for (const f of fs.readdirSync(path.join(root, "content", dir))) if (f.toUpperCase().endsWith(".PRS") && !f.startsWith(".")) files.push(new Uint8Array(fs.readFileSync(path.join(root, "content", dir, f))));
ok(files.length >= 197, "the shipped presets are there: " + files.length);
for (const d of files) { const r = P.parsePreset(d); ok(r.ok, "a shipped preset is valid: " + r.why); }
const one = files[0];
ok(!P.parsePreset(one.slice(0, 147)).ok, "a short file is refused");
const flip = Uint8Array.from(one); flip[50] ^= 1; ok(!P.parsePreset(flip).ok, "a damaged file is refused");
ok(/^\d{4} .+\.PRS$/.test(P.fileName(P.parsePreset(one).id, P.parsePreset(one).name)), "the file name follows the firmware's");
ok(P.fileName(12, "A/B: C") === "0012 A_B_ C.PRS", "characters a file name cannot have become _");

// ---- the client against the simulated device, at 100 times a cable's speed
function rig(n = 12) {
  const dev = new SimDevice(files.slice(0, n), 100);
  const c = new P.Client((b) => dev.receive(b));
  dev.onsend = (b) => c.receive(b);
  return { dev, c };
}
const fails_with = async (p, code) => { try { await p; return false; } catch (e) { return e instanceof P.ProtocolError && (code === undefined || e.code === code) ? e : false; } };
{
  const { c } = rig();
  const song = await c.songMidi(0);
  ok(new TextDecoder().decode(song.subarray(0, 4)) === "MThd", "a song comes across as a MIDI file");
  ok(await fails_with(c.songMidi(9), P.ERR.NO_SONG), "a song that is not there is an error");
  const z = await new Response(P.zip([{ name: "a.wav", bytes: Uint8Array.from([1, 2, 3]) }, { name: "b.wav", bytes: Uint8Array.from([4]) }])).arrayBuffer();
  ok(new Uint8Array(z)[0] === 0x50 && z.byteLength > 100, "files go into one zip");
}
{
  const { dev, c } = rig();
  const id = await c.identify();
  ok(id.name === "NEMESIS" && id.simulated && id.count === 12 && id.capacity === 1024, "identify, and it says it is simulated (the card's files: the built-in three do not count)");
  const list = await c.list();
  ok(list.length === 15 && list.slice(0, 3).every((e) => e.builtIn) && !list[3].builtIn, "the list has the built-in three, flagged, and the files");
  const first = list[3];
  const got = await c.download(first.id);
  ok(same(got, dev.files.get(first.id)), "download gives the file byte for byte");
  ok((await fails_with(c.download(first.id + 30000), P.ERR.NOT_FOUND)) !== false, "a preset that is not there is an error");
  ok((await fails_with(c.download(0), P.ERR.BUILT_IN)) !== false, "a built-in preset cannot be downloaded");

  const fresh = files[20], pf = P.parsePreset(fresh);
  let seen = 0;
  const up = await c.upload(fresh, false, (a) => (seen = a));
  ok(up.id === pf.id && same(dev.files.get(pf.id), fresh) && seen === 148, "upload stores the file byte for byte and reports its progress");
  ok((await fails_with(c.upload(fresh, false), P.ERR.ID_IN_USE)) !== false, "a number already there is refused without the replace flag");
  ok((await c.upload(fresh, true)).id === pf.id, "with the replace flag it goes through");

  ok((await fails_with(c.remove(0, new Uint8Array(22)), P.ERR.BUILT_IN)) !== false, "a built-in preset cannot be deleted");
  const stale = Uint8Array.from(first.nameField); stale[0] ^= 1;
  ok((await fails_with(c.remove(first.id, stale), P.ERR.NAME_MISMATCH)) !== false && dev.files.has(first.id), "a stale list deletes nothing");
  ok((await c.remove(first.id, first.nameField)).count === 12 && !dev.files.has(first.id), "delete removes the preset");
  ok((await fails_with(c.remove(first.id, first.nameField), P.ERR.NOT_FOUND)) !== false, "deleting it again is an error");
}
// failed transfers: each leaves the device as it was, and the next transfer works
for (const [fault, what, code] of [["bad-sum", "a damaged message", undefined], ["drop-chunk", "a lost chunk", P.ERR.SEQUENCE], ["corrupt-chunk", "bytes changed on the way", P.ERR.CRC_MISMATCH], ["no-reply", "silence", -1]]) {
  const { dev, c } = rig(30);   // (long enough for the list to take several chunks)
  dev.fault = fault;
  ok((await fails_with(c.list(), code)) !== false, "list: " + what + " fails the transfer");
  await new Promise((r) => setTimeout(r, 60));
  ok((await c.list()).length === 33, "list works again after " + what);
}
{
  const { dev, c } = rig();
  const fresh = files[20], pf = P.parsePreset(fresh), before = dev.files.size;
  for (const [fault, code] of [["corrupt-chunk", P.ERR.CRC_MISMATCH], ["write-fail", P.ERR.WRITE_FAILED], ["library-full", P.ERR.LIBRARY_FULL], ["no-card", P.ERR.NO_CARD], ["no-reply", -1]]) {
    dev.fault = fault;
    ok((await fails_with(c.upload(fresh, false), code)) !== false, "upload: " + fault + " fails it");
    ok(dev.files.size === before && !dev.files.has(pf.id), "upload: " + fault + " leaves the library as it was");
    await new Promise((r) => setTimeout(r, 40));
  }
  ok((await c.upload(fresh, false)).id === pf.id, "the upload works after all that");
  const flip2 = Uint8Array.from(fresh); flip2[50] ^= 1;
  ok((await fails_with(c.upload(flip2, false))) !== false, "the client will not send a damaged file");
}
{ // an upload that stops halfway: the device gives up, and starts nothing from the stale state
  const { dev, c } = rig();
  const t = c.start();
  const fresh = files[20];
  c.send(P.encode(P.CMD.UPLOAD_BEGIN, t, [...P.u16(P.parsePreset(fresh).id), ...P.u16(148), ...P.u32(P.crc32(fresh)), 0]));
  await c.reply(1000, [P.CMD.ACK]);
  ok((await fails_with(c.next(1000).then((m) => { if (m.cmd === P.CMD.ERROR) throw new P.ProtocolError("x", m.payload[1]); }), P.ERR.TIMEOUT)) !== false, "a stalled upload ends with a timeout");
  ok(dev.up === null, "and the device has dropped it");
}
{   // a transfer given up on: the device is told, or it would go on sending and answer nothing else
  const sent = [];
  const c = new P.Client((b) => sent.push(b));
  await fails_with(c.list(), -1);
  ok(sent.length === 2 && P.decode(sent[1]).cmd === P.CMD.ABORT && P.decode(sent[1]).tag === P.decode(sent[0]).tag, "no answer to a LIST: ABORT, with its tag");
}
console.log(checks + " checks, " + fails + " failed");
process.exit(fails ? 1 : 0);
