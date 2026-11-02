#pragma once

#include <nemesis/dsp/Real.h>
#include <cmath>
#include <cstdint>

#include <nemesis/dsp/WavetableOscillator.h>

namespace nemesis::dsp
{
    // PulseOscillator — CATHODE's TRUE analogue pulse (`synth.osc{1,2}.{pwSrc,pw,pwm,sawMix}`, the
    // `Pulse` mode appended to `CathodeOscCore::Mode` at index 4; built at T-CMP-15). Continuous duty
    // between 5 % and 95 %, and the square at exactly 50 % is an anchor rather than a happy accident.
    //
    // IT IS A DIFFERENCE OF TWO READS OF ONE BAND-LIMITED SAW, at a continuous phase offset — not a
    // hand-rolled square, not a PolyBLEP, and not a duty SWEEP over table frames (T-CMP-15 plan-gate
    // R1). Three things fall out of that one choice, and they are the entire argument for it:
    //
    //   1. THE DUTY IS CONTINUOUS BECAUSE THE OFFSET IS. The offset is a `double`; nothing is
    //      quantised. That is the measurable gap to the 7 shipped duty-sweep WAVETABLE banks
    //      (`factory.chip.pwm`/`duty8`, `factory.basic.analog`/`pwmwide`/`pwmnarrow`/`pwmreverse`/
    //      `dutyladder`), which give a modulatable width today but in 16/32/64 discrete steps — those
    //      banks STAY, they are a different mechanism at a different cost.
    //   2. THE ANTI-ALIASING IS M1-04's, ALREADY GOLDEN-TESTED, not a second implementation of it.
    //      This is T-CMP-02's plan-gate R1 held to rather than re-litigated: that card refused a naive
    //      −1-oct square for exactly this reason ("at the top of the keyboard a 50 % square's odd
    //      harmonics run past Nyquist and fold, and `WavetableOscillator` already solves that"). And
    //      the claim SURVIVES the subtraction analytically, not just empirically: a difference of two
    //      band-limited signals is LINEAR, so it creates no partial the operands did not carry. The
    //      comb `1 − e^{−jωd}` reshapes the harmonics; it cannot add one.
    //   3. DC CANCELS EXACTLY, AT EVERY DUTY, FOR FREE. Both reads are of the SAME table through the
    //      SAME resolved mip and frame, so whatever DC the stored saw frame carries (`BasicTableGenerator`
    //      records its own as −1/2048) appears identically in both terms and subtracts out. An ideal
    //      ramp makes the same point in closed form: with `r(φ) = 2φ − 1`, `r(φ) − r(φ + d)` is
    //      `2 − 2d` over a span of width `d` and `−2d` over the remaining `1 − d`, whose mean is
    //      `d(2 − 2d) + (1 − d)(−2d) = 0`. Peak-to-peak is 2 at every duty, so there is no
    //      duty-dependent gain to compensate either.
    //
    // THE TABLE IS SHARED AND NON-OWNING, exactly like a slot's bank and like `SubOscillator`'s.
    // `buildTable()` is the build-time half (allocates, runs FFTs, calls libm — off the audio thread);
    // `setWavetable()` is the RT half. ONE frame, not three: the pulse needs a saw and nothing else, so
    // the pyramid is ~180 KB rather than the sub's ~540. An UNBOUND pulse is silent.
    //
    // THE READ ORDER IS LOAD-BEARING AND IS THE ONE THING A REFACTOR COULD BREAK IN SILENCE. Both reads
    // must sample the SAME instant, so the non-advancing `sampleAt()` runs FIRST and the advancing
    // `processSample()` second. Swapped, the two terms would sit one sample apart and the pulse would
    // acquire a one-sample-wide glitch at each edge — audible as a click, invisible to a duty or DC
    // assertion. `test_pulse_oscillator.cpp` pins the order directly.
    class PulseOscillator
    {
    public:
        // The duty RAILS, as a fraction of the cycle spent HIGH. Param Ref's `synth.osc{1,2}.pw` states
        // 5–95 %, and the rails are here rather than at the apply row because a modulated request has to
        // meet them too: `pwm` CLAMPS at these, it does not fold. Duty 0 and duty 1 are both silence, so
        // folding would make an over-driven sweep chop where the player asked it to breathe — the one
        // place this family deliberately differs from `mask`, whose 9-bit space is cyclic.
        static constexpr Real kDutyMin = 0.05;
        static constexpr Real kDutyMax = 0.95;

