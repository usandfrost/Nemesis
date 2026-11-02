#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/CurveTable.h>

#include <cstddef>
#include <cstdint>

namespace nemesis::dsp
{
    namespace detail
    {
        // Shared band-limited-precision sine table for the LFO's Sine shape: kLfoSineTableSize + 1
        // points over one cycle (a guard point so linear interp never reads past the end),
        // table[i] = sin(2π·i / size). Built once, off the audio thread, on first use (the only
        // libm in this primitive lives in Lfo.cpp). ≈ −118 dB with linear interp at 2048 points —
        // far finer than an LFO needs. (The PD / PunchTransient sine LUTs are the same idiom; a
        // shared §0 SineTable primitive is a known candidate refactor, kept per-primitive for now.)
        inline constexpr std::size_t kLfoSineTableSize = 2048;
        const Real* lfoSineTableData() noexcept;
    }

    // Lfo — the CATHODE §4 LFO modulation-source primitive (CATHODE.md §4 "LFOs ×3 (shapes incl.
    // S&H/random)"; `synth.lfo1..3.{rate,shape,depth}`). A pure per-sample GENERATOR: processSample()
    // takes no audio input and returns a bipolar control value in [-1, 1] scaled by `depth` ∈ [0, 1].
    // Where it routes (its matrix destination) and any host-clock sync are higher-layer concerns, not
    // this primitive.
    //
    // Shapes (§4): Sine · Triangle · SawUp · SawDown · Square · SampleHold · RandomSmooth. The two
    // random shapes fold in as SHAPES (§4 "shapes incl. S&H/random"): both update once per LFO
    // period — SampleHold latches a new seeded-random value and holds it FLAT until the next period
    // boundary; RandomSmooth linearly interpolates between successive random targets across the
    // period (a smooth wander). Sine/Triangle start at 0 rising (in phase); SawUp ramps −1→+1,
    // SawDown +1→−1; Square is 50% duty (+1 then −1).
    //
    // [T-CGUI-61 leaf E2] Curve, the EIGHTH shape, plays a drawn `CurveTable` (setCurveTable) over
    // one period. It is appended outside the 7-value registry roster: `shapeForIndex` never returns
    // it, so FLTR's `lfoshape` and every stored SHAPE p-lock keep their meaning; the app selects it
    // through its own DRAW switch.
    //
    // NOT band-limited (contrast the delay-swing LFO in modulation.md §0.3): an LFO is a sub-audio
    // modulation signal and the hard edges of Square/SampleHold are WANTED for rhythmic movement;
    // anti-click is the destination-smoothing / mod-matrix concern, not the source.
    //
    // [T-PROTO-05] THAT LAW STANDS AND NOW HAS ONE NAMED EXCEPTION, stated here because the code
    // below would otherwise read as contradicting it. `setSmooth` softens SampleHold's edges on
    // demand — but it is a MUSICAL AXIS the player dials (`synth.sh.smooth`), not anti-click applied
    // at the source, and it DEFAULTS TO 0. So every existing caller, including
    // `synth.lfo{1,2,3}.shape = S&H`, keeps the hard edges this paragraph is about; only the
    // dedicated `synth.sh` slot has a surface that can ask for anything else.
    //
    // Convention (as AdsrEnvelope / PunchTransient): processSample() RETURNS the current sample's
    // value, THEN advances the phase (and, at a period wrap, the random state).
    //
    // RT-safety (§11): prepare() caches the sample rate + the sine-table pointer (the only non-RT
    // step); the hot path is inline pure arithmetic — no libm CALLS, no allocation, no locks.
    // Deterministic (§11): the random shapes are SEEDED (a deterministic LCG); a given
    // (params + seed) renders bit-identically and reset() reseeds, so §6 per-voice variation is
    // wireable (each voice gets its own setSeed). Computed in double, emitted as float. No JUCE.
    // Not thread-safe against prepare().
    class Lfo
    {
    public:
        enum class Shape : std::uint8_t {
            Sine,
            Triangle,
            SawUp,
            SawDown,
            Square,
            SampleHold,
            RandomSmooth,
            Curve // [T-CGUI-61] APPENDED: the seven above keep their ordinals, shapeForIndex never yields it
        };

