#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/CathodeOscCore.h>

#include <cmath>

namespace nemesis::dsp
{
    void CathodeOscCore::prepare (Wide sampleRate, int pdOversample, int xmodOversample)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 1.0;
        invSampleRate_ = 1.0 / sampleRate_; // FIREFORK N3
        for (Slot& s : slots_)
        {
            s.wt.setSampleRate (sampleRate_);
            s.pd.prepare (sampleRate_, pdOversample);
            if (!lazyMacro_) // FIREFORK: lazy mode allocates this in setMode(Macro) instead
                s.macro.prepare (sampleRate_); // allocates (pimpl + upstream carve) — off the audio thread
            // [T-MASK-03 leaf D] The ADAPTIVE overload deliberately: the bitmask slot's factor is
            // the leaf-C2 pitch-adaptive default (×4 / ×8 selected at note-on), NOT `pdOversample`
            // — that argument is the PD slot's contract and forwarding it here would silently
            // re-decide a ruled default. Designs two FIRs and allocates; off the audio thread.
            s.bm.prepare (sampleRate_);
            // [T-CMP-15] The pulse takes the rate only — it re-resolves mip selection against it, and
            // an unbound pulse is silent until `setPulseWavetable`, so there is nothing to allocate.
            s.pulse.setSampleRate (sampleRate_);
        }
        for (std::size_t i = 0; i < static_cast<std::size_t> (kNumMixSlots); ++i)
        {
            pdStage_[i] = {};
            if (pdSeat_[i] != nullptr)
                pdSeat_[i]->drop(); // [T-CMP-37 L3] the PD decimators were just re-prepared
        }
        // [T-CMP-04 leaf 1 / D-M-0208] The sync restart's band-limited step (M1-19) — ONE, since only
        // OSC 1 is ever synced. Defaults are M1-19's own (64x sub-sample resolution over a 32-sample
        // support), so [T-ARCH-12] it reads the process-wide shared design (built by the first prepare,
        // never rebuilt) and allocates only its accumulator here, off the audio thread; the insertion
        // path is zero-alloc.
        syncBlep_.prepare();
        syncMasterPhase_ = 0.0;
        syncMasterRead_ = 0.0;
        syncSlavePhase_ = 0.0;
        syncCrossU_ = -1.0;
        syncSeedPending_ = true;
        // [T-CMP-02] The sub takes the rate too — it re-resolves its mip selection against it, and
        // an unbound sub is silent until `setSubWavetable`, so there is nothing to allocate here.
        sub_.setSampleRate (sampleRate_);
        // [T-CMP-03 leaf 1] The cross-mod lockstep pair. Clamped to the scratch buffer's size: the
        // OS-phase array is a fixed member (RT-safety), so a caller asking for more phases than it
        // holds is clamped rather than allowed to write past it. Designs two FIRs each and
        // allocates; off the audio thread.
        const int xf = xmodOversample < 1 ? 1 : (xmodOversample > kMaxXmodOversample ? kMaxXmodOversample : xmodOversample);
        xmodA_.prepare (xf);
        xmodB_.prepare (xf);
        aPhase_.fill (0.0f);
        // [T-CMP-03 leaf 2a] The FM leg's pair, at the same factor.
        fmA_.prepare (xf);
        fmB_.prepare (xf);
        mPhase_.fill (0.0f);
        for (AdsrEnvelope& e : oscEnv_)
            e.prepare (sampleRate_); // [T-CGUI-58 leaf 2]
        // [stereo voice S4] The side decimators: copies of each mix slot's own, so the side is band-limited
        // and delayed exactly as the mid is. Allocates; off the audio thread.
        if (sideOs_ == nullptr)
            sideOs_ = std::make_unique<SideDecimators>();
        for (std::size_t i = 0; i < static_cast<std::size_t> (kNumMixSlots); ++i)
        {
            sideOs_->pd[i] = slots_[i].pd.oversampler();
            sideOs_->bmBase[i] = slots_[i].bm.baseOversampler();
            sideOs_->bmHigh[i] = slots_[i].bm.highOversampler();
            sideOs_->pd[i].reset();
            sideOs_->bmBase[i].reset();
            sideOs_->bmHigh[i].reset();
        }
        sidePrev_ = { false, false };
        sideNow_ = { false, false };
        sideOs_->ringA = xmodA_; // the ring's pair, prepared above at the cross-mod factor
        sideOs_->ringB = xmodB_;
        sideOs_->ringA.reset();
        sideOs_->ringB.reset();
        sideRingPrev_ = false;
        sideRingNow_ = false;
        // Re-resolve every slot's frequency against the new sample rate.
        for (int i = 0; i < kNumSlots; ++i)
            updateFrequency (i);
        // [row 323] The collapse fade, per sample; a fresh core lands any collapse at once.
        const Real fade = kStackFadeSeconds * sampleRate_;
        stackFadeStep_ = static_cast<float> (1.0 / (fade >= 1.0 ? fade : 1.0));
        const Real xfade = kStackXfadeSeconds * sampleRate_;
        stackXfadeStep_ = static_cast<float> (1.0 / (xfade >= 1.0 ? xfade : 1.0));
        stackFresh_ = true;
        // [T-CAL-23 G1] the A/B switches: the edge fade's step and priming, the level match's glide
        resolveXmodFade();
        xmodFresh_ = true;
        stackMatchCoef_ = static_cast<float> (std::exp (-1.0 / (kStackMatchGlideSeconds * sampleRate_)));
        stackMatch_.fill (1.0f);
    }

    void CathodeOscCore::updateTuning (int slot) noexcept
    {
        Slot& s = slots_[slot];
        const Real semitoneOffset = static_cast<Real> (s.octave) * 12.0
                                      + static_cast<Real> (s.semitones)
                                      + s.cents / 100.0;
        s.mult = std::exp2 (semitoneOffset / 12.0);
        updateFrequency (slot);
    }
}
