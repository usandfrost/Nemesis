#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/Oversampler.h>

#include <cmath>
#include <cstddef>

namespace nemesis::dsp
{
#if NEMESIS_FORK_EXACT
#ifndef NEMESIS_FORK_FAST_DATA
#define NEMESIS_FORK_FAST_DATA
#endif
    NEMESIS_FORK_FAST_DATA Real ForkTapPool::pool[ForkTapPool::kSize];
    NEMESIS_FORK_FAST_DATA std::uint16_t ForkTapPool::idx[ForkTapPool::kSize];
    std::size_t ForkTapPool::used = 0;
#endif
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;

        // Taps per polyphase phase either side of centre. M=8 -> 17 taps/phase; with a
        // 4-term Blackman-Harris window (~92 dB sidelobes) this carries the alias floor
        // well below the ADAA/naive base-rate clip while keeping the hot path short.
        // PASSBAND (it is not flat to base Nyquist): the cutoff sits AT base Nyquist, so the
        // transition band starts inside the audio band. Measured with a sine through the
        // identity-shaper cascade at fs = 48 kHz (a scratch run, not asserted by a test): about
        // -0.08 dB at 16 kHz, -0.5 dB at 18 kHz and -1.9 dB at 20 kHz, identical for factors 2, 4
        // and 8 (the design is relative to base Nyquist, so the figures scale with the base rate).
        // Up and down share one prototype, so each PASS is about half of that (roughly -1 dB at
        // 20 kHz) and the cascade is the pair; 1 kHz and 10 kHz read 0.000 dB.
        constexpr int kHalfTapsPerPhase = 8;
#if NEMESIS_FORK_EXACT
        // FIREFORK E14: the shared designs. Filled by prepare() (off the audio thread; not thread-safe, the firmware prepares
        // from one thread), never freed, never moved, so the views stay valid. Factors 2, 4, 8 need 465 coefficients in all.
        constexpr std::size_t kPoolSize = ForkTapPool::kSize;
        Real* const gPool = ForkTapPool::pool;
        std::size_t& gPoolUsed = ForkTapPool::used;
        struct Design
        {
            int factor = 0;
            std::size_t up = 0, nUp = 0, down = 0, nDown = 0;
        };
        Design gDesigns[8];
        int gDesignCount = 0;
        Oversampler::SharedTaps intern (const std::vector<Real>& v) noexcept
        {
            if (gPoolUsed + v.size() > kPoolSize)
                __builtin_trap(); // a design that does not fit is a build error in disguise: fail loudly
            Real* dst = gPool + gPoolUsed;
            for (std::size_t i = 0; i < v.size(); ++i)
                dst[i] = v[i];
            gPoolUsed += v.size();
            return { dst, v.size() };
        }
#endif
    }

    void Oversampler::prepare (int factor)
    {
        factor_ = factor < 1 ? 1 : factor;

        if (factor_ == 1)
        {
            // Bypass: no filtering, no delay, no state.
            phaseTaps_ = 0;
            downTaps_ = 0;
            ohistLen_ = 0;
            latency_ = 0;
            up_.clear();
            down_.clear();
            xhist_.clear();
            ohist_.clear();
            xpos_ = 0;
            opos_ = 0;
            return;
        }

        const int l = factor_;
        const int m = kHalfTapsPerPhase;
        const int nTaps = 2 * m * l + 1; // odd -> symmetric prototype with a centre tap
        const int centre = m * l;

        phaseTaps_ = 2 * m + 1;
        downTaps_ = nTaps;
        ohistLen_ = nTaps + l - 1; // + (l-1) so the phase-0 retained sample stays in range
        latency_ = 2 * m; // (m*l) OS-samples of group delay per filter, /l -> m base each

        // Prototype low-pass: windowed sinc at cutoff 1/(2L) (base-Nyquist relative to the
        // OS rate).  h_ideal[k] = sin(pi*k/L)/(pi*k), h_ideal[0] = 1/L.  Normalised to unity
        // DC gain after windowing so the cascade is transparent at low frequencies.
        std::vector<Real> h (static_cast<std::size_t> (nTaps));
        Real sum = 0.0;
        const Real denom = static_cast<Real> (nTaps - 1);
        for (int n = 0; n < nTaps; ++n)
        {
            const int k = n - centre;
            const Real sinc = (k == 0)
                                    ? 1.0 / static_cast<Real> (l)
                                    : std::sin (kPi * static_cast<Real> (k) / static_cast<Real> (l))
                                          / (kPi * static_cast<Real> (k));
            const Real t = 2.0 * kPi * static_cast<Real> (n) / denom;
            const Real win = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2.0 * t)
                               - 0.01168 * std::cos (3.0 * t);
            const Real v = sinc * win;
            h[static_cast<std::size_t> (n)] = v;
            sum += v;
        }
        for (Real& v : h)
            v /= sum;

        // Upsampling polyphase: phase p (output time n*L+p) uses prototype taps p, p+L, ...
        //   up[p][j] = L * h[j*L + p]   (0 where j*L+p runs past the prototype).
        // The L factor restores unity passband gain lost to zero-stuffing.
