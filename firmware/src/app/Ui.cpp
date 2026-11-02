#include "app/Ui.h"

#include "app/Looper.h"
#include "app/MidiMap.h"
#include "app/Slicer.h"
#include "app/WaveLib.h"

namespace app {

static const char* const kPrio[4] = {"LOW", "MID", "HIGH", "KEEP"};
#define DOT " \xC2\xB7 "      // " · "
#define RING "\xE2\x97\x8B"  // "○", the CLEAR key as it is printed
#define DISC "\xE2\x97\x8F"  // "●", the ADD key
#define ENDASH "\xE2\x80\x93" // "–"

constexpr uint32_t kHomeHoldMs = 600, kToastMs = 1100, kFlashMs = 900, kHoldMs = 350, kDoubleClearMs = 650, kClearHoldMs = 1000, kDeleteAskMs = 4000, kPreviewDelayMs = 140, kBusyHoldMs = 10000;

// ---------------------------------------------------------------------------------------------------------------------------------
// small helpers

const char* Ui::toast() const { return toast_.empty() || (int32_t)(sv_.nowMs() - toastUntil_) >= 0 ? nullptr : toast_.c_str(); }
bool Ui::laneFlashing(int lane) const { return flashLane_ == lane && (int32_t)(sv_.nowMs() - flashUntil_) < 0; }

void Ui::say(const char* msg) {
    toast_.clear();
    toast_.add(msg);
    toastUntil_ = sv_.nowMs() + kToastMs;
    dirty();
}

int Ui::curLaneIndex() const {
    const Pattern& p = pattern();
    if (p.lanes[sel_].part == part_) return sel_;
    for (int i = 0; i < kLanes; ++i) if (p.lanes[i].part == part_) return i;
    return sel_;
}

bool Ui::partHasLane(int part) const {
    for (int i = 0; i < kLanes; ++i) if (pattern().lanes[i].part == part) return true;
    return false;
}

bool Ui::pageExists(int page) const {
    if (isSamplerPart(part_)) return page == PG_GLOB || (lanePageIndex(page) >= 0 && partHasLane(part_));   // a sampler's pages are its lane's
    return page < kSynthPages;
}

int16_t& Ui::pageValue(int page, int param) {
    if (isSamplerPart(part_) && page != PG_GLOB) return pj_.patterns[tp_.pat].lanes[curLaneIndex()].smpParam(page, param);
    return pj_.parts[part_].pages[page][param];
}

int Ui::shownValue(int page, int param) {
    if (Step* lk = lockStep()) {
        const uint8_t key = lockKey(page, param);
        if (lk->hasLock(key)) return lk->lockValue(key, 0);
    }
    return pageValue(page, param);
}

int Ui::paramMax(int page, int param) const {
    const ParamDef& p = pageDef(page).p[param];
    if (page == PG_MOD && param == 2 && isSamplerPart(part_)) return 2;   // (LFO DEST: a sample has no phase distortion to move)
    return p.unit == Unit::Sample ? (lib_.count > 0 ? lib_.count - 1 : 0) : p.max;
}
int Ui::paramMin(int page, int param) const { return pageDef(page).p[param].min; }

Step* Ui::lockStep() { return lockOn_ && lockPat_ == tp_.pat ? &pattern().lanes[lockLane_].st[lockIdx_] : nullptr; }

int Ui::browseCatCount() const { return isSamplerPart(part_) ? (lib_.catCount > 0 ? lib_.catCount : 1) : kPresetCatCount; }
const char* Ui::browseCatName(int c) const {
    if (isSamplerPart(part_)) return lib_.catCount > 0 ? lib_.cats[c] : "ALL";
    return kPresetCats[c];
}

int Ui::browseCount() const {
    int n = 0;
    if (isSamplerPart(part_)) return lib_.catSize(browseCat_);
    for (int k = 0; k < presetCount(); ++k) n += browseCat_ == 0 || presetCat(presetIdAt(k)) == browseCat_;
    return n;
}

int Ui::browseItem(int row) const {
    int n = 0;
    if (isSamplerPart(part_)) {
        for (int i = 0; i < lib_.count; ++i)
            if (browseCat_ == 0 || lib_.items[i].cat == browseCat_) { if (n == row) return i; ++n; }
    } else {
        for (int k = 0; k < presetCount(); ++k) {
            const int id = presetIdAt(k);
            if (browseCat_ == 0 || presetCat(id) == browseCat_) { if (n == row) return id; ++n; }
        }
    }
    return -1;
}

double Ui::songBars(const Song& s) const {
    double b = 0;
    for (int i = 0; i < s.nRows; ++i) b += (double)s.rows[i].rep * patLen(pj_.patterns[s.rows[i].pat]) / 16.0;
    return b;
}

// a JavaScript number that is a multiple of 1/16, the way String() prints it ("2", "1.5", "0.0625")
static void jsNum16(Text& out, double v) {
    long n = (long)(v * 16.0 + (v < 0 ? -0.5 : 0.5));
    if (n < 0) { out.ch('-'); n = -n; }
    out.num(n / 16);
    long f = (n % 16) * 625;   // tenths of a thousandth: 1/16 = 0.0625
    if (f) {
        out.ch('.');
        Str<8> d;
        d.pad(f, 4);
        int len = 4;
        while (len > 0 && d.s[len - 1] == '0') --len;
        d.s[len] = 0;
        out.add(d.s);
    }
}

static void fmtClock(Text& out, double sec) {
    out.num((long)(sec / 60.0));
    out.ch(':');
    double m = sec - 60.0 * (double)(long)(sec / 60.0);
    out.pad(jsRound(m), 2);
}

const Ladder& Ui::ladder() {
    // the mockup rebuilds its ladder when the cost-relevant settings change (a JSON signature of the part descriptions)
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
    for (int i = 0; i < kParts; ++i) {
        const cost::PartDesc d = partDesc(pj_, i);
        mix(d.voices); mix(d.priority); mix(d.sampler);
        for (int s = 0; s < 2; ++s) { mix((uint32_t)d.voice.slot[s].audible); mix(d.voice.slot[s].unison); mix((uint32_t)d.voice.slot[s].kind); }
        mix((uint32_t)d.voice.f1); mix((uint32_t)d.voice.f2); mix(d.voice.f2On); mix((uint32_t)d.voice.route);
        mix(d.voice.drive > 0); mix(d.voice.ring > 0); mix(d.voice.sync > 0); mix(d.voice.width > 0); mix(d.voice.companding > 0);
        mix(d.voice.pd1); mix(d.voice.pd2); mix(d.voice.sub); mix(d.voice.noise); mix(d.voice.hpf); mix(d.voice.clip);
        mix(d.amp);
    }
    mix(fxDesc(pj_).fx);   // the shared effects in use: the same signature as the Instrument's, so the MIXER shows the ladder the governor walks
    mix(pj_.partsIdle);
    if (h != ladderSig_ || ladder_.count == 0) {
        ladderSig_ = h;
        buildLadder(pj_, ladder_);
    }
    return ladder_;
}

void Ui::fixSongRow() {
    const int n = song().nRows;
    songRow_ = clampi(songRow_, 0, n > 0 ? n - 1 : 0);
}

// whether any part sends to effect `e` (0 delay, 1 reverb, 2 chorus) in the project's own settings
bool Ui::sendUsedElsewhere(int e) const {
    for (int p = 0; p < kParts; ++p) if (pj_.fx.send[p][e] > 0) return true;
    return false;
}

// Everything the UI points at, made valid for the model as it is now: the part on screen may have changed type, the pattern may have
// switched under it (a queued switch, a song), a lane may have got shorter, a song may have lost its rows. Called before every input
// and every frame, so no handler and no drawing ever reads past what exists.
void Ui::settle() {
    const bool smp = isSamplerPart(part_);
    if (smp != browseSmp_) browseOpen();   // presets and samples have other categories
    browseCat_ = clampi(browseCat_, 0, browseCatCount() - 1);
    const int n = browseCount();
    browseIdx_ = clampi(browseIdx_, 0, n > 0 ? n - 1 : 0);
    ensurePage();
    fixPage();
    trackerFix();
    fixSongRow();
    if (held_.on && held_.idx >= lane().n) held_.on = false;
    if (lockOn_ && (lockPat_ != tp_.pat || lockIdx_ >= pattern().lanes[lockLane_].n)) lockOn_ = false;
    if (lockOn_ && mode_ == Mode::Fx) {   // a lock on the FX screen is for its lane's part and the two views that can hold one
        part_ = pattern().lanes[lockLane_].part;
        if (fxView_ != FX_SENDS && fxView_ != FX_AMP) fxView_ = FX_SENDS;
    }
    if (tp_.songPlaying) {   // a song replaced or undone while it plays (paste, undo): its position stays inside it
        const int rows = pj_.songs[tp_.songIdx].nRows;
        if (rows == 0 || tp_.playRow >= rows || tp_.jumpTo >= rows) {
            sv_.holdAudio(true);   // the sequencer moves these at a pattern end
            if (rows == 0) tp_.songPlaying = 0;   // no rows left: the pattern plays on
            else if (tp_.playRow >= rows) tp_.playRow = (uint8_t)(rows - 1);
            if (tp_.jumpTo >= rows) tp_.jumpTo = -1;
            sv_.holdAudio(false);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// bindings: what the eight knobs do in this mode

void Ui::bindings(Bind b[8]) const {
    baseBindings(b);
    if (!shift_) return;
    const bool smp = isSamplerPart(lane().part);
    for (int i = 0; i < 8; ++i) {   // SHIFT is the panel's coarse turn: on a sample knob that is a folder
        const int knob = b[i].id == B::Root && smp ? PickLane : b[i].id == B::StepSample ? PickStep : b[i].id == B::Param && page_ == PG_SMP && b[i].arg == 0 ? PickPage : -1;
        if (knob >= 0) { b[i].id = B::Folder; b[i].arg = (int8_t)knob; }
    }
}

void Ui::baseBindings(Bind b[8]) const {
    for (int i = 0; i < 8; ++i) b[i] = Bind();
    const Lane& ln = lane();
    const bool smp = isSamplerPart(ln.part);
    auto set = [&](int i, B id, int arg = 0) { b[i].id = id; b[i].arg = (int8_t)arg; };
    switch (mode_) {
        case Mode::Play:
            set(0, B::Tempo); set(1, B::Swing); set(2, B::PlayedBy); set(3, B::PatternSel);
            set(4, B::Root); set(5, B::Level); set(6, B::Length); set(7, B::Hits);
            return;
        case Mode::Song:
            set(0, B::SongSel); set(1, B::SongRowB); set(2, B::SongPat); set(3, B::SongRep);
            set(4, B::SongEnd); set(5, B::SongLen); set(6, B::SongTime);
            return;
        case Mode::ProjectM:
            if (projSub_ == 1) { set(0, B::MidiLane); set(1, B::MidiCh); set(2, B::MidiNote); set(3, B::MidiShared); return; }
            if (projSub_ == 2) { set(4, B::ExportSong); set(5, B::ExportKindB); return; }
            set(0, B::KeyB); set(1, B::ScaleB); set(2, B::Tempo); set(3, B::Swing); set(4, B::CardSlot);
            return;
        case Mode::Step:
            if (stepSub_ == 2) {
                set(0, B::Velocity); set(1, B::GateB); set(2, B::Prob);
                set(4, B::TrNote); set(5, B::TrOctave);
                if (!smp) { set(3, B::TrInScale); set(6, B::ChordB); set(7, B::Inv); }
                else {
                    set(6, B::StepSample);
                    if (ln.smpParam(PG_SLC, SLC_MODE) != SLICE_OFF) set(7, B::StepSlice);
                }
            } else if (stepSub_ == 1 && smp) {
                // a drum lane's second view: the sample per step (a sample lock, SMP:0)
                set(0, B::Root, 1); set(4, B::StepSample); set(5, B::Pitch); set(6, B::Velocity); set(7, B::Prob);
                if (ln.smpParam(PG_SLC, SLC_MODE) != SLICE_OFF) set(1, B::StepSlice);
            } else if (stepSub_ == 1) {
                set(0, B::ArpB); set(1, B::ArpRate); set(2, B::ArpOct); set(3, B::ArpGate);
                set(4, B::ChordB); set(5, B::Inv); set(6, B::Voicing); set(7, B::Strum);
            } else {
                set(0, B::Root); set(1, B::Length); set(2, B::Hits); set(3, B::Rotate);
                set(4, B::Note); set(5, B::Velocity); set(6, B::GateB); set(7, B::Prob);
            }
            return;
        case Mode::Sound:
            if (page_ == PG_SLC && sliceEdit_ && !lockOn_) {
                set(0, B::SliceSel); set(1, B::SliceMove); set(2, B::SlicePitch); set(3, B::SliceLevel);
                set(4, B::SliceRev); set(5, B::SliceOn); set(6, B::SliceSplit); set(7, B::SliceMerge);
                return;
            }
            for (int i = 0; i < 8; ++i) set(i, B::Param, i);
            return;
        case Mode::Mixer:
            if (mixSub_ == 3) { set(3, B::AutoDeg); set(4, B::CpuLoad); set(5, B::DegradeB); return; }
            for (int i = 0; i < kParts; ++i) set(i, mixSub_ == 0 ? B::MixLevel : mixSub_ == 1 ? B::Voices : B::Prio, i);   // a knob for each part, A on T1
            if (mixSub_ == 0) set(7, B::Master);
            if (mixSub_ == 1) set(7, B::CostB);
            return;
        case Mode::Browse:
            set(0, B::Bank); set(1, B::Category); set(2, B::BrowsePart); set(3, B::Instrument); set(4, B::Scroll); set(5, B::Scroll3); set(6, B::BrowseOk); set(7, B::BrowsePreview);
            return;
        case Mode::Record:
            set(0, B::RecSource); set(1, B::RecGain); set(2, B::RecCapture); set(3, B::RecThreshold);
            set(4, B::RecChannel); set(5, B::RecMonitor); set(6, B::RecStopB); set(7, B::RecRoom);
            return;
        case Mode::Save:
            set(0, B::SvCursor); set(1, B::SvChar); set(2, B::SvCat); set(3, B::SvMode); set(4, B::SvTarget); set(5, B::SvVoices); set(6, B::SvPart);
            return;
        case Mode::Fx:
            if (fxView_ == FX_SENDS) { set(0, B::FxPart); for (int i = 0; i < kSendsUsed; ++i) set(1 + i, B::FxParam, i); return; }
            for (int i = 0; i < kFxParams; ++i) set(i, B::FxParam, i);
            return;
    }
}

void Ui::describe(const Bind& bd, int i, Cell& c) {
    c = Cell();
    Lane& ln = lane();
    Step& st = ln.st[cursor_];
    const bool smp = isSamplerPart(ln.part);
    const int arg = bd.arg;
    Song& so = song();
    SongRow* r = so.nRows ? &so.rows[clampi(songRow_, 0, so.nRows - 1)] : nullptr;
    const float total = voiceTotal(pj_);
    switch (bd.id) {
        case B::None: c.dis = true; break;
        case B::Tempo: c.label.add("TEMPO"); c.value.num(pj_.bpm); break;
        case B::Swing: c.label.add("SWING"); c.value.num(pj_.swing).ch('%'); break;
        case B::PlayedBy:
            c.label.add("PLAYED BY");
            c.value.add(kPartName[ln.part]).add(smp ? " SAMPLE" : " SYNTH");
            c.col = kPartHex[ln.part];
            c.tag.add("SHIFT");
            c.costTag = true;
            break;
        case B::PatternSel:
            c.label.add("PATTERN");
            c.value.ch('P').num(tp_.pat + 1);
            if (tp_.nextPat >= 0) c.value.add(" \xE2\x96\xB8P").num(tp_.nextPat + 1);   // ▸
            break;
        case B::Root:
            if (smp) pickCell(pickFor(PickLane), c);
            else { c.label.add(arg ? "LANE SMP" : "ROOT"); noteName(c.value, ln.root); }
            break;
        case B::Folder: {
            c.label.add("FOLDER");
            if (!lib_.count) { c.value.add("?"); c.dis = true; break; }
            const int cat = pickFolder(pickFor(arg));
            int at = 0;
            const int n = lib_.catsInUse(cat, at);
            c.value.add(lib_.cats[cat]);
            c.tag.num(at + 1).ch('/').num(n);
            c.bar = n > 1 ? (float)at / (float)(n - 1) : 0.0f;
            break;
        }
        case B::Level: c.label.add("LEVEL"); c.value.num(ln.level); c.bar = ln.level / 127.0f; break;
        case B::Length: c.label.add("LENGTH"); c.value.num(ln.n); c.bar = ln.n / 32.0f; break;
        case B::Hits: c.label.add("HITS"); c.value.num(ln.k); c.bar = ln.n ? (float)ln.k / ln.n : 0; break;
        case B::Rotate: c.label.add("ROTATE"); c.value.num(ln.r); break;
        case B::Note:
            c.label.add(smp ? "PITCH" : "NOTE");
            c.value.sgn(st.note);
            if (smp) c.value.add(" st");
            else noteName(c.dest, scaleNote(ln.root + st.note, pj_.key, pj_.scale));
            break;
        case B::Pitch: c.label.add("PITCH"); c.value.sgn(st.note); break;
        case B::TrNote: {
            const int n = trackerNote();
            c.label.add(smp ? "PITCH" : "NOTE");
            c.value.sgn(n);
            if (smp) c.value.add(" st");
            else noteName(c.dest, clampi(scaleNote(ln.root + n, pj_.key, pj_.scale), 0, 127));   // (what plays: a high root and +24 run out of notes)
            c.warn = trStop_ != 0;
            break;
        }
        case B::TrOctave: {
            const int n = trackerNote();
            c.label.add("OCTAVE");
            if (smp) c.value.sgn(n / 12);
            else c.value.num(floorDiv(clampi(scaleNote(ln.root + n, pj_.key, pj_.scale), 0, 127), 12) - 1);
            c.warn = trStop_ != 0;
            break;
        }
        case B::TrInScale: c.label.add("IN SCALE"); c.value.add(inScale() ? "ON" : "OFF"); c.dis = pj_.scale == 0; break;
        case B::Velocity: c.label.add("VELOCITY"); c.value.num(st.vel); c.bar = st.vel / 127.0f; break;
        case B::GateB: c.label.add("GATE"); c.value.fixed(st.gate / 32.0, 2); c.bar = st.gate / 127.0f; break;
        case B::Prob: c.label.add("PROB"); c.value.num(st.prob).ch('%'); c.bar = st.prob / 100.0f; break;
        case B::SongSel: c.label.add("SONG"); c.value.add("SONG ").num(tp_.songIdx + 1); break;
        case B::ExportSong: c.label.add("SONG"); c.value.add("SONG ").num(exportSong_ + 1); break;
        case B::ExportKindB: { static const char* const kKinds[kExportKinds] = {"SONG WAV", "STEMS", "MIDI FILE"}; c.label.add("EXPORT"); c.value.add(kKinds[exportKind_]); break; }
        case B::SongRowB:
            c.label.add("ROW");
            if (so.nRows) c.value.num(songRow_ + 1).ch('/').num(so.nRows); else c.value.add(ENDASH);
            c.dis = !so.nRows;
            break;
        case B::SongPat:
            c.label.add("PATTERN");
            if (r) c.value.ch('P').num(r->pat + 1); else c.value.add(ENDASH);
            c.dis = !r;
            break;
        case B::SongRep:
            c.label.add("REPEATS");
            if (r) c.value.ch('x').num(r->rep); else c.value.add(ENDASH);
            c.dis = !r;
            break;
        case B::SongEnd: c.label.add("AT THE END"); c.value.add(so.loop ? "LOOP" : "STOP"); break;
        case B::SongLen: c.label.add("LENGTH"); jsNum16(c.value, songBars(so)); c.value.add(songBars(so) == 1 ? " bar" : " bars"); c.ro = true; break;
        case B::SongTime: c.label.add("TIME"); fmtClock(c.value, songBars(so) * 16 * stepDur()); c.ro = true; break;
        case B::KeyB: c.label.add("KEY"); c.value.add(kNoteNames[pj_.key]); break;
        case B::ScaleB: c.label.add("SCALE"); c.value.add(kScales[pj_.scale].name); break;
        case B::CardSlot: {
            SlotInfo si;
            if (slot_ < 0) {
                c.label.add("FACTORY");
                c.value.ch('F').num(slot_ + kFactoryProjects + 1);
                break;
            }
            c.label.add("CARD SLOT");
            c.value.num(slot_ + 1);
            if (!slotInfo(slot_, si)) c.value.add(" EMPTY");
            break;
        }
        case B::StepSample:
            pickCell(pickFor(PickStep), c);
            c.col = kPartHex[ln.part];
            break;
        case B::RecSource: c.label.add("SOURCE"); c.value.add(recSet_.source ? "OUTPUT" : "LINE IN"); break;
        case B::RecGain: c.label.add("GAIN"); c.value.add("+").fixed(recSet_.gain * 1.5, 1).add(" dB"); c.bar = recSet_.gain / 15.0f; c.dis = recSet_.source != 0; break;
        case B::RecCapture: c.label.add("CAPTURE"); c.value.add(recSet_.autoCapture ? "AUTO" : "MANUAL"); break;
        case B::RecThreshold: c.label.add("THRESHOLD"); c.value.num(recSet_.thresholdDb).add(" dB"); c.bar = (recSet_.thresholdDb + 60) / 54.0f; c.idle = !recSet_.autoCapture && recSet_.stop == 0; break;
        case B::RecChannel: { static const char* const kCh[3] = {"L+R", "LEFT", "RIGHT"}; c.label.add("INPUT"); c.value.add(kCh[recSet_.channel]); break; }
        case B::RecMonitor: c.label.add("MONITOR"); c.value.num(recSet_.monitor * 100 / 127).ch('%'); c.bar = recSet_.monitor / 127.0f; c.dis = recSet_.source != 0; break;
        case B::RecStopB: c.label.add("AUTO STOP"); if (recSet_.stop == 0) c.value.add("OFF"); else c.value.fixed(kRecStops[recSet_.stop], 1).add(" s"); break;
        case B::RecRoom: {
            c.label.add("ROOM");
            c.ro = true;
            if (rec_.capacity && rec_.state != REC_IDLE) c.value.fixed((double)(rec_.capacity - rec_.frames) / rec_.rate, 1).add(" s");
            else c.value.add("-");
            break;
        }
        case B::StepSlice: {
            const int own = st.lockValue(lockKey(PG_SLC, SLC_SLICE), 0);
            c.label.add("STEP SLICE");
            if (own > 0) c.value.num(own); else c.value.add("AUTO");
            c.lock = own > 0;
            c.col = kPartHex[ln.part];
            break;
        }
        case B::SliceSel: case B::SliceMove: case B::SlicePitch: case B::SliceLevel: case B::SliceRev: case B::SliceOn: case B::SliceSplit: case B::SliceMerge: {
            const SliceMap* m = shownSlices();
            const int k = m ? clampi(sliceSel_, 0, m->count - 1) : 0;
            const Slice* s = m ? &m->s[k] : nullptr;
            static const char* const kNames[] = {"SLICE", "START", "PITCH", "LEVEL", "REVERSE", "PLAYS", "SPLIT", "MERGE"};
            c.label.add(kNames[(int)bd.id - (int)B::SliceSel]);
            if (!s) { c.dis = true; c.value.add("-"); break; }
            switch (bd.id) {
                case B::SliceSel: c.value.num(k + 1).ch('/').num(m->count); break;
                case B::SliceMove: c.value.fixed(lib_.seconds(m->sample) * (float)s->start / (float)m->frames, 3).add(" s"); break;
                case B::SlicePitch: c.value.sgn(s->pitch).add(" st"); c.bar = (s->pitch + 24) / 48.0f; break;
                case B::SliceLevel: c.value.num(s->level * 100 / 127).ch('%'); c.bar = s->level / 127.0f; break;
                case B::SliceRev: c.value.add(s->flags & SLICE_REVERSE ? "ON" : "OFF"); break;
                case B::SliceOn: c.value.add(s->flags & SLICE_MUTED ? "MUTED" : "ON"); break;
                case B::SliceSplit: c.value.add("TURN RIGHT"); c.dis = m->count >= kSlices; break;
                default: c.value.add("TURN RIGHT"); c.dis = k == 0; break;
            }
            break;
        }
        case B::ArpB: c.label.add("ARP"); c.value.add(kArpModes[ln.arp.mode]); break;
        case B::ArpRate: c.label.add("ARP RATE"); c.value.ch('x').num(kArpRates[ln.arp.rate]); c.dis = ln.arp.mode == 0; break;
        case B::ArpOct: c.label.add("ARP OCT"); c.value.num(ln.arp.oct); c.dis = ln.arp.mode == 0; break;
        case B::ArpGate: c.label.add("ARP GATE"); c.value.num(ln.arp.gate).ch('%'); c.bar = ln.arp.gate / 100.0f; c.dis = ln.arp.mode == 0; break;
        case B::ChordB: c.label.add("CHORD"); c.value.add(kChords[st.chord].name); break;
        case B::Inv: c.label.add("INVERSION"); c.value.num(st.inv); c.dis = st.chord == 0; break;
        case B::Voicing: c.label.add("VOICING"); c.value.add(st.spread ? "OPEN" : "CLOSE"); c.dis = st.chord == 0; break;
        case B::Strum:
            c.label.add("STRUM"); c.value.num(st.strum).add(" ms"); c.bar = st.strum / 120.0f;
            c.dis = st.chord == 0 || ln.arp.mode > 0;
            break;
        case B::Param: {
            const int page = page_;
            const ParamDef& p = pageDef(page).p[arg];
            Step* lk = lockStep();
            const uint8_t key = lockKey(page, arg);
            const bool locked = lk && lk->hasLock(key);
            const int val = locked ? lk->lockValue(key, 0) : pageValue(page, arg);
            const int lo = paramMin(page, arg), hi = paramMax(page, arg);
            c.label.add(p.name);
            formatParam(c.value, page, arg, val, &lib_);
            c.bar = hi > lo ? (float)(val - lo) / (float)(hi - lo) : 0.0f;
            c.dis = p.name[0] == '-' && p.name[1] == 0;
            c.lock = locked;
            if (p.unit == Unit::Sample) pickCell(pickFor(PickPage), c);
            // an oscillator on a wavetable: PD SHAPE is where in the table it plays, PD AMT does nothing; a bank whose file is not on the card is flagged
            if ((page == PG_OSC1 || page == PG_OSC2) && !isSamplerPart(part_)) {
                const int w = pageValue(page, 0);
                if (arg == 0) c.warn = waveIsBank(w) && !waveSet().present[waveBankOf(w) < kWaveBanks ? waveBankOf(w) : 0];
                if (waveIsBank(w) && arg == 1) { c.label.clear(); c.label.add("POSITION"); }
                if (waveIsBank(w) && arg == 2) { c.dis = true; c.value.clear(); c.value.add("-"); c.bar = 0.0f; }
                if (!waveIsBank(w) && w != 0 && (arg == 1 || arg == 2)) c.idle = true;   // phase distortion bends a sine only
            }
            if (!isSamplerPart(part_)) {   // what has no effect in the patch as it stands is dimmed (c.idle: the value can still be turned, a lock too)
                if (page == PG_SUB && arg >= 5 && shownValue(PG_SUB, 4) <= 1) c.idle = true;   // the stack's detune, blend and width: one voice has none
                if (page == PG_FLT2 && arg != 5 && shownValue(PG_FLT2, 5) == 0) c.idle = true;                             // filter 2 is off
            }
            if (page == PG_MOD && (arg == 0 || arg == 2 || arg == 3) && shownValue(PG_MOD, 1) == 0) c.idle = true;   // the LFO has no depth: its rate, destination and shape do nothing
            if (page == PG_LOOP) {
                const Lane& ll = pattern().lanes[curLaneIndex()];
                if (arg == LP_XFADE) {
                    uint32_t ls = 0, le = 0;
                    double fade = 0, lap = 0;
                    const int ss = ll.sample();
                    c.value.clear();
                    if (val == 0) c.value.add("OFF");
                    else if (ll.smpParam(PG_LOOP, LP_UNIT) == LOOP_PCT) c.value.fixed(val / 127.0 * 50.0, 1).ch('%');
                    else if (shownLoop(ls, le, fade, lap, val, ss) && lib_.items[ss].rate) c.value.fixed(fade * 1000.0 / lib_.items[ss].rate, 1).add(" ms");   // (at most half the loop)
                    else c.value.fixed(kLoopFadeMaxMs * (val / 127.0) * (val / 127.0), 1).add(" ms");
                }
                if (ll.smpParam(PG_SMP, 3) != 2 || ll.smpParam(PG_SLC, SLC_MODE) == SLICE_SYNC) c.idle = true;   // only a looping sample has a loop
                if ((arg == LP_SFINE || arg == LP_EFINE) && ll.smpParam(PG_LOOP, LP_AUTO) != AUTO_OFF) c.idle = true;
                if (arg == LP_SEARCH && ll.smpParam(PG_LOOP, LP_AUTO) == AUTO_OFF) c.idle = true;
            }
            if (p.unit == Unit::MidiCh) {   // the lanes have the channels now (PROJECT, view 2); the part's own is kept in the file, unused
                c.value.clear();
                c.value.add("IN PROJECT");
                c.bar = 0.0f;
                c.ro = true;
                break;
            }
            if (!isSamplerPart(part_)) {
                if ((page == PG_FLT1 || page == PG_FLT2) && arg == 0 && !locked && menuHeavy(val) && (page == PG_FLT1 || descOf(pj_, part_).f2On))
                    c.tag.add("HEAVY ");
                if (!lk) costBadge(c.tag, pj_, part_, page, arg);
                c.costTag = !c.tag.empty();
            }
            break;
        }
        case B::Prio:
            c.label.add("PRIO ").add(kPartName[arg]); c.value.add(kPrio[pj_.parts[arg].priority]);
            c.col = kPartHex[arg]; c.bar = pj_.parts[arg].priority / 3.0f;
            break;
        case B::AutoDeg: c.label.add("AUTO DEGRADE"); c.value.add(autoDegrade_ ? "ON" : "OFF"); break;
        case B::CpuLoad: {
            const float load = autoDegrade_ ? sv_.cpuLoad() : 0.0f;
            c.label.add("CPU LOAD"); c.value.num(jsRound(load * 100)).ch('%'); c.ro = true;
            c.bar = load < 1 ? load : 1; c.warn = load >= 0.85f;
            break;
        }
        case B::DegradeB: {
            const int lvl = degradeLevel();
            c.label.add("DEGRADE"); c.value.num(lvl).ch('/').num(ladder().count); c.ro = true; c.warn = lvl > 0;
            break;
        }
        case B::MixLevel:
            c.label.add("LEVEL ").add(kPartName[arg]); c.value.num(pj_.parts[arg].level);
            c.bar = pj_.parts[arg].level / 127.0f; c.col = kPartHex[arg];
            break;
        case B::Master: c.label.add("MASTER"); c.value.num(pj_.master); c.bar = pj_.master / 127.0f; break;
        case B::Voices:
            c.label.add("VOICES ").add(kPartName[arg]); c.value.num(pj_.parts[arg].voices);
            c.warn = total > kVoiceBudget; c.col = kPartHex[arg];
            break;
        case B::CostB: {
            const int lvl = degradeLevel();
            c.label.add("COST"); c.value.fixed(total, 1).ch('/').num(kVoiceBudget); c.ro = true;
            c.warn = total > kVoiceBudget; c.bar = total / kVoiceBudget < 1 ? total / kVoiceBudget : 1;
            if (lvl > 0) { c.tag.add("EFF ").fixed(effTotal(pj_, ladder(), lvl), 1); c.costTag = true; }
            break;
        }
        case B::Bank: c.label.add("BANK"); c.value.add(isSamplerPart(part_) ? "SAMPLES" : "FACTORY"); c.ro = true; break;
        case B::Category: c.label.add("CATEGORY"); c.value.add(browseCatName(browseCat_)); break;
        case B::BrowsePart: c.label.add("PART"); c.value.add(kPartName[part_]); c.col = kPartHex[part_]; break;
        case B::Instrument:
            c.label.add("INSTRUMENT"); c.value.add(isSamplerPart(part_) ? "SAMPLER" : "SYNTH");
            c.tag.add("SHIFT"); c.costTag = true;
            break;
        case B::Scroll: c.label.add("SCROLL"); c.value.num(browseIdx_ + 1).ch('/').num(browseCount()); break;
        case B::Scroll3: c.label.add("SCROLL x3"); c.value.num(browseIdx_ + 1).ch('/').num(browseCount()); break;
        case B::BrowseOk: c.label.add("OK"); c.value.add("LOADS IT"); c.ro = true; break;   // the keys that act here, as PROJECT's SD CARD line says its own
        case B::BrowsePreview: c.label.add("SCROLLING"); c.value.add("PLAYS IT"); c.ro = true; break;
        case B::SvCursor: c.label.add("CURSOR"); c.value.num(saveCur_ + 1).ch('/').num(kPresetNameMax); break;
        case B::SvChar:
            c.label.add("CHARACTER");
            if (saveName_[saveCur_] == ' ') c.value.add("SPACE"); else c.value.ch(saveName_[saveCur_]);
            break;
        case B::SvCat: c.label.add("CATEGORY"); c.value.add(kPresetCats[saveCat_]); break;
        case B::SvMode:
            c.label.add("SAVE AS");
            c.value.add(saveReplaces() ? "REPLACE" : "NEW");
            c.dis = !saveCanReplace();
            break;
        case B::SvTarget: {
            c.label.add("PRESET ID");
            const int id = saveTargetId();
            if (id < 0) c.value.add("NO ROOM"); else c.value.num(id);
            c.ro = true;
            break;
        }
        case B::SvVoices: c.label.add("VOICES"); c.value.num(pj_.parts[part_].voices); c.ro = true; break;
        case B::SvPart: c.label.add("PART"); c.value.add(kPartName[part_]); c.col = kPartHex[part_]; c.ro = true; break;
        case B::FxPart:
            c.label.add("PART"); c.value.add(kPartName[part_]).add(isSamplerPart(part_) ? " SAMPLER" : " SYNTH"); c.col = kPartHex[part_];
            c.dis = lockOn_;
            break;
        case B::FxParam: {
            const FxParamDef& d = fxDef(fxView_, bd.arg);
            const int fp = lockOn_ ? pattern().lanes[lockLane_].part : part_;   // a step's lock is for its lane's part
            const int16_t* vals = fxValues(pj_.fx, fxView_, fp);
            Step* lk = lockStep();
            const int lkey = fxLockKey(fxView_, bd.arg);
            const bool locked = lk && lkey >= 0 && lk->hasLock((uint8_t)lkey);
            const int v = locked ? lk->lockValue((uint8_t)lkey, 0) : vals[bd.arg];
            c.label.add(d.name);
            formatFx(c.value, d, v);
            c.bar = d.list ? (d.nList > 1 ? (float)v / (float)(d.nList - 1) : 0.0f) : v / 127.0f;
            if (fxView_ == FX_AMP) c.col = kPartHex[fp];
            c.lock = locked;
            if (lk && lkey < 0) c.dis = true;                                   // a lock holds only a part's sends and amp knobs
            if (lk && fxView_ == FX_AMP && vals[0] == 0) c.dis = true;          // an amp that is off has nothing to lock
            // the delay's TIME only counts when MODE is FREE: otherwise the tempo sets it
            if (fxView_ == FX_DELAY && bd.arg == 1 && vals[0] != 0) {   // (turning it says why nothing happens: see turn())
                c.dis = true;
                c.value.clear();
                c.value.fixed(delayMs(vals[0], vals[1], pj_.bpm), 0).add(" ms");
            }
            break;
        }
        case B::MidiLane:
            c.label.add("LANE"); c.value.add("LANE ").num(midiLane_ + 1); c.col = kPartHex[pattern().lanes[midiLane_].part];
            break;
        case B::MidiCh: {
            const int ch = pj_.midi[midiLane_].ch;
            c.label.add("MIDI CH");
            if (ch) c.value.add("CH ").num(ch); else c.value.add("OFF");
            break;
        }
        case B::MidiNote: {
            // what the note is for: on a shared channel the lane's own note; alone, a sampler lane's untransposed note; a synth lane
            // alone on its channel plays every note, so it has none
            const LaneMidi& m = pj_.midi[midiLane_];
            const bool shared = m.ch && lanesOnChannel(pj_, m.ch) > 1, smp = isSamplerPart(pattern().lanes[midiLane_].part);
            // (a synth lane alone keeps its note for when it shares the channel)
            c.label.add(smp && !shared ? "BASE NOTE" : "TRIG NOTE");
            c.value.num(m.note).ch(' ');
            noteName(c.value, m.note);
            c.dis = !m.ch;
            break;
        }
        case B::MidiShared: {
            const int ch = pj_.midi[midiLane_].ch, n = ch ? lanesOnChannel(pj_, ch) : 0;
            c.label.add("ON CHANNEL");
            if (!ch) c.value.add("\xE2\x80\x93");
            else if (n == 1) c.value.add("ALONE");
            else c.value.num(n).add(" LANES");
            c.ro = true;
            break;
        }
        case B::LaneSample: break;
    }
    if (tracker() && i != 3 && !trig(ln, cursor_, false)) c.idle = true;   // an empty step's knobs: the first turn puts the last note on it
    // the corner tag: INFO on a read-only cell, LOCK on a locked one, a cost badge, else the knob's name
    if (c.ro) { c.tag.clear(); c.tag.add("INFO"); c.costTag = false; }
    else if (c.lock) { c.tag.clear(); c.tag.add("LOCK"); c.costTag = false; }
    else if (c.tag.empty()) { c.tag.ch(i < 4 ? 'T' : 'B').num(i % 4 + 1); c.costTag = false; }
}

void Ui::cells(Cell out[8]) {
    settle();
    Bind b[8];
    bindings(b);
    for (int i = 0; i < 8; ++i) describe(b[i], i, out[i]);
}

float Ui::knobPlace(int enc, float* stop) {
    float gap = 0.0f;
    if (stop) *stop = 0.0f;
    if (enc < 0 || enc >= 8) return -1.0f;
    Bind b[8];
    bindings(b);
    Cell c;
    describe(b[enc], enc, c);
    Lane& ln = lane();
    Step& st = ln.st[cursor_];
    Song& so = song();
    const int arg = b[enc].arg;
    if (c.bar >= 0.0f) {
        switch (b[enc].id) {
            case B::Param: {
                if (pageDef(page_).p[arg].unit == Unit::Sample) { const int s = lib_.catSize(pickFolder(pickFor(PickPage))); gap = s > 1 ? 1.0f / (float)(s - 1) : 0.0f; break; }
                const int lo = paramMin(page_, arg), hi = paramMax(page_, arg); gap = hi > lo ? 1.0f / (float)(hi - lo) : 0.0f; break; }
            case B::FxParam: { const FxParamDef& d = fxDef(fxView_, arg); gap = d.list && d.nList > 1 ? 1.0f / (float)(d.nList - 1) : 0.0f; break; }
            case B::Hits: gap = ln.n ? 1.0f / (float)ln.n : 0.0f; break;
            case B::Length: gap = 1.0f / 32.0f; break;
            case B::Prio: gap = 1.0f / 3.0f; break;
            // the sample knobs: their place is in the folder, so the next stop is the folder's next sample (a step's knob has LANE at its left stop)
            case B::Folder: { int at = 0; const int folders = lib_.catsInUse(pickFolder(pickFor(arg)), at); gap = folders > 1 ? 1.0f / (float)(folders - 1) : 0.0f; break; }
            case B::Root: { const int s = lib_.catSize(pickFolder(pickFor(PickLane))); gap = s > 1 ? 1.0f / (float)(s - 1) : 0.0f; break; }
            case B::StepSample: { const int s = lib_.catSize(pickFolder(pickFor(PickStep))); gap = s > 0 ? 1.0f / (float)s : 0.0f; break; }
            default: break;
        }
        if (stop) *stop = gap;
        return c.bar;
    }
    const auto at = [&](int v, int lo, int hi) { if (stop && hi > lo) *stop = 1.0f / (float)(hi - lo); return hi > lo ? (float)(v - lo) / (float)(hi - lo) : 0.0f; };
    switch (b[enc].id) {   // the limits are apply()'s
        case B::Tempo: return at(pj_.bpm, 40, 240);
        case B::Swing: return at(pj_.swing, 0, 50);
        case B::PlayedBy: return at(ln.part, 0, kParts - 1);
        case B::PatternSel: return at(tp_.nextPat >= 0 ? tp_.nextPat : tp_.pat, 0, kPatterns - 1);
        case B::Root: return isSamplerPart(ln.part) ? -1.0f : at(ln.root, 12, 108);   // (a sample knob's place is its cell's bar: below 0 here means no samples)
        case B::Rotate: return at(ln.r, 0, ln.n - 1);
        case B::Note: case B::Pitch: return at(st.note, -24, 24);
        case B::TrNote: return at(trackerNote(), -24, 24);
        case B::TrOctave: { const float p = at(trackerNote(), -24, 24); if (stop) *stop = 0.25f; return p; }   // (twelve of the 48)
        case B::TrInScale: return at(trScale_ ? 1 : 0, 0, 1);
        case B::SongSel: return at(tp_.songIdx, 0, kSongs - 1);
        case B::ExportSong: return at(exportSong_, 0, kSongs - 1);
        case B::ExportKindB: return at(exportKind_, 0, kExportKinds - 1);
        case B::SongRowB: return at(songRow_, 0, so.nRows > 0 ? so.nRows - 1 : 0);
        case B::SongPat: return so.nRows ? at(so.rows[songRow_].pat, 0, kPatterns - 1) : -1.0f;
        case B::SongRep: return so.nRows ? at(so.rows[songRow_].rep, 1, 16) : -1.0f;
        case B::SongEnd: return at(so.loop ? 0 : 1, 0, 1);
        case B::KeyB: return at(pj_.key, 0, 11);
        case B::ScaleB: return at(pj_.scale, 0, kScaleCount - 1);
        case B::CardSlot: return at(slot_, -kFactoryProjects, kCardSlots - 1);
        case B::StepSlice: {
            const SliceMap* m = findSlices(pj_, st.lockValue(lockKey(PG_SMP, 0), ln.sample()));
            return at(st.lockValue(lockKey(PG_SLC, SLC_SLICE), 0), 0, m ? m->count : kSlices);
        }
        case B::ArpB: return at(ln.arp.mode, 0, kArpModeCount - 1);
        case B::ArpRate: return at(ln.arp.rate, 0, kArpRateCount - 1);
        case B::ArpOct: return at(ln.arp.oct, 1, 4);
        case B::ChordB: return at(st.chord, 0, kChordCount - 1);
        case B::Inv: return at(st.inv, 0, 3);
        case B::Voicing: return at(st.spread ? 1 : 0, 0, 1);
        case B::Prio: return at(pj_.parts[arg].priority, 0, 3);
        case B::AutoDeg: return at(autoDegrade_ ? 1 : 0, 0, 1);
        case B::Voices: return at(pj_.parts[arg].voices, 1, isSamplerPart(arg) ? 8 : 5);
        case B::Category: return at(browseCat_, 0, browseCatCount() - 1);
        case B::BrowsePart: case B::FxPart: return at(part_, 0, kParts - 1);
        case B::Scroll: case B::Scroll3: return at(browseIdx_, 0, browseCount() > 0 ? browseCount() - 1 : 0);
        case B::SvCursor: return at(saveCur_, 0, kPresetNameMax - 1);
        case B::SvCat: return at(saveCat_, 1, kPresetCatCount - 1);
        case B::MidiLane: return at(midiLane_, 0, kLanes - 1);
        case B::MidiCh: return at(pj_.midi[midiLane_].ch, 0, 16);
        case B::MidiNote: return at(pj_.midi[midiLane_].note, 0, 127);
        default: return -1.0f;
    }
}

void Ui::apply(const Bind& bd, int d) {
    Lane& ln = lane();
    Step& st = ln.st[cursor_];
    const int sg = signOf(d);
    const int arg = bd.arg;
    Song& so = song();
    switch (bd.id) {
        case B::Tempo: pj_.bpm = (uint16_t)clampi(pj_.bpm + d, 40, 240); break;
        case B::Swing: pj_.swing = (uint8_t)clampi(pj_.swing + d, 0, 50); break;
        case B::PlayedBy: {
            if (!shift_) {
                Text t;
                t.add("HOLD SHIFT AND TURN T3 TO MOVE LANE ").num(sel_ + 1).add(" TO ANOTHER PART");
                say(t.c_str());
                return;
            }
            const int was = ln.part;
            ln.part = (uint8_t)clampi(ln.part + sg, 0, kParts - 1);
            if (ln.part == was) return;
            part_ = ln.part;
            flashLane_ = sel_;
            flashUntil_ = sv_.nowMs() + kFlashMs;
            Text t;
            t.add("LANE ").num(sel_ + 1).add(" NOW PLAYS ON PART ").add(kPartName[ln.part])
                .add(isSamplerPart(ln.part) ? " (SAMPLER: ITS OWN SAMPLE, NOTES = PITCH)" : " (SYNTH)");
            say(t.c_str());
            break;
        }
        case B::PatternSel: {
            const int next = tp_.nextPat;
            selectPattern((next >= 0 ? next : tp_.pat) + sg);
            break;
        }
        case B::Root:
            if (isSamplerPart(ln.part)) pickTurn(pickFor(PickLane), sg);
            else ln.root = (uint8_t)clampi(ln.root + d, 12, 108);
            break;
        case B::Folder: pickFolderTurn(pickFor(arg), sg); break;
        case B::Level: ln.level = (uint8_t)clampi(ln.level + d, 0, 127); break;
        case B::Length:
        {
            const int n = clampi(ln.n + sg, 1, 32);
            if (ln.k > n) ln.k = (uint8_t)n;
            ln.r = (uint8_t)(ln.r % n);
            ln.n = (uint8_t)n;
            fixPage();
        }
            break;
        case B::Hits:
            ln.k = (uint8_t)clampi(ln.k + sg, 0, ln.n);
            if (ln.k) ln.eu = 1;   // an emptied lane: hits bring the rhythm back
            break;
        case B::Rotate: ln.r = (uint8_t)mod(ln.r + sg, ln.n); break;
        case B::Note:
        case B::Pitch: st.note = (int8_t)clampi(st.note + sg, -24, 24); break;
        case B::TrNote: trackerMove(d, false); break;
        case B::TrOctave: trackerMove(sg, true); break;
        case B::TrInScale: trScale_ = d > 0; break;
        case B::Velocity: st.vel = (uint8_t)clampi(st.vel + d, 1, 127); break;
        case B::GateB: st.gate = (uint8_t)clampi(st.gate + d, 1, 127); break;
        case B::Prob: st.prob = (uint8_t)clampi(st.prob + d, 0, 100); break;
        case B::SongSel:
            if (tp_.songPlaying) { say("STOP THE SONG TO PICK ANOTHER"); return; }
            tp_.songIdx = (uint8_t)clampi(tp_.songIdx + sg, 0, kSongs - 1);
            songRow_ = 0;
            break;
        case B::SongRowB: songRow_ = clampi(songRow_ + sg, 0, so.nRows > 0 ? so.nRows - 1 : 0); break;
        case B::SongPat: if (so.nRows) { SongRow& r = so.rows[songRow_]; r.pat = (uint8_t)clampi(r.pat + sg, 0, kPatterns - 1); } break;
        case B::SongRep: if (so.nRows) { SongRow& r = so.rows[songRow_]; r.rep = (uint8_t)clampi(r.rep + sg, 1, 16); } break;
        case B::SongEnd: so.loop = d < 0 ? 1 : 0; break;
        case B::KeyB: pj_.key = (uint8_t)mod(pj_.key + sg, 12); break;
        case B::ScaleB: pj_.scale = (uint8_t)clampi(pj_.scale + sg, 0, kScaleCount - 1); break;
        case B::CardSlot: slot_ = clampi(slot_ + sg, -kFactoryProjects, kCardSlots - 1); break;
        case B::ExportSong: exportSong_ = clampi(exportSong_ + sg, 0, kSongs - 1); break;
        case B::ExportKindB: exportKind_ = clampi(exportKind_ + sg, 0, kExportKinds - 1); break;
        case B::StepSample: pickTurn(pickFor(PickStep), sg); break;
        case B::RecSource: if (rec_.state != REC_TAKING && rec_.state != REC_ARMED) { recSet_.source = sg > 0 ? 1 : 0; sv_.recListen(recSet_, true); } break;
        case B::RecGain: recSet_.gain = (uint8_t)clampi(recSet_.gain + sg, 0, 15); sv_.recListen(recSet_, true); break;
        case B::RecCapture: recSet_.autoCapture = sg > 0 ? 1 : 0; break;
        case B::RecThreshold: recSet_.thresholdDb = (int8_t)clampi(recSet_.thresholdDb + sg, -60, -6); break;
        case B::RecChannel: recSet_.channel = (uint8_t)clampi(recSet_.channel + sg, 0, 2); break;
        case B::RecMonitor: recSet_.monitor = (uint8_t)clampi(recSet_.monitor + d, 0, 127); sv_.recListen(recSet_, true); break;
        case B::RecStopB: recSet_.stop = (uint8_t)clampi(recSet_.stop + sg, 0, kRecStopChoices - 1); break;
        case B::RecRoom: return;
        case B::StepSlice: {
            const uint8_t key = lockKey(PG_SLC, SLC_SLICE);
            const SliceMap* m = findSlices(pj_, st.lockValue(lockKey(PG_SMP, 0), ln.sample()));
            const int v = clampi(st.lockValue(key, 0) + sg, 0, m ? m->count : kSlices);
            if (v == 0) st.removeLock(key);
            else if (!st.setLock(key, (int16_t)v)) say("A STEP HOLDS 8 LOCKS");
            else if (m) sv_.auditionSlice(m->sample, v - 1);
            lockEdited_ = true;
            break;
        }
        case B::SliceSel: case B::SliceMove: case B::SlicePitch: case B::SliceLevel: case B::SliceRev: case B::SliceOn: case B::SliceSplit: case B::SliceMerge: {
            SliceMap* m = const_cast<SliceMap*>(shownSlices());
            if (!m) return;
            sliceSel_ = clampi(sliceSel_, 0, m->count - 1);
            if (bd.id == B::SliceSel) {
                sliceSel_ = clampi(sliceSel_ + sg, 0, m->count - 1);
                sv_.auditionSlice(m->sample, sliceSel_);
                break;
            }
            if ((bd.id == B::SliceSplit || bd.id == B::SliceMerge) && d < 0) return;
            SliceMap e = *m;   // edited outside the hold: the audio is held only for the copy
            Slice& s = e.s[sliceSel_];
            switch (bd.id) {
                case B::SliceMove: {
                    const int64_t lo = sliceSel_ > 0 ? (int64_t)e.s[sliceSel_ - 1].start + 16 : 0;
                    const int64_t hi = (sliceSel_ + 1 < e.count ? (int64_t)e.s[sliceSel_ + 1].start : (int64_t)e.frames) - 16;
                    if (lo > hi) return;
                    const int64_t want = (int64_t)s.start + (int64_t)d * (int64_t)(e.frames / 4000 + 1);
                    s.start = (uint32_t)(want < lo ? lo : want > hi ? hi : want);
                    break;
                }
                case B::SlicePitch: s.pitch = (int8_t)clampi(s.pitch + sg, -24, 24); break;
                case B::SliceLevel: s.level = (uint8_t)clampi(s.level + d, 0, 127); break;
                case B::SliceRev: s.flags = (uint8_t)(d > 0 ? s.flags | SLICE_REVERSE : s.flags & ~SLICE_REVERSE); break;
                case B::SliceOn: s.flags = (uint8_t)(d < 0 ? s.flags | SLICE_MUTED : s.flags & ~SLICE_MUTED); break;
                case B::SliceSplit: {
                    const SampleItem* it = e.sample < lib_.count ? &lib_.items[e.sample] : nullptr;
                    if (!splitSlice(e, sliceSel_, it && it->data && it->length == e.frames ? it->data : nullptr)) { say(e.count >= kSlices ? "A SAMPLE HOLDS 32 SLICES" : "THIS SLICE IS TOO SHORT TO SPLIT"); return; }
                    break;
                }
                default:
                    if (!mergeSlice(e, sliceSel_)) return;
                    --sliceSel_;
                    break;
            }
            rememberSlices(*m, false);
            sv_.holdAudio(true);   // the sequencer and the hits read the table
            *m = e;
            sv_.holdAudio(false);
            if (bd.id != B::SliceMove) sv_.auditionSlice(m->sample, sliceSel_);
            break;
        }
        case B::ArpB: ln.arp.mode = (uint8_t)clampi(ln.arp.mode + sg, 0, kArpModeCount - 1); break;
        case B::ArpRate: ln.arp.rate = (uint8_t)clampi(ln.arp.rate + sg, 0, kArpRateCount - 1); break;
        case B::ArpOct: ln.arp.oct = (uint8_t)clampi(ln.arp.oct + sg, 1, 4); break;
        case B::ArpGate: ln.arp.gate = (uint8_t)clampi(ln.arp.gate + d, 5, 100); break;
        case B::ChordB: st.chord = (uint8_t)clampi(st.chord + sg, 0, kChordCount - 1); break;
        case B::Inv: st.inv = (uint8_t)clampi(st.inv + sg, 0, 3); break;
        case B::Voicing: st.spread = d > 0 ? 1 : 0; break;
        case B::Strum: st.strum = (uint8_t)clampi(st.strum + sg * 4, 0, 120); break;
        case B::Param: {
            const int page = page_;
            const ParamDef& p = pageDef(page).p[arg];
            if (p.unit == Unit::Sample) { pickTurn(pickFor(PickPage), sg); break; }
            const int lo = paramMin(page, arg), hi = paramMax(page, arg);
            if (hi == lo || (p.name[0] == '-' && p.name[1] == 0)) return;
            const int step = (p.list || p.unit == Unit::Sample || hi - lo <= 24) ? sg : d;
            Step* lk = lockStep();
            const uint8_t key = lockKey(page, arg);
            if (p.unit == Unit::Wave && !lk) {   // WAVE walks through the basic waves, then the banks the card has (a bank's file is loaded when it is picked)
                int16_t& v = pageValue(page, arg);
                int nv = v;
                for (int k = 0; k < (d < 0 ? -d : d); ++k) nv = stepWave(nv, d);
                v = (int16_t)nv;
                sv_.wavesChanged();
                break;
            }
            if (lk) {
                const bool lockable = !(page == PG_GLOB || p.unit == Unit::Wave || (p.name[0] == '-' && p.name[1] == 0) || (page == PG_SLC && arg != SLC_SLICE && arg != SLC_RANDOM && arg != SLC_REVRND) || (page == PG_LOOP && arg != LP_XFADE));
                if (!lockable) { say("NOT LOCKABLE"); return; }
                const int val = lk->lockValue(key, pageValue(page, arg));
                int nv = clampi(val + step, lo, hi);
                if (!isSamplerPart(part_)) {
                    Text t;
                    nv = lockedValue(pj_, part_, key, nv, *lk, t);
                    if (!t.empty()) say(t.c_str());
                }
                if (!lk->setLock(key, (int16_t)nv)) say("A STEP HOLDS 8 LOCKS");
                lockEdited_ = true;
            } else {
                int16_t& v = pageValue(page, arg);
                const int before = v;
                v = (int16_t)clampi(v + step, lo, hi);
                if (page == PG_GLOB && arg == 4) for (Part& q : pj_.parts) q.pages[PG_GLOB][4] = v;   // (the project's, shown on every part)
                if (page == PG_SMP || page == PG_LOOP) loopKnobAt_ = sv_.nowMs();
                if (page == PG_LOOP && arg == LP_AUTO && v != before && v != AUTO_OFF)   // a search measures how alike the ends are: the fade made to use that
                    pattern().lanes[curLaneIndex()].smpParam(PG_LOOP, LP_CURVE) = CURVE_MATCH;
                if (page == PG_SLC && (arg == SLC_SENS || arg == SLC_GRID)) { sliceCutFor_ = pattern().lanes[curLaneIndex()].sample(); sliceCutAt_ = sv_.nowMs(); }
                if (page == PG_SLC && arg == SLC_MODE) {
                    Lane& sl = pattern().lanes[curLaneIndex()];
                    const int si = sl.sample();
                    if (si >= 0 && si < lib_.count && v != SLICE_OFF && before == SLICE_OFF) sl.smpParam(PG_SLC, SLC_BEATS) = (int16_t)guessBeats(lib_.items[si].length, lib_.items[si].rate, pj_.bpm);   // turned on: how long is this loop?
                    refreshSlices();
                }
                if (!isSamplerPart(part_) && (page == PG_FLT1 || page == PG_FLT2) && (arg == 0 || (page == PG_FLT2 && arg == 5))) {
                    Text t;
                    if (enforceHeavy(pj_, part_, page, arg, t)) say(t.c_str());
                }
            }
            break;
        }
        case B::Prio: pj_.parts[arg].priority = (uint8_t)clampi(pj_.parts[arg].priority + sg, 0, 3); break;
        case B::AutoDeg: autoDegrade_ = d > 0; sv_.setAutoDegrade(autoDegrade_); break;
        case B::MixLevel: pj_.parts[arg].level = (uint8_t)clampi(pj_.parts[arg].level + d, 0, 127); break;
        case B::Master: pj_.master = (uint8_t)clampi(pj_.master + d, 0, 127); break;
        case B::Voices: pj_.parts[arg].voices = (uint8_t)clampi(pj_.parts[arg].voices + sg, 1, isSamplerPart(arg) ? 8 : 5); break;
        case B::Category: browseCatMove(sg); break;
        case B::SvCursor: saveCur_ = clampi(saveCur_ + sg, 0, kPresetNameMax - 1); saveArmUntil_ = 0; break;
        case B::SvChar: saveStepChar(d); break;
        case B::SvCat: saveCat_ = clampi(saveCat_ + sg, 1, kPresetCatCount - 1); saveArmUntil_ = 0; break;
        case B::SvMode:
            if (saveCanReplace()) { saveOver_ = sg > 0; saveArmUntil_ = 0; if (!saveEdited_) saveInitName(); }
            break;
        case B::SvTarget: case B::SvVoices: case B::SvPart: break;
        case B::BrowsePart: {
            part_ = clampi(part_ + sg, 0, kParts - 1);
            for (int i = 0; i < kLanes; ++i) if (pattern().lanes[i].part == part_) { sel_ = i; break; }
            browseOpen();
            break;
        }
        case B::Instrument: {
            const int pi = part_;
            const uint8_t to = d > 0 ? PT_SAMPLER : PT_CATHODE, from = pj_.parts[pi].type;
            if (to == from) return;
            const char* toName = to == PT_SAMPLER ? "SAMPLER" : "SYNTH";
            if (!shift_) {
                Text t;
                t.add("HOLD SHIFT AND TURN T4 TO MAKE PART ").add(kPartName[pi]).add(" A ").add(toName);
                say(t.c_str());
                return;
            }
            Str<40> l;
            l.add("PART ").add(kPartName[pi]).add(" MADE A ").add(toName);
            remember(U::PartType, l.c_str());
            undo_.part = (uint8_t)pi;
            undo_.i8a = (int8_t)from;
            undo_.i16b = loaded_[pi];
            undo_.i16a = pj_.parts[pi].voices;
            pj_.parts[pi].type = to;
            if (to == PT_CATHODE && pj_.parts[pi].voices > 5) pj_.parts[pi].voices = 5;
            if (to == PT_SAMPLER) loaded_[pi] = -1;
            browseOpen();
            ensurePage();
            Text t;
            t.add("PART ").add(kPartName[pi]).add(" IS NOW A ").add(toName).add(DOT "SHIFT + " RING " UNDOES");
            say(t.c_str());
            break;
        }
        case B::Scroll: browseIdx_ = clampi(browseIdx_ + sg, 0, browseCount() > 0 ? browseCount() - 1 : 0); previewBrowse(); break;
        case B::Scroll3: browseIdx_ = clampi(browseIdx_ + 3 * sg, 0, browseCount() > 0 ? browseCount() - 1 : 0); previewBrowse(); break;
        case B::FxPart:
            part_ = clampi(part_ + sg, 0, kParts - 1);
            for (int i = 0; i < kLanes; ++i) if (pattern().lanes[i].part == part_) { sel_ = i; break; }
            break;
        case B::FxParam: {
            const FxParamDef& fd = fxDef(fxView_, arg);
            const int hi = fxMax(fd);
            const int step = (fd.list || hi <= 24) ? sg : d;
            if (Step* lk = lockStep()) {   // the step's own: held for the step, then the part's value is back
                const int lkey = fxLockKey(fxView_, arg);
                if (lkey < 0) return;
                const int fp = pattern().lanes[lockLane_].part;
                const int16_t* base = fxValues(pj_.fx, fxView_, fp);
                const bool first = !lk->hasLock((uint8_t)lkey);
                const int val = lk->lockValue((uint8_t)lkey, base[arg]);
                if (!lk->setLock((uint8_t)lkey, (int16_t)clampi(val + step, 0, hi))) say("A STEP HOLDS 8 LOCKS");
                else if (first && fxView_ == FX_SENDS && !sendUsedElsewhere(arg)) say("NOTHING ELSE SENDS TO THIS EFFECT" DOT "ITS CPU IS NOT IN THE BUDGET");
                lockEdited_ = true;
                break;
            }
            int16_t* vals = fxValues(pj_.fx, fxView_, part_);
            vals[arg] = (int16_t)clampi(vals[arg] + step, 0, hi);
            break;
        }
        case B::MidiLane: midiLane_ = clampi(midiLane_ + sg, 0, kLanes - 1); break;
        case B::MidiCh: pj_.midi[midiLane_].ch = (uint8_t)clampi(pj_.midi[midiLane_].ch + sg, 0, 16); break;
        case B::MidiNote: pj_.midi[midiLane_].note = (uint8_t)clampi(pj_.midi[midiLane_].note + d, 0, 127); break;
        default: break;
    }
}

void Ui::turn(int enc, int detents) {
    if (enc < 0 || enc >= 8 || detents == 0 || exporting_) return;
    settle();
    trackerNoTap();
    clearDown_ = false;
    deleteAsked_ = -1;
    Bind b[8];
    bindings(b);
    Cell c;
    describe(b[enc], enc, c);
    if (c.dis) {
        if (mode_ == Mode::Fx && fxView_ == FX_DELAY && b[enc].arg == 1) say("TIME FOLLOWS THE TEMPO" DOT "SET MODE TO FREE TO USE IT");
        if (b[enc].id == B::SvMode) say("NOTHING TO REPLACE" DOT "THIS SOUND WAS NOT LOADED FROM THE CARD");
        if (b[enc].id == B::TrInScale) say("SET A KEY AND SCALE ON THE PROJECT SCREEN FIRST");
        if (mode_ == Mode::Fx && lockOn_) {
            if (b[enc].id == B::FxPart) say("A STEP LOCK IS FOR THE LANE'S OWN PART");
            else if (b[enc].id == B::FxParam && fxLockKey(fxView_, b[enc].arg) < 0) say("ONLY SENDS AND AMP KNOBS CAN BE LOCKED" DOT "THE REST IS SHARED");
            else if (b[enc].id == B::FxParam) say("THE AMP IS OFF" DOT "SET ITS MODEL FIRST");
        }
        return;
    }
    if (c.ro) {
        Text t;
        if (c.value == "IN PROJECT") t.add("MIDI CHANNELS ARE PER LANE: PROJECT > MIDI");   // the part's old MIDI CH
        else t.add(c.label.c_str()).add(": INFORMATION ONLY, NOT A SETTING");
        say(t.c_str());
        return;
    }
    if (mode_ == Mode::Save) saveTouched_ = true;
    const int coarse = shift_ ? 6 : 1;
    // hold + turn: edits the held step, never toggles it; an open lock moves to it first, so the turn goes into the held step's lock
    if (held_.on && (mode_ == Mode::Step || ((mode_ == Mode::Sound || mode_ == Mode::Fx) && lockOn_))) {
        if (!held_.edited) {
            cursor_ = held_.idx;
            if (lockOn_) { lockPat_ = tp_.pat; lockLane_ = sel_; lockIdx_ = held_.idx; }
        }
        held_.edited = true;
    }
    if (tracker()) trackerTouch(b[enc].id);
    apply(b[enc], detents * coarse);
    dirty();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// navigation

void Ui::recKey() {
    sv_.recStatus(rec_);
    if (rec_.state == REC_ARMED) { sv_.recStop(); say("NOT WAITING ANY MORE"); return; }
    if (rec_.state == REC_TAKING) { sv_.recStop(); return; }
    if (rec_.state == REC_DONE) {   // AUTO STOP may have ended it unnoticed: the press that was meant to stop it must not lose it
        if (!recDropAsked_) { recDropAsked_ = true; say("REC AGAIN DROPS THE TAKE" DOT "OK KEEPS IT"); return; }
        sv_.recDiscard();
    }
    recDropAsked_ = false;
    if (lib_.count >= kMaxSamples) {   // at REC, not at OK: a take that cannot be kept is not recorded at all
        Text t;
        t.add("THE SAMPLE LIST IS FULL: ").num(kMaxSamples).add(" SAMPLES" DOT "NO ROOM FOR A TAKE");
        say(t.c_str());
        return;
    }
    if (!sv_.recStart(recSet_)) { say(sv_.recNoRoom()); return; }
    recSeen_ = recSet_.autoCapture ? REC_ARMED : REC_TAKING;
    say(recSet_.autoCapture ? "WAITING FOR SOUND" : "RECORDING");
}

void Ui::recKeepTake(bool toLane) {
    sv_.recStatus(rec_);
    if (rec_.state != REC_DONE) return;
    recDropAsked_ = false;
    const int n = sv_.recKeep();
    Text t;
    if (n < 0) { say("THE TAKE COULD NOT BE KEPT"); return; }
    const int err = sv_.recSaveError();
    t.add(lib_.items[n].name);
    const int li = curLaneIndex();
    if (toLane && isSamplerPart(pattern().lanes[li].part)) {
        setLaneSample(li, n, false);
        t.add(" > LANE ").num(li + 1);
    } else {
        t.add(" IS IN FILE, REC");
    }
    if (err < 0) {
        const char* why = sv_.cardProblem();
        t.add(DOT).add(why ? why : "NO CARD").add(": NOT SAVED");
    }
    else if (err > 0) t.add(DOT "CARD ERROR ").num(err).add(": NOT SAVED");
    say(t.c_str());
}

void Ui::recLeave() {
    sv_.recStatus(rec_);
    if (rec_.state == REC_TAKING || rec_.state == REC_ARMED) { sv_.recStop(); sv_.recStatus(rec_); }
    if (rec_.state == REC_DONE) recKeepTake(false);   // a take is never lost by walking off
    sv_.recListen(recSet_, false);
}

void Ui::setMode(Mode m, int page) {
    if (mode_ == Mode::Record && m != Mode::Record) recLeave();
    if (m != Mode::Sound && m != Mode::Step && m != Mode::Fx) lockOn_ = false;
    const bool same = m == mode_ && (page < 0 || page == page_);
    if (!same && !(mode_ == Mode::Sound && m == Mode::Sound)) { backMode_ = mode_; backPage_ = page_; }
    if (m == Mode::Play) { backMode_ = Mode::Play; backPage_ = page_; }
    mode_ = m;
    if (page >= 0) page_ = page;
    dirty();
}

void Ui::goBack() {
    if (mode_ == Mode::Play) { say("ALREADY ON THE MAIN SCREEN"); return; }
    Mode bm = Mode::Play;
    int bp = page_;
    if (backMode_ != mode_) { bm = backMode_; bp = backPage_; }
    if (bm != Mode::Sound && bm != Mode::Step && bm != Mode::Fx) lockOn_ = false;
    if (mode_ == Mode::Record && bm != Mode::Record) recLeave();
    mode_ = bm;
    page_ = bp;
    backMode_ = Mode::Play;
    backPage_ = page_;
    if (mode_ == Mode::Record) {   // back on the REC screen: its input was closed on the way out
        sv_.recListen(recSet_, true);
        sv_.recStatus(rec_);
    }
    dirty();
}

void Ui::selectLane(int i) {
    if (lockOn_ && lockLane_ != i) lockOn_ = false;
    sel_ = i;
    part_ = lane().part;
    fixPage();
    dirty();
}

void Ui::fixPage() {
    const Lane& l = lane();
    const int pages = (l.n + 7) / 8 > 1 ? (l.n + 7) / 8 : 1;
    if (stepPage_ > pages - 1) stepPage_ = pages - 1;
    if (cursor_ > l.n - 1) cursor_ = l.n - 1;
}

void Ui::ensurePage() {
    if (mode_ != Mode::Sound || pageExists(page_)) return;
    if (isSamplerPart(part_) && !partHasLane(part_)) page_ = PG_GLOB;
    else if (isSamplerPart(part_)) {
        switch (page_) {
            case PG_OSC1: case PG_OSC2: case PG_SUB: page_ = PG_SMP; break;
            case PG_FLT1: case PG_FLT2: page_ = PG_SFLT; break;
            default: page_ = PG_SAMP; break;
        }
    } else {
        switch (page_) {
            case PG_SMP: page_ = PG_OSC1; break;
            case PG_SFLT: page_ = PG_FLT1; break;
            default: page_ = PG_AMP; break;
        }
    }
}

void Ui::selectPattern(int p) {
    p = clampi(p, 0, kPatterns - 1);
    if (tp_.songPlaying) { say("THE SONG CHOOSES THE PATTERN" DOT "STOP IT FIRST"); return; }
    lockOn_ = false;
    if (tp_.playing) {
        tp_.nextPat = (int8_t)(p == tp_.pat ? -1 : p);
        Text t;
        if (tp_.nextPat < 0) t.add("STAYING ON P").num(tp_.pat + 1);
        else t.ch('P').num(p + 1).add(" NEXT" DOT "AT THE END OF P").num(tp_.pat + 1);
        say(t.c_str());
    } else {
        tp_.pat = (uint8_t)p;
        tp_.nextPat = -1;
        tp_.pstep = 0;
    }
    fixPage();
    dirty();
}

bool Ui::listMove(int dir) {
    if (mode_ == Mode::Song) { songRow_ = clampi(songRow_ + dir, 0, song().nRows > 0 ? song().nRows - 1 : 0); dirty(); return true; }
    if (mode_ == Mode::Fx && lockOn_) { fxView_ = fxView_ == FX_SENDS ? FX_AMP : FX_SENDS; dirty(); return true; }   // a lock: only SENDS and AMP
    if (mode_ == Mode::Fx) { fxView_ = clampi(fxView_ + dir, 0, FX_VIEWS - 1); dirty(); return true; }
    if (mode_ == Mode::Browse) {
        const int n = browseCount();
        browseIdx_ = clampi(browseIdx_ + dir, 0, n > 0 ? n - 1 : 0);
        dirty();
        previewBrowse();
        return true;
    }
    if (mode_ == Mode::ProjectM && projSub_ == 1) { midiLane_ = clampi(midiLane_ + dir, 0, kLanes - 1); dirty(); return true; }
    if (mode_ == Mode::ProjectM) { slot_ = clampi(slot_ + dir, -kFactoryProjects, kCardSlots - 1); dirty(); return true; }
    return false;
}

// FILE opens where the sample knobs are: the folder of the lane's sample (or the one its folder knob went to), on that sample
void Ui::browseOpen() {
    browseSmp_ = isSamplerPart(part_);
    browseCat_ = 0;
    browseIdx_ = 0;
    if (!browseSmp_ || !partHasLane(part_) || !lib_.count) return;
    Pick p;
    p.lane = curLaneIndex();
    browseCat_ = pickFolder(p);
    browseIdx_ = lib_.catPlace(pickMoved(p) ? pickShown(p) : pickNow(p));
}

void Ui::browseCatMove(int dir) {
    if (!isSamplerPart(part_)) {
        browseCat_ = clampi(browseCat_ + dir, 0, browseCatCount() - 1);
        browseIdx_ = 0;
        return;
    }
    browseCat_ = lib_.catStep(browseCat_, dir, true);   // (a folder with no samples is passed over; ALL stays, here)
    browseIdx_ = 0;
    if (!partHasLane(part_) || !lib_.count) return;
    Pick p;
    p.lane = curLaneIndex();
    const int now = pickNow(p);
    if (browseCat_ == 0) { laneFolder_[p.lane] = 0; browseIdx_ = now; return; }
    laneFolder_[p.lane] = lib_.items[now].cat == browseCat_ ? 0 : (uint8_t)browseCat_;   // the same memory as the sample knobs' folder knob
    browseIdx_ = lib_.catPlace(pickMoved(p) ? pickShown(p) : now);
}

void Ui::browseCatStep(int dir) {
    browseCatMove(dir);
    Text t;
    t.add("CATEGORY: ").add(browseCatName(browseCat_));
    say(t.c_str());
}

void Ui::previewBrowse() {
    const int it = browseItem(browseIdx_);
    if (it < 0) return;
    if (isSamplerPart(part_)) { catLast_[lib_.items[it].cat] = (int16_t)it; auditionSoon(it); return; }
    // a synth preset: three notes on a copy of the pages, after the knob settles (the part is not changed until OK)
    previewPending_ = it;
    previewAt_ = sv_.nowMs() + kPreviewDelayMs;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// transport

void Ui::start() {
    if (tp_.playing) return;
    if (tp_.clockExt) say(pj_.parts[0].pages[PG_GLOB][4] == 1 ? "WAITING FOR THE MIDI CLOCK" : "WAITING FOR THE SYNC INPUT");
    tp_.pstep = 0;
    if (mode_ == Mode::Song) {   // PLAY on the SONG screen plays the song from the selected row
        Song& so = song();
        if (!so.nRows) { say("THIS SONG IS EMPTY" DOT DISC " ADDS A ROW"); return; }
        const int r = clampi(songRow_, 0, so.nRows - 1);
        tp_.playRow = (uint8_t)r;
        tp_.repLeft = so.rows[r].rep;
        tp_.jumpTo = -1;
        tp_.pat = so.rows[r].pat;
        tp_.nextPat = -1;
        tp_.songPlaying = 1;
        lockOn_ = false;
    } else {
        tp_.songPlaying = 0;
    }
    tp_.gstep = -1;
    tp_.shownStep = -1;
    tp_.playGen = tp_.playGen + 1;
    tp_.playing = 1;   // last: the sequencer starts on this
    dirty();
}

void Ui::stop() {
    tp_.playing = 0;
    tp_.gstep = -1;
    tp_.pstep = 0;
    tp_.nextPat = -1;
    tp_.songPlaying = 0;
    tp_.jumpTo = -1;
    dirty();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// keys

void Ui::keyDown(Key k) {
    if (exporting_) {                         // the whole instrument is busy: STOP, or wait
        if (k == Key::Stop) sv_.exportCancel();
        return;
    }
    settle();
    trackerNoTap();
    clearDown_ = false;
    if (k != Key::Clear) deleteAsked_ = -1;
    if (k == Key::Lane) { laneDown_ = true; laneDownAt_ = sv_.nowMs(); }
    switch (k) {
        case Key::Shift: shift_ = true; dirty(); return;
        case Key::Fill: tp_.fill = 1; say("FILL"); return;   // momentary: a fill while held
        case Key::Rec:
            if (!sv_.recAvailable()) { say("RECORDING IS ON THE DRUMBOY ITSELF"); return; }
            if (mode_ != Mode::Record) { setMode(Mode::Record); sv_.recListen(recSet_, true); sv_.recStatus(rec_); say("REC STARTS, OK KEEPS, " RING " DROPS"); return; }
            recKey();
            return;
        case Key::Count: return;
        default: pressKey(k); return;
    }
}

void Ui::keyUp(Key k) {
    if (k == Key::Lane) laneDown_ = false;
    if (k == Key::Ok) trackerOkUp();
    if (k == Key::Clear) clearDown_ = false;
    if (k == Key::Shift) { shift_ = false; dirty(); }
    if (k == Key::Fill && tp_.fill) { tp_.fill = 0; dirty(); }
}

void Ui::pressKey(Key k) {
    Lane& ln = lane();
    if (mode_ == Mode::Record && (k == Key::Add || k == Key::Mute)) return;   // they act on the lane: nothing here should change the project by a slip of the finger
    if (mode_ == Mode::Save) {   // SAVE PRESET: the arrows edit the name, OK saves, L cancels; nothing else reaches the project from here
        if (k != Key::Ok) saveTouched_ = true;
        switch (k) {
            case Key::Left: saveCur_ = clampi(saveCur_ - 1, 0, kPresetNameMax - 1); saveArmUntil_ = 0; dirty(); return;
            case Key::Right: saveCur_ = clampi(saveCur_ + 1, 0, kPresetNameMax - 1); saveArmUntil_ = 0; dirty(); return;
            case Key::Up: saveStepChar(1); return;
            case Key::Down: saveStepChar(-1); return;
            case Key::Ok: saveCommit(); return;
            case Key::Lane: say("PRESET NOT SAVED"); break;   // (goes back below)
            case Key::Play: case Key::Stop: break;
            default: return;
        }
    }
    if (tracker() && trackerKey(k)) return;
    switch (k) {
        case Key::Play: {
            if (tp_.playing) { stop(); break; }
            start();
            bool any = false;
            for (const Lane& l : pj_.patterns[tp_.pat].lanes) any = any || laneHasContent(l);
            if (!any && !tp_.songPlaying) say("NOTHING TO PLAY YET" DOT DISC " WRITES A RHYTHM" DOT "I HAS THE FACTORY PROJECTS");
            break;
        }
        case Key::Stop: stop(); tp_.shownStep = -1; dirty(); break;
        case Key::StepK:
            if (mode_ == Mode::Step) {
                stepSub_ = (stepSub_ + 1) % 3;
                if (stepSub_ == 2) trackerEnter();
                say(stepSub_ == 2 ? "STEP 3/3: TRACKER" : stepSub_ ? (isSamplerPart(ln.part) ? "STEP 2/3: SAMPLE PER STEP" : "STEP 2/3: CHORD AND ARP") : "STEP 1/3: NOTE DATA");
            } else setMode(Mode::Step);
            break;
        case Key::Lane: if (shift_ || mode_ == Mode::Play) setMode(Mode::Play); else goBack(); break;
        case Key::File:
            if (mode_ == Mode::Browse) goBack();
            else { browseOpen(); setMode(Mode::Browse); }
            break;
        case Key::Mixer:
            if (mode_ == Mode::Mixer) {
                static const char* const kViews[4] = {"MIXER 1/4: LEVELS", "MIXER 2/4: VOICES", "MIXER 3/4: PRIORITY", "MIXER 4/4: CPU AND DEGRADE"};
                mixSub_ = (mixSub_ + 1) % 4;
                say(kViews[mixSub_]);
            }
            else setMode(Mode::Mixer);
            break;
        case Key::Info:
            if (mode_ == Mode::ProjectM) {
                static const char* const kViews[3] = {"PROJECT 1/3: KEY AND CARD", "PROJECT 2/3: MIDI", "PROJECT 3/3: EXPORT"};
                projSub_ = (projSub_ + 1) % 3;
                say(kViews[projSub_]);
            }
            else setMode(Mode::ProjectM);
            break;
        case Key::SongK: if (mode_ == Mode::Song) goBack(); else setMode(Mode::Song); break;
        case Key::Spare:   // the bottom-left key: the effects. Again: the next of its six views
            if (lockOn_) {   // a step's lock: the views of the lane's own part, SENDS and AMP
                if (mode_ != Mode::Fx) setMode(Mode::Fx);
                else fxView_ = fxView_ == FX_SENDS ? FX_AMP : FX_SENDS;
                if (fxView_ != FX_SENDS && fxView_ != FX_AMP) fxView_ = FX_SENDS;
                part_ = pattern().lanes[lockLane_].part;
            } else if (mode_ == Mode::Fx) fxView_ = (fxView_ + 1) % FX_VIEWS;
            else setMode(Mode::Fx);
            { Text t; t.add("FX ").num(fxView_ + 1).add("/").num(FX_VIEWS).add(": ").add(fxViewName(fxView_)); say(t.c_str()); }
            dirty();
            break;
        case Key::PartK: {
            if (lockOn_ && mode_ == Mode::Fx) { say("A STEP LOCK IS FOR THE LANE'S OWN PART"); break; }
            part_ = (part_ + 1) % kParts;
            for (int i = 0; i < kLanes; ++i) if (pattern().lanes[i].part == part_) { sel_ = i; break; }
            fixPage();
            Text t;
            t.add("PART ").add(kPartName[part_]);
            say(t.c_str());
            break;
        }
        case Key::Osc1: case Key::Osc2: case Key::Sub: case Key::Flt1: case Key::Flt2: case Key::Grit: case Key::Amp: case Key::ModK:
        case Key::Glob: {
            static const struct { Key k; int8_t synth, sampler; const char* name; } kSoundKeys[] = {
                {Key::Osc1, PG_OSC1, PG_SMP, "OSC 1"}, {Key::Osc2, PG_OSC2, PG_SLC, "OSC 2"}, {Key::Sub, PG_SUB, PG_LOOP, "SUB"},
                {Key::Flt1, PG_FLT1, PG_SFLT, "FLT 1"}, {Key::Flt2, PG_FLT2, -1, "FLT 2"}, {Key::Grit, PG_GRIT, PG_GRIT, "GRIT"},
                {Key::Amp, PG_AMP, PG_SAMP, "AMP"}, {Key::ModK, PG_MOD, PG_MOD, "MOD"}, {Key::Glob, PG_GLOB, PG_GLOB, "GLOBAL"}};
            for (const auto& sk : kSoundKeys) {
                if (sk.k != k) continue;
                const int target = isSamplerPart(part_) ? sk.sampler : sk.synth;
                if (target < 0) {
                    Text t;
                    t.add(sk.name).add(": NOT ON A SAMPLER PART");
                    say(t.c_str());
                    break;
                }
                if (isSamplerPart(part_) && target != PG_GLOB && !partHasLane(part_)) { say("NO LANE PLAYS THIS PART: GIVE IT ONE WITH PLAYED BY"); break; }
                if (target == PG_SLC && mode_ == Mode::Sound && page_ == PG_SLC && !sliceEdit_ && !lockOn_) {   // the key again: the slices one by one
                    sliceEdit_ = true;
                    say("SLICE EDIT: ONE SLICE AT A TIME");
                    break;
                }
                sliceEdit_ = false;
                if (mode_ == Mode::Sound && page_ == target) { goBack(); break; }   // pressing the page you are on goes back
                setMode(Mode::Sound, target);
                break;
            }
            break;
        }
        // on a screen with a list (SONG rows, BROWSE, PROJECT card slots) up/down move through the list; elsewhere they pick a lane
        case Key::Up: if (!listMove(-1)) selectLane((sel_ + 9) % kLanes); break;
        case Key::Down: if (!listMove(1)) selectLane((sel_ + 1) % kLanes); break;
        case Key::Left:
            if (mode_ == Mode::Browse) { browseCatStep(-1); break; }
            if (mode_ == Mode::Step) { cursor_ = (cursor_ + ln.n - 1) % ln.n; stepPage_ = cursor_ / 8; dirty(); }
            else if (mode_ == Mode::Play) selectLane((sel_ + 9) % kLanes);
            break;
        case Key::Right:
            if (mode_ == Mode::Browse) { browseCatStep(1); break; }
            if (mode_ == Mode::Step) { cursor_ = (cursor_ + 1) % ln.n; stepPage_ = cursor_ / 8; dirty(); }
            else if (mode_ == Mode::Play) selectLane((sel_ + 1) % kLanes);
            break;
        case Key::Ok: okKey(); break;
        case Key::Add: addKey(); break;
        case Key::Mute: {
            ln.mute = !ln.mute;
            Text t;
            t.add("LANE ").num(sel_ + 1).add(ln.mute ? " MUTED" : " ON");
            say(t.c_str());
            break;
        }
        case Key::Copy: copyKey(); break;
        case Key::Paste: pasteKey(); break;
        case Key::Clear: clearKey(); break;
        default: break;
    }
}

void Ui::debugSetVoices(int part, int n) {
    if (part < 0 || part >= kParts || isSampler(pj_, part)) return;
    pj_.parts[part].voices = (uint8_t)clampi(n, 1, 8);
    dirty();
}

bool Ui::debugLoadPreset(int part, int id) {
    if (part < 0 || part >= kParts || isSampler(pj_, part)) return false;
    for (int l = 0; l < kLanes; ++l)
        if (pattern().lanes[l].part == part) { selectLane(l); break; }
    part_ = part;   // (a part no lane plays: selecting a lane did not get there)
    if (mode_ == Mode::Record) recLeave();
    mode_ = Mode::Browse;
    browseCat_ = 0;
    settle();
    int row = -1;
    for (int k = 0, n = browseCount(); k < n; ++k)
        if (browseItem(k) == id) { row = k; break; }
    if (row < 0) return false;
    browseIdx_ = row;
    okKey();
    previewPending_ = -1;   // selecting a row in BROWSE auditions it (three notes at once): not wanted when a script loads presets
    return true;
}

void Ui::okKey() {
    if (mode_ == Mode::Record) { recKeepTake(true); return; }
    Lane& ln = lane();
    if (mode_ == Mode::Fx) return;
    if (mode_ == Mode::Save) { saveCommit(); return; }
    if (shift_ && (mode_ == Mode::Browse || mode_ == Mode::Sound)) { openSave(); return; }
    if (mode_ == Mode::ProjectM && projSub_ == 2) { exportSong(); return; }
    if (mode_ == Mode::ProjectM && projSub_ == 1) { say("LOAD AND SAVE ARE IN PROJECT 1/3: PRESS INFO"); return; }   // the MIDI view
    if (mode_ == Mode::ProjectM) { if (shift_) saveSlot(); else loadSlot(); dirty(); return; }
    if (mode_ == Mode::Song) {   // play from this row (at the end of the current pattern if a song is playing)
        if (!song().nRows) return;
        if (tp_.songPlaying) {
            tp_.jumpTo = (int8_t)songRow_;
            Text t;
            t.add("ROW ").num(songRow_ + 1).add(" NEXT");
            say(t.c_str());
        } else pressKey(Key::Play);
        dirty();
        return;
    }
    if (mode_ == Mode::Step) { ln.st[cursor_].ov = !ln.st[cursor_].ov; dirty(); return; }
    if (mode_ != Mode::Browse) return;
    const int it = browseItem(browseIdx_);
    if (it < 0) return;
    Text t;
    if (isSamplerPart(part_)) {
        if (!partHasLane(part_)) { say("NO LANE PLAYS THIS PART: GIVE IT ONE WITH PLAYED BY"); return; }
        const int li = curLaneIndex();
        setLaneSample(li, it, false);
        t.add(lib_.items[it].name).add(" > LANE ").num(li + 1);
    } else {
        const int pi = part_;
        if (!presetLoad(it)) { say("PRESET FILE CANNOT BE READ"); return; }   // from the card: nothing has changed
        Str<40> lab;
        lab.add("PRESET ON PART ").add(kPartName[pi]);
        remember(U::PartPages, lab.c_str());
        undo_.part = (uint8_t)pi;
        memcpy(undoPages_, pj_.parts[pi].pages, sizeof(undoPages_));
        undo_.i16a = pj_.parts[pi].preset;
        undo_.i16b = loaded_[pi];
        undo_.idx = pj_.parts[pi].voices;
        undoFxAll_ = pj_.fx;
        FxSettings nf = pj_.fx;
        applyPresetFx(nf, pi, it);         // worked out outside the hold: the audio is held only for the copy
        postFx_ = nf;
        sv_.holdAudio(true);               // the sends and the amp change together (the audio reads them every block)
        pj_.fx = nf;
        sv_.holdAudio(false);
        applyPresetIndex(pj_.parts[pi].pages, it);
        bool trimmed = false;
        if (presetVoices(it) > 0) {
            pj_.parts[pi].voices = (uint8_t)presetVoices(it);   // the voices the preset was made for, as many as it and its effects fit in the CPU budget
            trimmed = fitVoicesToBudget(pj_, pi) < presetVoices(it);
        }
        pj_.parts[pi].preset = (int16_t)it;
        loaded_[pi] = (int16_t)it;
        sv_.wavesChanged();   // a bank it plays is loaded now, not at the next sweep
        t.add(presetName(it)).add(" > PART ").add(kPartName[pi]);
        if (trimmed) t.add(DOT "VOICES ").num(pj_.parts[pi].voices).add(" (THE EFFECTS NEED CPU)");
        t.add(DOT "SHIFT + " RING " UNDOES");
    }
    say(t.c_str());
    goBack();   // loading returns you to where you came from
}

void Ui::addKey() {
    if (mode_ == Mode::Fx) return;
    Lane& ln = lane();
    if (mode_ == Mode::Song) {   // ADD adds a row after the selected one: the same pattern, or the one on screen
        Song& so = song();
        if (so.nRows >= kSongRows) { say("A SONG HOLDS 64 ROWS"); return; }
        const int at = so.nRows ? songRow_ + 1 : 0;
        SongRow row = so.nRows ? so.rows[songRow_] : SongRow{tp_.pat, 1};
        for (int i = so.nRows; i > at; --i) so.rows[i] = so.rows[i - 1];
        so.rows[at] = row;
        ++so.nRows;
        songRow_ = at;
        Text t;
        t.add("ROW ").num(at + 1).add(" ADDED");
        say(t.c_str());
        return;
    }
    auto rnd = [&]() { return sv_.random() / 4294967296.0; };
    ln.eu = 1;
    const int span = ln.n - 1 < 9 ? ln.n - 1 : 9;
    ln.k = (uint8_t)(1 + (int)(rnd() * (span > 1 ? span : 1)));
    ln.r = (uint8_t)(int)(rnd() * ln.n);
    Text t;
    t.add("EUCLID ").num(ln.k).add(" / ").num(ln.n).add("  r").num(ln.r);
    say(t.c_str());
}

void Ui::copyKey() {
    if (mode_ == Mode::Fx) return;
    Text t;
    if (mode_ == Mode::Song) {
        clipSong_ = song();
        hasClipSong_ = true;
        t.add("SONG ").num(tp_.songIdx + 1).add(" COPIED");
    } else if (shift_) {   // SHIFT + C: the whole pattern
        clipPat_ = pattern();
        hasClipPat_ = true;
        t.add("PATTERN P").num(tp_.pat + 1).add(" COPIED");
    } else {
        clip_ = lane();
        hasClip_ = true;
        t.add("LANE ").num(sel_ + 1).add(" COPIED");
    }
    say(t.c_str());
}

void Ui::pasteKey() {
    if (mode_ == Mode::Fx) return;
    Text t;
    Str<40> lab;
    if (mode_ == Mode::Song) {
        if (!hasClipSong_) { say("NO SONG COPIED"); return; }
        const int i = tp_.songIdx;
        lab.add("PASTE ON SONG ").num(i + 1);
        remember(U::SongSnap, lab.c_str());
        undo_.idx = (uint8_t)i;
        undoSong_ = pj_.songs[i];
        pj_.songs[i] = clipSong_;
        songRow_ = 0;
        t.add("PASTED TO SONG ").num(i + 1).add(DOT "SHIFT + " RING " UNDOES");
        say(t.c_str());
        return;
    }
    if (shift_) {   // SHIFT + P: the whole pattern (also into another project)
        if (!hasClipPat_) { say("NO PATTERN COPIED" DOT "SHIFT + C COPIES ONE"); return; }
        const int i = tp_.pat;
        lab.add("PASTE ON P").num(i + 1);
        remember(U::PatternSnap, lab.c_str());
        undo_.pat = (uint8_t)i;
        undoPattern_ = pj_.patterns[i];
        pj_.patterns[i] = clipPat_;
        t.add("PASTED TO P").num(i + 1).add(DOT "SHIFT + " RING " UNDOES");
        say(t.c_str());
        return;
    }
    if (!hasClip_) { say("NOTHING COPIED"); return; }
    Lane& ln = lane();
    const uint8_t keep = ln.part;
    lab.add("PASTE ON LANE ").num(sel_ + 1);
    remember(U::LaneSnap, lab.c_str());
    undo_.pat = tp_.pat;
    undo_.lane = (uint8_t)sel_;
    undoLane_ = ln;
    ln = clip_;
    ln.part = keep;
    t.add("PASTED TO LANE ").num(sel_ + 1).add(DOT "SHIFT + " RING " UNDOES");
    say(t.c_str());
}

void Ui::clearKey() {
    if (mode_ == Mode::Record && !shift_) {
        sv_.recStatus(rec_);
        if (rec_.state == REC_DONE) { recDropAsked_ = false; sv_.recDiscard(); say("TAKE DROPPED"); }
        return;
    }
    if (shift_) { undo(); return; }   // SHIFT + CLEAR = undo
    if (mode_ == Mode::ProjectM) { deleteSlot(); return; }
    Text t;
    if (mode_ == Mode::Fx && !lockOn_) {          // CLEAR puts the view's settings (the part's, for AMP and SENDS) back to their defaults (undoable)
        FxSettings def;
        defaultFx(def);
        const int n = fxView_ == FX_SENDS ? kSendsUsed : kFxParams;
        int16_t* v = fxValues(pj_.fx, fxView_, part_);
        const int16_t* d = fxValues(def, fxView_, part_);
        bool same = true;
        for (int i = 0; i < n; ++i) same = same && v[i] == d[i];
        if (same) { say("ALREADY AT THE DEFAULTS"); return; }
        Str<40> lab;
        lab.add(fxViewName(fxView_)).add(" RESET");
        remember(U::FxSnap, lab.c_str());
        undo_.idx = (uint8_t)fxView_;      // only what the reset replaced comes back: edits made after it stay
        undo_.part = (uint8_t)part_;
        for (int i = 0; i < n; ++i) undoFxVals_[i] = v[i];   // (n: SENDS has only kSends values per part)
        sv_.holdAudio(true);               // the effects read these in the audio interrupt: all eight change together
        for (int i = 0; i < n; ++i) v[i] = d[i];
        sv_.holdAudio(false);
        t.add(fxViewName(fxView_));
        if (fxView_ == FX_AMP || fxView_ == FX_SENDS) t.add(" PART ").add(kPartName[part_]);
        t.add(" BACK TO DEFAULTS" DOT "SHIFT + " RING " UNDOES");
        say(t.c_str());
        return;
    }
    if (mode_ == Mode::Song) {        // CLEAR removes the selected row (undoable)
        Song& so = song();
        if (!so.nRows) return;
        const int at = songRow_;
        const SongRow gone = so.rows[at];
        Str<40> lab;
        lab.add("ROW ").num(at + 1).add(" REMOVED");
        remember(U::SongRow, lab.c_str());
        undo_.idx = tp_.songIdx;
        undo_.lane = (uint8_t)at;
        undo_.row = gone;
        undo_.i8a = 0;
        // the rows and the playing position change together: the sequencer moves both at a pattern end
        sv_.holdAudio(true);
        for (int i = at; i + 1 < so.nRows; ++i) so.rows[i] = so.rows[i + 1];
        --so.nRows;
        if (tp_.songPlaying && tp_.songIdx == undo_.idx) {   // the song playing: it goes on where it was
            const int jump = tp_.jumpTo;
            if (jump == at) tp_.jumpTo = -1;                        // the row it was asked to jump to is gone
            else if (jump > at) tp_.jumpTo = (int8_t)(jump - 1);
            if (at < tp_.playRow) tp_.playRow = (uint8_t)(tp_.playRow - 1);
            else if (at == tp_.playRow && tp_.jumpTo < 0 && at < so.nRows) {   // the row playing: the one after it moved up into its place
                tp_.jumpTo = (int8_t)at;
                undo_.i8a = 1;
            }
            if (so.nRows == 0) tp_.songPlaying = 0;
        }
        sv_.holdAudio(false);
        songRow_ = clampi(at, 0, so.nRows > 0 ? so.nRows - 1 : 0);
        t.add("ROW ").num(at + 1).add(" REMOVED" DOT "SHIFT + " RING " UNDOES");
        say(t.c_str());
        return;
    }
    if (lockOn_) {
        Step* s = lockStep();
        const int n = s->nLocks;
        s->clearLocks();
        if (n) t.add("STEP ").num(lockIdx_ + 1).add(": ").num(n).add(n > 1 ? " LOCKS CLEARED" : " LOCK CLEARED");
        else t.add("NO LOCKS ON THIS STEP");
        say(t.c_str());
        return;
    }
    if (tracker()) { trackerClear(); return; }
    Lane& ln = lane();
    const uint32_t now = sv_.nowMs();
    clearDown_ = (mode_ == Mode::Play || mode_ == Mode::Step) && !held_.on && !laneDown_;   // the hold: where the lanes are on show, with no other key down
    clearDownAt_ = now;
    clearFrom_.kind = U::None;
    clearFrom_.pat = (uint8_t)tp_.pat;
    if (!laneHasContent(ln)) {   // nothing to take, so the undo stays: after a hold it is the whole pattern
        t.add("LANE ").num(sel_ + 1).add(" IS EMPTY");
        say(t.c_str());
        return;
    }
    const bool sameLane = undo_.kind == U::LaneClear && undo_.pat == tp_.pat && undo_.lane == sel_;
    const bool second = hasClearPress_ && now - lastClear_ < kDoubleClearMs;
    // one undo must bring back what was there before the FIRST press of a double press
    if (!(second && sameLane)) {
        undo_.kind = U::LaneClear;
        undo_.pat = tp_.pat;
        undo_.lane = (uint8_t)sel_;
        undo_.eu = ln.eu;
        for (int i = 0; i < kMaxSteps; ++i) undo_.ov[i] = ln.st[i].ov;
    }
    undo_.label.clear();
    if (second) {
        undo_.label.add("LANE ").num(sel_ + 1).add(" EMPTIED");
        ln.eu = 0;
        for (Step& s : ln.st) s.ov = 0;
        t.add("LANE ").num(sel_ + 1).add(" EMPTY" DOT "SHIFT + " RING " UNDOES");
    } else {
        undo_.label.add("EDITS CLEARED");
        for (Step& s : ln.st) s.ov = 0;
        t.add("EDITS CLEARED" DOT "CLEAR AGAIN EMPTIES THE LANE");
    }
    lastClear_ = now;
    hasClearPress_ = true;
    clearFrom_ = undo_;
    say(t.c_str());
}

// CLEAR held: every lane loses its rhythm, its tapped steps and whatever the steps held. The lanes keep their sounds, levels and mutes.
void Ui::emptyPattern() {
    if (clearFrom_.pat != tp_.pat) { say("THE PATTERN CHANGED" DOT "NOTHING EMPTIED"); return; }
    Pattern& p = pattern();
    bool any = clearFrom_.kind == U::LaneClear;
    for (const Lane& l : p.lanes) any = any || l.k || laneHasContent(l);
    Text t;
    t.add("P").num(tp_.pat + 1);
    if (!any) {   // the undo stays: a second hold must not lose what the first one took
        t.add(" IS EMPTY");
        say(t.c_str());
        return;
    }
    Str<40> lab;
    lab.add("P").num(tp_.pat + 1).add(" EMPTIED");
    remember(U::PatternSnap, lab.c_str());
    undo_.pat = (uint8_t)tp_.pat;
    undoPattern_ = p;
    if (clearFrom_.kind == U::LaneClear) {   // the press itself cleared this lane: one undo must bring back what was there before it
        Lane& pressed = undoPattern_.lanes[clearFrom_.lane];
        pressed.eu = clearFrom_.eu;
        for (int i = 0; i < kMaxSteps; ++i) pressed.st[i].ov = clearFrom_.ov[i];
    }
    for (Lane& l : p.lanes) emptyLane(l);
    t.add(" EMPTY" DOT "SHIFT + " RING " UNDOES");
    say(t.c_str());
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the card

void Ui::loadSlot() {
    const int i = slot_;
    SlotInfo si;
    Text t;
    if (i >= 0 && !slotInfo(i, si)) {
        t.add("SLOT ").num(i + 1).add(" IS EMPTY" DOT "SHIFT + OK SAVES HERE");
        say(t.c_str());
        return;
    }
    if (tp_.playing) stop();
    if (!undoProject_) undoProject_ = new Project;
    if (!undoProject_) { say("NO MEMORY FOR UNDO" DOT "NOT LOADED"); return; }
    int missing = 0;
    if (i < 0) {                              // (the firmware's own: nothing can go wrong with it)
        *undoProject_ = pj_;
        Str<40> lab;
        lab.add("LOAD OF ").add(factoryProjectName(i + kFactoryProjects));
        remember(U::ProjectSnap, lab.c_str());
        undo_.i8a = (int8_t)loadedSlot_;
        sv_.loadFactory(i + kFactoryProjects, pj_);
        loadedSlot_ = i;
        loaded();
        t.add(pj_.name).add(" LOADED" DOT "SHIFT + " RING " UNDOES");
        say(t.c_str());
        return;
    }
    // read first: a slot that cannot be read costs neither the project in memory nor the undo waiting on the copy
    sv_.busy("LOADING\xE2\x80\xA6");
    if (!sv_.peekSlot(i, missing)) {
        t.add("SLOT ").num(i + 1).add(" COULD NOT BE READ");
        say(t.c_str());
        return;
    }
    *undoProject_ = pj_;
    sv_.takePeek(pj_);
    Str<40> lab;
    lab.add("LOAD OF SLOT ").num(i + 1);
    remember(U::ProjectSnap, lab.c_str());
    undo_.i8a = (int8_t)loadedSlot_;
    loadedSlot_ = i;
    loaded();
    t.add(pj_.name).add(" LOADED");
    if (sv_.loadFellBack()) t.add(DOT "LATEST SAVE DAMAGED" DOT "THE ONE BEFORE IT");
    if (missing) t.add(DOT).num(missing).add(missing == 1 ? " SAMPLE MISSING" : " SAMPLES MISSING");
    t.add(DOT "SHIFT + " RING " UNDOES");
    say(t.c_str());
}

// A song to files: the work goes on in tick(), a slice at a time
void Ui::exportSong() {
    Text t;
    if (pj_.songs[exportSong_].nRows == 0) {
        t.add("SONG ").num(exportSong_ + 1).add(" HAS NO ROWS" DOT "SONG ADDS THEM");
        say(t.c_str());
        return;
    }
    if (tp_.playing && exportKind_ != EXPORT_MIDI) stop();   // (a MIDI file is worked out to the side, and what plays plays on)
    export_ = ExportStatus();
    export_.kind = (uint8_t)exportKind_;
    exportNote_.clear();
    if (!sv_.exportStart(exportKind_, exportSong_)) {
        const char* why = sv_.exportWhy();
        exportNote_.add("NOT EXPORTED").add(DOT).add(why ? why : "NO CARD");
        say(exportNote_.c_str());
        return;
    }
    exporting_ = true;
    dirty();
}

// What a project that has just arrived needs
void Ui::loaded() {
    syncLoaded();
    pickForget();
    sv_.stopAllSound();
    tp_.nextPat = -1;
    lockOn_ = false;
    if (tp_.songIdx >= kSongs) tp_.songIdx = 0;
    songRow_ = 0;
    ensurePage();
    fixPage();
}

void Ui::saveSlot() {
    const int i = slot_;
    if (i < 0) { say("A FACTORY PROJECT STAYS AS IT IS" DOT "PICK A CARD SLOT"); return; }
    SlotInfo prev;
    const bool had = slotInfo(i, prev);
    Str<16> name;
    if (had) name.add(prev.name);
    else if (loadedSlot_ < 0 && loadedSlot_ != kNoSlot) name.add(pj_.name);   // a factory project keeps its name in its first slot
    else name.add("PROJECT ").num(i + 1);
    memset(pj_.name, 0, sizeof(pj_.name));
    memcpy(pj_.name, name.c_str(), name.size() < (int)sizeof(pj_.name) - 1 ? (size_t)name.size() : sizeof(pj_.name) - 1);
    Text t;
    if (!sv_.saveSlot(i, pj_)) {
        const char* why = sv_.cardProblem();
        t.add("NOT SAVED").add(DOT).add(why ? why : "CARD WRITE FAILED");
        say(t.c_str());
        return;
    }
    loadedSlot_ = i;
    refreshSlot(i);
    Str<40> lab;
    lab.add("SAVE TO SLOT ").num(i + 1);
    remember(U::CardSave, lab.c_str());
    undo_.idx = (uint8_t)i;
    t.add(had ? "OVERWROTE" : "SAVED TO").add(" SLOT ").num(i + 1).add(DOT).add(name.c_str());
    if (had) t.add(DOT "SHIFT + " RING " UNDOES");
    say(t.c_str());
}

// CLEAR on a slot asks, and again deletes. What was there stays in memory for one undo: the card keeps no copy, because a rename is the two directory writes the
// saves stay away from. The copy takes the undo's place only once the slot has gone, so a refused delete leaves the undo that was waiting.
void Ui::deleteSlot() {
    if (projSub_ != 0) { say("DELETING IS IN PROJECT 1/3: PRESS INFO"); return; }
    const int i = slot_;
    if (i < 0) { say("A FACTORY PROJECT CANNOT BE DELETED"); return; }
    SlotInfo si;
    Text t;
    const uint32_t now = sv_.nowMs();
    if (!slotInfo(i, si)) {
        if (now - deleteAskedAt_ < 400) return;   // a bounce of the press that deleted: what that said stays
        t.add("SLOT ").num(i + 1).add(" IS EMPTY");
        say(t.c_str());
        return;
    }
    if (deleteAsked_ != i || now - deleteAskedAt_ >= kDeleteAskMs) {
        deleteAsked_ = i;
        deleteAskedAt_ = now;
        deleteUnread_ = false;
        t.add(RING " AGAIN DELETES ").add(si.name);
        say(t.c_str());
        dirty();
        return;
    }
    if (now - deleteAskedAt_ < 400) return;   // a bounce of the press that asked
    sv_.busy("DELETING\xE2\x80\xA6");
    int missing = 0;
    const Project* copy = deleteUnread_ ? nullptr : sv_.peekSlot(i, missing);
    if (!copy && !deleteUnread_) {            // no copy, no undo: one more press, knowing that
        deleteUnread_ = true;
        deleteAskedAt_ = now;
        t.add("SLOT ").num(i + 1).add(" CANNOT BE READ" DOT RING " AGAIN DELETES IT FOR GOOD");
        say(t.c_str());
        dirty();
        return;
    }
    const bool fellBack = copy && sv_.loadFellBack();
    deleteAsked_ = -1;
    dirty();
    deleteAskedAt_ = now;
    if (!sv_.deleteSlot(i)) {
        const char* why = sv_.cardProblem();
        t.add("NOT DELETED").add(DOT).add(why ? why : "CARD WRITE FAILED");
        say(t.c_str());
        return;
    }
    refreshSlot(i);
    const int8_t wasLoaded = (int8_t)loadedSlot_;
    if (loadedSlot_ == i) loadedSlot_ = kNoSlot;   // what is in memory stays, and no longer has a place on the card
    t.add("SLOT ").num(i + 1).add(" DELETED");
    if (copy) {
        if (!undoProject_) undoProject_ = new Project;
        *undoProject_ = *copy;
        Str<40> lab;
        lab.add("DELETE OF SLOT ").num(i + 1);
        remember(U::SlotDelete, lab.c_str());
        undo_.idx = (uint8_t)i;
        undo_.i8a = wasLoaded;
        if (fellBack || missing) {
            t.add(DOT "UNDO:");
            if (fellBack) t.add(" OLDER SAVE");
            if (fellBack && missing) t.add(",");
            if (missing) t.ch(' ').num(missing).add(missing == 1 ? " SAMPLE LOST" : " SAMPLES LOST");
        } else {
            t.add(DOT).add(si.name);
        }
        t.add(DOT "SHIFT + " RING " UNDOES");
    } else {
        if (undo_.kind == U::CardSave && undo_.idx == i) undo_.kind = U::None;   // what that undo would take back has gone
        t.add(DOT "NO UNDO");
    }
    say(t.c_str());
}

// One slot, after a change to it: thirty slots are not read again for that
void Ui::refreshSlot(int i) {
    slotHas_[i] = sv_.slotInfo(i, slots_[i]);
}

void Ui::refreshCard() {
    for (int i = 0; i < kCardSlots; ++i) slotHas_[i] = sv_.slotInfo(i, slots_[i]);
    cardRead_ = true;
}

bool Ui::slotInfo(int i, SlotInfo& out) {
    if (!cardRead_) refreshCard();
    if (!slotHas_[i]) return false;
    out = slots_[i];
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// undo

void Ui::remember(U kind, const char* label) {
    undo_.kind = kind;
    undo_.label.clear();
    undo_.label.add(label);
}

void Ui::undo() {
    const int16_t clock = pj_.parts[0].pages[PG_GLOB][4];   // (the project's, whatever an undo of a part's pages brings back)
    bool undone = true;
    if (undo_.kind == U::None) { say("NOTHING TO UNDO"); return; }
    const Undo u = undo_;
    undo_.kind = U::None;
    switch (u.kind) {
        case U::LaneClear: {
            Lane& l = pj_.patterns[u.pat].lanes[u.lane];
            l.eu = u.eu;
            for (int i = 0; i < kMaxSteps; ++i) l.st[i].ov = u.ov[i];
            break;
        }
        case U::LaneSnap: pj_.patterns[u.pat].lanes[u.lane] = undoLane_; break;
        case U::PatternSnap: pj_.patterns[u.pat] = undoPattern_; break;
        case U::SongSnap: pj_.songs[u.idx] = undoSong_; break;
        case U::Slices: {
            SliceMap* to = sliceMapFor(pj_, undoSlices_.sample);
            if (!to) break;
            sv_.holdAudio(true);
            *to = undoSlices_;
            sv_.holdAudio(false);
            sliceSel_ = clampi(sliceSel_, 0, to->count > 0 ? to->count - 1 : 0);
            break;
        }
        case U::FxSnap: {
            int16_t* v = fxValues(pj_.fx, u.idx, u.part);
            sv_.holdAudio(true);
            for (int i = 0; i < (u.idx == FX_SENDS ? kSendsUsed : kFxParams); ++i) v[i] = undoFxVals_[i];
            sv_.holdAudio(false);
            break;
        }
        case U::SongRow: {
            Song& so = pj_.songs[u.idx];
            if (so.nRows < kSongRows) {
                sv_.holdAudio(true);
                for (int i = so.nRows; i > u.lane; --i) so.rows[i] = so.rows[i - 1];
                so.rows[u.lane] = u.row;
                ++so.nRows;
                if (tp_.songPlaying && tp_.songIdx == u.idx) {   // the playing song keeps its place
                    if (u.i8a && tp_.jumpTo == u.lane && tp_.playRow == u.lane) {
                        tp_.jumpTo = -1;   // the removed row never stopped playing: it is simply back, with its repeats
                    } else {
                        if (u.lane <= tp_.playRow && so.nRows > 1) tp_.playRow = (uint8_t)(tp_.playRow + 1);
                        if (tp_.jumpTo >= u.lane) tp_.jumpTo = (int8_t)(tp_.jumpTo + 1);
                    }
                }
                sv_.holdAudio(false);
            }
            break;
        }
        case U::PartPages:
            memcpy(pj_.parts[u.part].pages, undoPages_, sizeof(undoPages_));
            pj_.parts[u.part].preset = u.i16a;
            loaded_[u.part] = u.i16b;
            if (u.idx >= 1) pj_.parts[u.part].voices = u.idx;
            {   // the part's sends and amp go back; a shared effect goes back only if it is still as the load left it (an edit made since stays),
                // the compressor and the other parts' sends were not touched by the load and are not touched now
                FxSettings nf = pj_.fx;
                for (int k = 0; k < kSends; ++k) nf.send[u.part][k] = undoFxAll_.send[u.part][k];
                for (int k = 0; k < kFxParams; ++k) nf.amp[u.part][k] = undoFxAll_.amp[u.part][k];
                if (memcmp(pj_.fx.delay, postFx_.delay, sizeof(nf.delay)) == 0) memcpy(nf.delay, undoFxAll_.delay, sizeof(nf.delay));
                if (memcmp(pj_.fx.reverb, postFx_.reverb, sizeof(nf.reverb)) == 0) memcpy(nf.reverb, undoFxAll_.reverb, sizeof(nf.reverb));
                if (memcmp(pj_.fx.chorus, postFx_.chorus, sizeof(nf.chorus)) == 0) memcpy(nf.chorus, undoFxAll_.chorus, sizeof(nf.chorus));
                sv_.holdAudio(true);
                pj_.fx = nf;
                sv_.holdAudio(false);
            }
            sv_.wavesChanged();
            break;
        case U::LaneSample:
            pj_.patterns[u.pat].lanes[u.lane].smpParam(PG_SMP, 0) = u.i16a;
            laneFolder_[u.lane] = 0;
            break;
        case U::StepSnap:
            sv_.holdAudio(true);   // (the sequencer reads a step's locks: it sees all of it, or none)
            pj_.patterns[u.pat].lanes[u.lane].st[u.idx] = undoStep_;
            sv_.holdAudio(false);
            break;
        case U::StepSample: {
            Step& st = pj_.patterns[u.pat].lanes[u.lane].st[u.idx];
            if (u.i16a < 0) st.removeLock(lockKey(PG_SMP, 0));
            else st.setLock(lockKey(PG_SMP, 0), u.i16a);   // (it had this lock: the place is there)
            stepMoved_ = -1;
            break;
        }
        case U::PartType:
            pj_.parts[u.part].type = (uint8_t)u.i8a;
            pj_.parts[u.part].voices = (uint8_t)u.i16a;
            loaded_[u.part] = u.i16b;
            ensurePage();
            break;
        case U::ProjectSnap:
            if (undoProject_) pj_ = *undoProject_;
            pickForget();
            loadedSlot_ = u.i8a;
            tp_.nextPat = -1;
            lockOn_ = false;
            songRow_ = 0;
            ensurePage();
            fixPage();
            break;
        case U::CardSave: undone = sv_.undoSave(u.idx); break;
        case U::SlotDelete:
            undone = sv_.saveSlot(u.idx, *undoProject_);
            if (undone) loadedSlot_ = u.i8a;
            else undo_ = u;   // the only copy there is: it can be tried again
            break;
        case U::None: break;
    }
    fixSongRow();
    if (u.kind == U::CardSave || u.kind == U::SlotDelete) refreshSlot(u.idx);
    if (loadedSlot_ >= 0 && !slotHas_[loadedSlot_]) loadedSlot_ = kNoSlot;   // an undo can lead to a slot that has since gone
    if (u.kind != U::ProjectSnap) for (Part& p : pj_.parts) p.pages[PG_GLOB][4] = clock;
    Text t;
    if (undone) {
        t.add("UNDONE: ").add(u.label.c_str());
    } else {
        const char* why = sv_.cardProblem();
        t.add("NOT UNDONE").add(DOT).add(why ? why : "THE CARD STAYS AS IT IS");
    }
    say(t.c_str());
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the bottom keys

void Ui::pressLayer(int i) {
    if (shift_ && mode_ == Mode::Play) { selectPattern(i); return; }   // SHIFT + key 1-10: pattern 1-10
    if (mode_ == Mode::Step) {
        Lane& ln = lane();
        if (i == 8) { stepPage_ = stepPage_ > 0 ? stepPage_ - 1 : 0; cursor_ = stepPage_ * 8; dirty(); return; }
        if (i == 9) {
            const int last = (ln.n + 7) / 8 - 1;
            stepPage_ = stepPage_ + 1 < last ? stepPage_ + 1 : last;
            cursor_ = stepPage_ * 8;
            dirty();
            return;
        }
        const int idx = stepPage_ * 8 + i;
        if (idx >= ln.n) return;
        cursor_ = idx;
        ln.st[idx].ov = !ln.st[idx].ov;
        dirty();
    } else selectLane(i);
}

void Ui::layerDown(int i) {
    if (exporting_) return;
    settle();
    clearDown_ = false;
    deleteAsked_ = -1;
    if (mode_ == Mode::Save) return;
    if (tracker()) { trackerLayerDown(i); return; }
    const bool lockedSound = lockOn_ && (mode_ == Mode::Sound || mode_ == Mode::Fx) && i < 8;
    if (!lockedSound && (mode_ != Mode::Step || i >= 8)) { pressLayer(i); return; }
    const int idx = stepPage_ * 8 + i;
    if (idx >= lane().n) return;
    // the held step is selected at once, so knobs turned while it is held edit THAT step; a tap still toggles it
    held_ = Held();
    held_.on = true;
    held_.i = i;
    held_.idx = idx;
    held_.at = sv_.nowMs();
    if (!lockOn_) { cursor_ = idx; dirty(); }
}

void Ui::layerUp(int i) {
    settle();   // (a held step a pattern switch made too long for its lane is let go)
    const Held h = held_;
    held_.on = false;
    if (!h.on || h.i != i || h.latch) return;
    Text t;
    if (h.edited) { dirty(); return; }   // held and edited: the step keeps its state, no lock is left open
    if (h.lng) {                          // held without turning anything: open the step's lock
        lockOn_ = true;
        lockPat_ = tp_.pat;
        lockLane_ = sel_;
        lockIdx_ = h.idx;
        cursor_ = h.idx;
        lockEdited_ = false;
        t.add("LOCK STEP ").num(h.idx + 1).add(": PICK A SOUND PAGE, TURN A KNOB");
        say(t.c_str());
        return;
    }
    if (lockOn_) {                        // a lock is open: a tap moves it, tapping the same step closes it
        if (lockIdx_ == h.idx && lockLane_ == sel_) { lockOn_ = false; say("LOCK OFF"); }
        else {
            lockPat_ = tp_.pat;
            lockLane_ = sel_;
            lockIdx_ = h.idx;
            cursor_ = h.idx;
            t.add("LOCK STEP ").num(h.idx + 1);
            say(t.c_str());
        }
        return;
    }
    if (tracker()) { dirty(); return; }   // (there a key is a lane: it has moved the cursor, and steps are not tapped)
    cursor_ = h.idx;
    lane().st[h.idx].ov = !lane().st[h.idx].ov;
    dirty();
}

// ---------------------------------------------------------------------------------------------------------------------------------

void Ui::tick() {
    settle();
    const uint32_t now = sv_.nowMs();
    tp_.startRow = (int8_t)(mode_ == Mode::Song && song().nRows ? clampi(songRow_, 0, song().nRows - 1) : -1);
    if (exporting_) {
        const uint8_t was = export_.percent;
        if (sv_.exportStep(export_)) {
            if (export_.percent != was) dirty();
            return;
        }
        exporting_ = false;
        seenSongEnds_ = tp_.songEnds;         // (its passes are not the end of a song)
        seenPatChanges_ = tp_.patChanges;
        exportNote_.clear();
        if (export_.result == EXPORT_DONE) {
            exportNote_.add(export_.kind == EXPORT_MIDI ? "MIDI FILE" : export_.kind == EXPORT_STEMS ? "STEMS" : "SONG").add(" SAVED" DOT).add(sv_.exportWhere());
            if (export_.missing) exportNote_.add(DOT).num(export_.missing).add(export_.missing == 1 ? " SAMPLE MISSING" : " SAMPLES MISSING");
        } else if (export_.result == EXPORT_STOPPED) {
            exportNote_.add("EXPORT STOPPED");
        } else {
            const char* why = sv_.exportWhy();
            exportNote_.add("EXPORT FAILED").add(DOT).add(why ? why : "CARD WRITE FAILED");
        }
        say(exportNote_.c_str());
        dirty();
        return;
    }
    if (held_.on && !held_.edited && !held_.lng && now - held_.at >= kHoldMs) {
        held_.lng = true;
        Text t;
        t.add("STEP ").num(held_.idx + 1).add(": TURN A KNOB TO EDIT IT, OR RELEASE TO OPEN ITS LOCK");
        say(t.c_str());
    }
    if (laneDown_ && now - laneDownAt_ >= kHomeHoldMs) {   // L held: home (the press itself went back one level)
        laneDown_ = false;
        if (mode_ != Mode::Play) { held_.on = false; lockOn_ = false; setMode(Mode::Play); say("HOME"); }   // (from SAVE the press itself already cancelled it)
    }
    if (deleteAsked_ >= 0 && now - deleteAskedAt_ >= kDeleteAskMs) { deleteAsked_ = -1; dirty(); }
    if (clearDown_ && now - clearDownAt_ >= kClearHoldMs) {
        clearDown_ = false;
        emptyPattern();
    }
    if (tp_.patChanges != seenPatChanges_) {   // the sequencer switched pattern: a lock on the old one closes
        seenPatChanges_ = tp_.patChanges;
        lockOn_ = false;
        fixPage();
        dirty();
    }
    if (tp_.songEnds != seenSongEnds_) {
        seenSongEnds_ = tp_.songEnds;
        say("END OF SONG");
    }
    if (previewPending_ >= 0 && (int32_t)(now - previewAt_) >= 0) {
        if (!isSamplerPart(part_)) sv_.previewPreset(part_, previewPending_);
        previewPending_ = -1;
    }
    if (auditionWait_ >= 0 && (int32_t)(now - auditionAt_) >= 0) auditionGo(auditionWait_);
    // the CPU governor gave something up or took it back
    const int lvl = degradeLevel();
    if (lvl != lastDegrade_) {
        const int was = lastDegrade_;
        lastDegrade_ = lvl;
        const Ladder& l = ladder();
        const int k = (lvl > was ? lvl : was) - 1;
        Text t;
        if (lvl > was) t.add("DEGRADE ").num(lvl).add(": ");
        else t.add("RESTORED: ");
        if (k >= 0 && k < l.count) describeStep(t, l.steps[k]);
        say(t.c_str());
    }
    // things that change the screen without an input: a toast or a lane flash running out, the playhead, the MIXER's live numbers
    const bool toastNow = toast() != nullptr, flashNow = flashLane_ >= 0 && laneFlashing(flashLane_);
    if (toastNow != toastShown_ || flashNow != flashShown_) { toastShown_ = toastNow; flashShown_ = flashNow; dirty(); }
    const int shown = tp_.playing ? tp_.shownStep : -1;
    if (shown != lastShown_) { lastShown_ = shown; dirty(); }
    if (mode_ == Mode::Mixer && now - lastLive_ >= 100) { lastLive_ = now; dirty(); }
    if (mode_ == Mode::Record && now - lastRec_ >= 40) {
        lastRec_ = now;
        sv_.recStatus(rec_);
        recLevel_ = rec_.peak > recLevel_ ? rec_.peak : recLevel_ * 0.8f;
        if (rec_.state != recSeen_) {
            if (rec_.state == REC_TAKING && recSeen_ == REC_ARMED) say("SOUND: RECORDING");
            if (rec_.state == REC_DONE) say(rec_.clipped ? "TAKE CLIPPED: LOWER THE GAIN" DOT "OK KEEPS, " RING " DROPS" : "OK KEEPS, " RING " DROPS, REC AGAIN");
            recSeen_ = rec_.state;
        }
        dirty();
    }
    if (now - lastIdle_ >= 50) {   // (it reads every lane of every pattern, out in SDRAM: not on every pass of the main loop)
        lastIdle_ = now;
        uint32_t busy = 0;
        for (int i = 0; i < kParts; ++i) {   // a part played live stays in the budget for a while: going in and out with each note would change the ladder under every other part
            if (sv_.activeVoices(i) > 0) busyUntil_[i] = now + kBusyHoldMs;
            if ((int32_t)(busyUntil_[i] - now) > 0) busy |= 1u << i;
        }
        refreshIdleParts(pj_, busy);
        refreshSlices();
        refreshLoops();
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the sample knobs: a folder, then a sample in it

Ui::Pick Ui::pickFor(int knob) const {
    Pick p;
    if (knob == PickStep) { p.lane = sel_; p.step = cursor_; }
    else if (knob == PickLane) p.lane = sel_;
    else if (lockOpen()) { p.lane = lockLane_; p.step = lockIdx_; }   // the SAMPLE page, with a step's lock open
    else p.lane = curLaneIndex();
    return p;
}

int Ui::laneSampleNow(int lane) const { return clampi(pattern().lanes[lane].sample(), 0, lib_.count - 1); }

int Ui::pickNow(const Pick& p) const {
    if (p.step < 0) return laneSampleNow(p.lane);
    const int own = pattern().lanes[p.lane].st[p.step].lockValue(lockKey(PG_SMP, 0), -1);
    return own < lib_.count ? own : -1;
}

int Ui::pickFolder(const Pick& p) const {
    const int own = pickNow(p);
    if (p.step < 0) return laneFolder_[p.lane] ? laneFolder_[p.lane] : lib_.items[own].cat;
    if (own >= 0 && stepMoved_ != stepPlace(p)) return lib_.items[own].cat;
    return stepFolder_[p.lane] ? stepFolder_[p.lane] : lib_.items[laneSampleNow(p.lane)].cat;
}

bool Ui::pickMoved(const Pick& p) const {
    const int own = pickNow(p);
    return own >= 0 && lib_.items[own].cat != pickFolder(p);
}

int Ui::pickShown(const Pick& p) const {
    const int own = pickNow(p);
    if (own >= 0 && !pickMoved(p)) return own;
    const int cat = pickFolder(p);
    int s = catLast_[cat] >= 0 && catLast_[cat] < lib_.count && lib_.items[catLast_[cat]].cat == cat ? catLast_[cat] : -1;   // where it was last, in this folder
    if (own < 0) {   // a step with no lock of its own starts near the lane's sample, and a lock of the very same sample would change nothing
        const int ls = laneSampleNow(p.lane);
        if (s < 0 && lib_.items[ls].cat == cat) s = ls;
        if (s == ls) {
            const int after = lib_.catNext(ls, 1), before = lib_.catNext(ls, -1);
            s = after >= 0 ? after : before >= 0 ? before : ls;
        }
    }
    return s >= 0 ? s : lib_.catFirst(cat);
}

// The sample knob. What the screen shows is picked: after the folder knob has moved the first detent, either way, takes the sample on show; then each detent moves one
// sample, inside the folder. Until then the lane plays what it played.
void Ui::pickTurn(const Pick& p, int dir) {
    if (!lib_.count) return;
    const int now = pickNow(p);
    if (now < 0 && dir < 0) return;                                  // LANE is the step knob's left stop
    if (now < 0 || pickMoved(p)) { pickPut(p, pickShown(p)); return; }
    const int to = lib_.catNext(now, dir);
    if (to >= 0) { pickPut(p, to); return; }
    if (p.step >= 0 && dir < 0) { pickPut(p, -1); return; }          // past the folder's first: the step goes back to the lane's sample
    Text t;
    t.add(dir > 0 ? "LAST IN " : "FIRST IN ").add(lib_.cats[lib_.items[now].cat]).add(DOT "SHIFT + TURN: ANOTHER FOLDER");
    say(t.c_str());
}

// The folder knob moves the knob's place only: nothing is picked, the lane and the step play on. The sample on show is heard when the knob stops.
void Ui::pickFolderTurn(const Pick& p, int dir) {
    if (!lib_.count) return;
    const int was = pickFolder(p), cat = lib_.catStep(was, dir, false);
    if (cat == was) return;
    const int own = pickNow(p);
    if (p.step < 0) laneFolder_[p.lane] = lib_.items[own].cat == cat ? 0 : (uint8_t)cat;
    else {
        stepFolder_[p.lane] = (uint8_t)cat;
        stepMoved_ = own >= 0 && lib_.items[own].cat != cat ? stepPlace(p) : -1;
    }
    auditionWait_ = pickShown(p);
    auditionAt_ = sv_.nowMs() + kPreviewDelayMs;
    Text t;
    const int n = lib_.catSize(cat);
    t.add(lib_.cats[cat]).add(DOT).num(n).add(n == 1 ? " SAMPLE" DOT : " SAMPLES" DOT).add("LET GO OF SHIFT AND TURN TO PICK");
    say(t.c_str());
}

void Ui::setLaneSample(int li, int sample, bool run) {
    Lane& l = pattern().lanes[li];
    if (!(run && undo_.kind == U::LaneSample && undo_.pat == tp_.pat && undo_.lane == li)) {
        Str<40> lab;
        lab.add("SAMPLE ON LANE ").num(li + 1);
        remember(U::LaneSample, lab.c_str());
        undo_.pat = tp_.pat;
        undo_.lane = (uint8_t)li;
        undo_.i16a = l.smpParam(PG_SMP, 0);
    }
    l.smpParam(PG_SMP, 0) = (int16_t)sample;
    laneFolder_[li] = 0;
    if (sample < lib_.count) catLast_[lib_.items[sample].cat] = (int16_t)sample;
    loopKnobAt_ = sv_.nowMs();
}

// `sample` < 0: a step with no lock of its own
void Ui::pickPut(const Pick& p, int sample) {
    Text t;
    if (p.step < 0) setLaneSample(p.lane, sample, true);
    else {
        Step& st = pattern().lanes[p.lane].st[p.step];
        const uint8_t key = lockKey(PG_SMP, 0);
        const int was = st.lockValue(key, -1);
        if (sample < 0) st.removeLock(key);
        else if (!st.setLock(key, (int16_t)sample)) { say("A STEP HOLDS 8 LOCKS"); return; }
        const bool same = undo_.pat == tp_.pat && undo_.lane == p.lane && undo_.idx == p.step;
        if (!(same && (undo_.kind == U::StepSample || undo_.kind == U::StepSnap))) {   // a run of turns is one undo (a tracker's edit has the whole step already)
            Str<40> lab;
            lab.add("SAMPLE ON STEP ").num(p.step + 1);
            remember(U::StepSample, lab.c_str());
            undo_.pat = tp_.pat;
            undo_.lane = (uint8_t)p.lane;
            undo_.idx = (uint8_t)p.step;
            undo_.i16a = (int16_t)was;
        }
        lockEdited_ = true;
        stepMoved_ = -1;
        if (sample >= 0) stepFolder_[p.lane] = lib_.items[sample].cat;
    }
    if (sample < 0) { t.add("STEP ").num(p.step + 1).add(" PLAYS THE LANE'S SAMPLE"); say(t.c_str()); return; }
    const SampleItem& it = lib_.items[sample];
    catLast_[it.cat] = (int16_t)sample;
    t.add(lib_.cats[it.cat]).ch(' ').num(lib_.catPlace(sample) + 1).ch('/').num(lib_.catSize(it.cat)).add(DOT).add(it.name);
    say(t.c_str());
    auditionSoon(sample);
}

// A sample knob's cell: the folder over the name, the place in the folder in the corner, and the bar and the panel's knob at that place. An amber name is not picked yet.
void Ui::pickCell(const Pick& p, Cell& c) {
    c.label.clear();
    c.value.clear();
    if (!lib_.count) { c.label.add(p.step >= 0 ? "STEP SAMPLE" : "SAMPLE"); c.value.add(p.step >= 0 ? "LANE" : "?"); return; }
    const int cat = pickFolder(p), s = pickShown(p);
    if (pickNow(p) < 0) {   // a step that plays the lane's sample: the knob's left stop, in this folder
        c.label.add("STEP SAMPLE");
        c.value.add("LANE");
        c.tag.add(lib_.cats[cat]);
        c.bar = 0.0f;
        return;
    }
    const int n = lib_.catSize(cat), at = lib_.catPlace(s);
    if (c.lock) c.label.add(lib_.cats[cat]).ch(' ').num(at + 1).ch('/').num(n);   // (the corner says LOCK)
    else {
        c.label.add(p.step >= 0 ? "STEP " : "").add(lib_.cats[cat]);
        c.tag.num(at + 1).ch('/').num(n);
    }
    c.value.add(lib_.items[s].name);
    c.pend = pickMoved(p);
    if (p.step < 0) c.bar = n > 1 ? (float)at / (float)(n - 1) : 0.0f;
    else c.bar = (float)(at + 1) / (float)n;                          // (LANE is the place before the first)
}

void Ui::pickForget() {
    memset(laneFolder_, 0, sizeof(laneFolder_));
    memset(stepFolder_, 0, sizeof(stepFolder_));
    stepMoved_ = -1;
}

// A knob on the samples: the first detent is heard at once, a fast turn only where it stops. Each audition can read the card, and the main loop waits for that.
void Ui::auditionSoon(int sample) {
    const uint32_t now = sv_.nowMs();
    if (auditionWait_ < 0 && (int32_t)(now - auditionLast_) >= (int32_t)kPreviewDelayMs) { auditionGo(sample); return; }
    auditionWait_ = sample;
    auditionAt_ = now + kPreviewDelayMs;
}

void Ui::auditionGo(int sample) {
    auditionWait_ = -1;
    auditionLast_ = sv_.nowMs();
    sv_.auditionSample(sample);
    if (const char* why = sv_.auditionProblem()) say(why);
}

const SliceMap* Ui::shownSlices() const {
    if (!isSamplerPart(part_) || !partHasLane(part_)) return nullptr;
    return findSlices(pj_, pattern().lanes[curLaneIndex()].sample());
}

bool Ui::shownLoop(uint32_t& start, uint32_t& end, double& fade, double& lap, int xfade, int sample) const {
    if (!isSamplerPart(part_) || !partHasLane(part_)) return false;
    const Lane& l = pattern().lanes[curLaneIndex()];
    const int smp = l.sample();
    if (smp < 0 || smp >= lib_.count || smp != sample || !lib_.items[smp].length) return false;
    const SampleItem& it = lib_.items[smp];
    const int16_t* sm = l.smp[lanePageIndex(PG_SMP)];
    int16_t lp[kParamsPerPage];
    memcpy(lp, l.smp[lanePageIndex(PG_LOOP)], sizeof(lp));
    lp[LP_XFADE] = (int16_t)xfade;
    LoopRegion r;
    loopRegion(sm, lp, it.length, it.rate, r);
    if (lp[LP_AUTO] != AUTO_OFF) {
        const LoopFit* f = findLoopFit(pj_, smp, sm[1], sm[2], lp[LP_AUTO], lp[LP_SEARCH], loopFadeKey(l.smp[lanePageIndex(PG_LOOP)]));
        if (f && f->frames == it.length) { r.start = f->start; r.end = f->end; r.fade = loopFadeFrames(lp, r.end - r.start, it.rate); }
    }
    start = (uint32_t)r.start;
    end = (uint32_t)r.end;
    fade = r.fade;
    const bool rev = sm[6] == 1;
    lap = rev ? ((double)it.length - 1.0 - r.fade < r.end ? (double)it.length - 1.0 - r.fade : r.end) - r.start : r.end - (r.start > r.fade ? r.start : r.fade);
    return true;
}

void Ui::refreshLoops() {
    if ((int32_t)(sv_.nowMs() - loopKnobAt_) < 150) return;   // still turning
    for (const Pattern& p : pj_.patterns)
        for (const Lane& l : p.lanes) {
            const int smp = l.sample();
            const int16_t* sm = l.smp[lanePageIndex(PG_SMP)];
            const int16_t* lp = l.smp[lanePageIndex(PG_LOOP)];
            if (!isSampler(pj_, l.part) || smp < 0 || smp >= lib_.count || sm[3] != 2 || lp[LP_AUTO] == AUTO_OFF) continue;
            const SampleItem& it = lib_.items[smp];
            if (!it.data || it.length < 2) continue;   // not in memory yet: a later tick
            const LoopFit* had = findLoopFit(pj_, smp, sm[1], sm[2], lp[LP_AUTO], lp[LP_SEARCH], loopFadeKey(lp));
            if (had && had->frames == it.length) continue;
            LoopFit* to = had ? const_cast<LoopFit*>(had) : loopFitFor(pj_);
            if (!to) continue;   // every entry is in use: this lane stays on its knobs' points
            LoopRegion r;
            loopRegion(sm, lp, it.length, it.rate, r);
            uint32_t s = (uint32_t)r.start, e = (uint32_t)r.end;
            float corr = 0.0f;
            const bool found = findLoopPoints(it.data, it.length, it.rate, s, e, lp[LP_AUTO], lp[LP_SEARCH], r.fade, s, e, corr);
            LoopFit fresh;
            memset(&fresh, 0, sizeof(fresh));
            fresh.sample = (int16_t)smp;
            fresh.wantS = sm[1];
            fresh.wantE = sm[2];
            fresh.algo = (uint8_t)lp[LP_AUTO];
            fresh.search = (uint8_t)lp[LP_SEARCH];
            fresh.fadeKey = loopFadeKey(lp);
            fresh.corr = (uint8_t)(corr * 100.0f + 0.5f);
            fresh.frames = it.length;
            fresh.start = s;
            fresh.end = e;
            sv_.holdAudio(true);
            *to = fresh;
            sv_.holdAudio(false);
            if (&l == &pattern().lanes[curLaneIndex()]) {
                Text t;
                if (found) t.add("LOOP FOUND").add(DOT).fixed((double)(e - s) / it.rate, 3).add(" S");
                else t.add("NO BETTER LOOP THERE");
                say(t.c_str());
            }
            return;
        }
}

void Ui::rememberSlices(const SliceMap& m, bool fresh) {
    if (!fresh && undo_.kind == U::Slices && undoSlices_.sample == m.sample) return;   // a run of edits is one undo
    remember(U::Slices, "SLICES");
    undoSlices_ = m;
}

void Ui::refreshSlices() {
    const bool due = sliceCutFor_ >= 0 && (int32_t)(sv_.nowMs() - sliceCutAt_) >= 120;
    bool waiting = false;
    for (Lane& l : pattern().lanes) {
        const int smp = l.sample();
        if (!isSampler(pj_, l.part) || smp < 0 || smp >= lib_.count) continue;
        const SampleItem& it = lib_.items[smp];
        const bool wants = l.smpParam(PG_SLC, SLC_MODE) != SLICE_OFF;
        const SliceMap* had = findSlices(pj_, smp);
        const bool asked = due && smp == sliceCutFor_ && (wants || had);
        if (!asked && !(wants && (!had || had->frames != it.length))) continue;
        if (!it.data || it.length < 2) { waiting = waiting || asked; continue; }   // not in memory yet: a later tick
        cutSlices(l, smp, asked);
        return;
    }
    if (due && !waiting) sliceCutFor_ = -1;
}

bool Ui::cutSlices(Lane& l, int smp, bool asked) {
    const SampleItem& it = lib_.items[smp];
    static SliceMap fresh;   // (not on the stack, and the audio is held only for the copy)
    fresh.sample = (int16_t)smp;
    if (sliceCutFor_ == smp) sliceCutFor_ = -1;
    SliceMap* to = sliceMapFor(pj_, smp);
    if (!to) { say("16 SAMPLES ARE CUT ALREADY"); return false; }
    const int n = detectSlices(it.data, it.length, it.rate, l.smpParam(PG_SLC, SLC_SENS), kSliceGrid[clampi(l.smpParam(PG_SLC, SLC_GRID), 0, kSliceGridChoices - 1)], fresh);
    if (asked && to->sample == smp) rememberSlices(*to, true);   // what the knob replaces comes back with undo. (A tick's own cut is not yours to undo.)
    sv_.holdAudio(true);
    *to = fresh;
    sv_.holdAudio(false);
    sliceSel_ = 0;
    if (&l != &pattern().lanes[curLaneIndex()]) return true;
    Text t;
    const int beats = kSliceBeats[clampi(l.smpParam(PG_SLC, SLC_BEATS), 0, kSliceBeatChoices - 1)];
    t.num(n).add(n == 1 ? " SLICE" : " SLICES").add(DOT).num(beats).add(beats == 1 ? " BEAT" DOT : " BEATS" DOT).fixed(beats * 60.0 / lib_.seconds(smp), 1).add(" BPM");
    if (asked) t.add(DOT "SHIFT + " RING " UNDOES");
    say(t.c_str());
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// SAVE PRESET

static const char kSaveChars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-.()+#'";

void Ui::saveInitName() {
    memset(saveName_, ' ', kPresetNameMax);
    saveName_[kPresetNameMax] = 0;
    const int src = pj_.parts[part_].preset;
    const char* base = presetExists(src) ? presetName(src) : "New Sound";
    int n = 0;
    for (; base[n] && n < kPresetNameMax; ++n) saveName_[n] = base[n];
    while (n > 0 && saveName_[n - 1] == ' ') --n;   // (a loaded name's trailing spaces)
    if (!saveReplaces() && presetExists(src) && n <= kPresetNameMax - 2) { saveName_[n] = ' '; saveName_[n + 1] = '2'; }   // "Acid Roll" -> "Acid Roll 2"
}

void Ui::openSave() {
    if (isSamplerPart(part_)) { say("SAVING A PRESET IS FOR THE SYNTH PARTS"); return; }
    if (lockOn_) { say("CLOSE THE STEP LOCK FIRST" DOT "A PRESET IS THE PART'S OWN SOUND"); return; }
    previewPending_ = -1;   // a browse preview that was about to play does not
    const int src = pj_.parts[part_].preset;
    saveSrc_ = src >= kPresetIdMin && presetExists(src) ? src : -1;
    saveOver_ = false;
    saveEdited_ = false;
    saveCur_ = 0;
    saveArmUntil_ = 0;
    saveCat_ = presetExists(src) ? clampi(presetCat(src), 1, kPresetCatCount - 1) : browseCat_ > 0 ? browseCat_ : 1;
    saveInitName();
    saveOpenedAt_ = sv_.nowMs();
    saveTouched_ = false;
    setMode(Mode::Save);
    say("SAVE PRESET" DOT "OK SAVES" DOT "L CANCELS");
}

void Ui::saveStepChar(int d) {
    const int n = (int)sizeof(kSaveChars) - 1;
    int i = 0;
    while (i < n && kSaveChars[i] != saveName_[saveCur_]) ++i;
    if (i == n) i = 0;   // a character not in the list (cannot be typed, but a loaded name may have one)
    i = ((i + d) % n + n) % n;
    saveName_[saveCur_] = kSaveChars[i];
    saveEdited_ = true;
    saveArmUntil_ = 0;
    dirty();
}

// The part's sound as a preset record: the nine pages (a wavetable is the bank field, not the WAVE knob), the voices, and the part's effects.
void Ui::saveRecord(PresetRecord& rec) const {
    memset(&rec, 0, sizeof(rec));
    const Part& p = pj_.parts[part_];
    rec.cat = (uint8_t)clampi(saveCat_, 1, kPresetCatCount - 1);
    rec.voices = (uint8_t)clampi(p.voices, 1, kPresetVoicesMax);
    rec.bank[0] = rec.bank[1] = 255;
    for (int pg = 0; pg < kSynthPages; ++pg)
        for (int k = 0; k < kParamsPerPage; ++k) rec.v[pg * kParamsPerPage + k] = (int8_t)clampi(p.pages[pg][k], -128, 127);
    for (int s = 0; s < 2; ++s) {
        const int w = p.pages[s == 0 ? PG_OSC1 : PG_OSC2][0];
        if (w >= kBasicWaves) { rec.bank[s] = (uint8_t)clampi(w - kBasicWaves, 0, kWaveBanks - 1); rec.v[(s == 0 ? PG_OSC1 : PG_OSC2) * kParamsPerPage] = 0; }
    }
    const FxSettings& fx = pj_.fx;
    for (int e = 0; e < 3; ++e) rec.send[e] = (uint8_t)clampi(fx.send[part_][e], 0, 127);
    if (fx.amp[part_][0] != 0) {
        rec.mask |= PM_AMP;
        for (int k = 0; k < kFxParams; ++k) rec.amp[k] = (uint8_t)clampi(fx.amp[part_][k], 0, 127);
    }
    for (int k = 0; k < kFxParams; ++k) {   // the shared effects it sends to
        if (rec.send[0] > 0) rec.delay[k] = (uint8_t)clampi(fx.delay[k], 0, 127);
        if (rec.send[1] > 0) rec.reverb[k] = (uint8_t)clampi(fx.reverb[k], 0, 127);
        if (rec.send[2] > 0) rec.chorus[k] = (uint8_t)clampi(fx.chorus[k], 0, 127);
    }
    if (rec.send[0] > 0) rec.mask |= PM_DELAY;
    if (rec.send[1] > 0) rec.mask |= PM_REVERB;
    if (rec.send[2] > 0) rec.mask |= PM_CHORUS;
}

void Ui::saveCommit() {
    if (!saveTouched_ && (int32_t)(sv_.nowMs() - saveOpenedAt_) < 400) return;   // the same press that opened the editor, or a bounce, does not save
    int len = kPresetNameMax;
    while (len > 0 && saveName_[len - 1] == ' ') --len;
    if (len == 0 || saveName_[0] == ' ') { say("A NAME MUST START WITH A CHARACTER"); return; }
    if (saveOver_ && !saveCanReplace()) { say("THE PRESET TO REPLACE IS GONE"); saveOver_ = false; saveArmUntil_ = 0; dirty(); return; }
    const bool replace = saveReplaces();
    if (replace) {   // replacing asks twice: OK again within four seconds
        const bool armed = saveArmUntil_ != 0 && (int32_t)(sv_.nowMs() - saveArmUntil_) < 0;
        if (!armed) {
            saveArmUntil_ = sv_.nowMs() + 4000;
            if (saveArmUntil_ == 0) saveArmUntil_ = 1;
            Text t;
            t.add("REPLACE ").add(presetName(saveSrc_)).add("? PRESS OK AGAIN");
            say(t.c_str());
            dirty();
            return;
        }
    }
    PresetRecord rec;
    saveRecord(rec);
    memcpy(rec.name, saveName_, (size_t)len);
    int id = -1;
    sv_.busy("SAVING PRESET\xE2\x80\xA6");
    const PresetSave r = presetSave(rec, replace ? saveSrc_ : -1, id);
    saveArmUntil_ = 0;
    switch (r) {
        case PresetSave::Ok: {
            pj_.parts[part_].preset = (int16_t)id;
            loaded_[part_] = (int16_t)id;
            Text t;
            t.add("SAVED ").add(rec.name).add(replace ? " OVER ID " : " AS ID ").num(id);
            goBack();
            if (mode_ == Mode::Browse)   // BROWSE comes back on the preset just saved (a stray OK then loads what it just saved)
                for (int i = 0; i < browseCount(); ++i) if (browseItem(i) == id) browseIdx_ = i;
            say(t.c_str());
            dirty();
            return;
        }
        case PresetSave::NoCard: say("NO PRESET FOLDER ON THE CARD"); break;
        case PresetSave::Full: say("THE PRESET LIST IS FULL"); break;
        case PresetSave::WriteFailed: {
            const char* why = sv_.cardProblem();
            say(why ? why : "THE CARD COULD NOT BE WRITTEN");
            break;
        }
        case PresetSave::Exists: say("A FILE WITH THAT NAME IS ALREADY THERE"); break;
        case PresetSave::Missing: say("THE PRESET TO REPLACE IS GONE"); break;
        case PresetSave::BadPreset: say("THAT NAME OR SOUND CANNOT BE SAVED"); break;
    }
    dirty();
}

}  // namespace app
