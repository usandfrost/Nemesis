"use strict";
// What the preset editor does to a .PRS file. The 148 bytes are the preset: every edit writes the bytes of the one value that changed and the CRC at the end, and nothing else, so what the
// editor does not know about stays as it was. No page and no browser in here (web/test/preset-edit.mjs runs it in Node). The numbers come from preset-schema.js, made from the firmware's tables.
(() => {
  const P = globalThis.PresetSysex, S = globalThis.PresetSchema, F = S.format, AT = F.at;
  const NO_BANK = F.noBank, OSC_PAGES = 2;
  const BLANK = { name: "New preset", cat: 1, voices: 2 };

  const seal = (b) => { b.set(P.u32(P.crc32(b.subarray(0, AT.crc))), AT.crc); return b; };
  const signed = (x) => (x > 127 ? x - 256 : x);
  const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

  // Every knob that has a place in the file: a page's eight, the sends, and each effect's eight.
  const SLOTS = [];
  S.pages.forEach((page, p) => page.knobs.forEach((knob, k) => SLOTS.push({ id: `${page.key}:${k}`, page, knob, k, at: AT.values + p * F.perPage + k, signed: true })));
  for (const page of S.fx) page.knobs.forEach((knob, k) => SLOTS.push({ id: `${page.key}:${k}`, page, knob, k, at: page.at + k, signed: false, fx: page }));
  const slotAt = new Map(SLOTS.map((s) => [s.id, s]));
  const slot = (id) => { const s = slotAt.get(id); if (!s) throw new RangeError("no such knob: " + id); return s; };

  // The value as the firmware reads the file: one may stray outside a knob's range, and is brought into it (parsePresetFile). The byte stays until someone moves that knob.
  const raw = (b, s) => (s.signed ? signed(b[s.at]) : b[s.at]);
  const read = (b, id) => { const s = slot(id); return clamp(raw(b, s), s.knob.min, s.knob.max); };
  function write(b, id, v) {
    const s = slot(id);
    if (!Number.isInteger(v) || v < s.knob.min || v > s.knob.max || s.knob.unused) throw new RangeError(`${id}: ${v}`);
    b[s.at] = v & 255;
    return seal(b);
  }
  // How many values the file holds outside their knobs' ranges.
  const strays = (b) => SLOTS.filter((s) => { const v = raw(b, s); return v < s.knob.min || v > s.knob.max; }).length;

  // ---- the head of the file
  const name = (b) => P.nameOf(b.subarray(AT.name, AT.name + F.nameField));
  // Why a name cannot be used, in words, or null (presetNameOk).
  function nameProblem(t) {
    if (!t.length) return "A preset needs a name.";
    if (t.length > F.nameMax) return `A name has ${F.nameMax} characters at most.`;
    if (t[0] === " ") return "A name cannot start with a space.";
    for (const c of t) { const n = c.codePointAt(0); if (n < 0x20 || n > 0x7E) return `A name can only use plain keyboard characters: A to Z, digits and signs. "${c}" is not one.`; }
    return null;
  }
  function setName(b, t) {
    const why = nameProblem(t);
    if (why) throw new RangeError(why);
    b.fill(0, AT.name, AT.name + F.nameField);
    for (let i = 0; i < t.length; ++i) b[AT.name + i] = t.charCodeAt(i);
    return seal(b);
  }
  const id = (b) => P.rd16(b, AT.id);
  const cat = (b) => b[AT.cat];
  const voices = (b) => b[AT.voices];
  function setCat(b, c) { if (!Number.isInteger(c) || c < 1 || c >= S.cats.length) throw new RangeError("category " + c); b[AT.cat] = c; return seal(b); }
  function setVoices(b, v) { if (!Number.isInteger(v) || v < 1 || v > F.voicesMax) throw new RangeError("voices " + v); b[AT.voices] = v; return seal(b); }
  // The same file under another number.
  function withId(b, n) {
    if (!Number.isInteger(n) || n < F.idMin || n > F.idMax) throw new RangeError("number " + n);
    const c = Uint8Array.from(b);
    c.set(P.u16(n), AT.id);
    return seal(c);
  }

  // ---- an oscillator's wave: a basic wave is the page's WAVE value, a bank is in the bank field and wins over it
  function wave(b, osc) {
    const bank = b[AT.bank + osc];
    return bank !== NO_BANK ? { bank } : { basic: read(b, `${S.pages[osc].key}:0`) };
  }
  function setWave(b, osc, w) {
    if (osc < 0 || osc >= OSC_PAGES) throw new RangeError("oscillator " + osc);
    if (w.bank !== undefined) {
      if (!Number.isInteger(w.bank) || w.bank < 0 || w.bank >= F.banks) throw new RangeError("bank " + w.bank);
      b[AT.bank + osc] = w.bank;
      return seal(b);
    }
    b[AT.bank + osc] = NO_BANK;
    return write(b, `${S.pages[osc].key}:0`, w.basic);
  }

  // ---- what the preset carries: the mask says which of the effect settings count. A setting the file never held (all zeros: the amp and shared effects of a preset without them)
  // starts from the panel's defaults, which is what that effect would show. One that is switched off keeps its bytes, so switching it on again brings them back; but if `base`, the file as
  // it was opened, did not carry it either, its bytes go back to base's: switched on and off, it is the file it was.
  const fxPage = (key) => { const e = S.fx.find((x) => x.key === key); if (!e || !e.mask) throw new RangeError("not an effect a preset can carry: " + key); return e; };
  const carried = (b, key) => !!(b[AT.mask] & fxPage(key).mask);
  function setCarried(b, key, on, base) {
    const e = fxPage(key);
    if (on && !carried(b, key) && e.knobs.every((_, k) => b[e.at + k] === 0)) e.knobs.forEach((knob, k) => (b[e.at + k] = knob.def));
    if (!on && base && !carried(base, key)) e.knobs.forEach((_, k) => (b[e.at + k] = base[e.at + k]));
    b[AT.mask] = on ? b[AT.mask] | e.mask : b[AT.mask] & ~e.mask;
    return seal(b);
  }

  // A new preset: every knob at its panel default, no wavetable, nothing sent to the effects, no effect settings of its own.
  function blank() {
    const b = new Uint8Array(F.size);
    b.set([0x44, 0x42, 0x50, 0x52, 1, 0]);
    b.set(P.u16(F.firstUser), AT.id);
    b[AT.cat] = BLANK.cat;
    b[AT.voices] = BLANK.voices;
    b[AT.bank] = b[AT.bank + 1] = NO_BANK;
    for (const s of SLOTS) if (!s.fx) b[s.at] = s.knob.def & 255;
    return setName(b, BLANK.name);
  }

  // The number a new preset gets, as the firmware's presetNextId: past the highest in use from 1000 up, so a number that was deleted does not come back while there is room above (a project
  // stores numbers). At the top, the first free one. -1: none.
  function nextFreeId(list) {
    const used = new Set(list.map((e) => e.id));
    const top = Math.max(F.firstUser - 1, ...used);
    if (top < F.idMax) return top + 1;
    for (let n = F.firstUser; n <= F.idMax; ++n) if (!used.has(n)) return n;
    return -1;
  }

  globalThis.PresetModel = { SLOTS, slot, read, write, strays, name, nameProblem, setName, id, cat, voices, setCat, setVoices, withId, wave, setWave, carried, setCarried, blank, nextFreeId };
})();
