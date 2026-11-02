#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/MaskTableGenerator.h>
#include <nemesis/dsp/Oversampler.h>

#include <array>
#include <cmath>

namespace nemesis::dsp
{
    // BitmaskOscillator — the RENDER-TIME 9-bit bitmask oscillator (T-MASK-03 leaf B, ADR-037).
    // A pure sine cycle is split into nine segments that are repeated, scaled, inverted, mirrored
    // or silenced according to the bits of a single number 0..511, and that number IS the timbre
    // control: `synth.osc{1,2}.mask`. This is the axis the reference instrument (Kodamo MASK 1)
    // is built on, and ADR-037 is the decision to reach it at render time rather than by
    // pre-rendering frames.
    //
    // WHY THIS IS CHEAP, which is the whole argument of ADR-037. Every op is a segment-LOCAL
    // transform of the carrier — an index displacement plus a gain — so all 512 masks are served by
    // ONE resident 2048-point sine table (`MaskTableGenerator::segmentTap` is the shared law). The
    // build-time alternative costs `frames x 11 x 2048 x 8` bytes: ~88 MB for the full space, which
    // is why the 2026-07-16 design shipped 16 of the 512 states and this one ships all of them.
    //
    // ONE LAW, TWO VIEWS. `MaskTableGenerator::renderMask` walks the same `segmentTap` over the
    // 2048 integer positions to produce a frame; this class evaluates it per sample at fractional
    // phase. Their agreement is BIT-EXACT and is a test, not a convention — every op gain (±1, 0,
    // kScaleGain) is exact in binary, and a table read at an integral index is exact under linear
    // interpolation. A wrong segment boundary would show up there and nowhere else.
    //
    // RT-safety: prepare() builds the sine table and forces the shared DC table (allocates, off the
    // audio thread); setMask/setFrequency are control-rate and O(1); processSample() is inline pure
    // arithmetic — no libm CALLS (the phase wrap is std::floor, one arm64 frintm, the sanctioned
    // category), no allocation, no locks. Deterministic — no randomness, so a re-render is
    // bit-identical. Computed in double, emitted as float. No JUCE. Not thread-safe vs prepare().
    //
    // BAND-LIMITING (T-MASK-03 leaf C, ruling 1). A bitmask waveform is hard discontinuities (a
    // silenced or inverted segment is a step; Mirror and Repeat add slope breaks), and the mip
    // pyramid that used to supply band-limiting for free is exactly what ADR-037 gives up. So the
    // generator runs inside `dsp::Oversampler` — the phasor advances inc/factor per oversampled
    // sample and the decimating FIR band-limits on the way down, the seam PhaseDistortionOscillator
    // already proves for a GENERATOR (a dummy 0 input upsamples to zeros; the waveform is
    // synthesised at each OS phase inside the shaper lambda). The phase RAMP is never upsampled —
    // its 1→0 wrap would smear under a band-limiting interpolator.
    //
    // AND THE BYPASS IS EXPOSED, which is the half of ruling 1 that is a product decision rather
    // than a DSP one. The reference instrument is hardware described as "bleepy, raspy", and the
    // clean path is cleaner than the thing we are trying to sound like — so the naive base-rate
    // readout is a per-slot USER CHOICE (`setOversamplingBypassed`), not a road not taken. It is a
    // runtime flag and never a re-`prepare()`: `prepare` designs the FIR and ALLOCATES.
    //
    // THE BYPASS CHANGES THE SLOT'S GROUP DELAY, stated rather than silently inherited. The
    // decimating cascade delays by `latencySamples()` base samples and the base-rate path by zero,
    // so flipping the switch shifts this oscillator against its sibling slot. That is consistent
    // with existing behaviour rather than new debt — `CathodeOscCore` already documents that it does
    // NOT sample-align its two slots, because a fixed offset between free-running oscillators is an
    // inaudible initial-phase difference and stays deterministic.
    class BitmaskOscillator
    {
    public:
        // The carrier length, and therefore the mask space's segment geometry: shared with the
        // generator so the two views cannot disagree about the frame the law is defined on.
        static constexpr int kFrameLength = MaskTableGenerator::kFrameLength; // 2048
        static constexpr int kMaskSpace = MaskTableGenerator::kMaskSpace; // 512
        // Oversampling factor for the masked waveform. 4x is the PhaseDistortion slot's shipped
        // figure and it clears the keystone's 6 dB bar by a wide margin here; the segment steps are
        // harder edges than a CZ warp, so this is measured per pitch rather than argued.
        static constexpr int kDefaultOversample = 4;
        // [leaf C2] The PITCH-ADAPTIVE pair, and the pitch that separates them. The alias floor
        // degrades with pitch (measured: −37.9 dB at A2 down to −21.8 at A8 at a flat 4x), and the
        // cost of fixing it is only paid where it is needed — so the shipped default escalates to 8x
        // for high fundamentals and stays at 4x everywhere else. Chosen so the adaptive path is
        // NEVER WORSE than the flat 4x it replaces at any pitch: it only ever raises the factor.
        // 1100 Hz sits just above A5 (880), so the whole ordinary playing range keeps 4x and the
        // escalation is reached by the top octaves, where fundamentals are rare.
        static constexpr int kHighOversample = 8;
        static constexpr Wide kEscalateHz = 1100.0;

