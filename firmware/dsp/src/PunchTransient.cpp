#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/PunchTransient.h>

#include <array>
#include <cmath>
#include <cstddef>

namespace nemesis::dsp
{
    namespace
    {
        constexpr Real kPi = 3.14159265358979323846;
    }

    const float* PunchTransient::bandlimitedSineTable() noexcept
    {
        // One shared table for all voices; the magic-static guard's one-time init runs off the
        // audio thread (forced from prepare()). kSineTableSize+1 with a guard point equal to
        // table[0] so linear interpolation never indexes past the end.
        static const std::array<float, kSineTableSize + 1> table = [] {
            std::array<float, kSineTableSize + 1> t {};
            for (int i = 0; i <= kSineTableSize; ++i)
                t[static_cast<std::size_t> (i)] = static_cast<float> (
                    std::sin (2.0 * kPi * static_cast<Real> (i) / static_cast<Real> (kSineTableSize)));
            return t;
        }();
        return table.data();
    }

    void PunchTransient::prepare (Real sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        sine_ = bandlimitedSineTable(); // forces the one-time table build off the audio thread
        bp_.prepare (sampleRate_);
        ping_.prepare (sampleRate_);
        recomputeDecayCoef();
        recomputeClickCoef();
        reset();
    }

    void PunchTransient::reset() noexcept
    {
        active_ = false;
        env_ = 0.0;
        burstGain_ = 0.0;
        lpState_ = 0.0;
        impulseArmed_ = false;
        noiseState_ = kNoiseSeed;
        carrierPhase_ = 0.0;
        modPhase_ = 0.0;
        fmIndex_ = 0.0;
        ringPhase_ = 0.0;
        bp_.reset();
        ping_.reset();
    }

    void PunchTransient::setDecayMs (float ms) noexcept
    {
        decayMs_ = clamp (ms, kDecayMinMs, kDecayMaxMs); // 40 ms cap is locked (§3)
        recomputeDecayCoef();
    }

    void PunchTransient::setTuneSemitones (float semitones) noexcept
    {
        tuneSemitones_ = clamp (semitones, -24.0f, 24.0f);
        recomputeClickCoef();
    }

    void PunchTransient::trigger (float velocity) noexcept
    {
        if (amount_ <= 0.0f)
        {
            active_ = false; // off — the generator is not scheduled (zero DSP cost, §6)
            return;
        }

        const Real vel = static_cast<Real> (clamp (velocity, 0.0f, 1.0f));
        // Exponential velocity curve (γ≈1.5, §5), blended in by velSens.
        const Real velFactor = (1.0 - static_cast<Real> (velSens_))
                                 + static_cast<Real> (velSens_) * std::pow (vel, kVelGamma);
        burstGain_ = static_cast<Real> (amount_) * velFactor;

        env_ = 1.0;
        lpState_ = 0.0;
        impulseArmed_ = true;
        noiseState_ = kNoiseSeed; // deterministic per note-on

        // Resolve the concrete generator (the exciter fallback folds into Impulse here, so the
        // hot path pays no fallback branch; §4.3 stays deferred to M1-07c).
        switch (tone_)
        {
            case Tone::Noise:
                genKind_ = Gen::Noise;
                break;
            case Tone::FmClick:
                genKind_ = Gen::Fm;
                break;
            case Tone::RingMod:
                genKind_ = Gen::Ring;
                break;
            case Tone::Exciter:
                genKind_ = Gen::Exciter;
                break;
            case Tone::Impulse:
            default:
                genKind_ = Gen::Impulse;
                break;
        }

        const Real nyqCut = 0.45 * sampleRate_;

        // Configure the noise-burst bandpass at its (clamped) centre, then clear its state so
        // the burst starts clean.
        const Real bandHz = std::fmin (std::fmax (static_cast<Real> (noiseBandHz_), 20.0), nyqCut);
        bp_.setCutoff (static_cast<float> (bandHz), static_cast<float> (kNoiseQ));
        bp_.reset();

        // fm_click coeffs (§4.1): carrier tracks the voice fundamental shifted by tune; the
        // modulator sits at a fixed ratio; the index sweeps to the -60 dB floor over ~8 ms.
        const Real tuneRatio = std::pow (2.0, static_cast<Real> (tuneSemitones_) / 12.0);
        const Real carrierHz = std::fmin (std::fmax (static_cast<Real> (noteHz_) * tuneRatio, 20.0), nyqCut);
        carrierInc_ = carrierHz / sampleRate_;
        modInc_ = kFmRatio * carrierInc_;
        carrierPhase_ = 0.0;
        modPhase_ = 0.0;
        fmIndex_ = kFmPeakIndex;
        const Real idxSamples = kFmIndexDecayMs * 0.001 * sampleRate_;
        fmIndexDecay_ = std::exp (std::log (kEnvFloor) / idxSamples);

        // ringmod_zing coeff (§4.5): fixed high carrier (~1.2 kHz), tune-shiftable.
        const Real ringHz = std::fmin (std::fmax (kRingBaseHz * tuneRatio, 20.0), nyqCut);
        ringInc_ = ringHz / sampleRate_;
        ringPhase_ = 0.0;

        // exciter_ping (§4.3): configure the lightweight modal bank — inharmonic free-bar
        // partials above the carrier, reduced decay (a fraction of punch.decay so the ring stays
        // inside the cap), higher modes quieter. setMode resolves cos/sin/pow control-rate here;
        // reset() clears the resonator so the strike starts from silence.
        ping_.setModeCount (kPingModes);
        for (int i = 0; i < kPingModes; ++i)
        {
            const Real freq = carrierHz * kPingRatio[static_cast<std::size_t> (i)];
            const Real t60Ms = static_cast<Real> (decayMs_) * kPingDecayFrac[static_cast<std::size_t> (i)];
            ping_.setMode (i,
                static_cast<float> (freq),
                static_cast<float> (t60Ms),
                static_cast<float> (kPingGain[static_cast<std::size_t> (i)]));
        }
        ping_.reset();

        active_ = true;
    }

    void PunchTransient::recomputeDecayCoef() noexcept
    {
        // env(n) = decayCoef^n, chosen so env == kEnvFloor after `decay` ms → the burst is
        // fully decayed (and deactivates) within the (≤40 ms) decay time.
        const Real decaySamples = static_cast<Real> (decayMs_) * 0.001 * sampleRate_;
        decayCoef_ = std::exp (std::log (kEnvFloor) / decaySamples);
    }

    void PunchTransient::recomputeClickCoef() noexcept
    {
        // Impulse click brightness: a 1-pole LP at clickHz (base ~4 kHz, shifted by tune).
        // coef = 1 - e^(-2π·fc/fs) band-limits the click so it does not spray HF (§4.4/§6).
        const Real clickHz = static_cast<Real> (kClickBandHz)
                               * std::pow (2.0, static_cast<Real> (tuneSemitones_) / 12.0);
        const Real fc = std::fmin (std::fmax (clickHz, 200.0), 0.45 * sampleRate_);
        lpCoef_ = 1.0 - std::exp (-2.0 * 3.14159265358979323846 * fc / sampleRate_);
    }
}
