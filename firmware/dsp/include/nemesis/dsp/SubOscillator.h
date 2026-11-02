#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/WavetableOscillator.h>

#include <cstdint>

namespace nemesis::dsp
{
    // SubOscillator — CATHODE's sub-oscillator (`synth.sub.{wave,level}`, T-SPEC-17's reservation,
    // built at T-CMP-02). −1 octave below the played note ([T-CGUI-57] now `synth.sub.oct`, −2…0,
    // default −1), three waveforms, and a level
    // whose 0 is a bit-exact bypass. The Juno/BS2 low-end anchor "without spending osc slot 2" —
    // which is the whole argument for it being a fourth generator rather than a third slot.
    //
    // IT IS A WAVETABLE OSCILLATOR, not a hand-rolled square (T-CMP-02 plan gate R1). A naive
    // −1-oct square is precisely the content the M1-04 mip pyramid exists for: at the top of the
    // keyboard a 50 % square's odd harmonics run past Nyquist and fold, and `WavetableOscillator`
    // already solves that by storing per-octave band-limited mips and biasing selection to the safe
    // side (`kMipGuard`). So the three waves are three FRAMES of one `Wavetable`, generated from
    // the shipped `WavetableKernels` laws, and the anti-aliasing is the primitive's, already
    // golden-tested, rather than a second implementation of it here.
    //
    // THE TABLE IS SHARED AND NON-OWNING, exactly like a slot's bank (`CathodeOscCore::setWavetable`).
    // One three-frame pyramid is ~540 KB; owning one per voice would be 4 MB of identical content
    // for an eight-voice pool. `buildTable()` is the build-time half (allocates, runs FFTs, calls
    // libm — off the audio thread, like `CathodeMachine::buildBank`), `setWavetable()` the RT half.
    // An UNBOUND sub is silent, the same contract an unbound WT slot has.
    //
    // WAVE SELECTION IS A POSITION, AND IT LANDS EXACTLY. `WavetableOscillator::resolvePosition`
    // computes `fp = position · (numFrames − 1)`; with three frames that is `position · 2`, so
    // 0.0 / 0.5 / 1.0 give fp = 0 / 1 / 2 with a ZERO fractional part in binary, i.e. frame 0, 1
    // and 2 read alone with no morph blend. The enum is therefore a discrete selector even though
    // the mechanism underneath it is continuous — asserted, not assumed, because a fourth wave
    // would silently break the exactness.
    //
    // LEVEL 0 IS BIT-EXACT, AND THAT IS THE CALLER'S JOB. `live()` reports whether the sub
    // contributes; a caller that is not live must not call `processSample()` at all — not because
    // the return would be wrong (it would be a true 0.0f) but because summing a literal 0.0f flips
    // a −0.0f sign bit, and because a silent oscillator must not ADVANCE its phase and colour the
    // note that switches it on. The same guard shape as `CathodeVoiceStage`'s `hpfLive_`.
    class SubOscillator
    {
    public:
        // The three waves, in `MachineRegistry`'s `kSubWaves[]` order — the ordinal IS the enum
        // row's plain value, so reordering here is a model-visible change and not a private one.
        enum class Wave : std::uint8_t {
            Square = 0,
            Sine = 1,
            Pulse = 2
        };

        static constexpr int kNumWaves = 3;

        // The fixed interval, as a frequency multiplier. −1 octave = ×0.5, which is EXACT in
        // binary, so the sub's pitch is the note's pitch halved to the last bit (the T-B19 pitch
        // verifier is measuring an exact relationship, not an approximate one).
        static constexpr Real kOctaveDown = 0.5;
        // [T-CGUI-57] The interval is now a control; its default is the fixed one above.
        static constexpr int kMinOctave = -2;
        static constexpr int kMaxOctave = 0;
        static constexpr int kDefaultOctave = -1;

        // The duty the `Pulse` wave is generated at. 25 % rather than a second 50 %: at 50 % duty a
        // pulse's EVEN harmonics cancel exactly, so `Square` and `Pulse` would be the same wave
        // under two names. 25 % puts harmonic 2 back, which is what makes the difference between
        // the two rows measurable rather than nominal.
        static constexpr Real kPulseDuty = 0.25;

        // Fills `out` with the three single-cycle frames laid out contiguously in `Wave` order
        // (`3 · WavetableBank::kFrameLength` floats) and builds `table`'s mip pyramid from them.
        // NOT RT-safe: allocates, runs FFTs and calls libm. Deterministic — no RNG, no clock, no
        // file input, so two runs are bit-identical.
        static void buildTable (Wavetable& table);

