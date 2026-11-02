#pragma once

#include <nemesis/dsp/Real.h>
#include <memory>
#include <vector>

namespace nemesis::dsp
{
    // MinBlep — a minimum-phase band-limited step, and the residual that replaces a naive
    // phase reset (M1-19).
    //
    // WHY: resetting an oscillator's phase mid-sample is a step discontinuity, and a step has
    // energy at every frequency — so the reset itself aliases, however band-limited the
    // waveform either side of it was. The fix is to subtract the difference between an IDEAL
    // step and a BAND-LIMITED one, inserted at the exact fractional instant of the reset.
    // PRISM §10.1 names this one of three genuinely-new clean-path generators the project
    // still owes, citing Brandt, "Hard Sync Without Aliasing"; CATHODE §11/:43 names it as
    // hard sync's anti-alias law (owner ruling R2 at T-CMP-04's plan gate).
    //
    // WHY MINIMUM PHASE, and not the linear-phase BLEP most implementations actually ship:
    // a linear-phase correction is symmetric about the discontinuity, so half of it lands
    // BEFORE the reset — audible pre-ringing, and a fixed half-support latency the consumer
    // has to report and compensate. A minimum-phase correction is causal and front-loaded:
    // same magnitude response, no pre-echo, no latency to carry. The `impulse()` accessor and
    // the `designBandlimitedImpulse` static exist so a test can build the linear-phase leg
    // from the SAME windowed sinc and measure the difference, rather than take the word
    // "minimum" on trust.
    //
    // Design: prototype = windowed sinc (4-term Blackman-Harris), cutoff 1/(2·oversampling)
    // normalised — the same prototype family as `Oversampler.h`, which is the nearest built
    // precedent and worth reading beside this. That impulse is converted to minimum phase by
    // the real-cepstrum method (log-magnitude → cepstrum → causal fold → exponentiate), then
    // integrated to a step and normalised to settle at 1. Coefficients held in double.
    //
    // The FFT the cepstrum needs is a private radix-2 routine in this .cpp — the FOURTH such
    // copy in `dsp/` (`Convolution.cpp`, `WavetableOscillator.cpp`, `MaskTableGenerator.cpp`
    // are the others). ADR-007 D2 mandates a shared L1 FFT seam and it has never been built;
    // owner-ruled 2026-08-21 to follow the three precedents here rather than realise the seam
    // inside a Tier-A DSP card. The duplication is ledgered, not accepted silently.
    //
    // RT-safety: prepare() designs the table and allocates; everything else is const reads
    // and pure arithmetic. NOT thread-safe against prepare().
    //
    // [T-ARCH-12] ONE DEFAULT DESIGN PER PROCESS. The design (impulse + step table) is a pure function
    // of (oversampling, zeroCrossings) and immutable once built, and `CathodeOscCore` prepares every
    // voice with the defaults — 32 identical cepstral designs (four FFTs each) per machine prepare,
    // ~13 % of the debug test leg. A default `prepare()` now reads `sharedDefaultDesign()`, built once
    // (thread-safe static init) on the first prepare — never in `processSample`/`addStep`. Bit-exact by
    // construction: it is the same `buildDesign` call, run once instead of per instance.
    class MinBlep
    {
    public:
        static constexpr int kDefaultOversampling = 64;
        static constexpr int kDefaultZeroCrossings = 16;

        // The immutable DESIGN an instance reads: the minimum-phase band-limited impulse and its
        // running integral, the step table. Per-instance state (the accumulator) is not in it.
        struct Design
        {
            int oversampling = 0;
            int zeroCrossings = 0;
            std::vector<Real> impulse; // minimum-phase band-limited impulse
            std::vector<Real> table; // its running integral, settling at 1
        };

        // A FRESH design (clamped as `prepare` clamps). Allocates and runs libm; off the audio thread.
        [[nodiscard]] static Design buildDesign (int oversampling, int zeroCrossings);

        // The default design, built once per process on first use and shared by every default
        // `prepare()`. Off the audio thread (first use allocates).
        [[nodiscard]] static const Design& sharedDefaultDesign();

        // Designs the table. `oversampling` is the sub-sample resolution of the residual (how
        // finely a reset's fractional instant can be placed); `zeroCrossings` is the sinc's
        // half-width in base samples, so the correction spans 2·zeroCrossings samples.
        // Both are clamped to sane minima. Not RT-safe; call off the audio thread. The defaults
        // share `sharedDefaultDesign()` rather than building a copy.
        void prepare (int oversampling = kDefaultOversampling, int zeroCrossings = kDefaultZeroCrossings);

        // Clears any correction still in flight. The table survives — it is a function of the
        // design, not of the signal.
        void reset() noexcept;

