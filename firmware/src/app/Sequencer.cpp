#include "app/Sequencer.h"

#include <string.h>

namespace app {

double Sequencer::rnd() {   // xorshift32 -> [0, 1)
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0;
}

void Sequencer::push(const Ev& in) {
    if (count_ >= kQueue) {
        // full: a start is dropped (a missing note); an end that cannot wait is delivered now (an early release, never a hanging note)
        if (in.fx == 0 && !in.start) sink_.noteOff(in.note.part, in.handle, 0);
        return;
    }
    Ev e = in;
    e.seq = seq_++;
    int i = count_++;
    while (i > 0) {   // sift up
        const int p = (i - 1) / 2;
        if (!before(e, q_[p])) break;
        q_[i] = q_[p];
        i = p;
    }
    q_[i] = e;
}

void Sequencer::pop() {
    const Ev last = q_[--count_];
    int i = 0;
    for (;;) {   // sift the last element down from the root
        int c = 2 * i + 1;
        if (c >= count_) break;
        if (c + 1 < count_ && before(q_[c + 1], q_[c])) ++c;
        if (!before(q_[c], last)) break;
        q_[i] = q_[c];
        i = c;
    }
    if (count_ > 0) q_[i] = last;
}

void Sequencer::dropStarts() {
    // STOP: notes not yet started are dropped; the ends of the ones sounding still come at their time, so every gate completes
    int n = 0;
    for (int i = 0; i < count_; ++i) if (!q_[i].start && q_[i].fx == 0) q_[n++] = q_[i];   // (the effects locks go with it: below)
    count_ = 0;
    for (int i = 0; i < n; ++i) { const Ev e = q_[i]; push(e); }   // rebuild the heap (seq order is kept: push renumbers in order)
    sink_.fxReleaseAll();
    const Pattern& p = pj_.patterns[tp_.pat];
    for (int li = 0; li < kLanes; ++li)
        if (isSampler(pj_, p.lanes[li].part) && p.lanes[li].smpParam(PG_SLC, SLC_MODE) == SLICE_SYNC) sink_.sliceCut(p.lanes[li].part, li);
}

void Sequencer::deliver(uint64_t end) {
    // everything due in this block, in time order (a start that is due now may queue an end that is also due now)
    while (count_ > 0 && q_[0].at < end) {
        const Ev e = q_[0];
        pop();
        const int off = e.at > clock_ ? (int)(e.at - clock_) : 0;
        if (e.fx == 1) {
            sink_.fxSet(e.note.lane, e.note.part, e.note.locks, e.note.nLocks, e.token, off);
        } else if (e.fx == 2) {
            sink_.fxRelease(e.note.lane, e.token, off);
        } else if (e.start) {
            const int32_t h = sink_.noteOn(e.note, off);
            if (h >= 0) {
                Ev o = e;
                o.start = false;
                o.handle = h;
                o.at = e.at + (e.gate > 0 ? e.gate : 1);
                push(o);
            }
        } else {
            sink_.noteOff(e.note.part, e.handle, off);
        }
    }
}

void Sequencer::process(int frames, uint32_t nowMs) {
    const uint64_t end = clock_ + (uint64_t)frames;
    if (running_ && tp_.playGen != gen_) {   // STOP and PLAY again between two blocks: the old run ends here
        dropStarts();
        running_ = false;
        ending_ = false;
    }
    if (!tp_.playing) {
        tp_.extSteps = 0;   // (a clock's steps are for this run)
        if (running_) { dropStarts(); running_ = false; ending_ = false; tp_.shownStep = -1; }
        deliver(end);
        clock_ = end;
        return;
    }
    if (!running_) {   // PLAY: the first step falls on the start of this block
        running_ = true;
        ending_ = false;
        gen_ = tp_.playGen;
        grid_ = (double)clock_;
    }
    for (;;) {
        const double dur = sr_ * 60.0 / pj_.bpm / 4.0;
        if (ending_) {   // a song that stops: the last step has sounded for its full length
            if (grid_ < (double)end) {
                tp_.playing = 0;
                tp_.songPlaying = 0;
                tp_.pstep = 0;
                tp_.gstep = -1;
                tp_.shownStep = -1;
                tp_.songEnds = tp_.songEnds + 1;
                running_ = false;   // not a STOP: what is queued (a strum's last notes) still plays
                ending_ = false;
            }
            break;
        }
        if (tp_.clockExt) {   // a clock from outside: each step falls when it says, on this block
            if (tp_.extSteps == 0) { grid_ = (double)end; break; }
            tp_.extSteps = (uint16_t)(tp_.extSteps - 1);
            step(clock_, nowMs);
            grid_ = (double)clock_ + dur;
            continue;
        }
        const int32_t g = tp_.gstep + 1;
        const double swung = (g % 2 == 1) ? dur * (swing() / 100.0) : 0.0;
        const double when = grid_ + swung;
        if (when >= (double)end) break;
        step((uint64_t)(when < (double)clock_ ? (double)clock_ : when), nowMs);
        grid_ += dur;
    }
    deliver(end);
    clock_ = end;
}

void Sequencer::step(uint64_t when, uint32_t nowMs) {
    tp_.gstep = tp_.gstep + 1;
    const int ps = tp_.pstep;
    tp_.shownStep = (int16_t)ps;
    const int pat = tp_.pat;
    Pattern& p = pj_.patterns[pat];
    const double sd = 60.0 / pj_.bpm / 4.0;   // seconds per step
    const bool fill = tp_.fill != 0;
    for (int li = 0; li < kLanes; ++li) {
        const Lane& ln = p.lanes[li];
        const bool sliced = isSampler(pj_, ln.part) && ln.smpParam(PG_SLC, SLC_MODE) != SLICE_OFF;
        const bool syncing = sliced && ln.smpParam(PG_SLC, SLC_MODE) == SLICE_SYNC;
        if (ln.mute || ln.n == 0) {
            if (syncing) sink_.sliceCut(ln.part, li);
            continue;
        }
        const int i = ps % ln.n;
        if (syncing) { syncLane(li, ln, pat, when, nowMs, i); continue; }
        if (!trig(ln, i, fill)) continue;
        const Step& st = ln.st[i];
        if (st.prob < 100 && rnd() * 100.0 > st.prob) continue;
        int slice = -1;
        uint8_t sliceRev = 0;
        if (sliced) {   // a step plays the slice of its place in the lane, the one it locks, or (RANDOM) any
            const SliceMap* m = findSlices(pj_, st.lockValue(lockKey(PG_SMP, 0), ln.sample()));
            if (m) {
                slice = pickSlice(*m, ln, st, i, sliceRev);
                if (m->s[slice].flags & SLICE_MUTED) continue;
            }
        }
        tp_.hitAtMs[li] = nowMs;
        const bool smp = isSampler(pj_, ln.part);
        // one step becomes one or more notes: a single note, a chord (all at once, or strummed), or an arpeggio spread over the step
        const int base = (smp ? 0 : ln.root) + st.note;   // a sampler lane reads the note as a pitch offset in semitones
        int notes[kMaxChordNotes];
        const int nn = chordNotes(base, clampi(st.chord, 0, kChordCount - 1), st.inv, st.spread, notes);
        const float vel01 = (float)(st.vel * (ln.level / 127.0) / 127.0);
        Ev e;
        e.start = true;
        e.handle = -1;
        e.note.part = ln.part;
        e.note.pattern = (uint8_t)pat;
        e.note.lane = (uint8_t)li;
        e.note.vel01 = vel01;
        e.note.slice = (int8_t)slice;
        e.note.sliceRev = sliceRev;
        e.fx = 0;
        e.token = 0;
        // the sound locks go with each note; the effects locks (sends, amp) are the step's own: set at its start, let go at its end
        Lock fxl[kLocksPerStep];
        int nf = 0, ns = 0;
        for (int k = 0; k < st.nLocks && k < kLocksPerStep; ++k) {
            if (lockIsFx(st.locks[k].key)) fxl[nf++] = st.locks[k];
            else e.note.locks[ns++] = st.locks[k];
        }
        e.note.nLocks = (uint8_t)ns;
        pushFxLocks(li, ln.part, fxl, nf, when);
        auto emit = [&](double dtSec, int pitch, double gateSec) {
            if (!smp) pitch = scaleNote(pitch, pj_.key, pj_.scale);   // the project scale: synth parts only
            e.note.pitch = (int8_t)clampi(pitch, smp ? -48 : 0, smp ? 48 : 127);
            e.note.sampler = smp ? 1 : 0;
            e.at = when + (uint64_t)(dtSec * sr_ + 0.5);
            const double g = gateSec * sr_;
            e.gate = g < 1 ? 1u : (uint32_t)g;
            push(e);
        };
        if (ln.arp.mode > 0) {
            const int count = kArpRates[clampi(ln.arp.rate, 0, kArpRateCount - 1)];
            const double sub = sd / count;
            int seq[kMaxArpNotes];
            arpSeq(notes, nn, ln.arp.mode, clampi(ln.arp.oct, 1, 4), count, [&]() { return rnd(); }, seq);
            for (int k = 0; k < count; ++k) emit(k * sub, seq[k], sub * ln.arp.gate / 100.0);
        } else {
            for (int k = 0; k < nn; ++k) emit(k * st.strum / 1000.0, notes[k], (st.gate / 32.0) * sd);
        }
    }
    tp_.pstep = (uint16_t)(ps + 1);
    if (tp_.pstep >= patLen(p) && !patternEnd()) ending_ = true;   // a song that stops: let the last step sound, then stop
}

void Sequencer::pushFxLocks(int li, int part, const Lock* locks, int n, uint64_t when) {
    if (n <= 0 || count_ + 2 > kQueue) return;   // (the pair or neither: a set without its release would hold on)
    // it lets go where the next step starts: that one is swung when its index is odd, and a swung step is late by swing% of a step
    const double dur = 60.0 / pj_.bpm / 4.0 * sr_;
    const double nextAt = grid_ + dur + ((tp_.gstep + 1) % 2 == 1 ? dur * (swing() / 100.0) : 0.0);
    uint64_t endAt = (uint64_t)(nextAt + 0.5);
    if (endAt <= when) endAt = when + 1;
    Ev f;
    memset(&f, 0, sizeof(f));
    f.fx = 1;
    f.at = when;
    f.token = ++fxToken_ ? fxToken_ : ++fxToken_;
    f.note.part = (uint8_t)part;
    f.note.lane = (uint8_t)li;
    f.note.nLocks = (uint8_t)n;
    for (int k = 0; k < n; ++k) f.note.locks[k] = locks[k];
    push(f);
    f.fx = 2;
    f.at = endAt;
    f.note.nLocks = 0;
    push(f);
}

int Sequencer::pickSlice(const SliceMap& m, const Lane& ln, const Step& st, int pos, uint8_t& rev) {
    const int locked = st.lockValue(lockKey(PG_SLC, SLC_SLICE), ln.smpParam(PG_SLC, SLC_SLICE));
    int k = (locked > 0 ? locked - 1 : pos) % m.count;
    const int chance = st.lockValue(lockKey(PG_SLC, SLC_RANDOM), ln.smpParam(PG_SLC, SLC_RANDOM));
    if (chance > 0 && rnd() * 127.0 < chance) k = (int)(rnd() * m.count) % m.count;
    const int revChance = st.lockValue(lockKey(PG_SLC, SLC_REVRND), ln.smpParam(PG_SLC, SLC_REVRND));
    rev = revChance > 0 && rnd() * 127.0 < revChance ? 1 : 0;
    return k;
}

// A lane in SYNC: the loop runs on the song's clock, not the lane's rhythm. Each slice falls where it falls in the loop, so the loop keeps its feel at any tempo.
// The lane's own steps (as on screen) still count: a step's locks reach the slices it holds, a locked slice replaces them, its PROB can drop them.
static constexpr double kOnTheGrid = 0.05;   // of a step. A slice this early was cut at the zero crossing before its beat: it plays at its own time, but is that beat's

void Sequencer::syncLane(int li, const Lane& ln, int pat, uint64_t when, uint32_t nowMs, int laneStep) {
    const SliceMap* m = findSlices(pj_, ln.sample());
    if (!m || !m->frames) return;
    const int steps = kSliceBeats[clampi(ln.smpParam(PG_SLC, SLC_BEATS), 0, kSliceBeatChoices - 1)] * 4;
    const int pos = tp_.gstep % steps;
    const double stepFrames = 60.0 / pj_.bpm / 4.0 * sr_;
    // swing moves the odd steps late, which stretches an even step and squeezes an odd one: inside them the slices stretch and squeeze with it, and stay in order
    const double late = swing() / 100.0, inStep = stepFrames * (tp_.gstep % 2 == 1 ? 1.0 - late : 1.0 + late);
    Ev e;
    e.start = true;
    e.handle = -1;
    e.fx = 0;
    e.token = 0;
    e.note.part = ln.part;
    e.note.pattern = (uint8_t)pat;
    e.note.lane = (uint8_t)li;
    e.note.sampler = 1;
    auto fire = [&](const Step& st, int place, double at, double lenFrames) {
        uint8_t rev = 0;
        const int k = pickSlice(*m, ln, st, place, rev);
        if (m->s[k].flags & SLICE_MUTED) return;
        e.note.pitch = (int8_t)clampi(st.note, -48, 48);
        e.note.vel01 = (float)(st.vel * (ln.level / 127.0) / 127.0);
        int ns = 0;
        for (int i = 0; i < st.nLocks && i < kLocksPerStep; ++i)   // (the loop is the lane's: a step cannot play another sample's slices)
            if (!lockIsFx(st.locks[i].key) && st.locks[i].key != lockKey(PG_SMP, 0)) e.note.locks[ns++] = st.locks[i];
        e.note.nLocks = (uint8_t)ns;
        e.note.slice = (int8_t)k;
        e.note.sliceRev = rev;
        e.at = when + (uint64_t)(at + 0.5);
        e.gate = lenFrames < 1 ? 1u : (uint32_t)lenFrames;
        push(e);
        tp_.hitAtMs[li] = nowMs;
    };
    const Step& now = ln.st[laneStep];
    const Step& after = ln.st[(laneStep + 1) % ln.n];
    const uint8_t sliceKey = lockKey(PG_SLC, SLC_SLICE);
    if (now.lockValue(sliceKey, 0) > 0 && (now.prob >= 100 || rnd() * 100.0 <= now.prob)) fire(now, 0, 0.0, stepFrames);
    for (int k = 0; k < m->count; ++k) {
        const double place = (double)m->s[k].start * steps / m->frames;
        const int at = (int)place;
        if (at % steps != pos) continue;
        const Step& own = (int)(place + kOnTheGrid) > at ? after : now;
        if (own.lockValue(sliceKey, 0) > 0) continue;   // that step plays its own slice instead
        if (own.prob < 100 && rnd() * 100.0 > own.prob) continue;
        const double next = k + 1 < m->count ? (double)m->s[k + 1].start * steps / m->frames : (double)steps;
        fire(own, k, (place - at) * inStep, (next - place) * stepFrames);
    }
    Lock fxl[kLocksPerStep];
    int nf = 0;
    for (int k = 0; k < now.nLocks && k < kLocksPerStep; ++k) if (lockIsFx(now.locks[k].key)) fxl[nf++] = now.locks[k];
    pushFxLocks(li, ln.part, fxl, nf, when);
}

bool Sequencer::patternEnd() {
    tp_.pstep = 0;
    if (tp_.songPlaying) {
        const Song& so = pj_.songs[tp_.songIdx];
        int r;
        if (tp_.jumpTo >= 0) { r = tp_.jumpTo; tp_.jumpTo = -1; }
        else if (tp_.repLeft > 1) { tp_.repLeft = tp_.repLeft - 1; return true; }
        else r = tp_.playRow + 1;
        if (r >= so.nRows) {
            if (!so.loop || so.nRows == 0) return false;
            r = 0;
        }
        tp_.playRow = (uint8_t)r;
        tp_.repLeft = so.rows[r].rep;
        tp_.pat = so.rows[r].pat;
        tp_.patChanges = tp_.patChanges + 1;
        return true;
    }
    if (tp_.nextPat >= 0) {
        tp_.pat = (uint8_t)tp_.nextPat;
        tp_.nextPat = -1;
        tp_.patChanges = tp_.patChanges + 1;
    }
    return true;
}

}  // namespace app
