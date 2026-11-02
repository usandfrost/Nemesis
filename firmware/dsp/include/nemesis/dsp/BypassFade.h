#pragma once

#include <nemesis/dsp/Real.h>
#include <algorithm>
#include <cmath>

namespace nemesis::dsp
{
    // [T-CGUI-08] ONE STAGE'S BYPASS AS A LINEAR GAIN FADE — the law every CATHODE stage bypass
    // shares (Param Ref §2.13, R15: route the stage out, keep every parameter, no click). The gain
    // runs 1 → 0 on a bypass and 0 → 1 on its release over `prepare`'s 10 ms (the machine's own
    // control-ramp time), and its last step lands EXACTLY on the target, so a caller can branch on
    // `inCircuit()` / `parked()` and keep both endpoints bit-exact: in circuit the stage's built
    // expression runs untouched, parked the stage is not in the signal at all.
    //
    // The TARGET is a control parameter and survives `settle()`; the GAIN is audio state and snaps
    // to it there — the `reset()` rule of every stage that owns one of these.
    class BypassFade
    {
    public:
        void prepare (Real sampleRate) noexcept
        {
            len_ = std::max (1, static_cast<int> (sampleRate * 0.010));
            step_ = 1.0f / static_cast<float> (len_);
        }

        // Returns false when the target did not change (a repeat write is not a new fade).
        bool set (bool bypassed) noexcept
        {
            if (bypassed == bypassed_)
                return false;
            bypassed_ = bypassed;
            const float target = bypassed ? 0.0f : 1.0f;
            left_ = static_cast<int> (std::ceil (std::abs (target - gain_) / step_));
            if (left_ == 0)
                gain_ = target;
            return true;
        }

        // Advances ONE sample. Returns true on the sample a fade-OUT lands, so the caller can clear
        // the state it will not run while parked.
        bool tick() noexcept
        {
            if (left_ == 0)
                return false;
            gain_ += bypassed_ ? -step_ : step_;
            if (--left_ == 0)
            {
                gain_ = bypassed_ ? 0.0f : 1.0f;
                return bypassed_;
            }
            gain_ = std::clamp (gain_, 0.0f, 1.0f);
            return false;
        }

        // Ends any fade at once, on its target.
        void settle() noexcept
        {
            left_ = 0;
            gain_ = bypassed_ ? 0.0f : 1.0f;
        }

        [[nodiscard]] bool bypassed() const noexcept { return bypassed_; }
        [[nodiscard]] float gain() const noexcept { return gain_; }
        [[nodiscard]] bool fading() const noexcept { return left_ > 0; }
        [[nodiscard]] bool inCircuit() const noexcept { return left_ == 0 && !bypassed_; } // gain exactly 1
        [[nodiscard]] bool parked() const noexcept { return left_ == 0 && bypassed_; } // gain exactly 0

    private:
        bool bypassed_ = false;
        float gain_ = 1.0f;
        int left_ = 0;
        int len_ = 480;
        float step_ = 1.0f / 480.0f;
    };
}
