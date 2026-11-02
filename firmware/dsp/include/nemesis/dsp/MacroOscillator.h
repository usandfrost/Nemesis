#pragma once

#include <nemesis/dsp/Real.h>
#include <array>
#include <cstdint>
#include <memory>

namespace nemesis::dsp
{
    // MacroOscillator — the first-party seam over the vendored Plaits macro-oscillator
    // (ADR-008, T-PLAITS-01b). "Macro" is the house module label; the 16 v1 engines are
    // the classic plaits banks (registration indices 8..23) under the signed-off house
    // labels (owner, 2026-07-16). This wrapper is the ONLY production L1 code that may
    // include a plaits/ or stmlib/ header (check-includes.sh R3) — and it confines them
    // to its .cpp (pimpl), so the vendored include root never leaks to consumers.
    //
    // Contract:
    //  - prepare(fs) performs ALL allocation (the pimpl + the upstream BufferAllocator
    //    carve that plaits::Voice::Init() slices its engine scratch from) — call it off
    //    the audio thread. processSample() is zero-alloc.
    //  - Block-quantised control: plaits renders 12-sample blocks (kBlockSizeSamples);
    //    processSample() reads from an internal block buffer, so setter changes take
    //    effect at the next block boundary (<= 12 samples, ~0.25 ms @ 48k). There is NO
    //    added signal latency — the first sample is rendered on the first call.
    //  - Host-rate pitch compensation (owner pick): setNoteHz maps the requested Hz to
    //    the plaits note against the HOST rate, inverting the upstream pitch law
    //    (normalized = a0 * 0.25 * 2^((note - 9) / 12), a0 = 55 / 47872.34 — the
    //    Eurorack module's corrected clock). Pitch is exact at any fs; the engines'
    //    internal envelope/filter time constants still assume 48 kHz, so timbre is
    //    marginally rate-dependent (captured by the per-SR goldens, T-PLAITS-01c).
    //  - Determinism vs the GLOBAL stmlib::Random (upstream: one static LCG shared by
    //    every instance): the wrapper reseeds the global before EVERY block from an
    //    instance-owned splitmix-style counter stream, so each instance's noise is
    //    fully self-determined — two same-seed instances are bit-identical even when
    //    rendered interleaved. The exact upstream sample stream is NOT preserved
    //    (noise statistics are unchanged); sound sign-off rides the 01c goldens.
    //    Since T-CORE-28 that LCG is `thread_local` (the vendored random.h patch), so
    //    concurrent renders on DIFFERENT threads no longer race; instances on one thread
    //    are still serialised by the per-block reseed. [T-CGUI-44] The engine gives every
    //    (voice, slot) its own seed — two instances left on the default share one stream.
    //  - Each plaits engine renders an out/aux pair; v1 exposes the main out only
    //    (owner pick — an aux accessor later is purely additive).
    //  - reset() re-runs the upstream Init over the SAME pre-allocated block (the
    //    carve is deterministic) and reseeds: render/reset/re-render is bit-identical.
    //    No heap allocation. Output is float in [-1, 1] (upstream 16-bit / 32768).
    class MacroOscillator
    {
    public:
        // The signed-off v1 roster, in plaits registration order (index 8 + ordinal).
        enum class Engine : std::uint8_t {
            Analog, // virtual_analog
            Shaper, // waveshaping
            TwoOpFm, // fm
            Grains, // grain
            Harmonics, // additive
            Tables, // wavetable
            Chords, // chord
            Vox, // speech
            Swarm, // swarm
            Noise, // noise
            Particles, // particle
            String, // string
            Modal, // modal
            Kick, // bass_drum
            Snare, // snare_drum
            Hat // hi_hat
        };

        static constexpr int kNumEngines = 16;
        static constexpr int kBlockSizeSamples = 12; // == plaits::kBlockSize (static_assert in .cpp)

        MacroOscillator();
        ~MacroOscillator();
        MacroOscillator (MacroOscillator&&) noexcept;
        MacroOscillator& operator= (MacroOscillator&&) noexcept;
        MacroOscillator (const MacroOscillator&) = delete;
        MacroOscillator& operator= (const MacroOscillator&) = delete;

        // Allocates (pimpl + upstream buffer carve) — off the audio thread. Re-preparing
        // is a full deterministic reset at the new rate.
        void prepare (Wide sampleRate);

        // Deterministic re-init on the already-prepared storage; no heap allocation.
        void reset() noexcept;

        // FIREFORK: true once prepare() has allocated the Plaits storage. An unprepared macro renders silence.
        [[nodiscard]] bool isPrepared() const noexcept { return impl_ != nullptr; }

        void setEngine (Engine engine) noexcept { engine_ = engine; }
        Engine engine() const noexcept { return engine_; }

        // Control-rate (log2 in the .cpp). Pitch lands on `hz` at the prepared host rate.
        void setNoteHz (Wide hz) noexcept;

        // Plaits macro controls, each clamped to [0, 1] upstream.
        void setHarmonics (float value) noexcept { harmonics_ = value; }
        void setTimbre (float value) noexcept { timbre_ = value; }
        void setMorph (float value) noexcept { morph_ = value; }