        // The duty at which a pulse IS a square, because its even harmonics cancel exactly there.
        // `SubOscillator::kPulseDuty` records the same fact from the other side (it picks 25 % precisely
        // so its `Square` and `Pulse` rows are not one wave under two names). Param Ref makes 50 %
        // land exactly on `pw` = 64, which is why the row cannot take a plain 0–127 → 5–95 % linear map.
        static constexpr Real kSquareDuty = 0.5;

        // Fills `table` with the ONE band-limited saw frame this oscillator reads and builds its mip
        // pyramid. NOT RT-safe: allocates, runs FFTs, calls libm. Deterministic — no RNG, no clock, no
        // file input, so two runs are bit-identical.
        static void buildTable (Wavetable& table);

        // Binds the shared table (non-owning; must outlive this oscillator and not be rebuilt while in
        // use). Control-rate; RESETS THE PHASE, because `WavetableOscillator::setWavetable` does — so a
        // caller binds once at prepare, never per block (`SubOscillator`'s note, same hazard).
        void setWavetable (const Wavetable& table) noexcept
        {
            osc_.setWavetable (table);
            bound_ = true;
        }

        // Playback sample rate (Hz). Control-rate; re-resolves mip selection.
        void setSampleRate (Wide sampleRate) noexcept { osc_.setSampleRate (sampleRate); }

        // The sounding frequency (Hz). Control-rate; re-resolves mip selection. Unlike `SubOscillator`
        // there is no interval law here — a pulse slot sounds at the note, so the caller's pitch is
        // passed through and this class owns no transposition to spell differently.
        void setFrequency (Wide hz) noexcept { osc_.setFrequency (hz); }

        // The fraction of the cycle spent HIGH, clamped to [kDutyMin, kDutyMax]. Control-rate.
        void setDuty (Real duty) noexcept
        {
            duty_ = duty < kDutyMin ? kDutyMin : (duty > kDutyMax ? kDutyMax : duty);
        }

        // How much of the naked SAW to blend in alongside the pulse (0..1) — the Juno/JP idiom, on the
        // page because the construction makes it nearly free: the first of the two reads IS the saw.
        //
        // IT IS A CROSSFADE, NOT A SUM, and that is a choice with a reason. Summing at full level would
        // put peak-to-peak at 4 into a slot gain that already defaults to 1.0, so the mode would clip
        // where the other four do not; a crossfade keeps the envelope bounded at every setting. The
        // cost is that the pulse thins as the saw comes in, which is what a single blend control means.
        // If the ear wants both at full, that is an additive variant and a ledger row, not a silent
        // change here.
        void setSawMix (float mix) noexcept
        {
            sawMix_ = mix < 0.0f ? 0.0f : (mix > 1.0f ? 1.0f : mix);
        }

        // [T-CAL-23 G1, D-M-0206] THE SAW LAW, an A/B for the owner's ear and NOT a host param.
        // `Crossfade` (the default, shipped) is the blend above: `pulse + (saw − pulse)·mix`, the pulse
        // thinning as the saw comes in. `Additive` is the Juno idiom, both at full: `pulse + saw·mix`,
        // so the pulse never thins and peak-to-peak grows with the mix (≈4 at mix 1, the headroom cost
        // the class note names). The default is the shipped bits exactly.
        enum class SawLaw : std::uint8_t {
            Crossfade,
            Additive
        };
        void setSawLaw (SawLaw law) noexcept { sawLaw_ = law; }
        [[nodiscard]] SawLaw sawLaw() const noexcept { return sawLaw_; }

        // Zeroes the phase. RT-safe.
        void reset() noexcept { osc_.reset(); }

