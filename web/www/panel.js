// The virtual Drumboy: the front plate drawn in the hardware's own layout (a black plate, rounded keys with line glyphs, white-ringed encoders, the
// ten clear step keys under the screen), with the firmware's real 960x540 screen in the middle. Positions are percentages of the plate, measured from
// the device's photograph (centres of the keys, encoders and screen); everything is drawn here (no image of the device is used).
// buildPlate({send, startAudio, master}) fills #panelWrap and returns {canvas}.

const PLATE = { w: 1588, h: 902, x0: 172, y0: 100 };   // the photograph's plate: the origin and size the numbers below were taken from
const px = (x) => ((x - PLATE.x0) / PLATE.w) * 100, py = (y) => ((y - PLATE.y0) / PLATE.h) * 100;

// Glyphs on a 100 x 100 box, stroked (thin, round caps), as the keys print them. Shapes and sizes are measured from the photograph of the panel: the box is 68% of a
// key (style.css), so 163 units are one key pitch. Note the two filter keys carry the same curve, and the small ring on the left block is not the transport's ring.
const G = {
  stop: '<rect x="26" y="26" width="48" height="48" rx="5"/>',
  play: '<path d="M27 29 Q27 20.5 34.5 24.5 L72 45.7 Q79 50 72 54.3 L34.5 75.5 Q27 79.5 27 71 Z"/>',
  ring: '<circle cx="50" cy="50" r="24"/>',
  ringS: '<circle cx="50" cy="50" r="14.7"/>',
  rec: '<circle cx="50" cy="50" r="15" fill="currentColor"/>',
  plus: '<path d="M50 25.5 V74.5 M25.5 50 H74.5"/>',
  minus: '<path d="M25.5 50 H74.5"/>',
  left: '<path d="M73 29 Q73 20.5 65.5 24.5 L28 45.7 Q21 50 28 54.3 L65.5 75.5 Q73 79.5 73 71 Z"/>',
  right: '<path d="M27 29 Q27 20.5 34.5 24.5 L72 45.7 Q79 50 72 54.3 L34.5 75.5 Q27 79.5 27 71 Z"/>',
  bars: '<path d="M26 26 V74 M50 26 V74 M74 26 V74"/>',
  sine: '<path d="M11.5 50 C17.8 35.1 23.8 21 30.8 21 S43.7 35.1 50 50 S62.3 79 69.3 79 S82.2 64.9 88.5 50"/>',
  fork: '<path d="M32 15.5 V34 C32 46 40 53.5 50 53.5 C60 53.5 68 46 68 34 V15.5 M50 53.5 V84.5"/>',
  sub: '<circle cx="50" cy="50" r="24.5"/><circle cx="7.5" cy="50" r="5" fill="currentColor" stroke="none"/><circle cx="92.5" cy="50" r="5" fill="currentColor" stroke="none"/>',
  lp: '<path d="M9.5 24 H39.4 C62 24 88.5 44 90.2 77.7"/>',
  sliders: '<path d="M29.2 23.5 V40.2 M29.2 59.8 V76.5 M70.8 23.5 V40.2 M70.8 59.8 V76.5"/><circle cx="29.2" cy="50" r="9.8"/><circle cx="70.8" cy="50" r="9.8"/>',
  tgt1: '<circle cx="50" cy="50" r="34"/><circle cx="50" cy="50" r="7.5" fill="currentColor" stroke="none"/>',
  tgt2: '<circle cx="50" cy="50" r="34"/><circle cx="50" cy="50" r="12.4"/>',
  grid: '<g fill="currentColor" stroke="none">' + [26, 50, 74].map((y) => [26, 50, 74].map((x) => `<circle cx="${x}" cy="${y}" r="5"/>`).join("")).join("") + "</g>",
  power: '<circle cx="50" cy="50" r="34"/><path d="M50 48 V84"/>',
};
const glyph = (name) => `<svg viewBox="0 0 100 100" aria-hidden="true">${G[name]}</svg>`;
const letter = (t, small) => `<span class="ch${small ? " small" : ""}">${t}</span>`;

