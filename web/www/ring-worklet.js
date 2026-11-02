// Plays what the engine worker has written into the shared ring buffer (frames in, 128 out per call). Silence when the ring runs dry.
class RingPlayer extends AudioWorkletProcessor {
  constructor(opts) {
    super();
    const { sab, cap } = opts.processorOptions;
    this.cap = cap;
    this.hdr = new Int32Array(sab, 0, 4);
    this.l = new Float32Array(sab, 16, cap);
    this.r = new Float32Array(sab, 16 + cap * 4, cap);
    this.underruns = 0;
  }
  process(inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    const rd = Atomics.load(this.hdr, 0), wr = Atomics.load(this.hdr, 1);
    if (((wr - rd) | 0) >= n) {
      const at = rd & (this.cap - 1);
      const first = Math.min(n, this.cap - at);
      out[0].set(this.l.subarray(at, at + first), 0);
      out[1].set(this.r.subarray(at, at + first), 0);
      if (first < n) { out[0].set(this.l.subarray(0, n - first), first); out[1].set(this.r.subarray(0, n - first), first); }
      Atomics.store(this.hdr, 0, (rd + n) | 0);
    } else {
      this.underruns++;
      Atomics.store(this.hdr, 2, this.underruns);
    }
    return true;
  }
}
registerProcessor("ring-player", RingPlayer);
