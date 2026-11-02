#pragma once

#include <nemesis/dsp/Real.h>
#include <cstdint>

namespace nemesis::dsp
{
    // Key-tracked sample-RATE reducer — the CATHODE §2 "broken converter" resample+reconstruct
    // half (CATHODE.md §2 "key-tracked variable rate" + "imperfect reconstruction"; params
    // synth.conv.{rate,track,interp}). It models a vintage sampler that stores the voice at a
    // native converter rate `fs_dev` and replays it varispeed: the input is CAPTURED at an
    // effective rate `fs_eff` and RECONSTRUCTED with either zero-order hold (staircase imaging)
    // or linear interpolation (softer). Higher notes push `fs_eff` up (brighter/aliased), lower
    // notes down (darker) — the Mirage/SP "plays faster going up" character; `track` sets how
    // strongly pitch shifts the rate.
    //
    // This is a SIBLING of BrokenConverter (M1-05), which is the bit-reduction + µ-law
    // companding half of the same §2 stage. In the full §0 voice flow the order is
    // "var-rate · bits · compand · ZOH", i.e. the bit/compand quantiser sits BETWEEN capture
    // and reconstruction; wiring that interleave is a CathodeVoiceStage COMPOSITION concern.
    // Here capture->reconstruct is one processSample (equivalent to the interleaved chain for
    // ZOH — a held constant quantises the same either way; the interleaved variant is a later
    // stage card). loop-point error and drift/jitter are the other deferred M1-09x card.
    //
    // Do NOT oversample this (CATHODE.md §2/§11): like every digital lo-fi stage the
    // imaging/aliasing IS the effect. There is a real bit-exact BYPASS: when `fs_eff >= fs`
    // (device rate at or above the host rate) no reduction is possible and the output is
    // bit-identical to the input, for both reconstruction modes — the "dial to clean" anchor.
    //
    // Hot path is pure arithmetic (no libm, no allocation, no JUCE); the rate coefficient
    // `inc_ = fs_eff/fs` (which needs exp2/log2 for key tracking) is resolved CONTROL-RATE in
    // the setters (SampleRateReducer.cpp), per the §0 libm-free-hot-path convention.
    // Deterministic (§11): same input + same param snapshot -> bit-identical output; RNG-free.
    class SampleRateReducer
    {
    public:
        enum class Interp : std::uint8_t {
            ZeroOrderHold, // staircase / imaging (the un-anti-aliased "digital" grit)
            Linear // softer, continuous reconstruction between captured samples
        };

        // Sets the host sample rate, recomputes the rate coefficient, and clears state.
        void prepare (Wide hostSampleRate) noexcept;

        // Native converter rate in Hz (the "rate" control). Control-rate; clamped to a sane
        // floor. At or above the host rate the reducer is a bit-exact bypass.
        void setDeviceRateHz (float rateHz) noexcept;

        // Key-tracking amount in [0, 1]: 0 = effective rate is pitch-independent; 1 = full
        // varispeed (an octave up doubles the effective rate). Control-rate.
        void setKeyTrack (float amount) noexcept;

        // Current voice note in Hz (drives key tracking). Control-rate.
        void setNoteHz (float noteHz) noexcept;

        // Reconstruction mode; trivial, safe to call on the audio thread.
        void setInterpolation (Interp mode) noexcept { zoh_ = (mode == Interp::ZeroOrderHold); }

        // Clears the reconstruction state (phase, held samples, first-sample prime). Does NOT
        // touch the control-rate coefficients (like the other DSP primitives' reset()).
        void reset() noexcept;

        // The effective converter rate in Hz after key tracking (introspection / tests).
        Wide effectiveRateHz() const noexcept { return fsEff_; }

