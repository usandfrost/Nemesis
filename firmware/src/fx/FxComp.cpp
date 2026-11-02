#include "fx/FxComp.h"

#include <string.h>

namespace fx {

void FxComp::prepare(float sr) {
    comp_.prepare(sr, 32);
    comp_.setType(3);                    // Bus
    comp_.setSaturationEnabled(false);
    comp_.setMixPercent(100.0f);         // the dry blend is ours (smoothed)
    comp_.setMakeupDb(0.0f);
    comp_.setKneeDb(6.0f);
    on_.setTime(10.0f, sr);
    makeup_.setTime(10.0f, sr);
    mix_.setTime(10.0f, sr);
    on_.snap(0.0f);
    makeup_.snap(1.0f);
    mix_.snap(1.0f);
}

void FxComp::set(bool active, float threshDb, float ratio, float attackMs, float releaseMs, float makeupDb, float mix01, float scHpfHz) {
    if (threshDb != th_) { th_ = threshDb; comp_.setThresholdDb(threshDb); }
    if (ratio != ra_) { ra_ = ratio; comp_.setRatio(ratio); }
    if (attackMs != at_) { at_ = attackMs; comp_.setAttackMs(attackMs); }
    if (releaseMs != re_) { re_ = releaseMs; comp_.setReleaseMs(releaseMs); }
    if (scHpfHz != hp_) { hp_ = scHpfHz; comp_.setSidechainHpHz(scHpfHz); }
    on_.set(active ? 1.0f : 0.0f);
    makeup_.set(dbToLin(makeupDb));
    mix_.set(mix01);
}

void FxComp::process(float* l, float* r, int n) {
    if (!running()) return;
    if (n > 64) n = 64;
    memcpy(dry_[0], l, sizeof(float) * (size_t)n);
    memcpy(dry_[1], r, sizeof(float) * (size_t)n);
    comp_.process(l, r, n);
    for (int i = 0; i < n; ++i) {
        const float a = on_.next(), g = makeup_.next(), m = mix_.next() * a;
        l[i] = dry_[0][i] * (1.0f - m) + l[i] * g * m;
        r[i] = dry_[1][i] * (1.0f - m) + r[i] * g * m;
    }
}

void FxComp::reset() {
    comp_.reset();
    on_.snap(0.0f);
}

}  // namespace fx