        // Binds the shared table (non-owning; must outlive this oscillator and not be rebuilt
        // while in use). Control-rate; RESETS THE PHASE, because `WavetableOscillator::setWavetable`
        // does — so a caller must bind once at prepare rather than per block (the same hazard
        // `CathodeMachine::bindBank`'s already-bound guard exists for).
        //
        // NO RE-APPLY OF FREQUENCY OR WAVE HERE, and its ABSENCE is the tested part. The first
        // version called both, on the reasoning that a sub configured BEFORE its bind would have
        // resolved its position against `numFrames_ = 0`. True as far as it goes — and irrelevant,
        // because `WavetableOscillator::setWavetable` ends by re-resolving mip AND position itself,
        // so both are already correct whichever order the caller used. The re-apply was therefore a
        // guard nothing could break: deleting it caused no test to fail, which is what says it was
        // dead (T-B26 — delete a redundant guard rather than label it). The order-independence it
        // was there to buy is REAL and still owed, so it is asserted directly in
        // `test_sub_oscillator.cpp` instead, where it reads as a claim about the primitive's
        // contract rather than as a line of ours nobody can question.
        void setWavetable (const Wavetable& table) noexcept
        {
            osc_.setWavetable (table);
            bound_ = true;
        }

        // Playback sample rate (Hz). Control-rate; re-resolves mip selection.
        void setSampleRate (Wide sampleRate) noexcept { osc_.setSampleRate (sampleRate); }

        // The PLAYED note's frequency in Hz. The sub sounds an octave below it — the halving is
        // this class's law and is applied here, so no caller can spell the interval differently.
        void setNoteHz (Wide hz) noexcept
        {
            noteHz_ = hz;
            applyFrequency();
        }

        // [T-CGUI-57] `synth.sub.oct`, −2…0 (default −1 = `kOctaveDown`). Clamped; every value is a
        // power of two, so the interval stays bit-exact.
        void setOctave (int octaves) noexcept
        {
            octave_ = octaves < kMinOctave ? kMinOctave : (octaves > kMaxOctave ? kMaxOctave : octaves);
            applyFrequency();
        }
        [[nodiscard]] int octave() const noexcept { return octave_; }

        void setWave (Wave wave) noexcept
        {
            wave_ = wave;
            applyWave();
        }

        // Output level, clamped >= 0. 0 = bypass (see `live()`), and it is the ONLY off-state:
        // `synth.sub.level` is a plain `kPercent` row with no `offAtZero` flag, so unlike
        // `synth.hpf`/`kOnsetMs` its unit already reaches 0 and there is no clamped-vLo trap here.
        void setLevel (float level) noexcept { level_ = level < 0.0f ? 0.0f : level; }

        // Zeroes the phase. RT-safe.
        void reset() noexcept { osc_.reset(); }

        // Whether the sub contributes at all. A caller MUST test this before calling
        // `processSample()` — see the class note on why level 0 is the caller's guard.
        [[nodiscard]] bool live() const noexcept { return bound_ && level_ > 0.0f; }

        [[nodiscard]] float level() const noexcept { return level_; }
        [[nodiscard]] Wave wave() const noexcept { return wave_; }
        // The frequency the sub is actually RUNNING at — read at the oscillator, not recomputed
        // from `noteHz_`, so a missing `applyFrequency()` is observable rather than papered over.
        [[nodiscard]] Wide frequencyHz() const noexcept { return osc_.frequencyHz(); }
        [[nodiscard]] const Wavetable* boundTable() const noexcept { return osc_.boundTable(); }

        // One level-scaled sample. Hot path: the wavetable read plus one multiply.
        float processSample() noexcept { return level_ * osc_.processSample(); }

    private:
        // The morph position that reads frame `w` alone. See the class note: with three frames
        // these three values are exact.
        [[nodiscard]] static constexpr float positionFor (Wave w) noexcept
        {
            return static_cast<float> (static_cast<int> (w)) / static_cast<float> (kNumWaves - 1);
        }

        // The three multipliers are exact powers of two; −1 is `kOctaveDown`, today's interval.
        [[nodiscard]] static constexpr Real octaveMultiplier (int octaves) noexcept
        {
            return octaves <= -2 ? 0.25 : (octaves == -1 ? kOctaveDown : 1.0);
        }
        void applyFrequency() noexcept { osc_.setFrequency (noteHz_ * octaveMultiplier (octave_)); }
        void applyWave() noexcept { osc_.setPosition (positionFor (wave_)); }

        WavetableOscillator osc_;
        Wide noteHz_ = 0.0;
        float level_ = 0.0f;
        int octave_ = kDefaultOctave;
        Wave wave_ = Wave::Square;
        bool bound_ = false;
    };
}