// [key index (app::Key), column x, row y (photo pixels), face, what it is]
const KEYS = [
  [0, 310, 210, glyph("stop"), "STOP"], [1, 413, 210, glyph("play"), "PLAY"], [2, 516, 210, glyph("ring"), "REC"],
  [4, 310, 369, glyph("rec"), "ADD"], [3, 413, 369, glyph("plus"), "UP"], [10, 516, 369, glyph("ringS"), "CLEAR"],
  [6, 310, 470, glyph("left"), "LEFT"], [7, 413, 470, glyph("bars"), "OK"], [8, 516, 470, glyph("right"), "RIGHT"],
  [9, 310, 571, letter("C", true), "COPY"], [5, 413, 571, glyph("minus"), "DOWN"], [11, 516, 571, letter("P", true), "PASTE"],
  [12, 310, 732, letter("M"), "MUTE"], [13, 413, 732, letter("F"), "FILL"], [14, 516, 732, letter("S"), "STEP"],
  [15, 1420, 213, letter("F"), "FILE"], [16, 1521, 213, letter("D"), "PART"], [17, 1623, 213, letter("R"), "MIXER"],
  [18, 1420, 369, glyph("sine"), "OSC 1"], [19, 1521, 369, glyph("fork"), "OSC 2"], [20, 1623, 369, glyph("sub"), "SUB"],
  [21, 1420, 470, glyph("lp"), "FILTER 1"], [22, 1521, 470, glyph("lp"), "FILTER 2"], [23, 1623, 470, glyph("sliders"), "GRIT"],
  [24, 1420, 571, glyph("tgt1"), "AMP"], [25, 1521, 571, glyph("tgt1"), "MOD"], [26, 1623, 571, glyph("tgt2"), "GLOB"],
  [27, 1420, 732, letter("L"), "LANE"], [28, 1521, 732, letter("I"), "PROJECT"], [29, 1623, 732, letter("S"), "SONG"],
  [30, 1624, 891, glyph("grid"), "SHIFT"],
];
// what a few keys do, beyond their name (the hover line)
const TIPS = { SHIFT: "SHIFT (the nine dots): click it, and the next key you click is pressed with SHIFT held (or hold Shift on your keyboard)", LANE: "LANE (the L key): back one level; hold it for HOME", PROJECT: "PROJECT (the I key): key, scale and card; again for the MIDI view",
  STEP: "STEP (the S key on the left): the step editor; again for the second view, and again for the tracker", PART: "PART (the D key): the next part", MIXER: "MIXER (the R key): levels; again for voices, priority and the CPU", FX: "FX (the bottom-left key): the effects; again for the next of six views",
  REC: "REC (the large ring): the recording screen, then start and stop a take. In a browser a drum loop stands in for the line input", FILE: "FILE (the F key on the right): browse and load presets or samples (OK loads)", SONG: "SONG (the S key on the right): the song rows; again to go back",
  MUTE: "MUTE (the M key): silences the lane", FILL: "FILL (the F key on the left): a fill while held", COPY: "COPY (the C key): the lane; with SHIFT the pattern", PASTE: "PASTE (the P key): the lane; with SHIFT the pattern",
  ADD: "ADD (the dot): a new rhythm, or a song row", CLEAR: "CLEAR (the small ring): twice empties the lane, held for a second it empties the pattern, on PROJECT twice deletes the slot; with SHIFT it undoes",
  UP: "UP (the + key): the lane above, or up a list", DOWN: "DOWN (the minus key): the lane below, or down a list", OK: "OK (the three bars)",
  "OSC 1": "OSC 1 (the sine wave): oscillator 1; on a sampler lane the SAMPLE page", "OSC 2": "OSC 2 (the fork): oscillator 2; on a sampler lane the SLICE page, again for SLICE EDIT", SUB: "SUB (the ring with two dots): the sub; on a sampler lane the LOOP page",
  "FILTER 1": "FILTER 1 (the first curve)", "FILTER 2": "FILTER 2 (the second curve): not on a sampler lane", GRIT: "GRIT (the two sliders)", AMP: "AMP (the first target): the amp envelope", MOD: "MOD (the second target): the LFO", GLOB: "GLOB (the double ring): the part's global page" };