        // [T-CMP-04 leaf 4] Sets the phase outright — the pulse's twin of
        // `WavetableOscillator::setPhase`, forwarding to the ONE phasor this class owns. There is
        // only one to set: the lagged term is a non-advancing peek at an offset from the lead, so
        // restarting the lead restarts the pulse.
        void setPhase (Real p) noexcept { osc_.setPhase (p); }

        // [T-CMP-04 leaf 4] The pulse's value at an ABSOLUTE phase, without advancing. Hard sync
        // needs the value either side of a restart to size the band-limited step, and computing
        // that in the CALLER would put a second copy of the pulse expression there — exactly what
        // `WavetableOscillator::readAt` was extracted to prevent one level down.
        [[nodiscard]] float sampleAtPhase (Real ph) const noexcept
        {
            ph -= std::floor (ph);
            Real lag = ph + duty_;
            if (lag >= 1.0)
                lag -= 1.0;
            return blend (static_cast<Real> (readAbs (ph)), static_cast<Real> (readAbs (lag)));
        }

        // Phase in turns, i.e. the LEAD read's phase — the one `processSample()` advances. There is
        // only one: the lagged term is a non-advancing peek at an offset from this, which is what
        // makes "the two reads sample the same instant" a property of the design rather than of two
        // clocks kept in step. Introspection / tests, and `CathodeOscCore::phase()`'s Pulse arm.
        [[nodiscard]] Wide phase() const noexcept { return osc_.phase(); }

        // Whether the oscillator can produce anything. UNLIKE `SubOscillator::live()` this is NOT a
        // level test — the pulse has no level of its own (`synth.osc{1,2}.level` is the slot's, one
        // layer up), so the only off-state is an unbound table. A caller must still test it: an unbound
        // read would dereference nothing.
        [[nodiscard]] bool live() const noexcept { return bound_; }

        // The APPLIED duty and saw mix, i.e. after this class's own clamps — read back rather than
        // recomputed, for the reason `WavetableOscillator::position()` states: a test that re-derived
        // the clamp itself would pass whether or not the oscillator was ever told.
        [[nodiscard]] Real duty() const noexcept { return duty_; }
        [[nodiscard]] float sawMix() const noexcept { return sawMix_; }
        [[nodiscard]] Wide frequencyHz() const noexcept { return osc_.frequencyHz(); }

        // Introspection for the alias keystone: the saw the pulse is BUILT from, so a test can compare
        // the two floors rather than compare the pulse against a number somebody typed.
        [[nodiscard]] const WavetableOscillator& saw() const noexcept { return osc_; }

        // One output sample; advances the phase once. Hot path: two reads of the same table, one
        // subtract, and at most one blend. No libm, no allocation.
        float processSample() noexcept
        {
            // ORDER IS LOAD-BEARING — see the class note. The non-advancing read first, so both terms
            // sample the same instant; the advancing read second.
            const Real lagged = static_cast<Real> (osc_.sampleAt (duty_));
            const Real saw = static_cast<Real> (osc_.processSample());

            // THIS BRANCH IS A COST GUARD, AND SAYING SO IS A CORRECTION. It was written as a
            // bit-exactness guard, by analogy with `SubOscillator`'s level-0 rule — and the analogy does
            // not hold here. That rule protects a −0.0 sign bit and a phase that must not advance;
            // neither applies to this expression. `pulse + (saw − pulse) * 0.0` differs from `pulse`
            // only when `pulse` is −0.0, and `pulse` is `saw − lagged`, which under round-to-nearest
            // yields +0.0 whenever the operands are equal. So the branch can never change the OUTPUT.
            // Caught by predicting the battery: the mutation that deletes it was predicted to SURVIVE,
            // which is the T-UI-56 rule — when a mutation cannot fail, ask whether the CLAIM is
            // falsifiable before assuming the test is weak, then say what the code really is.
            // It stays, as the skip of a multiply-add on the common setting, relabelled rather than
            // shipped with a decorative assertion behind it. The sawMix-0 OFF-ANCHOR is still a real
            // contract and is still asserted — it is just a claim about the output, which holds with
            // this branch or without it.
            return blend (saw, lagged);
        }

