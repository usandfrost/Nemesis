"use strict";
// The preset librarian's page: connect, list, back up, load, send, delete. The bytes are in sysex.js, the simulated device in sim-device.js and the editor in preset-editor.js. Reading has been run on a real cable; sending and deleting have not, and the page says so.
(() => {
  const P = globalThis.PresetSysex;
  const $ = (id) => document.getElementById(id);
  const S = { client: null, sim: null, simulated: false, list: [], files: [], busy: false, access: null };
  const devSel = new Set(), pcSel = new Set();   // preset numbers ticked on the device; indexes of the ticked files
  let Ed = null;                                 // the preset editor (the end of this file)

  const log = (t) => { const l = $("log"); l.textContent += t + "\n"; l.scrollTop = l.scrollHeight; };
  function status(t, kind) { for (const s of [$("status"), $("edStatus")]) { s.textContent = t; s.className = "status" + (kind ? " " + kind : ""); } log(t); }   // (the editor's copy: the top of the page may be out of sight)
  const progress = (a, b) => ($("bar2").style.width = (b ? Math.round((a / b) * 100) : 0) + "%");
  const catName = (c) => P.CATS[c] || "?";

  // The gadgets follow the state: what cannot be done now is off.
  function gadgets() {
    const on = !!S.client && !S.busy;
    $("refresh").disabled = !on;
    $("saveMidi").disabled = !on;
    $("backupSel").disabled = !on || !devSel.size;
    $("backupAll").disabled = !on || !S.list.some((e) => !e.builtIn);
    $("deleteSel").disabled = !on || !devSel.size;
    $("uploadSel").disabled = !on || !pcSel.size;
    $("clearFiles").disabled = S.busy || !S.files.length;
    $("useSim").disabled = $("useMidi").disabled = S.busy;
    $("tryPorts").disabled = S.busy;
    if (Ed) Ed.gadgets();
  }
  function row(tbody, id, cells, checked, disabled, onTick, label, onEdit) {
    const tr = document.createElement("tr");
    tr.className = checked ? "sel" : "";
    const td = document.createElement("td");
    const box = document.createElement("input");
    box.type = "checkbox"; box.checked = checked; box.disabled = disabled; box.value = id; box.setAttribute("aria-label", label);
    box.onchange = () => { onTick(box.checked); tr.className = box.checked ? "sel" : ""; gadgets(); };
    td.appendChild(box);
    tr.appendChild(td);
    for (const c of cells) { const t = document.createElement("td"); t.textContent = c; tr.appendChild(t); }
    const end = document.createElement("td");
    if (onEdit) {
      const b = document.createElement("button");
      b.className = "btn mini edit-open"; b.textContent = "Edit"; b.disabled = S.busy; b.setAttribute("aria-label", label.replace("Select", "Edit")); b.onclick = onEdit;
      end.appendChild(b);
    }
    tr.appendChild(end);
    tbody.appendChild(tr);
    return tr;
  }
  function drawDevice() {
    const t = $("devList");
    t.textContent = "";
    for (const e of S.list) row(t, e.id, [String(e.id).padStart(4, "0"), e.name + (e.builtIn ? " (built in)" : ""), catName(e.cat)], devSel.has(e.id), e.builtIn, (on) => (on ? devSel.add(e.id) : devSel.delete(e.id)), "Select " + e.name, e.builtIn ? null : () => Ed.openDevice(e));
    gadgets();
  }
  function drawFiles() {
    const t = $("pcList");
    t.textContent = "";
    S.files.forEach((f, i) => {
      if (f.p.ok) row(t, i, [String(f.p.id).padStart(4, "0"), f.p.name, catName(f.p.cat)], pcSel.has(i), false, (on) => (on ? pcSel.add(i) : pcSel.delete(i)), "Select " + f.p.name, () => Ed.openFile(f));
      else {
        const tr = document.createElement("tr"), td = document.createElement("td");
        td.colSpan = 5; td.className = "bad-file"; td.textContent = `${f.name}: ${f.p.why}`;
        tr.appendChild(td); t.appendChild(tr);
      }
    });
    gadgets();
  }

  // One job at a time. A failure lands here, in words, and leaves the page usable.
  async function job(fn, onFail) {
    if (S.busy) return;
    S.busy = true; gadgets(); progress(0, 0);
    S.jobOk = false;
    try { await fn(); S.jobOk = true; }
    catch (e) { if (e instanceof P.ProtocolError) status(onFail(e), "bad"); else { status("Something went wrong in this page: " + e.message, "bad"); console.error(e); } }
    S.busy = false; gadgets();
  }

  async function refresh() {
    S.list = await S.client.list(progress);
    for (const id of [...devSel]) if (!S.list.some((e) => e.id === id)) devSel.delete(id);
    drawDevice();
    log(`The device lists ${S.list.length} presets.`);
  }
  function connected(info) {
    const what = info.simulated ? "the simulated device" : "a device";
    $("devTitle").textContent = info.simulated ? "On the simulated device" : "On the device";
    $("simBox").hidden = !info.simulated;
    status(`Connected to ${what}: ${info.count} presets of ${info.capacity} on its card.`, "good");
  }
  // `why`: the connection was lost, and whoever waits for an answer is told so
  function disconnect(why) {
    const c = S.client;
    if (S.input) S.input.onmidimessage = null;
    S.input = S.output = S.client = S.sim = null;
    S.list = []; devSel.clear(); drawDevice();
    $("simBox").hidden = true; $("devTitle").textContent = "On the device";
    if (c && why) c.fail(new P.ProtocolError(why, P.GONE));
  }
  const where = () => (S.sim ? "the simulated device" : "the device");

  // ---- the simulated device starts with the first presets of the library the site already has
  async function startingPresets() {
    try {
      const buf = new Uint8Array(await (await fetch("data/prs.bin")).arrayBuffer()), out = [];
      for (let o = 0; o < buf.length && out.length < 24;) {
        o += 1 + buf[o];
        const size = buf[o] | (buf[o + 1] << 8);
        o += 2;
        out.push(buf.slice(o, o + size));
        o += size;
      }
      return out;
    } catch (e) { return []; }
  }
  $("useSim").onclick = () => job(async () => {
    disconnect();
    $("ports").hidden = true;
    const dev = new SimDevice(await startingPresets());
    const c = new P.Client((b) => dev.receive(b), log);
    dev.onsend = (b) => c.receive(b);
    S.client = c; S.sim = dev;
    $("fault").value = "none";
    connected(await c.identify());
    await refresh();
  }, (e) => "The simulated device did not start: " + e.message);
  $("fault").onchange = (e) => { if (S.sim) { S.sim.fault = e.target.value; log(`Simulation: next transfer, "${e.target.selectedOptions[0].text}".`); } };
  // (the fault is one-shot: show it spent, once a job has run)
  const spent = () => { if (S.sim) $("fault").value = S.sim.fault; };

  // ---- Web MIDI: every way it can go wrong has a plain answer
  function fillPorts() {
    const fill = (sel, ports) => { sel.textContent = ""; for (const p of ports) { const o = document.createElement("option"); o.value = p.id; o.textContent = p.name; sel.appendChild(o); } };
    const inputs = [...S.access.inputs.values()], outputs = [...S.access.outputs.values()];
    fill($("portIn"), inputs); fill($("portOut"), outputs);
    return inputs.length > 0 && outputs.length > 0;
  }
  const NO_DEVICE = "No MIDI device found. A real connection needs a USB-MIDI interface wired to both the MIDI in and MIDI out sockets. You can still try the simulated device.";
  function portChange(e) {
    const mine = S.input && e.port.state === "disconnected" && (e.port.id === S.input.id || e.port.id === (S.output && S.output.id));
    if (mine) {
      if ($("ask").open) $("askNo").click();
      const msg = "The MIDI interface was unplugged.";
      disconnect(msg);
      status(msg, "bad");
      if (!fillPorts()) $("ports").hidden = true;
      return;
    }
    if (!$("ports").hidden && !S.input && !fillPorts()) { $("ports").hidden = true; status(NO_DEVICE, "bad"); }
  }
  $("useMidi").onclick = () => job(async () => {
    if (!navigator.requestMIDIAccess) { status("This browser has no Web MIDI, so it cannot talk to a device. Chrome and Edge have it. You can still try the simulated device.", "bad"); return; }
    let access;
    try { access = await navigator.requestMIDIAccess({ sysex: true }); }
    catch (e) { status("The browser was not allowed to use MIDI. Allow MIDI and system exclusive messages for this site in the browser's settings, then try again. You can still try the simulated device.", "bad"); return; }
    disconnect();
    $("ports").hidden = true;
    S.access = access;
    access.onstatechange = portChange;
    if (!fillPorts()) { status(NO_DEVICE, "bad"); return; }
    $("ports").hidden = false;
    status("Choose the ports the Drumboy Pro is on, then press Ask the device.");
  }, (e) => e.message);
  $("tryPorts").onclick = () => job(async () => {
    disconnect();
    const input = S.access.inputs.get($("portIn").value), output = S.access.outputs.get($("portOut").value);
    if (!input || !output) { status("That MIDI port has gone. Press Connect over MIDI again.", "bad"); return; }
    const c = new P.Client((b) => { try { output.send(Array.from(b)); } catch (e) { throw new P.ProtocolError("the MIDI port is closed", -1); } }, log);
    input.onmidimessage = (e) => c.receive(e.data);
    S.input = input; S.output = output; S.client = c;
    let info;
    try { info = await c.identify(); }
    catch (e) {
      disconnect();
      throw e.code === -1 ? new P.ProtocolError("Nothing answered. Check the cable and the ports, and that Nemesis Beta 2 is on the Drumboy.", P.GONE) : e;
    }
    connected(info);
    await refresh();
  }, (e) => { disconnect(); return "The device answered, then stopped: " + e.message + ". Press Ask the device to try again."; });

  // ---- to the computer
  function save(name, bytes) {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
    a.download = name;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  }
  // A zip of stored files: nothing to install to open it, and this page can read it back.
  const zip = P.zip;
  function unzip(u8) {
    const out = [], rd = (o) => P.rd32(u8, o);
    for (let o = 0; o + 30 <= u8.length && rd(o) === 0x04034B50;) {
      if (P.rd16(u8, o + 8) !== 0 || P.rd16(u8, o + 6) & 8) throw new Error("this zip is compressed");
      const size = rd(o + 18), nl = P.rd16(u8, o + 26), xl = P.rd16(u8, o + 28), start = o + 30 + nl + xl;
      const name = new TextDecoder().decode(u8.subarray(o + 30, o + 30 + nl));
      if (start + size > u8.length) throw new Error("this zip is cut off");
      out.push({ name: name.split("/").pop(), bytes: u8.slice(start, start + size) });
      o = start + size;
    }
    return out;
  }
  async function backup(entries) {
    const got = [];
    let i = 0;
    for (const e of entries) {
      const bytes = await S.client.download(e.id);
      const p = P.parsePreset(bytes);
      if (!p.ok) throw new P.ProtocolError(`preset ${e.id} came back wrong: ${p.why}`);
      if (p.id !== e.id) throw new P.ProtocolError(`the device sent preset ${p.id} when asked for ${e.id}`);
      got.push({ name: P.fileName(p.id, p.name), bytes });
      progress(++i, entries.length);
    }
    if (got.length === 1) save(got[0].name, got[0].bytes);
    else save(`nemesis-presets-${new Date().toISOString().slice(0, 10)}.zip`, zip(got));
    status(`Handed ${got.length} preset${got.length === 1 ? "" : "s"} to the browser as a download.`, "good");
  }
  $("saveMidi").onclick = () => job(async () => {
    const song = Number($("songNo").value);
    save(`nemesis-song-${song}.mid`, await S.client.songMidi(song - 1, progress));
    spent();
    status(`Handed song ${song} to the browser as a MIDI file.`, "good");
  }, (e) => { spent(); return `The song did not arrive: ${e.message}. Nothing was saved. You can try again.`; });
  const failedBackup = (e) => { spent(); return `The backup stopped: ${e.message}. Nothing was saved. You can try again.`; };
  $("backupSel").onclick = () => job(async () => { await backup(S.list.filter((e) => devSel.has(e.id))); spent(); }, failedBackup);
  $("backupAll").onclick = () => job(async () => { await backup(S.list.filter((e) => !e.builtIn)); spent(); }, failedBackup);
  $("refresh").onclick = () => job(async () => { await refresh(); spent(); status("The list is up to date.", "good"); }, (e) => { spent(); return "The list could not be read: " + e.message + ". You can try again."; });

  // ---- a question that needs a press: nothing is sent to the device without it
  function ask(title, text, lines, goLabel) {
    const d = $("ask");
    $("askTitle").textContent = title; $("askText").textContent = text; $("askGo").textContent = goLabel;
    const ul = $("askList");
    ul.textContent = "";
    for (const l of lines) { const li = document.createElement("li"); li.textContent = l; ul.appendChild(li); }
    return new Promise((res) => {
      const done = (v) => { d.onclose = null; d.close(); res(v); };
      $("askGo").onclick = () => done(true);
      $("askNo").onclick = () => done(false);
      d.onclose = () => res(false);        // Esc
      d.showModal();
      $("askNo").focus();                  // Cancel has the keyboard: Enter must not change a device
    });
  }

  // After a job the list is read again: its work may have gone through, in part. What the job said stays on show: the log takes anything that goes wrong here.
  async function reread() {
    if (S.busy || !S.client) return;
    S.busy = true; gadgets();
    try { await refresh(); }
    catch (e) {   // after a job that failed, what it said stays on show: it says how far it got
      if (S.jobOk) status("That was done, but the list could not be read again, so it may be out of date. Press Refresh.", "bad");
      else log("The list could not be read after that. Press Refresh.");
    }
    S.busy = false; gadgets();
  }

  $("deleteSel").onclick = async () => {
    const chosen = S.list.filter((e) => devSel.has(e.id));
    if (!chosen.length || S.busy) return;
    const yes = await ask(`Delete from ${where()}?`, `This removes ${chosen.length} preset${chosen.length === 1 ? "" : "s"} from ${where()} for good. Back them up first if you may want them again.`, chosen.map((e) => `${e.id}  ${e.name}`), `Delete ${chosen.length}`);
    if (!yes || !S.client) { log("Nothing was deleted."); return; }
    let n = 0;
    await job(async () => {
      for (const e of chosen) {
        await S.client.remove(e.id, e.nameField);
        devSel.delete(e.id);
        progress(++n, chosen.length);
      }
      spent();
      status(`Deleted ${n} preset${n === 1 ? "" : "s"} from ${where()}.`, "good");
    }, (e) => { spent(); return `Delete stopped: ${e.message}. ${n} of ${chosen.length} were deleted` + (e.code === -1 ? ", and the next one may have been too." : "."); });
    await reread();
  };

  $("uploadSel").onclick = async () => {
    const chosen = [...pcSel].sort((a, b) => a - b).map((i) => S.files[i]);
    if (!chosen.length || S.busy) return;
    const has = (id) => S.list.some((e) => e.id === id);
    const builtIn = chosen.filter((f) => S.list.some((e) => e.id === f.p.id && e.builtIn));
    const replacing = chosen.filter((f) => has(f.p.id));
    const lines = chosen.map((f) => `${f.p.id}  ${f.p.name}` + (has(f.p.id) ? `  (replaces ${S.list.find((e) => e.id === f.p.id).name})` : ""));
    const dup = chosen.find((f, i) => chosen.findIndex((g) => g.p.id === f.p.id) !== i);
    if (dup) { status(`Two of the selected files have the number ${dup.p.id}. Keep one of them.`, "bad"); return; }
    if (builtIn.length) { status(`Number ${builtIn[0].p.id} is a built-in preset, which cannot be replaced.`, "bad"); return; }
    const text = replacing.length ? `This writes ${chosen.length} preset${chosen.length === 1 ? "" : "s"} to ${where()}, and ${replacing.length} of them replace${replacing.length === 1 ? "s" : ""} a preset already there.` : `This writes ${chosen.length} preset${chosen.length === 1 ? "" : "s"} to ${where()}.`;
    const yes = await ask(`Send to ${where()}?`, text, lines, `Send ${chosen.length}`);
    if (!yes || !S.client) { log("Nothing was sent."); return; }
    let n = 0;
    await job(async () => {
      for (const f of chosen) {
        await S.client.upload(f.bytes, has(f.p.id));
        progress(++n, chosen.length);
      }
      spent();
      status(`Sent ${n} preset${n === 1 ? "" : "s"} to ${where()}.`, "good");
    }, (e) => { spent(); return `Sending stopped: ${e.message}. ${n} of ${chosen.length} went through` + (e.code === -1 ? ", and the next one may have too." : "."); });
    await reread();
  };

  // ---- from the computer
  async function addFiles(list) {
    for (const file of list) {
      let u8;
      if (file.size > 64 * 1024 * 1024) { S.files.push({ name: file.name, bytes: new Uint8Array(0), p: { ok: false, why: "it is far too big to be presets" } }); continue; }
      try { u8 = new Uint8Array(await file.arrayBuffer()); }
      catch (e) { S.files.push({ name: file.name, bytes: new Uint8Array(0), p: { ok: false, why: "it could not be read (a folder?)" } }); continue; }
      if (/\.zip$/i.test(file.name)) {
        let inside;
        try { inside = unzip(u8); } catch (e) { S.files.push({ name: file.name, bytes: u8, p: { ok: false, why: `${e.message}. Only zips made by this page can be opened` } }); continue; }
        if (!inside.length) S.files.push({ name: file.name, bytes: u8, p: { ok: false, why: "there are no files in this zip" } });
        for (const f of inside) S.files.push({ ...f, p: P.parsePreset(f.bytes) });
      } else S.files.push({ name: file.name, bytes: u8, p: P.parsePreset(u8) });
    }
    drawFiles();
    log(`${S.files.filter((f) => f.p.ok).length} valid preset files in the list.`);
  }
  $("files").onchange = async (e) => { await addFiles([...e.target.files]); e.target.value = ""; };
  $("clearFiles").onclick = () => { S.files = []; pcSel.clear(); drawFiles(); };
  const drop = $("drop");
  drop.ondragover = (e) => { e.preventDefault(); drop.classList.add("over"); };
  drop.ondragleave = () => drop.classList.remove("over");
  drop.ondrop = (e) => { e.preventDefault(); drop.classList.remove("over"); addFiles([...e.dataTransfer.files]); };

  Ed = globalThis.PresetEditor({ S, status, log, progress, job, ask, save, spent, reread, refresh, where });
  gadgets();
})();
