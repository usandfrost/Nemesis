"use strict";
// A SIMULATED device, for trying the librarian with no hardware. It is JavaScript in this page and it keeps its presets in memory, until the page is reloaded. It answers the protocol of
// docs/PRESET_SYSEX.md byte for byte, at the speed of a MIDI cable, so the page takes the same path it would with a real device. It proves nothing about any firmware: none speaks this yet.
(() => {
  const P = globalThis.PresetSysex;
  const { CMD, ERR, KIND, CHUNK } = P;
  const BYTE_MS = 10 * 1000 / 31250;                  // a byte on a MIDI cable: ten bits at 31250 baud
  const STALL_MS = 2000;                              // an upload that stops is given up after this long
  const BUILT_IN = [{ id: 0, name: "Iron Squelch", cat: 1, voices: 0 }, { id: 1, name: "Sub Pressure", cat: 1, voices: 0 }, { id: 2, name: "Dimension Bloom", cat: 3, voices: 0 }];

  class SimDevice {
    // `presets`: the .PRS files it starts with. `speed` above 1 is faster than a cable (the tests). `onsend(bytes)`: what the device sends.
    constructor(presets, speed = 1) {
      this.files = new Map();
      for (const d of presets) { const p = P.parsePreset(d); if (p.ok) this.files.set(p.id, d); }
      this.speed = speed;
      this.fault = "none";      // one-shot, set from the page: bad-sum, drop-chunk, corrupt-chunk, no-reply, library-full, write-fail, no-card
      this.onsend = () => {};
      this.up = null;
      this.timer = null;
      this.queue = Promise.resolve();
    }
    takeFault(...kinds) { if (kinds.includes(this.fault)) { const f = this.fault; this.fault = "none"; return f; } return null; }
    wire(bytes) { return new Promise((r) => setTimeout(r, bytes * BYTE_MS / this.speed)); }
    // What goes out takes the cable's time, in order.
    out(bytes) { this.queue = this.queue.then(async () => { await this.wire(bytes.length); this.onsend(bytes); }); return this.queue; }
    msg(cmd, tag, payload) { this.out(P.encode(cmd, tag, payload)); }
    error(forCmd, tag, code, detail = 0) { this.msg(CMD.ERROR, tag, [forCmd, code, ...P.u16(detail)]); }
    entries() {
      const lib = [...this.files.entries()].sort((a, b) => a[0] - b[0]).map(([id, d]) => { const p = P.parsePreset(d); return { id, name: p.name, cat: p.cat, voices: p.voices, banks: p.banks, builtIn: false }; });
      return [...BUILT_IN.map((b) => ({ ...b, banks: [255, 255], builtIn: true })), ...lib];
    }
    // A message comes in over the cable. A damaged one gets an ERROR, as far as its tag can be trusted: it is the byte after the command.
    async receive(bytes) {
      await this.wire(bytes.length);
      const m = P.decode(bytes);
      if (!m.ours) return;
      if (m.bad) { this.error(bytes[5] & 0x7F, bytes[6] & 0x7F, m.version !== undefined ? ERR.BAD_VERSION : ERR.BAD_MESSAGE); return; }
      if (this.takeFault("no-reply")) return;
      this.handle(m);
    }
    handle(m) {
      const p = m.payload, tag = m.tag;
      if (this.up && ![CMD.DATA_CHUNK, CMD.UPLOAD_COMMIT, CMD.ABORT, CMD.IDENTIFY].includes(m.cmd)) { this.error(m.cmd, tag, ERR.BUSY); return; }
      switch (m.cmd) {
        case CMD.IDENTIFY: {
          const name = new Uint8Array(8); "NEMESIS".split("").forEach((c, i) => (name[i] = c.charCodeAt(0)));
          this.msg(CMD.IDENTITY, tag, [1, 0, 0, 1, ...P.u16(this.files.size), ...P.u16(P.LIBRARY_MAX), ...name]);
          break;
        }
        case CMD.LIST: this.sendData(KIND.LIST, tag, Uint8Array.from(this.entries().flatMap(P.listEntry))); break;
        case CMD.SONG_MIDI: {
          if (p[0] > 7) { this.error(m.cmd, tag, ERR.NO_SONG, p[0]); break; }
          // (a real file, with one track and one note)
          this.sendData(KIND.SONG, tag, Uint8Array.from([0x4D, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 1, 0, 1, 0, 96, 0x4D, 0x54, 0x72, 0x6B, 0, 0, 0, 13, 0, 0x90, 60, 100, 48, 0x80, 60, 0, 0, 0xFF, 0x2F, 0, 0]));
          break;
        }
        case CMD.DOWNLOAD: {
          const id = P.rd16(p, 0);
          if (BUILT_IN.some((b) => b.id === id)) { this.error(m.cmd, tag, ERR.BUILT_IN, id); break; }
          const d = this.files.get(id);
          if (!d) { this.error(m.cmd, tag, ERR.NOT_FOUND, id); break; }
          this.sendData(KIND.PRESET, tag, d);
          break;
        }
        case CMD.UPLOAD_BEGIN: {
          if (this.takeFault("no-card")) { this.error(m.cmd, tag, ERR.NO_CARD); break; }
          const id = P.rd16(p, 0), size = P.rd16(p, 2), crc = P.rd32(p, 4), replace = !!(p[8] & 1);
          if (size !== P.PRS_SIZE) { this.error(m.cmd, tag, ERR.BAD_PRESET); break; }
          if (BUILT_IN.some((b) => b.id === id)) { this.error(m.cmd, tag, ERR.BUILT_IN, id); break; }
          if (this.files.has(id) && !replace) { this.error(m.cmd, tag, ERR.ID_IN_USE, id); break; }
          if ((this.takeFault("library-full") || this.files.size >= P.LIBRARY_MAX) && !this.files.has(id)) { this.error(m.cmd, tag, ERR.LIBRARY_FULL); break; }
          this.up = { tag, id, size, crc, data: new Uint8Array(size), at: 0, next: 0 };
          this.stall();
          this.msg(CMD.ACK, tag, P.u16(P.ACK_BEGIN));
          break;
        }
        case CMD.DATA_CHUNK: {
          if (!this.up || tag !== this.up.tag) { this.error(m.cmd, tag, ERR.SEQUENCE); break; }
          const i = P.rd16(p, 0);
          if (i !== this.up.next || this.up.at + p.length - 2 > this.up.size) { this.up = null; clearTimeout(this.timer); this.error(m.cmd, tag, ERR.SEQUENCE, i); break; }
          this.up.data.set(p.subarray(2), this.up.at);
          this.up.at += p.length - 2;
          this.up.next++;
          if (this.takeFault("corrupt-chunk")) this.up.data[20] ^= 0x10;   // a bit flipped in the device's own memory: only the whole transfer's CRC can see it
          this.stall();
          this.msg(CMD.ACK, tag, P.u16(i));
          break;
        }
        case CMD.UPLOAD_COMMIT: {
          const u = this.up;
          if (!u || tag !== u.tag) { this.error(m.cmd, tag, ERR.SEQUENCE); break; }
          this.up = null; clearTimeout(this.timer);
          if (u.at !== u.size) { this.error(m.cmd, tag, ERR.SEQUENCE); break; }
          if (P.crc32(u.data) !== u.crc) { this.error(m.cmd, tag, ERR.CRC_MISMATCH); break; }
          const v = P.parsePreset(u.data);
          if (!v.ok || v.id !== u.id) { this.error(m.cmd, tag, ERR.BAD_PRESET); break; }
          if (this.takeFault("write-fail")) { this.error(m.cmd, tag, ERR.WRITE_FAILED); break; }
          this.files.set(u.id, u.data);
          this.msg(CMD.DONE, tag, [...P.u16(u.id), ...P.u16(this.files.size)]);
          break;
        }
        case CMD.DELETE: {
          const id = P.rd16(p, 0), crc = P.rd32(p, 2);
          if (BUILT_IN.some((b) => b.id === id)) { this.error(m.cmd, tag, ERR.BUILT_IN, id); break; }
          const d = this.files.get(id);
          if (!d) { this.error(m.cmd, tag, ERR.NOT_FOUND, id); break; }
          if (P.crc32(d.subarray(12, 12 + P.NAME_FIELD)) !== crc) { this.error(m.cmd, tag, ERR.NAME_MISMATCH, id); break; }
          if (this.takeFault("write-fail")) { this.error(m.cmd, tag, ERR.WRITE_FAILED); break; }
          this.files.delete(id);
          this.msg(CMD.DONE, tag, [...P.u16(id), ...P.u16(this.files.size)]);
          break;
        }
        case CMD.ABORT: this.up = null; clearTimeout(this.timer); break;   // no answer: the client has already moved on
        default: this.error(m.cmd, tag, ERR.UNKNOWN_COMMAND);
      }
    }
    stall() { clearTimeout(this.timer); const u = this.up; this.timer = setTimeout(() => { if (this.up === u) { this.up = null; this.error(CMD.UPLOAD_BEGIN, u.tag, ERR.TIMEOUT); } }, STALL_MS / this.speed); }
    sendData(kind, tag, data) {
      const chunks = Math.ceil(data.length / CHUNK), crc = P.crc32(data), drop = this.takeFault("drop-chunk") ? Math.min(1, chunks - 1) : -1, corrupt = this.takeFault("corrupt-chunk") ? 0 : -1, bad = this.takeFault("bad-sum") ? 0 : -1;
      this.msg(CMD.DATA_BEGIN, tag, [kind, ...P.u32(data.length), ...P.u32(crc), ...P.u16(chunks)]);
      for (let i = 0; i < chunks; ++i) {
        if (i === drop) continue;
        const part = Array.from(data.subarray(i * CHUNK, (i + 1) * CHUNK));
        if (i === corrupt) part[Math.min(20, part.length - 1)] ^= 0x10;   // the message check is made after this, so only the whole transfer's CRC can see it
        const m = P.encode(CMD.DATA_CHUNK, tag, [...P.u16(i), ...part]);
        if (i === bad) m[m.length - 2] ^= 0x7F;                           // the message's own check is wrong
        this.out(m);
      }
      this.msg(CMD.DATA_END, tag, P.u16(chunks));
    }
  }
  globalThis.SimDevice = SimDevice;
})();
