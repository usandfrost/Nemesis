"use strict";
// The preset librarian's protocol (docs/PRESET_SYSEX.md): the bytes of a message, the .PRS file checks (the same ones as firmware/src/app/PresetFile.h), and the client the page
// talks through. Nothing here knows about Web MIDI or the simulated device: `send(bytes)` goes out, `receive(bytes)` comes in.
(() => {
  const HEAD = [0xF0, 0x7D, 0x4E, 0x50, 0x01];   // start, the non-commercial maker number, "NP", the protocol's version
  const CMD = { IDENTIFY: 0x01, LIST: 0x02, DOWNLOAD: 0x03, UPLOAD_BEGIN: 0x04, UPLOAD_COMMIT: 0x05, DELETE: 0x06, ABORT: 0x07, SONG_MIDI: 0x08,
    IDENTITY: 0x41, DATA_BEGIN: 0x42, DATA_CHUNK: 0x43, DATA_END: 0x44, ACK: 0x45, DONE: 0x46, ERROR: 0x7F };
  const ERR = { NO_SONG: 16, BAD_MESSAGE: 1, UNKNOWN_COMMAND: 2, BAD_VERSION: 3, NOT_FOUND: 4, BUILT_IN: 5, BAD_PRESET: 6, CRC_MISMATCH: 7, NO_CARD: 8, LIBRARY_FULL: 9, BUSY: 10, TIMEOUT: 11, SEQUENCE: 12, ID_IN_USE: 13, WRITE_FAILED: 14, NAME_MISMATCH: 15 };
  const ERR_TEXT = { 1: "a message arrived damaged", 2: "the device does not know that command", 3: "the device speaks another version of the protocol", 4: "that preset is not on the device",
    5: "that is a built-in preset, which cannot be changed", 6: "the device says the preset file is not valid", 7: "the checksum of the whole transfer did not match, so nothing was kept",
    8: "the device has no card in it", 9: "the preset library is full", 10: "the device is busy with something else", 11: "the transfer stopped halfway and the device gave up on it",
    16: "that song has no rows, or it is too long to send", 12: "a piece of the transfer went missing", 13: "a preset with that number is already on the device", 14: "the device could not write to its card", 15: "the preset on the device is not the one in this list any more: refresh the list" };
  const KIND = { LIST: 1, PRESET: 2, SONG: 3 };
  const CHUNK = 154, ACK_BEGIN = 0xFFFF, ENTRY = 29, GONE = -2;   // bytes of data in a chunk; the ACK that answers UPLOAD_BEGIN; bytes of a list entry
  const REPLY_MS = 1000, COMMIT_MS = 30000;           // how long the client waits for an answer, and for one that has to reach the card

  // ---- 8 bits in 7: every group of up to seven bytes is led by a byte that holds their top bits (bit 0 for the group's first byte)
  function pack7(d) {
    const out = [];
    for (let i = 0; i < d.length; i += 7) {
      let top = 0;
      const n = Math.min(7, d.length - i);
      for (let k = 0; k < n; ++k) top |= ((d[i + k] >> 7) & 1) << k;
      out.push(top);
      for (let k = 0; k < n; ++k) out.push(d[i + k] & 0x7F);
    }
    return out;
  }
  function unpack7(d) {
    const out = [];
    for (let i = 0; i < d.length; i += 8) {
      const top = d[i];
      for (let k = 1; k < 8 && i + k < d.length; ++k) out.push(d[i + k] | (((top >> (k - 1)) & 1) << 7));
    }
    return Uint8Array.from(out);
  }

  const sum7 = (a) => a.reduce((s, v) => s + v, 0) & 0x7F;
  // A whole message. The check covers the version, the command, the tag and the packed payload.
  function encode(cmd, tag, payload = []) {
    const body = [HEAD[4], cmd, tag & 0x7F, ...pack7(payload)];
    return Uint8Array.from([...HEAD.slice(0, 4), ...body, sum7(body), 0xF7]);
  }
  // A message's parts, { ours: false } when it is not this protocol's, or { bad } when it is and it is damaged.
  function decode(bytes) {
    const b = Array.from(bytes);
    if (b.length < 4 || b[0] !== 0xF0 || b[1] !== HEAD[1] || b[2] !== HEAD[2] || b[3] !== HEAD[3]) return { ours: false };
    if (b[b.length - 1] !== 0xF7 || b.length < 9) return { ours: true, bad: "the message is cut off" };
    const body = b.slice(4, -2);
    if (body.some((v) => v > 0x7F)) return { ours: true, bad: "a byte in the message has its top bit set" };
    if (sum7(body) !== b[b.length - 2]) return { ours: true, bad: "the message check does not match" };
    if (body[0] !== HEAD[4]) return { ours: true, bad: "the message is another version of the protocol", version: body[0] };
    return { ours: true, cmd: body[1], tag: body[2], payload: unpack7(body.slice(3)) };
  }

  // ---- little-endian fields, as the preset file has them
  const u16 = (v) => [v & 255, (v >> 8) & 255];
  const u32 = (v) => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255];
  const rd16 = (p, o) => p[o] | (p[o + 1] << 8);
  const rd32 = (p, o) => (p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (p[o + 3] << 24)) >>> 0;

  const CRC_TABLE = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  function crc32(d) { let c = 0xFFFFFFFF; for (let i = 0; i < d.length; ++i) c = CRC_TABLE[(c ^ d[i]) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; }

  // ---- a .PRS file (firmware/src/app/PresetFile.h)
  const PRS_SIZE = 148, ID_MIN = 10, ID_MAX = 32000, NAME_MAX = 20, NAME_FIELD = 22, BANKS = 200, VOICES_MAX = 5, LIBRARY_MAX = 1024;
  const CATS = ["ALL", "BASS", "LEAD", "PAD", "PLUCK", "STAB", "POLY", "KEYS", "FX", "SEQ", "TEXTURE", "BELL"];
  function nameOk(f) {
    let n = 0;
    while (n < f.length && f[n]) { if (f[n] < 0x20 || f[n] > 0x7E) return false; ++n; }
    if (n < 1 || n > NAME_MAX || f[0] === 0x20) return false;
    for (let i = n; i < f.length; ++i) if (f[i]) return false;
    return true;
  }
  const nameOf = (f) => { let s = ""; for (let i = 0; i < f.length && f[i]; ++i) s += String.fromCharCode(f[i]); return s; };
  // What parsePresetFile refuses, refused here too, in words. { ok: true, id, cat, voices, banks, name } or { ok: false, why }.
  function parsePreset(d) {
    if (d.length !== PRS_SIZE) return { ok: false, why: `it is ${d.length} bytes, and a preset file is ${PRS_SIZE}` };
    if (d[0] !== 0x44 || d[1] !== 0x42 || d[2] !== 0x50 || d[3] !== 0x52) return { ok: false, why: "it does not start like a preset file" };
    if (d[4] !== 1 || d[5] !== 0) return { ok: false, why: "it is from another version of the preset format" };
    if (crc32(d.subarray(0, PRS_SIZE - 4)) !== rd32(d, PRS_SIZE - 4)) return { ok: false, why: "its checksum does not match, so the file is damaged" };
    const id = rd16(d, 6), cat = d[8], voices = d[9], banks = [d[10], d[11]], field = d.subarray(12, 12 + NAME_FIELD);
    if (id < ID_MIN || id > ID_MAX) return { ok: false, why: `its number ${id} is outside ${ID_MIN} to ${ID_MAX}` };
    if (cat < 1 || cat >= CATS.length) return { ok: false, why: "its category is not one the firmware knows" };
    if (voices < 1 || voices > VOICES_MAX) return { ok: false, why: `it asks for ${voices} voices, and a part has 1 to ${VOICES_MAX}` };
    if (banks.some((b) => b !== 255 && b >= BANKS)) return { ok: false, why: "it names a wavetable bank that does not exist" };
    if (!nameOk(field) || d[34] || d[35]) return { ok: false, why: "its name field is not valid" };
    if (d[111] & ~15) return { ok: false, why: "its effect settings are not valid" };
    return { ok: true, id, cat, voices, banks, name: nameOf(field) };
  }
  // The file name a preset goes by, as presetFileNameFor makes it: "0123 Name.PRS"
  const fileName = (id, name) => String(id).padStart(4, "0") + " " + name.replace(/[^0-9A-Za-z _.()-]/g, "_") + ".PRS";

  // ---- a list entry: id, category, voices, banks, flags, the 22 bytes of the name
  const BUILT_IN = 1;
  function listEntry(e) { const name = new Uint8Array(NAME_FIELD); for (let i = 0; i < e.name.length && i < NAME_FIELD; ++i) name[i] = e.name.charCodeAt(i); return [...u16(e.id), e.cat, e.voices, e.banks[0], e.banks[1], e.builtIn ? BUILT_IN : 0, ...name]; }
  function parseList(d) {
    const out = [];
    for (let o = 0; o + ENTRY <= d.length; o += ENTRY) {
      const field = d.subarray(o + 7, o + ENTRY);
      out.push({ id: rd16(d, o), cat: d[o + 2], voices: d[o + 3], banks: [d[o + 4], d[o + 5]], builtIn: !!(d[o + 6] & BUILT_IN), name: nameOf(field), nameField: Uint8Array.from(field) });
    }
    return out;
  }

  class ProtocolError extends Error { constructor(msg, code) { super(msg); this.code = code; } }

  // ---- the client: one request at a time
  class Client {
    constructor(send, log) { this.send = send; this.log = log || (() => {}); this.tag = 0; this.wait = null; this.got = []; }
    // Hands a message that came in to whoever waits for it. Replies with another tag are stale: a late answer to a request that was given up on.
    receive(bytes) {
      const m = decode(bytes);
      if (!m.ours) return;
      if (m.bad) { this.fail(new ProtocolError(m.version !== undefined ? ERR_TEXT[ERR.BAD_VERSION] : m.bad)); return; }
      if (m.tag !== this.tag) return;
      this.got.push(m);
      this.pump();
    }
    pump() { if (this.wait && this.got.length) { const w = this.wait; this.wait = null; clearTimeout(w.t); w.res(this.got.shift()); } }
    fail(e) { if (this.wait) { const w = this.wait; this.wait = null; clearTimeout(w.t); w.rej(e); } else this.pendingFail = e; }
    next(ms = REPLY_MS) {
      return new Promise((res, rej) => {
        if (this.pendingFail) { const e = this.pendingFail; this.pendingFail = null; rej(e); return; }
        const t = setTimeout(() => { this.wait = null; rej(new ProtocolError("no answer from the device", -1)); }, ms);
        this.wait = { res, rej, t };
        this.pump();
      });
    }
    start() { this.tag = (this.tag % 127) + 1; this.got = []; this.pendingFail = null; return this.tag; }
    async reply(ms, want) {
      const m = await this.next(ms);
      if (m.cmd === CMD.ERROR) throw new ProtocolError(ERR_TEXT[m.payload[1]] || "the device reported an error", m.payload[1]);
      if (!want.includes(m.cmd)) throw new ProtocolError("the device answered out of turn");
      return m;
    }
    async identify() {
      this.send(encode(CMD.IDENTIFY, this.start()));
      const p = (await this.reply(REPLY_MS, [CMD.IDENTITY])).payload;
      return { protocol: p[0], major: p[1], minor: p[2], simulated: !!(p[3] & 1), count: rd16(p, 4), capacity: rd16(p, 6), name: nameOf(p.subarray(8, 16)) };
    }
    // A transfer from the device: begin, the chunks in order, end. The whole of it is checked against the size and CRC-32 the device announced.
    // Given up on, the device is told: it would go on sending, and until it stopped it would not answer anything else.
    async receiveData(kind, onProgress) {
      try { return await this.incoming(kind, onProgress); }
      catch (e) { this.abort(); throw e; }
    }
    abort() { try { this.send(encode(CMD.ABORT, this.tag)); } catch (ignored) { /* the port has gone: the first error is the one to tell */ } }
    async incoming(kind, onProgress) {
      const b = (await this.reply(kind === KIND.SONG ? COMMIT_MS : REPLY_MS, [CMD.DATA_BEGIN])).payload;   // (a song is worked out first)
      if (b[0] !== kind) throw new ProtocolError("the device sent something else than was asked for");
      const size = rd32(b, 1), crc = rd32(b, 5), chunks = rd16(b, 9);
      if (size > (kind === KIND.LIST ? (LIBRARY_MAX + 3) * ENTRY : kind === KIND.SONG ? 1 << 20 : PRS_SIZE)) throw new ProtocolError("the device announced more than a transfer can hold");
      const data = new Uint8Array(size);
      let at = 0;
      for (let i = 0; i < chunks; ++i) {
        const c = (await this.reply(REPLY_MS, [CMD.DATA_CHUNK])).payload;
        if (rd16(c, 0) !== i) throw new ProtocolError(ERR_TEXT[ERR.SEQUENCE], ERR.SEQUENCE);
        const part = c.subarray(2);
        if (at + part.length > size) throw new ProtocolError("the device sent more than it announced");
        data.set(part, at); at += part.length;
        if (onProgress) onProgress(at, size);
      }
      await this.reply(REPLY_MS, [CMD.DATA_END]);
      if (at !== size) throw new ProtocolError(ERR_TEXT[ERR.SEQUENCE], ERR.SEQUENCE);
      if (crc32(data) !== crc) throw new ProtocolError(ERR_TEXT[ERR.CRC_MISMATCH], ERR.CRC_MISMATCH);
      return data;
    }
    async list(onProgress) { this.send(encode(CMD.LIST, this.start())); return parseList(await this.receiveData(KIND.LIST, onProgress)); }
    async songMidi(song, onProgress) { this.send(encode(CMD.SONG_MIDI, this.start(), [song])); return this.receiveData(KIND.SONG, onProgress); }
    async download(id, onProgress) { this.send(encode(CMD.DOWNLOAD, this.start(), u16(id))); return this.receiveData(KIND.PRESET, onProgress); }
    // A transfer to the device: each chunk waits for its ACK, so the device sets the pace. Nothing counts until UPLOAD_COMMIT, and that is only sent when every chunk arrived.
    async upload(bytes, replace, onProgress) {
      const p = parsePreset(bytes);
      if (!p.ok) throw new ProtocolError("this file cannot be sent: " + p.why);
      const tag = this.start();
      try {
        this.send(encode(CMD.UPLOAD_BEGIN, tag, [...u16(p.id), ...u16(bytes.length), ...u32(crc32(bytes)), replace ? 1 : 0]));
        const open = await this.reply(REPLY_MS, [CMD.ACK]);
        if (rd16(open.payload, 0) !== ACK_BEGIN) throw new ProtocolError("the device answered out of turn");
        const chunks = Math.ceil(bytes.length / CHUNK);
        for (let i = 0; i < chunks; ++i) {
          const part = bytes.subarray(i * CHUNK, (i + 1) * CHUNK);
          this.send(encode(CMD.DATA_CHUNK, tag, [...u16(i), ...part]));
          const a = await this.reply(REPLY_MS, [CMD.ACK]);
          if (rd16(a.payload, 0) !== i) throw new ProtocolError(ERR_TEXT[ERR.SEQUENCE], ERR.SEQUENCE);
          if (onProgress) onProgress(Math.min((i + 1) * CHUNK, bytes.length), bytes.length);
        }
      } catch (e) { this.abort(); throw e; }
      this.send(encode(CMD.UPLOAD_COMMIT, tag));
      const d = await this.reply(COMMIT_MS, [CMD.DONE]);
      return { id: rd16(d.payload, 0), count: rd16(d.payload, 2) };
    }
    // The delete names the preset by its number and the CRC-32 of its name field, so a stale list cannot remove something else.
    async remove(id, nameField) {
      this.send(encode(CMD.DELETE, this.start(), [...u16(id), ...u32(crc32(nameField))]));
      const d = await this.reply(COMMIT_MS, [CMD.DONE]);
      return { id: rd16(d.payload, 0), count: rd16(d.payload, 2) };
    }
  }

  // Files in one .zip, stored as they are
  function zip(files) {
    const le16 = (v) => [v & 255, v >> 8], le32 = (v) => u32(v), parts = [], dir = [];
    let at = 0;
    for (const { name, bytes } of files) {
      const n = Array.from(new TextEncoder().encode(name)), crc = le32(crc32(bytes)), size = le32(bytes.length);
      const common = [...le16(20), 0, 0, 0, 0, 0, 0, 0x21, 0, ...crc, ...size, ...size, ...le16(n.length), 0, 0];   // version, flags, stored, time, a date of 1980-01-01
      const head = [0x50, 0x4B, 3, 4, ...common, ...n];
      dir.push([0x50, 0x4B, 1, 2, ...le16(20), ...common, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ...le32(at), ...n]);
      parts.push(Uint8Array.from(head), bytes);
      at += head.length + bytes.length;
    }
    const d = Uint8Array.from(dir.flat());
    const end = Uint8Array.from([0x50, 0x4B, 5, 6, 0, 0, 0, 0, ...le16(files.length), ...le16(files.length), ...le32(d.length), ...le32(at), 0, 0]);
    return new Blob([...parts, d, end]);
  }
  globalThis.PresetSysex = { zip, CMD, ERR, ERR_TEXT, KIND, CHUNK, ACK_BEGIN, ENTRY, GONE, REPLY_MS, COMMIT_MS, PRS_SIZE, LIBRARY_MAX, NAME_FIELD, CATS,
    pack7, unpack7, encode, decode, u16, u32, rd16, rd32, crc32, parsePreset, nameOf, fileName, listEntry, parseList, Client, ProtocolError };
})();
