#include "app/MidiExport.h"

#include <new>
#include <string.h>

#include "app/MidiMap.h"
#include "app/Sequencer.h"

namespace app {

namespace {

constexpr double kRate = 44100.0;
constexpr int kBlock = 512;
constexpr uint32_t kMostSeconds = 4 * 3600;   // an end for a song that would not have one

struct Counter : ByteSink {
    uint32_t bytes = 0;
    bool write(const void*, size_t n) override { bytes += (uint32_t)n; return true; }
};

bool be32(ByteSink& o, uint32_t v) { const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; return o.write(b, 4); }
bool be16(ByteSink& o, uint32_t v) { const uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v}; return o.write(b, 2); }

bool delta(ByteSink& o, uint32_t v) {
    uint8_t b[5];
    int n = 0;
    b[n++] = (uint8_t)(v & 0x7F);
    while ((v >>= 7) != 0) b[n++] = (uint8_t)((v & 0x7F) | 0x80);
    bool ok = true;
    while (n > 0) ok = o.write(&b[--n], 1) && ok;
    return ok;
}

bool text(ByteSink& o, uint8_t kind, const char* s) {
    const size_t n = strlen(s);
    const uint8_t b[3] = {0x00, 0xFF, kind};
    return o.write(b, 3) && delta(o, (uint32_t)n) && o.write(s, n);
}

struct Event {
    uint32_t tick;
    uint8_t lane, status, note, vel;
};
constexpr int kEvents = 2 * kMidiMostNotes;   // 384 KB, taken once (midiExportInit), like the sequencer beside it
Event* gEvents = nullptr;
void* gSequencer = nullptr;
bool gTooMany = false;

// The notes as they come off the sequencer, which hands them over in order of time
class Collector : public NoteSink {
   public:
    Collector(const Project& pj, double ticksPerSample) : pj_(pj), perSample_(ticksPerSample) { memset(heldLane_, 0xFF, sizeof heldLane_); }
    uint64_t base = 0;   // the block's first sample
    int count = 0;
    bool full = false;
    uint32_t notes[kLanes] = {};

    int32_t noteOn(const NoteStart& n, int offset) override {
        const int note = noteOut(pj_, n.lane, n.pitch, n.sampler != 0);
        int vel = (int)(n.vel01 * 127.0f + 0.5f);
        vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;
        const uint64_t at = base + (uint64_t)offset;
        for (int i = 0; i < kHeld; ++i)   // the same note again: it ends first, as it does at the socket
            if (heldLane_[i] == n.lane && heldNote_[i] == (uint8_t)note) end(i, at);
        const uint32_t tick = ticks(at);
        add(n.lane, 0x90, note, vel, tick);
        ++notes[n.lane];
        for (int i = 0; i < kHeld; ++i) {
            if (heldLane_[i] != 0xFF) continue;
            heldLane_[i] = n.lane;
            heldNote_[i] = (uint8_t)note;
            heldOn_[i] = tick;
            heldGen_[i] = ++gen_;
            return (int32_t)gen_ << 8 | i;
        }
        add(n.lane, 0x80, note, 0, tick + 1);   // more at once than any part plays: it still ends
        return -1;
    }
    void noteOff(int, int32_t handle, int offset) override {
        const int i = handle & 0xFF;
        if (handle < 0 || i >= kHeld || heldLane_[i] == 0xFF || heldGen_[i] != (uint32_t)handle >> 8) return;   // (ended already, by the same note again)
        end(i, base + (uint64_t)offset);
    }
    void endAll(uint64_t at) {
        for (int i = 0; i < kHeld; ++i) if (heldLane_[i] != 0xFF) end(i, at);
    }

