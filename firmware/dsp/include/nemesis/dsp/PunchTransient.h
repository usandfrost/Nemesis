#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ModalResonator.h>
#include <nemesis/dsp/Svf.h>

#include <array>
#include <cmath>
#include <cstdint>

namespace nemesis::dsp
{
    // Punch — the shared §0 per-voice onset-transient primitive (punch_transient.md). An
    // optional short percussive burst summed into a voice AT NOTE-ON to add attack impact
    // without colouring the sustained timbre (the "DX7 EP click under an analog pad" trick,
    // generalised across the melodic engines). Two locked/resolved traits define it and are
    // the keystones the acceptance tests prove:
    //
    //   1. Transient, NOT a layer. The decay envelope is exponential and HARD-capped at
    //      40 ms (§2/§3, locked, not user-extendable). Beyond it the burst would read as a
    //      second voice; the cap is what keeps it complimentary.
    //   2. Velocity sensitivity is an EXPONENTIAL curve (γ≈1.5, §5 resolved) — it scales
    //      punch.amount to match percussive strike dynamics.
    //
    // And the headline promise (§1/§6): OFF by default (amount = 0) at literally ZERO DSP
    // cost — trigger() with amount 0 never schedules the generator, so processSample() is a
    // single branch returning 0. It frees itself when the envelope decays.
    //
    // Signal-flow model (§2): punch is a fully INDEPENDENT path summed at the voice's mix
    // point; it never reads the main oscillator/filter state (except ringmod_zing, deferred).
    // processSample() therefore RETURNS the punch contribution for the caller to sum — where
    // it sums (punch.mix pre/post-filter, §2) is the voice's concern, not the primitive's.
    //
    // Flavours (§4, enum order fixed for the future preset contract). ALL FIVE are now
    // implemented: Impulse (§4.4, bandlimited click through a 1-pole LP; the spec's designated
    // universal fallback), Noise (§4.2, white burst through a resonant BP), FmClick (§4.1, a
    // 2-op FM burst whose modulator index sweeps fast high→~0 via its own sub-envelope; carrier
    // = voice fundamental ±tune), RingMod (§4.5, the live voice briefly ring-modulated by a
    // fixed high carrier, crossfading back to unity as the decay elapses — the ONE voice-derived
    // flavour) and Exciter (§4.3, a fast impulse struck into a lightweight ModalResonator — a
    // "second lightweight exciter instance" with reduced decay; band-limited by construction).
    // FmClick and RingMod share a libm-free band-limited sine (a table built once off the audio
    // thread, read with linear interpolation).
    //
    // RT-safety: prepare() clears state and caches the shared sine table; the note-on path
    // (trigger/setters) resolves every transcendental (decay/LP/BP coeffs + FM/ring frequencies
    // via exp/tan/pow) at CONTROL rate; processSample() is inline and pure arithmetic — no libm
    // CALLS, no allocation, no locks (the phase wrap uses std::floor, a single arm64 frintm, the
    // same sanctioned category as BBD's fsqrt). Deterministic: the seeded noise LCG reseeds in
    // reset()/trigger() and FM/ring have no randomness, so a given note-on (and voice input, for
    // ringmod) renders bit-identically. Computed in double, emitted as float. No JUCE. Not
    // thread-safe vs prepare().
    class PunchTransient
    {
    public:
        // Flavour algorithm (`punch.tone`, §4). Enum index order is the spec's and is stable.
        enum class Tone : std::uint8_t {
            FmClick = 0, // §4.1 — implemented (2-op FM click)
            Noise = 1, // §4.2 — implemented
            Exciter = 2, // §4.3 — implemented (fast impulse into a lightweight modal resonator)
            Impulse = 3, // §4.4 — implemented (the universal fallback)
            RingMod = 4 // §4.5 — implemented (voice-derived ring-mod)
        };

        // Sets the sample rate (used only to resolve control-rate coefficients), clears all
        // state and reseeds the noise. Not RT-safe — call like prepareToPlay.
        void prepare (Real sampleRate) noexcept;

        // Clears the envelope/filter state, disarms any active burst, and reseeds the noise
        // generator (so the next trigger renders identically to the first). RT-safe.
        void reset() noexcept;

        // Linear gain into the voice sum, 0..1 (`punch.amount`). 0 = fully off, zero DSP cost.
        // Control-rate.
        void setAmount (float amount) noexcept { amount_ = clamp (amount, 0.0f, 1.0f); }

