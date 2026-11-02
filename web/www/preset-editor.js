"use strict";
// The preset editor on the librarian page. The pages, knobs, ranges and list choices come from preset-schema.js (made from the firmware's tables); the file's bytes are handled by preset-model.js;
// this file is the controls, and sending through the librarian's client and its questions. Text rules as in learn.js: plain words, no term before it is explained.
(() => {
  const P = globalThis.PresetSysex, M = globalThis.PresetModel, Sc = globalThis.PresetSchema, F = Sc.format;
  const $ = (id) => document.getElementById(id);
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };

  // What each page and knob is for. The names here are checked against the schema (web/test/preset-edit.mjs), so a knob cannot lose its line. A knob the Drumboy ignores is in NOT_USED instead.
  const HELP = {
    OSC1: { page: "An oscillator makes the raw sound that everything else shapes. A preset has two, and this is the first.",
      WAVE: "The basic shape of the sound, or a wavetable: a set of richer shapes.", "PD SHAPE": "The kind of bend PD AMT gives a sine wave.", "PD AMT": "How far the sine wave is bent.",
      OCT: "Moves the pitch by whole octaves.", SEMI: "Moves the pitch by piano keys.", FINE: "Moves the pitch a tiny amount. A small gap between the oscillators sounds thicker.", LEVEL: "How loud this oscillator is." },
    OSC2: { page: "The second oscillator. A preset that does not use it has its LEVEL at 0.",
      WAVE: "The basic shape of the sound, or a wavetable: a set of richer shapes.", "PD SHAPE": "The kind of bend PD AMT gives a sine wave.", "PD AMT": "How far the sine wave is bent.",
      OCT: "Moves the pitch by whole octaves.", SEMI: "Moves the pitch by piano keys.", FINE: "Moves the pitch a tiny amount. A small gap between the oscillators sounds thicker.", LEVEL: "How loud this oscillator is.",
      SYNC: "Makes oscillator 1 restart with this one, for a hard, cutting tone. It turns the UNISON copies off." },
    SUB: { page: "Adds weight and width: a lower tone, some noise, and stacked copies of oscillator 1.",
      "SUB LVL": "How loud an extra tone below the note is.", "SUB OCT": "How far below the note that tone sits.", NOISE: "Adds hiss, like wind or breath.", RING: "Multiplies the two oscillators for a metallic, bell-like tone. Both need some LEVEL, and it turns the UNISON copies off.",
      UNISON: "How many slightly detuned copies play together.", "UNI DET": "How far apart the copies are tuned.", "UNI BLND": "How loud the copies are against the main tone.", WIDTH: "How far the copies spread left and right." },
    FLT1: { page: "A filter takes part of the sound away, so it decides how bright or dark the sound is.",
      MODEL: "The kind of filter. LP keeps the lows, HP keeps the highs, BP keeps the middle. The others have characters of their own.", CUTOFF: "Where the filter starts to cut. Lower is darker.", RESO: "Makes the sound ring around the cutoff.",
      DRIVE: "Pushes the filter harder for a rougher sound.", KEYTRK: "Makes higher notes brighter.", "ENV AMT": "How much the filter opens at the start of each note.", HPF: "Removes the deep lows under this point.", "MIX F1/F2": "Moves from both filters to filter 2 alone." },
    FLT2: { page: "A second filter. It does nothing until ACTIVE is ON.",
      KEYTRK: "Makes higher notes brighter, on top of following filter 1.", "ENV AMT": "How much this filter opens at the start of each note, on top of following filter 1.",
      MODEL: "The kind of filter. LP keeps the lows, HP keeps the highs, BP keeps the middle. The others have characters of their own.", CUTOFF: "Where the filter starts to cut. Lower is darker.", RESO: "Makes the sound ring around the cutoff.",
      DRIVE: "Pushes the filter harder for a rougher sound.", ROUTE: "SERIES puts it after filter 1. PARALLEL runs the two side by side.", ACTIVE: "Turns filter 2 on or off." },
    GRIT: { page: "Makes the sound rougher, like an early digital machine.",
      "LOOP ERR": "A small error at the end of each cycle of the wave: a buzz at the pitch of the note.", WOW: "A slow wander in the pitch, like a worn tape.",
      BITS: "Fewer bits make the sound coarser.", COMPAND: "Adds the squeezed, grainy sound of old digital chips.", RATE: "A lower rate adds a harsh, metallic edge.", DRIFT: "Lets the sound wander a little, like old electronics.",
      CLIP: "Flattens the loudest peaks for a harder edge.", MIX: "How much of BITS, COMPAND, RATE and DRIFT you hear. 0% leaves them out." },
    AMP: { page: "How the loudness moves during a note, from the first moment to the last.",
      ATTACK: "How long the sound takes to rise.", DECAY: "How long it takes to fall to SUSTAIN. The filter's movement follows it.", SUSTAIN: "How loud it stays while the note is held.", RELEASE: "How long it takes to fade after the note ends.",
      "VEL SENS": "How much louder a harder hit plays.", LEVEL: "How loud the preset is.", PAN: "Where it sits between left and right.", "VEL>ATK": "Makes harder hits start faster." },
    MOD: { page: "Movement. The LFO is a slow wave that moves another setting up and down by itself.",
      "LFO RATE": "How fast the LFO moves.", "LFO DEPTH": "How far it moves.", "LFO DEST": "What it moves.", "LFO SHAPE": "The shape of the movement.",
      "ENV>PD": "Each note's start moves PD AMT, or the place in a wavetable.", "ENV AMT+": "Opens the filter further at the start of each note.", "MOD WHL": "How far the mod wheel of a MIDI keyboard opens the filter.", AFTERT: "How far pressing a held key harder on a MIDI keyboard opens the filter." },
    GLOB: { page: "Settings for the whole part that plays this preset.",
      MASTER: "The part's overall volume.", "BEND RNG": "How far the pitch wheel of a MIDI keyboard bends a note.", TUNE: "Tunes both oscillators together.", PREVIEW: "How loud the preset plays when you look through the list on the Drumboy Pro." },
    DELAY: { page: "An echo, shared by all the parts. The SENDS page sets how much of this preset goes into it.",
      MODE: "FREE uses TIME. The others follow the tempo.", TIME: "The gap between echoes.", FEEDBACK: "How many times the echo repeats.", TONE: "Lower makes the echoes darker.",
      SPREAD: "Moves the echoes between left and right.", MOD: "Makes the echoes waver a little.", HPF: "Removes the lows from the echoes.", RETURN: "How loud the echoes are." },
    REVERB: { page: "A room around the sound, shared by all the parts. The SENDS page sets how much of this preset goes into it.",
      SIZE: "How big the room sounds.", DECAY: "How long the room rings.", DAMP: "Lower makes the ringing darker.", "PRE-DLY": "A short wait before the room answers.",
      WIDTH: "How wide the room sounds.", MOD: "Keeps the ringing moving, so it sounds less metallic.", "LOW CUT": "Removes the lows from the room.", RETURN: "How loud the room is." },
    CHORUS: { page: "Wavering copies of the sound, for a wider, softer result. Shared by all the parts. The SENDS page sets how much of this preset goes into it.",
      RATE: "How fast the copies waver.", DEPTH: "How far they waver.", DELAY: "How far behind the sound the copies play.", FEEDBACK: "Feeds the copies back in for a sharper tone.",
      WIDTH: "How wide they spread.", TONE: "Lower makes the copies darker.", HPF: "Removes the lows from the copies.", RETURN: "How loud the copies are." },
    AMPSIM: { page: "A guitar amplifier for this part alone.",
      MODEL: "The kind of amp. OFF leaves the sound alone.", DRIVE: "How hard the amp is pushed.", BASS: "Raises or lowers the lows.", MID: "Raises or lowers the middle.", TREBLE: "Raises or lowers the highs.",
      PRESENCE: "Adds bite at the very top.", CAB: "The speaker box it plays through.", LEVEL: "How loud the amp is." },
    SENDS: { page: "How much of this preset goes to each of the shared effects. At 0% that effect stays out of it.",
      DELAY: "How much goes to the echo.", REVERB: "How much goes to the room.", CHORUS: "How much goes to the chorus." },
  };
  // Knobs that are in the file and that a preset does not set: the firmware keeps the value (Ui.cpp, Params.cpp).
  const NOT_USED = { "GLOB:4": "The clock belongs to the project, on the GLOBAL page. A preset leaves it alone.", "GLOB:2": "Not used any more: the PROJECT screen sets the MIDI channels. It stays in the preset." };
  // What an effect's tick means. A shared effect takes these settings only if no other part is using it (applyPresetFx).
  const CARRY = {
    DELAY: ["This preset brings its own delay settings", "They are used when no other part is sending to the delay. Without them it uses the project's."],
    REVERB: ["This preset brings its own reverb settings", "They are used when no other part is sending to the reverb. Without them it uses the project's."],
    CHORUS: ["This preset brings its own chorus settings", "They are used when no other part is sending to the chorus. Without them it uses the project's."],
    AMPSIM: ["This preset has an amp", "Without it the part plays with no amp."],
  };
  const POSITION = "POSITION";   // what the panel calls PD SHAPE on a wavetable (Ui.cpp; web/test/preset-edit.mjs keeps an eye on it)
  const pages = [...Sc.pages, ...Sc.fx];
  const textOf = (k, v) => Sc.texts[k.text][v - k.min];

  // `host`: what the librarian lends: S, status, log, progress, job, ask, save, spent, reread, refresh, where
  globalThis.PresetEditor = (host) => {
    const S = host.S, { status, log, ask, where } = host;
    const E = { b: null, base: null, isNew: false, page: pages[0].key, nameWhy: null };
    const ctl = new Map();   // knob id -> its control
    const same = (a, b) => a.every((v, i) => v === b[i]);
    const dirty = () => !!E.b && (!same(E.b, E.base) || $("edName").value !== M.name(E.b));
    const pad = (id) => String(id).padStart(4, "0");

    // ---- building the pages, once
    function hintFor(page, knob) { return HELP[page.key][knob.name]; }
    function field(id, page, knob, input) {
      const cell = el("div", "knob");
      cell.dataset.k = id;
      const label = el("label", "knob-top");
      label.htmlFor = input.id = "ed-" + id;
      const name = el("span", "knob-name", knob.name), value = el("span", "knob-val");
      label.append(name, value);
      const hint = el("p", "knob-hint", hintFor(page, knob));
      hint.id = input.id + "-hint";
      input.setAttribute("aria-describedby", hint.id);
      cell.append(label, input, hint);
      ctl.set(id, { id, page, knob, cell, input, name, value, hint });
      return cell;
    }
    function slider(id, page, knob) {
      const input = el("input", "ed-range");
      input.type = "range"; input.min = knob.min; input.max = knob.max; input.step = 1;
      input.oninput = () => edit(() => M.write(E.b, id, +input.value));
      return field(id, page, knob, input);
    }
    function list(id, page, knob) {
      const input = el("select", "btn");
      for (let v = knob.min; v <= knob.max; ++v) { const o = el("option", "", textOf(knob, v)); o.value = v; input.appendChild(o); }
      input.onchange = () => edit(() => M.write(E.b, id, +input.value));
      return field(id, page, knob, input);
    }
    // WAVE: the basic waves, then the banks that exist under the firmware's own group names
    function waveList(id, page, knob, osc) {
      const input = el("select", "btn");
      const basic = el("optgroup"); basic.label = "BASIC WAVES";
      Sc.wave.basic.forEach((n, i) => { const o = el("option", "", n); o.value = "w" + i; basic.appendChild(o); });
      input.appendChild(basic);
      Sc.wave.cats.forEach((cat, c) => {
        const banks = Sc.wave.exist.filter((b) => Sc.wave.bankCat[b] === c);
        if (!banks.length) return;
        const g = el("optgroup"); g.label = "WAVETABLES: " + cat;
        for (const b of banks) { const o = el("option", "", Sc.wave.bankNames[b]); o.value = "b" + b; g.appendChild(o); }
        input.appendChild(g);
      });
      input.onchange = () => edit(() => M.setWave(E.b, osc, input.value[0] === "w" ? { basic: +input.value.slice(1) } : { bank: +input.value.slice(1) }));
      return field(id, page, knob, input);
    }
    const waveValue = (b, osc) => { const w = M.wave(b, osc); return w.bank !== undefined ? "b" + w.bank : "w" + w.basic; };
    // A file may name a bank that is not among the ones that exist: it stays chosen, marked as the panel marks it, until something else is picked.
    function missingBank(input, value) {
      input.querySelector("option.missing")?.remove();
      if (value[0] !== "b" || input.querySelector(`option[value="${value}"]`)) return;
      const o = el("option", "missing", Sc.wave.bankNames[+value.slice(1)] + " ?");
      o.value = value;
      input.prepend(o);
    }

    function buildPages() {
      const tabs = $("edTabs"), panels = $("edPanels");
      pages.forEach((page, n) => {
        if (n === Sc.pages.length) tabs.appendChild(el("span", "ed-gap", "Effects"));
        const tab = el("button", "ed-tab", page.title), panel = el("div", "ed-panel");
        tab.type = "button"; tab.id = "edTab-" + page.key; tab.setAttribute("role", "tab"); tab.setAttribute("aria-controls", "edPanel-" + page.key);
        tab.onclick = () => showPage(page.key);
        tabs.appendChild(tab);
        panel.id = "edPanel-" + page.key; panel.setAttribute("role", "tabpanel"); panel.setAttribute("aria-labelledby", tab.id); panel.hidden = true;
        panel.appendChild(el("p", "ed-about", HELP[page.key].page));
        if (page.mask) {
          const box = el("label", "ed-carry"), tick = el("input");
          tick.type = "checkbox"; tick.id = "edCarry-" + page.key;
          tick.onchange = () => edit(() => M.setCarried(E.b, page.key, tick.checked, E.base));
          box.append(tick, el("span", "", CARRY[page.key][0]));
          panel.append(box, el("p", "ed-about", CARRY[page.key][1]));
        }
        const grid = el("div", "knobs");
        page.knobs.forEach((knob, k) => {
          if (knob.unused) return;
          const id = `${page.key}:${k}`;
          grid.appendChild(knob.wave ? waveList(id, page, knob, n) : knob.list ? list(id, page, knob) : slider(id, page, knob));
        });
        panel.appendChild(grid);
        panels.appendChild(panel);
      });
      tabs.onkeydown = (e) => {
        const i = pages.findIndex((p) => p.key === E.page);
        const to = { ArrowRight: i + 1, ArrowDown: i + 1, ArrowLeft: i - 1, ArrowUp: i - 1, Home: 0, End: pages.length - 1 }[e.key];
        if (to === undefined) return;
        e.preventDefault();
        showPage(pages[(to + pages.length) % pages.length].key);
        $("edTab-" + E.page).focus();
      };
      Sc.cats.forEach((c, i) => { if (i) { const o = el("option", "", c); o.value = i; $("edCat").appendChild(o); } });
      for (let v = 1; v <= F.voicesMax; ++v) { const o = el("option", "", String(v)); o.value = v; $("edVoices").appendChild(o); }
      $("edName").maxLength = F.nameMax;
      showPage(E.page);
    }
    function showPage(key) {
      E.page = key;
      for (const p of pages) {
        const on = p.key === key, tab = $("edTab-" + p.key);
        tab.setAttribute("aria-selected", on);
        tab.tabIndex = on ? 0 : -1;
        $("edPanel-" + p.key).hidden = !on;
      }
    }

    // ---- what a knob can do as the other knobs stand: off, dimmed with the reason, or called something else. The panel's own rules (Ui.cpp), and what the voice does (LeanPart.cpp)
    function state(c) {
      const b = E.b, k = c.id, n = +k.split(":")[1];
      const osc = Sc.pages.indexOf(c.page);
      if (c.page.mask && !M.carried(b, c.page.key)) return { off: true };
      if (NOT_USED[k]) return { why: NOT_USED[k] };
      if (osc === 0 || osc === 1) {
        const w = M.wave(b, osc);
        if (n === 0 && w.bank !== undefined && !Sc.wave.exist.includes(w.bank)) return { hint: "The ? means this wavetable does not exist. The Drumboy Pro plays the basic wave nearest to it." };
        if (w.bank !== undefined && n === 1) return { label: POSITION, hint: "Where in the wavetable it plays." };
        if (w.bank !== undefined && n === 2) return { off: true, text: "-", why: "A wavetable does not use this." };
        if (w.bank === undefined && w.basic !== 0 && (n === 1 || n === 2)) return { why: "Only the SIN wave uses this." };
      }
      if (c.page.key === "SUB" && n >= 5 && M.read(b, "SUB:4") <= 1) return { why: "One copy has nothing to do this to. Raise UNISON to use it." };
      if (c.page.key === "FLT2" && n !== 5 && M.read(b, "FLT2:5") === 0) return { why: "Filter 2 is off. Set ACTIVE to ON to use it." };
      if (k === "FLT1:7" && !(M.read(b, "FLT2:5") && M.read(b, "FLT2:4"))) return { why: "This only works with filter 2 ACTIVE and its ROUTE at PARALLEL." };
      if (c.page.key === "MOD" && (n === 0 || n === 2 || n === 3) && M.read(b, "MOD:1") === 0) return { why: "The LFO has no depth. Raise LFO DEPTH to use it." };
      if (k === "DELAY:1" && M.read(b, "DELAY:0") !== 0) return { why: "The delay follows the tempo. Set MODE to FREE to use it." };
      // an older file keeps a second detune in a slot that has no knob now: the firmware plays the larger of the two (migratePages)
      if (k === "SUB:5" && M.read(b, "OSC1:7") > M.read(b, k)) return { hint: `An older setting in this preset holds this at ${textOf(c.knob, M.read(b, "OSC1:7"))} or more.` };
      return {};
    }
    function showControl(c) {
      const st = state(c), wave = c.knob.wave, osc = Sc.pages.indexOf(c.page);
      const v = wave ? waveValue(E.b, osc) : M.read(E.b, c.id), was = wave ? waveValue(E.base, osc) : M.read(E.base, c.id);
      if (wave) missingBank(c.input, v);
      c.input.value = v;
      c.input.disabled = !!st.off;
      if (!wave && !c.knob.list) {   // (a list shows its own value)
        const text = st.text || textOf(c.knob, v);
        c.value.textContent = text;
        c.input.setAttribute("aria-valuetext", text);
        c.input.style.setProperty("--p", ((v - c.knob.min) / (c.knob.max - c.knob.min)) * 100 + "%");
      }
      c.name.textContent = st.label || c.knob.name;
      c.hint.textContent = st.why || st.hint || hintFor(c.page, c.knob);
      const moved = v !== was;
      c.cell.classList.toggle("changed", moved);
      c.cell.classList.toggle("idle", !!st.why || !!st.off);
      return moved;
    }
    // Everything on show follows the bytes. (Under a hundred controls: one pass is quicker than knowing which depend on which.)
    function show() {
      if (!E.b) return;
      const moved = new Set();
      for (const c of ctl.values()) if (showControl(c)) moved.add(c.page.key);
      for (const p of pages) {
        if (p.mask) { const t = $("edCarry-" + p.key); t.checked = M.carried(E.b, p.key); if (t.checked !== M.carried(E.base, p.key)) moved.add(p.key); }
        $("edTab-" + p.key).classList.toggle("changed", moved.has(p.key));
      }
      $("edCat").value = M.cat(E.b);
      $("edVoices").value = M.voices(E.b);
      $("edStray").hidden = !M.strays(E.b);
      $("edShown").textContent = `${E.isNew ? "" : pad(M.id(E.b)) + "  "}${M.name(E.b)}`;
      $("edFrom").textContent = E.from;
      gadgets();
    }
    function edit(fn) { fn(); show(); }

    // What Send will do, said before it is pressed.
    function plan() {
      if (!S.client) return "To send it, connect to a device at the top of this page.";
      const id = M.id(E.b), old = E.isNew ? null : S.list.find((e) => e.id === id), next = M.nextFreeId(S.list);
      if (old) return `Send to device replaces ${id}, ${old.name}, on ${where()}. Send as new keeps that one and adds this as number ${next}.`;
      return `Send to device adds it to ${where()} as number ${E.isNew ? next : id}.`;
    }
    function gadgets() {
      const idle = !S.busy, open = !!E.b, can = open && !E.nameWhy, link = !!S.client && idle;
      $("edNew").disabled = !idle;
      $("edSave").disabled = !can || !idle;
      $("edSend").disabled = !can || !link;
      $("edSendNew").disabled = !can || !link || E.isNew;
      $("edUndo").disabled = !dirty() || !idle;
      $("edHear").disabled = !can && !hear.playing();   // (Stop always works)
      $("edDirty").hidden = !dirty();
      for (const b of document.querySelectorAll(".edit-open")) b.disabled = !idle;
      if (open) $("edPlan").textContent = plan();
    }

    // ---- opening
    function adopt(bytes, from, isNew) {
      hear.stop();
      E.b = Uint8Array.from(bytes); E.base = Uint8Array.from(bytes); E.isNew = isNew; E.from = from; E.nameWhy = null;
      $("edName").value = M.name(E.b);
      $("edNameNote").hidden = true;
      $("edBody").hidden = false;
      show();
      $("edWin").scrollIntoView({ block: "start" });
      $("edHead").focus({ preventScroll: true });   // (not the name: on a phone that would pull up the keyboard)
    }
    const mayDrop = async () => {
      if (!dirty()) return true;
      return ask("Drop your changes?", `${$("edName").value || "This preset"} has changes that are not saved or sent. If you go on they are gone.`, [], "Drop changes");
    };
    async function openDevice(e) {
      if (S.busy || !S.client) return;
      if (!await mayDrop()) { log("Nothing was opened."); return; }
      if (!S.client) return;
      let got = null;
      $("edBody").inert = true;   // (what was on show is being replaced: nothing moved now would be asked about)
      await host.job(async () => {
        const bytes = await S.client.download(e.id, host.progress);
        const p = P.parsePreset(bytes);
        if (!p.ok) throw new P.ProtocolError(`preset ${e.id} came back wrong: ${p.why}`);
        if (p.id !== e.id) throw new P.ProtocolError(`the device sent preset ${p.id} when asked for ${e.id}`);
        got = bytes;
        host.spent();
      }, (err) => { host.spent(); return `The preset could not be read: ${err.message}. The editor has not changed. You can try again.`; });
      $("edBody").inert = false;
      if (!got) return;
      adopt(got, `from ${where()}`, false);
      status(`Opened ${e.id}, ${M.name(E.b)}, from ${where()}.`, "good");
    }
    async function openFile(f) {
      if (S.busy) return;
      if (!await mayDrop()) { log("Nothing was opened."); return; }
      adopt(f.bytes, `from the file ${f.name}`, false);
      status(`Opened the file ${f.name}.`, "good");
    }
    $("edNew").onclick = async () => {
      if (S.busy) return;
      if (!await mayDrop()) { log("Nothing was opened."); return; }
      adopt(M.blank(), "a new preset", true);
      status("Started a new preset.", "good");
    };
    $("edUndo").onclick = async () => {
      if (!await mayDrop()) { log("The changes stay."); return; }
      E.b = Uint8Array.from(E.base); E.nameWhy = null;
      $("edName").value = M.name(E.b);
      $("edNameNote").hidden = true;
      show();
      status("Your changes were dropped.", "good");
    };

    // ---- the name, category and voices
    $("edName").oninput = () => {
      const t = $("edName").value;
      E.nameWhy = M.nameProblem(t);
      $("edNameNote").hidden = !E.nameWhy;
      $("edNameNote").textContent = E.nameWhy ? E.nameWhy + " Until that is fixed the preset cannot be saved or sent." : "";
      if (!E.nameWhy) M.setName(E.b, t);
      show();
    };
    $("edCat").onchange = () => edit(() => M.setCat(E.b, +$("edCat").value));
    $("edVoices").onchange = () => edit(() => M.setVoices(E.b, +$("edVoices").value));

    // ---- to the computer
    $("edSave").onclick = () => {
      if (!E.b || E.nameWhy || S.busy) return;
      if (E.isNew && S.client) { const n = M.nextFreeId(S.list); if (n >= 0) E.b = M.withId(E.b, n); }
      const out = Uint8Array.from(E.b), p = P.parsePreset(out);
      if (!p.ok) { status(`This preset cannot be saved: ${p.why}.`, "bad"); return; }
      host.save(P.fileName(p.id, p.name), out);
      E.base = Uint8Array.from(out);
      show();
      status("Handed 1 preset to the browser as a download.", "good");
    };

    // ---- to the device: the list is read first, so the question says what is there now, then it goes through the question and the client every upload goes through
    async function send(asNew) {
      if (!E.b || E.nameWhy || S.busy || !S.client) return;
      let listed = false;
      await host.job(async () => { await host.refresh(); host.spent(); listed = true; },
        (e) => { host.spent(); return `The list could not be read, so nothing was sent: ${e.message}. Your edit is still here. You can try again.`; });
      if (!listed || !S.client) return;
      if (E.nameWhy) { status("Nothing was sent, because the name cannot be used.", "bad"); return; }   // (typed while the list was read)
      const fresh = asNew || E.isNew, id = fresh ? M.nextFreeId(S.list) : M.id(E.b), name = M.name(E.b);
      if (id < 0) { status("The device has no free number left for a new preset.", "bad"); return; }
      const old = fresh ? null : S.list.find((e) => e.id === id);
      if (old && old.builtIn) { status(`Number ${id} is a built-in preset, which cannot be replaced.`, "bad"); return; }
      const yes = old
        ? await ask(`Replace a preset on ${where()}?`, `This overwrites preset ${id}, ${old.name}, on ${where()} for good. Back it up first if you may want it again.`, [`${id}  ${name}  (replaces ${old.name})`], `Replace ${id}`)
        : await ask(`Send to ${where()}?`, `This adds a new preset to ${where()}, number ${id}.`, [`${id}  ${name}`], "Send 1");
      if (!yes || !S.client) { log("Nothing was sent."); return; }
      const out = M.withId(E.b, id);
      let sent = false;
      await host.job(async () => {
        await S.client.upload(out, !!old, host.progress);
        host.spent();
        sent = true;
        status(`Sent 1 preset to ${where()}: number ${id}, ${name}` + (old ? `, in place of ${old.name}.` : "."), "good");
      }, (e) => {
        host.spent();
        const unsure = e.code === -1 || e.code === P.GONE;   // (no answer, or no device: the last message may have arrived)
        return `Sending stopped: ${e.message.replace(/\.$/, "")}. ` + (unsure ? "It may have gone through all the same, so look at the list when the device answers. " : "") + "Your edit is still here" + (S.client ? ", so you can try again." : ".");
      });
      if (sent) {   // (anything moved while it was on its way stays as a change)
        E.b = M.withId(E.b, id); E.base = out; E.isNew = false; E.from = `sent to ${where()}`;
        show();
      }
      await host.reread();
    }
    $("edSend").onclick = () => send(false);
    $("edSendNew").onclick = () => send(true);

    // ---- hearing it
    const hearing = (on) => { $("edHear").textContent = on ? "Stop" : "Hear it"; $("edHear").setAttribute("aria-pressed", on); gadgets(); };
    const hear = globalThis.PresetHear((t) => status(t, "bad"), (peak) => ($("edLevel").style.width = Math.min(100, Math.sqrt(peak) * 100) + "%"), () => hearing(false));
    $("edHear").onclick = async () => {
      if (hear.playing()) { hear.stop(); return; }
      if (!globalThis.crossOriginIsolated) { status("The sound engine cannot run on this server: it does not send the two headers the engine needs. Saving and sending still work.", "bad"); return; }
      hearing(true);
      try { await hear.start(() => ({ bytes: M.withId(E.b, F.idMax), id: F.idMax, cat: Sc.cats[M.cat(E.b)] })); }
      catch (e) { hear.stop(); status(`The sound engine did not start: ${e.message}. Saving and sending still work.`, "bad"); }
    };
    document.addEventListener("visibilitychange", () => { if (document.hidden) hear.stop(); });

    // ---- nothing is lost by leaving the page by accident
    addEventListener("beforeunload", (e) => { if (dirty()) { e.preventDefault(); e.returnValue = ""; } });

    buildPages();
    return { openDevice, openFile, gadgets };
  };
  globalThis.PresetEditor.HELP = HELP;
  globalThis.PresetEditor.NOT_USED = NOT_USED;
})();