        // [T-CORE-14 leaf C, rehomed T-FX-09 leaf 4] The registry's 7-value shape ROSTER →
        // this enum, and **the one enum map in the build that is not the identity**: the
        // roster is {Sine, Tri, Saw, Ramp, Square, S&H, Rand} while `Shape` is {Sine,
        // Triangle, SawUp, SawDown, Square, SampleHold, RandomSmooth}, so indices 2 and 3
        // are TRANSPOSED — Param Ref:195 states the realisation explicitly ("Saw = SawDown,
        // Ramp = SawUp"), i.e. a registry Saw ramps DOWN.
        //
        // IT LIVES HERE, ON THE PRIMITIVE THAT OWNS THE ENUM, because there are now TWO
        // consumers of the same roster — `synth.lfo{1..3}.shape` (per voice, via
        // `CathodeMachine`) and `fltr.lfoshape` (a TRACK insert, via `FltrEffect`) — and this
        // is exactly the ADR-012 C2 "one datum, two transcriptions" shape: a second copy of a
        // map whose whole risk is a silent INVERSION would pass any "these two indices render
        // differently" test in either consumer (the T-CORE-13 orientation trap).
        //
        // A switch rather than a clamped cast, for that same reason, and out-of-range falls
        // back to Sine (the roster's own first value) rather than wrapping.
        [[nodiscard]] static Shape shapeForIndex (int index) noexcept
        {
            switch (index)
            {
                case 1:
                    return Shape::Triangle;
                case 2:
                    return Shape::SawDown; // "Saw" — Param Ref:195
                case 3:
                    return Shape::SawUp; // "Ramp" — Param Ref:195
                case 4:
                    return Shape::Square;
                case 5:
                    return Shape::SampleHold;
                case 6:
                    return Shape::RandomSmooth;
                default:
                    return Shape::Sine;
            }
        }

        // Caches the sample rate + sine-table pointer, resolves the phase increment, and resets
        // (phase 0, reseeded). Not RT-safe (first call builds the shared sine table).
        void prepare (Wide sampleRate) noexcept
        {
            sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
            sine_ = detail::lfoSineTableData();
            updateInc();
            updateDelay();
            reset();
        }

        // Returns the LFO to phase 0 and RESEEDS the random stream (deterministic). RT-safe. Does
        // not change the parameters. Clears the retrigger START phase too (see setPhase): a reset is
        // the hard-stop path, so a start phase set for a patch that is being torn down must not
        // survive as the next note's restart target.
        void reset() noexcept
        {
            phase_ = 0.0;
            startPhase_ = 0.0;
            delayCounter_ = 0; // [T-CMP-01] a hard stop disarms the onset; see the note above
            state_ = baseSeed_;
            cur_ = lcg (state_);
            next_ = lcg (state_);
            unwound_ = 0;
        }

        // LFO rate in Hz (`synth.lfo*.rate`). Clamped ≥ 0 (0 → a held DC value). Control-rate.
        void setRateHz (float hz) noexcept
        {
            rateHz_ = hz > 0.0f ? hz : 0.0f;
            updateInc();
        }

        // The current rate in Hz (post-clamp) — the §6 rate-spread test/introspection anchor.
        [[nodiscard]] float rateHz() const noexcept { return rateHz_; }

        // Waveform shape (`synth.lfo*.shape`). Hot-path enum — no re-prepare, no phase reset.
        void setShape (Shape s) noexcept { shape_ = s; }

        // The current shape. [T-CORE-14 leaf C] Exposed for the param-layer's enum ORIENTATION pin:
        // `synth.lfo*.shape`'s 7-value list crosses two of them (Param Ref:195 — Saw = SawDown,
        // Ramp = SawUp), and a render test can only ever say "these two indices differ", which is
        // symmetric under swapping them (the T-CORE-13 distinctness lesson). Reading the applied
        // shape back is what makes the map's DIRECTION assertable.
        [[nodiscard]] Shape shape() const noexcept { return shape_; }