        // Exponential decay time in ms (`punch.decay`), HARD-clamped to [3, 40] — the 40 ms cap
        // is locked (§3). Control-rate (recomputes the envelope coefficient via exp).
        void setDecayMs (float ms) noexcept;

        // Flavour select (`punch.tone`). Any tone is accepted; unimplemented flavours route to
        // the Impulse generator (§4.4 universal fallback). Control-rate.
        void setTone (Tone tone) noexcept { tone_ = tone; }

        // Pitched-flavour tuning in semitones, clamped [-24, 24] (`punch.tune`). For the Impulse
        // flavour it shifts the click's LP brightness. Control-rate (recomputes the LP coeff).
        void setTuneSemitones (float semitones) noexcept;

        // Noise-burst bandpass centre in Hz (`punch.tune` reinterpreted for `noise_burst`, §4.2),
        // clamped [20, 0.45·fs]. Applied when the burst is (re)triggered. Control-rate.
        void setNoiseBandHz (float hz) noexcept { noiseBandHz_ = hz; }

        // Voice fundamental in Hz for the PITCHED flavour (fm_click, §4.1) — its carrier tracks
        // this, shifted by `punch.tune` semitones. The voice passes its note frequency here; a
        // sane default keeps the primitive usable standalone. Clamped [20, 8000]. Applied on the
        // next trigger(). Control-rate. (Impulse/Noise/RingMod ignore it.)
        void setNoteHz (float hz) noexcept { noteHz_ = clamp (hz, 20.0f, 8000.0f); }

        // Velocity sensitivity 0..1 (`punch.velSens`): blends the γ≈1.5 velocity curve into the
        // amount. 0 = velocity-independent, 1 = full vel^γ scaling. Control-rate.
        void setVelSens (float sens) noexcept { velSens_ = clamp (sens, 0.0f, 1.0f); }

        // Fires the transient at note-on with velocity in [0, 1]. Latches the per-burst gain
        // (amount · velocity^γ blend), resets the envelope, and configures the flavour generator.
        // Control-rate (uses libm). With amount == 0 it does NOT schedule the generator.
        void trigger (float velocity) noexcept;

        // True while a burst is decaying (for tests / voice bookkeeping).
        [[nodiscard]] bool active() const noexcept { return active_; }

        // [T-CORE-30 leaf D] Parameter read-back, the `decayMs()` idiom below extended to the rest
        // of the §3 set — each returns the CLAMPED value the primitive actually holds, so a caller
        // that pins one is pinning what the DSP will use rather than what it was handed. They exist
        // because the six `punch.*` registry rows cross two layer boundaries to get here, and a
        // wrong CONVERSION on the way (a percent that stayed 0–100, a semitone/Hz pair swapped) is
        // invisible to any render assertion that only asks whether the burst got louder.
        [[nodiscard]] float amount() const noexcept { return amount_; }
        [[nodiscard]] Tone tone() const noexcept { return tone_; }
        [[nodiscard]] float tuneSemitones() const noexcept { return tuneSemitones_; }
        [[nodiscard]] float noiseBandHz() const noexcept { return noiseBandHz_; }
        [[nodiscard]] float noteHz() const noexcept { return noteHz_; }
        [[nodiscard]] float velSens() const noexcept { return velSens_; }

        // The effective (clamped-to-cap) decay time in ms — exposes the 40 ms cap for tests.
        [[nodiscard]] float decayMs() const noexcept { return decayMs_; }

        // The unpitched flavours' CLICK BAND centre in Hz (§4.2's "fixed click band (~2–6 kHz)",
        // which §4.4's Impulse LP shares). Public because the CALLER owns the one place §3's
        // `punch.tune` row becomes a noise-band frequency: the row is ±24 semitones, so taking §4.2's
        // "Hz here, not semitones" literally would ask a ±24 range to name a click band. The
        // resolution is the offset applied to THIS centre — the same reading `setTuneSemitones` gives
        // tune for the Impulse flavour — and it is stated here so the mapping has one home rather
        // than a magic 4000 in the app layer. [T-CORE-30 leaf D]
        static constexpr float kClickBandHz = 4000.0f;

