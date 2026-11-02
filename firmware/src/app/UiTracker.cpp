#include "app/Ui.h"

// The tracker: STEP's third view. It is a second way into the same steps, so nothing here is new data: the row is a place on screen, the step it stands for is row % lane length,
// and an edit is an edit of Step, as the other two views make it. Ui.cpp only calls in here at the few places a key or a knob behaves differently. docs/TRACKER.md says why it is as it is.
namespace app {

#define DOT " \xC2\xB7 "      // " · "
#define RING "\xE2\x97\x8B"  // "○", the CLEAR key as it is printed

constexpr int kNoteLimit = 24;   // a step's note against the lane's root: what Step and the file hold, as on the NOTE knob

int Ui::trackerNote() const {
    const Lane& l = lane();
    return trig(l, cursor_, false) ? l.st[cursor_].note : trLast_[sel_];
}

void Ui::trackerSample(Text& name, Text& folder, bool& pending) const {
    pending = false;
    if (!lib_.count) { name.add("?"); return; }
    Pick p;
    p.lane = sel_;
    p.step = cursor_;
    // a step with no sample of its own plays the lane's; pending, as on the knob's cell: the folder knob is elsewhere and the next turn takes what is shown
    const int own = pickNow(p), cat = pickFolder(p), ls = laneSampleNow(sel_);
    pending = own < 0 ? lib_.items[ls].cat != cat : pickMoved(p);
    const int s = pending ? pickShown(p) : own < 0 ? ls : own;
    name.add(lib_.items[s].name);
    folder.add(lib_.cats[cat]).ch(' ').num(lib_.catPlace(s) + 1).ch('/').num(lib_.catSize(cat));
}

void Ui::trackerFix() {
    if (!tracker()) {   // left the view, by any way there is
        trackerClose();
        if (held_.tracker) held_.on = false;   // (elsewhere its key would tap a step on letting go)
        return;
    }
    const Lane& l = lane();
    const int rows = trackerRows();
    if (trRow_ >= rows) trRow_ = rows > 0 ? rows - 1 : 0;
    cursor_ = l.n ? trRow_ % l.n : 0;
    stepPage_ = cursor_ / 8;   // (the other two views come back on this step)
    if (trPop_ && (trPopRow_ != trRow_ || trPopLane_ != sel_ || trPopPat_ != tp_.pat)) trackerClose();   // it closes when you move on
    if (held_.on && (held_.idx != cursor_ || (held_.tracker && held_.i != sel_))) held_.on = false;      // a key held on a cell that has gone (a pattern switched under it)
    if (lockOn_ && lockLane_ != sel_) lockOn_ = false;                                                   // (PART moves the lane without selectLane)
}

void Ui::trackerClose() {
    if (trPop_) dirty();
    trPop_ = false;
    trRun_ = false;
    trStop_ = 0;
}

void Ui::trackerEnter() {
    const Lane& l = lane();
    if (!l.n || trRow_ % l.n != cursor_) trRow_ = cursor_;
    trackerFix();
}

void Ui::trackerOpen() {
    trPop_ = true;
    trPopRow_ = trRow_;
    trPopLane_ = sel_;
    trPopPat_ = tp_.pat;
    dirty();
}

// One undo for a run of edits: as long as the popover stays open on this step, undo goes back to before the first of them.
void Ui::trackerSnap(const char* label, bool fresh) {
    if (!fresh && undo_.kind == U::StepSnap && undo_.pat == tp_.pat && undo_.lane == sel_ && undo_.idx == cursor_) return;
    remember(U::StepSnap, label);
    undo_.pat = tp_.pat;
    undo_.lane = (uint8_t)sel_;
    undo_.idx = (uint8_t)cursor_;
    undoStep_ = lane().st[cursor_];
}

// Every edit starts here: an empty step gets the lane's last note and is switched on, the way a tap switches it (Step.ov), over a rhythm or not.
Step& Ui::trackerBegin() {
    Lane& l = lane();
    Step& st = l.st[cursor_];
    Str<40> lab;
    lab.add("EDIT OF STEP ").num(cursor_ + 1);
    trackerSnap(lab.c_str(), !trRun_);
    trRun_ = true;
    if (!trig(l, cursor_, false)) {
        st.note = trLast_[sel_];     // first: the sequencer may read the step as soon as it is on
        st.ov = !st.ov;
    }
    trackerOpen();
    trStop_ = 0;
    return st;
}

// The next note of the scale that is not the one playing now: a note outside the scale plays as its neighbour, so going to that neighbour would change nothing you can hear.
static int nextInScale(const Project& pj, int root, int note, int dir) {
    const int now = scaleNote(root + note, pj.key, pj.scale);
    for (int n = note + dir; n >= -kNoteLimit && n <= kNoteLimit; n += dir)
        if (root + n != now && scaleNote(root + n, pj.key, pj.scale) == root + n) return n;
    return note;
}

// `d`: semitones (with IN SCALE, notes of the scale), or with `octave` octaves. An octave that would leave the limit stays where it is: stopping at the limit would change the note.
void Ui::trackerMove(int d, bool octave) {
    Step& st = trackerBegin();
    const int root = lane().root, dir = signOf(d);
    int n = st.note;
    if (octave) {
        if (n + d * 12 >= -kNoteLimit && n + d * 12 <= kNoteLimit) n += d * 12;
        else trStop_ = (int8_t)dir;
    } else if (inScale()) {
        for (int k = d < 0 ? -d : d; k > 0; --k) {
            const int to = nextInScale(pj_, root, n, dir);
            if (to == n) { trStop_ = (int8_t)dir; break; }
            n = to;
        }
    } else {
        if (n + d < -kNoteLimit || n + d > kNoteLimit) trStop_ = (int8_t)dir;
        n = clampi(n + d, -kNoteLimit, kNoteLimit);
    }
    st.note = (int8_t)n;
    trLast_[sel_] = (int8_t)n;
    dirty();
}

// The keys around OK move the cursor, or with OK down the note. OK itself acts when it comes up: only then is it known to be a tap. Any other key or a knob while it is down makes it no tap
// (Ui::keyDown, Ui::turn, trackerLayerDown).
bool Ui::trackerKey(Key k) {
    if (k == Key::Ok) {
        trOkDown_ = true;
        trOkUsed_ = false;
        return true;
    }
    const bool rows = k == Key::Up || k == Key::Down, lanes = k == Key::Left || k == Key::Right;
    if (!rows && !lanes) return false;
    const int dir = k == Key::Down || k == Key::Right ? 1 : -1;
    if (trOkDown_) {
        trackerMove(rows ? -dir : dir, rows);   // up is up an octave
        return true;
    }
    held_.on = false;   // a layer key held belongs to the cell it was pressed on
    if (lanes) selectLane(mod(sel_ + dir, kLanes));
    else {
        const int n = trackerRows(), pages = (n + kTrackerRows - 1) / kTrackerRows;
        if (shift_) trRow_ = mod(trRow_ + dir * kTrackerRows, pages * kTrackerRows);
        else trRow_ = mod(trRow_ + dir, n);
    }
    trackerFix();
    dirty();
    return true;
}

void Ui::trackerOkUp() {
    const bool tap = trOkDown_ && !trOkUsed_;
    trOkDown_ = false;
    if (!tap) return;
    settle();
    if (!tracker()) return;
    Lane& l = lane();
    if (!trig(l, cursor_, false)) { trackerBegin(); return; }
    if (trPop_) { trackerClose(); return; }
    trLast_[sel_] = l.st[cursor_].note;   // a note you have looked at is the next one to enter
    trackerOpen();
}

// CLEAR takes the step away, all of it: a note put here later starts clean.
void Ui::trackerClear() {
    Lane& l = lane();
    Step& st = l.st[cursor_];
    Text t;
    if (!trig(l, cursor_, false)) {
        t.add("STEP ").num(cursor_ + 1).add(" IS EMPTY");
        say(t.c_str());
        return;
    }
    Str<40> lab;
    lab.add("STEP ").num(cursor_ + 1).add(" REMOVED");
    trackerSnap(lab.c_str(), true);
    st.ov = !st.ov;
    st.nLocks = 0;
    st.note = 0;
    st.vel = 100;
    st.gate = l.gate;
    st.prob = 100;
    st.chord = st.inv = st.spread = st.strum = 0;
    trackerClose();
    t.add("STEP ").num(cursor_ + 1).add(" REMOVED" DOT "SHIFT + " RING " UNDOES");
    say(t.c_str());
}

// A layer key is a lane here. Held, it is still what it is in the editor: a knob edits the step, and letting go after a while opens its lock (layerUp).
void Ui::trackerLayerDown(int i) {
    trackerNoTap();
    selectLane(i);
    trackerFix();
    held_ = Held();
    held_.on = true;
    held_.i = i;
    held_.idx = cursor_;
    held_.at = sv_.nowMs();
    held_.tracker = true;
}

void Ui::trackerTouch(B id) {
    switch (id) {
        case B::Velocity: case B::GateB: case B::Prob: case B::ChordB: case B::Inv: case B::StepSample: case B::StepSlice:
            trackerBegin();
            break;
        case B::Folder:   // the folder knob moves the knob's place, nothing is picked: the popover shows it
            trackerOpen();
            break;
        default: break;   // (NOTE and OCTAVE begin in trackerMove; IN SCALE edits no step)
    }
}

}  // namespace app
