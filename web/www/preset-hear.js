"use strict";
// Hearing the preset being edited: the site's own engine (engine-worker.js, the firmware in WebAssembly) with the lean voice the Drumboy Pro plays, started when it is asked for. The edited bytes
// go in as a preset file and load the way the panel loads one, so what plays is what the file says. A note repeats, and a change is picked up between two notes.
(() => {
  const CAP = 16384, NOTE_MS = 1600, GAP_MS = 350, VELOCITY = 100, MASTER = 100;
  const REGISTER = { BASS: 36, LEAD: 60, PAD: 48, PLUCK: 55, STAB: 52, POLY: 52, KEYS: 52, FX: 48, SEQ: 43, TEXTURE: 48, BELL: 60 };   // (the note the bench plays each type on)
  const START_MS = 15000;
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  // `tell(text)`: something the player should know. `level(0..1)`: the peak, ten times a second. `stopped()`: it has stopped.
  globalThis.PresetHear = (tell, level, stopped) => {
    let worker = null, ctx = null, ready = null, seq = 0, loaded = null, on = false, round = 0;
    const waiting = new Map();   // seq -> what is waiting for that load

    function startEngine() {
      if (ready) return ready;
      ready = new Promise((res, rej) => {
        const ring = new SharedArrayBuffer(16 + CAP * 8);
        const t = setTimeout(() => rej(new Error("it took too long")), START_MS);
        worker = new Worker("engine-worker.js" + "?hr=1");   // (the effects at half rate, as on the board and the panel pages; tools/make_site_php.py rewrites this line's start)
        worker.onerror = () => { clearTimeout(t); rej(new Error("it could not be loaded")); };
        worker.onmessage = (e) => {
          const m = e.data;
          if (m.ev === "ready") { clearTimeout(t); res(ring); }
          else if (m.ev === "stats") { if (on) level(Math.max(m.peakL, m.peakR)); }
          else if (m.ev === "loaded" && waiting.has(m.seq)) { waiting.get(m.seq)(m); waiting.delete(m.seq); }
          else if (m.ev === "error") { console.warn(m.msg); if (/wavetable bank/.test(m.msg)) tell("A wavetable could not be fetched, so this plays the basic wave nearest to it."); }
        };
        worker.postMessage({ cmd: "init", sab: ring, cap: CAP });
      }).then(async (ring) => {
        await ctx.audioWorklet.addModule("ring-worklet.js");
        const node = new AudioWorkletNode(ctx, "ring-player", { outputChannelCount: [2], processorOptions: { sab: ring, cap: CAP } });
        node.connect(ctx.destination);
        worker.postMessage({ cmd: "master", v: MASTER });
        worker.postMessage({ cmd: "engine", lean: 1 });
        setInterval(() => { if (on) worker.postMessage({ cmd: "stats" }); }, 100);
      });
      ready.catch(() => { ready = null; if (worker) worker.terminate(); worker = null; });   // a later press tries again
      return ready;
    }

    function load(bytes, id) {
      const s = ++seq;
      return new Promise((res) => { waiting.set(s, res); worker.postMessage({ cmd: "audition", bytes: Uint8Array.from(bytes), id, seq: s }); });
    }
    const same = (a, b) => !!a && a.length === b.length && a.every((v, i) => v === b[i]);

    // `current()`: { bytes, id, cat } as they are now. Stops by itself when the press that started it is not the latest.
    async function loop(current, mine) {
      while (on && mine === round) {
        const now = current();
        if (!same(loaded, now.bytes)) { const m = await load(now.bytes, now.id); if (!on || mine !== round) return; if (!m.ok) { tell("This preset could not be loaded into the sound engine."); stop(); return; } loaded = Uint8Array.from(now.bytes); }
        const note = REGISTER[now.cat] || 48;
        worker.postMessage({ cmd: "noteOn", note, vel: VELOCITY });
        await sleep(NOTE_MS);
        if (mine !== round) return;   // (it was stopped, and a new start may be playing this note)
        worker.postMessage({ cmd: "noteOff", note });
        await sleep(GAP_MS);
      }
    }
    async function start(current) {
      if (on) return;
      on = true;
      const mine = ++round;
      ctx = ctx || new AudioContext({ sampleRate: 44100, latencyHint: "interactive" });   // here, inside the press: some browsers only allow sound that starts from one
      ctx.resume().catch(() => {});
      await startEngine();          // (it throws for the caller, who stops this)
      if (on && mine === round) loop(current, mine);
    }
    function stop() {
      if (!on) return;
      on = false; ++round;
      if (worker) worker.postMessage({ cmd: "allOff" });
      level(0);
      stopped();
    }
    return { start, stop, playing: () => on };
  };
})();
