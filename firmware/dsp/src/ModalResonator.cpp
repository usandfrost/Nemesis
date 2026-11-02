#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ModalResonator.h>

#include <cmath>
#include <cstddef>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;
    }

    void ModalResonator::prepare (Wide sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (int i = 0; i < kMaxModes; ++i)
            recomputeMode (i); // re-resolve coefficients against the (possibly new) rate
        reset();
    }

    void ModalResonator::reset() noexcept
    {
        u_.fill (0.0);
        v_.fill (0.0);
    }

    void ModalResonator::setMode (int i, float freqHz, float decayMs, float gain) noexcept
    {
        if (i < 0 || i >= kMaxModes)
            return;
        freqHz_[static_cast<std::size_t> (i)] = freqHz;
        decayMs_[static_cast<std::size_t> (i)] = decayMs;
        gain_[static_cast<std::size_t> (i)] = static_cast<Real> (gain);
        recomputeMode (i);
    }

    void ModalResonator::recomputeMode (int i) noexcept
    {
        const std::size_t idx = static_cast<std::size_t> (i);

        // Frequency → rotation angle ω (clamped below Nyquist so cos/sin stay meaningful).
        const Wide maxHz = 0.49 * sampleRate_;
        Wide fHz = static_cast<Real> (freqHz_[idx]);
        if (fHz < 0.0)
            fHz = 0.0;
        if (fHz > maxHz)
            fHz = maxHz;
        const Real omega = 2.0 * kPi * fHz / sampleRate_;

        // T60 decay → pole radius r: r^(T60 samples) = 1e-3 (−60 dB). Clamp the decay time
        // above zero and r just below 1 so a huge decay can never reach marginal stability.
        Real decayMs = static_cast<Real> (decayMs_[idx]);
        if (decayMs < 0.1)
            decayMs = 0.1;
        const Real t60Samples = decayMs * 0.001 * sampleRate_;
        Real r = std::pow (10.0, -3.0 / t60Samples);
        if (r > kMaxRadius)
            r = kMaxRadius;

        rc_[idx] = r * std::cos (omega);
        rs_[idx] = r * std::sin (omega);
    }
}
