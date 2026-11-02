#include "fx/FxSection.h"

#include <math.h>

#include <string.h>

namespace fx {

static const float kSilent = 1e-6f;   // what counts as nothing coming in: -120 dBFS

using namespace app;

void FxSection::init(float sr) {
    sr_ = sr;
    delay_.prepare(sr);
    reverb_.prepare(sr);
    chorus_.prepare(sr);
    comp_.prepare(sr);
    for (int p = 0; p < kParts; ++p) {
        amp_[p].prepare(sr);
        for (int e = 0; e < EFFECTS; ++e) {
            send_[p][e].setTime(6.0f, sr);
            send_[p][e].snap(0.0f);
        }
    }
    for (int e = 0; e < EFFECTS; ++e) { sleeping_[e] = true; quiet_[e] = 0; peak_[e] = 0.0f; byp_[e].setTime(10.0f, sr); byp_[e].snap(1.0f); }
    inited_ = true;
    haveApplied_ = false;
}

void FxSection::setDegrade(uint16_t fxMask, const uint16_t ampMask[kParts]) {
    if (fxMask != degFx_) { degFx_ = fxMask; degDirty_ = true; }
    for (int p = 0; p < kParts; ++p) if (ampMask[p] != degAmp_[p]) { degAmp_[p] = ampMask[p]; degDirty_ = true; }
}

void FxSection::apply(const FxSettings& fx, int bpm) {
    if (!inited_) return;
    if (haveApplied_ && !degDirty_ && bpm == bpm_ && memcmp(&applied_, &fx, sizeof(fx)) == 0) return;
    degDirty_ = false;
    applied_ = fx;
    bpm_ = bpm;
    haveApplied_ = true;
    auto R = [&](int view, const int16_t* v, int i) { return (float)fxReal(fxDef(view, i), v[i]); };
    for (int p = 0; p < kParts; ++p)
        for (int e = 0; e < EFFECTS; ++e) {
            const float s = fx.send[p][e] < 0 ? 0.0f : fx.send[p][e] > 127 ? 1.0f : fx.send[p][e] / 127.0f;
            send_[p][e].set(s);
        }
    const int16_t* d = fx.delay;
    delay_.set((float)delayMs(d[0], d[1], bpm), R(FX_DELAY, d, 2) / 100.0f, R(FX_DELAY, d, 3), R(FX_DELAY, d, 4) / 100.0f, R(FX_DELAY, d, 5) / 100.0f,
               R(FX_DELAY, d, 6), R(FX_DELAY, d, 7) / 100.0f);
    const int16_t* r = fx.reverb;
    reverb_.set(R(FX_REVERB, r, 0) / 100.0f, R(FX_REVERB, r, 1), R(FX_REVERB, r, 2), R(FX_REVERB, r, 3), R(FX_REVERB, r, 4) / 100.0f,
                R(FX_REVERB, r, 5) / 100.0f, R(FX_REVERB, r, 6), R(FX_REVERB, r, 7) / 100.0f * kReverbTrim);
    const int16_t* c = fx.chorus;
    chorus_.set(R(FX_CHORUS, c, 0), R(FX_CHORUS, c, 1) / 100.0f, R(FX_CHORUS, c, 2), R(FX_CHORUS, c, 3) / 100.0f, R(FX_CHORUS, c, 4) / 100.0f,
                R(FX_CHORUS, c, 5), R(FX_CHORUS, c, 6), R(FX_CHORUS, c, 7) / 100.0f);
    const int16_t* k = fx.comp;
    // the ladder's bypass: bits are cost::Action (DelayOff 11, ReverbOff 12, ChorusOff 13, CompOff 14)
    byp_[DELAY].set((degFx_ >> 11) & 1 ? 0.0f : 1.0f);
    byp_[REVERB].set((degFx_ >> 12) & 1 ? 0.0f : 1.0f);
    byp_[CHORUS].set((degFx_ >> 13) & 1 ? 0.0f : 1.0f);
    comp_.set(k[0] != 0 && !((degFx_ >> 14) & 1), R(FX_COMP, k, 1), (float)compRatio(k[2]), R(FX_COMP, k, 3), R(FX_COMP, k, 4), R(FX_COMP, k, 5), R(FX_COMP, k, 6) / 100.0f,
              R(FX_COMP, k, 7));
    for (int p = 0; p < kParts; ++p) {
        const int16_t* a = fx.amp[p];
        AmpSettings s;
        s.model = a[0];
        if ((degAmp_[p] >> 10) & 1) s.model = 0;                       // AmpOff
        else if (((degAmp_[p] >> 9) & 1) && s.model > 1) s.model = 1;  // AmpToClean
        s.drive = R(FX_AMP, a, 1) / 100.0f;
        s.bassDb = R(FX_AMP, a, 2);
        s.midDb = R(FX_AMP, a, 3);
        s.trebleDb = R(FX_AMP, a, 4);
        s.presence = R(FX_AMP, a, 5) / 100.0f;
        s.cab = a[6];
        s.levelDb = R(FX_AMP, a, 7);
        amp_[p].set(s);
    }
}

bool FxSection::active() const {
    if (!inited_ || !haveApplied_) return false;
    for (int p = 0; p < kParts; ++p) if (amp_[p].running()) return true;
    for (int e = 0; e < EFFECTS; ++e) {
        if (byp_[e].tgt < 0.5f && byp_[e].cur < 1e-4f) continue;
        if (!sleeping_[e]) return true;
        for (int p = 0; p < kParts; ++p) if (send_[p][e].tgt > 0.0f) return true;   // asleep with a send up: the section has to see the signal that wakes it
    }
    return comp_.running();
}

void FxSection::process(float* const partL[kParts], float* const partR[kParts], const float gain[kParts], uint32_t sounding, float* outL, float* outR, int n) {
    for (int off = 0; off < n; off += kChunk) {
        const int m = n - off < kChunk ? n - off : kChunk;
        float* pl[kParts];
        float* pr[kParts];
        for (int p = 0; p < kParts; ++p) { pl[p] = partL[p] + off; pr[p] = partR[p] + off; }
        block(pl, pr, gain, sounding, outL + off, outR + off, m);
    }
}

void FxSection::block(float* const partL[kParts], float* const partR[kParts], const float gain[kParts], uint32_t sounding, float* outL, float* outR, int n) {
    int live[kParts], nLive = 0;   // the parts with anything in their buffers: most of the seven are silent at any moment
    for (int p = 0; p < kParts; ++p) {
        if (amp_[p].running()) amp_[p].process(partL[p], partR[p], n);
        else if (!((sounding >> p) & 1)) continue;
        live[nLive++] = p;
        // the fader after the amp; the buffers then hold what the part contributes to the mix. A sample that is not a sane number is
        // dropped here, before it can enter a feedback loop (and the effects are flagged for clearing if one got in anyway: below).
        const float g = gain[p];
        for (int i = 0; i < n; ++i) {
            const float l = partL[p][i] * g, r = partR[p][i] * g;
            partL[p][i] = (l > -8.0f && l < 8.0f) ? l : 0.0f;
            partR[p][i] = (r > -8.0f && r < 8.0f) ? r : 0.0f;
        }
    }
    for (int i = 0; i < n; ++i) {
        float l = outL[i], r = outR[i];
        for (int k = 0; k < nLive; ++k) { l += partL[live[k]][i]; r += partR[live[k]][i]; }
        outL[i] = l;
        outR[i] = r;
    }
    for (int e = 0; e < EFFECTS; ++e) {
        bool sending = false;
        for (int p = 0; p < kParts; ++p) sending = sending || send_[p][e].tgt > 0.0f || !send_[p][e].settled();
        // bypassed by the ladder: faded out over a few ms, then not run at all (what is in its lines stays frozen until it is given back)
        if (byp_[e].tgt < 0.5f && byp_[e].cur < 1e-4f) { wasByp_[e] = true; continue; }
        if (wasByp_[e]) { wasByp_[e] = false; if (e == DELAY) delay_.wake(); else if (e == REVERB) reverb_.wake(); sleeping_[e] = false; quiet_[e] = 0; }
        if (!sending && sleeping_[e]) continue;
        if (sleeping_[e]) {   // a send up is not enough: something has to reach it. Woken here, so its settings are the ones just applied
            byp_[e].snap(byp_[e].tgt);        // (a bypass taken while asleep needs no fade: it must not wake the effect just to fade it out)
            if (byp_[e].tgt < 0.5f) { wasByp_[e] = true; continue; }
            bool heard = false;
            for (int k = 0; k < nLive && !heard; ++k) {
                const int p = live[k];
                const float g = send_[p][e].tgt;   // the same measure as `fed` below, or it would wake and fall back asleep
                if (g <= 0.0f) continue;
                const float limit = kSilent / g;
                for (int i = 0; i < n && !heard; ++i) heard = fabsf(partL[p][i]) > limit || fabsf(partR[p][i]) > limit;
            }
            if (!heard) {
                // nothing is going through it, so the sends can land at once and the first sound goes in at its level. With sound there they glide: a send
                // raised on a part already playing must not click
                for (int p = 0; p < kParts; ++p) send_[p][e].snap(send_[p][e].tgt);
                continue;
            }
            sleeping_[e] = false;
            quiet_[e] = 0;
            if (e == DELAY) delay_.wake();    // after the settings: its pointers go to the new time
            else if (e == REVERB) reverb_.wake();
            else chorus_.wake();
        }
        int feed[kParts], nFeed = 0;   // a silent part's send lands at once: nothing is going through it to click
        for (int p = 0, k = 0; p < kParts; ++p) {
            const bool isLive = k < nLive && live[k] == p;
            if (isLive) ++k;
            if (isLive && (send_[p][e].tgt > 0.0f || !send_[p][e].settled())) feed[nFeed++] = p;
            else send_[p][e].snap(send_[p][e].tgt);
        }
        float fed = 0.0f;
        for (int i = 0; i < n; ++i) {
            float l = 0.0f, r = 0.0f;
            for (int k = 0; k < nFeed; ++k) {
                const int p = feed[k];
                const float g = send_[p][e].next();
                l += partL[p][i] * g;
                r += partR[p][i] * g;
            }
            busL_[i] = l;
            busR_[i] = r;
            const float a = fabsf(l), c = fabsf(r);
            if (a > fed) fed = a;
            if (c > fed) fed = c;
        }
        float pk = 0.0f;
        switch (e) {
            case DELAY: pk = delay_.process(busL_, busR_, retL_, retR_, n); break;
            case REVERB: pk = reverb_.process(busL_, busR_, retL_, retR_, n); break;
            default: pk = chorus_.process(busL_, busR_, retL_, retR_, n); break;
        }
        // a return that is not finite or has run away: the loop holds garbage until it is cleared (main loop: the memory is megabytes);
        // until then it is kept out of the mix
        const bool bad = !(pk < 16.0f);
        if (bad) { fault_ = true; pk = 0.0f; }
        else for (int i = 0; i < n; ++i) { const float b = byp_[e].next(); outL[i] += retL_[i] * b; outR[i] += retR_[i] * b; }
        peak_[e] = pk;
        // asleep: nothing coming in and the tail below -100 dBFS for as long as the effect's longest delay plus a second (an echo is silent
        // until it arrives, so a long delay must not fall asleep between two echoes)
        const int need = (int)(sr_ * (e == DELAY ? FxDelay::kMaxMs * 0.001f + 1.5f : 1.0f));
        if (fed > kSilent || bad || !(pk <= 1e-5f)) quiet_[e] = 0;
        else if ((quiet_[e] += n) > need) { sleeping_[e] = true; quiet_[e] = 0; }
    }
    comp_.process(outL, outR, n);
}

void FxSection::clear() {
    if (!inited_) return;
    delay_.reset();
    reverb_.reset();
    chorus_.reset();
    comp_.reset();
    for (int p = 0; p < kParts; ++p) {
        amp_[p].reset();
        for (int e = 0; e < EFFECTS; ++e) send_[p][e].snap(send_[p][e].tgt);
    }
    for (int e = 0; e < EFFECTS; ++e) { sleeping_[e] = true; quiet_[e] = 0; peak_[e] = 0.0f; }
    fault_ = false;
    for (int e = 0; e < EFFECTS; ++e) wasByp_[e] = false;
    haveApplied_ = false;   // the next apply() pushes everything again
}

}  // namespace fx
