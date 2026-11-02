// The preset editor's bytes, without a browser: the data against the firmware, every value the editor can produce against the page's parsePreset, and then, for the same records, against the
// firmware's parsePresetFile (tests/build/preset_accept) and tools/prs.py. Editing one value must leave every other byte as it was.
//   node web/test/preset-edit.mjs      needs tests/build (cmake --build tests/build). Exit status 1 when a check fails
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
const root = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "..");
for (const f of ["sysex.js", "preset-schema.js", "preset-model.js", "preset-hear.js", "preset-editor.js"]) await import(path.join(root, "web/www", f));
const P = globalThis.PresetSysex, M = globalThis.PresetModel, S = globalThis.PresetSchema, F = S.format, HELP = globalThis.PresetEditor.HELP, NOT_USED = globalThis.PresetEditor.NOT_USED;
let checks = 0, fails = 0;
const ok = (c, m) => { ++checks; if (!c) { ++fails; if (fails < 40) console.log("FAIL", m); } };
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const refused = (fn) => { try { fn(); return false; } catch (e) { return e instanceof RangeError; } };

// ---- the shipped presets
const files = [];
for (const dir of ["presets", "presets2", "synthwave", "techno", "synthpop", "trance", "ebm", "royksopp", "delerium", "voice"])
  for (const f of fs.readdirSync(path.join(root, "content", dir))) if (f.toUpperCase().endsWith(".PRS") && !f.startsWith(".")) files.push(new Uint8Array(fs.readFileSync(path.join(root, "content", dir, f))));
ok(files.length >= 197, "the shipped presets are there: " + files.length);

// ---- the data says what sysex.js and the rest of the firmware say
ok(F.size === P.PRS_SIZE && F.nameField === P.NAME_FIELD && F.cap === P.LIBRARY_MAX, "the file's size, name field and library cap agree with sysex.js");
ok(same(S.cats, P.CATS), "the categories agree with sysex.js");
ok(S.pages.length === F.pages && S.pages.every((p) => p.knobs.length === F.perPage), "nine pages of eight knobs");
ok(M.SLOTS.length === 72 + 8 * 4 + 3, "every place in the file has a knob: " + M.SLOTS.length);
ok(new Set(M.SLOTS.map((s) => s.id)).size === M.SLOTS.length, "no two knobs share a name in the editor");
ok(new Set(M.SLOTS.map((s) => s.at)).size === M.SLOTS.length, "no two knobs share a byte");
ok(F.at.values + 72 <= S.fx.find((e) => e.key === "SENDS").at && F.at.mask === S.fx.find((e) => e.key === "SENDS").at + 3, "the file's parts are in the order PresetFile.h has them");
const wtFiles = fs.readdirSync(path.join(root, "content/data/wavetables")).filter((f) => /^wt\d{3}\.wt$/.test(f)).map((f) => +f.slice(2, 5)).sort((a, b) => a - b);
ok(same(S.wave.exist, wtFiles) && wtFiles.length === 88, "the banks on offer are the banks that exist: " + S.wave.exist.length);
ok(S.wave.bankNames.length === F.banks && S.wave.bankCat.length === F.banks, "every catalogue number has a name");
for (const d of files) for (const b of [d[F.at.bank], d[F.at.bank + 1]]) ok(b === F.noBank || S.wave.exist.includes(b), "a shipped preset's bank exists: " + b);
for (const s of M.SLOTS) if (!s.knob.unused && !s.knob.wave) ok(Sc(s).length === s.knob.max - s.knob.min + 1, "a knob has a word for every value: " + s.id);
function Sc(s) { return S.texts[s.knob.text]; }
const ui = fs.readFileSync(path.join(root, "firmware/src/app/Ui.cpp"), "utf8");
ok(ui.includes('c.label.add("POSITION")'), "the panel still calls PD SHAPE POSITION on a wavetable, as the editor does");
ok(ui.includes("waveIsBank(w) && arg == 2") && ui.includes("page == PG_SUB && arg >= 5") && ui.includes("page == PG_FLT2 && arg != 5") && ui.includes("fxView_ == FX_DELAY && bd.arg == 1") && ui.includes("page == PG_MOD && (arg == 0 || arg == 2 || arg == 3)"), "the panel still has the rules the editor dims knobs by");
const lean = fs.readFileSync(path.join(root, "firmware/src/lean/LeanPart.cpp"), "utf8");
ok(lean.includes("if (P.f2on && P.f2route == 1)") && lean.includes("p[PG_OSC1][7] > p[PG_SUB][5]"), "the voice still reads MIX F1/F2 in parallel only, and the older detune");
for (const [id, code] of [["FLT2:6", "p[PG_FLT2][6]"], ["FLT2:7", "p[PG_FLT2][7]"], ["GRIT:4", "p[PG_GRIT][4]"], ["GRIT:5", "p[PG_GRIT][5]"]]) ok(!NOT_USED[id] && lean.includes(code), "the voice uses " + id);
const engine = ["app/Ui.cpp", "app/Sequencer.cpp", "app/MidiMap.cpp", "audio/Instrument.cpp", "audio/PatchMap.cpp", "lean/LeanPart.cpp"].map((f) => fs.readFileSync(path.join(root, "firmware/src", f), "utf8")).join("\n");
ok(NOT_USED["GLOB:2"] && !engine.includes("[PG_GLOB][2]"), "the firmware still ignores GLOB:2");
ok(NOT_USED["GLOB:4"] && engine.includes("[PG_GLOB][4]") && fs.readFileSync(path.join(root, "firmware/src/app/Params.cpp"), "utf8").includes("pg[PG_GLOB][4] = clock;"), "CLOCK is the project's: the editor says so, and a preset leaves it alone");
ok(ui.includes("the part's own is kept in the file, unused"), "the panel still leaves the part's MIDI CH unused");

