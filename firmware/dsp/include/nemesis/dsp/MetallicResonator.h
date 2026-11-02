#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ModalResonator.h>

#include <array>
#include <cmath>

namespace nemesis::dsp
{
    // MetallicResonator — the CATHODE §5 "small metallic resonator bank (tuned band-passes
    // excited by the osc/noise) for clangs and atmospheres". A PURE COMPOSITION of the signed-off
    // ModalResonator: a fixed INHARMONIC metallic mode set (bell/idiophone ratios) scaled by
    // `tune` (base Hz -> mode i at tune * ratio[i]) with an `amount` wet mix, excited by an input
    // signal. amount = 0 is an exact bypass (silent).
    //
    // Because the DSP is entirely ModalResonator (which carries its own golden), the correctness
    // anchor is FAITHFULNESS — MetallicResonator{tune, amount=1} == a ModalResonator configured
    // with modeFreq()/modeDecayMs()/modeGain() (the M1-08b composition rule). The inharmonic ratio
    // set makes the bank read METALLIC (peaks are not integer multiples of the tune).
    //
    // The three §5 EXTENDED NOISE shape controls (CATHODE §5 / Param Ref §2.7, M1-18k) reshape the
    // built tables on top of `tune`: `decay` (global ring-time scale), `struct` (partial-spacing
    // stretch about the bell set) and `color` (spectral tilt of the mode gains). Each is NEUTRAL at
    // 1.0 = the built table bit-for-bit (pow-endpoint guards, the GRIT/MOTION exact-anchor rule), so
    // an unshaped resonator is identical to the M1-18a build. `mix` (the osc<->noise excitation
    // balance) is NOT here — it is a CathodeVoiceStage concern (a later slice).
    //
    // The ratio / decay / gain tables are owner-tunable defaults, not spec constants. Dense clangs,
    // material recipes, per-mode pan and resonator coupling are DROSS-engine concerns (M11) built on
    // ModalResonator, NOT here. RT-safety inherits ModalResonator's: setTune()/setDecayScale()/
    // setStructure()/setColor() resolve the mode coefficients control-rate; processSample() is pure
    // arithmetic, no libm / alloc / locks. Deterministic (no RNG). No JUCE.
    class MetallicResonator
    {
    public:
        static constexpr int kNumModes = 6;

        // Inharmonic bell/idiophone partial ratios (mode 0 = the fundamental at 1.0) -> metallic.
        static float modeFreq (float baseHz, int i) noexcept
        {
            return static_cast<float> (static_cast<Real> (baseHz) * kRatios[static_cast<std::size_t> (i)]);
        }
        static float modeDecayMs (int i) noexcept { return static_cast<float> (kDecaysMs[static_cast<std::size_t> (i)]); }
        static float modeGain (int i) noexcept { return static_cast<float> (kGains[static_cast<std::size_t> (i)]); }

        // ---- §5 extended NOISE shape controls (CATHODE §5 / Param Ref §2.7; M1-18k) ----
        // The shape math is exposed as pure statics: each reshapes one built table AROUND a neutral
        // value of 1.0 that returns the built table BIT-EXACTLY (pow-endpoint guards, the GRIT/MOTION
        // exact-anchor discipline) so a neutral resonator == the M1-18a build (golden untouched).
        //
        // struct — partial-spacing stretch: mode i sits at pow(builtRatio[i], structure). structure 1
        // = the built inharmonic bell set; > 1 stretches the partials apart (more clangorous), < 1
        // compresses them toward the fundamental (more tonal). Mode 0 (ratio 1.0) is FIXED (pow(1,e)==1).
        static Real structuredRatio (int i, float structure) noexcept
        {
            const Real b = kRatios[static_cast<std::size_t> (i)];
            return structure == 1.0f ? b : std::pow (b, static_cast<Real> (structure));
        }
        // decay — global scale on the modal ring times: builtDecayMs * decayScale (short percussive
        // clang <-> long sustained atmosphere). decayScale 1 = the built decay set (* 1.0f is exact).
        static float modeDecayScaled (int i, float decayScale) noexcept
        {
            return static_cast<float> (kDecaysMs[static_cast<std::size_t> (i)]) * decayScale;
        }
        // color — spectral tilt of the mode gains: builtGain[i] * pow(color, i/(N-1)) so the TOP mode is
        // scaled by exactly `color` and the tilt is spread evenly across the bank. color 1 = the built
        // gain set (skips pow); > 1 lifts HF (brighter), < 1 cuts it (darker). Mode 0 is FIXED (pow(c,0)==1).
        static float modeGainColored (int i, float color) noexcept
        {
            const Real g = kGains[static_cast<std::size_t> (i)];
            if (color == 1.0f || kNumModes <= 1)
                return static_cast<float> (g);
            const Real e = static_cast<Real> (i) / static_cast<Real> (kNumModes - 1);
            return static_cast<float> (g * std::pow (static_cast<Real> (color), e));
        }

