#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/AdsrEnvelope.h>

#include <algorithm>
#include <cmath>

namespace nemesis::dsp
{
    // [T-CGUI-81] Each setter recomputes ONLY the coefficient its time feeds — one `pow` per
    // attack/decay/release setter, none for delay/hold — so a routed envelope time pays for one
    // coefficient, not three. The formulas are unchanged, so every coefficient is bit-equal to the
    // full recompute `prepare()` runs (and the constructor, so the invariant holds before prepare).
    void AdsrEnvelope::prepare (Real sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        updateCoefficients();
        reset();
    }

    void AdsrEnvelope::setDelayMs (float ms) noexcept
    {
        delayMs_ = ms > 0.0f ? ms : 0.0f;
        updateDelay();
    }

    void AdsrEnvelope::setAttackMs (float ms) noexcept
    {
        attackMs_ = ms > 0.0f ? ms : 0.0f;
        updateAttack();
    }

    void AdsrEnvelope::setHoldMs (float ms) noexcept
    {
        holdMs_ = ms > 0.0f ? ms : 0.0f;
        updateHold();
    }

    void AdsrEnvelope::setDecayMs (float ms) noexcept
    {
        decayMs_ = ms > 0.0f ? ms : 0.0f;
        updateDecay();
    }

    void AdsrEnvelope::setReleaseMs (float ms) noexcept
    {
        releaseMs_ = ms > 0.0f ? ms : 0.0f;
        updateRelease();
    }

    void AdsrEnvelope::updateCoefficients() noexcept
    {
        // [T-CGUI-40] The restrike dip depends on the rate alone, so only a full recompute sets it.
        dipSamples_ = std::max (1, static_cast<int> (kRestrikeDipMs * (sampleRate_ * 0.001)));
        updateDelay();
        updateHold();
        updateAttack();
        updateDecay();
        updateRelease();
    }

    // Delay + Hold: integer sample counts (truncated, matching a phase countdown). hold 0 → 0
    // samples → Attack hands straight to Decay (bit-exact vs the prior DADSR).
    void AdsrEnvelope::updateDelay() noexcept
    {
        delaySamples_ = static_cast<int> (static_cast<Real> (delayMs_) * (sampleRate_ * 0.001));
    }

    void AdsrEnvelope::updateHold() noexcept
    {
        holdSamples_ = static_cast<int> (static_cast<Real> (holdMs_) * (sampleRate_ * 0.001));
    }

    // Attack [T-CMP-30]: an RC charge from 0 toward T reads T·(1 − c^n); it crosses 1.0 at n = N when
    // c = (1 − 1/T)^(1/N), so a fresh note reaches the peak at attack·fs samples. A zero attack is
    // c = 0: the first step lands on T, past the peak — effectively instant.
    void AdsrEnvelope::updateAttack() noexcept
    {
        const Real attackSamples = static_cast<Real> (attackMs_) * (sampleRate_ * 0.001);
        attackK_ = attackSamples > 0.0 ? 1.0 - std::pow (1.0 - 1.0 / kAttackTarget, 1.0 / attackSamples) : 1.0;
    }

    // Decay / Release: exponential per-step factor hitting −60 dB (1e-3) at the set time
    // (the analog RC convention, as ModalResonator's T60). Zero time → factor 0 → instant.
    void AdsrEnvelope::updateDecay() noexcept
    {
        const Real decaySamples = static_cast<Real> (decayMs_) * (sampleRate_ * 0.001);
        decayCoef_ = decaySamples > 0.0 ? std::pow (10.0, -3.0 / decaySamples) : 0.0;
    }

    void AdsrEnvelope::updateRelease() noexcept
    {
        const Real releaseSamples = static_cast<Real> (releaseMs_) * (sampleRate_ * 0.001);
        releaseCoef_ = releaseSamples > 0.0 ? std::pow (10.0, -3.0 / releaseSamples) : 0.0;
    }
}