const ENC_X = [728, 886, 1044, 1202];
const LAYER_X = [512, 614, 715, 815, 916, 1016, 1115, 1215, 1316, 1417];
const SCREWS = [[218, 145], [1714, 145], [212, 955], [1713, 957], [625, 213], [1306, 213], [625, 731], [1307, 731]];

function buildPlate(h) {
  const wrap = document.getElementById("panelWrap");
  wrap.innerHTML = "";
  const el = (cls, html, x, y, w, ht, tag) => {
    const d = document.createElement(tag || "div");
    d.className = cls;
    if (html) d.innerHTML = html;
    if (x !== undefined) { d.style.left = px(x) + "%"; d.style.top = py(y) + "%"; }
    if (w) { d.style.width = (w / PLATE.w) * 100 + "%"; d.style.height = (ht / PLATE.h) * 100 + "%"; }
    plate.appendChild(d);
    return d;
  };
  const plate = document.createElement("div");
  plate.id = "plate";
  wrap.appendChild(plate);

  // the two level bars (twelve segments each, fed by the master peaks) in the side grips
  const seg12 = '<i></i>'.repeat(12);
  el("bar l", seg12, 205, 470, 16, 158); el("bar r", seg12, 1722, 470, 16, 158);
  el("jack l", "IO", 190, 288, 36, 80); el("jack r", "SD", 1735, 288, 36, 80);
  for (const [x, y] of SCREWS) el("screw", "", x, y, 22, 22);
  // the printed labels
  const lab = (t, x, y, cls) => el("lab " + (cls || ""), t, x, y);
  lab("LINE", 365, 128); lab("HP", 517, 128); lab("CODE", 623, 128); lab("SYNC", 1365, 130); lab("MIDI", 1568, 130);
  lab("DRUMBOY", 966, 270, "big"); lab("PRO EDITION", 965, 677);
  const hint = document.createElement("div");
  hint.id = "panelHint";
  hint.textContent = "Hover a key to see what it is. Drag a knob up or down: 160 pixels is its whole range. Wheel, or arrows on a clicked knob: one step.";

  // LINE ▽ △ turn the master volume (the same as the slider in the header); SYNC and MIDI have nothing to do on the web
  const tri = (x, y, up, fn, title) => {
    const b = el("tri" + (fn ? "" : " dead"), `<svg viewBox="0 0 20 20"><path d="${up ? "M10 4 L17 16 H3 Z" : "M3 4 H17 L10 16 Z"}" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"/></svg>`, x, y, 40, 40, "button");
    b.title = title;
    if (fn) b.onclick = fn;
  };
  const vol = (d) => () => { const m = h.master(); m.value = Math.max(0, Math.min(127, +m.value + d)); m.dispatchEvent(new Event("input")); hint.textContent = "LINE level " + m.value + " / 127"; };
  tri(315, 128, false, vol(-8), "LINE down"); tri(416, 128, true, vol(8), "LINE up");
  tri(1315, 130, false, null, "SYNC (no sync port here)"); tri(1416, 130, true, null, "SYNC (no sync port here)");
  tri(1517, 130, false, null, "MIDI (no MIDI port here)"); tri(1619, 130, true, null, "MIDI (no MIDI port here)");

  // the screen: the firmware's own picture
  const bez = el("bezel", "", 646 + 637 / 2, 295 + 355 / 2, 637 + 12, 355 + 12);
  const canvas = document.createElement("canvas");
  canvas.id = "screen"; canvas.width = 960; canvas.height = 540;
  bez.appendChild(canvas);

  // one pointer cannot hold SHIFT and press a key: a click on SHIFT stays down until the next key has been pressed and let go, or until SHIFT is clicked again
  let latched = null;
  const keyPressed = () => { h.startAudio(); if (document.activeElement?.classList.contains("enc")) document.activeElement.blur(); };   // (a key takes the arrow keys back from a knob)
  const press = (b, fn) => {
    b.addEventListener("pointerdown", (e) => { if (e.button !== 0) return; e.preventDefault(); keyPressed(); b.classList.add("down"); b.setPointerCapture?.(e.pointerId); fn(true); });
    const up = () => { if (b.classList.contains("down")) { b.classList.remove("down"); fn(false); if (latched) latched(); } };
    b.addEventListener("pointerup", up); b.addEventListener("pointercancel", up);
    b.addEventListener("lostpointercapture", up);
  };
  window.plateUnshift = () => { if (latched) latched(); };
  const byName = {};   // key name -> {el, send(down)}: what the computer keyboard presses, and what a lesson lights and presses (learn.js)
  window.plateKeys = byName;
  for (const [k, x, y, face, name] of KEYS) {
    const b = el("key", face, x, y, 92, 92, "button");
    byName[name] = { el: b, send: (d) => h.send({ cmd: "key", k, down: d }) };
    b.title = name; b.setAttribute("aria-label", name);
    b.addEventListener("pointerenter", () => (hint.textContent = TIPS[name] || name));
    if (name === "SHIFT") {
      b.addEventListener("pointerdown", (e) => {
        if (e.button !== 0) return;
        e.preventDefault(); keyPressed();
        if (latched) { latched(); return; }
        b.classList.add("down"); h.send({ cmd: "key", k, down: true });
        latched = () => { latched = null; b.classList.remove("down"); h.send({ cmd: "key", k, down: false }); };
      });
      continue;
    }
    press(b, (d) => h.send({ cmd: "key", k, down: d }));
  }
  LAYER_X.forEach((x, i) => {
    const b = el("key step", '<i class="led"></i>', x, 888, 97, 100, "button");
    b.title = "LAYER " + (i + 1); b.setAttribute("aria-label", "layer " + (i + 1));
    byName["LAYER " + (i + 1)] = { el: b, send: (d) => h.send({ cmd: "layer", i, down: d }) };
    b.addEventListener("pointerenter", () => (hint.textContent = "LAYER " + (i + 1) + ": lane " + (i + 1) + " in PLAY, step " + (i + 1) + " in STEP"));
    press(b, (d) => h.send({ cmd: "layer", i, down: d }));
  });
  // the bottom-left key (the matrix's 16th left-keypad key, Key::Spare): the effects. The device's cap carries a ring with a line from its centre down; the web bench is always on
  const pw = el("key power", glyph("power"), 309, 890, 92, 92, "button");
  pw.title = "FX (the bottom-left key: effects; again for the next view)"; pw.setAttribute("aria-label", "FX");
  pw.addEventListener("pointerenter", () => (hint.textContent = "FX: the effects pages (the bottom-left key); again for the next of six views"));
  press(pw, (d) => h.send({ cmd: "key", k: 31, down: d }));
  byName["FX"] = { el: pw, send: (d) => h.send({ cmd: "key", k: 31, down: d }) };

  // encoders: T1..T4 above the screen, B1..B4 below. On the device they turn for ever and the screen shows the value, which is hard work with a mouse. Here a
  // knob has stops: its pointer shows the value, a drag covers the whole range whatever its steps, and the wheel and the arrow keys go one detent at a time.
  // What has no place between limits (an action, say) steps as it is dragged.
  const turn = (i, d) => { h.send({ cmd: "turn", enc: i, d }); };
  for (let i = 0; i < 8; i++) {
    const e = el("enc", '<i class="cap"><u></u></i>', ENC_X[i % 4] + (i < 4 ? 0 : -2), i < 4 ? 200 : 743, 106, 106);
    e.title = (i < 4 ? "T" : "B") + ((i % 4) + 1);
    byName[e.title] = { el: e, turn: (n) => spin(n) };
    e.addEventListener("pointerenter", () => (hint.textContent = "knob " + e.title + ": the screen shows what it does; drag it up or down to set it"));
    const cap = e.firstChild;
    plateKnob[i] = { cap, bar: -1 };
    let acc = 0, lastY = null, startY = 0, startBar = -1;
    const spin = (n) => turn(i, n);
    // (a mouse's notch is one event of 100 or more, a trackpad many small ones: at most one detent an event, or a notch would jump over the middle of a short list)
    e.addEventListener("wheel", (ev) => { ev.preventDefault(); acc += Math.max(-30, Math.min(30, ev.deltaY * (ev.deltaMode === 1 ? 16 : ev.deltaMode === 2 ? 200 : 1))); while (acc <= -30) { acc += 30; spin(1); } while (acc >= 30) { acc -= 30; spin(-1); } }, { passive: false });
    e.addEventListener("pointerdown", (ev) => { if (ev.button !== 0) return; ev.preventDefault(); h.startAudio(); e.focus({ preventScroll: true }); lastY = startY = ev.clientY; startBar = plateKnob[i].bar; acc = 0; e.setPointerCapture?.(ev.pointerId); e.classList.add("down"); });   // (the arrow keys go to the knob last clicked)
    e.addEventListener("pointermove", (ev) => {
      if (lastY === null) return;
      if (startBar >= 0) {
        const to = Math.min(1, Math.max(0, startBar + (startY - ev.clientY) / KNOB_DRAG_PX));
        showKnob(i, to);
        h.send({ cmd: "knob", enc: i, to });
        return;
      }
      acc += lastY - ev.clientY; lastY = ev.clientY;
      while (acc >= 8) { acc -= 8; spin(1); }
      while (acc <= -8) { acc += 8; spin(-1); }
    });
    const end = () => { lastY = null; e.classList.remove("down"); };
    e.addEventListener("pointerup", end); e.addEventListener("pointercancel", end);
    e.tabIndex = 0;
    e.addEventListener("keydown", (ev) => { if (ev.key.startsWith("Arrow")) ev.stopPropagation(); if (ev.key === "ArrowUp" || ev.key === "ArrowRight") { ev.preventDefault(); spin(1); } else if (ev.key === "ArrowDown" || ev.key === "ArrowLeft") { ev.preventDefault(); spin(-1); } });
  }
  wrap.appendChild(hint);
  const help = document.createElement("div");
  help.id = "panelKeysHelp";
  help.innerHTML = KEYBOARD_HELP;
  wrap.appendChild(help);
  plateKeyboard(byName, h);
  return { canvas };
}

