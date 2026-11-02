#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/MinBlep.h>

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;

        // In-place iterative radix-2 Cooley-Tukey FFT (decimation-in-time) on split re/im
        // arrays of length n (a power of two). `inverse` flips the twiddle sign and scales by
        // 1/n. Runs only in prepare() (off the audio thread), so twiddles use libm directly.
        //
        // THE FOURTH COPY of this routine in dsp/ — see the header's note. ADR-007 D2's shared
        // FFT seam is unbuilt; owner-ruled to follow the three precedents rather than realise
        // the seam inside this card.
        void fft (Real* re, Real* im, int n, bool inverse) noexcept
        {
            for (int i = 1, j = 0; i < n; ++i)
            {
                int bit = n >> 1;
                for (; (j & bit) != 0; bit >>= 1)
                    j ^= bit;
                j ^= bit;
                if (i < j)
                {
                    const Real tr = re[i];
                    re[i] = re[j];
                    re[j] = tr;
                    const Real ti = im[i];
                    im[i] = im[j];
                    im[j] = ti;
                }
            }

            const Real sign = inverse ? 1.0 : -1.0;
            for (int len = 2; len <= n; len <<= 1)
            {
                const int half = len >> 1;
                const Real theta = sign * 2.0 * kPi / static_cast<Real> (len);
                for (int i = 0; i < n; i += len)
                {
                    for (int k = 0; k < half; ++k)
                    {
                        const Real ang = theta * static_cast<Real> (k);
                        const Real wRe = std::cos (ang);
                        const Real wIm = std::sin (ang);
                        const int a = i + k;
                        const int b = i + k + half;
                        const Real vRe = re[b] * wRe - im[b] * wIm;
                        const Real vIm = re[b] * wIm + im[b] * wRe;
                        re[b] = re[a] - vRe;
                        im[b] = im[a] - vIm;
                        re[a] += vRe;
                        im[a] += vIm;
                    }
                }
            }

            if (inverse)
            {
                const Real norm = 1.0 / static_cast<Real> (n);
                for (int i = 0; i < n; ++i)
                {
                    re[i] *= norm;
                    im[i] *= norm;
                }
            }
        }

        // 4-term Blackman-Harris, same window family as Oversampler.h's prototype.
        Real blackmanHarris (Real t) noexcept // t in [0,1]
        {
            constexpr Real a0 = 0.35875;
            constexpr Real a1 = 0.48829;
            constexpr Real a2 = 0.14128;
            constexpr Real a3 = 0.01168;
            const Real w = 2.0 * kPi * t;
            return a0 - a1 * std::cos (w) + a2 * std::cos (2.0 * w) - a3 * std::cos (3.0 * w);
        }

        Real sincNorm (Real x) noexcept // sin(pi x) / (pi x), with the removable hole filled
        {
            if (std::abs (x) < 1e-12)
                return 1.0;
            const Real px = kPi * x;
            return std::sin (px) / px;
        }

        int nextPowerOfTwoAtLeast (int n) noexcept
        {
            int p = 1;
            while (p < n)
                p <<= 1;
            return p;
        }
    }

    std::vector<Real> MinBlep::designBandlimitedImpulse (int oversampling, int zeroCrossings)
    {
        if (oversampling < 2)
            oversampling = 2;
        if (zeroCrossings < 2)
            zeroCrossings = 2;

        const int n = 2 * zeroCrossings * oversampling + 1;
        const auto len = static_cast<std::size_t> (n);
        std::vector<Real> h (len, 0.0);

        // Cutoff = base-rate Nyquist expressed in OS-sample units: 0.5 / oversampling cycles
        // per OS sample, so the impulse is band-limited to what the base rate can carry.
        const Real centre = 0.5 * static_cast<Real> (n - 1);
        const Real cutoff = 1.0 / static_cast<Real> (oversampling);

        Real sum = 0.0;
        for (std::size_t i = 0; i < len; ++i)
        {
            const Real x = (static_cast<Real> (i) - centre) * cutoff;
            const Real w = blackmanHarris (static_cast<Real> (i) / static_cast<Real> (n - 1));
            h[i] = sincNorm (x) * w;
            sum += h[i];
        }

        // Unit DC gain, so integrating the impulse gives a step that settles at exactly 1.
        if (std::abs (sum) > 1e-15)
            for (auto& v : h)
                v /= sum;

        return h;
    }

    MinBlep::Design MinBlep::buildDesign (int oversampling, int zeroCrossings)
    {
        Design d;
        d.oversampling = oversampling < 2 ? 2 : oversampling;
        d.zeroCrossings = zeroCrossings < 2 ? 2 : zeroCrossings;

        const auto proto = designBandlimitedImpulse (d.oversampling, d.zeroCrossings);
        const int n = static_cast<int> (proto.size());

        // Cepstral minimum-phase reconstruction. The transform is exact only in the limit of
        // an unwrapped, densely-sampled log spectrum, so the FFT is padded well beyond the
        // prototype: too little padding aliases the cepstrum and shows up as a magnitude
        // error, which is precisely what the acceptance test measures.
        const int m = nextPowerOfTwoAtLeast (8 * n);
        const auto mlen = static_cast<std::size_t> (m);
        std::vector<Real> re (mlen, 0.0);
        std::vector<Real> im (mlen, 0.0);
        for (int i = 0; i < n; ++i)
            re[static_cast<std::size_t> (i)] = proto[static_cast<std::size_t> (i)];

        fft (re.data(), im.data(), m, false);

        // log|H|. The floor keeps a stopband null from producing -inf; it sits far below the
        // window's own sidelobe floor, so it changes nothing the passband can see.
        constexpr Real kMagnitudeFloor = 1e-12;
        for (std::size_t i = 0; i < mlen; ++i)
        {
            const Real mag = std::sqrt (re[i] * re[i] + im[i] * im[i]);
            re[i] = std::log (mag < kMagnitudeFloor ? kMagnitudeFloor : mag);
            im[i] = 0.0;
        }

        // Real cepstrum.
        fft (re.data(), im.data(), m, true);

        // Causal fold: double the positive-quefrency half, keep 0 and the midpoint, zero the
        // negative half. Getting the DOUBLING wrong is the classic error here and it shows up
        // as a magnitude error rather than a phase one — battery-proven (M1-19 M05).
        //
        // THE MIDPOINT ENDPOINT IS DIFFERENT, and this is measured rather than assumed: a
        // mutation that zeroes bin m/2 along with the negative half SURVIVES the battery, and
        // it survives because it is EQUIVALENT here, not because a test is weak. Measured,
        // pristine vs mutant: passband relative magnitude error 0.000953 vs 0.000897,
        // absolute 0.00136 vs 0.00128, stopband 2.741e-6 vs 2.742e-6, table endpoint 8.9e-16
        // vs 6.7e-16 — every figure inside the noise, and two of them slightly BETTER. The
        // cepstrum of a heavily-windowed band-limited sinc carries essentially nothing at the
        // Nyquist quefrency, so there is nothing at m/2 to keep.
        //
        // `half + 1` stays because it is what the transform is DEFINED as, and a prototype
        // with real energy at m/2 would need it. No test pins it, and inventing one would be
        // pinning an implementation detail with no observable consequence.
        const int half = m / 2;
        for (int i = 1; i < half; ++i)
        {
            re[static_cast<std::size_t> (i)] *= 2.0;
            im[static_cast<std::size_t> (i)] *= 2.0;
        }
        for (int i = half + 1; i < m; ++i)
        {
            re[static_cast<std::size_t> (i)] = 0.0;
            im[static_cast<std::size_t> (i)] = 0.0;
        }

        // Exponentiate the folded cepstrum's spectrum to get the minimum-phase spectrum.
        fft (re.data(), im.data(), m, false);
        for (std::size_t i = 0; i < mlen; ++i)
        {
            const Real mag = std::exp (re[i]);
            const Real ang = im[i];
            re[i] = mag * std::cos (ang);
            im[i] = mag * std::sin (ang);
        }

        fft (re.data(), im.data(), m, true);

        // The minimum-phase impulse is causal and front-loaded, so the prototype's length is
        // enough to hold it; re-normalise to unit DC gain because the round trip through the
        // log/exp pair leaves a small gain error.
        d.impulse.assign (static_cast<std::size_t> (n), 0.0);
        Real sum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            d.impulse[static_cast<std::size_t> (i)] = re[static_cast<std::size_t> (i)];
            sum += d.impulse[static_cast<std::size_t> (i)];
        }
        if (std::abs (sum) > 1e-15)
            for (auto& v : d.impulse)
                v /= sum;

        // Integrate to the step table. It settles at 1 by construction of the normalisation.
        d.table.assign (static_cast<std::size_t> (n), 0.0);
        Real acc = 0.0;
        for (int i = 0; i < n; ++i)
        {
            acc += d.impulse[static_cast<std::size_t> (i)];
            d.table[static_cast<std::size_t> (i)] = acc;
        }
        return d;
    }

    namespace
    {
        // [T-ARCH-12] Built by the first default `prepare()` (message thread), once per process; C++11
        // static initialisation is thread-safe, and nothing writes it afterwards.
        const std::shared_ptr<const MinBlep::Design>& sharedDefault()
        {
            static const std::shared_ptr<const MinBlep::Design> design = std::make_shared<const MinBlep::Design> (
                MinBlep::buildDesign (MinBlep::kDefaultOversampling, MinBlep::kDefaultZeroCrossings));
            return design;
        }
    }

    const MinBlep::Design& MinBlep::sharedDefaultDesign()
    {
        return *sharedDefault();
    }

    void MinBlep::prepare (int oversampling, int zeroCrossings)
    {
        oversampling_ = oversampling < 2 ? 2 : oversampling;
        zeroCrossings_ = zeroCrossings < 2 ? 2 : zeroCrossings;

        if (oversampling_ == kDefaultOversampling && zeroCrossings_ == kDefaultZeroCrossings)
            design_ = sharedDefault();
        else
            design_ = std::make_shared<const Design> (buildDesign (oversampling_, zeroCrossings_));

        // The correction window IS the buffer — sized to the declared support, so a leak past
        // it is not possible rather than merely not intended.
        accum_.assign (static_cast<std::size_t> (supportSamples()), 0.0);
        accumPos_ = 0;
    }

    const std::vector<Real>& MinBlep::impulse() const noexcept
    {
        static const std::vector<Real> kNone;
        return design_ ? design_->impulse : kNone;
    }

    void MinBlep::addStep (Real stepSize, Real frac) noexcept
    {
        const int n = static_cast<int> (accum_.size());
        if (n <= 0)
            return;

        if (frac < 0.0)
            frac = 0.0;
        else if (frac > 1.0)
            frac = 1.0;

        // Output j sits `j + 1 - frac` samples after the discontinuity. The naive signal has
        // already taken the whole step by then, so what is added is the SHORTFALL of the
        // band-limited step against the ideal one: table - 1, scaled by the step.
        for (int j = 0; j < n; ++j)
        {
            const Real t = static_cast<Real> (j + 1) - frac;
            const auto idx = static_cast<int> (std::lround (t * static_cast<Real> (oversampling_)));
            const Real residual = tableAt (idx) - 1.0;

            int p = accumPos_ + j;
            if (p >= n)
                p -= n;
            accum_[static_cast<std::size_t> (p)] += stepSize * residual;
        }
    }

    Real MinBlep::addRamp (Real slopeChange, Real frac) noexcept
    {
        const int n = static_cast<int> (accum_.size());
        if (n < 2)
            return 0.0;

        if (frac < 0.0)
            frac = 0.0;
        else if (frac > 1.0)
            frac = 1.0;

        // The next output sits 1 - frac after the kink, the one after 2 - frac; the sample just returned, -frac.
        int p = accumPos_;
        accum_[static_cast<std::size_t> (p)] += slopeChange * rampResidualAt (1.0 - frac);
        p = p + 1 >= n ? 0 : p + 1;
        accum_[static_cast<std::size_t> (p)] += slopeChange * rampResidualAt (2.0 - frac);
        return slopeChange * rampResidualAt (-frac);
    }

    Real MinBlep::rampResidualAt (Real t) noexcept
    {
        if (t <= -2.0 || t >= 2.0)
            return 0.0;
        // (1/120)·Σ (−1)^j·C(4,j)·(t+2−j)_+^5, the twice-integrated cubic B-spline, less the ideal ramp.
        constexpr Real kBinom[] = { 1.0, -4.0, 6.0, -4.0, 1.0 };
        Real smooth = 0.0;
        for (int j = 0; j < 5; ++j)
        {
            const Real x = t + 2.0 - static_cast<Real> (j);
            if (x > 0.0)
                smooth += kBinom[j] * x * x * x * x * x;
        }
        return smooth / 120.0 - (t > 0.0 ? t : 0.0);
    }

    float MinBlep::processSample() noexcept
    {
        const int n = static_cast<int> (accum_.size());
        if (n <= 0)
            return 0.0f;

        const Real v = accum_[static_cast<std::size_t> (accumPos_)];
        accum_[static_cast<std::size_t> (accumPos_)] = 0.0; // clearing is what bounds the support
        accumPos_ = (accumPos_ + 1 >= n) ? 0 : accumPos_ + 1;
        return static_cast<float> (v);
    }

    void MinBlep::reset() noexcept
    {
        for (auto& v : accum_)
            v = 0.0;
        accumPos_ = 0;
    }

    Real MinBlep::tableAt (int i) const noexcept
    {
        if (!design_ || design_->table.empty())
            return 0.0;
        const std::vector<Real>& table = design_->table;
        if (i <= 0)
            return table.front();
        if (i >= static_cast<int> (table.size()))
            return table.back();
        return table[static_cast<std::size_t> (i)];
    }
}