        // ADAPTIVE (the shipped default): builds the shared tables and designs BOTH decimating FIRs,
        // then selects between them per note. Not RT-safe (allocates) — call like prepareToPlay.
        void prepare (Wide sampleRate);

        // FIXED factor: one FIR at `oversampleFactor` (clamped >= 1; 1 = naive), no escalation. The
        // form the alias-floor and group-delay measurements use, because a claim about "factor N"
        // has to be able to pin N.
        void prepare (Wide sampleRate, int oversampleFactor);

        // Clears the phase AND both decimators, and COMMITS the pending factor — so the note that
        // starts here runs at the factor its own pitch asks for.
        //
        // WHY THE COMMIT IS HERE AND NOT IN setFrequency: switching decimator mid-note would step
        // the output (stale filter state) and jog the group delay, which is a click. Note-on is the
        // one moment both are free. The consequence is deliberate and worth stating — a glide or a
        // pitch-bend that crosses kEscalateHz keeps the factor it started on, so a note NEVER
        // changes character part-way through. RT-safe: an index, not a design.
        void reset() noexcept
        {
            phase_ = 0.0;
            commitPendingFactor(); // the factor half, shared with the note-on hook below
        }

        // [T-MASK-03 leaf D] The NOTE-ON commit for an owner that does NOT reset its oscillator at
        // note-on — which is what CATHODE turned out to be. `reset()` above commits the armed factor
        // and zeroes the phase; a CATHODE voice leaves both oscillator slots free-running across
        // notes (`CathodeModulatedVoice::gateOn` restarts envelopes and trig-synced LFOs and nothing
        // else), so routing the commit through `reset()` there would either impose a phase reset
        // this instrument has never had — a separate decision, carded as T-CMP-10 — or leave the
        // pitch-adaptive factor decided ONCE inside `prepare()`, at frequency 0, i.e. never
        // escalating at all. That second outcome is what shipped until this hook existed.
        //
        // Clears BOTH decimators, deliberately: the one becoming active must not open the note with
        // a previous note's tail in its ring, and the ~9-sample ramp-in that costs is the same
        // warm-up transient the band-limited path already has at every reset. RT-safe (an index and
        // two memsets over pre-allocated storage — no design, no allocation).
        void commitPendingFactor() noexcept
        {
            activeHigh_ = pendingHigh_;
            resolveIncOs();
            osBase_.reset();
            osHigh_.reset();
        }

        // Oscillator frequency in Hz. Control-rate (re-resolves BOTH phase increments — the
        // base-rate one the bypass advances by, and the OS-rate one the generator advances by).
        void setFrequency (Wide hz) noexcept
        {
            freqHz_ = hz;
            inc_ = sampleRate_ > 0.0 ? (fork::kApprox ? freqHz_ * invSampleRate_ : freqHz_ / sampleRate_) : 0.0; // FIREFORK N3
            pendingHigh_ = adaptive_ && freqHz_ >= kEscalateHz; // takes effect at the next reset()
            resolveIncOs();
        }