// What the LEDs show. The ten layer keys follow the firmware (wb_layer_leds, the rules of board/Leds.cpp: in STEP the steps that are on, the cursor blinks,
// elsewhere the selected lane and a flash for a lane that plays); the bars are the master peaks with the board's thresholds, up fast and down slowly.
let ledBits = -1;
// A knob's travel, the way a pot's has stops, and how far a drag has to go for all of it
const KNOB_SWEEP = 270, KNOB_DRAG_PX = 160;
const plateKnob = [];
function showKnob(i, bar) {
  const k = plateKnob[i];
  k.bar = bar;
  k.cap.style.transform = `rotate(${bar < 0 ? 0 : (bar - 0.5) * KNOB_SWEEP}deg)`;
}
function plateKnobs(bars) {
  if (!bars) return;
  bars.forEach((b, i) => { if (plateKnob[i] && plateKnob[i].bar !== b) showKnob(i, b); });
}

function plateLeds(bits) {
  if (bits === undefined || bits === ledBits) return;
  ledBits = bits;
  document.querySelectorAll("#plate .key.step").forEach((k, i) => k.classList.toggle("lit", (bits >> i) & 1));
}
const BAR_AT = [207, 519, 1036, 2067, 4125, 8231, 11626, 16422, 20675, 23197, 26028, 29193];   // 16-bit peak where each segment lights, -44 .. -1 dB
const barLevel = [0, 0];
function plateBars(l, r) {
  [l, r].forEach((p, c) => {
    barLevel[c] = p > barLevel[c] ? p : barLevel[c] * 0.85;
    const v = barLevel[c] * 32767;
    const segs = document.querySelectorAll(c ? "#plate .bar.r i" : "#plate .bar.l i");
    segs.forEach((s, k) => s.classList.toggle("on", v >= BAR_AT[11 - k]));   // the first child is the top segment
  });
}

