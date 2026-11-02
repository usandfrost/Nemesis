#include "audio/Instrument.h"

#include <string.h>

#include "app/MidiMap.h"

namespace audio {

using namespace app;

namespace {
constexpr float kCathodeHeadroom = 2.0f;    // the synth parts about 8 dB under the drums, as in the mockup; a full chord meets the limiter
constexpr float kMasterScale = 0.7f;        // the mockup's master gain law: master / 127 x 0.7
constexpr float kAuditionLevel = 0.6f;

inline float softLimit(float x) {   // the mockup ran its master through a compressor; a gentle clip keeps peaks inside +-1
    const float a = x < 0 ? -x : x;
    if (a <= 0.8f) return x;
    const float y = 0.8f + (a - 0.8f) / (1.0f + (a - 0.8f) * 5.0f);
    return x < 0 ? -(y > 1 ? 1 : y) : (y > 1 ? 1 : y);
}
}  // namespace

void Instrument::init(double sampleRate, Project& pj, Transport& tp, const SampleLib& lib) {
    sr_ = sampleRate;
    pj_ = &pj;
    tp_ = &tp;
    lib_ = &lib;
    tables_ = new SharedTables;
    tables_->build();
    for (int i = 0; i < kParts; ++i) {
        type_[i] = 0xFF;
        cap_[i] = 5;
        seenAmp_[i] = 0xFF;   // nothing seen yet: the first sync works everything out
#if DBOY_OLD_ENGINE
        cathode_[i].init(sampleRate, *tables_);
#endif
#if DBOY_LEAN
        leanPart_[i].init((float)sampleRate, *tables_);
#endif
        sampler_[i].init(sampleRate);
        memset(&base_[i], 0x80, sizeof(base_[i]));   // nothing applied: the first sync sets everything
        base_[i].f1 = base_[i].f2 = 0xFF;
    }
    audition_.init(sampleRate);
    fx_.init((float)sampleRate);
    seq_ = new Sequencer(pj, tp, *this);
    seq_->setSampleRate(sampleRate);
    syncParts();   // the parts' instruments, voices and patches as the project has them, before any request arrives
}

// ---------------------------------------------------------------------------------------------------------------------------------
// main loop -> audio interrupt

bool Instrument::push(const Req& r) {
    const uint32_t h = qHead_;
    if (h - qTail_ >= 32) return false;   // full: a preview more or less does not matter
    q_[h & 31] = r;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    qHead_ = h + 1;
    return true;
}
void Instrument::auditionSample(int sample) { push({ReqAudition, 0, (int16_t)sample}); }
void Instrument::auditionSlice(int sample, int slice) { push({ReqAuditionSlice, (uint8_t)slice, (int16_t)sample}); }
void Instrument::stopSamplers() {
    stopsAsked_ = stopsAsked_ + 1;
    if (!push({ReqStopSamplers, 0, 0})) stopsAsked_ = stopsAsked_ - 1;
}
void Instrument::previewPreset(int part, int preset) { push({ReqPreview, (uint8_t)part, (int16_t)preset}); }
void Instrument::stopAllSound() {
    stopsAsked_ = stopsAsked_ + 1;
    if (!push({ReqStop, 0, 0})) stopsAsked_ = stopsAsked_ - 1;
}

int Instrument::allocNote(int part, int midi, bool live, bool preview) {
    for (int k = 0; k < kNotes; ++k) {
        Note& n = notes_[part][k];
        if (n.used) continue;
        n.used = true;
        n.live = live;
        n.preview = preview;
        n.gen = (uint16_t)((n.gen + 1) & 0x3FF);
        if (n.gen == 0) n.gen = 1;
        n.midi = (int8_t)midi;
        n.voice = -1;
        n.order = noteOrder_++;
        return k;
    }
    return -1;
}

bool Instrument::queue(const Ev& e) {
    if (nev_ >= 64) return false;
    ev_[nev_++] = e;
    return true;
}

void Instrument::endPreview() {
    for (PreviewNote& p : preview_) {
        if (p.state == 1 && previewPart_ >= 0) endCathode(previewPart_, p.slot, p.gen);
        p.state = 2;
        p.midi = -1;
    }
    previewPart_ = -1;
    previewFxUntil_ = 0;
}

void Instrument::takeRequests() {
    while (qTail_ != qHead_) {
        const Req r = q_[qTail_ & 31];
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        qTail_ = qTail_ + 1;
        switch (r.type) {
#if DBOY_LEAN
            case ReqLean: setLean(r.a != 0); break;
#endif
            case ReqAudition:
            case ReqAuditionSlice: {
                // the raw sample, straight to the master (the mockup's auditionSample); one that is not loaded plays nothing
                if (r.a < 0 || r.a >= lib_->count) break;
                const SampleItem& it = lib_->items[r.a];
                if (!it.data || it.length < 2 || !it.rate) break;
                SampleHit h;
                h.smp = &it;
                h.data = it.data;
                h.length = it.length;
                h.start = 0;
                h.end = it.length - 1;
                h.step = (double)it.rate / sr_;
                h.attack = 0.001f;
                h.decayTau = 1.0f;
                h.sustain = 1.0f;
                h.releaseTau = 0.01f;
                h.level = kAuditionLevel;
                h.cutoff = h.peak = 18000.0f;
                if (r.type == ReqAuditionSlice) {
                    const SliceMap* sm = findSlices(*pj_, r.a);
                    if (!sm || r.part >= sm->count || sm->frames != it.length) break;
                    h.start = sm->s[r.part].start;
                    h.end = r.part + 1 < sm->count ? sm->s[r.part + 1].start : it.length - 1;
                    h.reverse = (sm->s[r.part].flags & SLICE_REVERSE) != 0;
                    h.step *= pow(2.0, sm->s[r.part].pitch / 12.0);
                    h.level *= sm->s[r.part].level / 127.0f;
                }
                audition_.stopAll();
                audition_.start(h, 2, 0);
                break;
            }
            case ReqPreview: {
                // three notes in the project key with the preset's patch on a copy of the part's pages; the part is not changed
                endPreview();   // a preview still sounding is let go first
                const int p = r.part;
                if (p >= kParts || pj_->parts[p].type != PT_CATHODE || !presetReady(r.a)) break;   // its file was read by presetLoad (main loop) before the request
                Part copy = pj_->parts[p];
                applyPresetIndex(copy.pages, r.a);
                previewFx_ = pj_->fx;                       // the preset's effects too: what loading it will sound like
                applyPresetFx(previewFx_, p, r.a);
                // ... unless the CPU is already busy: the degrade ladder is built from the project, so a preview's effects cannot be shed (a dry preview)
                if (loadFn_ && loadFn_() > 0.65f) previewFx_ = pj_->fx;
                previewFxUntil_ = 0;
                makePatch(copy, nullptr, 0, actions_[p], previewPatch_);
                previewPart_ = p;
                const int root = 48 + pj_->key, third = scaleHas(pj_->scale, 3) && !scaleHas(pj_->scale, 4) ? 3 : 4;
                const int notes[3] = {root, root + third, root + 7};
                for (int k = 0; k < 3; ++k) {
                    preview_[k].midi = (int8_t)scaleNote(notes[k], pj_->key, pj_->scale);
                    preview_[k].at = clock_ + (uint64_t)(k * 0.18 * sr_);
                    preview_[k].off = preview_[k].at + (uint64_t)(0.16 * sr_);
                    preview_[k].state = 0;
                    preview_[k].slot = -1;
                }
                break;
            }
            case ReqStop:
                sendAllOff();
                for (MidiHeld& m : held_) m.used = false;
                sustain_ = 0;
                for (int p = 0; p < kParts; ++p) {   // a wheel or bend held through a load does not detune the next project
                    ctlBend(p, 0.0f);
                    ctlWheel(p, 0.0f);
                    ctlPressure(p, 0.0f);
                }
                endPreview();
                for (int p = 0; p < kParts; ++p) {
                    for (int k = 0; k < kNotes; ++k) endCathode(p, k);
                    sampler_[p].stopAll();
                }
                audition_.stopAll();
                stopsTaken_ = stopsTaken_ + 1;
                break;
            case ReqStopSamplers:
                for (int p = 0; p < kParts; ++p) sampler_[p].stopAll();
                audition_.stopAll();
                stopsTaken_ = stopsTaken_ + 1;
                break;
            default: break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the parts follow the project

void Instrument::ctlBend(int p, float v) {
#if DBOY_OLD_ENGINE
    cathode_[p].engine().setPitchBend(v);
#endif
#if DBOY_LEAN
    leanPart_[p].setBend(v);
#endif
}
void Instrument::ctlWheel(int p, float v) {
#if DBOY_OLD_ENGINE
    cathode_[p].engine().setModWheel(v);
#endif
#if DBOY_LEAN
    leanPart_[p].setWheel(v);
#endif
    sampler_[p].setWheel(v);
}
void Instrument::ctlPressure(int p, float v) {
#if DBOY_OLD_ENGINE
    cathode_[p].engine().setChannelPressure(v);
#endif
#if DBOY_LEAN
    leanPart_[p].setPressure(v);
#endif
    sampler_[p].setPressure(v);
}

#if DBOY_LEAN
void Instrument::setLean(bool on) {
    if (on == lean_) return;
#if !DBOY_OLD_ENGINE
    return;   // nothing else to play through
#endif
    for (int p = 0; p < app::kParts; ++p) {   // the engine that is left goes quiet: its tails would otherwise freeze and come back with the old sound
        leanPart_[p].panic();
#if DBOY_OLD_ENGINE
        cathode_[p].engine().reset();
#endif
        lean::makeParams(base_[p], *tables_, leanBase_[p]);
        for (int k = 0; k < kNotes; ++k) notes_[p][k].used = false;   // the voices they name are gone (a late note-off would otherwise land on the other engine's voice)
    }
    for (PreviewNote& pn : preview_) pn.state = 2;
    previewPart_ = -1;
    for (MidiHeld& m : held_) if (m.cathode) m.used = false;
    lean_ = on;
}
#endif

void Instrument::applyVoicePatch(int p, int v, float vel01) {
    const VoiceState& vs = voices_[p][v];
#if DBOY_LEAN
    if (lean_) {   // the lean voice takes the same patches; a note's own sound is set when it starts (startCathode), the parts' knobs reach what sounds here
        const Patch* pp = &base_[p];
        if (vs.kind == VpPreview) pp = &previewPatch_;
        else if (vs.kind == VpLocks) { makePatch(pj_->parts[p], vs.locks, vs.nLocks, actions_[p], scratch_); pp = &scratch_; }
        if (!leanPart_[p].voiceUsed(v)) return;
        if (pp == &base_[p]) { leanPart_[p].setParams(v, leanBase_[p]); return; }
        lean::Params prm;
        lean::makeParams(*pp, *tables_, prm);
        leanPart_[p].setParams(v, prm);
        return;
    }
#endif
#if DBOY_OLD_ENGINE
    if (vs.kind == VpPreview) { cathode_[p].applyVoice(v, previewPatch_, vel01); return; }
    if (vs.kind == VpLocks) {
        // the note's locks over the part's pages as they are now, with the degrade bypasses as they are now
        makePatch(pj_->parts[p], vs.locks, vs.nLocks, actions_[p], scratch_);
        cathode_[p].applyVoice(v, scratch_, vel01);
        return;
    }
    cathode_[p].applyVoice(v, base_[p], vel01);
#endif
}

void Instrument::syncParts() {
    // the degrade ladder is rebuilt when what it is made of changes; the governor keeps its level (held to the new ladder), so a
    // knob turned under load does not hand the CPU back all at once
    bool changed[kParts], anyChanged = false;
    for (int i = 0; i < kParts; ++i) {
        const uint8_t amp = (uint8_t)pj_->fx.amp[i][0];
        changed[i] = amp != seenAmp_[i] || memcmp(&pj_->parts[i], &seenPart_[i], sizeof(Part)) != 0;
        if (!changed[i]) continue;
        seenPart_[i] = pj_->parts[i];
        seenAmp_[i] = amp;
        desc_[i] = partDesc(*pj_, i);
        anyChanged = true;
    }
    const uint8_t fxMask = fxDesc(*pj_).fx;   // the shared effects in use
    bool ladderMoved = false;
    if (anyChanged || fxMask != seenFx_ || pj_->partsIdle != seenIdle_) {
        seenFx_ = fxMask;
        seenIdle_ = pj_->partsIdle;
        uint32_t h = 2166136261u;
        auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
        for (int i = 0; i < kParts; ++i) {
            const cost::PartDesc& d = desc_[i];
            mix(d.voices); mix(d.priority); mix(d.sampler);
            for (int s = 0; s < 2; ++s) { mix((uint32_t)d.voice.slot[s].audible); mix(d.voice.slot[s].unison); mix((uint32_t)d.voice.slot[s].kind); }
            mix((uint32_t)d.voice.f1); mix((uint32_t)d.voice.f2); mix(d.voice.f2On); mix((uint32_t)d.voice.route);
            mix(d.voice.drive > 0); mix(d.voice.ring > 0); mix(d.voice.sync > 0); mix(d.voice.width > 0); mix(d.voice.companding > 0);
            mix(d.voice.pd1); mix(d.voice.pd2); mix(d.voice.sub); mix(d.voice.noise); mix(d.voice.hpf); mix(d.voice.clip);
            mix(d.amp);
        }
        mix(fxMask);
        mix(pj_->partsIdle);
        if (h != ladderSig_) {
            ladderSig_ = h;
            buildLadder(*pj_, ladder_);
            if (level_ > ladder_.count) level_ = ladder_.count;
            ladderMoved = true;
        }
    }
    if (level_ != seenLevel_) { seenLevel_ = level_; ladderMoved = true; }
    if (ladderMoved) {   // what the ladder has taken from the effects (the shared ones are its last part; the amps belong to their parts)
        ActionMask am[kParts];
        for (int i = 0; i < kParts; ++i) am[i] = actionsAt(ladder_.steps, ladder_.count, level_, i);
        fx_.setDegrade(actionsAt(ladder_.steps, ladder_.count, level_, kParts), am);
    }
    const bool tablesMoved = tables_->bankGen != tablesSeen_;   // a bank came or went: each voice sets its waves again
    for (int i = 0; i < kParts; ++i) {
        const Part& part = pj_->parts[i];
        // a part that changed instrument lets go of what the old one was playing (a CATHODE engine renders its tails out)
        if (type_[i] != part.type) {
            if (type_[i] == PT_CATHODE) for (int k = 0; k < kNotes; ++k) endCathode(i, k);
            if (type_[i] == PT_SAMPLER) sampler_[i].stopAll();
            if (previewPart_ == i) endPreview();
            type_[i] = part.type;
        }
        const int capBefore = cap_[i];
        const bool redo = changed[i] || ladderMoved;
        if (redo) {
            actions_[i] = actionsAt(ladder_.steps, ladder_.count, level_, i);
            cap_[i] = cost::degraded(desc_[i], i, ladder_.steps, ladder_.count, level_).voices;
        }
        if (part.type == PT_CATHODE) {
            // the engine's voice pool is the part's VOICES: release tails then share the same voices (the CPU budget counts them), and
            // the engine steals with its fade when they run out. A change resets the engine (the MIXER's VOICES knob: a rare move).
            if (engineVoices_[i] != part.voices) {
                engineVoices_[i] = part.voices;
#if DBOY_OLD_ENGINE
                cathode_[i].engine().setVoiceCount(part.voices);
#endif
#if DBOY_LEAN
                leanPart_[i].panic();   // as the engine's reset above: every voice let go (the notes were cleared, nothing would release them)
                leanPart_[i].setVoices(part.voices > CathodePart::kVoices ? CathodePart::kVoices : part.voices);
#endif
                for (int k = 0; k < kNotes; ++k) notes_[i][k].used = false;   // the reset let every voice go
                if (previewPart_ == i) { for (PreviewNote& pn : preview_) pn.state = 2; previewPart_ = -1; }
            }
#if DBOY_OLD_ENGINE
            if (tablesMoved) cathode_[i].invalidateTables();
#endif
            if (redo || tablesMoved) makePatch(part, nullptr, 0, actions_[i], scratch_);
            if (tablesMoved || (redo && memcmp(&scratch_, &base_[i], sizeof(Patch)) != 0)) {
                base_[i] = scratch_;
#if DBOY_LEAN
                if (lean_) lean::makeParams(base_[i], *tables_, leanBase_[i]);
#endif
#if DBOY_OLD_ENGINE
                cathode_[i].setBendRange(base_[i]);
#endif
                for (int v = 0; v < CathodePart::kVoices; ++v) applyVoicePatch(i, v, -1.0f);   // each voice keeps its note's own locks
            }
            while (redo && heldNotes(i) > cap_[i]) {   // fewer voices: the oldest note goes (its release still sounds)
                int oldest = -1;
                for (int k = 0; k < kNotes; ++k)
                    if (notes_[i][k].used && notes_[i][k].voice >= 0 && (oldest < 0 || notes_[i][k].order < notes_[i][oldest].order)) oldest = k;
                if (oldest < 0) break;
                endCathode(i, oldest);
            }
            if (cap_[i] < capBefore) {   // a released voice keeps costing until its release has faded: judge the new level after that
                const float rel = (float)envMs(part.pages[PG_AMP][3]) / 1000.0f;
                governor_.holdFor((int)(rel * sr_ / 32.0f) + 1);
            }
        }
#if DBOY_OLD_ENGINE
        else if (tables_->bankGen != tablesSeen_) cathode_[i].unbindTables();   // a sampler part's CATHODE tails must not hold a bank that is let go
#endif
#if DBOY_LEAN
        if (tablesMoved) leanPart_[i].rebind(*tables_);   // (a part that became a sampler may still be ringing out on a bank)
#endif
        partGain_[i] = part.level / 127.0f * (part.pages[PG_GLOB][0] / 127.0f);
    }
    tablesSeen_ = tables_->bankGen;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// MIDI

int Instrument::sendOn(const NoteStart& n) {
    if (!midiOut_ || n.lane >= kLanes) return 0;
    const int ch = pj_->midi[n.lane].ch;
    if (!ch) return 0;
    for (int s = 0; s < kMidiSent; ++s) {
        if (sent_[s].used) continue;
        const int note = noteOut(*pj_, n.lane, n.pitch, n.sampler);
        int vel = (int)(n.vel01 * 127.0f + 0.5f);
        vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;
        // the same note again while it is still on (a drum retriggered, a chord on a shared channel's trigger note): end it first,
        // so the synth listening starts it again; only its last end goes out (sendOff)
        uint8_t m[6] = {(uint8_t)(0x80 | (ch - 1)), (uint8_t)note, 0, (uint8_t)(0x90 | (ch - 1)), (uint8_t)note, (uint8_t)vel};
        const bool again = soundingOut(ch, note, -1);
        if (!midiOut_(again ? m : m + 3, again ? 6 : 3)) return 0;   // the socket is behind: dropped (the retrigger's end with it)
        sent_[s] = MidiSent{true, false, (uint8_t)ch, (uint8_t)note};
        return s + 1;
    }
    return 0;
}

bool Instrument::soundingOut(int ch, int note, int except) const {
    for (int s = 0; s < kMidiSent; ++s)
        if (s != except && sent_[s].used && !sent_[s].flushed && sent_[s].ch == ch && sent_[s].note == note) return true;
    return false;
}

void Instrument::sendOff(int slot) {
    if (slot < 1 || slot > kMidiSent || !sent_[slot - 1].used) return;
    MidiSent& s = sent_[slot - 1];
    if (!s.flushed && midiOut_ && !soundingOut(s.ch, s.note, slot - 1)) sendNoteOff(s.ch, s.note);
    s.used = false;
}

void Instrument::sendAllOff() {
    // STOP: every note still out ends now; the slots stay taken until the sequencer's ends for them come (MidiSent)
    for (int s = 0; s < kMidiSent; ++s) {
        MidiSent& m = sent_[s];
        if (!m.used || m.flushed) continue;
        m.flushed = true;
        if (midiOut_ && !soundingOut(m.ch, m.note, s)) sendNoteOff(m.ch, m.note);
    }
}

void Instrument::midiNoteOn(int ch, int note, int vel) {
    // what plays it: a lane on that channel, or (a channel no lane uses) the synth part on screen
    MidiRoute r;
    int part, pitch;
    const Lane* lane = nullptr;
    if (routeIn(*pj_, tp_->pat, ch, note, r)) {
        lane = &pj_->patterns[tp_->pat].lanes[r.lane];
        part = lane->part;
        pitch = r.pitch;
    } else if (lanesOnChannel(*pj_, ch) == 0) {
        part = livePart_;
        pitch = note;
    } else {
        return;   // a note no lane on this channel is
    }
    if (part < 0 || part >= kParts) return;
    int h = -1;
    for (MidiHeld& m : held_) if (m.used && m.sustained && m.ch == ch && m.note == note) releaseHeld(m);   // (the same key again under the pedal: the note starts over)
    for (int k = 0; k < 32 && h < 0; ++k) if (!held_[k].used) h = k;
    for (int k = 0; k < 32 && h < 0; ++k) if (held_[k].sustained) { releaseHeld(held_[k]); h = k; }   // (a pedal must not stop the keys)
    if (h < 0) return;
    MidiHeld& m = held_[h];
    if (pj_->parts[part].type == PT_SAMPLER) {
        if (!lane) return;   // a sampler part is played through its lanes
        SampleHit hit;
        const SliceMap* sm = lane->smpParam(PG_SLC, SLC_MODE) != SLICE_OFF ? findSlices(*pj_, lane->sample()) : nullptr;
        // a sliced lane: the keys play the slices (the note the lane sits on is the first), not the sample at other pitches
        if (!makeHit(*lane, nullptr, 0, sm ? 0 : pitch, (float)vel, pj_->parts[part].level / 127.0f, *lib_, sr_, hit)) return;
        if (sm) {
            const int k = ((pitch % sm->count) + sm->count) % sm->count;
            if ((sm->s[k].flags & SLICE_MUTED) || !applySlice(hit, *lane, *sm, k, false, pj_->bpm, r.lane)) return;
        } else {
            applyLoopFit(hit, *pj_, (int)(hit.smp - lib_->items));
        }
        const int32_t sh = sampler_[part].start(hit, cap_[part], 0);
        if (sh < 0) return;
        m = MidiHeld{true, (uint8_t)ch, (uint8_t)note, (uint8_t)part, false, sh, 0};
        return;
    }
    const int k = allocNote(part, pitch, true, false);
    if (k < 0) return;
    Ev e{};
    e.on = 1;
    e.part = (uint8_t)part;
    e.note = (uint8_t)k;
    e.gen = notes_[part][k].gen;
    e.vel = vel / 127.0f;
    if (!queue(e)) { notes_[part][k].used = false; return; }
    m = MidiHeld{true, (uint8_t)ch, (uint8_t)note, (uint8_t)part, true, k, notes_[part][k].gen};
}

void Instrument::midiNoteOff(int ch, int note) {
    for (MidiHeld& m : held_) {
        if (!m.used || m.sustained || m.ch != ch || m.note != note) continue;
        if ((sustain_ >> (ch - 1)) & 1) { m.sustained = true; continue; }
        releaseHeld(m);
    }
}

void Instrument::releaseHeld(MidiHeld& m) {
    m.used = false;
    m.sustained = false;
    if (!m.cathode) { sampler_[m.part].release(m.handle, 0); return; }
    Ev e{};
    e.on = 0;
    e.part = m.part;
    e.note = (uint8_t)m.handle;
    e.gen = m.gen;
    if (!queue(e)) endCathode(m.part, m.handle, m.gen);
}

// ---- the clock: what the sequencer follows and what goes out. GLOBAL's CLOCK is a project's, kept the same on every part.
// A MIDI clock has 24 ticks to a quarter note, so a step is 6. The SYNC socket gives 2 pulses to a quarter note, as the stock firmware does by default, so a pulse is 2 steps:
// the second falls half a pulse later. Neither swings: the clock sets the time. The tempo on screen follows what comes in, for the gates and the delay.
// Inside, Nemesis makes the clock it sends. Following, it passes on what it follows: its own count would drift from the master.
static constexpr uint32_t kPulseFrames = 220;   // 5 ms

void Instrument::sendByte(uint8_t b) {
    if (midiOut_) midiOut_(&b, 1);
}

void Instrument::addExtStep() {
    if (tp_->playing) tp_->extSteps = (uint16_t)(tp_->extSteps + 1);
}

void Instrument::setFollowed(int bpm) {
    bpm = bpm < 40 ? 40 : bpm > 240 ? 240 : bpm;   // (past the ends of the tempo knob the steps still follow; gates and delays go by the end)
    if (bpm > pj_->bpm + 1 || bpm < pj_->bpm - 1 || bpm == 40 || bpm == 240) pj_->bpm = (uint16_t)bpm;   // (a flicker of one is not a change of tempo)
}

void Instrument::clockTick() {
    sendByte(0xF8);
    if (tick_ % 12 == 0) pulseLeft_ = kPulseFrames;
    // the tempo over a whole beat, leaving out a gap: a master that stopped and came back says nothing about its speed
    const uint32_t gap = sinceTick_;
    sinceTick_ = 0;
    if (beatTicks_ > 0 && gap > 3 * (beatFrames_ / beatTicks_ + (int)blockFrames_)) { beatFrames_ = 0; beatTicks_ = 0; }
    else if (tickSeen_) {
        beatFrames_ += gap;
        if (++beatTicks_ == 24) {
            setFollowed((int)(60.0 * sr_ / beatFrames_ + 0.5));
            beatFrames_ = 0;
            beatTicks_ = 0;
        }
    }
    tickSeen_ = true;
    if (!tp_->playing) return;   // (the master's place does not move while it is stopped)
    if (tick_ % 6 == 0) addExtStep();
    ++tick_;
}

void Instrument::clockReset() {
    tick_ = 0;
    tickSeen_ = false;
    beatFrames_ = 0;
    beatTicks_ = 0;
    tp_->extSteps = 0;
}

void Instrument::runClock() {
    const int16_t asked = pj_->parts[0].pages[PG_GLOB][4];
    const int source = rendering_ || !clockIn_ ? 0 : asked == 1 || asked == 2 ? asked : 0;   // (a page with nothing to follow plays by itself)
    if (source != lastSource_) {
        lastSource_ = source;
        clockReset();
        sincePulse_ = 0x10000000u;   // (longer than any pulse: the first one is not a bounce, and gives no period)
        pulsePeriod_ = 0;
        halfIn_ = 0;
        halfOwed_ = false;
    }
    tp_->clockExt = source != 0;
    sinceTick_ += (int)blockFrames_;
    if (source == 2 && syncIn_) {
        sincePulse_ += (int)blockFrames_;
        const bool high = syncIn_();
        if (high && !syncHigh_ && sincePulse_ > 256) {   // (a bounce is not a pulse)
            if (pulsePeriod_ == 0 || sincePulse_ <= 3 * pulsePeriod_) {   // (after a gap the last good period stands)
                if (sincePulse_ < 8 * sr_) pulsePeriod_ = sincePulse_;
                if (pulsePeriod_) setFollowed((int)(60.0 * sr_ / (2.0 * pulsePeriod_) + 0.5));
            }
            sincePulse_ = 0;
            pulseLeft_ = kPulseFrames;
            if (halfOwed_ || halfIn_ > 0) addExtStep();   // (the half that had no time to fall: this run stays on the grid)
            halfOwed_ = false;
            addExtStep();
            if (!tp_->playing) halfIn_ = 0;
            else if (pulsePeriod_) halfIn_ = (int32_t)(pulsePeriod_ / 2);
            else { halfIn_ = 0; halfOwed_ = true; }
        }
        syncHigh_ = high;
        if (halfIn_ > 0) {
            halfIn_ -= (int)blockFrames_;
            if (halfIn_ <= 0) addExtStep();
        }
    }
    if (rendering_) {
        if (wasPlaying_) sendByte(0xFC);
        wasPlaying_ = false;
        pulseLeft_ = 0;
        if (syncOut_) syncOut_(false, false);
        return;
    }
    const bool playing = tp_->playing != 0;
    if (source == 0) {   // what goes out: clock, start and stop at the MIDI socket, and pulses at the SYNC socket
        if (playing != wasPlaying_) {
            sendByte(playing ? 0xFA : 0xFC);
            outTicks_ = 0;
            outIn_ = 0.0;
            lastStep_ = -1;
        }
        if (playing) {
            // the steps are the truth: on each even step (swing leaves those alone) the count comes back to them, so a big change of tempo cannot leave the two apart
            const int32_t g = tp_->gstep;
            if (g >= 0 && g != lastStep_ && g % 2 == 0) {
                const int64_t want = (int64_t)g * 6 + 1, have = outTicks_;
                if (want > have + 1 || want < have - 1) {
                    if (want > have) for (int64_t i = have; i < want && i < have + 12; ++i) sendByte(0xF8);
                    outTicks_ = (uint32_t)want;
                    outIn_ = sr_ * 60.0 / pj_->bpm / 24.0;
                }
            }
            lastStep_ = g;
            outIn_ -= (int)blockFrames_;
            while (outIn_ <= 0.0) {
                sendByte(0xF8);
                if (outTicks_ % 12 == 0) pulseLeft_ = kPulseFrames;
                ++outTicks_;
                outIn_ += sr_ * 60.0 / pj_->bpm / 24.0;
            }
        }
    }
    wasPlaying_ = playing;
    if (syncOut_) syncOut_(pulseLeft_ > 0, playing);
    if (pulseLeft_ > 0) pulseLeft_ -= (int)blockFrames_;
}

void Instrument::takeMidi() {
    if (!midiIn_) return;
    midi::Event e;
    while (midiIn_->pop(e)) {
        if (rendering_) continue;
        const int ch = e.channel + 1;
        // the synth parts the message is for: those of the lanes on its channel, or the part on screen
        auto forParts = [&](auto fn) {
            bool any = false;
            for (int i = 0; i < kLanes; ++i)
                if (pj_->midi[i].ch == ch) { fn(pj_->patterns[tp_->pat].lanes[i].part); any = true; }
            if (!any) fn(livePart_);
        };
        switch (e.type) {
            case midi::Type::NoteOn:
                if (e.data2 > 0) midiNoteOn(ch, e.data1, e.data2);
                else midiNoteOff(ch, e.data1);
                break;
            case midi::Type::NoteOff: midiNoteOff(ch, e.data1); break;
            case midi::Type::PitchBend: {
                const float bend = ((int)(e.data1 | (e.data2 << 7)) - 8192) / 8192.0f;
                forParts([&](int p) { if (p >= 0 && p < kParts) ctlBend(p, bend); });
                break;
            }
            case midi::Type::ChannelPressure:
                forParts([&](int p) { if (p >= 0 && p < kParts) ctlPressure(p, e.data1 / 127.0f); });
                break;
            case midi::Type::ControlChange:
                if (e.data1 == 1) forParts([&](int p) { if (p >= 0 && p < kParts) ctlWheel(p, e.data2 / 127.0f); });
                else if (e.data1 == 64) {   // the sustain pedal: keys let go while it is down ring on until it comes up
                    const uint16_t bit = (uint16_t)(1u << e.channel);
                    if (e.data2 >= 64) sustain_ |= bit;
                    else {
                        sustain_ &= (uint16_t)~bit;
                        for (MidiHeld& m : held_) if (m.used && m.sustained && m.ch == ch) releaseHeld(m);
                    }
                }
                else if (e.data1 == 121) {   // reset all controllers
                    forParts([&](int p) {
                        if (p < 0 || p >= kParts) return;
                        ctlBend(p, 0.0f);
                        ctlWheel(p, 0.0f);
                        ctlPressure(p, 0.0f);
                    });
                    sustain_ &= (uint16_t)~(1u << e.channel);
                    for (MidiHeld& m : held_) if (m.used && m.sustained && m.ch == ch) releaseHeld(m);
                }
                else if (e.data1 == 120 || e.data1 == 123) {   // all sound / all notes off: the notes this channel started
                    sustain_ &= (uint16_t)~(1u << e.channel);
                    for (MidiHeld& m : held_) if (m.used && m.ch == ch) releaseHeld(m);
                }
                break;
            case midi::Type::Clock:
                if (following()) clockTick();
                break;
            case midi::Type::Start:
                if (!following()) break;
                sendByte(0xFA);
                clockReset();
                tp_->pstep = 0;
                tp_->gstep = -1;
                tp_->nextPat = -1;
                if (tp_->startRow >= 0 && tp_->startRow < pj_->songs[tp_->songIdx].nRows) {   // on the SONG screen: the song, as PLAY does there
                    const SongRow& row = pj_->songs[tp_->songIdx].rows[tp_->startRow];
                    tp_->playRow = (uint8_t)tp_->startRow;
                    tp_->repLeft = row.rep;
                    tp_->pat = row.pat;
                    tp_->songPlaying = 1;
                } else if (tp_->songPlaying) {
                    const SongRow& row = pj_->songs[tp_->songIdx].rows[0];
                    tp_->playRow = 0;
                    tp_->repLeft = row.rep;
                    tp_->pat = row.pat;
                }
                tp_->playGen = tp_->playGen + 1;
                tp_->playing = 1;
                break;
            case midi::Type::Continue:
                if (!following()) break;
                sendByte(0xFB);
                tp_->playing = 1;
                break;
            case midi::Type::Stop:
                if (!following()) break;
                sendByte(0xFC);
                tp_->playing = 0;
                tp_->songPlaying = 0;
                tp_->nextPat = -1;
                tp_->extSteps = 0;
                break;
            default: break;
        }
    }
}

void Instrument::clearFx() {
    fxHoldReq_ = 1;
    // the audio interrupt acknowledges at its next block (< 1 ms); with nothing rendering (a host test) the wait is just a short spin
    for (uint32_t spin = 0; !fxHoldAck_ && spin < 4000000u; ++spin) __atomic_signal_fence(__ATOMIC_SEQ_CST);
    fx_.clear();
    fxHoldReq_ = 0;
}

void Instrument::beginBlock() {
    fxHoldAck_ = fxHoldReq_;
    takeRequests();
    takeMidi();
    runClock();
    // the governor judges the block that just finished: quick to give things up, slow to take them back
    if (autoDegrade_ && loadFn_) level_ = governor_.update(loadFn_(), ladder_.count);
    else if (!autoDegrade_) { governor_.reset(); level_ = 0; }
    syncParts();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// notes

int Instrument::heldNotes(int part) const {
    int n = 0;
    for (int k = 0; k < kNotes; ++k) n += notes_[part][k].used;
    return n;
}

int32_t Instrument::noteOn(const NoteStart& ns, int offset) {
    if (onlyLane_ >= 0 && ns.lane != onlyLane_) return -1;
    const int p = ns.part;
    if (p >= kParts) return -1;
    // the lane's MIDI channel hears it too: the sent note's slot rides in the handle, so the end goes out where the start did
    const int sent = rendering_ ? 0 : sendOn(ns);
    const int32_t sentBits = (int32_t)sent << kSentShift;
    // the part changed type between the step being read and now (a strum, an arp): its pitch means something else, so it does not sound
    if ((pj_->parts[p].type == PT_SAMPLER) != (ns.sampler != 0)) return sent ? (sentBits | kNoInternal) : -1;
    if (pj_->parts[p].type == PT_SAMPLER) {
        const Lane& ln = pj_->patterns[ns.pattern].lanes[ns.lane];
        SampleHit hit;
        int32_t h = -1;
        bool played = makeHit(ln, ns.locks, ns.nLocks, ns.pitch, ns.vel01 * 127.0f, pj_->parts[p].level / 127.0f, *lib_, sr_, hit);
        if (played && ns.slice >= 0) {   // (the table may have changed since the note was queued: then nothing, not the whole sample)
            const SliceMap* sm = findSlices(*pj_, (int)(hit.smp - lib_->items));
            played = sm && applySlice(hit, ln, *sm, ns.slice, ns.sliceRev != 0, pj_->bpm, ns.lane);
        } else if (played) {
            applyLoopFit(hit, *pj_, (int)(hit.smp - lib_->items));
        }
        if (played)
            h = sampler_[p].start(hit, cap_[p], offset);
        if (h < 0) return sent ? (sentBits | kNoInternal) : -1;
        return (h & ((1 << kSentShift) - 1)) | sentBits;
    }
    const int k = nev_ < 63 ? allocNote(p, ns.pitch, false, false) : -1;   // (room is kept for an end)
    if (k < 0) return sent ? (sentBits | kNoInternal) : -1;
    Ev e{};
    e.on = 1;
    e.part = (uint8_t)p;
    e.note = (uint8_t)k;
    e.gen = notes_[p][k].gen;
    e.offset = (int16_t)offset;
    e.vel = ns.vel01;
    e.nLocks = ns.nLocks;
    for (int i = 0; i < ns.nLocks; ++i) e.locks[i] = ns.locks[i];
    queue(e);
    return kCathodeTag | sentBits | (int32_t)(notes_[p][k].gen << 5) | k;
}

void Instrument::noteOff(int part, int32_t handle, int offset) {
    if (part >= kParts || handle < 0) return;
    sendOff((handle >> kSentShift) & 0x3F);
    if (handle & kNoInternal) return;
    handle &= ~(0x3F << kSentShift);
    if (!(handle & kCathodeTag)) { sampler_[part].release(handle, offset); return; }   // by the handle, not the part's type now
    const int k = handle & 31, gen = (handle >> 5) & 0x3FF;
    if (k >= kNotes || !notes_[part][k].used || notes_[part][k].gen != gen) return;
    Ev e{};
    e.on = 0;
    e.part = (uint8_t)part;
    e.note = (uint8_t)k;
    e.gen = (uint16_t)gen;
    e.offset = (int16_t)offset;
    if (!queue(e)) endCathode(part, k, gen);   // no room to wait for its sample: end it now rather than never
}

void Instrument::startCathode(const Ev& e) {
    const int p = e.part;
    Note& n = notes_[p][e.note];
    if (!n.used || n.gen != e.gen) return;
    // over the part's voices (or what the governor left it): the oldest note goes first
    for (;;) {
        int sounding = 0, oldest = -1;
        for (int k = 0; k < kNotes; ++k) {
            const Note& o = notes_[p][k];
            if (!o.used || o.voice < 0) continue;
            ++sounding;
            if (oldest < 0 || o.order < notes_[p][oldest].order) oldest = k;
        }
        if (sounding < cap_[p] || oldest < 0) break;
        endCathode(p, oldest);
    }
    float vel = e.vel;
    if (n.preview) vel *= pj_->parts[p].pages[PG_GLOB][5] / 100.0f;   // GLOBAL PREVIEW: the preview's level (100 = as played)
#if DBOY_LEAN
    if (lean_) {   // the lean voice: the note carries its own sound (p-locks, a preview's preset, or the part's)
        const Patch* pp = &base_[p];
        if (n.preview) pp = &previewPatch_;
        else if (e.nLocks) { makePatch(pj_->parts[p], e.locks, e.nLocks, actions_[p], scratch_); pp = &scratch_; }
        lean::Params prm;
        if (pp == &base_[p]) prm = leanBase_[p];
        else lean::makeParams(*pp, *tables_, prm);
        leanPart_[p].setVoices(cap_[p] < 1 ? 1 : (cap_[p] > CathodePart::kVoices ? CathodePart::kVoices : cap_[p]));
        const int v = leanPart_[p].noteOn(n.midi, vel > 1 ? 1 : vel, prm);
        if (v < 0) { n.used = false; return; }
        n.voice = (int8_t)v;
        n.group = leanPart_[p].lastOrder();
        VoiceState& vs = voices_[p][v];
        vs.kind = n.preview ? VpPreview : e.nLocks ? VpLocks : VpBase;
        vs.nLocks = e.nLocks;
        for (int k = 0; k < e.nLocks; ++k) vs.locks[k] = e.locks[k];
        return;
    }
#endif
#if DBOY_OLD_ENGINE
    nemesis::dsp::CathodeVoiceEngine& eng = cathode_[p].engine();
    const uint32_t cOn = cyclesFn_ ? cyclesFn_() : 0;
    eng.noteOn(n.midi, vel > 1 ? 1 : vel);
    const uint32_t cOn1 = cyclesFn_ ? cyclesFn_() : 0;
    const int v = eng.lastTriggeredVoice();
    if (v < 0) { n.used = false; return; }
    n.voice = (int8_t)v;
    n.group = eng.voiceGroupAt(v);
    // the note's own patch on every voice of its unison stack: its p-locks, a preview's preset, or the part's
    for (int i = 0; i < CathodePart::kVoices; ++i) {
        if (!eng.voiceHeld(i) || eng.voiceGroupAt(i) != n.group) continue;
        VoiceState& vs = voices_[p][i];
        vs.kind = n.preview ? VpPreview : e.nLocks ? VpLocks : VpBase;
        vs.nLocks = e.nLocks;
        for (int k = 0; k < e.nLocks; ++k) vs.locks[k] = e.locks[k];
        applyVoicePatch(p, i, vel);
    }
    if (cyclesFn_) {
        const uint32_t on = cOn1 - cOn, patch = cyclesFn_() - cOn1;
        ++sec_.starts;
        sec_.patchSum += patch;
        sec_.noteOnSum += on;
        if (on > sec_.noteOnMax) sec_.noteOnMax = on;
        if (patch > sec_.patchMax) sec_.patchMax = patch;
    }
#endif
}

void Instrument::endCathode(int part, int k, int gen) {
    if (k < 0 || k >= kNotes) return;
    Note& n = notes_[part][k];
    if (!n.used || (gen >= 0 && n.gen != gen)) return;
    n.used = false;
#if DBOY_LEAN
    if (lean_) { if (n.voice >= 0) leanPart_[part].release(n.voice, (uint32_t)n.group); n.voice = -1; return; }
#endif
#if DBOY_OLD_ENGINE
    if (n.voice >= 0) cathode_[part].engine().noteOffVoice(n.voice, n.group, n.midi);
#endif
    n.voice = -1;
}

void Instrument::renderCathode(int from, int to) {
    if (to <= from) return;
    for (int p = 0; p < kParts; ++p) {
#if DBOY_LEAN
        if (lean_) {
            if (leanPart_[p].activeVoices() == 0) { leanPart_[p].idle(to - from); continue; }
            const float gl = (useFx_ ? 1.0f : partGain_[p]) * kCathodeHeadroom;
            sounding_ |= 1u << p;
            const uint32_t c0 = cyclesFn_ ? cyclesFn_() : 0;
            for (int i = from; i < to; i += lean::kCtl) leanPart_[p].render(outL_[p] + i, outR_[p] + i, to - i < lean::kCtl ? to - i : lean::kCtl, gl);
            if (cyclesFn_) sec_.part[p] += (uint32_t)(cyclesFn_() - c0);
            continue;
        }
#endif
#if DBOY_OLD_ENGINE
        nemesis::dsp::CathodeVoiceEngine& eng = cathode_[p].engine();
        // an idle engine costs nothing; one whose part became a sampler renders its tails out. A note that landed on a voice whose release was
        // just ending waits behind the engine's steal fade: the engine must run until that fade has handed the note over, or the note never starts
        if (eng.activeVoiceCount() == 0) {
            bool fading = false;
            for (int v = 0; v < CathodePart::kVoices && !fading; ++v) fading = eng.stealFading(v);
            if (!fading) continue;
        }
        // with effects the part's fader comes after its amp (FxSection::process): here only the engine's headroom
        sounding_ |= 1u << p;
        const float g = (useFx_ ? 1.0f : partGain_[p]) * kCathodeHeadroom;
        float* l = outL_[p];
        float* r = outR_[p];
        const uint32_t c0 = cyclesFn_ ? cyclesFn_() : 0;
        for (int i = from; i < to; ++i) {
            const auto s = eng.processSample();
            l[i] += s.left * g;
            r[i] += s.right * g;
        }
        if (cyclesFn_) sec_.part[p] += (uint32_t)(cyclesFn_() - c0);
#endif
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------

void Instrument::fxSet(int lane, int part, const app::Lock* locks, int n, uint32_t token, int) {
    if (onlyLane_ >= 0 && lane != onlyLane_) return;
    if (lane < 0 || lane >= app::kLanes || part < 0 || part >= kParts) return;
    FxOverride& o = fxOv_[lane];
    if (!o.on) ++fxOvOn_;
    o.on = true;
    o.part = (uint8_t)part;
    o.n = (uint8_t)(n < app::kLocksPerStep ? n : app::kLocksPerStep);
    o.token = token;
    o.order = ++fxOvOrder_;
    for (int i = 0; i < o.n; ++i) o.locks[i] = locks[i];
}

void Instrument::fxRelease(int lane, uint32_t token, int) {
    if (lane < 0 || lane >= app::kLanes) return;
    FxOverride& o = fxOv_[lane];
    if (o.on && o.token == token) { o.on = false; --fxOvOn_; }   // (a later step's set has another token: its start came first)
}

void Instrument::fxReleaseAll() {
    for (FxOverride& o : fxOv_) o.on = false;
    fxOvOn_ = 0;
}

void Instrument::render(float* left, float* right, int frames) {
    blockFrames_ = (uint32_t)frames;
    if (frames > kFxFrames) {   // (the board renders 32; a longer block goes in pieces so the effects see all of it)
        for (int off = 0; off < frames; off += kFxFrames) render(left + off, right + off, frames - off < kFxFrames ? frames - off : kFxFrames);
        return;
    }
    for (int i = 0; i < frames; ++i) left[i] = right[i] = 0.0f;
    // effects in use (an amp, a send, the compressor, a tail still ringing): every part renders into its own buffers, which the section
    // mixes; otherwise the parts add straight into the output, as they always did
    bool useFx = false;
    if (!fxHoldReq_) {
        if (previewPart_ >= 0 || clock_ < previewFxUntil_) {
            fx_.apply(previewFx_, pj_->bpm);
        } else if (fxOvOn_ > 0) {
            fxEff_ = pj_->fx;
            int idx[app::kLanes], n = 0;
            for (int l = 0; l < app::kLanes; ++l) if (fxOv_[l].on) idx[n++] = l;
            for (int a = 1; a < n; ++a) {   // oldest first, so the latest set wins
                const int v = idx[a];
                int b = a;
                while (b > 0 && fxOv_[idx[b - 1]].order > fxOv_[v].order) { idx[b] = idx[b - 1]; --b; }
                idx[b] = v;
            }
            for (int a = 0; a < n; ++a) {
                const FxOverride& o = fxOv_[idx[a]];
                for (int i = 0; i < o.n; ++i) {
                    const int pg = app::lockPage(o.locks[i].key), pr = app::lockParam(o.locks[i].key);
                    if (!app::fxLockableParam(pg, pr)) continue;
                    const int hi = app::fxMax(app::fxDef(pg == app::PG_FXSEND ? app::FX_SENDS : app::FX_AMP, pr));
                    const int v = o.locks[i].value < 0 ? 0 : o.locks[i].value > hi ? hi : o.locks[i].value;
                    if (pg == app::PG_FXSEND) fxEff_.send[o.part][pr] = (int16_t)v;
                    else fxEff_.amp[o.part][pr] = (int16_t)v;
                }
            }
            fx_.apply(fxEff_, pj_->bpm);
        } else {
            fx_.apply(pj_->fx, pj_->bpm);
        }
        useFx = fx_.active();
    }
    useFx_ = useFx;
    sounding_ = 0;
    for (int p = 0; p < kParts; ++p) {
        if (useFx) {
            outL_[p] = partL_[p];
            outR_[p] = partR_[p];
            for (int i = 0; i < frames; ++i) partL_[p][i] = partR_[p][i] = 0.0f;
        } else {
            outL_[p] = left;
            outR_[p] = right;
        }
    }
    const uint32_t ms = msFn_ ? msFn_() : 0;
    // the sequencer: CATHODE notes come back as events with their offsets; sampler hits are scheduled inside the sampler
    const uint32_t cSeq = cyclesFn_ ? cyclesFn_() : 0;
    seq_->process(frames, ms);
    if (cyclesFn_) sec_.seq += (uint32_t)(cyclesFn_() - cSeq);
    // a preset preview's three notes
    if (previewPart_ >= 0) {
        const uint64_t end = clock_ + (uint64_t)frames;
        bool live = false;
        for (PreviewNote& pn : preview_) {
            if (pn.state == 0 && pn.at < end) {
                const int k = allocNote(previewPart_, pn.midi, false, true);
                Ev e{};
                e.on = 1;
                e.part = (uint8_t)previewPart_;
                e.offset = (int16_t)(pn.at > clock_ ? pn.at - clock_ : 0);
                e.vel = 100.0f / 127.0f;
                if (k >= 0) {
                    e.note = (uint8_t)k;
                    e.gen = notes_[previewPart_][k].gen;
                    if (queue(e)) { pn.slot = (int8_t)k; pn.gen = e.gen; pn.state = 1; }
                    else { notes_[previewPart_][k].used = false; pn.state = 2; }
                } else {
                    pn.state = 2;   // no slot: this note is skipped
                }
            }
            if (pn.state == 1 && pn.off < end) {
                Ev e{};
                e.on = 0;
                e.part = (uint8_t)previewPart_;
                e.note = (uint8_t)pn.slot;
                e.gen = pn.gen;
                e.offset = (int16_t)(pn.off > clock_ ? pn.off - clock_ : 0);
                if (!queue(e)) endCathode(previewPart_, pn.slot, pn.gen);
                pn.state = 2;
            }
            live = live || pn.state != 2;
        }
        if (!live) { previewPart_ = -1; previewFxUntil_ = clock_ + (uint64_t)(3.0 * sr_); }   // (the notes are still releasing: their effects go on a while)
    }
    // events in time order (they arrive in order per source; the previews may interleave)
    for (int i = 1; i < nev_; ++i)
        for (int j = i; j > 0 && ev_[j - 1].offset > ev_[j].offset; --j) { const Ev t = ev_[j]; ev_[j] = ev_[j - 1]; ev_[j - 1] = t; }
    int pos = 0;
    for (int i = 0; i < nev_; ++i) {
        const Ev& e = ev_[i];
        const int at = e.offset < 0 ? 0 : e.offset > frames ? frames : e.offset;
        renderCathode(pos, at);
        pos = at;
        if (e.on) startCathode(e);
        else endCathode(e.part, e.note, e.gen);
    }
    renderCathode(pos, frames);
    nev_ = 0;
    // the samplers, each scaled by its part's gain
    const uint32_t cSmp = cyclesFn_ ? cyclesFn_() : 0;
    float tl[64], tr[64];
    for (int p = 0; p < kParts; ++p) {
        if (type_[p] != PT_SAMPLER && !sampler_[p].active()) continue;
        if (sampler_[p].active()) sounding_ |= 1u << p;
        for (int off = 0; off < frames; off += 64) {
            const int n = frames - off < 64 ? frames - off : 64;
            for (int i = 0; i < n; ++i) tl[i] = tr[i] = 0.0f;
            sampler_[p].render(tl, tr, n);
            const float g = useFx_ ? 1.0f : pj_->parts[p].pages[PG_GLOB][0] / 127.0f;   // the part's LEVEL is in the hit already
            float* ol = outL_[p] + off;
            float* orr = outR_[p] + off;
            for (int i = 0; i < n; ++i) { ol[i] += tl[i] * g; orr[i] += tr[i] * g; }
        }
    }
    if (cyclesFn_) sec_.sampler += (uint32_t)(cyclesFn_() - cSmp);
    const uint32_t cFx = cyclesFn_ ? cyclesFn_() : 0;
    if (useFx) {
        float gain[app::kParts];
        for (int p = 0; p < app::kParts; ++p) gain[p] = type_[p] == PT_SAMPLER ? pj_->parts[p].pages[PG_GLOB][0] / 127.0f : partGain_[p];
        fx_.process(outL_, outR_, gain, sounding_, left, right, frames);
    }
    if (cyclesFn_) sec_.fx += (uint32_t)(cyclesFn_() - cFx);
    audition_.render(left, right, frames);
    // the master, a gentle limiter, the meters
    const float m = pj_->master / 127.0f * kMasterScale;
    float pl = peak_[0], pr = peak_[1];
    for (int i = 0; i < frames; ++i) {
        left[i] = softLimit(left[i] * m);
        right[i] = softLimit(right[i] * m);
        const float al = left[i] < 0 ? -left[i] : left[i], ar = right[i] < 0 ? -right[i] : right[i];
        if (al > pl) pl = al;
        if (ar > pr) pr = ar;
    }
    peak_[0] = pl;
    peak_[1] = pr;
    for (int p = 0; p < kParts; ++p) active_[p] = type_[p] == PT_SAMPLER ? sampler_[p].active() :
#if DBOY_LEAN
                                                                lean_ ? leanPart_[p].activeVoices() :
#endif
#if DBOY_OLD_ENGINE
                                                                        cathode_[p].engine().activeVoiceCount();
#else
                                                                        0;
#endif
    ++sec_.blocks;
    clock_ += (uint64_t)frames;
}

}  // namespace audio