        // The per-slot clean/dirty switch (ruling 1). true = the naive base-rate readout, aliasing
        // and all. RT-safe and O(1) — a flag, never a re-`prepare()`, because that designs the FIR
        // and allocates. Leaves the decimator's state alone, so flipping back mid-note resumes from
        // whatever it held; `reset()` is the clean restart.
        void setOversamplingBypassed (bool bypassed) noexcept { bypassed_ = bypassed; }

        [[nodiscard]] bool oversamplingBypassed() const noexcept { return bypassed_; }
        // The factor CURRENTLY RUNNING — under adaptation this is a per-note value, not a constant.
        [[nodiscard]] int oversampleFactor() const noexcept { return activeOs().factor(); }
        [[nodiscard]] bool pitchAdaptive() const noexcept { return adaptive_; }
        // The factor the NEXT reset() would commit for the frequency now set (introspection/tests).
        [[nodiscard]] int pendingOversampleFactor() const noexcept
        {
            return (adaptive_ && pendingHigh_) ? osHigh_.factor() : osBase_.factor();
        }

        // Base-rate group delay of the emitted signal, or 0 when bypassed — the switch MOVES this,
        // see the class note. FRACTIONAL, and it is the GENERATOR figure (M + (factor−1)/factor,
        // 8.75 at the default factor 4), not the pass-through cascade's 2M: the waveform is
        // synthesised inside the shaper and so never meets the interpolating FIR.
        [[nodiscard]] Real latencySamples() const noexcept
        {
            return bypassed_ ? 0.0 : activeOs().generatedLatencySamples();
        }

        // The mask, 0..511 — wrapped into the 9-bit space, so a modulated request that runs off
        // either end folds rather than reading bits nobody defined. Control-rate but O(1): the DC
        // correction is a table lookup precisely because this is a modulation destination and moves
        // at audio rate under leaf F's delta-decay envelope.
        void setMask (int mask) noexcept
        {
            mask_ = mask & (kMaskSpace - 1);
            dc_ = MaskTableGenerator::dcOffsetFor (mask_);
        }

        [[nodiscard]] int mask() const noexcept { return mask_; }
        [[nodiscard]] Wide phase() const noexcept { return phase_; }

        // [T-CMP-04 leaf 6] HARD SYNC's phase pair, and they exist because THIS oscillator's
        // bypassed path ADVANCES THEN READS while `WavetableOscillator`'s reads then advances.
        // `phase()` above is therefore "the phase the last sample was read at" here and "the phase
        // the next sample will be read at" there — the same accessor name meaning two things one
        // increment apart, which is precisely how a sync restart lands a cycle-fraction off and
        // sounds like a tuning error rather than a bug.
        //
        // So the ordering quirk is owned HERE, by the class that has it, rather than by every
        // caller that has to remember it. Both are expressed in terms of the NEXT read, which is
        // the only frame of reference a restart cares about.
        [[nodiscard]] Wide nextReadPhase() const noexcept { return wrap01 (phase_ + inc_); }
        void setNextReadPhase (Real p) noexcept { phase_ = wrap01 (p - inc_); }

        // The OS-rate twin, for the oversampled path — where the restart must land on the
        // generator's own grid (the leaf-5 finding, applied to this oscillator's other branch).
        void setPhase (Real p) noexcept { phase_ = wrap01 (p); }
        [[nodiscard]] int activeOversampleFactor() const noexcept
        {
            return bypassed_ ? 1 : (activeHigh_ ? osHigh_.factor() : osBase_.factor());
        }

        // [T-CMP-04 leaf 6] One synced OS block: `step` runs before each OS advance so the caller
        // can restart the phase at an OS instant. The oversampled twin of `processSample()`, and
        // it selects the SAME decimator that path would.
        template <typename Step>
        float processSampleSynced (Step&& step) noexcept
        {
            auto& os = activeHigh_ ? osHigh_ : osBase_;
            return os.generateSample (/* FIREFORK E15 */ [this, &step] () noexcept {
                step (*this);
                return generateOsSample();
            });
        }

