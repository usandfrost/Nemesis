#include "fx/FxAmp.h"

#include <math.h>
#include <string.h>

namespace fx {

using nemesis::dsp::GampEffect;

static const float kRestMs = 500.0f;     // the power amp's sag and the DC blockers have settled: what is left is small on the next note
static const float kSilent = 1e-7f;      // -140 dBFS: the amp's own gain, near 60 dB at the most, lifts anything louder toward hearing

void FxAmp::prepare(float sr) {
    amp_.prepare(sr, 32, 1);   // every model at 1x: half the cost of 2x, and its soft clips already band-limit themselves (ADAA)
    rest_ = (int)(kRestMs * 0.001f * sr);
    quiet_ = 0;
    amp_.setMono(true);
    amp_.setMixPercent(100.0f);
    amp_.setMic(0);            // dynamic
    fade_.setTime(6.0f, sr);
    fade_.snap(0.0f);
    primed_ = false;
}

void FxAmp::apply() {
    static const int kModel[5] = {0, (int)GampEffect::Model::Clean, (int)GampEffect::Model::Crunch, (int)GampEffect::Model::Lead, (int)GampEffect::Model::Bass};
    static const int kCab[4] = {(int)GampEffect::Cabinet::Direct, (int)GampEffect::Cabinet::Single12, (int)GampEffect::Cabinet::Dual12, (int)GampEffect::Cabinet::Quad12};
    const AmpSettings& s = want_;
    if (s.model <= 0) return;
    // every setter re-resolves a good part of the amp (about 5 us on the host: nearly a block's worth): only what moved is pushed
    const bool all = !primed_ || s.model != have_.model;
    if (all) amp_.setModel(kModel[s.model > 4 ? 4 : s.model]);
    if (all || s.drive != have_.drive) amp_.setGainDb(s.drive * 36.0f);
    if (all || s.bassDb != have_.bassDb) amp_.setBassPercent(50.0f + s.bassDb / 12.0f * 50.0f);
    if (all || s.midDb != have_.midDb) amp_.setMidPercent(50.0f + s.midDb / 12.0f * 50.0f);
    if (all || s.trebleDb != have_.trebleDb) amp_.setTreblePercent(50.0f + s.trebleDb / 12.0f * 50.0f);
    if (all || s.presence != have_.presence) amp_.setPresencePercent(s.presence * 100.0f);
    if (all || s.cab != have_.cab) amp_.setCab(kCab[s.cab < 0 ? 0 : s.cab > 3 ? 3 : s.cab]);
    if (all || s.levelDb != have_.levelDb) amp_.setLevelDb(s.levelDb);
    have_ = s;
    primed_ = true;
}

void FxAmp::set(const AmpSettings& s) { want_ = s; }

bool FxAmp::running() const { return want_.model > 0 || fade_.cur > 1e-4f; }

void FxAmp::process(float* l, float* r, int n) {
    if (!running()) return;
    if (n > 64) n = 64;
    if (want_.model <= 0) {
        fade_.tgt = 0.0f;                                  // switched off: out through the dry signal
    } else if (primed_ && have_.model == want_.model) {
        fade_.tgt = 1.0f;
        if (!(have_ == want_)) { apply(); quiet_ = 0; }    // every other knob glides inside the class (only while it runs, so it wakes to finish)
    } else if (fade_.cur > 0.02f) {
        fade_.tgt = 0.0f;                                  // a new model: fade out first
    } else {
        apply();                                           // switch at the bottom, then fade in
        quiet_ = 0;
        fade_.tgt = 1.0f;
    }
    float pk = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float a = fabsf(l[i]), b = fabsf(r[i]);
        if (a > pk) pk = a;
        if (b > pk) pk = b;
    }
    if (pk > kSilent) quiet_ = 0;
    else if (quiet_ >= rest_) { fade_.snap(fade_.tgt); return; }   // silence in, silence out: the buffers stay as they are, and the amp picks up where it stopped
    else quiet_ += n;
    float dl[64], dr[64];
    memcpy(dl, l, sizeof(float) * (size_t)n);
    memcpy(dr, r, sizeof(float) * (size_t)n);
    for (int i = 0; i < n; ++i) a_[0][i] = a_[1][i] = 0.5f * (l[i] + r[i]);
    if (primed_) amp_.process(a_[0], a_[1], n);
    for (int i = 0; i < n; ++i) {
        const float f = primed_ ? fade_.next() : 0.0f;
        l[i] = dl[i] * (1.0f - f) + a_[0][i] * f;
        r[i] = dr[i] * (1.0f - f) + a_[0][i] * f;
    }
}

void FxAmp::reset() {
    amp_.reset();
    quiet_ = 0;
    fade_.snap(0.0f);
    primed_ = false;
}

}  // namespace fx
