#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/PhaseDistortionOscillator.h>

#include <array>
#include <cmath>
#include <cstddef>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;
    }

    const float* PhaseDistortionOscillator::bandlimitedSineTable() noexcept
    {
        // One shared table for all oscillators; the magic-static guard's one-time init runs off
        // the audio thread (forced from prepare()). kSineTableSize+1 with a guard point equal to
        // table[0] so linear interpolation never indexes past the end. (Mirrors the punch sine
        // table; a shared SineTable primitive is a candidate later refactor.)
        static const std::array<float, kSineTableSize + 1> table = [] {
            std::array<float, kSineTableSize + 1> t {};
            for (int i = 0; i <= kSineTableSize; ++i)
                t[static_cast<std::size_t> (i)] = static_cast<float> (
                    std::sin (2.0 * kPi * static_cast<Real> (i) / static_cast<Real> (kSineTableSize)));
            return t;
        }();
#if NEMESIS_FORK_EXACT // FIREFORK P4: a copy of the table (the same floats: the compiler may fold the original at build time,
                      // a run-time sin would round differently), in storage the build may place (the firmware: DTCM)
#ifndef NEMESIS_FORK_FAST_DATA
#define NEMESIS_FORK_FAST_DATA
#endif
        NEMESIS_FORK_FAST_DATA static std::array<float, kSineTableSize + 1> fastTable;
        static bool copied = false;
        if (!copied)
        {
            fastTable = table;
            copied = true;
        }
        return fastTable.data();
#else
        return table.data();
#endif
    }

    void PhaseDistortionOscillator::prepare (Wide sampleRate, int oversampleFactor)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        invSampleRate_ = 1.0 / sampleRate_; // FIREFORK N3
        factor_ = oversampleFactor < 1 ? 1 : oversampleFactor;
        sine_ = bandlimitedSineTable(); // forces the one-time table build off the audio thread
        os_.prepare (factor_);
        setFrequency (freqHz_); // re-resolve inc_/incOs_ against the new rate + factor
        recomputeWarp();
        reset();
    }

    void PhaseDistortionOscillator::reset() noexcept
    {
        phase_ = 0.0;
        os_.reset();
    }
}