        // [D-M-0208] The OS-rate free/synced CROSSFADE, the `PhaseDistortionOscillator` twin (see its
        // note): `phase_` advances free, `syncPhase` is the synced copy restarted at `restartAtOs` OS
        // steps into this base sample, both summed before the ACTIVE decimator. Band-limited side only;
        // the caller routes a bypassed oscillator to its base-rate MinBLEP path.
        float processSampleSyncBlend (Wide& syncPhase, Real restartAtOs, Real freeGain, Real syncGain) noexcept
        {
            auto& os = activeHigh_ ? osHigh_ : osBase_;
            int j = 0;
            return os.generateSample (/* FIREFORK E15 */ [this, &syncPhase, &j, restartAtOs, freeGain, syncGain] () noexcept {
                phase_ = wrap01 (phase_ + incOs_);
                const Real elapsed = static_cast<Real> (++j) - restartAtOs;
                syncPhase = restartAtOs >= 0.0 && elapsed >= 0.0 && elapsed < 1.0 ? wrap01 (incOs_ * elapsed)
                                                                                  : wrap01 (syncPhase + incOs_);
                Real out = syncGain * static_cast<Real> (sampleAt (syncPhase));
                if (freeGain > 0.0)
                    out += freeGain * static_cast<Real> (sampleAt (phase_));
                return static_cast<float> (out);
            });
        }

        // The DC correction applied at `m` (introspection / tests). Exactly 0.0f at mask 0, which
        // is what makes the pure-sine anchor bit-exact rather than an ulp away from one.
        [[nodiscard]] float dcOffsetForMask (int m) const noexcept
        {
            return MaskTableGenerator::dcOffsetFor (m);
        }

        // The masked waveform at an arbitrary phase in turns — STATELESS, so it is both the hot
        // path's kernel and the frame view the agreement keystone compares against renderMask.
        [[nodiscard]] float sampleAt (Wide phaseTurns) const noexcept
        {
            const Real x = wrap01 (phaseTurns) * static_cast<Real> (kFrameLength);
            const auto tap = MaskTableGenerator::segmentTap (mask_, x);
            return static_cast<float> (static_cast<Real> (tap.gain) * carrierAt (tap.index)) - dc_;
        }

        // One base-rate sample, through whichever path the switch selects. Hot path: pure
        // arithmetic, no libm calls, no allocation, on both sides of the branch.
        //
        // The bypass is its own branch and not `Oversampler`'s `factor_ == 1` short-circuit: that
        // one still costs a `prepare()` to reach, and the whole point is a switch a note can flip.
        float processSample() noexcept
        {
            if (bypassed_)
            {
                phase_ = wrap01 (phase_ + inc_);
                return sampleAt (phase_);
            }
            auto& os = activeHigh_ ? osHigh_ : osBase_;
            return os.generateSample (/* FIREFORK E15 */ [this] () noexcept { return generateOsSample(); });
        }

        // [T-CMP-03 leaf 2c] The PHASE-MODULATED read. `phaseOffsetTurns` is added to the phase this
        // oscillator READS and never to `phase_` — an offset that accumulated would be a frequency
        // error, not a phase modulation.
        //
        // NOTE WHAT IS *NOT* HERE: a wrap. `sampleAt` takes an ABSOLUTE phase and calls `wrap01`
        // itself, which is the convention difference against `WavetableOscillator`'s relative,
        // caller-wraps `sampleAt`. Adding the offset INSIDE this oscillator is what keeps that
        // difference from reaching the core, where two conventions in one switch is how a slot ends
        // up reading the wrong instant.
        //
        // Both branches take the offset, including the `bypassed_` one — a slot with band-limiting
        // switched off is still a slot a patch can modulate.
        float processSample (Wide phaseOffsetTurns) noexcept
        {
            if (bypassed_)
            {
                phase_ = wrap01 (phase_ + inc_);
                return sampleAt (phase_ + phaseOffsetTurns);
            }
            auto& os = activeHigh_ ? osHigh_ : osBase_;
            return os.generateSample (/* FIREFORK E15 */ [this, phaseOffsetTurns] () noexcept {
                return generateOsSample (phaseOffsetTurns);
            });
        }