// ---- the words: one line for every page and knob, none for a knob that is gone
for (const page of [...S.pages, ...S.fx]) {
  const h = HELP[page.key];
  ok(h && h.page, "a page has its line: " + page.key);
  const names = new Set(page.knobs.filter((k) => !k.unused).map((k) => k.name));
  page.knobs.forEach((k, i) => { if (!k.unused) ok(!!h[k.name] !== !!NOT_USED[`${page.key}:${i}`], `${page.key} ${k.name} has its line, or says the firmware ignores it`); });
  for (const n of Object.keys(h)) ok(n === "page" || names.has(n), `${page.key} ${n} is a knob on that page`);
}
ok(Object.keys(HELP).length === S.pages.length + S.fx.length, "no words for a page that is gone");

// ---- a new preset
const blank = M.blank();
ok(P.parsePreset(blank).ok, "a new preset is valid: " + P.parsePreset(blank).why);
for (const s of M.SLOTS) if (!s.fx && !s.knob.unused) ok(M.read(blank, s.id) === s.knob.def, "a new preset's knob is at the panel's default: " + s.id);
ok(M.id(blank) === F.firstUser && same([blank[F.at.bank], blank[F.at.bank + 1]], [255, 255]) && blank[F.at.mask] === 0, "with the first user number, no wavetable and no effects of its own");
ok(M.name(blank) === "New preset" && M.cat(blank) === 1 && M.voices(blank) === 2, "its name, type and voices");
ok(S.fx.every((e) => e.knobs.every((_, k) => blank[e.at + k] === 0)), "it sends nothing, and its effect settings are empty");
ok(M.SLOTS.every((s) => !s.knob.unused || blank[s.at] === 0), "and the slots with no knob are empty");

