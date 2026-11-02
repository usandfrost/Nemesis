// The instrument in a Web Worker: runs the WebAssembly module, keeps a ring buffer (shared with the audio worklet) filled ahead of the sound card,
// fetches the preset library and the wavetable banks it needs, and answers the page's messages.
importScripts(self.location.search.includes("hr=1") ? "drumboy_hr.js" : "drumboy.js");   // ?hr=1: the effects at half the sample rate

const BLOCK = 128, AHEAD = 1536;   // frames per render call; how far ahead of the sound card the ring is kept
const STATE = 83;                  // the values wb_state gives (web_api.cpp: kStateValues)
let M = null, hdr = null, ringL = null, ringR = null, cap = 0;
let pL = 0, pR = 0;
const haveBank = new Set();
const loading = new Map();         // bank -> promise
let pumping = false;

const mem = () => M.HEAPU8.buffer;   // (the heap can move when it grows: views are taken fresh)

const badBank = new Set();   // a bank that could not be fetched: asked for once, not on every frame
async function fetchBank(b) {
  if (haveBank.has(b)) return;
  if (loading.has(b)) return loading.get(b);
  const p = (async () => {
    const r = await fetch("wtp/wt" + String(b).padStart(3, "0") + ".wtp");
    if (!r.ok) throw new Error("bank " + b + ": " + r.status);
    const bytes = new Uint8Array(await r.arrayBuffer());
    const ptr = M._malloc(bytes.length);
    M.HEAPU8.set(bytes, ptr);
    M._wb_bank_add(b, ptr, bytes.length);
    M._free(ptr);
    haveBank.add(b);
  })();
  loading.set(b, p);
  try { await p; } finally { loading.delete(b); }
}

function presetBanks(id) {
  const p = M._malloc(8);
  M._wb_preset_banks(id, p);
  const v = new Int32Array(mem(), p, 2);
  const out = [v[0], v[1]].filter((b) => b >= 0);
  M._free(p);
  return out;
}

function cstr(s) {
  const bytes = new TextEncoder().encode(s + "\0");
  const p = M._malloc(bytes.length);
  M.HEAPU8.set(bytes, p);
  return p;
}

async function loadLibrary() {
  const buf = new Uint8Array(await (await fetch("data/prs.bin")).arrayBuffer());
  let o = 0;
  while (o < buf.length) {
    const nl = buf[o++];
    const name = new TextDecoder().decode(buf.subarray(o, o + nl)); o += nl;
    const size = buf[o] | (buf[o + 1] << 8); o += 2;
    const np = cstr(name), dp = M._malloc(size);
    M.HEAPU8.set(buf.subarray(o, o + size), dp); o += size;
    M._wb_preset_add(np, dp, size);
    M._free(np); M._free(dp);
  }
  M._wb_presets_ready();
  const list = [];
  for (let k = 0, n = M._wb_preset_count(); k < n; ++k) {
    const id = M._wb_preset_id(k);
    list.push({ id, name: M.UTF8ToString(M._wb_preset_name(id)), cat: M.UTF8ToString(M._wb_cat_name(M._wb_preset_cat(id))), voices: M._wb_preset_voices(id), banks: presetBanks(id) });
  }
  return list;
}

// A preset onto part A, its banks first. `missingOk`: a bank that cannot be fetched plays its nearest basic wave, as a bank that is not on the card does.
async function loadPreset(m, missingOk) {
  const banks = presetBanks(m.id);
  const bad = (await Promise.allSettled(banks.map(fetchBank))).find((r) => r.status === "rejected");
  if (bad) {
    postMessage({ ev: "error", msg: "wavetable bank: " + bad.reason });
    if (!missingOk) { postMessage({ ev: "loaded", id: m.id, ok: 0, banks, ms: 0, seq: m.seq }); return; }
  }
  const ok = M._wb_load_preset(m.id);
  let waited = 0;
  while (!M._wb_ready() && waited < 3000) { await new Promise((r) => setTimeout(r, 20)); waited += 20; }
  postMessage({ ev: "loaded", id: m.id, ok, banks, ms: waited, seq: m.seq });
}