#if NEMESIS_FORK_EXACT
        for (int k = 0; k < gDesignCount; ++k) // FIREFORK E14: designed before: share it
            if (gDesigns[k].factor == l)
            {
                up_ = { gPool + gDesigns[k].up, gDesigns[k].nUp };
                down_ = { gPool + gDesigns[k].down, gDesigns[k].nDown };
                xhist_.assign (2 * static_cast<std::size_t> (phaseTaps_), 0.0);
                ohist_.assign (2 * static_cast<std::size_t> (ohistLen_), 0.0);
                xpos_ = 0;
                opos_ = 0;
                return;
            }
        std::vector<Real> upW;
#else
        std::vector<Real>& upW = up_;
#endif
        upW.assign (static_cast<std::size_t> (l) * static_cast<std::size_t> (phaseTaps_), 0.0);
        for (int p = 0; p < l; ++p)
        {
            for (int j = 0; j < phaseTaps_; ++j)
            {
                const int idx = j * l + p;
                if (idx < nTaps)
                {
                    const auto dst = static_cast<std::size_t> (p) * static_cast<std::size_t> (phaseTaps_)
                                     + static_cast<std::size_t> (j);
                    upW[dst] = static_cast<Real> (l) * h[static_cast<std::size_t> (idx)];
                }
            }
        }

        // Downsampling filter = the unity-DC prototype, applied as a decimating FIR.
#if NEMESIS_FORK_EXACT
        {
            const auto u = intern (upW);
            const auto d = intern (h);
#if NEMESIS_FORK_APPROX
            // FIREFORK N1: right after the taps: [centre coef][pair count][pair coefs...], offsets in ForkTapPool::idx.
            {
                const int c = nTaps / 2;
                std::vector<Real> blk { h[static_cast<std::size_t> (c)], 0.0 };
                std::vector<int> offs { 0, 0 };
                for (int j = 0; j < c; ++j)
                    if ((c - j) % l != 0)
                    {
                        blk.push_back (0.5 * (h[static_cast<std::size_t> (j)] + h[static_cast<std::size_t> (nTaps - 1 - j)]));
                        offs.push_back (j);
                    }
                blk[1] = static_cast<Real> (blk.size() - 2);
                const std::size_t at = gPoolUsed;
                (void) intern (blk);
                for (std::size_t i = 0; i < offs.size(); ++i)
                    ForkTapPool::idx[at + i] = static_cast<std::uint16_t> (offs[i]);
            }
#endif
            up_ = u;
            down_ = d;
            if (u.data() != nullptr && d.data() != nullptr && gDesignCount < 8)
                gDesigns[gDesignCount++] = { l, static_cast<std::size_t> (u.data() - gPool), u.size(),
                                             static_cast<std::size_t> (d.data() - gPool), d.size() };
        }
#else
        down_ = h;
#endif

        xhist_.assign (2 * static_cast<std::size_t> (phaseTaps_), 0.0); // mirrored (Oversampler.h)
        ohist_.assign (2 * static_cast<std::size_t> (ohistLen_), 0.0);
        xpos_ = 0;
        opos_ = 0;
    }

    void Oversampler::reset() noexcept
    {
        for (Real& v : xhist_)
            v = 0.0;
        for (Real& v : ohist_)
            v = 0.0;
        xpos_ = 0;
        opos_ = 0;
    }
}