// ---- every value of every knob, on a new preset and on real ones
const blob = [];
const onlyChanged = (a, b, places, what) => {
  const crc = (i) => i >= F.at.crc;
  for (let i = 0; i < F.size; ++i) if (a[i] !== b[i] && !crc(i) && !places.includes(i)) { ok(false, `${what}: byte ${i} moved as well`); return; }
  ok(true, what);
};
const full = files.find((d) => d[F.at.mask] === 15 && d[F.at.bank] !== 255) || files.find((d) => d[F.at.mask] === 15);
const dry = files.find((d) => d[F.at.mask] === 0);
const stale = files.find((d) => d[F.at.values + 7] !== 0);   // (one that has a value in the slot of the retired knob)
ok(!!full && !!dry && !!stale, "the seeds were found");
for (const seed of [blank, full, dry, stale]) {
  for (const s of M.SLOTS) {
    if (s.knob.unused) continue;
    const start = Uint8Array.from(seed);
    if (s.fx && s.fx.mask) M.setCarried(start, s.fx.key, true);
    const wave = s.knob.wave ? S.pages.indexOf(s.page) : -1;
    if (wave >= 0) M.setWave(start, wave, { basic: 0 });
    for (let v = s.knob.min; v <= s.knob.max; ++v) {
      const b = Uint8Array.from(start);
      M.write(b, s.id, v);
      const p = P.parsePreset(b);
      if (!p.ok || M.read(b, s.id) !== v) ok(false, `${s.id} = ${v}: ${p.why}, reads ${M.read(b, s.id)}`); else ++checks;
      onlyChanged(start, b, [s.at], `${s.id} = ${v} moves that byte alone`);
      blob.push(b);
    }
  }
  for (let osc = 0; osc < 2; ++osc) {
    for (let w = 0; w < F.basicWaves; ++w) { const b = Uint8Array.from(seed); M.setWave(b, osc, { basic: w }); ok(P.parsePreset(b).ok && M.wave(b, osc).basic === w, "a basic wave"); onlyChanged(seed, b, [F.at.bank + osc, F.at.values + osc * F.perPage], "a basic wave moves the wave's bytes alone"); blob.push(b); }
    for (let n = 0; n < F.banks; ++n) { const b = Uint8Array.from(seed); M.setWave(b, osc, { bank: n }); ok(P.parsePreset(b).ok && M.wave(b, osc).bank === n, "a bank"); onlyChanged(seed, b, [F.at.bank + osc], "a bank moves the bank's byte alone"); blob.push(b); }
  }
  for (let c = 1; c < S.cats.length; ++c) { const b = Uint8Array.from(seed); M.setCat(b, c); ok(M.cat(b) === c && P.parsePreset(b).ok, "a category"); onlyChanged(seed, b, [F.at.cat], "a category moves its byte alone"); blob.push(b); }
  for (let v = 1; v <= F.voicesMax; ++v) { const b = Uint8Array.from(seed); M.setVoices(b, v); ok(M.voices(b) === v && P.parsePreset(b).ok, "voices"); onlyChanged(seed, b, [F.at.voices], "voices move their byte alone"); blob.push(b); }
  const nameBytes = Array.from({ length: F.nameField }, (_, i) => F.at.name + i);
  for (let code = 0x20; code <= 0x7E; ++code) {
    const t = code === 0x20 ? "A " : String.fromCharCode(code) + "x";
    const b = Uint8Array.from(seed); M.setName(b, t);
    ok(M.name(b) === t && P.parsePreset(b).ok, "a name with the character " + code);
    onlyChanged(seed, b, nameBytes, "a name moves the name's bytes alone");
    blob.push(b);
  }
  for (let n = 1; n <= F.nameMax; ++n) { const b = Uint8Array.from(seed); M.setName(b, "N".repeat(n)); ok(M.name(b).length === n && P.parsePreset(b).ok, "a name of length " + n); blob.push(b); }
  for (const id of [F.idMin, F.firstUser, 9999, 10000, F.idMax]) { const b = M.withId(seed, id); ok(P.parsePreset(b).ok && P.parsePreset(b).id === id, "a number"); onlyChanged(seed, b, [F.at.id, F.at.id + 1], "a number moves its bytes alone"); blob.push(b); }
  // what a preset carries: all sixteen
  const carry = S.fx.filter((e) => e.mask);
  for (let m = 0; m < 16; ++m) {
    const b = Uint8Array.from(seed);
    for (const e of carry) M.setCarried(b, e.key, !!(m & e.mask));
    ok(b[F.at.mask] === m && P.parsePreset(b).ok, "the effects' mask " + m);
    blob.push(b);
  }
}