        // [T-CGUI-61 leaf E2] The baked table `Shape::Curve` plays — NON-OWNING: the caller keeps it
        // alive and unmodified for as long as it is set (a lock-free hand-over is the app layer's).
        // nullptr makes Curve output 0 — no modulation — rather than stand in some other shape. The
        // pointer is read by the Curve arm only, so setting it moves nothing on the other shapes.
        void setCurveTable (const CurveTable* table) noexcept { curve_ = table; }
        [[nodiscard]] const CurveTable* curveTable() const noexcept { return curve_; }

        // Output depth in [0, 1] (`synth.lfo*.depth`). A pure output multiply. Control-rate.
        void setDepth (float d) noexcept { depth_ = d < 0.0f ? 0.0 : (d > 1.0f ? 1.0 : static_cast<Real> (d)); }
        // [T-CORE-14 leaf D] The applied depth, read back (the `rateHz()`/`shape()` pin idiom). The
        // §2.9 MOTION macro SCALES this row (Param Ref:121 — "scales the dialed LFO/step depths"), so
        // what reaches the setter is a product of two registry values; a render can say the depth
        // moved but not that it landed on `motion·dialed`, which is the right-setter/wrong-arithmetic
        // survivor class an exact read-back closes.
        [[nodiscard]] float depth() const noexcept { return static_cast<float> (depth_); }

        // [T-PROTO-05] SMOOTH (`synth.sh.smooth`) — the FRACTION OF THE PERIOD the SampleHold shape
        // spends ramping to the newly latched target before it holds flat. This is NOT a new shape
        // and not a filter: it is the continuous axis between two shapes this class ALREADY ships,
        // and both of its endpoints are bit-exact rather than approximate.
        //   * `smooth == 0` returns `cur_` by the same expression as before, so a default-constructed
        //     Lfo renders BIT-IDENTICALLY to the pre-T-PROTO-05 build. That is what lets the row go
        //     live without moving the committed golden, and it is asserted, not assumed.
        //   * `smooth == 1` reduces to `cur_ + (next_ - cur_) * p` — `Shape::RandomSmooth`'s own
        //     expression, exactly (division by 1.0 is exact in IEEE). So the top of the range is the
        //     shipped shape rather than something that merely resembles it.
        // The output is CONTINUOUS for every smooth > 0: the ramp lands on `next_` at `p == smooth`
        // and holds there, and the boundary then assigns `cur_ = next_`, so nothing steps.
        // DEFAULTS TO 0, which is why `synth.lfo{1,2,3}.shape = S&H` is untouched by this row — the
        // hard edges Square/SampleHold exist for (see the header note above) stay hard unless a
        // caller asks otherwise. Control-rate; the dedicated `synth.sh` slot is its only writer.
        void setSmooth (float s) noexcept
        {
            smooth_ = s < 0.0f ? 0.0 : (s > 1.0f ? 1.0 : static_cast<Real> (s));
        }
        // Read back the applied value, the `rateHz()`/`shape()`/`depth()` pin idiom: a render can say
        // the curve moved but not that it landed on the value the seam sent.
        [[nodiscard]] float smooth() const noexcept { return static_cast<float> (smooth_); }

        // Base RNG seed for the deterministic random shapes — drives §6 per-voice variation.
        // Reseeds + returns to phase 0 so a render from here is reproducible.
        void setSeed (std::uint32_t seed) noexcept
        {
            baseSeed_ = seed;
            reset();
        }

        // Sets the phase accumulator directly, in [0, 1) (for §6 per-voice phase spread / retrigger
        // policy — the voice layer's concern). Wrapped into range. Does not touch the random state.
        //
        // It ALSO records the phase as the retrigger START phase (see retrigger below).
        void setPhase (float p01) noexcept
        {
            Real p = static_cast<Real> (p01);
            p -= static_cast<Real> (static_cast<long> (p)); // fractional part
            if (p < 0.0)
                p += 1.0;
            phase_ = p;
            startPhase_ = p;
        }

