#pragma once

#include <nemesis/dsp/Real.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nemesis::dsp
{
    // Comb filter — the CATHODE §3 "Comb mode" / filter.md §8 "Comb" primitive: a tuned comb with
    // metallic, flanger-like resonances (CATHODE.md §3 "tuned feedback comb for metallic/voweled/
    // flanged industrial timbres; tracks pitch or free"; filter.md §8 "fractional-delay
    // feedforward+feedback comb; cutoff = comb tuning; reso = feedback (±)"). Linear — NOT
    // oversampled (filter.md §0.2/§8 "Comb ... need no oversampling").
    //
    // This is a genuinely-NEW DSP primitive (like LoopError / ConverterDrift), not a composition of
    // a signed-off primitive. It reuses the small-ring-buffer + linear-interp fractional-read idiom
    // from LoopError (M1-09xc). The §3 fold-in into CathodeVoiceStage as a selectable filter MODE is
    // a later composition card (mirroring FormantFilter -> M1-09fd), and the knob->param remap
    // (`synth.filt.cutoff` 0-127 -> combHz, `reso` 0-127 -> bipolar feedback) is a param/mod-matrix
    // concern — this primitive takes Hz and a bipolar feedback directly, with dedicated setters
    // (setCombHz / setFeedback / setType), exactly like FormantFilter's setVowel / setFormantQ.
    //
    // Model — a UNIVERSAL comb over ONE fractional-delay ring buffer storing the feedback state xh:
    //
    //     xh[n] = x[n] + fb * xh[n-D]        (feedback path; D = fs/combHz, fractional)
    //     y[n]  = xh[n] + ff * xh[n-D]       (feedforward tap of the SAME delay line)
    //
    // The Type selects the coefficients from the single bipolar macro g (= reso, the ± polarity):
    //     Feedforward -> fb=0, ff=g   =>  y[n] = x[n] + g*x[n-D]   (FIR comb, notches/peaks)
    //     Feedback    -> ff=0, fb=g   =>  y[n] = x[n] + g*y[n-D]   (IIR comb, resonant ring)
    //     Both        -> ff=g, fb=g   =>  combined
    // So FF and FB are exact special cases of Both, and g = 0 is a bit-exact BYPASS (xh=x, y=xh=x) —
    // the "dial to clean" anchor (early return). |g| is clamped < 1 (kMaxFb) so the feedback comb
    // RINGS but never self-oscillates (filter.md §11 "Comb ... ring instead"). The feedforward peak
    // gain is 1+g and its null gain is 1-g; the feedback peak gain is 1/(1-g), null 1/(1+g); a
    // negative g swaps peaks <-> nulls (the ± polarity).
    //
    // The fractional read is a linear interpolation strictly BEHIND the write head: combHz is clamped
    // so D >= 2, which keeps the upper interp neighbour (writePos-1) already written this sample.
    //
    // Do NOT oversample (filter.md §0.2/§8): this is a LINEAR model, no in-loop saturation. The comb
    // period in samples is SR-dependent (D = fs/combHz), so a render is per-SR (per-SR golden). Hot
    // path is pure arithmetic: no libm, no allocation, no JUCE. Even the control-rate tuning math is
    // transcendental-free (a division), so the whole primitive is header-only. Deterministic (§11):
    // same input + same param snapshot -> bit-identical output; reset() clears state. Computed in
    // double, emitted as float. Not thread-safe against prepare().
    class CombFilter
    {
    public:
        // Which comb paths are active. Feedforward/Feedback are exact special cases of Both.
        enum class Type : std::uint8_t {
            Feedforward, // y = x + g*x[n-D]      (FIR: notches/peaks, no ring)
            Feedback, // y = x + g*y[n-D]      (IIR: resonant ring)
            Both // both paths, one macro
        };

        // Allocates the delay ring buffer (sized for the lowest tuning) and resolves the
        // (SR-dependent) delay. Clears state. Not RT-safe (allocates) — call like prepareToPlay.
        void prepare (Real hostSampleRate)
        {
            fs_ = hostSampleRate > 0.0 ? hostSampleRate : 44100.0;

            // Max delay is at the lowest comb tuning; + margin for the interp neighbour + overshoot.
            const Real maxDelay = fs_ / kMinCombHz;
            size_ = static_cast<int> (maxDelay) + 4;
            if (size_ < 4)
                size_ = 4;
            buf_.assign (static_cast<std::size_t> (size_), 0.0);

            setCombHz (combHz_);
            reset();
        }

        // Comb tuning in Hz -> delay D = fs/combHz samples (control-rate; a division, no libm).
        // Clamped so D >= 2 (combHz <= kMaxTuneFrac*fs) and combHz >= kMinCombHz (D fits the buffer).
        void setCombHz (float hz) noexcept
        {
            const Real maxHz = kMaxTuneFrac * fs_;
            Real h = static_cast<Real> (hz);
            if (h < kMinCombHz)
                h = kMinCombHz;
            if (h > maxHz)
                h = maxHz;
            combHz_ = static_cast<float> (h);
            delay_ = fs_ / h; // >= 1/kMaxTuneFrac (>= 2) by the clamp
        }

        // The bipolar feedback macro (= reso, the ± polarity) in [-1, 1]. Clamped to +/-kMaxFb for
        // BIBO stability (rings, never self-oscillates). 0 = bit-exact bypass. Control-rate.
        void setFeedback (float g) noexcept
        {
            Real v = static_cast<Real> (g);
            if (v > kMaxFb)
                v = kMaxFb;
            if (v < -kMaxFb)
                v = -kMaxFb;
            g_ = v;
            resolveCoeffs();
        }

        // Selects which comb paths are active. Control-rate.
        void setType (Type t) noexcept
        {
            type_ = t;
            resolveCoeffs();
        }

        // Clears the delay line (silences the ring). Does NOT touch tuning / type / feedback.
        void reset() noexcept
        {
            for (Real& v : buf_)
                v = 0.0;
            writePos_ = 0;
        }

        // Processes one sample. g = 0 -> bit-exact pass. Otherwise: read the fractional-delayed
        // feedback state, form xh = x + fb*xhDelayed, store it, emit xh + ff*xhDelayed. Pure
        // arithmetic; zero allocation.
        float processSample (float x) noexcept
        {
            if (ff_ == 0.0 && fb_ == 0.0) // macro off: transparent bypass (the "dial to clean" anchor)
                return x;

            // Fractional read strictly behind the write head (delay_ >= 2 -> i1 <= writePos-1).
            Real rp = static_cast<Real> (writePos_) - delay_ + static_cast<Real> (size_);
            int i0 = static_cast<int> (rp); // rp > 0 by construction -> truncation == floor
            const Real frac = rp - static_cast<Real> (i0);
            i0 %= size_;
            int i1 = i0 + 1;
            if (i1 >= size_)
                i1 = 0;
            const Real xhDelayed = buf_[static_cast<std::size_t> (i0)] * (1.0 - frac)
                                     + buf_[static_cast<std::size_t> (i1)] * frac;

            const Real xh = static_cast<Real> (x) + fb_ * xhDelayed;
            buf_[static_cast<std::size_t> (writePos_)] = xh;
            const Real y = xh + ff_ * xhDelayed;

            ++writePos_;
            if (writePos_ >= size_)
                writePos_ = 0;

            return static_cast<float> (y);
        }

        // Accessors (control-rate resolved operands) — used by tests / a future fold-in.
        [[nodiscard]] float combHz() const noexcept { return combHz_; }
        [[nodiscard]] Real delaySamples() const noexcept { return delay_; }
        // [T-CMP-29] The feedback macro the ring holds (after the ±kMaxFb clamp) — the read-back the
        // synth/FLTR "same law" claim is asserted on.
        [[nodiscard]] Real feedback() const noexcept { return g_; }

    private:
        // Resolve the feedforward/feedback coefficients from the single macro + the type.
        void resolveCoeffs() noexcept
        {
            switch (type_)
            {
                case Type::Feedforward:
                    ff_ = g_;
                    fb_ = 0.0;
                    break;
                case Type::Feedback:
                    ff_ = 0.0;
                    fb_ = g_;
                    break;
                case Type::Both:
                    ff_ = g_;
                    fb_ = g_;
                    break;
            }
        }

        // Design constants (the fixed voicing the macros scale).
        static constexpr Real kMaxFb = 0.97; // feedback clamp: rings, never self-oscillates
        static constexpr Real kMinCombHz = 20.0; // lowest tuning (sets the buffer length)
        static constexpr Real kMaxTuneFrac = 0.45; // highest tuning fraction of fs -> D >= ~2.2

    public:
        // FIREFORK B5: hand the delay ring over without copying it (the twin takes it lazily, see CathodeFilterBank).
        void swapRing (std::vector<Real>& v) noexcept { buf_.swap (v); }
        void copyRingFrom (const CombFilter& o) noexcept
        {
            if (buf_.size() == o.buf_.size())
                std::copy (o.buf_.begin(), o.buf_.end(), buf_.begin());
        }

    private:
        // Control-rate parameters + derived coefficients.
        Real fs_ = 44100.0;
        float combHz_ = 1000.0f; // comb tuning
        Real delay_ = 44100.0 / 1000.0; // delay in samples (D = fs/combHz)
        Real g_ = 0.0; // bipolar feedback macro (0 = bypass)
        Type type_ = Type::Feedforward;
        Real ff_ = 0.0; // feedforward coefficient
        Real fb_ = 0.0; // feedback coefficient
        int size_ = 4; // ring buffer length

        // Per-sample state.
        std::vector<Real> buf_;
        int writePos_ = 0;
    };
}