function pump() {
  if (!M || !hdr) return;
  let w = Atomics.load(hdr, 1);
  const r = Atomics.load(hdr, 0);
  let guard = 0;
  while (((w - r) | 0) < AHEAD && guard++ < 64) {   // (the positions wrap at 2^31, after 13 hours: the ring's size is a power of two, so that does no harm)
    M._wb_render(pL, pR, BLOCK);
    const l = new Float32Array(mem(), pL, BLOCK), rr = new Float32Array(mem(), pR, BLOCK);
    const at = w & (cap - 1);
    ringL.set(l, at);
    ringR.set(rr, at);
    w = (w + BLOCK) | 0;
    Atomics.store(hdr, 1, w);
    if (M._wb_exporting()) break;   // (a slice of a render has gone by: STOP and the screen get a turn)
  }
  setTimeout(pump, 2);
}

onmessage = async (e) => {
  const m = e.data;
  if (!M && m.cmd !== "init") return;   // (a new worker: the page's meters ask before it has started)
  try {
    switch (m.cmd) {
      case "init": {
        M = await createDrumboy();
        M._wb_init();
        cap = m.cap;
        hdr = new Int32Array(m.sab, 0, 4);
        ringL = new Float32Array(m.sab, 16, cap);
        ringR = new Float32Array(m.sab, 16 + cap * 4, cap);
        pL = M._malloc(BLOCK * 4); pR = M._malloc(BLOCK * 4);
        const list = await loadLibrary();
        postMessage({ ev: "ready", list });
        pump();
        break;
      }
      case "load": await loadPreset(m, false); break;
      case "audition": {   // the librarian's editor: a preset that is only on the page goes in as one more file, under one name that is overwritten each time
        const np = cstr("EDITING.PRS"), dp = M._malloc(m.bytes.length);
        M.HEAPU8.set(m.bytes, dp);
        M._wb_preset_add(np, dp, m.bytes.length);
        M._free(np); M._free(dp);
        M._wb_presets_ready();
        M._wb_load_preset(m.id);   // (the list has this file's banks once the file has been read)
        await loadPreset(m, true);
        break;
      }
      case "noteOn": M._wb_note_on(m.note, m.vel); break;
      case "noteOff": M._wb_note_off(m.note); break;
      case "allOff": M._wb_all_off(); break;
      case "engine": M._wb_set_engine(m.lean ? 1 : 0); break;
      case "preview": M._wb_device_preview(m.id); break;
      case "master": M._wb_master(m.v); break;
      case "key": M._wb_key(m.k, m.down ? 1 : 0); break;
      case "layer": M._wb_layer(m.i, m.down ? 1 : 0); break;
      case "turn": M._wb_turn(m.enc, m.d); break;
      case "knob": M._wb_knob_to(m.enc, m.to); break;
      case "trackerNote": M._wb_tracker_note(m.offset); break;
      case "stats": postMessage({ ev: "stats", peakL: M._wb_peak(0), peakR: M._wb_peak(1), voices: M._wb_voices(), engine: M._wb_engine() }); M._wb_clear_peaks(); break;
      case "bootImage": {
        const p = M._malloc(960 * 540 * 4);
        M._wb_boot_image(p);
        const px = new Uint8ClampedArray(mem(), p, 960 * 540 * 4).slice();
        M._free(p);
        postMessage({ ev: "bootImage", px: px.buffer }, [px.buffer]);
        break;
      }
      case "frame": {
        if (M._wb_export_count() > 0) {
          const files = [];
          for (let i = 0, n = M._wb_export_count(); i < n; ++i) files.push({ name: M.UTF8ToString(M._wb_export_name(i)), bytes: new Uint8Array(mem(), M._wb_export_data(i), M._wb_export_size(i)).slice() });
          M._wb_export_clear();
          postMessage({ ev: "export", files }, files.map((f) => f.bytes.buffer));
        }
        const p = M._malloc(960 * 540 * 4);
        M._wb_frame(p);
        const px = new Uint8ClampedArray(mem(), p, 960 * 540 * 4).slice();
        M._free(p);
        const st = M._malloc(STATE * 4);
        M._wb_state(st);
        const state = Array.from(new Int32Array(mem(), st, STATE));
        M._free(st);
        const kb = M._malloc(32);
        M._wb_knob_places(kb);
        const knobs = Array.from(new Float32Array(mem(), kb, 8));
        M._free(kb);
        const want = M._wb_missing_bank();
        if (want >= 0 && !badBank.has(want)) fetchBank(want).catch((e) => { badBank.add(want); postMessage({ ev: "error", msg: "wavetable bank: " + e }); });
        postMessage({ ev: "frame", px: px.buffer, rev: M._wb_revision(), leds: M._wb_layer_leds(Date.now() >>> 0), knobs, state }, [px.buffer]);
        break;
      }
    }
  } catch (err) {
    postMessage({ ev: "error", msg: String(err && err.stack || err) });
  }
};
