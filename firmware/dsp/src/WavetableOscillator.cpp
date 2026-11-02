#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/WavetableOscillator.h>

#include <cmath>
#include <cstddef>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;

        // In-place iterative radix-2 Cooley-Tukey FFT (decimation-in-time) on split re/im
        // arrays of length n (a power of two). `inverse` flips the twiddle sign and scales by
        // 1/n. Runs only in build() (off the audio thread), so twiddles use libm directly —
        // no precomputed table needed here (unlike the audio-thread burst in Convolution).
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
    }

    // FIREFORK W1: the geometry of a pyramid: mip count (top harmonic frameLength/2 >> j, down to the fundamental), and where each mip lives.
    void Wavetable::setGeometry (int numFrames, int frameLength, bool decimate)
    {
        frameLength_ = frameLength < 2 ? 2 : frameLength;
        numFrames_ = numFrames < 1 ? 1 : numFrames;
        numMips_ = 1;
        for (int t = frameLength_ / 2; t > 1; t >>= 1)
            ++numMips_;
        if (numMips_ > static_cast<int> (mipLen_.size()))
            numMips_ = static_cast<int> (mipLen_.size());
        decimated_ = decimate;
        constexpr int kDecShift = 2; // a decimated mip j is stored at L >> (j - 2) samples: 2^2 x its Nyquist length (clean linear reads)
        constexpr int kDecMin = 128; // and never fewer than 128: the top mips hold one or two harmonics, and linear interpolation of a 64-sample level leaves spurs at -47 dB
        std::size_t off = 0;
        for (int j = 0; j < numMips_; ++j)
        {
            int len = frameLength_;
            if (decimate && j > 0)
            {
                len = frameLength_ >> (j > kDecShift ? j - kDecShift : 0); // 2^kDecShift x oversampled: linear interpolation stays clean
                if (len < kDecMin)
                    len = kDecMin < frameLength_ ? kDecMin : frameLength_;
            }
            mipLen_[static_cast<std::size_t> (j)] = len;
            mipOff_[static_cast<std::size_t> (j)] = off;
            off += static_cast<std::size_t> (len);
        }
        frameSize_ = off;
    }

    std::size_t Wavetable::pyramidSamples (int numFrames, int frameLength, bool decimated)
    {
        Wavetable t;
        t.setGeometry (numFrames, frameLength, decimated);
        return static_cast<std::size_t> (t.numFrames_) * t.frameSize_;
    }

    bool Wavetable::adoptPyramid (int numFrames, int frameLength, bool decimated, const std::int16_t* q, std::size_t count, float scale)
    {
        setGeometry (numFrames, frameLength, decimated);
        const std::size_t want = static_cast<std::size_t> (numFrames_) * frameSize_;
        if (count != want || (ext_ != nullptr && want > extCap_))
        {
            numFrames_ = 0;
            data_.clear();
            return false;
        }
        Real* dst = ext_;
        if (dst == nullptr)
        {
            data_.assign (want, 0.0);
            dst = data_.data();
        }
        // `q` may be the second half of `dst` itself (the device reads the file there and converts forward in place: every write lands
        // on bytes already read), so both pointers are may_alias: the compiler must not move a load past a store
        using Q16 = std::int16_t __attribute__ ((may_alias));
        using RA = Real __attribute__ ((may_alias));
        const Q16* qq = q;
        RA* dd = dst;
        for (std::size_t i = 0; i < want; ++i)
            dd[i] = static_cast<Real> (qq[i]) * static_cast<Real> (scale);
        return true;
    }

    void Wavetable::build (const float* samples, int numFrames, int frameLength, bool decimate)
    {
        ext_ = nullptr; // a built table lives in data_
        setGeometry (numFrames, frameLength, decimate);

        const auto L = static_cast<std::size_t> (frameLength_);
        data_.assign (static_cast<std::size_t> (numFrames_) * frameSize_, 0.0);

        std::vector<Real> re (L, 0.0);
        std::vector<Real> im (L, 0.0);
        std::vector<Real> specRe (L, 0.0);
        std::vector<Real> specIm (L, 0.0);

        for (int f = 0; f < numFrames_; ++f)
        {
            // Forward FFT of this single-cycle frame -> its full-band spectrum.
            for (std::size_t i = 0; i < L; ++i)
            {
                re[i] = static_cast<Real> (samples[static_cast<std::size_t> (f) * L + i]);
                im[i] = 0.0;
            }
            fft (re.data(), im.data(), frameLength_, false);
            specRe = re;
            specIm = im;

            for (int mip = 0; mip < numMips_; ++mip)
            {
                const int top = (frameLength_ / 2) >> mip; // harmonics 1..top survive

                // Copy the spectrum, zero every bin above the mip's top harmonic. Bin k and
                // its conjugate (frameLength-k) carry harmonic k; keep 0..top and the mirror.
                for (std::size_t k = 0; k < L; ++k)
                {
                    const int kk = static_cast<int> (k);
                    const int harmonic = kk <= frameLength_ / 2 ? kk : frameLength_ - kk;
                    if (harmonic <= top)
                    {
                        re[k] = specRe[k];
                        im[k] = specIm[k];
                    }
                    else
                    {
                        re[k] = 0.0;
                        im[k] = 0.0;
                    }
                }

                fft (re.data(), im.data(), frameLength_, true);

                Real* dst = data_.data() + frameStride (f, mip);
                const auto len = static_cast<std::size_t> (mipLen_[static_cast<std::size_t> (mip)]);
                const std::size_t step = L / len; // 1 unless decimated: the level holds nothing above 2 x len / 4, so every step-th sample is exact
                for (std::size_t i = 0; i < len; ++i)
                    dst[i] = re[i * step];
            }
        }
    }
}