        // [T-CORE-18] The three CONTROL values back out, for the same reason
        // `effectiveRateHz()` exists: the §2.2 GRIT macro is a depth-scaler whose rate leg
        // interpolates the DEVICE rate from the host rate (its clean anchor) toward the
        // dialed value, and a render assertion cannot tell a geometric leg from a linear
        // one — nor `loopErr` from `drift`, which share range, curve and law. The dialed
        // value is what the read-back reports, i.e. AFTER `setDeviceRateHz`'s floor clamp.
        [[nodiscard]] Wide deviceRateHz() const noexcept { return deviceRate_; }
        [[nodiscard]] Real keyTrack() const noexcept { return track_; }
        [[nodiscard]] Interp interpolation() const noexcept { return zoh_ ? Interp::ZeroOrderHold : Interp::Linear; }
        // [T-CGUI-09] The NOMINAL reconstruction delay, in host samples, while reducing. MEASURED with
        // a ramp (y[n] = x[n − delay]): a hold includes its own capture sample, so it is on average
        // (T − 1)/2 old for a capture period T = 1/inc; the linear ramp runs from the PREVIOUS capture
        // and trails by T − r·T, r the phase left after a wrap — T when inc is exact in binary (0.5),
        // T − 1 when it falls just short (0.1), T − ½ on average; T − ½ is never more than ½ off.
        // 0 while not reducing (inc ≥ 1).
        [[nodiscard]] Real nominalDelaySamples() const noexcept
        {
            if (inc_ >= 1.0)
                return 0.0;
            return zoh_ ? 0.5 / inc_ - 0.5 : 1.0 / inc_ - 0.5;
        }

        // Processes one sample with a caller-supplied CAPTURE-TIME quantiser (M1-09xd, ADR-005).
        // `quantize` (a float(float) callable) is applied to each CAPTURED sample — and to the
        // bypass / first-prime sample — so a bits/compand quantiser can run genuinely BETWEEN
        // capture and reconstruction (the §0 order "var-rate · bits · compand · ZOH"): ZOH holds
        // and linear ramps then interpolate between QUANTISED captures, spec-exact for BOTH modes.
        // The quantiser is a template param, so it inlines with no std::function allocation (the
        // M1-02b Oversampler shaper idiom). With an identity quantiser this is bit-identical to the
        // single-argument processSample(x) below. inc_ = fs_eff/fs captures-per-host-sample (in
        // (0, 1] once reducing); inc_ >= 1 is a pass through the quantiser. Pure arithmetic; zero alloc.
        template <class Quantize>
        float processSample (float x, Quantize&& quantize) noexcept
        {
            if (inc_ >= 1.0) // effective rate >= host rate: no rate reduction (grit still applies)
                return quantize (x);

            if (first_) // prime the hold on the very first reducing sample
            {
                heldPrev_ = heldCur_ = static_cast<Real> (quantize (x));
                first_ = false;
                phase_ = 0.0;
                return static_cast<float> (heldCur_);
            }

            phase_ += inc_;
            if (phase_ >= 1.0)
            {
                phase_ -= 1.0; // carry the leftover -> self-correcting, no unbounded drift
                heldPrev_ = heldCur_;
                heldCur_ = static_cast<Real> (quantize (x));
            }

            const Real out = zoh_ ? heldCur_ : heldPrev_ + (heldCur_ - heldPrev_) * phase_;
            return static_cast<float> (out);
        }

        // Processes one sample with no capture-domain processing (the original API). Delegates to
        // the quantiser overload with identity, so it is bit-identical to the prior implementation
        // (float->double->float round-trips exactly) — the committed golden is preserved.
        float processSample (float x) noexcept
        {
            return processSample (x, [] (float v) noexcept { return v; });
        }

    private:
        void recompute() noexcept; // control-rate: fs_eff and inc_ (exp2/log2), in the .cpp

        static constexpr Wide kReferenceHz = 261.6255653005986; // C4 — pitchMult == 1 here
        static constexpr Wide kMinRateHz = 1.0; // effective-rate floor (avoid 0/negative)
        static constexpr Real kMinInc = 1.0e-9; // inc floor (avoid an eternal hold)

        // Control-rate parameters + derived coefficient.
        Wide fs_ = 44100.0;
        Real deviceRate_ = 44100.0; // default == host -> transparent bypass
        Real track_ = 0.0;
        Wide noteHz_ = kReferenceHz;
        Real fsEff_ = 44100.0;
        Wide inc_ = 1.0; // fs_eff / fs (captures per host sample)

        // Reconstruction state.
        Wide phase_ = 0.0;
        Real heldCur_ = 0.0;
        Real heldPrev_ = 0.0;
        bool first_ = true; // capture-and-pass the first reducing sample
        bool zoh_ = true;
    };
}