// The computer keyboard on the Drumboy tab: the layer keys on 1 2 3 .. 9 0, the transport and the arrows where they are, a letter for each key that has one
// on its cap. (On the presets tab the keyboard is the piano.) A held key is a held key: SHIFT, FILL and the lock's long press work.
const KEYMAP = { " ": "PLAY", Escape: "STOP", Enter: "OK", ArrowUp: "UP", ArrowDown: "DOWN", ArrowLeft: "LEFT", ArrowRight: "RIGHT", Shift: "SHIFT", Backspace: "CLEAR",
  r: "REC", m: "MUTE", f: "FILL", s: "STEP", c: "COPY", p: "PASTE", l: "LANE", i: "PROJECT", o: "SONG", x: "FX", b: "FILE", n: "PART", e: "MIXER",
  q: "OSC 1", w: "OSC 2", t: "SUB", a: "FILTER 1", d: "FILTER 2", g: "GRIT", h: "AMP", j: "MOD", k: "GLOB",
  "+": "UP", "=": "UP", "-": "DOWN", "_": "DOWN", ".": "ADD", 1: "LAYER 1", 2: "LAYER 2", 3: "LAYER 3", 4: "LAYER 4", 5: "LAYER 5", 6: "LAYER 6", 7: "LAYER 7", 8: "LAYER 8", 9: "LAYER 9", 0: "LAYER 10" };