        // Sets the sample rate, (re)configures the metallic modes at the current tune, clears state.
        // Not RT-safe — call like prepareToPlay.
        void prepare (Real sampleRate) noexcept
        {
            modal_.prepare (sampleRate);
            configureModes();
        }

        // Clears the ring state (falls silent) without changing tune / amount. RT-safe.
        void reset() noexcept { modal_.reset(); }

        // Base frequency in Hz; mode i is placed at tune * ratio[i]. Control-rate (recomputes the
        // ModalResonator coefficients). Does not clear state -> can be retuned while ringing.
        void setTune (float baseHz) noexcept
        {
            tune_ = baseHz;
            configureModes();
        }

        // Wet mix / output level of the resonator [0, 1] (clamped >= 0). amount = 0 -> exact bypass.
        void setAmount (float amount) noexcept { amount_ = amount < 0.0f ? 0.0f : amount; }

        // ---- §5 extended NOISE shape controls (M1-18k) ----
        // Each reconfigures the modes at the current tune (control-rate; recomputes the ModalResonator
        // coefficients). Like setTune, they do NOT clear state -> can be reshaped while ringing. The
        // neutral value is 1.0 for all three (== the built table, bit-exact). Clamped defensively; the
        // musical 0-127 -> unit mapping is the registry's concern (Param Ref §2.7), not the DSP's.

        // Global ring-time scale (> 0). 1 = the built decay set; higher = longer ring.
        void setDecayScale (float scale) noexcept
        {
            decayScale_ = scale < kMinDecayScale ? kMinDecayScale : scale;
            configureModes();
        }
        // Partial-spacing stretch exponent (>= 0). 1 = the built inharmonic set; > 1 stretches, < 1 compresses.
        void setStructure (float structure) noexcept
        {
            structure_ = structure < kMinStructure ? kMinStructure : (structure > kMaxStructure ? kMaxStructure : structure);
            configureModes();
        }
        // Spectral tilt of the mode gains (> 0). 1 = the built gain set; > 1 brighter (HF lifted), < 1 darker.
        void setColor (float color) noexcept
        {
            color_ = color < kMinColor ? kMinColor : (color > kMaxColor ? kMaxColor : color);
            configureModes();
        }

        [[nodiscard]] float tune() const noexcept { return tune_; }
        [[nodiscard]] float amount() const noexcept { return amount_; }
        [[nodiscard]] float decayScale() const noexcept { return decayScale_; }
        [[nodiscard]] float structure() const noexcept { return structure_; }
        [[nodiscard]] float color() const noexcept { return color_; }

        // Processes one sample: excites the bank with `excitation` and returns amount x the ring.
        // amount = 0 early-returns exact 0 (no ring cost). Hot path — pure arithmetic.
        float processSample (float excitation) noexcept
        {
            if (amount_ == 0.0f)
                return 0.0f;
            return amount_ * modal_.processSample (excitation);
        }

    private:
        static constexpr std::array<Real, kNumModes> kRatios { 1.0, 2.76, 5.40, 8.93, 11.34, 13.87 };
        static constexpr std::array<Real, kNumModes> kDecaysMs { 800.0, 600.0, 450.0, 350.0, 250.0, 180.0 };
        static constexpr std::array<Real, kNumModes> kGains { 1.0, 0.6, 0.4, 0.28, 0.2, 0.15 };

        // Defensive shape clamps (the musical range lives in the registry curve, Param Ref §2.7).
        static constexpr float kMinDecayScale = 0.001f;
        static constexpr float kMinStructure = 0.0f;
        static constexpr float kMaxStructure = 8.0f;
        static constexpr float kMinColor = 0.001f;
        static constexpr float kMaxColor = 8.0f;

        // Applies the three shape controls on top of the built ratio/decay/gain tables. At the neutral
        // defaults (all 1.0) every mode reproduces the built table bit-for-bit -> identical to the
        // M1-18a configuration (the committed golden + faithfulness keystone are untouched).
        void configureModes() noexcept
        {
            modal_.setModeCount (kNumModes);
            for (int i = 0; i < kNumModes; ++i)
            {
                const float freq = static_cast<float> (static_cast<Real> (tune_) * structuredRatio (i, structure_));
                modal_.setMode (i, freq, modeDecayScaled (i, decayScale_), modeGainColored (i, color_));
            }
        }

        ModalResonator modal_;
        float tune_ = 440.0f;
        float amount_ = 1.0f;
        float decayScale_ = 1.0f;
        float structure_ = 1.0f;
        float color_ = 1.0f;
    };
}
