#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/NoiseSource.h>

#include <cmath>

namespace nemesis::dsp
{
    void NoiseSource::prepare (Real sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        // One-pole low-pass feedback coefficient for the tape roll-off: a = e^(-2 pi fc / fs).
        constexpr Real kTwoPi = 6.283185307179586;
        tapeCoef_ = std::exp (-kTwoPi * kTapeCutoffHz / sampleRate_);

        // Crackle: per-sample trigger probability (events/sec / fs) and the pop-envelope decay
        // (e^(-1/(tau*fs)) -> the click amplitude decays by 1/e over kVinylPopTauS seconds).
        popProb_ = kVinylPopRate / sampleRate_;
        popDecay_ = std::exp (-1.0 / (kVinylPopTauS * sampleRate_));

        reset();
    }

    void NoiseSource::reset() noexcept
    {
        rng_ = seed_;
        b0_ = b1_ = b2_ = b3_ = b4_ = b5_ = b6_ = 0.0;
        lp_ = 0.0;
        pop_ = 0.0;
    }
}