        // Processes one sample, returning the punch contribution to SUM into the voice at its
        // mix point. `voiceSample` is read only by the ringmod_zing flavour (§4.5); the other
        // flavours self-generate and ignore it. Hot path: pure arithmetic, no libm calls / alloc
        // / locks (std::floor in sine() is a single frintm instruction, not a libm call).
        float processSample (float voiceSample) noexcept
        {
            if (!active_)
                return 0.0f; // off / decayed — the generator is not scheduled

            Real g = 0.0;
            switch (genKind_)
            {
                case Gen::Noise:
                    // white noise → resonant BP (band-limited burst, §4.2)
                    g = static_cast<Real> (bp_.processSample (static_cast<float> (nextNoise())).bp);
                    break;

                case Gen::Fm:
                {
                    // 2-op phase-modulation FM (§4.1). The modulator index sweeps down via its
                    // own fast sub-envelope, so the onset is bright and collapses to the pure
                    // carrier. Band-limited: the source is a clean table sine.
                    const Real m = sine (modPhase_);
                    g = sine (carrierPhase_ + fmIndex_ * m);
                    carrierPhase_ = wrap01 (carrierPhase_ + carrierInc_);
                    modPhase_ = wrap01 (modPhase_ + modInc_);
                    fmIndex_ *= fmIndexDecay_;
                    break;
                }

                case Gen::Ring:
                {
                    // Ring-mod the live voice by a fixed high carrier (§4.5). The additive
                    // contribution voice·(carrier-1) turns the summed voice into voice·carrier
                    // at onset and (as env→0) crossfades it back to unity.
                    const Real c = sine (ringPhase_);
                    g = static_cast<Real> (voiceSample) * (c - 1.0);
                    ringPhase_ = wrap01 (ringPhase_ + ringInc_);
                    break;
                }

                case Gen::Exciter:
                {
                    // exciter_ping (§4.3): a single fast unit impulse struck into the lightweight
                    // modal resonator. The resonator's output is pure modal sinusoids, so it is
                    // band-limited BY CONSTRUCTION — the raw impulse never appears at the output.
                    const Real raw = impulseArmed_ ? 1.0 : 0.0;
                    impulseArmed_ = false;
                    g = static_cast<Real> (ping_.processSample (static_cast<float> (raw)));
                    break;
                }

                case Gen::Impulse:
                default:
                {
                    // Impulse (§4.4): a single unit impulse smoothed by a 1-pole LP → a
                    // bandlimited click (no HF splatter). The universal fallback.
                    const Real raw = impulseArmed_ ? 1.0 : 0.0;
                    impulseArmed_ = false;
                    lpState_ += lpCoef_ * (raw - lpState_);
                    g = lpState_;
                    break;
                }
            }

            const Real out = env_ * burstGain_ * g;

            env_ *= decayCoef_;
            if (env_ < kEnvFloor)
                active_ = false; // fully decayed within `decay` ms — frees itself

            return static_cast<float> (out);
        }

    private:
        // Envelope target at `decay` ms and the deactivation threshold (-60 dB): the burst is
        // "over" once the envelope falls this far, which bounds its length to `decay` (≤ 40 ms).
        static constexpr Real kEnvFloor = 1.0e-3;
        static constexpr float kDecayMinMs = 3.0f;
        static constexpr float kDecayMaxMs = 40.0f; // LOCKED cap (§3)
        // (the click band centre is `kClickBandHz`, public above — §4.2/§4.4)
        static constexpr Real kNoiseQ = 4.0; // resonant BP for the noise burst (§4.2)
        static constexpr Real kVelGamma = 1.5; // exponential velocity curve (§5)
        static constexpr std::uint32_t kNoiseSeed = 0x50'75'6E'63u; // 'Punc'

        // fm_click (§4.1): 2-op FM. Fixed modulator:carrier ratio + a peak index (in phase
        // turns; 0.5 turns ≈ π rad) that decays over a fast sub-envelope so the click starts
        // bright and collapses to the pure carrier.
        static constexpr Real kFmRatio = 2.0;
        static constexpr Real kFmPeakIndex = 0.5;
        static constexpr Real kFmIndexDecayMs = 8.0; // index sweep 5–15 ms (§4.1)
        // ringmod_zing (§4.5): fixed high carrier (~800 Hz–2 kHz), tune-shiftable.
        static constexpr Real kRingBaseHz = 1200.0;
        // exciter_ping (§4.3): a fast unit impulse into a lightweight modal resonator — a
        // "second lightweight exciter instance" with reduced decay (§5). A small INHARMONIC
        // metallic bank at the classic free-bar ratios above the voice fundamental (carrier =
        // noteHz ± tune); higher partials are quieter and decay faster (physical). The resonator
        // output is pure modal sinusoids → band-limited by construction. Each mode's T60 is a
        // fraction of punch.decay so the ring always sits inside the (≤40 ms) envelope cap.
        static constexpr int kPingModes = 3;
        static constexpr std::array<Real, kPingModes> kPingRatio { 1.0, 2.76, 5.40 };
        static constexpr std::array<Real, kPingModes> kPingGain { 1.0, 0.55, 0.30 };
        static constexpr std::array<Real, kPingModes> kPingDecayFrac { 0.8, 0.5, 0.3 };
        // Shared band-limited sine table (a pure sinusoid → single partial). 2048 pts + 1 guard;
        // linear-interp error ≈ 1.2e-6 (~-118 dB), well below the 1e-6 golden tolerance's audio
        // relevance. Built once off the audio thread; read on the hot path via the cached pointer.
        static constexpr int kSineTableSize = 2048;

