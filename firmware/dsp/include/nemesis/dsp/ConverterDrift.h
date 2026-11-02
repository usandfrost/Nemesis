#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/BypassFade.h>
#include <nemesis/dsp/ValueNoise.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nemesis::dsp
{
    // Converter drift/jitter — the CATHODE §2 "broken converter" `synth.conv.drift` subset
    // (CATHODE.md §2 "drift/jitter"). It models the way a vintage sampler/converter is never
    // quite static: slow SEEDED-random pitch wow/flutter (+clock jitter) plus a slow level
    // instability, all scaled by one `drift` macro. The pitch term is a low-frequency-noise ->
    // fractional-delay Doppler modulation (the §12.1 clean-path model; NOT the GPL ChowTapeModel);
    // the level term is a low-frequency-noise amplitude wobble.
    //
    // This is a SIBLING of BrokenConverter (M1-05, bits+compand) and SampleRateReducer (M1-09xa,
    // rate+track+interp) — the three make up the §2 broken converter. The §0-flow ordering of
    // drift within that chain is a CathodeVoiceStage COMPOSITION concern, not this primitive; the
    // periodic loop-point error (`loopErr`) is the OTHER M1-09x artifact (its own follow-on card).
    //
    // [T-CMP-08 leaf C] The low-frequency modulator this primitive invented — seeded value noise —
    // was LIFTED out of here into `dsp/ValueNoise.h` unchanged, because the Analog/Tape delay
    // family needs the same generator for clock drift and wow/flutter. Nothing about this class's
    // behaviour moved: its committed golden is the equivalence proof, and it did not change.
    //
    // Do NOT oversample this (CATHODE.md §2/§11): like every digital lo-fi stage the artifact IS
    // the effect. At `drift = 0` it is a bit-exact BYPASS (output == input) — the "dial to clean"
    // anchor. The low-frequency modulators are seeded "value noise" (random targets linearly
    // interpolated at a rate in Hz), so a higher host rate stretches the segment lengths in
    // samples; `setSeed` lets the voice layer give each voice its own deterministic variation
    // (§6 rotation / per-voice spread) — bounce-parity holds (§11).
    //
    // Hot path is pure arithmetic: no libm, no allocation, no JUCE. Even the control-rate
    // segment/depth math is transcendental-free (a division + integer rounding), so the whole
    // primitive is header-only. Deterministic (§11): same input + same param snapshot + same seed
    // -> bit-identical output; reset() reseeds. Computed in double, emitted as float.
    // Not thread-safe against prepare().
    class ConverterDrift
    {
    public:
        // Allocates the modulation delay buffer and resolves the (SR-dependent) segment lengths
        // and pitch depth. Clears state and reseeds. Not RT-safe (allocates) — call like
        // prepareToPlay.
        void prepare (Real hostSampleRate)
        {
            fs_ = hostSampleRate > 0.0 ? hostSampleRate : 44100.0;

            // Pitch-wander depth in samples: fixed in TIME so the peak pitch deviation is roughly
            // sample-rate-independent. center > depth so the read pointer never crosses the write
            // pointer nor goes non-positive at full modulation.
            depth_ = kMaxDepthSeconds * fs_;
            center_ = depth_ + kGuardSamples;
            size_ = static_cast<int> (kGuardSamples + 2.0 * depth_) + 3; // + interp/overshoot margin
            if (size_ < 4)
                size_ = 4;
            buf_.assign (static_cast<std::size_t> (size_), 0.0);

            wow_.seg = ValueNoise::segFromRate (fs_, kWowRateHz);
            flutter_.seg = ValueNoise::segFromRate (fs_, kFlutterRateHz);
            level_.seg = ValueNoise::segFromRate (fs_, kLevelRateHz);

            fade_.prepare (fs_);
            reset();
        }

        // The single drift macro in [0, 1]: 0 = bit-exact bypass, 1 = full wow/flutter/jitter +
        // level instability. Control-rate (trivial; only scales the modulators in the hot path).
        void setDrift (float amount) noexcept
        {
            drift_ = amount < 0.0f ? 0.0 : (amount > 1.0f ? 1.0 : static_cast<Real> (amount));
            // [T-CGUI-68 S5R] The WET leg keeps the last non-zero amount through a fade-OUT: computing it
            // at drift 0 would jump the read head (up to ~57 samples) on the fade's first sample — the
            // very click the fade exists to remove.
            if (drift_ > 0.0)
                wetDrift_ = drift_;
            // [T-CGUI-68 leg 2] Crossing 0 FADES (10 ms, the BypassFade law) — except before the first
            // sample after a reset, where there is no signal to click and a patch load must land as is.
            fade_.set (drift_ <= 0.0);
            if (fresh_)
                fade_.settle();
        }

        // [T-CORE-18] The macro amount back out (introspection / tests): the §2.2 GRIT
        // depth-scaler drives this and `LoopError::setLoopErr` on the SAME linear leg, so a
        // transposed pair renders differently from the truth but identically to each other.
        [[nodiscard]] float drift() const noexcept { return static_cast<float> (drift_); }

        // [T-CGUI-09] The NOMINAL delay this stage imposes, in samples: the read pointer's centre
        // while active (the wow/flutter swings AROUND it and cannot be matched), 0 in bypass. What
        // a latency-matched dry leg is delayed by (R40). [T-CGUI-69 leg 1] Weighted by the 0-crossing
        // fade's gain: the output is x + g·(wet − x), so mid-fade the delay is g·centre, not the target's.
        [[nodiscard]] Real nominalDelaySamples() const noexcept { return static_cast<Real> (fade_.gain()) * center_; }

        // [T-CGUI-24, owner 2026-09-25] The converter's wander as a MOD SOURCE (`Source::ConvDriftReadout`),
        // in (−1, 1): the pitch modulator scaled by the patch's drift amount and the 0-crossing fade's
        // gain, so a patch with no drift sends nothing and a crossing is continuous. Lags one sample
        // behind the matrix (the voice reads it before this stage runs).
        [[nodiscard]] float readout() const noexcept
        {
            return static_cast<float> (static_cast<Real> (fade_.gain()) * wetDrift_ * mp_);
        }

        // Base RNG seed for the (deterministic) modulators — drives per-voice variation (§6).
        // Reseeds and clears state so a render from here is reproducible.
        void setSeed (std::uint32_t seed) noexcept
        {
            baseSeed_ = seed;
            reset();
        }

        // Clears the delay buffer + modulator phases and RESEEDS the modulators (like
        // BbdDelayLine). Does NOT touch the control-rate segment/depth coefficients.
        void reset() noexcept
        {
            for (Real& v : buf_)
                v = 0.0;
            writePos_ = 0;
            mp_ = 0.0;

            wow_.seed = baseSeed_;
            flutter_.seed = baseSeed_ ^ 0x9E3779B9u; // decorrelated streams
            level_.seed = baseSeed_ + 0x6D2B79F5u;
            wow_.reseed();
            flutter_.reseed();
            level_.reseed();
            fresh_ = true;
            fade_.settle(); // [T-CGUI-68 S5R] the fade GAIN is audio state and snaps (BypassFade's reset rule)
        }

        // ⚠ PRECONDITION: prepare() first — the parked path writes the ring too (T-CGUI-68), so an
        // unprepared instance is no longer a safe no-op at drift 0.
        // Processes one sample. drift = 0 (settled) -> bit-exact pass. Otherwise: write the input,
        // read it back through a low-freq-noise-modulated fractional delay (Doppler pitch
        // wow/flutter), then apply the low-freq-noise level wobble. Pure arithmetic; zero allocation.
        float processSample (float x) noexcept
        {
            fresh_ = false;
            buf_[static_cast<std::size_t> (writePos_)] = static_cast<Real> (x);
            if (fade_.parked()) // macro off: transparent bypass (the "dial to clean" anchor)
            {
                // [T-CGUI-68 leg 2] …that keeps WRITING, so a resume reads the live signal late,
                // never a stale ring. The modulators hold.
                if (++writePos_ >= size_)
                    writePos_ = 0;
                return x;
            }

            // Pitch: wow (slow) + flutter (faster), each in [-1, 1) -> a bounded [-1, 1) term.
            const Real mp = kWowMix * wow_.next() + kFlutterMix * flutter_.next();
            mp_ = mp; // [T-CGUI-24] held for readout(); the modulators hold while parked, and so does this
            const Real delay = center_ + depth_ * wetDrift_ * mp; // in samples, always >= guard > 0

            Real rp = static_cast<Real> (writePos_) - delay + static_cast<Real> (size_);
            int i0 = static_cast<int> (rp); // rp > 0 by construction -> truncation == floor
            const Real frac = rp - static_cast<Real> (i0);
            i0 %= size_;
            int i1 = i0 + 1;
            if (i1 >= size_)
                i1 = 0;
            const Real s = buf_[static_cast<std::size_t> (i0)] * (1.0 - frac)
                             + buf_[static_cast<std::size_t> (i1)] * frac;

            ++writePos_;
            if (writePos_ >= size_)
                writePos_ = 0;

            // Level instability: a slow amplitude wobble around unity.
            const Real gain = 1.0 + kLevelDev * wetDrift_ * level_.next();
            const auto wet = static_cast<float> (s * gain);
            if (fade_.inCircuit())
                return wet;
            fade_.tick();
            return x + fade_.gain() * (wet - x);
        }

    private:
        // Design constants (the fixed "character" the single macro scales).
        static constexpr Real kMaxDepthSeconds = 0.0012; // pitch-wander depth at full drift
        static constexpr Real kGuardSamples = 4.0; // read-pointer headroom below the write head
        static constexpr Real kLevelDev = 0.15; // ±15% level wobble at full drift
        static constexpr Real kWowRateHz = 2.7; // slow wow
        static constexpr Real kFlutterRateHz = 11.0; // faster flutter/jitter
        static constexpr Real kLevelRateHz = 1.3; // slow level instability
        static constexpr Real kWowMix = 0.7; // wow + flutter mix stays within [-1, 1)
        static constexpr Real kFlutterMix = 0.3;
        static constexpr std::uint32_t kDefaultSeed = 0x1357'9BDFu;

        // Control-rate parameters + derived coefficients.
        Real fs_ = 44100.0;
        Real drift_ = 0.0;
        Real depth_ = 0.0; // pitch-wander depth (samples)
        Real center_ = 1.0; // nominal read delay (samples)
        int size_ = 4; // delay buffer length
        std::uint32_t baseSeed_ = kDefaultSeed;

        // Per-sample state.
        std::vector<Real> buf_;
        int writePos_ = 0;
        ValueNoise wow_, flutter_, level_;
        BypassFade fade_ = [] {
            BypassFade f;
            f.set (true); // drift_ starts at 0: parked
            f.settle();
            return f;
        }();
        bool fresh_ = true; // no sample since reset(): a 0-crossing settles instead of fading
        Real mp_ = 0.0; // [T-CGUI-24] the last pitch-modulator value, for readout()
        Real wetDrift_ = 0.0; // the wet leg's amount: `drift_`, held at its last non-zero value while fading out
    };
}
