#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/Oversampler.h>
#include <nemesis/dsp/Waveshaper.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <memory>

namespace nemesis::dsp
{
    // SelfOscLimiter — the CATHODE filter's self-oscillation limiter (T-CMP-32), lifted out of
    // `CathodeFilterBank` at D-M-0372 so it can run oversampled while, and ONLY while, a filter sings.
    //
    // The STAGE is unchanged: `AdaaSoftClip` (the family's cubic soft clip) scaled by `kLimitDrive` in
    // and its reciprocal out — unity-ceilinged — blended `dry + blend * (limited - dry)`. What changed
    // is the RATE it runs at, by owner ruling 2026-10-06 (D-M-0372): ADAA-1 alone measured an in-band
    // alias floor of -85 / -53 / -38 / -40 dB at 470 Hz / 2 / 4 / 8 kHz (3.0 at the stage), and a
    // self-oscillation's pitch IS the cutoff, so the grit grew as the filter opened. Engaged, the same
    // blend runs inside a 2x M1-02b `Oversampler` with its clip at the OS rate.
    //
    // ⚠ THE COST IS PAID ONLY WHILE THE BLEND IS NONZERO. At blend 0 with no crossing in flight,
    // `process` is the T-CMP-32 stage verbatim — its input BIT FOR BIT, plus the row-759 history store
    // — and the oversampler does not run. Every patch below the onset is bit-identical to before.
    //
    // ⚠ ENGAGE/DISENGAGE IS A CROSSING, NOT A SWITCH — the T-CMP-27 Ladder recipe. The oversampled
    // path lags by the cascade's group delay (`Oversampler::latencySamples()`, 16 base samples), so a
    // switch from the base path to it would splice two different moments of the waveform (a click),
    // and a crossfade WITHOUT alignment would comb. Up: the base-rate path keeps running and its output
    // is GLIDED (a varispeed read, 0 -> latency) over `crossfadeSamples()`, then linearly faded into the
    // oversampled path over as many again. Down: the reverse. The oversampled path starts clean (its
    // FIR history and OS-rate clip memory belong to no earlier epoch) only when it was not running, and
    // the start transient is never heard: the first half of the crossing reads the base path alone.
    // No filter state is touched — the limiter sits after the core and the drive.
    //
    // LATENCY IS REPORTED, NOT COMPENSATED — the in-loop oversampler convention (LadderFilter's driven
    // core, every model core's OS): `latencySamples (blend)` is the TARGET path's, 0 at blend 0 and the
    // cascade's while engaged, and the bank sums it like every other core's.
    //
    // ⚠ THE 2x PATH LIVES ON THE HEAP, allocated once by prepare(), so the stage adds ~24 B to every
    // `CathodeFilterBank` rather than ~420. Measured: inline, it took one CathodeMachine from 362768 B to
    // 376336 B (2 banks x 16 voices) and redded the T-CMP-20 eager-storage warning (`test_apply_table`,
    // 131 % against 133 %). The comb ring and every oversampler's histories are prepare-time heap
    // already; this is the same home. Unprepared, the stage runs its base path only.
    //
    // RT-safety: prepare() allocates; process()/reset() are inline and allocation-free. No JUCE.
    // A COPY is deep and allocates (a bank is copied by tests to render a probe off a live stage).
    class SelfOscLimiter
    {
    public:
        // The unity ceiling: 2/3 in, its reciprocal out (see CathodeFilterBank's T-CMP-32 note).
        static constexpr float kLimitDrive = 2.0f / 3.0f;
        static constexpr float kInvLimitDrive = 1.0f / kLimitDrive; // FIREFORK N2
        // Owner ruling 2026-10-06: 2x.
        static constexpr int kOversample = 2;
        // Each crossing phase (the glide, then the fade) — the Ladder's `kDriveCrossfadeMs`.
        static constexpr Real kCrossfadeMs = 5.0;