   private:
    static constexpr int kHeld = 128;
    uint32_t ticks(uint64_t sample) const { return (uint32_t)((double)sample * perSample_ + 0.5); }
    void end(int i, uint64_t sample) {
        const uint32_t tick = ticks(sample);
        add(heldLane_[i], 0x80, heldNote_[i], 0, tick > heldOn_[i] ? tick : heldOn_[i] + 1);   // (a note has a length, however short)
        heldLane_[i] = 0xFF;
    }
    void add(int lane, int status, int note, int vel, uint32_t tick) {
        if (count >= kEvents) { full = true; return; }
        gEvents[count++] = Event{tick, (uint8_t)lane, (uint8_t)status, (uint8_t)note, (uint8_t)vel};
    }
    const Project& pj_;
    double perSample_;
    uint8_t heldLane_[kHeld], heldNote_[kHeld];
    uint32_t heldOn_[kHeld] = {}, heldGen_[kHeld] = {}, gen_ = 0;
};

// The song once through. The same run each time: the sequencer starts from its own seed, on a transport of its own
bool collect(Project& pj, int song, Collector& c) {
    Transport tp;
    const Song& so = pj.songs[song];
    tp.songIdx = (uint8_t)song;
    tp.songPlaying = 1;
    tp.pat = so.rows[0].pat;
    tp.playRow = 0;
    tp.repLeft = so.rows[0].rep;
    tp.playing = 1;
    tp.playGen = 1;
    Sequencer* seq = new (gSequencer) Sequencer(pj, tp, c);
    seq->setSampleRate(kRate);
    uint64_t at = 0;
    const uint64_t most = (uint64_t)(kMostSeconds * kRate);
    while (tp.playing && at < most && !c.full) {
        c.base = at;
        seq->process(kBlock, 0);
        at += kBlock;
    }
    for (int i = 0; i < (int)(kRate * 4 / kBlock) && seq->pending() > 0 && !c.full; ++i) {   // what a strum or a long gate still has to end
        c.base = at;
        seq->process(kBlock, 0);
        at += kBlock;
    }
    c.endAll(at);
    return !c.full;
}

// The song's last tick, rests and all, so that a track ends where the song does
uint32_t songTicks(const Project& pj, int song) {
    const Song& so = pj.songs[song];
    uint32_t ticks = 0;
    for (int r = 0; r < so.nRows; ++r) ticks += (uint32_t)so.rows[r].rep * (uint32_t)patLen(pj.patterns[so.rows[r].pat]) * kMidiPpq / 4;
    return ticks;
}

// `end`: the delta to the end of the track goes in the file
bool laneEvents(const Project& pj, const Collector& c, int lane, uint32_t end, ByteSink& o) {
    const LaneMidi& m = pj.midi[lane];
    const uint8_t ch = (uint8_t)((m.ch ? m.ch - 1 : lane) & 0x0F);
    uint32_t last = 0;
    bool ok = true;
    for (int i = 0; i < c.count && ok; ++i) {
        const Event& e = gEvents[i];
        if (e.lane != lane) continue;
        const uint32_t tick = e.tick < last ? last : e.tick;
        const uint8_t b[3] = {(uint8_t)(e.status | ch), e.note, e.vel};
        ok = delta(o, tick - last) && o.write(b, 3);
        last = tick;
    }
    return ok && delta(o, end > last ? end - last : 0);
}

}  // namespace

bool midiTooManyNotes() { return gTooMany; }

int laneFirstRow(const Project& pj, int song, int lane) {
    const Song& so = pj.songs[song];
    for (int r = 0; r < so.nRows; ++r) {
        const Lane& l = pj.patterns[so.rows[r].pat].lanes[lane];
        for (int i = 0; i < l.n; ++i) if (trig(l, i, false)) return r;
    }
    return 0;
}

void midiExportInit() {
    if (gEvents) return;
    gEvents = new Event[kEvents];
    gSequencer = ::operator new(sizeof(Sequencer));
}

bool writeSongMidi(Project& pj, int song, const SampleLib* lib, ByteSink& out) {
    if (song < 0 || song >= kSongs || pj.songs[song].nRows == 0) return false;
    midiExportInit();
    pj.songs[song].loop = 0;
    gTooMany = false;
    Collector c(pj, kMidiPpq * pj.bpm / (60.0 * kRate));
    if (!collect(pj, song, c)) { gTooMany = true; return false; }
    const uint32_t end = songTicks(pj, song);
    int tracks = 1;
    for (int l = 0; l < kLanes; ++l) tracks += c.notes[l] ? 1 : 0;

    bool ok = out.write("MThd", 4) && be32(out, 6) && be16(out, 1) && be16(out, (uint32_t)tracks) && be16(out, kMidiPpq);
    const uint32_t usPerBeat = (uint32_t)(60000000.0 / pj.bpm + 0.5);
    Counter head;
    const auto tempoTrack = [&](ByteSink& o) {
        static const uint8_t kFourFour[] = {0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}, kEnd[] = {0x00, 0xFF, 0x2F, 0x00};
        const uint8_t tempo[] = {0x00, 0xFF, 0x51, 0x03, (uint8_t)(usPerBeat >> 16), (uint8_t)(usPerBeat >> 8), (uint8_t)usPerBeat};
        return text(o, 0x03, pj.name) && o.write(tempo, sizeof tempo) && o.write(kFourFour, sizeof kFourFour) && o.write(kEnd, sizeof kEnd);
    };
    tempoTrack(head);
    ok = ok && out.write("MTrk", 4) && be32(out, head.bytes) && tempoTrack(out);

    for (int l = 0; l < kLanes && ok; ++l) {
        if (!c.notes[l]) continue;
        Str<48> name;
        name.add("Lane ").num(l + 1);
        const Lane& ln = pj.patterns[pj.songs[song].rows[laneFirstRow(pj, song, l)].pat].lanes[l];
        if (isSampler(pj, ln.part) && lib && ln.sample() >= 0 && ln.sample() < lib->count) name.ch(' ').add(lib->items[ln.sample()].name);
        else name.add(" Part ").add(kPartName[ln.part]);
        const auto track = [&](ByteSink& o) {
            static const uint8_t kEnd[] = {0xFF, 0x2F, 0x00};
            return text(o, 0x03, name.c_str()) && laneEvents(pj, c, l, end, o) && o.write(kEnd, sizeof kEnd);
        };
        Counter n;
        track(n);
        ok = out.write("MTrk", 4) && be32(out, n.bytes) && track(out);
    }
    return ok;
}

}  // namespace app
