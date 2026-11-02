#include <nemesis/dsp/HarmonicFrame.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace nemesis::dsp
{
    namespace
    {
        constexpr double kTwoPi = 6.283185307179586476925286766559;
    }

    void HarmonicFrame::addBin (std::vector<double>& amps, double position, double gain)
    {
        if (position < 1.0 || position > static_cast<double> (kMaxHarmonics))
            return;

        const auto lo = static_cast<int> (std::floor (position));
        const double frac = position - static_cast<double> (lo);

        if (lo >= 1 && lo <= static_cast<int> (amps.size()))
            amps[static_cast<std::size_t> (lo - 1)] += gain * (1.0 - frac);
        if (lo + 1 >= 1 && lo + 1 <= static_cast<int> (amps.size()))
            amps[static_cast<std::size_t> (lo)] += gain * frac;
    }

    double HarmonicFrame::resonance (double freqHz, double centreHz, double widthOctaves) noexcept
    {
        if (freqHz <= 0.0 || centreHz <= 0.0 || widthOctaves <= 0.0)
            return 0.0;
        const double octaves = std::log2 (freqHz / centreHz);
        return std::exp (-(octaves * octaves) / (2.0 * widthOctaves * widthOctaves));
    }

    void HarmonicFrame::renderAdditive (const std::vector<double>& amps,
        const std::vector<double>& phases,
        float* out)
    {
        const int count = std::min (static_cast<int> (amps.size()), kMaxHarmonics);

        std::vector<double> work (static_cast<std::size_t> (kFrameLength), 0.0);
        for (int k = 1; k <= count; ++k)
        {
            const double a = amps[static_cast<std::size_t> (k - 1)];
            if (a == 0.0)
                continue;
            const double phase = static_cast<std::size_t> (k - 1) < phases.size()
                                     ? phases[static_cast<std::size_t> (k - 1)]
                                     : 0.0;
            for (int i = 0; i < kFrameLength; ++i)
                work[static_cast<std::size_t> (i)] += a
                                                      * std::sin (kTwoPi * static_cast<double> (k) * static_cast<double> (i)
                                                                      / static_cast<double> (kFrameLength)
                                                                  + phase);
        }

        // Peak-normalise to unity. The degenerate case (every amplitude zero) is a LAW bug rather
        // than an input a caller can reach through a category generator, and the alternative to
        // this guard is a frame of NaNs instead of a silent one — the same reasoning
        // `BasicTableGenerator`'s normalise carries, and the same labelled non-claim.
        double peak = 0.0;
        for (const double v : work)
            peak = std::max (peak, std::abs (v));
        const double scale = peak > 1.0e-12 ? 1.0 / peak : 1.0;

        for (int i = 0; i < kFrameLength; ++i)
            out[i] = static_cast<float> (work[static_cast<std::size_t> (i)] * scale);
    }
} // namespace nemesis::dsp