// ---- switching an effect on and off
{
  const d = Uint8Array.from(dry), e = S.fx.find((x) => x.key === "REVERB");
  M.setCarried(d, "REVERB", true);
  ok(e.knobs.every((k, i) => M.read(d, `REVERB:${i}`) === k.def), "an effect the file never held starts at the panel's defaults");
  M.write(d, "REVERB:1", 99);
  M.setCarried(d, "REVERB", false);
  ok(M.read(d, "REVERB:1") === 99, "switched off, its settings stay in the file");
  M.setCarried(d, "REVERB", true);
  ok(M.read(d, "REVERB:1") === 99, "and come back");
  const c = Uint8Array.from(full), before = Uint8Array.from(c);
  M.setCarried(c, "CHORUS", false);
  M.setCarried(c, "CHORUS", true);
  ok(same(c, before), "off and on again is the same file");
  const t = Uint8Array.from(dry);
  M.setCarried(t, "DELAY", true, dry);
  M.write(t, "DELAY:2", 77);
  M.setCarried(t, "DELAY", false, dry);
  ok(same(t, dry), "an effect the file did not have, switched on and off, leaves the file it was");
}

// ---- every shipped preset, read and written back knob by knob: nothing moves
for (const d of files) {
  const b = Uint8Array.from(d);
  for (const s of M.SLOTS) if (!s.knob.unused && !s.knob.wave) { const v = M.read(b, s.id); if (b[s.at] === (v & 255)) M.write(b, s.id, v); else ok(false, "a shipped value is out of range: " + s.id); }
  M.setName(b, M.name(b)); M.setCat(b, M.cat(b)); M.setVoices(b, M.voices(b));
  for (const e of S.fx) if (e.mask) M.setCarried(b, e.key, M.carried(b, e.key));
  ok(same(b, d), "written back knob by knob, a shipped preset is the same file: " + M.name(d));
  ok(M.strays(d) === 0, "and it has no value outside a knob's range");
}

// ---- what the editor must not do
ok(refused(() => M.write(Uint8Array.from(blank), "FLT1:1", 128)), "a value above the knob is refused");
ok(refused(() => M.write(Uint8Array.from(blank), "OCT", 0)), "a knob that does not exist is refused");
ok(refused(() => M.write(Uint8Array.from(blank), "OSC1:7", 1)), "the retired knob cannot be written");
ok(refused(() => M.write(Uint8Array.from(blank), "OSC1:0", 4)), "WAVE holds a basic wave: a bank goes in the bank field");
ok(refused(() => M.setName(Uint8Array.from(blank), "")) && refused(() => M.setName(Uint8Array.from(blank), " Name")) && refused(() => M.setName(Uint8Array.from(blank), "x".repeat(21))) && refused(() => M.setName(Uint8Array.from(blank), "Blå")), "names the firmware refuses are refused");
ok(refused(() => M.setCat(Uint8Array.from(blank), 0)) && refused(() => M.setCat(Uint8Array.from(blank), 12)) && refused(() => M.setVoices(Uint8Array.from(blank), 0)) && refused(() => M.setVoices(Uint8Array.from(blank), 6)), "a category or voice count out of range is refused");
ok(refused(() => M.withId(blank, 9)) && refused(() => M.withId(blank, 32001)), "a number out of range is refused");
ok(M.nameProblem("Bad\x7F") !== null && M.nameProblem("Bad\x1F") !== null && M.nameProblem("~ok ") === null, "the name's characters are the firmware's: 0x20 to 0x7E");
ok(refused(() => M.setWave(Uint8Array.from(blank), 0, { bank: F.banks })) && refused(() => M.setWave(Uint8Array.from(blank), 2, { basic: 0 })), "a bank that is not in the catalogue is refused");
{ const b = Uint8Array.from(blank); M.withId(b, 2000); ok(same(b, blank), "the same file under another number leaves the first alone: the editor's copy for the engine must not renumber the preset"); }
{ const b = Uint8Array.from(blank); b[M.slot("OSC1:7").at] = 200; ok(M.strays(b) === 1, "a slot with no knob can stray too"); }
{ // a hand-made file strays: shown in range, left alone until that knob moves
  const b = Uint8Array.from(blank); b[M.slot("OSC1:3").at] = 100; b[M.slot("SENDS:0").at] = 200;
  ok(M.read(b, "OSC1:3") === 2 && M.read(b, "SENDS:0") === 127 && M.strays(b) === 2, "a value outside its range reads as the firmware brings it in");
}