        SelfOscLimiter() = default;
        ~SelfOscLimiter() = default;
        SelfOscLimiter (SelfOscLimiter&&) noexcept = default;
        SelfOscLimiter& operator= (SelfOscLimiter&&) noexcept = default;
        SelfOscLimiter (const SelfOscLimiter& other) { *this = other; }
        SelfOscLimiter& operator= (const SelfOscLimiter& other)
        {
            if (this == &other)
                return *this;
            clip_ = other.clip_;
            prev_ = other.prev_;
            warm_ = other.warm_;
            // [stereo voice S3] Both prepared: copy INTO the existing path — no allocation, so a stage can
            // start its right chain (a copy of the left) on the audio thread.
            if (path_ != nullptr && other.path_ != nullptr)
                *path_ = *other.path_;
            else
                path_ = other.path_ != nullptr ? std::make_unique<OversampledPath> (*other.path_) : nullptr;
            xfLen_ = other.xfLen_;
            xfPos_ = other.xfPos_;
            osLive_ = other.osLive_;
            return *this;
        }

        // Designs the oversampler and sizes the crossing at `sampleRate`. Allocates; call off the audio
        // thread. Leaves the base path's history alone (the stage it was lifted from had no prepare).
        void prepare (Real sampleRate)
        {
            if (path_ == nullptr)
                path_ = std::make_unique<OversampledPath>();
            path_->os.prepare (kOversample);
            const int latency = path_->os.latencySamples();
            const Real fs = sampleRate > 0.0 ? sampleRate : 48000.0;
            const auto n = static_cast<int> (std::lround (kCrossfadeMs * 0.001 * fs)); // prepare-time libm
            // The glide must rise slower than one sample per sample (ringRead never reaches a slot older
            // than the crossing), the ring must hold the whole lag, and the OS path's start transient
            // (its two FIR spans, ~2 x latency) must be over before the fade lets it be heard.
            assert (latency < static_cast<int> (kRingMask) && "the glide ring must hold the latency");
            xfLen_ = std::max (n, 2 * latency + 2);
            clearOversampledPath();
            xfPos_ = 0;
            osLive_ = false;
        }

        // Clears all audio state. Nothing is sounding afterwards, so there is nothing to cross FROM:
        // `engaged` (the blend is nonzero) lands the crossing on its target at once.
        void reset (bool engaged) noexcept
        {
            clip_.reset();
            prev_ = 0.0f; // primes x1_ = 0: the reset clip's own history
            warm_ = false;
            clearOversampledPath();
            xfPos_ = engaged && path_ != nullptr ? 2 * xfLen_ : 0;
            osLive_ = xfPos_ != 0;
        }

        // One sample of the stage at `blend` in [0,1] (control-rate, `selfOscBlendFor`).
        float process (float x, float blend) noexcept
        {
            if (xfPos_ == 0 && blend == 0.0f)
                return baseStep (x, blend); // settled below the onset: the T-CMP-32 stage, bit for bit
            return crossStep (x, blend);
        }

        // Base-rate group delay of the path the stage is heading for: 0 at blend 0, the cascade's
        // engaged. (Across a crossing the heard lag GLIDES between the two, as the Ladder's does.)
        [[nodiscard]] int latencySamples (float blend) const noexcept
        {
            return blend == 0.0f || path_ == nullptr ? 0 : path_->os.latencySamples();
        }
        // True while the oversampled path runs (engaged, or a crossing in flight) — the cost read-back.
        [[nodiscard]] bool oversampling() const noexcept { return osLive_; }
        // Samples each crossing phase takes at the prepared rate.
        [[nodiscard]] int crossfadeSamples() const noexcept { return xfLen_; }
        // True while a crossing is in progress (neither settled end).
        [[nodiscard]] bool crossing() const noexcept { return xfPos_ != 0 && xfPos_ != 2 * xfLen_; }

    private:
        static constexpr std::size_t kRingLen = 64; // a power of two above the cascade's latency (16)
        static constexpr std::size_t kRingMask = kRingLen - 1;