        // [T-CMP-03 leaf 2d] A PITCH offset in semitones, added to the resolved note when the next
        // block renders. Read the name literally: this is NOT phase modulation and the record must
        // never call it FM.
        //
        // WHY IT IS THE MOST THIS ENGINE CAN DO. There is no phase here to offset. This wrapper
        // exposes engine/pitch/harmonics/timbre/morph and nothing else, and underneath it most of
        // the 16 plaits engines have no phasor at all — Noise, Particles, Modal, String, Speech,
        // Chords. Reaching for one would mean entering the vendored tree ADR-008 confines to this
        // class's .cpp. So the CATHODE cross-mod's Macro slot drives pitch instead, and the honest
        // consequence is stated wherever the row is described: control is block-quantised to
        // `kBlockSizeSamples`, i.e. ~4 kHz at 48 k, so a modulator above roughly 2 kHz aliases in
        // the control path. Owner-ruled 2026-08-20 with that limitation named.
        //
        // SEMITONES, not Hz, and that is what keeps it off the hot path: `setNoteHz` resolves
        // through a `std::log2`, while plaits already speaks in notes — so an offset is one
        // addition at render time and adds no libm call to the audio thread.
        void setNoteOffsetSemitones (float semitones) noexcept { noteOffset_ = semitones; }

        [[nodiscard]] float noteOffsetSemitones() const noexcept { return noteOffset_; }

        // [T-CGUI-122] A NOTE-ON for the percussive engines (Kick, Snare, Hat, String, Modal): patches the trigger and
        // fires one rising edge, so the engine strikes and decays instead of droning (plaits' unpatched SUSTAIN mode).
        // The trigger stays patched until `reset()`, which returns to the drone every golden rests on. A no-op on the
        // other engines (their trigger would engage plaits' LPG). Audio-thread safe; heard from the next block, plus
        // plaits' own 5-block trigger delay.
        void strike() noexcept
        {
            if (!isPercussive (engine_))
                return;
            struck_ = true;
            strikePending_ = true;
        }
        // [b39 A] PATCH the trigger without firing it — the state a strike leaves behind, reached with no strike: a
        // percussive engine then RESTS SILENT until the next `strike()` instead of droning in plaits' unpatched SUSTAIN.
        // For a host whose oscillator runs while it is not sounding (CathodeOscCore: `processPoly` ticks idle voices),
        // so the first strike lands on a silent string rather than on one bowed behind a closed AMP. Undone by
        // `reset()`, which keeps the drone this class's own goldens rest on. Audio-thread safe.
        void armTrigger() noexcept { struck_ = true; }
        [[nodiscard]] static constexpr bool isPercussive (Engine e) noexcept
        {
            return e == Engine::String || e == Engine::Modal || e == Engine::Kick || e == Engine::Snare || e == Engine::Hat;
        }

        // Per-instance RNG stream seed (§6 per-voice variation); takes effect from the
        // next block. prepare()/reset() rewind the stream to this seed.
        void setSeed (std::uint32_t seed) noexcept
        {
            seed_ = seed;
            rngState_ = seed;
        }

        // Zero-alloc. Renders a 12-sample block on demand, then reads it out per sample.
        // Before prepare() it returns exact silence.
        float processSample() noexcept
        {
            if (pos_ >= kBlockSizeSamples)
                renderBlock();
            return out_[static_cast<std::size_t> (pos_++)];
        }

        // The signed-off house label for an engine (user-facing string).
        static const char* engineLabel (Engine engine) noexcept;

        // The vendored-tree registration index this enum value drives (8..23) — exposed
        // so tests/goldens pin the enum<->sound mapping to the bit.
        static int plaitsEngineIndex (Engine engine) noexcept;

        // The host-rate pitch-compensation law (exposed static for bit-exact keystones):
        // the plaits note that renders `hz` when the engine runs at `sampleRate`.
        static Wide noteForHz (Wide hz, Wide sampleRate) noexcept;

    private:
        struct Impl;

        void renderBlock() noexcept; // refills out_, resets pos_ (silence before prepare)

        std::unique_ptr<Impl> impl_;
        std::array<float, kBlockSizeSamples> out_ {};
        int pos_ = kBlockSizeSamples; // force a render on the first processSample()

        Engine engine_ = Engine::Analog;
        Wide sampleRate_ = 48000.0;
        Wide noteHz_ = 220.0;
        float note_ = 0.0f; // compensated plaits note, resolved control-rate
        float noteOffset_ = 0.0f; // [T-CMP-03 leaf 2d] block-rate pitch offset, semitones
        float harmonics_ = 0.5f;
        float timbre_ = 0.5f;
        float morph_ = 0.5f;
        bool struck_ = false; // [T-CGUI-122] the trigger is patched (a strike has happened since reset)
        bool strikePending_ = false; // …and the next block carries the trigger HIGH (the rising edge)
        std::uint32_t seed_ = 1;
        std::uint32_t rngState_ = 1;
    };
} // namespace nemesis::dsp