        // Schedule a correction. The naive signal jumps by `stepSize` at time `frac` samples
        // AFTER the last sample processSample() returned, so the next output sample sits
        // `1 - frac` after the discontinuity. `frac` is clamped to [0,1]: unlike
        // WavetableOscillator::sampleAt — which refuses to clamp so a wrong phase offset
        // shows up as a crash rather than a quiet wrong answer — an out-of-range instant here
        // has a correct nearest answer and no way to index out of bounds, so clamping loses
        // no signal a refusal would have surfaced.
        //
        // Overlapping steps SUPERPOSE: hard sync at a high ratio resets more often than the
        // support is long, so that is the normal case rather than an edge one.
        void addStep (Real stepSize, Real frac) noexcept;

        // [D-M-0207 / T-CGUI-118] THE BAND-LIMITED RAMP (the BLAMP): a SLOPE correction. A BLEP says nothing about
        // a slope discontinuity, and a hard-synced sine restarts at phase 0 where its derivative is largest (M1-19
        // leaf C measured the BLEP alone at 11.2 dB there, against 25.4 on a saw); an arbitrary wavetable at an
        // arbitrary ratio jumps both ways. The naive signal's derivative jumps by `slopeChange` (per base-rate
        // sample) at `frac` samples after the last sample returned — `addStep`'s instant convention, its
        // accumulator, so one restart is a call of each and they superpose.
        //
        // WHY NOT THE MINIMUM-PHASE TABLE `addStep` uses (D-M-0207 asked for one, and it was built and measured
        // first). A minimum-phase ramp LAGS the ideal one by the impulse's group delay for ever (2.9 samples
        // here), so its residual never returns to 0; ending it inside the support takes a compensating
        // band-limited step of that lag times the slope change — on a synced sine several times the signal's own
        // size — and its residual made the floor WORSE than the BLEP alone (-28.7 against -31.7 dB). A ramp's
        // rounding must be SYMMETRIC about the kink to leave nothing behind, so this is the cubic-B-spline BLAMP
        // (`rampResidualAt`, four samples wide): its taps on the sample just returned and the next two are
        // applied — the first as this call's RETURN, which the caller adds to that sample before it goes out —
        // and the one before that, already out, is dropped (at most 1/120 of the slope change; measured no
        // worse). Zero latency, no table. Hot path: three closed-form quintics, no allocation, no libm.
        [[nodiscard]] Real addRamp (Real slopeChange, Real frac) noexcept;

        // The B-spline ramp residual `t` samples from a unit slope change (negative: before it): the cubic
        // B-spline-smoothed ramp less the ideal one, (1/120)·Σ_{j=0..4} (−1)^j·C(4,j)·(t+2−j)_+^5 − t_+.
        // Symmetric, non-negative (a convex corner rounded), 7/30 at the kink, exactly 0 from |t| = 2 on.
        [[nodiscard]] static Real rampResidualAt (Real t) noexcept;

        // Advances one base-rate sample and returns the correction to ADD to the naive
        // output. Exactly 0.0f when nothing is in flight — the off-anchor a consumer that
        // never resets relies on (MAGMA §6.10's rule, adopted across the CATHODE depth rows).
        // Hot path: no allocation, no libm.
        [[nodiscard]] float processSample() noexcept;

        [[nodiscard]] int oversampling() const noexcept { return oversampling_; }
        [[nodiscard]] int zeroCrossings() const noexcept { return zeroCrossings_; }

        // Length of the oversampled step table, 2·zeroCrossings·oversampling + 1.
        [[nodiscard]] int tableLength() const noexcept { return design_ ? static_cast<int> (design_->table.size()) : 0; }

        // Base-rate span of the correction, in samples.
        [[nodiscard]] int supportSamples() const noexcept { return 2 * zeroCrossings_; }

        // The step table at oversampled index `i`: a band-limited unit step, rising to 1 and
        // settling there. The RESIDUAL a consumer adds is `tableAt(i) - 1`, scaled by the
        // size of the step being corrected. Out-of-range indices clamp.
        [[nodiscard]] Real tableAt (int i) const noexcept;

        // The minimum-phase band-limited IMPULSE the table is the integral of. Exposed so a
        // test can check the transform preserved the prototype's magnitude response — the
        // structural check on the cepstrum, which no shape assertion on the table can make.
        [[nodiscard]] const std::vector<Real>& impulse() const noexcept;

        // The prototype BEFORE the minimum-phase transform: a Blackman-Harris-windowed sinc
        // of length 2·zeroCrossings·oversampling + 1, normalised to unit DC gain. Static and
        // public so a control leg shares every design constant with the leg under test
        // (the T-FX-03 apples-to-apples rule).
        [[nodiscard]] static std::vector<Real> designBandlimitedImpulse (int oversampling, int zeroCrossings);

    private:
        int oversampling_ = 0;
        int zeroCrossings_ = 0;

        // The design this instance reads: `sharedDefaultDesign()`'s storage for the defaults, its own
        // otherwise. Never written after `prepare()`, so a const read from the audio thread.
        std::shared_ptr<const Design> design_;

        // Corrections in flight, as a circular accumulator exactly supportSamples() long.
        // A read CLEARS its slot, which is what makes the support bound a property of the
        // mechanism rather than a promise: nothing can leak past the window because the
        // window is the buffer.
        std::vector<Real> accum_;
        int accumPos_ = 0;
    };
}