const KEYBOARD_HELP = "<b>Keyboard</b>: <kbd>1</kbd>..<kbd>0</kbd> layer keys, <kbd>Space</kbd> play, <kbd>Esc</kbd> stop, <kbd>Enter</kbd> OK, arrows, <kbd>Shift</kbd>, <kbd>Backspace</kbd> clear, <kbd>.</kbd> add, <kbd>+</kbd> (<kbd>=</kbd> on a US keyboard) and <kbd>-</kbd> up and down; " +
  "<kbd>R</kbd> rec <kbd>M</kbd> mute <kbd>F</kbd> fill <kbd>S</kbd> step <kbd>C</kbd> copy <kbd>P</kbd> paste <kbd>L</kbd> lane <kbd>I</kbd> project <kbd>O</kbd> song <kbd>X</kbd> fx <kbd>B</kbd> file <kbd>N</kbd> part <kbd>E</kbd> mixer; " +
  "sound pages <kbd>Q</kbd> osc1 <kbd>W</kbd> osc2 <kbd>T</kbd> sub <kbd>A</kbd> flt1 <kbd>D</kbd> flt2 <kbd>G</kbd> grit <kbd>H</kbd> amp <kbd>J</kbd> mod <kbd>K</kbd> glob. " +
  "<b>In the tracker</b> the letters are a piano: <kbd>A</kbd> to <kbd>K</kbd> with <kbd>W</kbd> <kbd>E</kbd> <kbd>T</kbd> <kbd>Y</kbd> <kbd>U</kbd> for the black keys write a note and go down a row, <kbd>Z</kbd> <kbd>X</kbd> an octave; hold <kbd>Enter</kbd> and use the arrows to move a note; <kbd>L</kbd> leaves";