        // [T-CGUI-63] THE UNISON STACK — `n` members, each its own caller-owned phase running at this
        // oscillator's increment × `incMult[k]`, weighted by `gains[k]`. On the band-limited side the
        // members are summed at the OS rate and share the ACTIVE decimator; bypassed, they are summed at
        // the base rate (advance-then-read, as `processSample()` does). `phase_` does not advance.
        float processSampleStack (Wide* phases, const Wide* incMult, const float* gains, int n) noexcept
        {
            if (bypassed_)
            {
                Real sum = 0.0;
                for (int k = 0; k < n; ++k)
                {
                    phases[k] = wrap01 (phases[k] + inc_ * incMult[k]);
                    sum += static_cast<Real> (gains[k]) * static_cast<Real> (sampleAt (phases[k]));
                }
                return static_cast<float> (sum);
            }
            auto& os = activeHigh_ ? osHigh_ : osBase_;
            return os.generateSample (/* FIREFORK E15 */ [this, phases, incMult, gains, n] () noexcept {
                Real sum = 0.0;
                for (int k = 0; k < n; ++k)
                {
                    phases[k] = wrap01 (phases[k] + incOs_ * incMult[k]);
                    sum += static_cast<Real> (gains[k]) * static_cast<Real> (sampleAt (phases[k]));
                }
                return static_cast<float> (sum);
            });
        }

        // [stereo voice S4, D-M-0188] THE STACK WITH A SIDE — `processSampleStack` exactly (the mid), and
        // each member also weighted by `sideGains[k]` into a second stream: decimated by the caller's twin
        // of the ACTIVE decimator (`sideBase`/`sideHigh`, prepared as `baseOversampler`/`highOversampler`),
        // or summed directly on the bypassed (dirty) path, as the mid is.
        float processSampleStackSide (Wide* phases, const Wide* incMult, const float* gains, const float* sideGains, int n, Oversampler& sideBase, Oversampler& sideHigh, float& side) noexcept
        {
            if (bypassed_)
            {
                Real sum = 0.0;
                Real sd = 0.0;
                for (int k = 0; k < n; ++k)
                {
                    phases[k] = wrap01 (phases[k] + inc_ * incMult[k]);
                    const auto v = static_cast<Real> (sampleAt (phases[k]));
                    sum += static_cast<Real> (gains[k]) * v;
                    sd += static_cast<Real> (sideGains[k]) * v;
                }
                side = static_cast<float> (sd);
                return static_cast<float> (sum);
            }
            auto& os = activeHigh_ ? osHigh_ : osBase_;
            std::array<float, kMaxSideSteps> steps; // NOLINT(cppcoreguidelines-pro-type-member-init) — written before read
            int written = 0;
            const float mid = os.generateSample (/* FIREFORK E15 */ [&] () noexcept {
                Real sum = 0.0;
                Real sd = 0.0;
                for (int k = 0; k < n; ++k)
                {
                    phases[k] = wrap01 (phases[k] + incOs_ * incMult[k]);
                    const auto v = static_cast<Real> (sampleAt (phases[k]));
                    sum += static_cast<Real> (gains[k]) * v;
                    sd += static_cast<Real> (sideGains[k]) * v;
                }
                if (written < kMaxSideSteps)
                    steps[static_cast<std::size_t> (written++)] = static_cast<float> (sd);
                return static_cast<float> (sum);
            });
            int read = 0;
            side = (activeHigh_ ? sideHigh : sideBase).decimateSample ([&]() noexcept {
                return read < written ? steps[static_cast<std::size_t> (read++)] : 0.0f;
            });
            return mid;
        }
        // The two decimators, for a caller preparing its side twins alike (copy them off the audio thread).
        [[nodiscard]] const Oversampler& baseOversampler() const noexcept { return osBase_; }
        [[nodiscard]] const Oversampler& highOversampler() const noexcept { return osHigh_; }
        static constexpr int kMaxSideSteps = 64; // OS steps per base sample the side buffers (factor <= 64)

