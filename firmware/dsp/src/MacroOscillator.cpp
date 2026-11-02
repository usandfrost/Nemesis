#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/MacroOscillator.h>

// The ONE sanctioned production include of the vendored tree (check-includes.sh R3,
// ADR-008): everything plaits/stmlib stays behind this TU.
#include "plaits/dsp/dsp.h"
#include "plaits/dsp/voice.h"
#include "stmlib/utils/buffer_allocator.h"
#include "stmlib/utils/random.h"

#include <cmath>

namespace nemesis::dsp
{
    namespace
    {
        static_assert (MacroOscillator::kBlockSizeSamples
                           == static_cast<int> (plaits::kBlockSize),
            "header block size must mirror the vendored plaits::kBlockSize");

        // Upstream carves every engine's scratch out of one pre-allocated block
        // (the ADR-008 §5 BufferAllocator pattern); 16 KiB matches the firmware.
        constexpr std::size_t kAllocatorBytes = 16384;

        // The upstream pitch law in double precision: normalized frequency
        // = (55 / 47872.34) * 0.25 * 2^((note - 9) / 12) (plaits engine.h
        // NoteToFrequency; 47872.34 is the module's corrected I2S clock).
        constexpr Wide kCorrectedSampleRate = 47872.34;
        constexpr Real kNormalizedC = (55.0 / kCorrectedSampleRate) * 0.25;

        // splitmix-style block-seed stream: a Weyl counter finalized by the murmur3
        // mixer, so consecutive block seeds are decorrelated (a plain LCG step would
        // make each block's draws a one-step shift of the previous block's — a tonal
        // 4 kHz artifact on noise engines).
        std::uint32_t mixSeed (std::uint32_t state) noexcept
        {
            std::uint32_t z = state;
            z ^= z >> 16;
            z *= 0x85EBCA6Bu;
            z ^= z >> 13;
            z *= 0xC2B2AE35u;
            z ^= z >> 16;
            return z;
        }
    }

    struct MacroOscillator::Impl
    {
        std::array<char, kAllocatorBytes> block {};
        plaits::Voice voice;
        std::array<plaits::Voice::Frame, plaits::kBlockSize> frames {};
    };

    MacroOscillator::MacroOscillator() = default;
    MacroOscillator::~MacroOscillator() = default;
    MacroOscillator::MacroOscillator (MacroOscillator&&) noexcept = default;
    MacroOscillator& MacroOscillator::operator= (MacroOscillator&&) noexcept = default;

    void MacroOscillator::prepare (Wide sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        if (impl_ == nullptr)
            impl_ = std::make_unique<Impl>();
        note_ = static_cast<float> (noteForHz (noteHz_, sampleRate_));
        reset();
    }

    void MacroOscillator::reset() noexcept
    {
        if (impl_ != nullptr)
        {
            // Re-Init over the SAME storage: the allocator carve is deterministic, so
            // this is a full bit-deterministic reset with zero heap allocation.
            stmlib::BufferAllocator allocator (impl_->block.data(), impl_->block.size());
            impl_->voice.Init (&allocator);
        }
        rngState_ = seed_;
        struck_ = false;
        strikePending_ = false;
        pos_ = kBlockSizeSamples;
        out_.fill (0.0f);
    }

    void MacroOscillator::setNoteHz (Wide hz) noexcept
    {
        noteHz_ = hz;
        note_ = static_cast<float> (noteForHz (hz, sampleRate_));
    }

    void MacroOscillator::renderBlock() noexcept
    {
        pos_ = 0;
        if (impl_ == nullptr)
        {
            out_.fill (0.0f);
            return;
        }

        // Per-instance RNG routing (header contract): advance our own counter stream
        // and pin stmlib's (thread_local, T-CORE-28) LCG to it for the duration of this block.
        rngState_ += 0x9E3779B9u;
        stmlib::Random::Seed (mixSeed (rngState_));

        // Free-running no-CV drone configuration (trigger/level unpatched): the
        // deterministic configuration every keystone and golden rests on.
        plaits::Patch patch {};
        // [T-CMP-03 leaf 2d] The cross-mod's block-rate pitch offset. Zero unless a Macro slot is
        // the CATHODE FM carrier, so every committed golden is untouched.
        patch.note = note_ + noteOffset_;
        patch.harmonics = harmonics_;
        patch.timbre = timbre_;
        patch.morph = morph_;
        patch.frequency_modulation_amount = 0.0f;
        patch.timbre_modulation_amount = 0.0f;
        patch.morph_modulation_amount = 0.0f;
        patch.engine = plaitsEngineIndex (engine_);
        patch.decay = 0.5f;
        patch.lpg_colour = 0.0f;

        plaits::Modulations modulations {};
        // [T-CGUI-122] Unpatched unless a percussive engine has been struck: then one block HIGH is the rising
        // edge, and the blocks after it LOW re-arm the next strike. All five are `already_enveloped`, so the
        // patched trigger never engages plaits' LPG.
        modulations.trigger_patched = struck_ && isPercussive (engine_);
        modulations.trigger = modulations.trigger_patched && strikePending_ ? 1.0f : 0.0f;
        strikePending_ = false;
        modulations.level_patched = false;

        impl_->voice.Render (patch, modulations, impl_->frames.data(), plaits::kBlockSize);

        for (std::size_t i = 0; i < static_cast<std::size_t> (kBlockSizeSamples); ++i)
            out_[i] = static_cast<float> (impl_->frames[i].out) * (1.0f / 32768.0f);
    }

    const char* MacroOscillator::engineLabel (Engine engine) noexcept
    {
        switch (engine)
        {
            case Engine::Analog:
                return "Analog";
            case Engine::Shaper:
                return "Shaper";
            case Engine::TwoOpFm:
                return "2-Op FM";
            case Engine::Grains:
                return "Grains";
            case Engine::Harmonics:
                return "Harmonics";
            case Engine::Tables:
                return "Tables";
            case Engine::Chords:
                return "Chords";
            case Engine::Vox:
                return "Vox";
            case Engine::Swarm:
                return "Swarm";
            case Engine::Noise:
                return "Noise";
            case Engine::Particles:
                return "Particles";
            case Engine::String:
                return "String";
            case Engine::Modal:
                return "Modal";
            case Engine::Kick:
                return "Kick";
            case Engine::Snare:
                return "Snare";
            case Engine::Hat:
                return "Hat";
        }
        return "?";
    }

    int MacroOscillator::plaitsEngineIndex (Engine engine) noexcept
    {
        // Registration order (vendored voice.cc): engine2 bank 0..7, classic green
        // 8..15, red 16..23 — the enum is declared in classic-bank order.
        return 8 + static_cast<int> (engine);
    }

    Wide MacroOscillator::noteForHz (Wide hz, Wide sampleRate) noexcept
    {
        const Real fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        const Real f = hz > 0.0 ? hz : 1.0;
        return 9.0 + 12.0 * std::log2 (f / (fs * kNormalizedC));
    }
} // namespace nemesis::dsp
