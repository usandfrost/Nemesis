#pragma once

#include <nemesis/dsp/Real.h>
#include <cmath>

namespace nemesis::dsp
{
    // [D-M-0318] One-pole, both taps — the first-order filter `GampEffect` (coupling caps, plate-load
    // roll-off, DC blocker) and `DriveEffect` (RC gain-set leg, post-clip roll-off, DC blocker) each held
    // a private copy of. Extracted bit-exact: the same state update in double, the same coefficient law.
    //
    // `hp(x) == x − lp(x)` in the SAME frame (it advances the low-pass), so a coupling cap costs one
    // extra subtract and the pair reconstructs the input exactly.
    //
    // RT: `lp`/`hp` are pure arithmetic. `setCutoff` is control rate (one `exp`).
    struct OnePole
    {
        // `a = 1 − exp(−2π·fc/fs)`. A non-positive corner or rate gives `a = 0` — a silent low-pass,
        // a transparent high-pass — and the result is held to [0, 1], so both ends are legal and need
        // no special case downstream. The corner is NOT clamped below Nyquist here: a consumer that
        // wants that guard applies it to `hz` first (`DriveEffect` clamps to 0.49·fs).
        void setCutoff (Real hz, Real fs) noexcept
        {
            constexpr Real kTwoPi = 6.283185307179586;
            if (hz <= 0.0 || fs <= 0.0)
            {
                a_ = 0.0;
                return;
            }
            const Real a = 1.0 - std::exp (-kTwoPi * hz / fs);
            a_ = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
        }
        void reset() noexcept { z_ = 0.0; }

        [[nodiscard]] float lp (float x) noexcept
        {
            z_ += a_ * (static_cast<Real> (x) - z_);
            return static_cast<float> (z_);
        }
        [[nodiscard]] float hp (float x) noexcept { return x - lp (x); }

        Real a_ = 1.0;
        Real z_ = 0.0;
    };
}