// ---- the number of a new preset
const list = (...ids) => ids.map((id) => ({ id }));
ok(M.nextFreeId(list(0, 1, 2, 10, 500)) === 1000, "the first new preset is 1000");
ok(M.nextFreeId(list(0, 1, 2, 1000, 1001, 1005)) === 1006, "then past the highest, as the firmware's presetNextId");
ok(M.nextFreeId(list(1000, F.idMax)) === 1001, "at the top: the first free one");
ok(M.nextFreeId(Array.from({ length: F.idMax - 999 }, (_, i) => ({ id: 1000 + i }))) === -1, "no number left");

// ---- the firmware and tools/prs.py on all of it
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "nemesis-presets-"));
const write = (name, recs) => { const p = path.join(tmp, name); fs.writeFileSync(p, Buffer.concat(recs.map((b) => Buffer.from(b)))); return p; };
const accept = path.join(root, "tests/build/preset_accept");
if (!fs.existsSync(accept)) ok(false, "no tests/build/preset_accept: build the host tests first");
else {
  const all = write("all.bin", blob);
  const r = spawnSync(accept, [all], { encoding: "utf8" });
  ok(r.status === 0, `the firmware's parsePresetFile takes every record unchanged: ${r.stdout}${r.stderr}`);
  console.log("firmware: " + r.stdout.trim());
  const py = spawnSync("python3", [path.join(root, "web/test/prs_roundtrip.py"), all], { encoding: "utf8" });
  ok(py.status === 0, `tools/prs.py reads every record and writes the same bytes: ${py.stdout}${py.stderr}`);
  console.log("prs.py: " + py.stdout.trim());
  // the check can fail: records the firmware must refuse or change
  const bad = [];
  const poke = (i, v) => { const b = Uint8Array.from(blank); b[i] = v; b.set(P.u32(P.crc32(b.subarray(0, F.at.crc))), F.at.crc); bad.push(b); };
  poke(F.at.values, 4); poke(F.at.values + 3, 3); poke(M.slot("SENDS:0").at, 128); poke(F.at.cat, 0); poke(F.at.voices, 6); poke(F.at.mask, 16); poke(F.at.bank, 200); poke(F.at.name, 0); poke(M.slot("DELAY:0").at, 8);
  const flipped = Uint8Array.from(blank); flipped[50] ^= 1; bad.push(flipped);
  const r2 = spawnSync(accept, ["--refused", write("bad.bin", bad)], { encoding: "utf8" });
  ok(r2.status === 0, `the firmware refuses or changes every bad record: ${r2.stdout}${r2.stderr}`);
  // the file names: the page's and the firmware's
  const names = ["Plain", "A/B: C", "Tab_ok-(1).x", "#%&*", "Trailing ", "x".repeat(20)].map((t) => { const b = M.setName(M.withId(blank, 1234), t); return b; });
  names.push(M.withId(M.setName(Uint8Array.from(blank), "Five digits"), 10234));
  const r3 = spawnSync(accept, ["--names", write("names.bin", names)], { encoding: "utf8" });
  const lines = r3.stdout.split("\n").slice(0, names.length);
  ok(same(lines, names.map((b) => P.fileName(M.id(b), M.name(b)))), `the page names a file as the firmware does: ${lines.join(" | ")}`);
}
fs.rmSync(tmp, { recursive: true, force: true });
console.log(checks + " checks, " + fails + " failed");
process.exit(fails ? 1 : 0);