        // The concrete per-sample generator, resolved from `tone_` at trigger() (so the exciter
        // fallback costs no hot-path branch). Distinct from the public Tone enum.
        enum class Gen : std::uint8_t { Impulse,
            Noise,
            Fm,
            Ring,
            Exciter };

        static float clamp (float v, float lo, float hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        // Wraps a phase in turns into [0, 1). std::floor lowers to a single arm64 frintm
        // instruction — not a libm call (same sanctioned category as BBD's fsqrt).
        static Real wrap01 (Real p) noexcept { return p - std::floor (p); }

        // Deterministic LCG noise in [-1, 1). Reseeded in reset()/trigger() → bit-identical.
        Real nextNoise() noexcept
        {
            noiseState_ = noiseState_ * 1664525u + 1013904223u;
            return static_cast<Real> (static_cast<std::int32_t> (noiseState_)) * (1.0 / 2147483648.0);
        }

        // Band-limited sine of a phase in turns, via linear interpolation of the shared table
        // (cached in sine_ during prepare()). Pure arithmetic + one frintm — no libm call.
        Real sine (Real phaseTurns) const noexcept
        {
            const Real p = wrap01 (phaseTurns); // → [0, 1)
            const Real x = p * static_cast<Real> (kSineTableSize);
            const int i = static_cast<int> (x);
            const Real frac = x - static_cast<Real> (i);
            const Real a = static_cast<Real> (sine_[i]);
            const Real b = static_cast<Real> (sine_[i + 1]); // guard point at [kSineTableSize]
            return a + frac * (b - a);
        }

        // Builds (once, off the audio thread) and returns the shared kSineTableSize+1 sine table.
        static const float* bandlimitedSineTable() noexcept;

        void recomputeDecayCoef() noexcept; // decayMs_ + fs → decayCoef_ (exp)
        void recomputeClickCoef() noexcept; // tune_ + fs → lpCoef_ (Impulse LP, exp)

        Real sampleRate_ = 48000.0;

        // Parameters (control-rate).
        float amount_ = 0.0f; // off by default
        float decayMs_ = 12.0f;
        float tuneSemitones_ = 0.0f;
        float noiseBandHz_ = 3200.0f;
        float noteHz_ = 220.0f; // voice fundamental for fm_click (pitched flavour)
        float velSens_ = 0.6f;
        Tone tone_ = Tone::Impulse;

        // Control-rate coefficients.
        Real decayCoef_ = 0.0; // per-sample envelope multiplier
        Real lpCoef_ = 0.4; // Impulse 1-pole LP coefficient

        // Per-burst latched / per-sample state.
        Gen genKind_ = Gen::Impulse; // resolved generator (fallbacks folded in at trigger)
        bool active_ = false;
        Real env_ = 0.0; // current envelope value (1 → kEnvFloor)
        Real burstGain_ = 0.0; // amount · velocity^γ blend, latched at trigger
        Real lpState_ = 0.0; // Impulse LP state
        bool impulseArmed_ = false; // fires the single unit impulse
        std::uint32_t noiseState_ = kNoiseSeed;

        // fm_click state (§4.1): phase accumulators (turns) + a decaying modulation index.
        Real carrierInc_ = 0.0, modInc_ = 0.0; // per-sample phase increments (turns)
        Real carrierPhase_ = 0.0, modPhase_ = 0.0;
        Real fmIndex_ = 0.0, fmIndexDecay_ = 0.0; // index sub-envelope (latched at trigger)

        // ringmod_zing state (§4.5): carrier phase accumulator (turns) + increment.
        Real ringInc_ = 0.0, ringPhase_ = 0.0;

        const float* sine_ = nullptr; // shared band-limited sine table (cached in prepare())

        TptSvf bp_; // resonant bandpass for the Noise flavour (reused §0 primitive)
        ModalResonator ping_; // struck modal bank for the Exciter flavour (reused §0 primitive)
    };
}
