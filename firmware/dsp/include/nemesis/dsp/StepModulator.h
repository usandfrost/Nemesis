#pragma once

#include <nemesis/dsp/Real.h>
#include <array>
#include <cstddef>

namespace nemesis::dsp
{
    // StepModulator — the CATHODE §4 step-modulator primitive (CATHODE.md §4 "a step modulator (a
    // short stepped/'mini-sequencer' CV lane) for rhythmic industrial movement"; `synth.step.*`,
    // feeds the §9 `MOTION` dock macro). A pure per-sample GENERATOR: processSample() takes no audio
    // input and returns a bipolar control value in [-1, 1] scaled by `depth` ∈ [0, 1].
    //
    // A short looping sequence of up to kMaxSteps caller-set bipolar step VALUES; the active window
    // is `stepCount` steps, advanced one step per period (period = fs/rateHz, free-running) and
    // looping back to step 0. The output is held FLAT within each step (piecewise-constant — the
    // "stepped" character). Unlike the M1-17a LFO's SampleHold shape (which latches RANDOM values),
    // the step values are caller-set → the modulator is fully DETERMINISTIC with NO RNG.
    //
    // NOT band-limited: a sub-audio CV lane whose hard steps are WANTED; anti-click is the
    // destination-smoothing / mod-matrix concern, not the source (as the LFO).
    //
    // Convention (as AdsrEnvelope / Lfo): processSample() RETURNS the current sample's value, THEN
    // advances the phase (and, at a period wrap, the step index).
    //
    // RT-safety (§11): prepare() caches the sample rate + resolves the phase increment; the hot path
    // is inline pure arithmetic — no libm, no allocation, no locks (inc = rateHz/fs is a division;
    // there are no transcendentals, so the whole primitive is header-only). Deterministic (§11): a
    // given (values + params) renders bit-identically; reset() returns to step 0. Computed in double,
    // emitted as float. No JUCE. Not thread-safe against prepare().
    class StepModulator
    {
    public:
        static constexpr int kMaxSteps = 16;

        // Caches the sample rate, resolves the phase increment, and resets (step 0, phase 0).
        // RT-safe (no allocation).
        void prepare (Wide sampleRate) noexcept
        {
            sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
            updateInc();
            reset();
        }

        // [T-CGUI-59] BAR-LOCK: put the pattern at `steps` elapsed since the bar grid's origin — the
        // step index is the whole part modulo the count, the phase the fraction. Negative positions
        // (a host's pre-roll) wrap the same way. No libm. RT-safe.
        void lockPosition (Real steps) noexcept
        {
            auto whole = static_cast<long long> (steps);
            Real frac = steps - static_cast<Real> (whole);
            if (frac < 0.0)
            {
                frac += 1.0;
                --whole;
            }
            if (frac >= 1.0) // [S5R r4] a tiny negative position rounds to exactly 1.0: that is the NEXT step
            {
                frac = 0.0;
                ++whole;
            }
            const long long n = static_cast<long long> (stepCount_);
            step_ = static_cast<int> (((whole % n) + n) % n);
            phase_ = frac;
        }
        // [T-CGUI-59] Read-back: the current step index (tests pin the lock through it).
        [[nodiscard]] int currentStep() const noexcept { return step_; }
        [[nodiscard]] Wide stepPhase() const noexcept { return phase_; } // [T-CGUI-59] within the current step
        [[nodiscard]] int stepCount() const noexcept { return stepCount_; }

        // Returns to step 0 / phase 0. RT-safe. Does not change the parameters or step values.
        void reset() noexcept
        {
            phase_ = 0.0;
            step_ = 0;
        }

        // Advance rate in Hz — one step per 1/rateHz seconds (`synth.step.rate`). Clamped ≥ 0
        // (0 → the sequence never advances, holding step 0). Control-rate.
        void setRateHz (float hz) noexcept
        {
            rateHz_ = hz > 0.0f ? hz : 0.0f;
            updateInc();
        }

        // Number of active steps in the loop, clamped to [1, kMaxSteps] (`synth.step.count`).
        // Control-rate. Does not reset the current position (a mid-run change re-wraps on the next
        // boundary); call reset() to restart the sequence.
        void setStepCount (int count) noexcept
        {
            stepCount_ = count < 1 ? 1 : (count > kMaxSteps ? kMaxSteps : count);
            if (step_ >= stepCount_)
                step_ = 0;
        }

        // Sets the bipolar value of step `index` (clamped to [-1, 1]). Out-of-range indices are
        // ignored. Control-rate.
        void setStepValue (int index, float value) noexcept
        {
            if (index < 0 || index >= kMaxSteps)
                return;
            const float v = value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
            values_[static_cast<std::size_t> (index)] = static_cast<Real> (v);
        }

        // The stored value of step `index` (post-clamp), or 0 out of range. [T-CORE-14 leaf C] The
        // read-back for the `step.values[]` side-state edge: the lane is 16 cells that core owns and
        // the param layer carries across, and "the lane reached the instrument" cannot be read off
        // value() (which only ever shows the CURRENT step, scaled by depth). The
        // `Lfo::rateHz()`/`AdsrEnvelope::attackMs()` pin idiom.
        [[nodiscard]] float stepValue (int index) const noexcept
        {
            if (index < 0 || index >= kMaxSteps)
                return 0.0f;
            return static_cast<float> (values_[static_cast<std::size_t> (index)]);
        }

        // Output depth in [0, 1] (`synth.step.depth`). A pure output multiply. Control-rate.
        void setDepth (float d) noexcept { depth_ = d < 0.0f ? 0.0 : (d > 1.0f ? 1.0 : static_cast<Real> (d)); }
        // [T-CORE-14 leaf D] The applied depth, read back — `Lfo::depth()`'s twin, and for the same
        // reason: `synth.step.depth` is the fourth member of the §2.9 MOTION set (Param Ref:202), so
        // the value that reaches this setter is `motion · dialed` and only an exact read-back can pin
        // that product.
        [[nodiscard]] float depth() const noexcept { return static_cast<float> (depth_); }

        // The current output value (depth · value[currentStep]), without advancing.
        [[nodiscard]] float value() const noexcept
        {
            return static_cast<float> (depth_ * values_[static_cast<std::size_t> (step_)]);
        }

        // Advances one sample and returns THIS sample's value (read first, then advance — as
        // AdsrEnvelope). Hot path: pure arithmetic, no libm calls / allocation / locks.
        float processSample() noexcept
        {
            const Real out = depth_ * values_[static_cast<std::size_t> (step_)];

            phase_ += inc_;
            // A while-loop wrap advances the step deterministically even if inc_ ≥ 1.
            while (phase_ >= 1.0)
            {
                phase_ -= 1.0;
                if (++step_ >= stepCount_)
                    step_ = 0;
            }

            return static_cast<float> (out);
        }

    private:
        void updateInc() noexcept { inc_ = rateHz_ / sampleRate_; }

        // Configuration / control-rate.
        Wide sampleRate_ = 48000.0;
        float rateHz_ = 1.0f;
        Real depth_ = 1.0;
        int stepCount_ = kMaxSteps;
        Wide inc_ = 1.0 / 48000.0; // phase increment per sample = rateHz / fs
        std::array<Real, kMaxSteps> values_ {}; // caller-set step values (default 0)

        // Per-sample state.
        Wide phase_ = 0.0; // [0, 1) within the current step
        int step_ = 0; // current step index in [0, stepCount_)
    };
}
