#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/Lfo.h>

#include <array>
#include <cmath>

namespace nemesis::dsp::detail
{
    // The shared LFO sine table, built once on first use (off the audio thread — the only libm in
    // this primitive). kLfoSineTableSize + 1 points over one cycle with a guard point so the hot
    // path's linear interp never reads past the end. table[i] = sin(2π·i / size).
    const Real* lfoSineTableData() noexcept
    {
        static constexpr Real kTwoPi = 6.283185307179586;
        static const std::array<Real, kLfoSineTableSize + 1> table = [] {
            std::array<Real, kLfoSineTableSize + 1> t {};
            for (std::size_t i = 0; i <= kLfoSineTableSize; ++i)
                t[i] = std::sin (kTwoPi * static_cast<Real> (i) / static_cast<Real> (kLfoSineTableSize));
            return t;
        }();
        return table.data();
    }
}
