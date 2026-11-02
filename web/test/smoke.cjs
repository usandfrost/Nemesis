// Node smoke test of the module: loads the library and a preset, plays a note through each engine, prints levels and writes WAVs.
//   node web/test/smoke.cjs PRESET_ID [OUT_PREFIX]
const fs = require("fs"), path = require("path");
const www = path.join(__dirname, "..", "www"), root = path.join(__dirname, "..", "..");
const createDrumboy = require(path.join(www, "drumboy.js"));
const FOLDERS = ["presets", "presets2", "synthwave", "techno", "synthpop", "trance", "ebm", "royksopp", "delerium", "voice"];
(async () => {
  const M = await createDrumboy();
  const put = (bytes) => { const p = M._malloc(bytes.length); M.HEAPU8.set(bytes, p); return p; };
  M._wb_init();
  let n = 0;
  for (const f of FOLDERS) for (const name of fs.readdirSync(path.join(root, "content", f))) {
    if (!name.toUpperCase().endsWith(".PRS") || name.startsWith(".")) continue;
    const b = fs.readFileSync(path.join(root, "content", f, name));
    const np = put(Buffer.from(name + "\0")); const dp = put(b);
    M._wb_preset_add(np, dp, b.length); M._free(np); M._free(dp); n++;
  }
  console.log("files", n, "library", M._wb_presets_ready(), "presets in list", M._wb_preset_count());
  const id = parseInt(process.argv[2] || "11"), prefix = process.argv[3];
  const rootNote = parseInt(process.env.ROOT || "52"), NOTES = (process.env.CHORD ? [0, 3, 7, 10] : [0]).map((i) => rootNote + i);   // ROOT=note, CHORD=1 plays the bench's chord
  const bk = M._malloc(8); M._wb_preset_banks(id, bk);
  const h32 = new Int32Array(M.HEAPU8.buffer, bk, 2); const banks = [h32[0], h32[1]].filter((b) => b >= 0);
  for (const b of banks) { const f = fs.readFileSync(path.join(root, "content/data/wtp", "wt" + String(b).padStart(3, "0") + ".wtp")); const p = put(f); M._wb_bank_add(b, p, f.length); M._free(p); }
  const name = M.UTF8ToString(M._wb_preset_name(id));
  console.log("preset", id, name, "banks", banks, "load", M._wb_load_preset(id));
  const L = M._malloc(128 * 4), R = M._malloc(128 * 4);
  const render = (sec) => { const out = []; for (let i = 0; i < Math.round(sec * 44100 / 128); i++) { M._wb_render(L, R, 128); out.push(new Float32Array(M.HEAPU8.buffer, L, 128).slice(), new Float32Array(M.HEAPU8.buffer, R, 128).slice()); } return out; };
  for (const eng of [0, 1, 0]) {
    M._wb_set_engine(eng); render(0.2);
    M._wb_clear_peaks(); for (const nt of NOTES) M._wb_note_on(nt, 100);
    const a = render(1.0); const voices = M._wb_voices(); for (const nt of NOTES) M._wb_note_off(nt); const b = render(0.8);
    let e = 0, k = 0; for (const blk of a) for (const x of blk) { e += x * x; k++; }
    console.log("engine", M._wb_engine(), "rms(hold)", Math.sqrt(e / k).toFixed(4), "peak", M._wb_peak(0).toFixed(3), "voices", voices);
    if (prefix) { const all = [...a, ...b]; const lf = []; for (let i = 0; i < all.length; i += 2) lf.push(all[i]); fs.writeFileSync(prefix + "_e" + eng + ".f32", Buffer.from(Float32Array.from(lf.flatMap((x) => [...x])).buffer)); }
    M._wb_all_off(); render(0.2);
  }
})();
