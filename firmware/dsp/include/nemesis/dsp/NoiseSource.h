#pragma once

#include <nemesis/dsp/Real.h>
#include <cmath>
#include <cstdint>

namespace nemesis::dsp
{
    // NoiseSource — the CATHODE §5 per-voice noise / texture generator. A seeded,
    // deterministic multi-type noise GENERATOR (processSample() takes no input) with a linear
    // output `level`. Five types, each a distinct spectral / statistical signature:
    //
    //   White    — seeded LCG uniform in [-1, 1); flat PSD.
    //   Pink     — white through Paul Kellet's public-domain refined 7-pole filter (~-3 dB/oct
    //              over ~9 octaves); flat power per octave.
    //   TapeHiss — white through a one-pole low-pass ("broadband-but-rolled" hiss); HF-rolled.
    //   Vinyl    — a low continuous hiss bed + sparse SEEDED crackle/pops (impulsive, decaying).
    //   Digital  — white QUANTISED to a coarse fixed step (harsh, few discrete values — the
    //              broken-converter family).
    //
    // The noise TYPE palette is self-contained here (recording_era's decade-fidelity crackle /
    // hiss models are a separate ERA FX). The metallic resonator half of §5 is MetallicResonator.
    // The §5 texture COMPOSITION (mixing noise + resonator pre-filter into the voice) and the §4
    // NoiseLevel mod destination are follow-on cards.
    //
    // RT-safety: prepare()/setSeed() resolve the control-rate coefficients (exp for the tape
    // low-pass, the crackle decay, the pop probability); processSample() is inline and pure
    // arithmetic — LCG integer math + filter mul/add + one std::floor for the quantiser (a single
    // arm64 frintm, the sanctioned category). No libm calls, no allocation, no locks. Fixed-size
    // state, no heap. Deterministic: same seed + same params -> bit-identical; reset() reseeds.
    // Computed in double, emitted as float. No JUCE. Not thread-safe vs prepare().
    class NoiseSource
    {
    public:
        enum class Type : std::uint8_t {
            White,
            Pink,
            TapeHiss,
            Vinyl,
            Digital
        };

        // Sets the sample rate (resolves control-rate coefficients) and reseeds/clears state.
        // Not RT-safe — call like prepareToPlay.
        void prepare (Real sampleRate) noexcept;

        // Reseeds the RNG to the current seed and clears all filter / crackle state, so a render
        // from here is reproducible. RT-safe.
        void reset() noexcept;

        // Latches the noise type. Control-rate (does not clear state — switching mid-stream is
        // allowed and continues the shared RNG stream).
        void setType (Type type) noexcept { type_ = type; }

        // Linear output gain (clamped >= 0; nominally [0, 1]). Applied as the final float multiply.
        void setLevel (float level) noexcept { level_ = level < 0.0f ? 0.0f : level; }

        // Base RNG seed — drives per-voice variation (§6). Reseeds and clears state.
        void setSeed (std::uint32_t seed) noexcept
        {
            seed_ = seed;
            reset();
        }

        [[nodiscard]] Type type() const noexcept { return type_; }
        [[nodiscard]] float level() const noexcept { return level_; }

        // Generates one sample. Hot path — pure arithmetic, no libm (bar std::floor) / alloc.
        // level is applied as (float)base * level_ (float x float) so a level of 0.5 yields
        // EXACTLY 0.5f x the level-1 output (bit-exact linear gain).
        float processSample() noexcept
        {
            Real base = 0.0;
            switch (type_)
            {
                case Type::White:
                    base = nextWhite();
                    break;
                case Type::Pink:
                    base = pink();
                    break;
                case Type::TapeHiss:
                    base = tape();
                    break;
                case Type::Vinyl:
                    base = vinyl();
                    break;
                case Type::Digital:
                    base = digital();
                    break;
            }
            return static_cast<float> (base) * level_;
        }

    private:
        // --- chosen depth / character constants (owner-tunable defaults, not spec constants) ---
        static constexpr Real kPinkScale = 0.11; // normalises the Kellet output toward white RMS
        static constexpr Real kTapeCutoffHz = 5000.0; // one-pole LP corner for the tape roll-off
        static constexpr Real kVinylHiss = 0.03; // continuous surface-hiss bed level
        static constexpr Real kVinylPopRate = 120.0; // crackle events per second
        static constexpr Real kVinylPopTauS = 0.0006; // pop decay time constant (~0.6 ms click)
        static constexpr Real kVinylPopMin = 0.4; // minimum pop amplitude (max = 1.0)
        static constexpr Real kDigitalLevels = 8.0; // quantiser steps per side (~4-bit grit)

        // --- 32-bit LCG (Numerical Recipes constants); top 24 bits -> uniform ---
        std::uint32_t nextU32() noexcept
        {
            rng_ = rng_ * 1664525u + 1013904223u;
            return rng_;
        }
        Real nextU01() noexcept { return static_cast<Real> (nextU32() >> 8) * (1.0 / 16777216.0); }
        Real nextWhite() noexcept { return static_cast<Real> (nextU32() >> 8) * (2.0 / 16777216.0) - 1.0; }

        Real pink() noexcept
        {
            const Real w = nextWhite();
            b0_ = 0.99886 * b0_ + w * 0.0555179;
            b1_ = 0.99332 * b1_ + w * 0.0750759;
            b2_ = 0.96900 * b2_ + w * 0.1538520;
            b3_ = 0.86650 * b3_ + w * 0.3104856;
            b4_ = 0.55000 * b4_ + w * 0.5329522;
            b5_ = -0.7616 * b5_ - w * 0.0168980;
            const Real out = b0_ + b1_ + b2_ + b3_ + b4_ + b5_ + b6_ + w * 0.5362;
            b6_ = w * 0.115926;
            return out * kPinkScale;
        }

        Real tape() noexcept
        {
            const Real w = nextWhite();
            lp_ = tapeCoef_ * lp_ + (1.0 - tapeCoef_) * w;
            return lp_;
        }

        Real vinyl() noexcept
        {
            const Real bed = kVinylHiss * nextWhite();
            if (nextU01() < popProb_)
            {
                const Real amp = kVinylPopMin + nextU01() * (1.0 - kVinylPopMin);
                pop_ = (nextU32() & 1u) != 0u ? amp : -amp;
            }
            const Real out = bed + pop_;
            pop_ *= popDecay_;
            return out;
        }

        Real digital() noexcept
        {
            const Real w = nextWhite();
            return std::floor (w * kDigitalLevels + 0.5) / kDigitalLevels;
        }

        Real sampleRate_ = 48000.0;
        Type type_ = Type::White;
        float level_ = 1.0f;
        std::uint32_t seed_ = 0x1u;
        std::uint32_t rng_ = 0x1u;

        // Control-rate coefficients (resolved in prepare()).
        Real tapeCoef_ = 0.0; // one-pole LP feedback coefficient
        Real popProb_ = 0.0; // per-sample crackle-trigger probability
        Real popDecay_ = 0.0; // per-sample pop-envelope decay

        // Filter / crackle running state.
        Real b0_ = 0.0, b1_ = 0.0, b2_ = 0.0, b3_ = 0.0, b4_ = 0.0, b5_ = 0.0, b6_ = 0.0;
        Real lp_ = 0.0;
        Real pop_ = 0.0;
    };
}
