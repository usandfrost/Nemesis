#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/EnvelopeFollower.h>

#include <cmath>

namespace nemesis::dsp
{
    // §12 capability 7, and the only libm in this primitive — control-rate, per the §0 convention
    // (the hot path in the header is pure arithmetic plus at most one arm64 fsqrt).
    Real EnvelopeFollower::alphaFor (float ms, Real sampleRate) noexcept
    {
        // One enforcement point for both rails (T-B26: prefer restructuring a guard conjunction over
        // labelling it). A negative time would give α = exp(+1/(τ·fs)) > 1 — an unstable one-pole —
        // and τ·fs = 0 is a division by zero the -Ofast release config does not guarantee to be inf.
        if (!(ms > 0.0f) || !(sampleRate > 0.0))
            return 0.0;

        const Real tau = static_cast<Real> (ms) * 1.0e-3; // ms → seconds
        return std::exp (-1.0 / (tau * sampleRate));
    }

    void EnvelopeFollower::prepare (Real sampleRate) noexcept
    {
        sampleRate_ = sampleRate;
        attackCoef_ = alphaFor (attackMs_, sampleRate_);
        releaseCoef_ = alphaFor (releaseMs_, sampleRate_);
        reset();
    }

    void EnvelopeFollower::setAttackMs (float ms) noexcept
    {
        attackMs_ = ms > 0.0f ? ms : 0.0f; // the stored value is the truth the read-back reports
        attackCoef_ = alphaFor (attackMs_, sampleRate_);
    }

    void EnvelopeFollower::setReleaseMs (float ms) noexcept
    {
        releaseMs_ = ms > 0.0f ? ms : 0.0f;
        releaseCoef_ = alphaFor (releaseMs_, sampleRate_);
    }
}