    private:
        // The decimator currently selected. Under a FIXED prepare() both members hold the same
        // factor and `activeHigh_` never leaves false, so this collapses to one object.
        [[nodiscard]] const Oversampler& activeOs() const noexcept
        {
            return activeHigh_ ? osHigh_ : osBase_;
        }

        // The OS-rate increment always tracks the ACTIVE factor, never the pending one — otherwise a
        // note would be transposed for the remainder of its life by a pitch it never played at.
        void resolveIncOs() noexcept
        {
            const int f = activeOs().factor();
            if constexpr (fork::kExact) // FIREFORK E7
                incOs_ = f > 0 ? fork::divByFactor (inc_, f) : inc_;
            else
                incOs_ = f > 0 ? inc_ / static_cast<Real> (f) : inc_;
        }

        // One oversampled sample: advance the phasor by inc/factor and read the masked waveform
        // there. Called `factor` times per base-rate sample, in OS-phase order.
        float generateOsSample() noexcept
        {
            phase_ = wrap01 (phase_ + incOs_);
            return sampleAt (phase_);
        }

        // [T-CMP-03 leaf 2c] The same step with a read-side phase offset. No wrap of the sum here
        // either, for the reason the public overload gives: `sampleAt` owns it.
        float generateOsSample (Wide phaseOffsetTurns) noexcept
        {
            phase_ = wrap01 (phase_ + incOs_);
            return sampleAt (phase_ + phaseOffsetTurns);
        }

        // Wraps a phase in turns into [0, 1). std::floor lowers to a single arm64 frintm — not a
        // libm call (the same sanctioned category as BBD's fsqrt).
        static Wide wrap01 (Wide p) noexcept { return p - std::floor (p); } // FIREFORK: was (Real p): float builds lost the phase

        // The carrier at a fractional index, by linear interpolation of the shared table. At an
        // INTEGRAL index frac is 0 and this returns the table entry exactly, which is what makes
        // the agreement with renderMask bit-exact rather than merely close.
        Real carrierAt (Real index) const noexcept
        {
            const int i = static_cast<int> (index);
            const Real frac = index - static_cast<Real> (i);
            const Real a = static_cast<Real> (carrier_[i]);
            const Real b = static_cast<Real> (carrier_[i + 1]); // guard point at [kFrameLength]
            return a + frac * (b - a);
        }

        // The one place a FIR may be designed (allocates). Both overloads of prepare() land here;
        // a FIXED prepare passes the same factor twice and adaptive = false, which pins activeHigh_
        // at false for the object's life.
        void prepareCommon (Wide sampleRate, int baseFactor, int highFactor, bool adaptive);

        // Builds (once, off the audio thread) and returns the shared kFrameLength+1 sine table.
        // Its first kFrameLength entries are bit-identical to the generator's own carrier
        // (`MaskTableGenerator.cpp::writeSine`); the guard point is the extra this view needs to
        // interpolate across the wrap.
        static const float* sharedCarrier() noexcept;

        Wide sampleRate_ = 48000.0;
        Wide invSampleRate_ = 1.0 / 48000.0; // FIREFORK N3: 1 / sampleRate_

        // Parameters (control-rate).
        Wide freqHz_ = 0.0;
        int mask_ = 0;

        // Control-rate coefficients.
        Wide inc_ = 0.0; // base-rate cycles/sample (the bypassed path)
        Wide incOs_ = 0.0; // OS-rate cycles/sample = inc/factor (the generator)
        float dc_ = 0.0f; // the current mask's zero-mean correction
        bool bypassed_ = false; // the clean/dirty switch; false = band-limited (ruling 1's default)
        bool adaptive_ = true; // pitch-adaptive factor (leaf C2, the shipped default)
        bool pendingHigh_ = false; // what the next reset() will commit
        bool activeHigh_ = false; // what this note is running

        // Per-sample state.
        Wide phase_ = 0.0; // [0,1)

        const float* carrier_ = nullptr; // shared sine table (cached in prepare())
        Oversampler osBase_; // 4x — the ordinary range (ruling 1)
        Oversampler osHigh_; // 8x — escalated above kEscalateHz (leaf C2)
    };
}
