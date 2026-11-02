#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/BypassFade.h>

#include <cmath>
#include <cstddef>
#include <vector>

namespace nemesis::dsp
{
    // Loop-point error — the CATHODE §2 "broken converter" `synth.conv.loopErr` subset
    // (CATHODE.md §2 "loop-point error"). It models the classic vintage-sampler flaw where a
    // single-cycle wave's loop points are NOT quite on integer sample boundaries: the loop is a
    // SUB-SAMPLE off, so each time it wraps the readout jumps slightly, injecting a PERIODIC
    // discontinuity at the fundamental — a gritty buzz/click that grows with the amount.
    //
    // This is a SIBLING of BrokenConverter (M1-05, bits+compand), SampleRateReducer (M1-09xa,
    // rate+track+interp) and ConverterDrift (M1-09xb, drift/jitter) — the four make up the §2
    // broken converter. The §0-flow ordering of loopErr within that chain is a CathodeVoiceStage
    // COMPOSITION concern, not this primitive.
    //
    // Model (bounded, streaming-friendly): the input is written to a small ring buffer and read
    // back through a fractional delay whose offset RAMPS by up to `loopErr` sub-samples across each
    // loop period (P = fs/noteHz samples) and SNAPS back to zero at the seam. The accumulated ramp
    // IS "the loop drifts a sub-sample off"; the snap IS the periodic seam discontinuity. Because
    // the offset is a within-period sawtooth it returns to its start every period, so the read
    // pointer never drifts unboundedly (unlike a permanent per-loop offset). The discontinuity is
    // signal-dependent (its size is the local slope × the error) — a constant input has no seam,
    // exactly like a constant single-cycle wave has no loop mismatch.
    //
    // Do NOT oversample this (CATHODE.md §2/§11): the aliased seam IS the effect. At `loopErr = 0`
    // it is a bit-exact BYPASS (output == input) — the "dial to clean" anchor. The loop period in
    // samples is SR-dependent (P = fs/noteHz), so a render is per-SR (per-SR golden). `setNoteHz`
    // lets the voice layer track the played note; per-voice loopErr SPREAD (§6) is a voice concern.
    //
    // Hot path is pure arithmetic: no libm, no allocation, no JUCE. Even the control-rate period
    // math is transcendental-free (a division), so the whole primitive is header-only. Deterministic
    // (§11): same input + same param snapshot -> bit-identical output; reset() clears state. Computed
    // in double, emitted as float. Not thread-safe against prepare().
    class LoopError
    {
    public:
        // Allocates the modulation ring buffer and resolves the (SR-dependent) loop period. Clears
        // state. Not RT-safe (allocates) — call like prepareToPlay.
        void prepare (Real hostSampleRate)
        {
            fs_ = hostSampleRate > 0.0 ? hostSampleRate : 44100.0;

            // The read delay is center + offset, offset in [0, kMaxErrSamples]. center gives the
            // read pointer headroom below the write head for the linear-interp neighbour.
            center_ = kGuardSamples;
            size_ = static_cast<int> (center_ + kMaxErrSamples) + 3; // + interp/overshoot margin
            if (size_ < 4)
                size_ = 4;
            buf_.assign (static_cast<std::size_t> (size_), 0.0);

            setNoteHz (noteHz_);
            fade_.prepare (fs_);
            reset();
        }

        // Fundamental of the looped wave in Hz -> loop period P = fs/noteHz samples (control-rate;
        // a division, no libm). Clamped so 2 <= P (noteHz in [minHz, Nyquist]).
        void setNoteHz (float noteHz) noexcept
        {
            const Real nyq = 0.5 * fs_;
            Real n = static_cast<Real> (noteHz);
            if (n < kMinNoteHz)
                n = kMinNoteHz;
            if (n > nyq)
                n = nyq;
            noteHz_ = static_cast<float> (n);
            period_ = fs_ / n; // >= 2 by the clamp
            if (phase_ >= period_) // FIREFORK B9: a shorter period: keep the phase inside it (the read stays in the ring)
                phase_ = std::fmod (phase_, period_);
        }

        // The single loop-error macro in [0, 1]: 0 = bit-exact bypass, 1 = a full sub-sample seam.
        // Control-rate (only scales the per-period ramp in the hot path).
        void setLoopErr (float amount) noexcept
        {
            const Real a = amount < 0.0f ? 0.0 : (amount > 1.0f ? 1.0 : static_cast<Real> (amount));
            err_ = a * kMaxErrSamples; // sub-sample loop offset at full amount
            if (err_ > 0.0)
                wetErr_ = err_; // [T-CGUI-68 S5R] the wet leg holds its last amount through a fade-out
            // [T-CGUI-68 leg 2] Crossing 0 FADES (10 ms, the BypassFade law) — except before the first
            // sample after a reset, where there is no signal to click and a patch load must land as is.
            fade_.set (err_ <= 0.0);
            if (fresh_)
                fade_.settle();
        }

