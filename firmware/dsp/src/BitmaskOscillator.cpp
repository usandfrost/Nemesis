#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/BitmaskOscillator.h>

#include <array>
#include <cmath>
#include <cstddef>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;
    }

    const float* BitmaskOscillator::sharedCarrier() noexcept
    {
        // kFrameLength points plus a guard point, so the interpolator can read [i+1] at i =
        // kFrameLength - 1 without a wrap branch in the hot path. The guard is table[0] rather than
        // sin(2*pi) so the table is EXACTLY periodic — sin(2*pi) evaluates to ~-2.4e-16, and the
        // discrepancy would land in the one interval the agreement keystone reads across.
        //
        // The first kFrameLength entries use the same expression as the generator's own carrier
        // (`MaskTableGenerator.cpp::writeSine`); that identity is what makes the two views'
        // agreement bit-exact, so the two formulas must not be allowed to drift apart.
        // STATIC STORAGE, not a heap buffer. `std::vector` here would make this function's first
        // call allocate — and it is reached from `setMask`, i.e. from the audio thread — so the
        // `noexcept` would be a lie and the RT-safety claim would hold only after a warm-up nobody
        // guarantees. 8 KB of static const float has neither problem.
        static const std::array<float, static_cast<std::size_t> (kFrameLength) + 1> table = [] {
            std::array<float, static_cast<std::size_t> (kFrameLength) + 1> t {};
            for (int i = 0; i < kFrameLength; ++i)
                t[static_cast<std::size_t> (i)] = static_cast<float> (
                    std::sin (2.0 * kPi * static_cast<Real> (i) / static_cast<Real> (kFrameLength)));
            t[static_cast<std::size_t> (kFrameLength)] = t[0];
            return t;
        }();

        return table.data();
    }

    void BitmaskOscillator::prepare (Wide sampleRate)
    {
        prepareCommon (sampleRate, kDefaultOversample, kHighOversample, true);
    }

    void BitmaskOscillator::prepare (Wide sampleRate, int oversampleFactor)
    {
        // The high slot is designed as a BYPASS (factor 1), not as a second copy of the fixed
        // factor. Two reasons, and the second is why a mutation survived until it was changed:
        // it costs no FIR design or delay line, and it makes the `adaptive_` guard in
        // setFrequency LOAD-BEARING — without that guard a fixed-mode note above kEscalateHz
        // would select this slot and fall to the naive readout, which is observable. Designing
        // both slots identically made the guard unobservable and therefore unprovable.
        prepareCommon (sampleRate, oversampleFactor, 1, false);
    }

    void BitmaskOscillator::prepareCommon (Wide sampleRate, int baseFactor, int highFactor, bool adaptive)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        invSampleRate_ = 1.0 / sampleRate_; // FIREFORK N3
        carrier_ = sharedCarrier();
        adaptive_ = adaptive;

        // Designs BOTH decimating FIRs and allocates their delay lines. This is the ONLY place that
        // may happen — both the clean/dirty switch and the pitch escalation are runtime selections
        // precisely so neither ever lands here. Before setFrequency(), which reads the active factor
        // back to resolve the OS-rate increment.
        osBase_.prepare (baseFactor);
        osHigh_.prepare (highFactor);

        // Force the shared DC table now, off the audio thread — its first use allocates, and the
        // hot path must never be the thing that triggers it.
        (void) MaskTableGenerator::dcOffsetFor (0);
        dc_ = MaskTableGenerator::dcOffsetFor (mask_);

        setFrequency (freqHz_);
        reset();
    }
}