        // [T-CORE-14 leaf C] Restart the waveform where it last STARTED — the phase-only half of
        // `synth.lfo*.sync` = Trig (Param Ref:194, "restart phase at note-on, per voice"). WHICH LFOs
        // restart and WHEN stays the voice layer's policy, as with setPhase above.
        //
        // The target is the last commanded phase rather than 0, and that is the whole reason this
        // exists as its own call: Param Ref:194 continues "composes with `voice.lfoPhaseSpread`
        // (spread offsets apply to the retriggered phase)", and the §6 spread is delivered through
        // setPhase (CathodeVoiceEngine::applyLfoPhases). Returning to 0 would delete each voice's
        // spread offset on its first note-on and silently re-sync the whole pool.
        //
        // Phase ONLY: the random stream carries on (matching setPhase's contract), so an S&H shape
        // under Trig keeps wandering instead of replaying one sequence per note. reset() is the call
        // that reseeds. RT-safe.
        void retrigger() noexcept
        {
            phase_ = startPhase_;
            unwound_ = 0; // [T-CGUI-79 A4] a restart owes no boundary from before it
        }

        // [T-CGUI-59] BAR-LOCK: set the RUNNING phase only, wrapped into [0, 1). Unlike setPhase it
        // leaves the retrigger start phase alone — the transport's tolerance lock writes this, and a
        // Trig slot's start must stay the §6 spread offset. No libm (the setPhase wrap idiom). RT-safe.
        void lockPhase (Real p01) noexcept
        {
            Real p = p01 - static_cast<Real> (static_cast<long long> (p01));
            if (p < 0.0)
                p += 1.0;
            phase_ = p >= 1.0 ? 0.0 : p;
        }

        // [T-CGUI-79 A4] Move the RUNNING phase by `d` cycles (|d| < 1), the period boundary it crosses
        // COUNTED so a shift never adds or drops a period's random draw: forward across it draws exactly
        // as the advance does; backward across it banks the boundary, and the next forward crossing
        // re-uses it instead of drawing. The retrigger start phase is untouched. No libm. RT-safe.
        void shiftPhase (Real d) noexcept
        {
            phase_ += d;
            if (phase_ >= 1.0)
            {
                phase_ -= 1.0;
                crossBoundary();
            }
            else if (phase_ < 0.0)
            {
                phase_ += 1.0;
                ++unwound_;
            }
        }

        // [T-CMP-01 leaf 2] Note-on onset delay in ms (`synth.lfo*.delay`, curve kOnsetMs; v = 0 is
        // the row's off-state and arrives as 0). Control-rate.
        void setDelayMs (float ms) noexcept
        {
            // Stored RAW. The negative/zero case is carried by updateDelay's own `n > 0.0`
            // below, and behind that by `delayCounter_ > 0` in processSample — so clamping here
            // as well was a third guard on the same condition, and the battery proved it: the
            // mutation removing it killed nothing, because nothing it protects is reachable.
            // One enforcement point rather than three (T-B26); the clamp is gone, not labelled.
            delayMs_ = ms;
            updateDelay();
        }

        // Arms the onset delay for THIS note. Called at every note-on for every LFO, Trig or Free
        // — the Juno delay is a property of the note, not of the sync mode — and it is a no-op when
        // no delay is dialed, which is why a Free LFO with delay 0 is still never touched (the
        // law CathodeModulatedVoice::gateOn states for the retrigger).
        void armDelay() noexcept { delayCounter_ = delaySamples_; }

        [[nodiscard]] int delaySamplesForTest() const noexcept { return delaySamples_; }
        // [T-CGUI-59, S5R r4] A SYNCED LFO RUNS THROUGH its onset delay: the output is muted but the phase
        // keeps advancing, so it releases ON the bar. A held phase would speak behind the bar and be pulled
        // forward while audible. Free and Trig keep the hold (the Juno "restart AND wait"), bit-exact.
        void setRunThroughHold (bool run) noexcept { runThroughHold_ = run; }
        [[nodiscard]] bool runThroughHold() const noexcept { return runThroughHold_; }