        // [T-CORE-18] The macro amount back out (introspection / tests): the §2.2 GRIT
        // depth-scaler drives this and `ConverterDrift::setDrift` with the SAME range, curve
        // and interpolation law, so a transposed pair is invisible to any render assertion.
        // Recovered from the stored seam rather than duplicated in a field.
        [[nodiscard]] float loopErr() const noexcept
        {
            return static_cast<float> (err_ / kMaxErrSamples);
        }

        // [T-CGUI-09] The NOMINAL delay, in samples: the centre plus the MEAN of the sawtooth
        // offset (it ramps 0 → err_ each loop), 0 in bypass, fade-weighted between (T-CGUI-69 leg 1).
        // See ConverterDrift::nominalDelaySamples.
        [[nodiscard]] Real nominalDelaySamples() const noexcept
        {
            return static_cast<Real> (fade_.gain()) * (center_ + 0.5 * wetErr_);
        }

        // Clears the ring buffer and the loop phase. Does NOT touch the control-rate period.
        void reset() noexcept
        {
            for (Real& v : buf_)
                v = 0.0;
            writePos_ = 0;
            phase_ = 0.0;
            fresh_ = true;
            fade_.settle(); // [T-CGUI-68 S5R] the fade GAIN is audio state and snaps
        }

        // ⚠ PRECONDITION: prepare() first — the parked path writes the ring too (T-CGUI-68).
        // Processes one sample. loopErr = 0 (settled) -> bit-exact pass. Otherwise: write the input,
        // read it back through the ramp+snap fractional delay, advance the loop phase. Pure
        // arithmetic; zero allocation.
        float processSample (float x) noexcept
        {
            fresh_ = false;
            buf_[static_cast<std::size_t> (writePos_)] = static_cast<Real> (x);
            if (fade_.parked()) // macro off: transparent bypass (the "dial to clean" anchor)
            {
                // [T-CGUI-68 leg 2] …that keeps WRITING, so a resume reads the live signal late,
                // never a stale ring. The loop phase holds. (With a ~3-sample ring the fade already
                // masks a stale read here — no test can see this write; ConverterDrift's 62 need it.)
                if (++writePos_ >= size_)
                    writePos_ = 0;
                return x;
            }

            // Within-period sawtooth read offset: ramps 0 -> err_ across the loop, snaps back at the
            // seam. delay always in [center_, center_ + err_] -> read pointer stays behind the write.
            const Real offset = wetErr_ * (phase_ / period_);
            const Real delay = center_ + offset;

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

            // Advance the loop phase; wrap by SUBTRACTING the period (carry the leftover) so a
            // fractional period self-corrects, no unbounded drift. period_ >= 2 -> one subtraction.
            phase_ += 1.0;
            if (phase_ >= period_)
                phase_ -= period_;

            const auto wet = static_cast<float> (s);
            if (fade_.inCircuit())
                return wet;
            fade_.tick();
            return x + fade_.gain() * (wet - x);
        }

    private:
        // Design constants (the fixed "character" the single macro scales).
        static constexpr Real kMaxErrSamples = 1.0; // sub-sample loop offset at full amount
        static constexpr Real kGuardSamples = 2.0; // read-pointer headroom below the write head
        static constexpr Real kMinNoteHz = 8.0; // floor so period_ stays finite/sane

        // Control-rate parameters + derived coefficients.
        Real fs_ = 44100.0;
        float noteHz_ = 261.626f; // C4 default (matches the SampleRateReducer reference note)
        Real period_ = 44100.0 / 261.626; // loop period in samples
        Real err_ = 0.0; // loop offset in samples (0 = bypass)
        Real center_ = kGuardSamples; // nominal read delay (samples)
        int size_ = 4; // ring buffer length

        // Per-sample state.
        std::vector<Real> buf_;
        int writePos_ = 0;
        Real phase_ = 0.0; // loop phase in [0, period_)
        BypassFade fade_ = [] {
            BypassFade f;
            f.set (true); // err_ starts at 0: parked
            f.settle();
            return f;
        }();
        bool fresh_ = true; // no sample since reset(): a 0-crossing settles instead of fading
        Real wetErr_ = 0.0; // the wet leg's offset: `err_`, held at its last non-zero value while fading out
    };
}