        // [T-CMP-03 leaf 2c] The PHASE-MODULATED read. BOTH terms take the same offset, which is the
        // only way a modulated pulse stays a pulse: the duty is the DISTANCE between the two reads,
        // so shifting one and not the other would modulate the WIDTH instead of the phase — a real
        // sound, and the wrong one, since `synth.osc{1,2}.pwm` already owns that gesture.
        //
        // `phaseOffsetTurns` MUST be in [0,1) — the caller's contract, inherited from
        // `WavetableOscillator::sampleAt` and refused-not-clamped there for the same reason. The one
        // conditional subtract below is then sufficient: duty_ < 1 and off < 1, so the sum is under 2.
        //
        // COST, named: three table reads where the unmodulated path takes two. The advancing read
        // exists only to move the phasor and its VALUE is discarded, because it would sample the
        // unmodulated instant. An `advance()` on the oscillator would save it; that is a further
        // public entry point on an L1 primitive to save one read, and it was declined.
        float processSample (Wide phaseOffsetTurns) noexcept
        {
            Wide lagPhase = duty_ + phaseOffsetTurns;
            if (lagPhase >= 1.0)
                lagPhase -= 1.0;

            const Real lagged = static_cast<Real> (osc_.sampleAt (lagPhase));
            const Real saw = static_cast<Real> (osc_.sampleAt (phaseOffsetTurns));
            (void) osc_.processSample(); // advance only — see the COST note above

            return blend (saw, lagged);
        }

    private:
        // [T-CMP-04 leaf 4] The pulse expression, in ONE place. It was written out TWICE — in
        // `processSample()` and in its phase-modulated overload — and leaf 4 needed it a third
        // time for `sampleAtPhase`, which is the point at which a duplicated law stops being
        // tolerable. Same reasoning `WavetableOscillator::readAt` records one level down.
        //
        // THE BRANCH IS A COST GUARD, AND SAYING SO IS A CORRECTION carried over from the site it
        // came from. It was written as a bit-exactness guard by analogy with `SubOscillator`'s
        // level-0 rule, and the analogy does not hold: that rule protects a -0.0 sign bit and a
        // phase that must not advance, and neither applies here. `pulse + (saw - pulse) * 0.0`
        // differs from `pulse` only when `pulse` is -0.0, and `saw - lagged` yields +0.0 whenever
        // the operands are equal under round-to-nearest — so it can never change the OUTPUT.
        // Caught by PREDICTING the battery (T-UI-56): the mutation deleting it was predicted to
        // survive, so the CLAIM was examined before the test was blamed. It stays as the skip of
        // a multiply-add on the common setting. The sawMix-0 off-anchor is a real contract and is
        // still asserted — it is a claim about the output, true with this branch or without it.
        [[nodiscard]] float blend (Real saw, Real lagged) const noexcept
        {
            const Real pulse = saw - lagged;
            if (sawMix_ <= 0.0f)
                return static_cast<float> (pulse);
            if (sawLaw_ == SawLaw::Additive) // [D-M-0206] A/B only; the crossfade below is the shipped bits
                return static_cast<float> (pulse + saw * static_cast<Real> (sawMix_));
            return static_cast<float> (pulse + (saw - pulse) * static_cast<Real> (sawMix_));
        }

        // A non-advancing read of the underlying saw at an ABSOLUTE phase, via `sampleAt`'s
        // offset form — so this class grows no copy of the table read either.
        [[nodiscard]] float readAbs (Real ph) const noexcept
        {
            Real off = ph - osc_.phase();
            off -= std::floor (off);
            if (off >= 1.0)
                off = 0.0;
            return osc_.sampleAt (off);
        }

        WavetableOscillator osc_;
        Real duty_ = kSquareDuty;
        float sawMix_ = 0.0f;
        SawLaw sawLaw_ = SawLaw::Crossfade;
        bool bound_ = false;
    };
}
