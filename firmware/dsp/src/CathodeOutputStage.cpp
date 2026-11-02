#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/CathodeOutputStage.h>

#include <algorithm>
#include <cmath>

namespace nemesis::dsp
{
    void CathodeOutputStage::setVelocity (float velocity01) noexcept
    {
        velocity_ = clamp01 (velocity01);
        updateVelGain();
    }

    void CathodeOutputStage::glideVelocity (float velocity01) noexcept
    {
        const float from = velGain_;
        velocity_ = clamp01 (velocity01);
        updateVelGain(); // resolves the target into velGain_ and cancels any glide…
        velGainTarget_ = velGain_;
        velGain_ = from; // …and the glide starts from where the ear is
        velGlideStep_ = (velGainTarget_ - from) / static_cast<float> (velGlideLen_);
        velGlideLeft_ = velGlideLen_;
    }

    void CathodeOutputStage::setVelSens (float sens01) noexcept
    {
        velSens_ = clamp01 (sens01);
        updateVelGain();
    }

    void CathodeOutputStage::setPan (float pan) noexcept
    {
        pan_ = std::clamp (pan, -1.0f, 1.0f);
        updatePan();
    }

    void CathodeOutputStage::updateVelGain() noexcept
    {
        // velGain = (1 − velSens) + velSens · velocity^γ. velSens 0 → 1 (velocity inert); the γ = 1.5
        // shape makes the vel-1.0/0.5 gain ratio 2^1.5 (the PunchTransient burst-gain convention).
        const Real shaped = std::pow (static_cast<Real> (velocity_), static_cast<Real> (kVelGamma));
        const Real sens = static_cast<Real> (velSens_);
        velGain_ = static_cast<float> ((1.0 - sens) + sens * shaped);
        velGainTarget_ = velGain_;
        velGlideLeft_ = 0; // a snapped write ends any glide (T-CGUI-69 leg 3)
    }

    void CathodeOutputStage::updatePan() noexcept
    {
        // Equal-power (constant channel-power) law: panL² + panR² = 1. The √ form gives EXACT endpoints
        // (the silent channel's gain is exactly 0) unlike the cos/sin form (cos(π/2) ≈ 6e-17).
        const Real p = static_cast<Real> (pan_);
        panL_ = static_cast<float> (std::sqrt (0.5 * (1.0 - p)));
        panR_ = static_cast<float> (std::sqrt (0.5 * (1.0 + p)));
    }
}
