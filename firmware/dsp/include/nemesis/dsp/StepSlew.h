#pragma once

#include <nemesis/dsp/Real.h>
#include <algorithm>
#include <cmath>

namespace nemesis::dsp
{
    // [T-CGUI-83] A STEP-TRIGGERED slew LIMITER — not a low-pass. While the input's sample-to-sample
    // change stays at or under `threshold`, `process` returns the input UNTOUCHED (the same float), so
    // every smooth modulation through it is bit-exact. Only a change ABOVE the threshold starts a
    // linear ramp from the current output to the input, landing on it exactly `rampSamples` samples
    // later. The ramp follows a moving input (each sample covers 1/remaining of the gap), and another
    // step mid-ramp restarts the full length from where the output is, so the output never jumps.
    //
    // ONSET: for `onsetSamples` after a `reset` the input passes through and only becomes the
    // reference — no step is detected. That is a note's own attack (an instant filter envelope steps
    // on its second sample), and a new note never ramps from the previous note's value. From idle the
    // filter starts from empty state, so that step cannot click the way a mid-note one does. On a
    // Mono-RT restrike or a poly re-gate of the same voice the filter still HOLDS state: an LFO with
    // Trig sync or a Step reset jumps the cutoff on that sample, unramped for the window (an env
    // continues from its level and does not step); on a restrike the AMP's 2 ms dip masks it.
    //
    // RT-safety: `prepare` (two lround) is control rate; `process` is a compare, plus one divide
    // while ramping. No libm, no allocation.
    class StepSlew
    {
    public:
        void prepare (Real sampleRate, Real rampSeconds, Real onsetSeconds) noexcept
        {
            rampSamples_ = std::max (1, static_cast<int> (std::lround (sampleRate * rampSeconds)));
            onsetSamples_ = std::max (1, static_cast<int> (std::lround (sampleRate * onsetSeconds)));
            reset();
        }
        void setThreshold (float threshold) noexcept { threshold_ = threshold; }
        void reset() noexcept
        {
            passLeft_ = onsetSamples_;
            left_ = 0;
        }

        float process (float x) noexcept
        {
            const float d = x - in_;
            in_ = x;
            if (passLeft_ > 0)
            {
                --passLeft_;
                out_ = x;
                return x;
            }
            if (d > threshold_ || d < -threshold_)
                left_ = rampSamples_;
            if (left_ == 0)
            {
                out_ = x;
                return x;
            }
            out_ = left_ == 1 ? x : out_ + (x - out_) / static_cast<float> (left_);
            --left_;
            return out_;
        }

        [[nodiscard]] bool ramping() const noexcept { return left_ > 0; }
        [[nodiscard]] float value() const noexcept { return out_; }
        [[nodiscard]] int rampSamples() const noexcept { return rampSamples_; }
        [[nodiscard]] int onsetSamples() const noexcept { return onsetSamples_; }

    private:
        float threshold_ = 0.0f;
        float in_ = 0.0f;
        float out_ = 0.0f;
        int rampSamples_ = 1;
        int onsetSamples_ = 1;
        int passLeft_ = 1;
        int left_ = 0;
    };
} // namespace nemesis::dsp