        // The current output value (depth · shape at the current phase), without advancing.
        [[nodiscard]] float value() const noexcept { return static_cast<float> (depth_ * shapeValue (phase_)); }
        // [T-CGUI-76 S5R2] What the NEXT `processSample` returns — `value()`, or 0 while a delay holds.
        [[nodiscard]] float nextValue() const noexcept { return delayCounter_ > 0 ? 0.0f : value(); }

        // [T-CAL-06] The phase accumulator itself, in [0, 1). Exposed for the same
        // reason `rateHz()` and `shape()` are: a claim about it is otherwise only
        // arguable. R10 rules clock-sync RATE-LOCKED and not phase-locked, so the
        // card owes the negative property — switching a row Free→Sync changes the
        // increment and must NOT restart the waveform — and `value()` cannot say
        // it (two different phases share a value on every shape but the saws, and
        // phase 0 and 0.5 are both exactly 0 on a sine). Read-only: nothing about
        // the phase POLICY moves here, which stays the voice layer's (setPhase).
        [[nodiscard]] float phase() const noexcept { return static_cast<float> (phase_); }

        // Advances one sample and returns THIS sample's value (read first, then advance — as
        // AdsrEnvelope). Hot path: pure arithmetic, no libm calls / allocation / locks.
        float processSample() noexcept
        {
            // [T-CMP-01 leaf 2] `synth.lfo*.delay` — the Juno delayed-vibrato onset. The hold sits
            // BEFORE both the value and the phase advance, so the LFO is HELD rather than muted:
            // after the hold it speaks from exactly the phase it was armed at. That is what makes
            // it compose with Trig — the card requires a Trig+delay LFO to "restart AND wait", and
            // a muted-but-running phase would throw the retrigger away. A free-running LFO is held
            // too, but only if a delay is dialed: at 0 this branch never fires and the row is a
            // bit-exact no-op, which is what keeps every existing patch unmoved. [T-CGUI-59] All of
            // this is Free/Trig: a SYNCED LFO runs through the hold instead (see setRunThroughHold).
            if (delayCounter_ > 0)
            {
                --delayCounter_;
                if (runThroughHold_)
                    advance(); // [T-CGUI-59] a synced LFO keeps the bar through its delay
                return 0.0f;
            }
            const Real out = depth_ * shapeValue (phase_);
            advance();
            return static_cast<float> (out);
        }

        // FIREFORK E8: processSample without computing the value nobody reads (the hold and the advance are the same).
        void tick() noexcept
        {
            if (delayCounter_ > 0)
            {
                --delayCounter_;
                if (runThroughHold_)
                    advance();
                return;
            }
            advance();
        }

    private:
        void advance() noexcept
        {
            phase_ += inc_;
            // A while-loop wrap stays deterministic even if inc_ ≥ 1 (draws one random per period).
            while (phase_ >= 1.0)
            {
                phase_ -= 1.0;
                crossBoundary();
            }
        }

        // One period boundary crossed forward: the next random target — unless a backward `shiftPhase`
        // already banked this boundary ([T-CGUI-79 A4]; 0 unless a shift ran, so every other path is
        // the draw it always was).
        void crossBoundary() noexcept
        {
            if (unwound_ > 0)
            {
                --unwound_;
                return;
            }
            cur_ = next_;
            next_ = lcg (state_);
        }

        void updateDelay() noexcept
        {
            const Real n = static_cast<Real> (delayMs_) * 0.001 * sampleRate_;
            delaySamples_ = n > 0.0 ? static_cast<int> (n) : 0;
        }

        void updateInc() noexcept { inc_ = rateHz_ / sampleRate_; }

        // Deterministic LCG in [-1, 1) (same generator as ConverterDrift / BbdDelayLine).
        static Real lcg (std::uint32_t& s) noexcept
        {
            s = s * 1664525u + 1013904223u;
            return static_cast<Real> (static_cast<std::int32_t> (s)) * (1.0 / 2147483648.0);
        }

