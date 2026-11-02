#include "app/MidiMap.h"

namespace app {

int lanesOnChannel(const Project& pj, int ch) {
    int n = 0;
    for (int i = 0; i < kLanes; ++i) n += pj.midi[i].ch == ch;
    return n;
}

bool routeIn(const Project& pj, int pattern, int ch, int note, MidiRoute& out) {
    if (ch < 1 || ch > 16 || pattern < 0 || pattern >= kPatterns) return false;
    const int shared = lanesOnChannel(pj, ch);
    for (int i = 0; i < kLanes; ++i) {
        if (pj.midi[i].ch != ch) continue;
        if (shared > 1 && pj.midi[i].note != note) continue;
        const Lane& l = pj.patterns[pattern].lanes[i];
        out.lane = i;
        out.sampler = isSampler(pj, l.part);
        if (shared > 1) out.pitch = out.sampler ? 0 : l.root;   // a trigger note: the lane's own sound
        else if (out.sampler) {   // as far as the sequencer transposes a sample
            const int d = note - pj.midi[i].note;
            out.pitch = d < -48 ? -48 : d > 48 ? 48 : d;
        } else out.pitch = note;
        return true;
    }
    return false;
}

int noteOut(const Project& pj, int lane, int pitch, bool sampler) {
    const LaneMidi& m = pj.midi[lane];
    int n;
    if (m.ch && lanesOnChannel(pj, m.ch) > 1) n = m.note;   // a shared channel: the lane is its trigger note
    else n = sampler ? m.note + pitch : pitch;
    return n < 0 ? 0 : n > 127 ? 127 : n;
}

}  // namespace app
