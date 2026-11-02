#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/DeltaDecayEnvelope.h>

#include <cmath>

namespace nemesis::dsp
{
    void DeltaDecayEnvelope::prepare (Real sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        setDecayMs (decayMs_); // re-resolve the coefficient against the new rate
        reset();
    }

    void DeltaDecayEnvelope::setDecayMs (float ms) noexcept
    {
        decayMs_ = ms > 0.0f ? ms : 0.0f;

        // −60 dB at the set time: the `AdsrEnvelope` decay/release convention verbatim (see the class
        // note — one law, so a decay time means the same thing wherever it is set). A sub-sample time
        // collapses to factor 0, which is the blip case rather than a latch.
        const Real decaySamples = static_cast<Real> (decayMs_) * sampleRate_ * 0.001;
        coef_ = decaySamples > 0.0 ? std::pow (10.0, -3.0 / decaySamples) : 0.0;
    }
}