        // The bipolar shape value at phase p ∈ [0, 1) (pre-depth). Pure arithmetic; the Sine branch
        // reads the cached LUT (linear interp), the random branches read the latched state.
        Real shapeValue (Real p) const noexcept
        {
            switch (shape_)
            {
                case Shape::Sine:
                {
                    const Real x = p * static_cast<Real> (detail::kLfoSineTableSize);
                    const auto i = static_cast<std::size_t> (x); // p ∈ [0,1) → i ∈ [0, size-1]
                    const Real f = x - static_cast<Real> (i);
                    return sine_[i] + (sine_[i + 1] - sine_[i]) * f;
                }

                case Shape::Triangle:
                    if (p < 0.25)
                        return 4.0 * p; // 0 → +1
                    if (p < 0.75)
                        return 2.0 - 4.0 * p; // +1 → −1
                    return 4.0 * p - 4.0; // −1 → 0

                case Shape::SawUp:
                    return 2.0 * p - 1.0;

                case Shape::SawDown:
                    return 1.0 - 2.0 * p;

                case Shape::Square:
                    return p < 0.5 ? 1.0 : -1.0;

                case Shape::SampleHold:
                {
                    // [T-PROTO-05] smooth 0 → this is `cur_`, the same expression as before and so
                    // bit-exact. smooth 1 → `cur_ + (next_ - cur_) * p`, which IS the RandomSmooth
                    // arm below. Anything between ramps across the first `smooth` of the period and
                    // then holds on `next_`, which the boundary is about to make `cur_` — continuous.
                    if (smooth_ <= 0.0)
                        return cur_; // held flat until the next period boundary
                    const Real t = p >= smooth_ ? 1.0 : p / smooth_;
                    return cur_ + (next_ - cur_) * t;
                }

                case Shape::RandomSmooth:
                    return cur_ + (next_ - cur_) * p; // linear ramp between random targets

                case Shape::Curve:
                    // [T-CGUI-61 leaf E2] The drawn table, 0–1 mapped onto the −1…+1 the other
                    // shapes span (val 0.5 → 0). Last arm; it writes no state.
                    return curve_ != nullptr ? 2.0 * curve_->valueAt (p) - 1.0 : 0.0;
            }
            return 0.0; // unreachable; keeps -Wreturn-type quiet
        }

        // Configuration / control-rate.
        Wide sampleRate_ = 48000.0;
        float rateHz_ = 1.0f;
        Real depth_ = 1.0;
        Real smooth_ = 0.0; // [T-PROTO-05] `synth.sh.smooth`; 0 = the shipped SampleHold
        Shape shape_ = Shape::Sine;
        std::uint32_t baseSeed_ = kDefaultSeed;
        Wide inc_ = 1.0 / 48000.0; // phase increment per sample = rateHz / fs

        const Real* sine_ = nullptr; // cached in prepare(); the Sine LUT
        const CurveTable* curve_ = nullptr; // [T-CGUI-61] see setCurveTable; not owned

        // Per-sample state.
        Wide phase_ = 0.0; // [0, 1)
        Wide startPhase_ = 0.0; // the phase retrigger() returns to — the last setPhase (see it)
        float delayMs_ = 0.0f; // [T-CMP-01] `synth.lfo*.delay`
        int delaySamples_ = 0;
        int delayCounter_ = 0;
        bool runThroughHold_ = false; // [T-CGUI-59] see setRunThroughHold
        std::uint32_t state_ = kDefaultSeed; // LCG state for the random shapes
        Real cur_ = 0.0; // random target at the start of the current period
        Real next_ = 0.0; // random target at the end of the current period
        int unwound_ = 0; // [T-CGUI-79 A4] boundaries a backward `shiftPhase` crossed, owed no draw

        static constexpr std::uint32_t kDefaultSeed = 0x1357'9BDFu;
    };
}