        // The T-CMP-32 stage at the host rate, verbatim. [row 759] Invariant: the clip's output is that
        // of a clip run on EVERY sample. It only RUNS inside the region; outside, the one input it would
        // have seen last is kept, and re-entry primes the ADAA history with it.
        float baseStep (float x, float blend) noexcept
        {
            if (blend == 0.0f)
            {
                prev_ = x;
                warm_ = false;
                return x;
            }
            if (!warm_)
            {
                clip_.processSample (prev_ * kLimitDrive); // x1_ := the previous input
                warm_ = true;
            }
            const float limited = fork::kApprox ? clip_.processSample (x * kLimitDrive) * kInvLimitDrive // FIREFORK N2
                                         : clip_.processSample (x * kLimitDrive) / kLimitDrive;
            return x + blend * (limited - x);
        }

        // Engaged, or crossing: both paths run, the base path's output is kept for the glide.
        float crossStep (float x, float blend) noexcept
        {
            if (path_ == nullptr)
                return baseStep (x, blend); // never prepared: no 2x path to cross to
            OversampledPath& p = *path_;
            if (!osLive_)
            {
                clearOversampledPath();
                osLive_ = true;
            }
            const float base = baseStep (x, blend);
            const float wet = p.os.processSample (x, [&p, blend] (float u) noexcept {
                const float limited = fork::kApprox ? p.clip.processSample (u * kLimitDrive) * kInvLimitDrive // FIREFORK N2
                                             : p.clip.processSample (u * kLimitDrive) / kLimitDrive;
                return u + blend * (limited - u);
            });
            p.ring[p.ringPos] = base;

            float out;
            if (xfPos_ >= 2 * xfLen_)
                out = wet; // settled engaged: the oversampled path alone, bit for bit
            else
            {
                // Phase 1 (0..N): the base path read `lag` behind, lag gliding 0 -> latency.
                // Phase 2 (N..2N): at full lag, faded linearly into the oversampled path.
                const int t = std::min (xfPos_, xfLen_);
                const Real lag = static_cast<Real> (p.os.latencySamples()) * static_cast<Real> (t) / static_cast<Real> (xfLen_);
                const float aligned = ringRead (p, lag);
                if (xfPos_ <= xfLen_)
                    out = aligned;
                else
                {
                    const float w = static_cast<float> (xfPos_ - xfLen_) / static_cast<float> (xfLen_);
                    out = aligned + w * (wet - aligned);
                }
            }
            p.ringPos = (p.ringPos + 1) & kRingMask;

            if (blend != 0.0f)
                xfPos_ = std::min (xfPos_ + 1, 2 * xfLen_);
            else if (xfPos_ > 0)
            {
                --xfPos_;
                osLive_ = xfPos_ != 0; // landed: back on the base path alone, the oversampler stops
            }
            return out;
        }

        // A read `lag` samples behind the one just written, linearly interpolated (the Ladder's).
        struct OversampledPath
        {
            Oversampler os;
            AdaaSoftClip clip; // runs at the OS rate
            std::array<float, kRingLen> ring {}; // the base path's output, for the glide
            std::size_t ringPos = 0;
        };

        [[nodiscard]] static float ringRead (const OversampledPath& p, Real lag) noexcept
        {
            const auto whole = static_cast<std::size_t> (lag);
            const auto frac = static_cast<float> (lag - static_cast<Real> (whole));
            const float a = p.ring[(p.ringPos - whole) & kRingMask];
            if (frac == 0.0f)
                return a;
            const float b = p.ring[(p.ringPos - whole - 1) & kRingMask];
            return a + frac * (b - a);
        }

        void clearOversampledPath() noexcept
        {
            if (path_ == nullptr)
                return;
            path_->os.reset();
            path_->clip.reset();
            path_->ring.fill (0.0f);
            path_->ringPos = 0;
        }

        // Base (T-CMP-32) path.
        AdaaSoftClip clip_;
        float prev_ = 0.0f; // [row 759] the clip's input on the last out-of-region sample
        bool warm_ = false; // the last processed sample ran the clip
        // The crossing, and the oversampled path (heap, see the class note).
        std::unique_ptr<OversampledPath> path_;
        int xfLen_ = 1;
        int xfPos_ = 0; // 0 (base, settled) .. N (glided to the latency) .. 2N (oversampled, settled)
        bool osLive_ = false;
    };
}
