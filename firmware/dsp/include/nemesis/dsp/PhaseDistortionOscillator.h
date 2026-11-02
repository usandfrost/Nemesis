#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/FastMath.h>
#include <nemesis/dsp/Oversampler.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace nemesis::dsp
{
    // PhaseDistortionOscillator — the Casio-CZ "phase distortion" oscillator, the genuinely-new
    // DSP half of CATHODE's dual-mode osc core (CATHODE.md §1.2). The WT-scan half is the shared
    // M1-04 WavetableOscillator; this is its sibling. A master phasor p in [0,1) is warped by a
    // piecewise-linear transfer function W(p) BEFORE it reads a band-limited sine table, so the
    // sine is read fast through one phase segment and slow through the other. That rate change
    // bunches the waveform — heard as added harmonics — morphing sine → saw/pulse. This is the
    // Casio CZ "DCW" technique (a public algorithm; §12.1 clean-path), realised as one sine LUT
    // through a two-segment phase map.
    //
    // Two control params (§1.2): pdShape = the KNEE (break-point) the warp bends toward at full
    // depth; pdAmount = the warp DEPTH. The break-point d interpolates from the identity (0.5,
    // W(p)=p → a clean sine) toward pdShape as pdAmount rises. Segment 0 maps [0,d]→[0,0.5],
    // segment 1 maps [d,1]→[0.5,1], so W is a continuous, monotonic bend of the phase ramp.
    // At pdAmount = 0 the warp is the identity and the output is a CLEAN SINE — the "normal
    // tone floor" (§1.2). W(p) is continuous, so the emitted sin(2π·W(p)) is continuous across
    // the cycle wrap (sin at W=0 and W=1 both = 0); only the SLOPE kinks at the knee, so the raw
    // spectrum rolls off like a triangle's, plus the warp harmonics. [B31] The Saw emits
    // sin(2π·W(p)) − (2/π)(2d − 1): the subtracted constant is that wave's exact cycle mean, so the
    // Saw is zero-mean at every knee (it was 0.64 DC at an end stop); and d's floor is two
    // base-rate samples from {0, 1}, tracking pitch (`resolveSawKnee`).
    //
    // Band-limiting (§11): phase distortion makes harmonics that alias at high pitch, so the
    // nonlinear phase map is OVERSAMPLED + decimated. The oscillator generates its waveform at
    // factor×fs (the phasor advances inc/factor per oversampled sample) and the internal
    // Oversampler's decimating FIR band-limits on the way down to one base-rate output. The
    // phase RAMP itself is never upsampled (its 1→0 wrap discontinuity would smear under a
    // band-limiting interpolator) — the generator synthesises the already-warped sine at each OS
    // phase instead, so the CZ shape stays exact and only its images are filtered. [T-CMP-37 L3]
    // The Oversampler is driven through its DECIMATING HALF alone (`decimateSample`): the
    // interpolating FIR this seam used to run on a dummy 0 input produced zeros that the generator
    // ignored (bit for bit what `processSample (0.0f, ...)` returned, D-M-0356). factor 1 is the naive, un-band-limited
    // readout — the baseline the alias-floor keystone measures against (mirrors M1-02b/M1-04).
    //
    // RT-safety: prepare() builds/caches the shared sine table and designs the decimator
    // (allocates, off the audio thread); the setters resolve inc + the warp slopes at CONTROL
    // rate; processSample() is inline and pure arithmetic — no libm CALLS (the phase wrap uses
    // std::floor, a single arm64 frintm, the same sanctioned category as BBD's fsqrt) — except the
    // Pulse and SawPulse types, whose formula is `ph^k` (one std::pow per OS sample) — no
    // allocation, no locks. Deterministic (no randomness) → bit-identical re-render. Computed
    // in double, emitted as float. No JUCE. Not thread-safe vs prepare().
    class PhaseDistortionOscillator
    {
    public:
        // Shared band-limited sine table size (a pure sinusoid → single partial; 2048 pts + a
        // guard point, linear-interp error ≈ -118 dB, below the 1e-6 golden tolerance).
        static constexpr int kSineTableSize = 2048;
        // Default oversampling factor for the phase map (§11: "oversample the nonlinear phase
        // map"; §1.1 suggests 2–4× for the WT side). 4× drops the alias floor well past the
        // keystone's 6 dB bar.
        static constexpr int kDefaultOversample = 4;

        // [T-CGUI-10 leaf B] `synth.osc{1,2}.pdType` — the eight PD types (CATHODE.md §1.2 PD TYPES, the
        // prototype's `PD_FN` on the owner's 2026-09-27 ruling). The ENUMERATOR IS THE STORED INDEX, so the
        // order is the registry's `kPdTypes` and is frozen. `Saw` is the two-segment map below, untouched;
        // the other seven are read by `typedRead`. Every type runs through the same generator-decimator,
        // so every type has the same latency and the same band-limit.
        enum class PdType : std::uint8_t { Saw,
            Square,
            Pulse,
            DblSine,
            SawPulse,
            ResoI,
            ResoII,
            ResoIII };
        static constexpr int kPdTypeCount = 8;

        // Builds the sine table, designs the decimating FIR for `oversampleFactor` (clamped
        // >= 1; 1 = naive/no band-limit) and clears state. Not RT-safe (allocates) — call like
        // prepareToPlay.
        void prepare (Wide sampleRate, int oversampleFactor = kDefaultOversample);

        // Clears the phase AND the oversampler's decimator state (a clean restart). RT-safe.
        void reset() noexcept;

        // Sets the oscillator frequency in Hz. Control-rate (re-resolves the phase increments, and the Saw's
        // knee, whose floor is two base-rate samples — `resolveSawKnee`).
        void setFrequency (Wide hz) noexcept
        {
            freqHz_ = hz;
            inc_ = sampleRate_ > 0.0 ? (fork::kApprox ? freqHz_ * invSampleRate_ : freqHz_ / sampleRate_) : 0.0; // FIREFORK N3
            if constexpr (fork::kExact) // FIREFORK E7: exact for the power-of-two factors
                incOs_ = factor_ > 0 ? fork::divByFactor (inc_, factor_) : inc_;
            else
                incOs_ = factor_ > 0 ? inc_ / static_cast<Real> (factor_) : inc_;
            resolveSawKnee();
        }

        // Warp knee target in [0,1] (`synth.oscN.pdShape`): where the break-point bends to at
        // full depth. Control-rate (recomputes the warp slopes).
        void setPdShape (float shape) noexcept
        {
            pdShape_ = clamp (shape, 0.0f, 1.0f);
            recomputeWarp();
        }

        // Warp depth in [0,1] (`synth.oscN.pdAmount`); 0 = clean sine (the "normal tone
        // floor", §1.2). Control-rate (recomputes the warp slopes).
        void setPdAmount (float amount) noexcept
        {
            pdAmount_ = clamp (amount, 0.0f, 1.0f);
            recomputeWarp();
        }

        // [T-CGUI-10 leaf B] The PD type. Control-rate (resolves the type's coefficients). Saw's warp
        // arithmetic is not touched by any type, so a Saw oscillator is bit-exact whatever came before.
        void setPdType (PdType type) noexcept
        {
            type_ = type;
            recomputeTyped();
        }
        [[nodiscard]] PdType pdType() const noexcept { return type_; }

        [[nodiscard]] Wide phase() const noexcept { return phase_; }
        [[nodiscard]] int oversampleFactor() const noexcept { return factor_; }
        // [T-CORE-16] The applied warp depth, CLAMPED (introspection / tests). The head of the
        // read-back chain `CathodeOscCore::pdAmount (slot)` → `CathodeVoiceStage::pdAmount (slot)`
        // → `EngineCore::pdAmountAt`, which is what makes a PER-SLOT warp-depth claim expressible
        // at all: a single-valued accessor cannot say "one rider, two different depths", and that
        // is precisely the property the §2.9 PD AMT family exists to protect. The OscPos
        // destination got the same chain at T-CORE-14 leaf D (`WavetableOscillator::position`).
        [[nodiscard]] float pdAmount() const noexcept { return pdAmount_; }

        // Base-rate group delay introduced by the decimating FIR (0 when factor == 1). FRACTIONAL.
        //
        // [T-MASK-03 leaf C] This used to report `os_.latencySamples()` — the PASS-THROUGH cascade
        // figure, 2M = 16 — which is wrong for this seam and was wrong from the day it was written:
        // the CZ waveform is synthesised inside the shaper lambda, so it never meets the
        // interpolating FIR and its true delay is M + (factor−1)/factor = 8.75 at factor 4. It read
        // as correct because every OTHER `os_.latencySamples()` consumer (the scream / soviet /
        // classic / reed / acid drive cores) really does pass its signal in, and because nothing
        // consumes this accessor — it is reported, never summed, so no shipped sound moved. Found
        // by measuring the delay rather than by reading the code, when leaf C asserted the same
        // claim on the bitmask oscillator and the assertion failed.
        [[nodiscard]] Real latencySamples() const noexcept { return os_.generatedLatencySamples(); }

        // Produces one band-limited base-rate sample. Hot path: the Oversampler calls the
        // generator `factor` times per base sample (in OS-phase order) and decimates the
        // results. Pure arithmetic (std::pow on Pulse/SawPulse only), no allocation.
        float processSample() noexcept
        {
            if constexpr (fork::kExact) // FIREFORK E12: the Saw generator from locals, then the same decimation
                if (type_ == PdType::Saw && factor_ > 1 && factor_ <= kMaxOsStepsFork)
                {
                    float steps[kMaxOsStepsFork];
                    Wide ph = phase_;
                    const Wide inc = incOs_;
                    const Real knee = knee_, s0 = seg0Slope_, s1 = seg1Slope_;
                    for (int p = 0; p < factor_; ++p)
                    {
                        ph = wrap01 (ph + inc);
                        const Real x = static_cast<Real> (ph);
                        const Real w = x < knee ? s0 * x : 0.5 + s1 * (x - knee);
                        steps[p] = static_cast<float> (sine (w) - kTwoOverPi * (2.0 * knee - 1.0));
                    }
                    phase_ = ph;
                    int q = 0;
                    return os_.decimateSample ([&q, &steps]() noexcept { return steps[q++]; });
                }
            return os_.decimateSample ([this]() noexcept { return generateOsSample(); });
        }
        static constexpr int kMaxOsStepsFork = 8; // FIREFORK E12

        // [T-CMP-37 L3] `processSample ()` IN TWO HALVES, so the engine can run the DECIMATING half for several voices at once
        // (OversamplerLanes::decimateSteps). The generator never reads the decimator, so it can run all of a base sample's OS steps
        // first: `generateSteps` writes this step's `oversampleFactor ()` OS-rate samples, oldest first, advancing the phasor as
        // `processSample` does; `decimateSteps` band-limits them to the base-rate sample. `decimateSteps (steps)` after
        // `generateSteps (steps)` is `processSample ()` bit for bit (the same floats pushed in the same order into the same FIR).
        static constexpr int kMaxOsSteps = 8; // the largest factor the two halves serve (the lane design's too)
        void generateSteps (float* steps) noexcept
        {
            for (int p = 0; p < factor_; ++p)
                steps[p] = generateOsSample();
        }
        float decimateSteps (const float* steps) noexcept
        {
            int p = 0;
            return os_.decimateSample ([&p, steps]() noexcept { return steps[p++]; });
        }
        // The decimator, mutable, for the lane-wise path to take its delay lines in and out (`OversamplerLanes::importLane`).
        [[nodiscard]] Oversampler& oversampler() noexcept { return os_; }

        // [T-CMP-03 leaf 2b] The PHASE-MODULATED read — one band-limited base-rate sample with
        // `phaseOffsetTurns` added to the phase this oscillator READS, at every one of its own OS
        // phases. The offset is applied to the read and never to `phase_`, because a phase modulation
        // that accumulated would be a frequency modulation that drifts.
        //
        // WHY THE OFFSET IS ONE VALUE FOR THE WHOLE OS BLOCK rather than interpolated per OS phase,
        // which is what the WT carrier does. This oscillator owns its own oversampler at its own
        // factor, and it must — the CZ warp is the nonlinearity that oversampling exists to
        // band-limit here. So the modulator, which arrives at BASE rate, is zero-order held across the
        // block. That is not an approximation of the modulator: it IS a base-rate signal, and a ZOH's
        // images sit at multiples of fs, which this oscillator's own decimator removes on the way
        // down. The in-band result is the modulator it was handed.
        //
        // The unmodulated `processSample()` above is UNTOUCHED — same operations, same order, same
        // doubles — so every committed PD golden holds bit for bit. That is the same constraint
        // T-CMP-15 leaf B met when it extracted `readAt` from `WavetableOscillator`.
        float processSample (Wide phaseOffsetTurns) noexcept
        {
            return os_.decimateSample ([this, phaseOffsetTurns]() noexcept {
                return generateOsSample (phaseOffsetTurns);
            });
        }

        // [T-CMP-04 leaf 5] HARD SYNC on an OVERSAMPLED generator, and the restart has to land at
        // an OS instant rather than a base-rate one. This oscillator advances its phasor `factor`
        // times per base sample, so restarting once per base sample would quantise the reset to a
        // grid `factor` times coarser than the generator's own — a pitch error at high ratios,
        // not merely a click.
        //
        // `step` is invoked BEFORE each OS advance and hands back this oscillator, so the caller
        // can restart the phase at that instant. The caller owns the master phasor because the
        // master is a property of the SLOT (its pitch), not of this oscillator.
        //
        // WHY NO BLEP HERE, unlike the wavetable and pulse carriers: the discontinuity is created
        // at the OS rate and this oscillator's own decimator band-limits it on the way down. That
        // is CATHODE §11's "oversample any cross-mod/sync" reading, and for THIS carrier it is
        // the right one — the anti-alias machinery the restart needs is already in the signal
        // path because the CZ warp needs it.
        //
        // `processSample()` above is UNTOUCHED, so every committed PD golden holds bit for bit —
        // the same constraint T-CMP-03 leaf 2b met when it added the phase-modulated entry point.
        template <typename Step>
        float processSampleSynced (Step&& step) noexcept
        {
            return os_.decimateSample ([this, &step]() noexcept {
                step (*this);
                return generateOsSample();
            });
        }

        // [D-M-0208] OSC 2-master HARD SYNC with a free/synced CROSSFADE, at the OS rate. `phase_`
        // advances FREE exactly as `processSample()` advances it; `syncPhase` (caller-owned) is the
        // synced copy at the same increment, RESTARTED at the OS instant `restartAtOs` (in OS steps
        // into this base sample, (0, factor]; < 0 = no restart) — landing `elapsed` steps past zero on
        // the first step at or after it. Output per OS step = freeGain·free + syncGain·synced, summed
        // before the ONE decimator (linear, so it equals blending the decimated signals). A zero
        // `freeGain` skips the free read. `processSample()` above is untouched (bit for bit).
        float processSampleSyncBlend (Wide& syncPhase, Real restartAtOs, Real freeGain, Real syncGain) noexcept
        {
            int j = 0;
            return os_.decimateSample ([this, &syncPhase, &j, restartAtOs, freeGain, syncGain]() noexcept {
                phase_ = wrap01 (phase_ + incOs_);
                const Real elapsed = static_cast<Real> (++j) - restartAtOs;
                syncPhase = restartAtOs >= 0.0 && elapsed >= 0.0 && elapsed < 1.0 ? wrap01 (incOs_ * elapsed)
                                                                                  : wrap01 (syncPhase + incOs_);
                Real out = syncGain * static_cast<Real> (warpAndRead (syncPhase));
                if (freeGain > 0.0)
                    out += freeGain * static_cast<Real> (warpAndRead (phase_));
                return static_cast<float> (out);
            });
        }

        // Sets the phase outright, wrapped. The OS-rate twin of `WavetableOscillator::setPhase`.
        void setPhase (Real p) noexcept { phase_ = wrap01 (p); }

        // [T-CGUI-63] THE UNISON STACK — `n` members, each its own phase (`phases`, in turns, owned by
        // the caller) running at this oscillator's OS increment × `incMult[k]`, weighted by `gains[k]`
        // and SUMMED AT THE OS RATE, so the whole stack shares this oscillator's ONE decimator. The sum
        // is linear, so decimating it equals decimating each member. `phase_` does not advance: the
        // stack owns the phases while it plays. `processSample()` above is untouched (bit for bit).
        float processSampleStack (Wide* phases, const Wide* incMult, const float* gains, int n) noexcept
        {
            return os_.decimateSample ([this, phases, incMult, gains, n]() noexcept {
                Real sum = 0.0;
                for (int k = 0; k < n; ++k)
                {
                    phases[k] = wrap01 (phases[k] + incOs_ * incMult[k]);
                    sum += static_cast<Real> (gains[k]) * static_cast<Real> (warpAndRead (phases[k]));
                }
                return static_cast<float> (sum);
            });
        }

        // [stereo voice S4, D-M-0188] THE STACK WITH A SIDE: `processSampleStack` exactly (the returned
        // mid), and each member ALSO weighted by `sideGains[k]` into a second OS-rate stream, which
        // `sideOs` — the caller's, prepared as this one's own (`oversampler()`) — decimates into `side`.
        // Same reads, so the side is band-limited like the mid and arrives with the same delay.
        float processSampleStackSide (Wide* phases, const Wide* incMult, const float* gains, const float* sideGains, int n, Oversampler& sideOs, float& side) noexcept
        {
            std::array<float, kMaxSideSteps> steps; // NOLINT(cppcoreguidelines-pro-type-member-init) — written before read
            int written = 0;
            const float mid = os_.decimateSample ([&]() noexcept {
                Real sum = 0.0;
                Real sd = 0.0;
                for (int k = 0; k < n; ++k)
                {
                    phases[k] = wrap01 (phases[k] + incOs_ * incMult[k]);
                    const auto v = static_cast<Real> (warpAndRead (phases[k]));
                    sum += static_cast<Real> (gains[k]) * v;
                    sd += static_cast<Real> (sideGains[k]) * v;
                }
                if (written < kMaxSideSteps)
                    steps[static_cast<std::size_t> (written++)] = static_cast<float> (sd);
                return static_cast<float> (sum);
            });
            int read = 0;
            side = sideOs.decimateSample ([&]() noexcept {
                return read < written ? steps[static_cast<std::size_t> (read++)] : 0.0f;
            });
            return mid;
        }
        // The decimator, for a caller preparing its side twin alike (copy it off the audio thread).
        [[nodiscard]] const Oversampler& oversampler() const noexcept { return os_; }
        static constexpr int kMaxSideSteps = 64; // OS steps per base sample the side buffers (factor <= 64)

    private:
        // Keeps the break-point off {0, 1} so neither warp segment divides by zero (the floor at inc 0).
        static constexpr Real kKneeEps = 1.0e-4;
        // 2/π: the cycle mean of sin(2π·W(p)) is (2/π)(2d − 1) — each segment is a half sine, mean ±2/π.
        static constexpr Real kTwoOverPi = 0.63661977236758134;
        // [T-CGUI-77] How close 7·pdShape must sit to an integer to be a Reso step centre (a float shape of
        // j/7 lands within 5e-7); the crossfade weight at this distance is ~3e-12, below a float's resolution.
        static constexpr Real kResoCentreEps = 1.0e-6;

        static float clamp (float v, float lo, float hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        // Wraps a phase in turns into [0, 1). std::floor lowers to a single arm64 frintm
        // instruction — not a libm call (same sanctioned category as BBD's fsqrt).
        static Wide wrap01 (Wide p) noexcept { return p - std::floor (p); } // FIREFORK: was (Real p): float builds lost the phase

        // Band-limited sine of a phase in turns, via linear interpolation of the shared table.
        // Pure arithmetic + one frintm — no libm call.
        Real sine (Wide phaseTurns) const noexcept
        {
            const Real p = wrap01 (phaseTurns); // → [0, 1)
            const Real x = p * static_cast<Real> (kSineTableSize);
            const int i = static_cast<int> (x);
            const Real frac = x - static_cast<Real> (i);
            const Real a = static_cast<Real> (sine_[i]);
            const Real b = static_cast<Real> (sine_[i + 1]); // guard point at [kSineTableSize]
            return a + frac * (b - a);
        }

        // One oversampled sample: advance the phasor by inc/factor, warp the phase through the
        // two-segment map, read the sine table at the warped index.
        float generateOsSample() noexcept
        {
            phase_ = wrap01 (phase_ + incOs_);
            return warpAndRead (phase_);
        }

        // [T-CMP-03 leaf 2b] The same step with a read-side phase offset. `wrap01` is needed here and
        // is deliberately NOT in the line above: `phase_` is already in [0,1) there, so the extra
        // floor would be a no-op that still had to be paid for on the unmodulated hot path.
        float generateOsSample (Wide phaseOffsetTurns) noexcept
        {
            phase_ = wrap01 (phase_ + incOs_);
            return warpAndRead (wrap01 (phase_ + phaseOffsetTurns));
        }

        // [T-CMP-03 leaf 2b] The two-segment warp and the table read, extracted so the modulated and
        // unmodulated generators cannot drift into two copies of one law (the T-CMP-15 leaf B rule).
        // [T-CGUI-10 leaf B] A non-Saw type branches off BEFORE the Saw arithmetic, which is unchanged.
        float warpAndRead (Real ph) const noexcept
        {
            if (type_ != PdType::Saw)
                return static_cast<float> (typedRead (ph));
            const Real w = ph < knee_ ? seg0Slope_ * ph : 0.5 + seg1Slope_ * (ph - knee_);
            // [B31] Minus the cycle mean (2/π)(2d − 1), so the Saw is zero-mean at every knee. Computed from
            // knee_ rather than stored: no per-voice bytes. Exactly 0 at d = 0.5 (pdAmount 0 is untouched).
            return static_cast<float> (sine (w) - kTwoOverPi * (2.0 * knee_ - 1.0));
        }

        // [T-CGUI-10 leaf B] The seven non-Saw types — the prototype's `PD_FN` (`CATHODE Prototype.dc.html`
        // :746-754) with sin(2π·x) read as `sine (x)` (x in turns) and each divisor pre-inverted at
        // control rate. Every `sine` argument is >= 0, so the table read's wrap is exact. `std::pow` on
        // Pulse / SawPulse is the one transcendental — the formula's `ph^k` (CATHODE.md §1.2).
        Real typedRead (Real ph) const noexcept
        {
            switch (type_)
            {
                case PdType::Square: // sin(2π(ph + c·sin(4π·ph)))
                    return sine (ph + sqDepth_ * sine (2.0 * ph));
                case PdType::Pulse: // sin(2π·ph^k)
                    return sine (fork::kApprox ? fm::exp2 (powExp_ * fm::log2 (ph)) : std::pow (ph, powExp_)); // FIREFORK N4
                case PdType::DblSine: // (1−a)·sin(2π·ph) + a·sin(4π·ph + π·s)
                    return sine (ph) * (1.0 - amt_) + amt_ * sine (2.0 * ph + dblPhase_);
                case PdType::SawPulse: // sin(2π·w^k), w the kn-knee two-segment map
                {
                    const Real w = ph < spKnee_ ? ph * spSlope0_ : 0.5 + (ph - spKnee_) * spSlope1_;
                    return sine (fork::kApprox ? fm::exp2 (powExp_ * fm::log2 (w)) : std::pow (w, powExp_)); // FIREFORK N4
                }
                case PdType::ResoI: // (1−a)·sin(2π·ph) + a·(1−ph)·sin(2π·r·ph)
                    return sine (ph) * (1.0 - amt_) + amt_ * (1.0 - ph) * resoTerm (ph);
                case PdType::ResoII: // … a·(1−ph)²·sin(2π·r·ph)
                    return sine (ph) * (1.0 - amt_) + amt_ * ((1.0 - ph) * (1.0 - ph)) * resoTerm (ph);
                case PdType::ResoIII: // … a·½(1 + cos 2π·ph)·sin(2π·r·ph)
                    return sine (ph) * (1.0 - amt_) + amt_ * (0.5 * (1.0 + sine (ph + 0.25))) * resoTerm (ph);
                case PdType::Saw:
                    break;
            }
            return 0.0;
        }

        // [T-CGUI-77, owner ruling 2026-09-27] The Reso types' sin(2π·r·ph) — the harmonic `resoRatio_` alone
        // at a step centre (`resoHi_` exactly 0: the prototype's read, bit for bit), else the equal-power pair
        // `resoLo_`·harmonic r + `resoHi_`·harmonic r+1 (CATHODE.md §1.2).
        Real resoTerm (Real ph) const noexcept
        {
            const Real lo = sine (ph * resoRatio_);
            return resoHi_ == 0.0 ? lo : resoLo_ * lo + resoHi_ * sine (ph * (resoRatio_ + 1.0));
        }

        // sin(π·u/2) on [0, 1] without libm (the setter runs per sample under a routed pdShape): the Taylor
        // series to u¹¹ (error ≤ 6e-8), divided by its own value at u = 1 so the pair reaches exactly 1 and 0.
        static Real quarterSine (Real u) noexcept
        {
            constexpr Real c1 = 1.5707963267948966, c3 = -0.6459640975062462, c5 = 0.07969262624616703,
                             c7 = -0.004681754135318687, c9 = 1.6044118478735982e-4, c11 = -3.598843235212085e-6;
            constexpr Real norm = 1.0 / (c1 + c3 + c5 + c7 + c9 + c11);
            const Real u2 = u * u;
            return norm * u * (c1 + u2 * (c3 + u2 * (c5 + u2 * (c7 + u2 * (c9 + u2 * c11)))));
        }

        // Control-rate: resolve the break-point and the two segment slopes from pdShape/pdAmount.
        // d = 0.5 + amount·(shape − 0.5): amount 0 → d = 0.5, slopes 1 → W(p) = p (clean sine).
        void recomputeWarp() noexcept
        {
            resolveSawKnee();
            if (type_ != PdType::Saw)
                recomputeTyped();
        }

        // [B31, owner ruling 2026-10-09] The knee is held at least eps = max(kKneeEps, 2·|inc|) from {0, 1}, so
        // the short segment always spans >= two base-rate samples: a narrower one is band-limited away by the
        // decimator and the end stop collapsed to a dark arch. eps is capped at 0.5 (above fs/4 only the sine
        // fits). Where min(d, 1 − d) >= eps nothing moves. Pitch-dependent, so `setFrequency` re-runs it.
        void resolveSawKnee() noexcept
        {
            Real eps = 2.0 * std::abs (inc_);
            eps = eps < kKneeEps ? kKneeEps : (eps > 0.5 ? 0.5 : eps);
            Real d = 0.5 + static_cast<Real> (pdAmount_) * (static_cast<Real> (pdShape_) - 0.5);
            if (d < eps)
                d = eps;
            if (d > 1.0 - eps)
                d = 1.0 - eps;
            if constexpr (fork::kExact) // FIREFORK E7: seg0/seg1 slopes are 0.5/d and 0.5/(1-d): unchanged with d
                if (d == knee_)
                    return;
            knee_ = d;
            seg0Slope_ = 0.5 / d;
            seg1Slope_ = 0.5 / (1.0 - d);
        }

        // [T-CGUI-10 leaf B] Control-rate: the typed coefficients from a = pdAmount, s = pdShape, operand
        // for operand as `PD_FN` writes them. Skipped while Saw plays, so Saw's setters cost what they did.
        // `std::round` (a single frinta) is JS `Math.round` here: 7·s ≥ 0 and 7k/127 never lands on a half.
        // [T-CGUI-77] The Reso harmonic GLIDES: at a step centre (7·s within kResoCentreEps of an integer)
        // r = 1 + round(7·s) alone, exactly as before; between centres k = floor(7·s), f its fraction, and an
        // equal-power crossfade over the smoothstep u = f²(3 − 2f) — cos(πu/2) of harmonic 1+k, sin(πu/2) of
        // 2+k. The smoothstep's zero slope at each centre keeps the u7 knob positions nearest one within −51 dB.
        void recomputeTyped() noexcept
        {
            const Real a = static_cast<Real> (pdAmount_);
            const Real s = static_cast<Real> (pdShape_);
            amt_ = a;
            sqDepth_ = a * 0.3 * (0.5 + s * 0.5);
            dblPhase_ = s * 0.5; // π·s radians = s/2 turns
            spKnee_ = 0.5 - 0.4 * a;
            spSlope0_ = 1.0 / (2.0 * spKnee_);
            spSlope1_ = 1.0 / (2.0 * (1.0 - spKnee_));
            powExp_ = type_ == PdType::SawPulse ? 1.0 + a * 1.8 * s : 1.0 + a * 3.0 * (0.35 + s);
            const Real x = s * 7.0;
            const Real centre = std::round (x);
            if (std::abs (x - centre) < kResoCentreEps)
            {
                resoRatio_ = 1.0 + centre;
                resoLo_ = 1.0;
                resoHi_ = 0.0;
            }
            else
            {
                const Real k = std::floor (x);
                const Real f = x - k;
                const Real u = f * f * (3.0 - 2.0 * f);
                resoRatio_ = 1.0 + k;
                resoLo_ = quarterSine (1.0 - u);
                resoHi_ = quarterSine (u);
            }
        }

        // Builds (once, off the audio thread) and returns the shared kSineTableSize+1 sine table.
        static const float* bandlimitedSineTable() noexcept;

        Wide sampleRate_ = 48000.0;
        Wide invSampleRate_ = 1.0 / 48000.0; // FIREFORK N3: 1 / sampleRate_
        int factor_ = kDefaultOversample;

        // Parameters (control-rate).
        Wide freqHz_ = 0.0;
        float pdShape_ = 0.5f;
        float pdAmount_ = 0.0f;

        // Control-rate coefficients.
        Wide inc_ = 0.0; // base-rate cycles/sample
        Wide incOs_ = 0.0; // OS-rate cycles/sample (inc/factor)
        Real knee_ = 0.5; // warp break-point d
        Real seg0Slope_ = 1.0; // 0.5/d
        Real seg1Slope_ = 1.0; // 0.5/(1-d)

        // [T-CGUI-10 leaf B] The type and its control-rate coefficients (`recomputeTyped`).
        PdType type_ = PdType::Saw;
        Real amt_ = 0.0; // a
        Real sqDepth_ = 0.0; // Square: 0.3·a·(0.5 + 0.5·s)
        Wide dblPhase_ = 0.0; // DblSine: s/2 turns
        Real spKnee_ = 0.5; // SawPulse: kn = 0.5 − 0.4·a
        Real spSlope0_ = 1.0; // 1/(2·kn)
        Real spSlope1_ = 1.0; // 1/(2·(1 − kn))
        Real powExp_ = 1.0; // Pulse: 1 + 3a(0.35 + s) · SawPulse: 1 + 1.8·a·s
        Real resoRatio_ = 1.0; // Reso I–III: r = 1 + round(7·s) at a centre, else 1 + floor(7·s) (T-CGUI-77)
        Real resoLo_ = 1.0; // Reso glide: harmonic r's gain, cos(πu/2)
        Real resoHi_ = 0.0; // … harmonic r+1's, sin(πu/2); exactly 0 at a step centre

        // Per-sample state.
        Wide phase_ = 0.0; // [0,1)

        const float* sine_ = nullptr; // shared band-limited sine table (cached in prepare())
        Oversampler os_; // decimating FIR band-limits the oversampled phase map (§11)
    };
}