// The tracker on a computer keyboard: the bench's piano (app.js PIANO), a key a note on the cursor's step and then a row down. It goes in through the firmware's own OK and NOTE knob
// (wb_tracker_note), so this is only a faster way of pressing them. C is the C of the lane's root; a sampler lane's is its pitch 0.
const TRACKER_PIANO = { a: 0, w: 1, s: 2, e: 3, d: 4, f: 5, t: 6, g: 7, y: 8, h: 9, u: 10, j: 11, k: 12 };
let plateNow = null, pianoOctave = 0;
const plateState = (a) => { plateNow = a; };
const inTracker = () => plateNow && plateNow[0] === 1 && plateNow[43] === 2;
const isPianoKey = (letter) => letter === "z" || letter === "x" || TRACKER_PIANO[letter] !== undefined;
function trackerPiano(letter, byName, h) {
  const say = (t) => { document.getElementById("panelHint").textContent = t; };
  if (letter === "z" || letter === "x") { pianoOctave = Math.max(-2, Math.min(2, pianoOctave + (letter === "x" ? 1 : -1))); say("piano octave " + (pianoOctave > 0 ? "+" : "") + pianoOctave); return; }
  const n = TRACKER_PIANO[letter];
  const root = plateNow[68], smp = plateNow[69] === 1;
  const offset = (smp ? 0 : Math.floor(root / 12) * 12 - root) + n + 12 * pianoOctave;
  if (Math.abs(offset) > 24) { say("that note is more than two octaves from the lane's root: Z and X move the piano"); return; }
  h.send({ cmd: "trackerNote", offset });
  byName.DOWN.send(true); byName.DOWN.send(false);
}
function plateKeyboard(byName, h) {
  if (plateKeyboard.on) return;
  plateKeyboard.on = true;
  const held = new Map();   // event.code -> the key name it pressed (by code: the letter is the same key whether or not Shift is down when it comes up)
  const active = (e) => document.getElementById("tab-panel").classList.contains("on") && !e.target.matches("input, textarea, select") && !(e.target.matches(".tab, #start, #lesson button, .menubar a") && (e.key === " " || e.key === "Enter")) && !e.metaKey && !e.ctrlKey && !e.altKey;
  const name = (e) => (e.code.startsWith("Key") ? KEYMAP[e.code.slice(3).toLowerCase()] : e.code.startsWith("Digit") ? KEYMAP[e.code.slice(5)] : e.code === "Space" ? KEYMAP[" "] : e.code.startsWith("Shift") ? "SHIFT" : KEYMAP[e.key]);
  window.addEventListener("keydown", (e) => {
    if (!active(e)) return;
    const letter = e.code.startsWith("Key") ? e.code.slice(3).toLowerCase() : "";
    if (inTracker() && !e.shiftKey && !held.has(e.code) && isPianoKey(letter)) {   // (a key held down repeats: one note, and never the panel key under it)
      e.preventDefault(); h.startAudio();
      if (!e.repeat) trackerPiano(letter, byName, h);
      return;
    }
    const n = name(e), k = n && byName[n];
    if (!k || held.has(e.code)) { if (k) e.preventDefault(); return; }
    e.preventDefault(); h.startAudio();
    if (document.activeElement?.classList.contains("enc")) document.activeElement.blur();   // (as a click on a key does)
    held.set(e.code, n); k.el.classList.add("down"); k.send(true);
  });
  const release = (key) => { const n = held.get(key); if (!n) return; held.delete(key); byName[n].el.classList.remove("down"); byName[n].send(false); if (n === "SHIFT") window.plateUnshift(); };   // (one SHIFT: letting go of this one lets go of a clicked one too)
  window.addEventListener("keyup", (e) => release(e.code));
  window.addEventListener("blur", () => [...held.keys()].forEach(release));
}
